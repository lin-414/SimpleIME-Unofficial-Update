//
// Created by jamie on 2026/3/8.
//

#pragma once

#include <limits>
#include <string>
#include <vector>

namespace Ime
{
//! @brief Temp struct to hold the configuration data loaded from or to be saved to the configuration file.
//! This struct is designed to be easily serializable and deserializable to and from the configuration file,
//! and should not contain any logic or data that is not directly related to the configuration file.
struct Configuration
{
    static constexpr std::uint32_t INVALID_COLOR = std::numeric_limits<uint32_t>::max();

    struct Logging
    {
        std::string level;
        std::string flushLevel;
    };

    struct Resources
    {
        std::string              translationDir;
        std::vector<std::string> fontPathList;
    };

    struct Appearance
    {
        /// Theme palette style: "default" (curated static palette) or "material" (dynamic color from themeSourceColor).
        /// The pre-rename spelling "boutique" is still accepted when loading.
        std::string themeStyle;
        uint32_t    themeSourceColor;
        double      themeContrastLevel;
        bool        themeDarkMode;
        std::string language;
        float       zoom;
        int         errorDisplayDuration;
        bool        verticalCandidateList;
        bool        autoToggleLanguageBar;
    };

    struct Input
    {
        bool        enableUnicodePaste;
        bool        keepImeOpen;
        std::string posUpdatePolicy;
        /// IME input support for Meridian UI (CEF) views (e.g. Tailor).
        bool        meridianSupport;
        /// Stand down while a Prisma UI (Ultralight, native IME) owns the keyboard.
        bool        prismaAvoidance;
        /// IME input support for SKSE Menu Framework (ImGui) text fields.
        bool        skseMenuFrameworkSupport;
        /// Deliver committed text through the engine's own input queue when the
        /// text target is an ImGui menu that is NOT ours to feed AND does not
        /// consume Scaleform char events (Tailor 3.x reads RE::InputEvents,
        /// ModExplorerMenu forwards GFx char events into its own ImGui). Those
        /// surfaces have no Scaleform field, so the Scaleform route would
        /// silently drop the text. Which of the two a surface is gets probed.
        bool        imguiSurfaceInput;
        /// Pause the game while the settings (tool) window is open. Off by default:
        /// RE::UI::numPausesGame is what host-side "is my menu still alive?"
        /// watchdogs read, so turning this on can close mod menus that treat a
        /// paused game as focus loss. Applied when the menu is off the stack, so it
        /// takes effect on the next time the settings window opens.
        bool        pauseGameWhileSettingsOpen;
        /// Runtime cache, NOT a preference: the last 中/英 (native) state observed
        /// in-game, stamped by the mod on every config save. Seeds the next
        /// session's first text-field entry — IMEs like WeChat publish no
        /// conversion compartment, so without this the first entry has no
        /// information at all to display.
        bool        lastNativeConversion;
    };

    std::string shortcut;
    bool        enableMod;
    bool        enableTsf;
    bool        fixInconsistentTextEntryCount;
    bool        autoToggleKeyboard;
    /// Disable the IME by activating the English keyboard profile (the old
    /// behavior) instead of leaving the user's input method selected and putting
    /// it in its English state. Guaranteed to hand the game raw keys, but the
    /// profile switch lands on the session-wide input locale: the taskbar shows
    /// ENG and keeps showing it after the game exits, because no process
    /// restores another process's input locale.
    bool        switchEnglishLayoutOnDisable;
    /// Declare per-monitor-v2 DPI awareness for the whole game process at load
    /// time. Skyrim ships DPI-unaware, so Windows bitmap-stretches the frame on
    /// scaled desktops and every in-game UI (SimpleIME's included) blurs.
    bool        forceDpiAwareness;
    Logging     logging;
    Resources   resources;
    Appearance  appearance;
    Input       input;
};

// The default configuration values intentionally set to invalid and only promise some important scalar value valid.
// These invalid value should be overridden by settings_converter later.
constexpr auto GetDefaultConfiguration() -> Configuration
{
    return Configuration{
        .shortcut                      = "",
        .enableMod                     = true,
        .enableTsf                     = true,
        .fixInconsistentTextEntryCount = true,
        .autoToggleKeyboard            = false,
        .switchEnglishLayoutOnDisable  = false,
        .forceDpiAwareness             = true,
        .logging                       = {.level = "", .flushLevel = ""},
        .resources                     = {.translationDir = "", .fontPathList = {}},
        .appearance =
            {.themeStyle           = "default",
                                          .themeSourceColor      = Configuration::INVALID_COLOR,
                                          .themeContrastLevel    = 0.0,
                                          .themeDarkMode         = false,
                                          .language              = "",
                                          .zoom                  = -1.0F,
                                          .errorDisplayDuration  = 10,
                                          .verticalCandidateList = false,
                                          .autoToggleLanguageBar = true},
        .input = {.enableUnicodePaste = true, .keepImeOpen = false, .posUpdatePolicy = "", .meridianSupport = true, .prismaAvoidance = true, .skseMenuFrameworkSupport = true, .imguiSurfaceInput = true, .pauseGameWhileSettingsOpen = false, .lastNativeConversion = true}
    };
}

} // namespace Ime
