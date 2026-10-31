#include "stdafx.h"
#include "LinuxCompat.h"

// (fixes) Wine / Proton compatibility for the Storm meta menu. See LinuxCompat.h.
//
// strm2.dll facts this file relies on (strm2.dll MD5 b7ed82462cdaccbd1d46803214d72548, the
// one shipped with TH4 0.774; the same build TH1dll.cpp PatchStormWin() already patches):
//   0x150452EC  import slot of PeekMessageA
//   0x150452E4  import slot of DispatchMessageA
//   ordinal 203 SDlgCheckTimers (0x1500FCB0): walks Storm's timer list, posts WM_TIMER
//   0x1500FD40  one iteration of the dialog modal loop:
//                 PeekMessageA(PM_REMOVE) -> IsDialogMessage / Translate / Dispatch
//                 nothing queued          -> SDlgCheckTimers() + software cursor update
//               SDlgDialogBoxParam repeats it until the dialog ends, without sleeping.
// Each import slot is only overwritten if it still holds the real user32 function, so a
// different strm2.dll build is left alone (and the log says so).

bool IsWine = false;
const char* WineVersion = "";

int MenuIdleSleep = -1;
int MenuTimerGuard = -1;
int MenuClipCursor = -1;
int MenuHideDialogWindows = -1;
bool MenuDiag = false;

// effective switches, resolved from the raw values in LinuxCompat_Init()
static bool UseIdleSleep = false;
static bool UseTimerGuard = false;
static bool UseMenuClip = true;
static bool UseHideDialogs = false;

extern bool MainMainMenu; // MainMenu.cpp

typedef BOOL( __stdcall* SDlgCheckTimers_t )();
static SDlgCheckTimers_t StormCheckTimers = nullptr;

//------------------------------------------------------------------------------------------
int ParseAutoOnOff( const char* v )
{
	if( !v || !*v || !_stricmp( v, "auto" ) ) return -1;
	if( !_stricmp( v, "off" ) || !strcmp( v, "0" ) ) return 0;
	return 1;
}

static bool Resolve( int raw ){ return raw < 0 ? IsWine : raw != 0; }

//------------------------------------------------------------------------------------------
// diagnostics
static FILE* DiagLog = nullptr;
static LARGE_INTEGER QpcFreq;

static double QpcMs( long long ticks ){ return QpcFreq.QuadPart ? ticks * 1000.0 / QpcFreq.QuadPart : 0.0; }
static long long QpcNow(){ LARGE_INTEGER t; QueryPerformanceCounter( &t ); return t.QuadPart; }

struct MsgCount { UINT msg; int count; };
static struct DiagCounters {
	int peek, got, idle, timer, paint, mouse, key, other, guard;
	MsgCount top[ 8 ];
	DWORD lastIdle, maxIdleGap;
	long long dispMax; UINT dispMaxMsg; long long dispTotal;
	int draws; long long drawTotal, drawMax;
	int clips, clipMismatch;
} D;
static DWORD DiagLastFlush = 0;
static DWORD DiagStart = 0;

static void DiagPrint( const char* fmt, ... )
{
	if( !DiagLog ) return;
	va_list args;
	va_start( args, fmt );
	vfprintf( DiagLog, fmt, args );
	va_end( args );
	fflush( DiagLog );
}

static void DiagReset()
{
	memset( &D, 0, sizeof D );
	D.lastIdle = GetTickCount();
}

static const char* WhichWindow( HWND w )
{
	if( !w ) return "none";
	if( w == ghMainWnd ) return "main";
	if( w == DlgHwnd ) return "dlg";
	HWND root = GetAncestor( w, GA_ROOT );
	if( root == DlgHwnd ) return "dlg-child";
	if( root == ghMainWnd ) return "main-child";
	DWORD pid = 0;
	GetWindowThreadProcessId( w, &pid );
	return pid == GetCurrentProcessId() ? "own-other" : "foreign";
}

static void DiagCountMsg( UINT msg )
{
	++D.got;
	if( msg == WM_TIMER ) ++D.timer;
	else if( msg == WM_PAINT ) ++D.paint;
	else if( msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST ) ++D.mouse;
	else if( msg >= WM_KEYFIRST && msg <= WM_KEYLAST ) ++D.key;
	else ++D.other;
	for( MsgCount& m: D.top ){
		if( m.count && m.msg == msg ){ ++m.count; return; }
		if( !m.count ){ m.msg = msg; m.count = 1; return; }
	}
}

static void DiagTick()
{
	if( !DiagLog ) return;
	const DWORD now = GetTickCount();
	if( gbRunGame ){ DiagReset(); DiagLastFlush = now; return; } // only the meta menu is of interest
	if( now - DiagLastFlush < 1000 ) return;
	DiagLastFlush = now;
	const DWORD gapNow = now - D.lastIdle;
	if( gapNow > D.maxIdleGap ) D.maxIdleGap = gapNow;
	char top[ 160 ] = "";
	for( const MsgCount& m: D.top ){
		if( !m.count ) break;
		char one[ 24 ];
		sprintf_s( one, " %04X:%d", m.msg, m.count );
		strcat_s( top, one );
	}
	DiagPrint( "[%7.1fs] %s peek=%d got=%d idle=%d maxIdleGap=%ums timer=%d paint=%d mouse=%d key=%d other=%d guard=%d |%s"
		" | disp max=%.1fms(%04X) sum=%.0fms | draw=%d avg=%.1fms max=%.1fms sum=%.0fms | clip=%d lost=%d | fg=%s focus=%s\n",
		( now - DiagStart ) / 1000.0, MainMainMenu ? "main" : "menu",
		D.peek, D.got, D.idle, D.maxIdleGap, D.timer, D.paint, D.mouse, D.key, D.other, D.guard, top,
		QpcMs( D.dispMax ), D.dispMaxMsg, QpcMs( D.dispTotal ),
		D.draws, D.draws ? QpcMs( D.drawTotal ) / D.draws : 0.0, QpcMs( D.drawMax ), QpcMs( D.drawTotal ),
		D.clips, D.clipMismatch,
		WhichWindow( GetForegroundWindow() ), WhichWindow( GetFocus() ) );
	DiagReset();
}

MenuDiagDrawScope::MenuDiagDrawScope(): t0( DiagLog && !gbRunGame ? QpcNow() : 0 ) {}
MenuDiagDrawScope::~MenuDiagDrawScope()
{
	if( !t0 ) return;
	const long long dt = QpcNow() - t0;
	++D.draws;
	D.drawTotal += dt;
	if( dt > D.drawMax ) D.drawMax = dt;
}

void MenuDiag_OnClip( const RECT& wanted )
{
	if( !DiagLog ) return;
	++D.clips;
	RECT now;
	if( GetClipCursor( &now ) && !EqualRect( &now, &wanted ) ) ++D.clipMismatch; // the OS did not keep our clip
}

//------------------------------------------------------------------------------------------
// strm2.dll import hooks
static BOOL __stdcall PeekMessage_storm( LPMSG msg, HWND hwnd, UINT filterMin, UINT filterMax, UINT removeMsg )
{
	const BOOL got = PeekMessageA( msg, hwnd, filterMin, filterMax, removeMsg );
	const DWORD now = GetTickCount();
	if( DiagLog ) ++D.peek;
	if( got ){
		if( DiagLog && ( removeMsg & PM_REMOVE ) ) DiagCountMsg( msg->message );
		// Storm only fires its timers when a Peek comes back empty. If something keeps the
		// queue busy, the pentagrams and the title freeze; fire them ourselves every ~55 ms
		// (the interval the menu dialogs use). SDlgCheckTimers just posts WM_TIMER.
		static DWORD lastGuard = 0;
		if( UseTimerGuard && StormCheckTimers && !gbRunGame && now - D.lastIdle > 55 && now - lastGuard > 55 ){
			lastGuard = now;
			StormCheckTimers();
			if( DiagLog ) ++D.guard;
		}
	}else{
		if( DiagLog ){
			++D.idle;
			const DWORD gap = now - D.lastIdle;
			if( gap > D.maxIdleGap ) D.maxIdleGap = gap;
		}
		D.lastIdle = now;
		// Storm's loop never sleeps: under Wine every empty Peek is a wineserver round trip,
		// so the menu keeps one core and wineserver busy doing nothing.
		if( UseIdleSleep && !gbRunGame ) Sleep( 1 );
	}
	DiagTick();
	return got;
}

static LRESULT __stdcall DispatchMessage_storm( const MSG* msg )
{
	if( !DiagLog || gbRunGame ) return DispatchMessageA( msg );
	const long long t0 = QpcNow();
	const LRESULT r = DispatchMessageA( msg );
	const long long dt = QpcNow() - t0;
	D.dispTotal += dt;
	if( dt > D.dispMax ){ D.dispMax = dt; D.dispMaxMsg = msg->message; }
	return r;
}

static bool HookStormImport( uint slot, const char* name, void* hook )
{
	void* real = (void*) GetProcAddress( GetModuleHandleA( "user32.dll" ), name );
	void** p = (void**) ( slot + StormReloc );
	if( !real || *p != real ){
		DiagPrint( "hook %s: import slot 0x%08X holds %p, expected %p -- different strm2.dll? not hooked\n", name, slot, *p, real );
		return false;
	}
	*p = hook;
	DiagPrint( "hook %s: ok\n", name );
	return true;
}

//------------------------------------------------------------------------------------------
void LinuxCompat_Init()
{
	HMODULE ntdll = GetModuleHandleA( "ntdll.dll" );
	typedef const char* ( __cdecl* wine_get_version_t )();
	wine_get_version_t wgv = ntdll ? (wine_get_version_t) GetProcAddress( ntdll, "wine_get_version" ) : nullptr;
	IsWine = wgv != nullptr;
	WineVersion = wgv ? wgv() : "";

	UseIdleSleep = Resolve( MenuIdleSleep );
	UseTimerGuard = Resolve( MenuTimerGuard );
	UseMenuClip = MenuClipCursor < 0 ? !IsWine : MenuClipCursor != 0;
	UseHideDialogs = Resolve( MenuHideDialogWindows );

	if( MenuDiag && !DiagLog ){
		QueryPerformanceFrequency( &QpcFreq );
		fopen_s( &DiagLog, "TH4_menu_diag.log", "w" );
		DiagStart = GetTickCount();
		DiagReset();
		SYSTEMTIME st;
		GetLocalTime( &st );
		DiagPrint( "TH4 menu diagnostics, %04d-%02d-%02d %02d:%02d:%02d\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond );
		DiagPrint( "wine=%d version=%s\n", (int) IsWine, IsWine ? WineVersion : "-" );
		DiagPrint( "options (raw -1 auto/0 off/1 on -> effective): MenuIdleSleep %d->%d MenuTimerGuard %d->%d MenuClipCursor %d->%d MenuHideDialogWindows %d->%d\n",
			MenuIdleSleep, UseIdleSleep, MenuTimerGuard, UseTimerGuard, MenuClipCursor, UseMenuClip, MenuHideDialogWindows, UseHideDialogs );
		DiagPrint( "config: DDraw=%d fullscreen=%d BigWin=%d screen=%dx%d desktop=%dx%d\n",
			(int) DDraw, (int) IsFullScreen, (int) BigWin, ScreenWidth, ScreenHeight, GetSystemMetrics( SM_CXSCREEN ), GetSystemMetrics( SM_CYSCREEN ) );
		DiagPrint( "columns: peek = PeekMessage calls by Storm, got = messages removed, idle = empty peeks,"
			" maxIdleGap = longest time without an empty peek (Storm fires its timers only then),"
			" disp = DispatchMessage time, draw = Storm surface -> screen (ConvertPaletteSurfaceToTrueColor incl. Present),"
			" clip = ClipCursor calls / lost = the OS did not keep the clip, fg/focus = which window has them\n" );
	}
}

void LinuxCompat_PatchStorm()
{
	if( !Storm2Base ) return;
	StormCheckTimers = (SDlgCheckTimers_t) GetProcAddress( Storm2Base, MAKEINTRESOURCEA( 203 ) );
	// Hook only when something needs it, so Windows without the diagnostics stays untouched.
	if( !( UseIdleSleep || UseTimerGuard || DiagLog ) ) return;
	HookStormImport( 0x150452EC, "PeekMessageA", (void*) PeekMessage_storm );
	HookStormImport( 0x150452E4, "DispatchMessageA", (void*) DispatchMessage_storm );
}

bool LinuxCompat_MenuClipAllowed()
{
	return UseMenuClip;
}

// Called from TH1dll.cpp CreateWindowEx_storm() for every window Storm creates there.
void LinuxCompat_OnStormWindowCreated( HWND hwnd, DWORD exStyle, DWORD style, int x, int y, int w, int h )
{
	bool hidden = false;
	if( hwnd && UseHideDialogs && !( style & WS_CHILD ) ){
		// Storm never paints these popups itself (it blits the dialog art into its DirectDraw
		// surface, which TH4 shows through D3D), so under Wine they come out as a white
		// 640x480 block over the corner of the game. Alpha 1 instead of 0: the window must
		// stay hit-testable, it is the one that receives the menu's clicks and keys.
		SetWindowLongA( hwnd, GWL_EXSTYLE, GetWindowLongA( hwnd, GWL_EXSTYLE ) | WS_EX_LAYERED );
		hidden = SetLayeredWindowAttributes( hwnd, 0, 1, LWA_ALPHA ) != FALSE;
	}
	DiagPrint( "storm window %p: ex=%08X style=%08X at %d,%d size %dx%d%s\n", hwnd, exStyle, style, x, y, w, h, hidden ? " -> made invisible (layered alpha 1)" : "" );
}
