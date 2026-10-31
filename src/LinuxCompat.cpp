#include "stdafx.h"
#include "LinuxCompat.h"
#include <intrin.h>
#pragma intrinsic( _ReturnAddress )

// (fixes) Wine / Proton compatibility for the Storm meta menu. See LinuxCompat.h.
//
// strm2.dll facts this file relies on (strm2.dll MD5 b7ed82462cdaccbd1d46803214d72548, the
// one shipped with TH4 0.774; the same build TH1dll.cpp PatchStormWin() already patches):
//   0x150452EC  import slot of PeekMessageA
//   0x150452E4  import slot of DispatchMessageA
//   0x150452CC  import slot of BeginPaint
//   0x15045364  import slot of GetWindowLongA
//   0x15011A71  return address of the BeginPaint call inside SDlgBeginPaint (ordinal 201).
//               SDlgBeginPaint takes the update region, calls BeginPaint with the class
//               brush temporarily removed, and draws the dialog art ONLY if the returned
//               PAINTSTRUCT.fErase is set (strm2.dll 0x15011A7D: test [ps+4]; je skip).
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
int MenuForceRepaint = -1;
int MenuKeepFocus = -1;
bool MenuDiag = false;

// effective switches, resolved from the raw values in LinuxCompat_Init()
static bool UseIdleSleep = false;
static bool UseTimerGuard = false;
static bool UseMenuClip = true;
static bool UseHideDialogs = false;
static bool UseForceRepaint = false;
static bool UseKeepFocus = false;

// Storm dialogs created WS_EX_TRANSPARENT (every nested full-screen dialog: hero list, class,
// subclass, traits, name entry ...). See LinuxCompat_OnStormWindowCreated: the style is taken
// off the real window and reported back to Storm through the GetWindowLongA hook.
static HWND StrippedTransparent[ 32 ];

static bool IsStrippedTransparent( HWND hwnd )
{
	for( HWND h: StrippedTransparent ) if( h && h == hwnd ) return true;
	return false;
}

static void RememberStrippedTransparent( HWND hwnd )
{
	for( HWND& h: StrippedTransparent ) if( h && !IsWindow( h ) ) h = 0; // drop destroyed dialogs
	for( HWND& h: StrippedTransparent ) if( !h ){ h = hwnd; return; }
}

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
	int sdlgPaint, forced;
	int refocus;
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
	POINT pt = { -1, -1 };
	GetCursorPos( &pt ); // the real one, screen coordinates (Storm's import is the patched GetCursorPos_)
	char top[ 160 ] = "";
	for( const MsgCount& m: D.top ){
		if( !m.count ) break;
		char one[ 24 ];
		sprintf_s( one, " %04X:%d", m.msg, m.count );
		strcat_s( top, one );
	}
	DiagPrint( "[%7.1fs] %s peek=%d got=%d idle=%d maxIdleGap=%ums timer=%d paint=%d mouse=%d key=%d other=%d guard=%d |%s"
		" | disp max=%.1fms(%04X) sum=%.0fms | draw=%d avg=%.1fms max=%.1fms sum=%.0fms | sdlgpaint=%d forced=%d refocus=%d | clip=%d lost=%d | fg=%s focus=%s | ptr=%d,%d over=%s\n",
		( now - DiagStart ) / 1000.0, MainMainMenu ? "main" : "menu",
		D.peek, D.got, D.idle, D.maxIdleGap, D.timer, D.paint, D.mouse, D.key, D.other, D.guard, top,
		QpcMs( D.dispMax ), D.dispMaxMsg, QpcMs( D.dispTotal ),
		D.draws, D.draws ? QpcMs( D.drawTotal ) / D.draws : 0.0, QpcMs( D.drawMax ), QpcMs( D.drawTotal ),
		D.sdlgPaint, D.forced, D.refocus,
		D.clips, D.clipMismatch,
		WhichWindow( GetForegroundWindow() ), WhichWindow( GetFocus() ),
		pt.x, pt.y, WhichWindow( WindowFromPoint( pt ) ) );
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
// Keep the keyboard on the menu.
// While a Storm dialog runs, SDlgDialogBoxParam disables its owner: the game window under the
// main menu, the hero dialog under the class list, and so on. On Windows a click on a disabled
// owner re-activates the enabled dialog on top of it. Under Wine/KWin the click activates the
// disabled window itself; from then on keys go nowhere (TH4_menu_diag.log: fg=main / own-other
// for many seconds). So: if one of our own disabled windows is in the foreground, hand the
// foreground back to the one enabled, visible Storm dialog, and restore its focused control.
static HWND LastDialogFocus = 0;

static BOOL CALLBACK FindEnabledStormDialog( HWND hwnd, LPARAM result )
{
	char cls[ 16 ];
	if( IsWindowVisible( hwnd ) && IsWindowEnabled( hwnd ) && GetClassNameA( hwnd, cls, sizeof cls ) && !strcmp( cls, "SDlgDialog" ) ){
		*(HWND*) result = hwnd;
		return FALSE;
	}
	return TRUE;
}

static void KeepMenuFocus( DWORD now )
{
	static DWORD lastCheck = 0;
	if( now - lastCheck < 100 ) return;
	lastCheck = now;
	const HWND fg = GetForegroundWindow();
	const HWND focus = GetFocus();
	if( focus && fg && IsWindowEnabled( fg ) && IsChild( fg, focus ) ) LastDialogFocus = focus; // remember the good state
	if( !fg || IsWindowEnabled( fg ) ) return;	// enabled foreground (or another program's window): leave it
	DWORD pid = 0;
	GetWindowThreadProcessId( fg, &pid );
	if( pid != GetCurrentProcessId() ) return;
	HWND dlg = 0;
	EnumThreadWindows( GetCurrentThreadId(), FindEnabledStormDialog, (LPARAM) &dlg );
	if( !dlg ) return;
	SetForegroundWindow( dlg );
	SetActiveWindow( dlg );
	const HWND now_focus = GetFocus();
	if( !now_focus || !IsChild( dlg, now_focus ) ){
		SetFocus( LastDialogFocus && IsChild( dlg, LastDialogFocus ) ? LastDialogFocus : dlg );
	}
	if( DiagLog ) ++D.refocus;
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
	if( UseKeepFocus && !gbRunGame ) KeepMenuFocus( now );
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

// SDlgBeginPaint draws the dialog art only when BeginPaint reports fErase. The menu animation
// (pentagrams via AnimateSelectionPentagram, the title via AnimateDialogTitle) invalidates with
// bErase = FALSE, so under Wine fErase comes back FALSE and Storm validates the area WITHOUT
// drawing it: the timers run, WM_PAINT arrives ~36 times a second, nothing reaches the screen
// (TH4_menu_diag.log: paint=36..200/s, draw=0). Only full repaints (focus changes ->
// InvalidateRect(NULL) in Hell.cpp, "removes menu freezes") went through. The update region
// Storm draws was taken with GetUpdateRgn before BeginPaint, so forcing fErase makes it draw
// exactly what was invalidated.
static HDC __stdcall BeginPaint_storm( HWND hwnd, LPPAINTSTRUCT ps )
{
	const HDC dc = BeginPaint( hwnd, ps );
	if( dc && _ReturnAddress() == (void*) ( 0x15011A71 + StormReloc ) ){
		if( DiagLog ) ++D.sdlgPaint;
		if( UseForceRepaint && !ps->fErase ){
			ps->fErase = TRUE;
			if( DiagLog ) ++D.forced;
		}
	}
	return dc;
}

// Storm reads a window's extended style to decide how to paint it and what it may cover
// (WS_EX_TRANSPARENT windows are painted over and never clip other dialogs). For the dialogs
// whose WS_EX_TRANSPARENT was taken off the real window, report it as still there.
static LONG __stdcall GetWindowLongA_storm( HWND hwnd, int index )
{
	LONG v = GetWindowLongA( hwnd, index );
	if( index == GWL_EXSTYLE && IsStrippedTransparent( hwnd ) ) v |= WS_EX_TRANSPARENT;
	return v;
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
	// The 640x482 dialog space is the only place the menu can be clicked (the art is drawn
	// stretched elsewhere), so the clip stays on by default under Wine too: without it the pointer
	// wanders onto the disabled game window and the menu loses the focus (second test).
	UseMenuClip = MenuClipCursor != 0;
	UseHideDialogs = Resolve( MenuHideDialogWindows );
	UseForceRepaint = Resolve( MenuForceRepaint );
	UseKeepFocus = Resolve( MenuKeepFocus );

	if( MenuDiag && !DiagLog ){
		QueryPerformanceFrequency( &QpcFreq );
		fopen_s( &DiagLog, "TH4_menu_diag.log", "w" );
		DiagStart = GetTickCount();
		DiagReset();
		SYSTEMTIME st;
		GetLocalTime( &st );
		DiagPrint( "TH4 menu diagnostics, %04d-%02d-%02d %02d:%02d:%02d\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond );
		DiagPrint( "wine=%d version=%s\n", (int) IsWine, IsWine ? WineVersion : "-" );
		DiagPrint( "options (raw -1 auto/0 off/1 on -> effective): MenuIdleSleep %d->%d MenuTimerGuard %d->%d MenuClipCursor %d->%d MenuHideDialogWindows %d->%d MenuForceRepaint %d->%d MenuKeepFocus %d->%d\n",
			MenuIdleSleep, UseIdleSleep, MenuTimerGuard, UseTimerGuard, MenuClipCursor, UseMenuClip, MenuHideDialogWindows, UseHideDialogs, MenuForceRepaint, UseForceRepaint, MenuKeepFocus, UseKeepFocus );
		DiagPrint( "config: DDraw=%d fullscreen=%d BigWin=%d screen=%dx%d desktop=%dx%d\n",
			(int) DDraw, (int) IsFullScreen, (int) BigWin, ScreenWidth, ScreenHeight, GetSystemMetrics( SM_CXSCREEN ), GetSystemMetrics( SM_CYSCREEN ) );
		DiagPrint( "columns: peek = PeekMessage calls by Storm, got = messages removed, idle = empty peeks,"
			" maxIdleGap = longest time without an empty peek (Storm fires its timers only then),"
			" disp = DispatchMessage time, draw = Storm surface -> screen (ConvertPaletteSurfaceToTrueColor incl. Present),"
			" sdlgpaint = SDlgBeginPaint calls / forced = of those, fErase was FALSE and got forced (MenuForceRepaint),"
			" refocus = foreground handed back to the menu dialog (MenuKeepFocus),"
			" clip = ClipCursor calls / lost = the OS did not keep the clip, fg/focus = which window has them,"
			" ptr/over = real pointer position and the window under it\n" );
	}
}

void LinuxCompat_PatchStorm()
{
	if( !Storm2Base ) return;
	StormCheckTimers = (SDlgCheckTimers_t) GetProcAddress( Storm2Base, MAKEINTRESOURCEA( 203 ) );
	// Hook only when something needs it, so Windows without the diagnostics stays untouched.
	if( UseIdleSleep || UseTimerGuard || UseKeepFocus || DiagLog ){
		HookStormImport( 0x150452EC, "PeekMessageA", (void*) PeekMessage_storm );
		HookStormImport( 0x150452E4, "DispatchMessageA", (void*) DispatchMessage_storm );
	}
	if( UseForceRepaint || DiagLog ){
		HookStormImport( 0x150452CC, "BeginPaint", (void*) BeginPaint_storm );
	}
	if( UseHideDialogs ){
		HookStormImport( 0x15045364, "GetWindowLongA", (void*) GetWindowLongA_storm );
	}
}

bool LinuxCompat_MenuClipAllowed()
{
	return UseMenuClip;
}

// Called from TH1dll.cpp CreateWindowEx_storm() for every window Storm creates there.
void LinuxCompat_OnStormWindowCreated( HWND hwnd, DWORD exStyle, DWORD style, int x, int y, int w, int h )
{
	bool hidden = false, stripped = false;
	if( hwnd && UseHideDialogs && !( style & WS_CHILD ) ){
		// Storm never paints these popups itself (it blits the dialog art into its DirectDraw
		// surface, which TH4 shows through D3D), so under Wine they come out as a white
		// 640x480 block over the corner of the game. Alpha 1 instead of 0: the window must
		// stay hit-testable, it is the one that receives the menu's clicks and keys.
		// WS_EX_LAYERED together with WS_EX_TRANSPARENT makes a window click-through (on Windows
		// and in Wine alike): the nested dialogs are WS_EX_TRANSPARENT, so the clicks fell
		// through to the disabled dialog below and the hero/class lists stopped reacting
		// (second test). Take WS_EX_TRANSPARENT off the window and let GetWindowLongA_storm
		// keep reporting it to Storm, which only uses it for painting decisions.
		LONG ex = GetWindowLongA( hwnd, GWL_EXSTYLE );
		if( ex & WS_EX_TRANSPARENT ){
			RememberStrippedTransparent( hwnd );
			ex &= ~WS_EX_TRANSPARENT;
			stripped = true;
		}
		SetWindowLongA( hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED );
		hidden = SetLayeredWindowAttributes( hwnd, 0, 1, LWA_ALPHA ) != FALSE;
	}
	DiagPrint( "storm window %p: ex=%08X style=%08X at %d,%d size %dx%d%s%s\n", hwnd, exStyle, style, x, y, w, h,
		hidden ? " -> made invisible (layered alpha 1)" : "", stripped ? ", WS_EX_TRANSPARENT kept for Storm only" : "" );
}
