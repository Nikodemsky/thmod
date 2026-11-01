#include "stdafx.h"

// (integrations) Discord Rich Presence.
//
// The game talks to the Discord client through its local IPC pipe (\\.\pipe\discord-ipc-N),
// the same way the official discord-rpc library does, without any extra dll: a frame is
// a 32-bit opcode, a 32-bit length and a JSON body. Works with the official client and with
// modified clients that run arRPC.
//
// Everything that touches the pipe runs on one background thread, so the game never waits
// for Discord. The game loop only builds a short JSON snapshot every 2 seconds and hands it
// over when it changed; the thread sends it (at most once every 4 seconds, Discord limits
// presence updates). If Discord is not running the thread retries every 15 seconds.
//
// Off by default. config.ini:
//   set DiscordRichPresence on
//   set DiscordAppId 1234567890   // optional: your own Discord application, default = the mod's

const char* DiscordAppIdFromConfig(); // Config.cpp

namespace {

const char DefaultDiscordAppId[] = "1566481693655699506";

enum : uint32_t { OP_HANDSHAKE = 0, OP_FRAME = 1, OP_CLOSE = 2, OP_PING = 3, OP_PONG = 4 };

CRITICAL_SECTION DiscordLock;
HANDLE WakeEvent = nullptr;
HANDLE Thread = nullptr;
volatile LONG Running = 0;
std::string PendingActivity; // guarded by DiscordLock; "" = clear the presence
char AppId[32] = "";

// main thread only
std::string LastPublished;
bool HasPublished = false;
DWORD LastBuildTick = 0;
long long SessionStart = 0;

// worker thread only
HANDLE Pipe = INVALID_HANDLE_VALUE;
unsigned Nonce = 0;

//----- JSON helpers --------------------------------------------------------
void AppendJsonString( std::string& out, const char* s )
{
	out += '"';
	for( ; s && *s; ++s ){
		unsigned char c = (unsigned char)*s;
		if( c == '"' || c == '\\' ){ out += '\\'; out += (char)c; }
		else if( c < 0x20 ) out += ' ';
		else if( c >= 0x80 ) out += '?'; // game strings are not UTF-8; keep the JSON valid
		else out += (char)c;
	}
	out += '"';
}

//----- pipe (worker thread) ------------------------------------------------
void Disconnect()
{
	if( Pipe != INVALID_HANDLE_VALUE ){
		CloseHandle( Pipe );
		Pipe = INVALID_HANDLE_VALUE;
	}
}

bool WriteFrame( uint32_t op, const std::string& json )
{
	std::string buf( 8 + json.size(), '\0' );
	uint32_t len = (uint32_t)json.size();
	memcpy( &buf[0], &op, 4 );
	memcpy( &buf[4], &len, 4 );
	if( len ) memcpy( &buf[8], json.data(), len );
	DWORD written = 0;
	return WriteFile( Pipe, buf.data(), (DWORD)buf.size(), &written, nullptr ) && written == buf.size();
}

// waits up to timeoutMs for at least `size` bytes, so a silent Discord can never hang the thread
// 1 = the bytes are there, 0 = timeout, -1 = the pipe is broken
int WaitBytes( DWORD size, DWORD timeoutMs )
{
	DWORD start = GetTickCount();
	for(;;){
		DWORD avail = 0;
		if( !PeekNamedPipe( Pipe, nullptr, 0, nullptr, &avail, nullptr ) ) return -1;
		if( avail >= size ) return 1;
		if( GetTickCount() - start >= timeoutMs ) return 0;
		Sleep( 20 );
	}
}

bool ReadExact( void* dst, DWORD size )
{
	char* p = (char*)dst;
	while( size ){
		DWORD got = 0;
		if( !ReadFile( Pipe, p, size, &got, nullptr ) || !got ) return false;
		p += got;
		size -= got;
	}
	return true;
}

// 1 = frame read, 0 = nothing within the timeout, -1 = the pipe is broken
int ReadFrame( uint32_t& op, std::string& json, DWORD timeoutMs )
{
	int ready = WaitBytes( 8, timeoutMs );
	if( ready <= 0 ) return ready;
	uint32_t header[2];
	if( !ReadExact( header, 8 ) ) return -1;
	op = header[0];
	if( header[1] > 64 * 1024 ) return -1;
	json.resize( header[1] );
	if( header[1] && ( WaitBytes( header[1], 2000 ) != 1 || !ReadExact( &json[0], header[1] ) ) ) return -1;
	return 1;
}

// handles whatever Discord sent on its own (pings); false = the connection is gone
bool Pump()
{
	for(;;){
		uint32_t op; std::string json;
		int r = ReadFrame( op, json, 0 );
		if( r < 0 ) return false;
		if( r == 0 ) return true;
		if( op == OP_PING && !WriteFrame( OP_PONG, json ) ) return false;
		if( op == OP_CLOSE ) return false;
	}
}

// waits for Discord's answer to a command; false = Discord closed the connection
bool WaitAnswer( DWORD timeoutMs )
{
	DWORD start = GetTickCount();
	for(;;){
		DWORD passed = GetTickCount() - start;
		if( passed >= timeoutMs ) return true; // no answer is not fatal
		uint32_t op; std::string json;
		int r = ReadFrame( op, json, timeoutMs - passed );
		if( r < 0 ) return false;
		if( r == 0 ) return true;
		if( op == OP_PING ){ if( !WriteFrame( OP_PONG, json ) ) return false; continue; }
		return op == OP_FRAME;
	}
}

bool Connect()
{
	for( int i = 0; i < 10 && Pipe == INVALID_HANDLE_VALUE; ++i ){
		char name[64];
		sprintf_s( name, "\\\\.\\pipe\\discord-ipc-%d", i );
		Pipe = CreateFileA( name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr );
		if( Pipe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY && WaitNamedPipeA( name, 1000 ) ){
			Pipe = CreateFileA( name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr );
		}
	}
	if( Pipe == INVALID_HANDLE_VALUE ) return false;

	std::string hello = "{\"v\":1,\"client_id\":";
	AppendJsonString( hello, AppId );
	hello += '}';
	uint32_t op; std::string answer;
	if( !WriteFrame( OP_HANDSHAKE, hello ) || ReadFrame( op, answer, 5000 ) != 1 || op != OP_FRAME ){
		Disconnect(); // OP_CLOSE here = Discord refused the application id
		return false;
	}
	return true;
}

bool SendActivity( const std::string& activity )
{
	char tail[64];
	sprintf_s( tail, "},\"nonce\":\"%u\"}", ++Nonce );
	std::string cmd = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string( GetCurrentProcessId() );
	if( !activity.empty() ) cmd += ",\"activity\":" + activity; // no activity = clear the presence
	cmd += tail;
	return WriteFrame( OP_FRAME, cmd ) && WaitAnswer( 3000 );
}

DWORD WINAPI Worker( void* )
{
	bool connected = false;
	DWORD retryAt = GetTickCount();
	std::string sent;
	bool hasSent = false;
	DWORD lastSend = 0;
	while( Running ){
		WaitForSingleObject( WakeEvent, 1000 );
		if( !Running ) break;
		if( !connected ){
			if( (int)( GetTickCount() - retryAt ) < 0 ) continue;
			connected = Connect();
			if( !connected ){ retryAt = GetTickCount() + 15000; continue; }
			hasSent = false;
		}
		if( !Pump() ){ Disconnect(); connected = false; retryAt = GetTickCount() + 5000; continue; }

		EnterCriticalSection( &DiscordLock );
		std::string activity = PendingActivity;
		LeaveCriticalSection( &DiscordLock );
		if( hasSent && activity == sent ) continue;
		if( hasSent && GetTickCount() - lastSend < 4000 ) continue; // picked up on a later wake
		if( !SendActivity( activity ) ){ Disconnect(); connected = false; retryAt = GetTickCount() + 5000; continue; }
		sent = activity;
		hasSent = true;
		lastSend = GetTickCount();
	}
	if( connected ) SendActivity( "" );
	Disconnect();
	return 0;
}

//----- activity (main thread) ----------------------------------------------
void AppendCommon( std::string& out )
{
	// "Diablo: The Hell 4 (ALPHA 2), v0.774"
	out += ",\"assets\":{\"large_image\":\"logo\",\"large_text\":";
	AppendJsonString( out, THE_HELL_VERSION_HUMAN_STRING );
	out += '}';
}

std::string BuildMenuActivity()
{
	std::string out = "{\"details\":\"In the main menu\"";
	AppendCommon( out );
	out += '}';
	return out;
}

std::string BuildGameActivity()
{
	Player& player = Players[CurrentPlayerIndex];
	char details[128];
	sprintf_s( details, "%s, Level %d", GetFullClassName( CurrentPlayerIndex ), (int)player.CharLevel );

	char location[96];
	strncpy_s( location, Dungeon->name ? Dungeon->name : "", _TRUNCATE );
	if( location[0] >= 'a' && location[0] <= 'z' ) location[0] -= 'a' - 'A'; // "the Infested Cellar"

	const char* mode = ( GameMode == GM_IRONMAN && ( player.gameChanger & BIT(GC_24_RUTHLESS) ) )
		? "Ruthless Iron" : getGameModeNameEnglish( GameMode );
	char state[192];
	if( GameMode == GM_COLISEUM ) sprintf_s( state, "%s | %s", location, mode ); // no difficulty there
	else sprintf_s( state, "%s | %s | %s", location, DifNameEnglish(), mode );

	std::string out = "{\"details\":";
	AppendJsonString( out, details );
	out += ",\"state\":";
	AppendJsonString( out, state );
	if( SessionStart ) out += ",\"timestamps\":{\"start\":" + std::to_string( SessionStart ) + "}";
	if( MaxCountOfPlayersInGame > 1 ){
		int inGame = 0;
		for( int i = 0; i < MaxCountOfPlayersInGame && i < PlayersMax_4; ++i ){
			if( Players[i].IsExists ) ++inGame;
		}
		if( inGame < 1 ) inGame = 1;
		out += ",\"party\":{\"id\":\"th4-game\",\"size\":[" + std::to_string( inGame ) + ","
			+ std::to_string( (int)MaxCountOfPlayersInGame ) + "]}";
	}
	AppendCommon( out );
	out += '}';
	return out;
}

void Publish( const std::string& activity )
{
	if( HasPublished && activity == LastPublished ) return;
	LastPublished = activity;
	HasPublished = true;
	EnterCriticalSection( &DiscordLock );
	PendingActivity = activity;
	LeaveCriticalSection( &DiscordLock );
	SetEvent( WakeEvent );
}

bool IsValidAppId( const char* s )
{
	size_t len = s ? strlen( s ) : 0;
	if( len < 15 || len >= sizeof( AppId ) ) return false;
	for( size_t i = 0; i < len; ++i ) if( s[i] < '0' || s[i] > '9' ) return false;
	return true;
}

} // namespace

//----- (integrations) --------------------------------------------------------
// after the config is loaded (TH1dll.cpp)
void DiscordRpc_Init()
{
	if( !DiscordRichPresence || Thread ) return;
	const char* id = DiscordAppIdFromConfig();
	strcpy_s( AppId, IsValidAppId( id ) ? id : DefaultDiscordAppId );
	InitializeCriticalSection( &DiscordLock );
	WakeEvent = CreateEventA( nullptr, FALSE, FALSE, nullptr );
	if( !WakeEvent ) return;
	Running = 1;
	Thread = CreateThread( nullptr, 0, Worker, nullptr, 0, nullptr );
	if( !Thread ){ Running = 0; return; }
	DiscordRpc_InMenu();
}

// start of a game session (RunGameLoop): the elapsed time counts from here
void DiscordRpc_GameStarted()
{
	if( !Thread ) return;
	SessionStart = _time64( nullptr );
	LastBuildTick = GetTickCount() - 10000; // build on the first DiscordRpc_Update
}

// every iteration of the game loop; cheap, rebuilds the snapshot every 2 seconds
void DiscordRpc_Update()
{
	if( !Thread ) return;
	DWORD now = GetTickCount();
	if( now - LastBuildTick < 2000 ) return;
	LastBuildTick = now;
	Publish( BuildGameActivity() );
}

// back in the menus (after RunGameLoop)
void DiscordRpc_InMenu()
{
	if( !Thread ) return;
	SessionStart = 0;
	Publish( BuildMenuActivity() );
}
