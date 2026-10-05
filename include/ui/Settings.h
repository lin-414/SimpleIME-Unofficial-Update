//
// Created by jamie on 2025/5/21.
//
#pragma once

#include "imgui.h"
#include "imguiex/m3/colors.h"

#include <algorithm>
#include <cstdint>
#include <spdlog/common.h>
#include <string>

namespace Ime
{

struct Settings
{
    enum class WindowPosUpdatePolicy : std::uint16_t
    {
        NONE = 0,
        BASED_ON_CURSOR,
        BASED_ON_CARET
    };
    static constexpr std::wstring_view DEFAULT_EMOJI_FONT_FAMILY  = L"Segoe UI Emoji";
    static constexpr std::wstring_view DEFAULT_SYMBOL_FONT_FAMILY = L"Segoe UI Symbol";
    /// System CJK fonts merged into the UI font chain so the Languages combo
    /// can always render 中文 / 日本語 / 한국어 even when the primary font
    /// (e.g. a Latin or simplified-Chinese one) lacks Hangul or Kana glyphs.
    static constexpr std::wstring_view DEFAULT_KOREAN_FONT_FAMILY      = L"Malgun Gothic";
    static constexpr std::wstring_view DEFAULT_JAPANESE_FONT_FAMILY    = L"Yu Gothic";
    static constexpr std::wstring_view DEFAULT_JAPANESE_FALLBACK_FAMILY = L"MS Gothic";
    static constexpr std::string_view  ICON_FILE                    = "lucide-icons.ttf";
    static constexpr auto              ZOOM_MAX                   = 2.0F;
    static constexpr auto              ZOOM_MIN                   = 0.5F;
    static constexpr int               ZOOM_STEP_PERCENT          = 25;

    //! UI layer is immediate mode(ImGui), no event callback, no commpoent search. So we need a way to present the UI layer state.
    //! This struct is for that.
    //!
    //! Thread ownership: the request* flags below cross the IME thread → render
    //! thread boundary (ImeManager runs on the IME thread, ImeOverlay consumes
    //! them during the ImGui frame), so they are atomics. Fields commented
    //! "cross-thread read" below are written on the render thread (ImGui
    //! checkboxes) and read on the IME thread: they stay plain bools ON PURPOSE
    //! — on x86 an aligned bool cannot tear, and the readers tolerate a
    //! one-frame-stale value the same way State::m_conversionMode does. Don't
    //! add new cross-thread fields without at least this guarantee.
    struct RuntimeData
    {
        bool requestCloseTopWindow = false; ///< Render thread only: request to close the current top-level UI window on the next frame.
        bool requestMonitorScale    = false; ///< Render thread only: reapply the current monitor DPI after selecting the follow-monitor zoom mode.
        /// Render thread only: one-frame latch armed by the Behaviour panel's shortcut-capture widget.
        /// ImeWnd::Draw consumes it at the top of the next frame to suppress both global overlay-toggle chord
        /// checks (they run before the capture widget, which must receive every key while armed, including a
        /// re-press of the currently-bound chord). Consumed via std::exchange, so it self-clears if the widget
        /// stops being drawn.
        bool swallowShortcutToggle = false;
        std::atomic_bool requestShowOverlay = false; ///< IME thread → render thread: show the overlay on the next frame.
        std::atomic_bool requestHideOverlay = false; ///< IME thread → render thread: hide the overlay on the next frame (if not pinned).

        bool overlayPinned     = false; ///< Render thread only: overlay was pinned by the user and should not auto-hide.
        bool overlayShowing    = false; ///< Render thread only: overlay is currently visible/rendered in this frame.
        bool toolWindowShowing = false; ///< Render thread only: auxiliary tool/settings window is currently visible.

        RuntimeData() = default;
        // The atomics are not copyable, but Settings is copied when it is loaded
        // from disk. These are transient one-frame requests, so a copy carries
        // the current value across.
        RuntimeData(const RuntimeData &other) :
            requestCloseTopWindow(other.requestCloseTopWindow),
            requestMonitorScale(other.requestMonitorScale),
            swallowShortcutToggle(other.swallowShortcutToggle),
            requestShowOverlay(other.requestShowOverlay.load()),
            requestHideOverlay(other.requestHideOverlay.load()),
            overlayPinned(other.overlayPinned),
            overlayShowing(other.overlayShowing),
            toolWindowShowing(other.toolWindowShowing)
        {
        }
        auto operator=(const RuntimeData &other) -> RuntimeData &
        {
            if (this != &other)
            {
                requestCloseTopWindow = other.requestCloseTopWindow;
                requestMonitorScale = other.requestMonitorScale;
                swallowShortcutToggle = other.swallowShortcutToggle;
                requestShowOverlay.store(other.requestShowOverlay.load());
                requestHideOverlay.store(other.requestHideOverlay.load());
                overlayPinned     = other.overlayPinned;
                overlayShowing    = other.overlayShowing;
                toolWindowShowing = other.toolWindowShowing;
            }
            return *this;
        }
    } runtimeData;

    //! Shortcut: support combination of Ctrl, Shift, Alt and a normal key. e.g. "ctrl+shift+f1", "alt+f2", "f3"...
    //! The named key is can't combine by bitwise operation, g.g. "F2 | A" will become to "F3".
    ImGuiKeyChord shortcut;
    bool          enableMod                     = true; ///< modify on UI thread every frame.
    bool          enableTsf                     = true;
    bool          fixInconsistentTextEntryCount = true; ///< modify in ToolWindow(ImeMenu). no need sync because ImeMenu is the topmost menu;
    /// Read once during plugin load (see plugin.cpp): declares per-monitor-v2
    /// process DPI awareness so the game frame (and with it every ImGui surface)
    /// renders 1:1 on scaled desktops instead of being stretched by DWM.
    bool          forceDpiAwareness             = true;
    /// Mirrors Configuration::autoToggleKeyboard and the shipped SimpleIME.toml.
    /// The TOML value wins on load, so this default only applies when the config
    /// is missing or unreadable — it must not disagree with them.
    bool          autoToggleKeyboard            = false;

    struct Logging
    {
        spdlog::level::level_enum level;
        spdlog::level::level_enum flushLevel;
    } logging;

    struct Resources
    {
        std::string              translationDir;
        std::vector<std::string> fontPathList;
    } resources;

    struct Appearance
    {
        ImGuiEx::M3::SchemeConfig schemeConfig; ///< modify in runtime by AppearancePanel; read once on Mod launch by ImeApp;
        std::string               language;
        float                     zoom; ///< modify in runtime by AppearancePanel; read once on Mod launch by ImeApp;
        int                       errorDisplayDuration;
        bool                      verticalCandidateList; ///< modify/read in runtime by UI thread.
        bool                      autoToggleLanguageBar; ///< cross-thread read: written on the render thread (panel), read on the IME thread (ImeManager) — see the ownership note above.
    } appearance;

    struct Input
    {
        bool                  enableUnicodePaste;
        bool                  keepImeOpen; ///< cross-thread read: written on the render thread (panel), read on the IME thread (ImeController) — see the ownership note above.
        WindowPosUpdatePolicy posUpdatePolicy;
        /// Read once at bridge install time (game thread, SKSE messaging); the
        /// bridges re-publish the decision through their own atomics.
        bool                  meridianSupport;
        bool                  prismaAvoidance;
        bool                  skseMenuFrameworkSupport;
    } input;
};

inline auto GetDefaultSettings() -> Settings
{
    return {
        .runtimeData                   = {},
        .shortcut                      = ImGuiKey_F2,
        .enableMod                     = true,
        .enableTsf                     = true,
        .fixInconsistentTextEntryCount = true,
        .forceDpiAwareness             = true,
        .autoToggleKeyboard            = false,
        .logging                       = {.level = spdlog::level::info, .flushLevel = spdlog::level::info},
        .resources                     = {.translationDir = "Data/interface/SimpleIME", .fontPathList = {}},
        .appearance =
            {.schemeConfig          = ImGuiEx::M3::GetDefaultSchemeConfig(false), // default theme: light
                                          .language              = "english",
                                          .zoom                  = -1.0F,
                                          .errorDisplayDuration  = 10,
                                          .verticalCandidateList = false,
                                          .autoToggleLanguageBar = true},
        .input = {.enableUnicodePaste = true, .keepImeOpen = false, .posUpdatePolicy = Settings::WindowPosUpdatePolicy::BASED_ON_CARET, .meridianSupport = true, .prismaAvoidance = true, .skseMenuFrameworkSupport = true}
    };
}

} // namespace Ime
