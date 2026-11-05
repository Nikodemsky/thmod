#include "stdafx.h"

// (thmod) Character panel, "More" page: technical stats of the current hero.
//
// The page replaces the stats page inside the same panel (CharPanelRect), so the panel's
// position, conflicts with other panels, click blocking and UI rects are all shared with it.
// Toggled with "/" (ToggleCharMorePanel), C goes back to the stats page (ToggleCharPanel).
// Two tabs above the panel (Character / Cap/Stats, Data\CharTabs.CEL) switch the pages with the mouse
// or the gamepad cursor (A); on the gamepad L3 also goes closed -> Character -> Cap/Stats -> closed.
//
// Background: Data\CharMore.CEL (320x462) from the optional TH_ns_assets.mor archive. Without it
// the quest log background is used and the labels are drawn as text.
//
// Data kept here (CharStats):
//  - playtime, total for the hero and for the current session (real time, the pause and the
//    single player game menu excluded),
//  - kills by type (bosses / champions / grunts); the total is the TH4 Player::totalKills,
//    counted under the same rules (single player, not Coliseum, see CreditMonsterKill),
//  - the length of the last finished attack and spell cast, in animation frames (see ActionEnd).
// Playtime and kill counters are stored as their own entry in the hero archive, so the TH4 save
// format itself is unchanged and a TH4 build without this code just ignores the entry.

bool CharPanelMorePage = false;
static char* Data_CharMoreCEL = nullptr;
static char* Data_CharTabsCEL = nullptr;			// 4 frames, 95x23: Character before/after, Cap/Stats before/after

// Tabs above the panel. "before" = not the shown page, "after" = the shown page (active tab).
DisplayObject CharPanelTabsRect;					// both tabs together: UI rect for clicks, hover and effects
static DisplayObject CharPanelTabRects[2];			// 0 = Character, 1 = Cap/Stats
static constexpr int CharTabWidth = 95;
static constexpr int CharTabHeight = 23;
static constexpr int CharTabOverlap = 2;			// the tabs cover the top 2 rows of the panel frame
static constexpr int CharTabX[2] = { 6, 102 };		// relative to the panel

//----------------------------------------------------------------------------------------------
// Stats
//----------------------------------------------------------------------------------------------

static constexpr uint CharStatsSignature = 'TMCS';
static constexpr uint CharStatsVersion = 1;
static char CharStatsEntryName[] = "thmodcharstats"; // entry in the hero archive

struct CharStatsSave
{
	uint	signature;
	uint	version;
	i64		playtimeUs;			// total playtime of the hero, microseconds
	uint	bossKills;			// monster.newBossId != 0 (uniques)
	uint	championKills;		// monster.ChampionNumber > 0
	uint	gruntKills;			// everything else ("normal" on the panel), boss minions included
	uint	reserved[9];		// zero; room for more counters without a format change
};
static_assert( sizeof(CharStatsSave) == 64, "CharStatsSave is a part of the hero save" );

static CharStatsSave CharStats {};
static i64 SessionPlaytimeUs = 0;
static bool PlaytimeClockRunning = false;
static std::chrono::steady_clock::time_point PlaytimeLastTick;

static int ActionStartTick = 0;
static int ActionStartType = -1;	// PCA_* of the measured action, -1 = none
static int LastAttackFrames = 0;	// 0 = not measured yet
static int LastCastFrames = 0;

//----- (thmod) -----------------------------------------------------------
// Called from GameLogic once per logic tick, only while the game is running (not paused, not in
// the single player game menu). A gap longer than a second means the game was not running in
// between (inactive window, loading screen, pause): such a gap counts as one tick.
void CharStats_Tick()
{
	using namespace std::chrono;
	const steady_clock::time_point now = steady_clock::now();
	if( PlaytimeClockRunning ){
		i64 us = duration_cast<microseconds>( now - PlaytimeLastTick ).count();
		if( us < 0 || us > 1'000'000 ) us = 1'000'000 / ENGINE_FPS;
		CharStats.playtimeUs += us;
		SessionPlaytimeUs += us;
	}
	PlaytimeLastTick = now;
	PlaytimeClockRunning = true;
}

//----- (thmod) -----------------------------------------------------------
// Called from CreditMonsterKill for the current player, after the TH4 total was counted
void CharStats_AddKill( int monsterIndex )
{
	uint* counter = &CharStats.gruntKills;
	if( monsterIndex >= 0 && monsterIndex < Monsters_Max_Count ){
		const Monster& monster = Monsters[monsterIndex];
		if( monster.newBossId ) counter = &CharStats.bossKills;
		else if( monster.ChampionNumber ) counter = &CharStats.championKills;
	}
	if( *counter < UINT_MAX ) ++*counter;
}

//----- (thmod) -----------------------------------------------------------
// Attack / cast length, measured: the logic tick of Start*() of the action is kept, and when its
// PM_Do*() reports it finished the difference is taken. Two ints, set once per action -- nothing
// runs in between. An interrupted action (got hit, death, ...) never finishes and is not recorded.
// The engine starts the next action on the tick the previous one ends, in place of its last frame,
// so an N-frame animation (skipped frames already taken off) takes N - 1 ticks. Shown as N, which
// is how TH4 counts frames (ClassPropTable, "fast block: 2 frames", ...).
void CharStats_ActionStart( int playerIndex, int action )
{
	if( playerIndex != CurrentPlayerIndex ) return;
	ActionStartTick = WorldFrameIndex;
	ActionStartType = action;
}

//----- (thmod) -----------------------------------------------------------
void CharStats_ActionEnd( int playerIndex, int action )
{
	if( playerIndex != CurrentPlayerIndex || action != ActionStartType ) return;
	const int frames = WorldFrameIndex - ActionStartTick;
	ActionStartType = -1;
	if( frames <= 0 || frames > 255 ) return; // not a real measurement (e.g. tick counter restarted)
	if( action == PCA_9_SPELL ) LastCastFrames = frames + 1;
	else LastAttackFrames = frames + 1; // PCA_4_ATTACK or PCA_5_RANGE_ATTACK
}

//----- (thmod) -----------------------------------------------------------
static void CharStats_WriteEntry( const CharStatsSave& stats )
{
	static uchar buffer[ CalcEncodeDstBytes( sizeof(CharStatsSave) ) ];
	CharStatsSave out = stats;
	out.signature = CharStatsSignature;
	out.version = CharStatsVersion;
	memcpy( buffer, &out, sizeof(CharStatsSave) );
	EncodeFile( buffer, sizeof(CharStatsSave), sizeof(buffer), CryptKey );
	MpqAddFile( CharStatsEntryName, buffer, sizeof(buffer) );
}

//----- (thmod) -----------------------------------------------------------
// Called from UpdatePlayerFile, while the hero archive is open for writing
void CharStats_WriteToSave()
{
	CharStats_WriteEntry( CharStats );
}

//----- (thmod) -----------------------------------------------------------
// Called from UiCreateHero: a new hero (or one re-created under an existing name) starts from zero
void CharStats_WriteEmptyToSave()
{
	CharStatsSave empty {};
	CharStats_WriteEntry( empty );
}

//----- (thmod) -----------------------------------------------------------
static bool CharStats_ReadEntry( HANDLE save, CharStatsSave& stats )
{
	memset( &stats, 0, sizeof(CharStatsSave) );
	HANDLE entry;
	if( ! SFileOpenFileEx( save, CharStatsEntryName, 0, &entry ) ){
		return false; // a hero saved without this code
	}
	bool ok = false;
	const DWORD fileSize = SFileGetFileSize( entry, 0 );
	if( fileSize ){
		uchar* data = (uchar*)AllocMem( fileSize );
		uint bytesRead = 0;
		if( SFileReadFile( entry, data, fileSize, (DWORD*)&bytesRead, 0 ) ){
			const uint size = DecodeFile( data, fileSize, CryptKey );
			if( size == sizeof(CharStatsSave) ){
				memcpy( &stats, data, sizeof(CharStatsSave) );
				ok = stats.signature == CharStatsSignature && stats.version >= 1;
			}
		}
		FreeMem( data );
	}
	SFileCloseFile( entry );
	if( ! ok ) memset( &stats, 0, sizeof(CharStatsSave) );
	return ok;
}

//----- (thmod) -----------------------------------------------------------
// heroSelected: the hero was chosen in the menu (LoadCurrentPlayerInfo) -- everything is loaded
// and a new session starts. Otherwise it is Load Game (ReadSaveFile): only the kill counters go
// back to the saved state, the playtime keeps what was really played.
void CharStats_ReadFromSave( HANDLE save, bool heroSelected )
{
	CharStatsSave loaded;
	const bool ok = CharStats_ReadEntry( save, loaded );
	if( heroSelected ){
		CharStats = loaded; // zero when there is no entry
		SessionPlaytimeUs = 0;
		PlaytimeClockRunning = false;
		ActionStartType = -1;
		LastAttackFrames = 0;
		LastCastFrames = 0;
	}else if( ok ){
		CharStats.bossKills = loaded.bossKills;
		CharStats.championKills = loaded.championKills;
		CharStats.gruntKills = loaded.gruntKills;
		PlaytimeClockRunning = false; // the loading screen is not playtime
		ActionStartType = -1;
	}
}

//----------------------------------------------------------------------------------------------
// Panel
//----------------------------------------------------------------------------------------------

//----- (thmod) -----------------------------------------------------------
void CharPanelMore_LoadGfx()
{
	HANDLE file;
	if( ! Data_CharMoreCEL && File_Open( "Data\\CharMore.CEL", &file, ONE_TRY ) ){ // optional: TH_ns_assets.mor
		File_Close( file );
		Data_CharMoreCEL = (char*)LoadFile( "Data\\CharMore.CEL" );
	}
	if( ! Data_CharTabsCEL && File_Open( "Data\\CharTabs.CEL", &file, ONE_TRY ) ){ // optional as well: no file, no tabs
		File_Close( file );
		Data_CharTabsCEL = (char*)LoadFile( "Data\\CharTabs.CEL" );
	}
}

//----- (thmod) -----------------------------------------------------------
void CharPanelMore_FreeGfx()
{
	FreeMemZero( Data_CharMoreCEL );
	FreeMemZero( Data_CharTabsCEL );
}

//----- (thmod) -----------------------------------------------------------
// Called from SetupDisplayObjects right after CharPanelRect (resolution changes, panel layout).
// Above the panel when there is room for it (the panel top at y >= 21, from about 648 px of screen
// height); lower resolutions push the panel to y = 0, then the tabs go over its top edge instead.
void CharPanelTabs_Setup()
{
	int top = CharPanelRect.Top - ( CharTabHeight - CharTabOverlap );
	LimitToMin( top, 0 );
	for( int i = 0; i < 2; ++i ){
		SetupDisplayObject( CharPanelTabRects[i], CharPanelRect.Left + CharTabX[i], top, CharTabWidth, CharTabHeight );
	}
	SetupDisplayObject( CharPanelTabsRect, CharPanelRect.Left + CharTabX[0], top, CharTabX[1] + CharTabWidth - CharTabX[0], CharTabHeight );
}

//----- (thmod) -----------------------------------------------------------
// The tabs are shown: the character panel is open and Data\CharTabs.CEL was found
bool CharPanelTabs_Visible()
{
	return IsCHARPanelVisible && Data_CharTabsCEL;
}

//----- (thmod) -----------------------------------------------------------
// The cursor (mouse or gamepad) is over the tabs of the open character panel
bool CursorIntoCharPanelTabs()
{
	return CharPanelTabs_Visible() && CursorIntoDisplayObject( CharPanelTabsRect );
}

//----- (thmod) -----------------------------------------------------------
// Drawn after the panel, so they lie on its frame
void CharPanelTabs_Draw()
{
	if( ! CharPanelTabs_Visible() ) return;
	for( int i = 0; i < 2; ++i ){
		const bool active = ( i == 1 ) == CharPanelMorePage;
		const DisplayObject& tab = CharPanelTabRects[i];
		Surface_DrawCEL( tab.Left + Screen_LeftBorder, tab.Down + Screen_TopBorder, Data_CharTabsCEL, i * 2 + ( active ? 2 : 1 ), CharTabWidth );
	}
}

//----- (thmod) -----------------------------------------------------------
// Left click / gamepad A on the tabs (Mouse_LButtonDown, XinputHandleAButtonLeftClick)
void CharPanelTabs_Click()
{
	for( int i = 0; i < 2; ++i ){
		if( CursorIntoDisplayObject( CharPanelTabRects[i] ) ){
			const bool morePage = i == 1;
			if( CharPanelMorePage != morePage ){
				CharPanelMorePage = morePage;
				PlayGlobalSound(75);
			}
			return;
		}
	}
}

//----- (thmod) -----------------------------------------------------------
// Hover over the tabs (DispatchCursorAboveIsoland): which page and its hotkeys
void CharPanelTabs_Tooltip()
{
	for( int i = 0; i < 2; ++i ){
		if( CursorIntoDisplayObject( CharPanelTabRects[i] ) ){
			Tooltip_Clear();
			Tooltip_AddLine( i == 0 ? "Character  (C)" : "Cap/Stats  (/)", C_0_White );
			return;
		}
	}
}

//----- (thmod) -----------------------------------------------------------
// Gamepad L3: closed -> Character -> Cap/Stats -> closed
void CharPanel_PadCycle()
{
	if( ! IsCHARPanelVisible ){
		ToggleCharPanel(); // opens on the Character page (closes the conflicting panels, plays the sound)
		return;
	}
	if( ! CharPanelMorePage ){
		CharPanelMorePage = true;
	}else{
		IsCHARPanelVisible = false;
	}
	PlayGlobalSound(75);
}

//----- (thmod) -----------------------------------------------------------
// "/": opens the character panel on the More page, switches the stats page to it, or closes it
void ToggleCharMorePanel()
{
	if( IsCHARPanelVisible ){
		if( CharPanelMorePage ){
			IsCHARPanelVisible = false;
		}else{
			CharPanelMorePage = true;
		}
		PlayGlobalSound(75);
		return;
	}
	ToggleCharPanel(); // opens the panel: closes the conflicting ones and plays the sound
	CharPanelMorePage = true;
}

// Value boxes of Data\CharMore.CEL: the dark inside of each box, relative to the panel
struct MoreBox { short x, y, w, h; };
enum MoreBoxId {
	MB_PLAYTIME_SESSION, MB_PLAYTIME_TOTAL,
	MB_KILLS_TOTAL, MB_KILLS_BOSSES, MB_KILLS_CHAMPIONS, MB_KILLS_GRUNTS,
	MB_STATCAP_CURRENT, MB_STATCAP_MAX, MB_STAT_SUM,
	MB_CAP_STR, MB_CAP_MAG, MB_CAP_DEX, MB_CAP_VIT,
	MB_ATTACK_FRAMES, MB_CAST_FRAMES, MB_CHAIN_AGGRO,
	MB_BASE_HP, MB_BASE_MP,
	MB_COUNT
};
static constexpr MoreBox MoreBoxes[MB_COUNT] = {
	{  20,  40, 135, 21 }, { 167,  40, 135, 21 },
	{  19, 106,  63, 21 }, {  91, 106,  63, 21 }, { 165, 106,  63, 21 }, { 239, 106,  63, 21 },
	{ 189, 152,  50, 21 }, { 252, 152,  50, 21 }, { 252, 182,  50, 21 },
	{  66, 236,  50, 21 }, { 128, 236,  50, 21 }, { 190, 236,  50, 21 }, { 252, 236,  50, 21 },
	{ 252, 286,  50, 21 }, { 252, 316,  50, 21 }, { 252, 346,  50, 21 },
	{ 104, 396,  50, 21 }, { 252, 396,  50, 21 },
};

// Labels, only drawn when Data\CharMore.CEL is missing (they are a part of the picture)
struct MoreLabel { short x, y; const char* text; };
static constexpr MoreLabel MoreLabels[] = {
	{  17,  28, "Playtime: session / total" },
	{  17,  96, "Kills: total / bosses / champs / normal" },
	{  17, 165, "Statcap current / max" },
	{  17, 195, "Sum of current attributes" },
	{  17, 227, "Attributes cap - str / mag / dex / vit" },
	{  17, 299, "Current attack frames" },
	{  17, 329, "Current casting frames" },
	{  17, 359, "Chain aggro" },
	{  17, 409, "Max base HP" },
	{ 160, 409, "Max base MP" },
};

//----- (thmod) -----------------------------------------------------------
// Centered in the box; a value too wide for it is drawn with tighter letter spacing
static void DrawMoreValue( MoreBoxId id, const char* text, int color )
{
	const MoreBox& box = MoreBoxes[id];
	const int left = CharPanelRect.Left + box.x + 1;
	const int right = CharPanelRect.Left + box.x + box.w - 2;
	const int baseline = CharPanelRect.Top + box.y + ( box.h >= 22 ? 16 : 15 );
	const int width = GetTextWidth( text );			// letters + 1 px after each
	if( width <= right - left + 1 ){
		DrawText( left, baseline, right, text, color );
		return;
	}
	const int letters = (int)strlen( text );
	const int scale = width - letters <= right - left + 1 ? 0 : -1; // 0 px or -1 px between the letters
	char buffer[32];
	strncpy( buffer, text, sizeof(buffer) - 1 );
	buffer[sizeof(buffer) - 1] = 0;
	DrawTextWithScale( left, baseline, right, buffer, color, scale );
}

//----- (thmod) -----------------------------------------------------------
static void FormatPlaytime( char (&buffer)[32], i64 us )
{
	const i64 seconds = us / 1'000'000;
	sprintf( buffer, "%lld:%02d:%02d", seconds / 3600, int(seconds / 60 % 60), int(seconds % 60) );
}

//----- (thmod) -----------------------------------------------------------
// up to 99999 as is, then 123.4K / 12.3M, so it fits the kill boxes
static void FormatCount( char (&buffer)[32], uint value )
{
	if( value < 100'000 ){
		sprintf( buffer, "%u", value );
	}else if( value < 1'000'000 ){
		sprintf( buffer, "%u.%uK", value / 1000, value % 1000 / 100 );
	}else{
		sprintf( buffer, "%u.%uM", value / 1'000'000, value % 1'000'000 / 100'000 );
	}
}

//----- (thmod) -----------------------------------------------------------
void CharPanelMore_Draw()
{
	const int xoffset = CharPanelRect.Left;
	const int yoffset = CharPanelRect.Top;
	if( Data_CharMoreCEL ){
		Surface_DrawCEL( xoffset + Screen_LeftBorder, CharPanelRect.Down + Screen_TopBorder, Data_CharMoreCEL, 1, CharPanelRect.Width );
	}else{
		Surface_DrawCEL( xoffset + Screen_LeftBorder, CharPanelRect.Down + Screen_TopBorder, Data_QuestCEL, 1, CharPanelRect.Width );
		for( const MoreLabel& label : MoreLabels ){
			DrawText( xoffset + label.x, yoffset + label.y, 0, label.text, C_3_Gold );
		}
	}

	Player& player = Players[CurrentPlayerIndex];
	char buffer[32];

	// playtime
	FormatPlaytime( buffer, SessionPlaytimeUs );
	DrawMoreValue( MB_PLAYTIME_SESSION, buffer, C_0_White );
	FormatPlaytime( buffer, CharStats.playtimeUs );
	DrawMoreValue( MB_PLAYTIME_TOTAL, buffer, C_0_White );

	// kills (TH4 total + the split counted here)
	FormatCount( buffer, player.totalKills );
	DrawMoreValue( MB_KILLS_TOTAL, buffer, C_0_White );
	FormatCount( buffer, CharStats.bossKills );
	DrawMoreValue( MB_KILLS_BOSSES, buffer, C_0_White );
	FormatCount( buffer, CharStats.championKills );
	DrawMoreValue( MB_KILLS_CHAMPIONS, buffer, C_0_White );
	FormatCount( buffer, CharStats.gruntKills );
	DrawMoreValue( MB_KILLS_GRUNTS, buffer, C_0_White );

	// statcap: current = the cap at the current level, max = what the hero can reach at the maximum
	// level -- the level cap, or less when the attribute caps add up to less (as in LevelPointsToMax)
	const PlayerStat maxStat = GetPlayerMaxStat( CurrentPlayerIndex );
	const int statSum = CalcCharBaseStatSum( player );
	const int capCurrent = GetStatSumLimit( CurrentPlayerIndex, player.CharLevel );
	int capMax = GetStatSumLimit( CurrentPlayerIndex, PlayerMaxLevel() );
	LimitToMax( capMax, maxStat.Strength + maxStat.Magic + maxStat.Dexterity + maxStat.Vitality );
	sprintf( buffer, "%i", capCurrent );
	DrawMoreValue( MB_STATCAP_CURRENT, buffer, statSum >= capCurrent ? C_3_Gold : C_0_White );
	sprintf( buffer, "%i", capMax );
	DrawMoreValue( MB_STATCAP_MAX, buffer, statSum >= capMax ? C_3_Gold : C_0_White );
	sprintf( buffer, "%i", statSum );
	DrawMoreValue( MB_STAT_SUM, buffer, statSum >= capCurrent ? C_3_Gold : C_0_White );

	// attribute caps, gold when the base value has reached it
	sprintf( buffer, "%i", maxStat.Strength );
	DrawMoreValue( MB_CAP_STR, buffer, player.BaseStrength >= maxStat.Strength ? C_3_Gold : C_0_White );
	sprintf( buffer, "%i", maxStat.Magic );
	DrawMoreValue( MB_CAP_MAG, buffer, player.BaseMagic >= maxStat.Magic ? C_3_Gold : C_0_White );
	sprintf( buffer, "%i", maxStat.Dexterity );
	DrawMoreValue( MB_CAP_DEX, buffer, player.BaseDexterity >= maxStat.Dexterity ? C_3_Gold : C_0_White );
	sprintf( buffer, "%i", maxStat.Vitality );
	DrawMoreValue( MB_CAP_VIT, buffer, player.BaseVitality >= maxStat.Vitality ? C_3_Gold : C_0_White );

	// last finished attack / cast, "-" until the first one this session
	if( LastAttackFrames ) sprintf( buffer, "%i", LastAttackFrames ); else strcpy( buffer, "-" );
	DrawMoreValue( MB_ATTACK_FRAMES, buffer, C_0_White );
	if( LastCastFrames ) sprintf( buffer, "%i", LastCastFrames ); else strcpy( buffer, "-" );
	DrawMoreValue( MB_CAST_FRAMES, buffer, C_0_White );

	// monster chain activation radius here and now (depends on the level)
	sprintf( buffer, "%i", GetChainActivationRadius() );
	DrawMoreValue( MB_CHAIN_AGGRO, buffer, C_0_White );

	// the highest base life / mana (no items) the hero can have: base Vitality / Magic at their caps,
	// at the maximum level -- the same class formula the game uses (CalcBaseLifeMana). Gold = reached.
	int maxBaseLife = player.MaxBaseLife, maxBaseMana = player.MaxBaseMana;
	CalcBaseLifeMana( CurrentPlayerIndex, maxStat.Vitality, maxStat.Magic, PlayerMaxLevel(), maxBaseLife, maxBaseMana );
	sprintf( buffer, "%i", maxBaseLife >> 6 );
	DrawMoreValue( MB_BASE_HP, buffer, player.MaxBaseLife >= maxBaseLife ? C_3_Gold : C_0_White );
	sprintf( buffer, "%i", maxBaseMana >> 6 );
	DrawMoreValue( MB_BASE_MP, buffer, player.MaxBaseMana >= maxBaseMana ? C_3_Gold : C_0_White );
}
