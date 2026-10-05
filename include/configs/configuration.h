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
    };

    std::string shortcut;
    bool        enableMod;
    bool        enableTsf;
    bool        fixInconsistentTextEntryCount;
    bool        autoToggleKeyboard;
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
        .input = {.enableUnicodePaste = true, .keepImeOpen = false, .posUpdatePolicy = "", .meridianSupport = true, .prismaAvoidance = true, .skseMenuFrameworkSupport = true}
    };
}

} // namespace Ime
