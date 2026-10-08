#pragma once

#include <imgui.h>

namespace Ime
{
//! Repairs of ImGui's input state, run every overlay frame from ImeWnd::Draw.
//! They are frame drivers, not ImeWnd state: everything they read is ImGui's
//! io, the physical keyboard or the menu stack.

//! Decide which pointer the user sees. While a menu owns the game cursor
//! (CursorMenu open — the settings window, any Scaleform menu), that cursor is
//! authoritative: MenuCursor is also what UpdateCursorPos feeds ImGui, so
//! drawing the ImGui cursor on top of it produced two overlapping arrows. In
//! gameplay the game hides its cursor, and the ImGui cursor is the only pointer
//! for the language bar. NEVER touch ToolWindowMenu's kUsesCursor from here:
//! resetting it mid-session closes the CursorMenu, which flips UpdateCursorPos
//! to GetCursorPos — a different coordinate space — so io.MousePos jumps every
//! other frame and clicks only ever land in the MenuCursor phases (the
//! "settings clicks only sometimes work" feedback loop).
void UpdateMouseCursorVisual();

/**
 * ## Why this heal exists
 *
 * Scaleform key-up events can get lost when the menu stack changes while a key
 * is held: pressing the overlay shortcut pauses the game (ToolWindowMenu has
 * kPausesGame) mid-press, so the Ctrl/L releases that follow can be swallowed by
 * the transition and never reach ImeMenu's GFx→ImGui forwarding. ImGui then
 * believes a key is still down — io.KeyMods is rebuilt from that state every
 * frame and IsKeyChordPressed requires an exact mods match plus a press edge on
 * the main key, so a stuck Ctrl makes the plain key match the chord (L alone
 * opens the overlay) and a stuck main key eats the press edge so the chord stops
 * responding entirely.
 *
 * Ground the check in the physical keyboard: when every key of the bound chord
 * reads up via GetAsyncKeyState but ImGui still holds one down, clear ImGui's
 * keyboard state (ClearInputKeys takes effect immediately, before the chord
 * evaluation below). Safe on the render thread; skipped while an ImGui text
 * field owns the keyboard (io.WantTextInput) so typing/capture is never stomped.
 */
void HealStuckShortcutKeys(ImGuiKeyChord chord);

//! Mouse counterpart of HealStuckShortcutKeys: a GFx mouse-up swallowed by a
//! menu-stack transition (opening the pausing ToolWindowMenu mid-click, the
//! console grabbing input, a stall eating the tail of a click) leaves the
//! button stuck down in ImGui — io.MouseDown stays true, so no later press has
//! a press edge and nothing becomes clickable (the wheel still scrolls). Ground
//! it in the physical buttons, exactly like the keyboard heal. The 300ms dwell
//! is not cosmetic: on a quick click the physical release precedes the GFx
//! mouse-up by a frame or two, and healing inside that window would cut a
//! legitimate press short (sliders!).
void HealStuckMouseButtons();
} // namespace Ime
