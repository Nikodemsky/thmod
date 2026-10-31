#pragma once
// (fixes) Wine / Proton compatibility for the Storm meta menu (main menu, hero selection,
// multiplayer dialogs), plus an optional diagnostics log of the menu message loop.
//
// Background (see LinuxCompat.cpp for the details):
//  - The meta menu is not drawn by the engine. Strm2.dll builds it from real Win32 dialogs:
//    a WS_POPUP window laid out in a 640x480 space at the top-left corner of the game
//    window, which receives the clicks, while TH4 copies Storm's 8-bit surface into the
//    D3D9 back buffer and stretches it to the middle of the window.
//  - Storm runs its own modal loop (strm2.dll 0x1500FD40): PeekMessage(PM_REMOVE) in a
//    busy loop with no sleep; only when the queue is EMPTY does it fire its own timers
//    (SDlgCheckTimers -> posted WM_TIMER, which drive the pentagram and title animation)
//    and move the software cursor.
//  - Under Wine the popup is a real X11 window (painted white, since Storm never paints it
//    itself), ClipCursor works only conditionally, and the menu animation stalls for
//    seconds at a time (recorded on Proton 11 / KDE with both D3D9 and DDraw renderers).
//
// Every option is tri-state in config.ini: auto (default) / on / off.
// "auto" means: on under Wine/Proton, off on Windows -- so Windows behaviour is unchanged.
// Exception: MenuClipCursor, whose auto means on everywhere (the original behaviour).

extern bool IsWine;               // ntdll exports wine_get_version
extern const char* WineVersion;   // "" on Windows

// raw config values (-1 auto, 0 off, 1 on), parsed in Config.cpp GetUserParameters()
extern int MenuIdleSleep;          // Sleep(1) in Storm's menu loop when the queue is empty
extern int MenuTimerGuard;         // fire Storm's timers even when the queue never empties
extern int MenuClipCursor;         // hold the pointer inside the 640x480 dialog space (auto = on everywhere)
extern int MenuHideDialogWindows;  // make Storm's popup dialog windows (near) invisible
extern int MenuForceRepaint;       // make Storm repaint every invalidated dialog area (animations)
extern int MenuKeepFocus;          // give the foreground back to the menu dialog after a stray click
extern int MenuKeepOnTop;          // keep the menu dialogs above the game window and in their place
extern bool MenuDiag;              // write TH4_menu_diag.log

int  ParseAutoOnOff( const char* v ); // "" / "auto" -> -1, "off" / "0" -> 0, anything else -> 1

void LinuxCompat_Init();          // after the config is parsed and strm2.dll is loaded
void LinuxCompat_PatchStorm();    // hook strm2.dll's PeekMessageA / DispatchMessageA imports

bool LinuxCompat_MenuClipAllowed();
void LinuxCompat_OnStormWindowCreated( HWND hwnd, DWORD exStyle, DWORD style, int x, int y, int w, int h );
void LinuxCompat_OnMainWindowShown( HWND hwnd ); // paints the game window black under Wine (no white flash)

// diagnostics hooks (no-ops unless MenuDiag is on)
void MenuDiag_OnClip( const RECT& wanted );
struct MenuDiagDrawScope {
	long long t0;
	MenuDiagDrawScope();
	~MenuDiagDrawScope();
};
