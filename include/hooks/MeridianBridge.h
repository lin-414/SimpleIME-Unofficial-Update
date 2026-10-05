#pragma once

#include "hooks/SupportState.h"

#include <array>
#include <string_view>

namespace NL::CEF
{
class IBrowser;
}

namespace Hooks::MeridianBridge
{
/// Negotiate `Meridian.View/1` from MeridianUI.dll (if loaded) and hook its
/// public vtable `TryFocus` slot to observe which Meridian view currently
/// holds focus. This is the FALLBACK backend for Meridian 1.0-era UIs; the
/// primary focus source is the NirnLabUIPlatform backend (NirnLabBridge),
/// which feeds its browser focus events into the same session machinery.
/// Safe to call even when Meridian is not installed.
/// Runs on the main thread (SKSE kDataLoaded).
void Install();
/// Stop routing text and observing focus (teardown; the vtable detour itself
/// is never removed — it degrades to a passthrough).
void Uninstall();

/// Install-time outcome across both backends (atomic; written by Install and
/// the NirnLabBridge messaging handshake on the main thread, readable from
/// any thread). Backs the settings UI's compatibility caption.
[[nodiscard]] SupportState State();

/// A Meridian view or a UIPlatform browser currently holds focus (atomic;
/// written from the vtable hooks on the game thread, readable from any
/// thread).
[[nodiscard]] bool HasFocus();

/// True when committed text should go to the Meridian DOM instead of the
/// Scaleform menu stack. Called from the IME thread (SendUiString).
[[nodiscard]] bool ShouldRoute();

/// True while the bridge's in-DOM overlay is the active composition/candidate
/// surface (a Meridian view or UIPlatform browser is focused and its text
/// field captured). The ImGui candidate window yields to it — drawn into the
/// game's swap chain it would sit underneath Meridian's own CEF composition,
/// blurred and flickering. Readable from any thread.
[[nodiscard]] bool OwnsCandidateUi();

/// Hand UTF-16 text over to the Meridian view. Called from the IME thread
/// (composition end / WM_CHAR / paste); the text is queued and flushed to the
/// view's DOM from the game thread in Tick().
void QueueText(std::wstring_view text);

/// Game-thread per-frame work (called from ImeMenu::PostDisplay, and from
/// ImeApp::PresentHook on the render thread while that fallback frame driver
/// is active): capture the focused DOM text field, flush queued text, validate
/// focus is still alive. Single-flight — overlapping callers from the two
/// frame drivers are collapsed to one running body.
void Tick();

/// Ask for one capture probe on the next Tick and drop the current capture.
/// Called from the game thread when a Meridian UI calls AllowTextInput(true) —
/// the moment its DOM text field gains focus. At view-focus time the field is
/// not focused yet (the probe reports no-field), so this is the earliest point
/// where the DOM leak baseline can be established, before the first keystroke.
void RequestCapture();

// ---- UIPlatform backend hooks (called by NirnLabBridge) ---------------

/// A UIPlatform browser took the keyboard (IBrowser::SetBrowserFocused(true)
/// detour, game thread). Mirrors the View/1 focus handling: session state is
/// reset, a capture probe is scheduled and the IME decision re-evaluated.
void OnBrowserFocused(NL::CEF::IBrowser *browser);
/// The browser lost the keyboard (SetBrowserFocused(false) detour, or its
/// last host reference was released) — tear the session down. Game thread.
void OnBrowserFocusGone(NL::CEF::IBrowser *browser);
/// Listener payload from the UIPlatform page→host callback channel
/// (SimpleIME.result in the page). Runs on the host's CEF callback thread;
/// the payload is already JSON-decoded and uses the same `<id>:<status>`
/// protocol as the View/1 listener.
void OnBackendListenerPayload(const char *payload);

/// Resolved colors/geometry of the ImGui candidate window's M3 theme, for the
/// Meridian DOM panel to mirror (the panel must look like the normal
/// candidate window, and the M3 palette is user-themeable at runtime).
struct UiThemePalette
{
    std::array<float, 4> windowBg;         ///< ImGuiCol_WindowBg (M3-themed)
    std::array<float, 4> text;             ///< ColorRole::onSurface (composition line + word)
    std::array<float, 4> caret;            ///< ColorRole::primary (caret)
    std::array<float, 4> divider;          ///< ColorRole::outlineVariant
    std::array<float, 4> numberText;       ///< ColorRole::onSurfaceVariant (dim candidate digit)
    std::array<float, 4> chipHoverBg;      ///< ColorRole::surfaceContainerHigh (hovered candidate)
    std::array<float, 4> chipSelectedBg;   ///< ColorRole::primary (selected candidate pill)
    std::array<float, 4> chipSelectedText; ///< ColorRole::onPrimary
    std::array<float, 4> rowSelectedBg;    ///< ColorRole::primary (vertical-mode selected row, same pill as the horizontal row)
    std::array<float, 4> rowSelectedText;  ///< ColorRole::onPrimary
    float                windowRounding;   ///< ImGui style window rounding (px)
    float                scale;            ///< M3 dp→px scale (DPI × user zoom)
    std::string          primaryFontPath;  ///< UTF-8 path of the effective primary font (empty = none)
};

/// Request a theme snapshot; consumed by the next render-thread frame (the M3
/// styles are not thread-safe, so the read must happen where ImGui renders).
void RequestUiThemeRefresh();
/// Render-thread only (call from ImeWnd::Draw). Returns false if no refresh
/// was pending; true when the request should be fulfilled now via
/// ConsumeUiThemeRefresh.
bool ConsumeUiThemeRefreshRequested();
/// Render-thread only: store the snapshot for the bridge to push to the panel.
bool ConsumeUiThemeRefresh(const UiThemePalette &palette);
} // namespace Hooks::MeridianBridge
