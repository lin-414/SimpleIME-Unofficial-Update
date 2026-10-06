#pragma once

#include "hooks/SupportState.h"

#include <string_view>

namespace RE
{
class InputEvent;
}

namespace Hooks::SkseMenuFrameworkBridge
{
/// Detect SKSEMenuFramework.dll, register a render-event callback and install
/// the raw-ASCII input filter. Safe to call when the framework is not
/// installed (stays off, retried lazily from Tick). Runs on the main thread
/// (SKSE kDataLoaded, like MeridianBridge::Install).
void Install();
/// Release the text-entry lease and reset session state (the input-dispatch
/// detour itself is never removed — it degrades to a passthrough).
void Uninstall();

/// Install-time outcome, updated again by the Tick retry (atomics; readable
/// from any thread). Backs the settings UI's compatibility caption.
[[nodiscard]] SupportState State();

/// Game-thread per-frame work (called from ImeMenu::PostDisplay): retry the
/// framework detection if it was not loaded at Install time, and force-end a
/// session whose framework render loop went silent (menu closed without the
/// ImGui state ever updating).
void Tick();

/// True while a framework text field owns the keyboard (atomic; written from
/// the framework render-event callback, readable from any thread). When true,
/// committed text must go to the framework's ImGui instead of the Scaleform
/// menu stack — the IME-thread counterpart of the text-entry lease that makes
/// ControlMap::HasTextEntry() (and thus the IME enable) true.
[[nodiscard]] bool SessionActive();

/// True when committed text should be routed into the framework's ImGui
/// fields instead of the Scaleform menu stack. Called from the IME thread
/// (SendUiString).
[[nodiscard]] bool ShouldRoute();

/// Screen position of the framework text field that most recently gained
/// focus, captured the moment WantTextInput rises (the engine cursor is then
/// still on the field the user just clicked). Backs the candidate window's
/// caret anchor for ImGui fields, which have no Scaleform caret. False before
/// the first capture; safe from any thread.
[[nodiscard]] bool HasFieldAnchor();
void              GetFieldAnchor(float &a_x, float &a_y);

/// Hand UTF-16 text over to the framework's focused ImGui field. Called from
/// the IME thread (composition end / WM_CHAR / paste); the text is queued and
/// injected via ImGuiIO_AddInputCharacter from the framework's render-event
/// callback (the same thread its ImGui frames run on).
void QueueText(std::wstring_view text);

/// The EventHandler leak healer zeroed the text-entry counter outside this
/// bridge (no real menu left on the stack). Forget our lease bookkeeping so a
/// later session can acquire a fresh one instead of silently no-oping.
void OnTextEntryCountHealed();

/// Force-end the session and release the lease (load transitions: the menu
/// stack is torn down underneath us and no ImGui frame will ever report the
/// field's deactivation).
void ForceEndSession();

/// Input-dispatch hook body (DispatchInputEventHookData thunk): while a
/// framework text session is active and the user's IME is in a composition
/// mode, zero the printable-ASCII CharEvents the game generates from
/// DirectInput — they are composition echoes that would otherwise land in the
/// ImGui field next to the committed CJK text.
void NeutralizeRawAscii(RE::InputEvent *const *events);
} // namespace Hooks::SkseMenuFrameworkBridge
