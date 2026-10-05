// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
namespace lab::input_block {
// Process-wide user32 detours (MinHook) that hide mouse and keyboard from the
// game while the in-game panel owns input: frozen GetCursorPos, ignored
// SetCursorPos, remembered-but-not-applied ClipCursor (the real clip is
// released so the cursor can reach the panel; mouse-look games re-clip to a
// point every frame), released key states, blanked raw input, and input window
// messages for the panel window consumed inside PeekMessage/GetMessage before
// the game's own pump sees them. Installed once for the process lifetime and
// switched per window; when inactive every function passes straight through.
// Not covered: DirectInput/XInput device polling.
using MessageSink=bool(*)(HWND,UINT,WPARAM,LPARAM); // returns true when the panel consumed the message
bool install(MessageSink sink) noexcept;      // idempotent; false = hooks unavailable, blocking disabled
bool installed() noexcept;
// on: capture the cursor position and the game's current clip, release the real clip.
// off: re-apply the game's last requested clip only while its window is foreground.
void activate(HWND window,bool on) noexcept;
bool active() noexcept;
BOOL real_cursor_position(POINT* out) noexcept; // original GetCursorPos for the panel itself
BOOL real_clip_rect(RECT* out) noexcept;        // original GetClipCursor (tests, diagnostics)
}
