# The Hell 4 on Linux (Wine/Proton) – main menu fix

Status: 2026-10-31. Branch `fixes` (based on `build`), commits 6bf0418, 10ef738, 9238d06 plus the "black first menu / white flash" changes (to be committed). Tested on Proton 11 (Wine 11.0), Garuda Linux with KDE, 2560x1440. The menu works with `DDraw on`; with `DDraw off` it worked except for the first (black) main menu, which the latest change addresses.

## What was wrong (from analysing Strm2.dll, MD5 b7ed82462cdaccbd1d46803214d72548)
- The meta menu (main menu, hero/class selection, multiplayer) is not drawn by the engine. Storm builds it from real Win32 dialogs: 640x482 WS_POPUP windows at the top-left corner of the game window, which receive the clicks and keys. TH4 copies Storm's 8-bit surface into D3D9/DDraw and draws it stretched in the middle of the screen.
- **Frozen animation / "stuttering" menu**: `SDlgBeginPaint` (ordinal 201) draws the dialog art only when `BeginPaint` reports `fErase = TRUE` (0x15011A7D). The animation (pentagrams, title) invalidates with `bErase = FALSE`; under Wine `fErase` then comes back `FALSE` and Storm validates the area without drawing it. Only full repaints on focus changes (`InvalidateRect(NULL)` in Hell.cpp, commented "removes menu freezes") got through, so the menu moved only when a key was pressed.
- **White rectangle in the top-left corner**: Storm never paints its popup windows itself; Wine shows them white.
- **Clicks falling through / lost focus**: the nested dialogs are `WS_EX_TRANSPARENT`, and LAYERED + TRANSPARENT makes a window click-through. `SDlgDialogBoxParam` disables the owner window, and under Wine/KWin a click on a disabled window activates it (Windows hands the activation to the enabled dialog instead), so the keyboard went nowhere.
- **Black first menu with DDraw off**: the game window ended up above the dialog in the window order (Windows keeps owned popups above their owner; Wine lost that while the D3D9 window was being set up). Storm clips its drawing to the part of the dialog that no other window covers (0x15005A30 / 0x150061B0), so it drew nothing and the clicks went to the disabled game window. The keyboard still worked (Enter moved on).
- **White screen at start-up**: under Wine the game window is white until it is painted, and TH4 swallows `WM_ERASEBKGND`.

## The fix – src/LinuxCompat.cpp / .h
config.ini options (a missing key means `auto`; `auto` = on under Wine/Proton, off on Windows; exception: `MenuClipCursor`, where `auto` = on everywhere, as in the original game):
- `MenuForceRepaint` – hooks Strm2.dll's `BeginPaint` import (0x150452CC) and, only for the call inside `SDlgBeginPaint` (return address 0x15011A71), forces `fErase`. The main fix.
- `MenuHideDialogWindows` – Storm's popups become `WS_EX_LAYERED` with alpha 1 (invisible but still clickable). `WS_EX_TRANSPARENT` is removed from the real window and reported back to Storm through a `GetWindowLongA` import hook (0x15045364).
- `MenuKeepFocus` – if a disabled game/menu window gets the foreground, it is handed back to the one enabled `SDlgDialog` window, with its last focused control.
- `MenuKeepOnTop` – every 100 ms puts Storm's dialogs back above the game window (keeping their own order) and at their intended position, then repaints them.
- `MenuClipCursor` – keeps the pointer inside the 640x482 area where the menu can actually be clicked.
- `MenuIdleSleep`, `MenuTimerGuard` – `PeekMessageA` import hook (0x150452EC): Sleep(1) when Storm's loop has nothing to do, and a fallback call of `SDlgCheckTimers` (ordinal 203).
- `MenuDiag` – writes `TH4_menu_diag.log` (`DispatchMessageA` hook 0x150452E4, timing of `ConvertPaletteSurfaceToTrueColor`, pointer/focus/window order).
- No option: under Wine the game window is painted black right after `ShowWindow` (Windows.cpp), so there is no white flash.

With the default settings on Windows no hook is installed and the behaviour is the original one.
