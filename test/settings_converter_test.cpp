//
// Created by jamie on 2026/3/8.
//

#include "RandomUtils.h"
#include "configs/ConfigSerializer.h"
#include "configs/configuration.h"
#include "configs/settings_converter.h"
#include "imgui.h"
#include "imguiex/ErrorNotifier.h"
#include "ui/Settings.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <format>

void ErrorNotifier::addError(std::string_view msg, const ErrorMsg::Level level)
{
    if (level < m_currentLevel)
    {
        return;
    }
    if (errors.size() >= MaxMessages)
    {
        errors.pop_front();
    }
    errors.push_back({.text = std::string(msg), .level = level, .confirmed = false});
}

TEST(ConfigurationToSettingsTest, should_convert_shortcut_string_to_ImGuiKeyChord)
{
    Ime::Configuration configuration = Ime::GetDefaultConfiguration();

    Ime::Settings defaultSettings = Ime::GetDefaultSettings();

    configuration.shortcut = "F2";
    auto settings          = Ime::ConvertConfigurationToSettings(configuration);
    EXPECT_EQ(settings.shortcut, ImGuiKey_F2);

    configuration.shortcut = "Ctrl+F1";
    settings               = Ime::ConvertConfigurationToSettings(configuration);
    EXPECT_EQ(settings.shortcut, ImGuiMod_Ctrl | ImGuiKey_F1);

    configuration.shortcut = "Alt+F2";
    settings               = Ime::ConvertConfigurationToSettings(configuration);
    EXPECT_EQ(settings.shortcut, ImGuiMod_Alt | ImGuiKey_F2);

    configuration.shortcut = "SHiFt+F3";
    settings               = Ime::ConvertConfigurationToSettings(configuration);
    EXPECT_EQ(settings.shortcut, ImGuiMod_Shift | ImGuiKey_F3);

    configuration.shortcut = "SHiFt    +alt+CTRL+F4";
    settings               = Ime::ConvertConfigurationToSettings(configuration);
    EXPECT_EQ(settings.shortcut, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiMod_Alt | ImGuiKey_F4);

    configuration.shortcut = " F5 +  ctrl +   alt + shift  ";
    settings               = Ime::ConvertConfigurationToSettings(configuration);
    EXPECT_EQ(settings.shortcut, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiMod_Alt | ImGuiKey_F5);

    configuration.shortcut = " F7 +  ctrl +   alt + shift +super ";
    settings               = Ime::ConvertConfigurationToSettings(configuration);
    EXPECT_EQ(settings.shortcut, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiMod_Alt | ImGuiMod_Super | ImGuiKey_F7);

    configuration.shortcut = " F6 +  ctrl +   alt + shift + ";
    settings               = Ime::ConvertConfigurationToSettings(configuration);
    EXPECT_EQ(settings.shortcut, defaultSettings.shortcut) << "Should not support shortcut when not end with modifier or key";

    configuration.shortcut = " F6  + A +  ctrl +   alt";
    settings               = Ime::ConvertConfigurationToSettings(configuration);
    EXPECT_EQ(settings.shortcut, defaultSettings.shortcut) << "Should not support shortcut when contains multiple normal keys";
}

TEST(ConfigurationToSettingsTest, should_convert_spdlog_level_string_to_spdlog_level_enum)
{
    Ime::Configuration configuration = Ime::GetDefaultConfiguration();

    Ime::Settings defaultSettings = Ime::GetDefaultSettings();

    configuration.logging.level = "debug";
    auto settings               = Ime::ConvertConfigurationToSettings(configuration);
    EXPECT_EQ(settings.logging.level, spdlog::level::debug) << "should correct convert log level string to spdlog level enum";

    configuration.logging.level = "  debug   ";
    settings                    = Ime::ConvertConfigurationToSettings(configuration);
    EXPECT_EQ(settings.logging.level, spdlog::level::debug) << "should trim the log level string before convert";

    const std::vector<spdlog::level::level_enum> expectedLevels = {
        spdlog::level::trace,
        spdlog::level::debug,
        spdlog::level::info,
        spdlog::level::warn,
        spdlog::level::err,
        spdlog::level::critical,
        spdlog::level::off
    };
    for (size_t i{}; auto &testLevelString : {"traCe", "dEbug", "iNfo", "Warn", "ErR", "critiCal", "oFf"})
    {
        configuration.logging.level = testLevelString;
        settings                    = Ime::ConvertConfigurationToSettings(configuration);
        EXPECT_EQ(settings.logging.level, expectedLevels[i++]) << "should case insensitive";
    }

    configuration.logging.level = "unknown level";
    settings                    = Ime::ConvertConfigurationToSettings(configuration);
    EXPECT_EQ(settings.logging.level, defaultSettings.logging.level) << "should not support unknown log level string, and keep the default value";

    // The shipped TOML documents "error"; spdlog's display name is "warning".
    // Both spellings must map to their level instead of silently falling back
    // to the default.
    configuration.logging.level = "error";
    settings                    = Ime::ConvertConfigurationToSettings(configuration);
    EXPECT_EQ(settings.logging.level, spdlog::level::err) << "should accept the documented \"error\" spelling";

    configuration.logging.level = "warning";
    settings                    = Ime::ConvertConfigurationToSettings(configuration);
    EXPECT_EQ(settings.logging.level, spdlog::level::warn) << "should accept spdlog's \"warning\" display name";
}

TEST(ConfigurationToSettingsTest, should_convert_WindowPolicy_string_to_enum)
{
    Ime::Configuration configuration = Ime::GetDefaultConfiguration();

    Ime::Settings defaultSettings = Ime::GetDefaultSettings();

    configuration.input.posUpdatePolicy = "unknown policy";
    auto settings                       = Ime::ConvertConfigurationToSettings(configuration);
    EXPECT_EQ(settings.input.posUpdatePolicy, defaultSettings.input.posUpdatePolicy) << "should not support unknown policy string";

    configuration.input.posUpdatePolicy = " based_on_caret ";
    settings                            = Ime::ConvertConfigurationToSettings(configuration);
    EXPECT_EQ(settings.input.posUpdatePolicy, Ime::Settings::WindowPosUpdatePolicy::BASED_ON_CARET) << "should trim the policy string before convert";

    configuration.input.posUpdatePolicy = "based_On_cuRsor";
    settings                            = Ime::ConvertConfigurationToSettings(configuration);
    EXPECT_EQ(settings.input.posUpdatePolicy, Ime::Settings::WindowPosUpdatePolicy::BASED_ON_CURSOR) << "should case insensitive";
}

TEST(ConfigurationToSettingsTest, should_set_base_type_member_value_from_configuration)
{
    std::uint32_t seed = 0;
    auto configuration = ImeTest::GetRandomConfiguation(&seed);
    // Failure output includes the seed so the round-trip can be re-run with
    // RandomUtils{seed} instead of rerolling.
    SCOPED_TRACE(std::format("RandomUtils seed: {}", seed));
    // The default theme style replaces the configured source color with its fixed
    // accent, so the color passthrough asserted below is only defined for the
    // material style (the random config leaves themeStyle empty → default theme).
    configuration.appearance.themeStyle = "material";

    auto settings = Ime::ConvertConfigurationToSettings(configuration);

    EXPECT_EQ(settings.enableMod, configuration.enableMod);
    EXPECT_EQ(settings.enableTsf, configuration.enableTsf);
    EXPECT_EQ(settings.fixInconsistentTextEntryCount, configuration.fixInconsistentTextEntryCount);
    EXPECT_EQ(settings.autoToggleKeyboard, configuration.autoToggleKeyboard);
    EXPECT_EQ(settings.switchEnglishLayoutOnDisable, configuration.switchEnglishLayoutOnDisable);
    EXPECT_EQ(settings.forceDpiAwareness, configuration.forceDpiAwareness);

    EXPECT_EQ(settings.resources.translationDir, configuration.resources.translationDir);
    EXPECT_EQ(settings.resources.fontPathList, configuration.resources.fontPathList);

    EXPECT_EQ(settings.appearance.schemeConfig.sourceColor, configuration.appearance.themeSourceColor);
    EXPECT_EQ(settings.appearance.schemeConfig.contrastLevel, configuration.appearance.themeContrastLevel);
    EXPECT_EQ(settings.appearance.schemeConfig.darkMode, configuration.appearance.themeDarkMode);
    EXPECT_EQ(settings.appearance.language, configuration.appearance.language);
    // Positive zoom is clamped into [ZOOM_MIN, ZOOM_MAX] by the converter
    // (negative means "follow the monitor scaling" and passes through).
    EXPECT_EQ(settings.appearance.zoom,
              std::clamp(configuration.appearance.zoom, Ime::Settings::ZOOM_MIN, Ime::Settings::ZOOM_MAX));
    EXPECT_EQ(settings.appearance.errorDisplayDuration, configuration.appearance.errorDisplayDuration);
    EXPECT_EQ(settings.appearance.verticalCandidateList, configuration.appearance.verticalCandidateList);
    EXPECT_EQ(settings.appearance.autoToggleLanguageBar, configuration.appearance.autoToggleLanguageBar);

    EXPECT_EQ(settings.input.enableUnicodePaste, configuration.input.enableUnicodePaste);
    EXPECT_EQ(settings.input.keepImeOpen, configuration.input.keepImeOpen);
    EXPECT_EQ(settings.input.meridianSupport, configuration.input.meridianSupport);
    EXPECT_EQ(settings.input.prismaAvoidance, configuration.input.prismaAvoidance);
    EXPECT_EQ(settings.input.skseMenuFrameworkSupport, configuration.input.skseMenuFrameworkSupport);
    EXPECT_EQ(settings.input.imguiSurfaceInput, configuration.input.imguiSurfaceInput);
    EXPECT_EQ(settings.input.pauseGameWhileSettingsOpen, configuration.input.pauseGameWhileSettingsOpen);
    EXPECT_EQ(settings.input.lastNativeConversion, configuration.input.lastNativeConversion);
}

// The first conversion normalizes (clamps, parses enums, pins the default
// theme's accent); everything after that must be a fixed point. This is the
// settings→config→settings direction the targeted tests above never covered.
TEST(SettingsRoundTripTest, settings_config_settings_is_a_fixed_point)
{
    const auto expectSameSettings = [](const Ime::Settings &actual, const Ime::Settings &expected) {
        EXPECT_EQ(actual.shortcut, expected.shortcut);
        EXPECT_EQ(actual.enableMod, expected.enableMod);
        EXPECT_EQ(actual.enableTsf, expected.enableTsf);
        EXPECT_EQ(actual.fixInconsistentTextEntryCount, expected.fixInconsistentTextEntryCount);
        EXPECT_EQ(actual.autoToggleKeyboard, expected.autoToggleKeyboard);
        EXPECT_EQ(actual.switchEnglishLayoutOnDisable, expected.switchEnglishLayoutOnDisable);
        EXPECT_EQ(actual.forceDpiAwareness, expected.forceDpiAwareness);

        EXPECT_EQ(actual.logging.level, expected.logging.level);
        EXPECT_EQ(actual.logging.flushLevel, expected.logging.flushLevel);

        EXPECT_EQ(actual.resources.translationDir, expected.resources.translationDir);
        EXPECT_EQ(actual.resources.fontPathList, expected.resources.fontPathList);

        EXPECT_EQ(actual.appearance.schemeConfig.variant, expected.appearance.schemeConfig.variant);
        EXPECT_EQ(actual.appearance.schemeConfig.sourceColor, expected.appearance.schemeConfig.sourceColor);
        EXPECT_EQ(actual.appearance.schemeConfig.contrastLevel, expected.appearance.schemeConfig.contrastLevel);
        EXPECT_EQ(actual.appearance.schemeConfig.darkMode, expected.appearance.schemeConfig.darkMode);
        EXPECT_EQ(actual.appearance.language, expected.appearance.language);
        EXPECT_EQ(actual.appearance.zoom, expected.appearance.zoom);
        EXPECT_EQ(actual.appearance.errorDisplayDuration, expected.appearance.errorDisplayDuration);
        EXPECT_EQ(actual.appearance.verticalCandidateList, expected.appearance.verticalCandidateList);
        EXPECT_EQ(actual.appearance.autoToggleLanguageBar, expected.appearance.autoToggleLanguageBar);

        EXPECT_EQ(actual.input.posUpdatePolicy, expected.input.posUpdatePolicy);
        EXPECT_EQ(actual.input.enableUnicodePaste, expected.input.enableUnicodePaste);
        EXPECT_EQ(actual.input.keepImeOpen, expected.input.keepImeOpen);
        EXPECT_EQ(actual.input.meridianSupport, expected.input.meridianSupport);
        EXPECT_EQ(actual.input.prismaAvoidance, expected.input.prismaAvoidance);
        EXPECT_EQ(actual.input.skseMenuFrameworkSupport, expected.input.skseMenuFrameworkSupport);
        EXPECT_EQ(actual.input.imguiSurfaceInput, expected.input.imguiSurfaceInput);
        EXPECT_EQ(actual.input.pauseGameWhileSettingsOpen, expected.input.pauseGameWhileSettingsOpen);
        EXPECT_EQ(actual.input.lastNativeConversion, expected.input.lastNativeConversion);
    };

    // Randomized configuration: the wide input space (random strings, out-of-
    // range numbers) exercises every normalization arm before the fixed point.
    std::uint32_t seed = 0;
    const auto    randomConfig = ImeTest::GetRandomConfiguation(&seed);
    SCOPED_TRACE(std::format("RandomUtils seed: {}", seed));
    const Ime::Settings s1 = Ime::ConvertConfigurationToSettings(randomConfig);
    const Ime::Settings s2 = Ime::ConvertConfigurationToSettings(Ime::ConvertSettingsToConfiguration(s1));
    expectSameSettings(s2, s1);

    // The compiled-in defaults must also be a fixed point (shortcut F2, zoom -1,
    // theme default): a save/load cycle on a fresh install changes nothing.
    const Ime::Settings defaults = Ime::GetDefaultSettings();
    expectSameSettings(Ime::ConvertConfigurationToSettings(Ime::ConvertSettingsToConfiguration(defaults)), defaults);
}

TEST(ConfigurationToSettingsTest, should_convert_default_configuration_to_default_settings)
{
    Ime::Configuration configuration = Ime::GetDefaultConfiguration();

    Ime::Settings defaultSettings = Ime::GetDefaultSettings();
    auto          settings        = Ime::ConvertConfigurationToSettings(configuration);

    EXPECT_EQ(settings.enableMod, defaultSettings.enableMod);
    EXPECT_EQ(settings.enableTsf, defaultSettings.enableTsf);
    EXPECT_EQ(settings.fixInconsistentTextEntryCount, defaultSettings.fixInconsistentTextEntryCount);
    EXPECT_EQ(settings.autoToggleKeyboard, defaultSettings.autoToggleKeyboard);
    // Disabling the IME must not move the session's language profile by default:
    // the English-profile route is what leaked ENG onto the desktop after the
    // game exited.
    EXPECT_EQ(settings.switchEnglishLayoutOnDisable, defaultSettings.switchEnglishLayoutOnDisable);
    EXPECT_EQ(settings.switchEnglishLayoutOnDisable, false);
    // The DPI-awareness fix must ship enabled: DPI-unaware rendering is what
    // blurred the settings UI on scaled desktops.
    EXPECT_EQ(settings.forceDpiAwareness, defaultSettings.forceDpiAwareness);
    EXPECT_EQ(settings.forceDpiAwareness, true);
    EXPECT_EQ(settings.shortcut, defaultSettings.shortcut);

    EXPECT_EQ(settings.logging.level, defaultSettings.logging.level);
    EXPECT_EQ(settings.logging.flushLevel, defaultSettings.logging.flushLevel);

    EXPECT_EQ(settings.resources.translationDir, defaultSettings.resources.translationDir);
    EXPECT_EQ(settings.resources.fontPathList, defaultSettings.resources.fontPathList);

    EXPECT_EQ(settings.appearance.schemeConfig.sourceColor, defaultSettings.appearance.schemeConfig.sourceColor);
    EXPECT_EQ(settings.appearance.schemeConfig.contrastLevel, defaultSettings.appearance.schemeConfig.contrastLevel);
    EXPECT_EQ(settings.appearance.schemeConfig.darkMode, defaultSettings.appearance.schemeConfig.darkMode);
    EXPECT_EQ(settings.appearance.language, defaultSettings.appearance.language);
    EXPECT_EQ(settings.appearance.zoom, defaultSettings.appearance.zoom);
    EXPECT_EQ(settings.appearance.errorDisplayDuration, defaultSettings.appearance.errorDisplayDuration);
    EXPECT_EQ(settings.appearance.verticalCandidateList, defaultSettings.appearance.verticalCandidateList);
    EXPECT_EQ(settings.appearance.autoToggleLanguageBar, defaultSettings.appearance.autoToggleLanguageBar);

    EXPECT_EQ(settings.input.posUpdatePolicy, defaultSettings.input.posUpdatePolicy);
    EXPECT_EQ(settings.input.enableUnicodePaste, defaultSettings.input.enableUnicodePaste);
    EXPECT_EQ(settings.input.keepImeOpen, defaultSettings.input.keepImeOpen);
    // New UI-framework integration switches default to ON in all three places
    // (GetDefaultConfiguration, GetDefaultSettings, shipped SimpleIME.toml).
    EXPECT_EQ(settings.input.meridianSupport, defaultSettings.input.meridianSupport);
    EXPECT_EQ(settings.input.meridianSupport, true);
    EXPECT_EQ(settings.input.prismaAvoidance, defaultSettings.input.prismaAvoidance);
    EXPECT_EQ(settings.input.prismaAvoidance, true);
    // The runtime cache must default to 中: with no persisted observation
    // (fresh install) that is the same guess the activation path makes.
    EXPECT_EQ(settings.input.lastNativeConversion, defaultSettings.input.lastNativeConversion);
    EXPECT_EQ(settings.input.lastNativeConversion, true);
}

TEST(ConfigurationToSettingsTest, should_convert_theme_style_string_to_variant)
{
    Ime::Configuration configuration = Ime::GetDefaultConfiguration();

    configuration.appearance.themeStyle = "material";
    auto settings                       = Ime::ConvertConfigurationToSettings(configuration);
    EXPECT_EQ(settings.appearance.schemeConfig.variant, ImGuiEx::M3::ThemeVariant::TonalSpot) << "material is the dynamic TonalSpot scheme";

    configuration.appearance.themeStyle = "default";
    settings                            = Ime::ConvertConfigurationToSettings(configuration);
    EXPECT_EQ(settings.appearance.schemeConfig.variant, ImGuiEx::M3::ThemeVariant::Default) << "default is the curated static scheme";

    configuration.appearance.themeStyle = "boutique";
    settings                            = Ime::ConvertConfigurationToSettings(configuration);
    EXPECT_EQ(settings.appearance.schemeConfig.variant, ImGuiEx::M3::ThemeVariant::Default) << "the pre-rename \"boutique\" spelling should keep loading";

    configuration.appearance.themeStyle = "unknown style";
    settings                            = Ime::ConvertConfigurationToSettings(configuration);
    EXPECT_EQ(settings.appearance.schemeConfig.variant, Ime::GetDefaultSettings().appearance.schemeConfig.variant) << "unknown values keep the compiled-in default";

    // The default variant ignores the configured source color and pins the
    // fixed theme accent instead.
    configuration.appearance.themeStyle       = "default";
    configuration.appearance.themeSourceColor = 0xFF0000FF;
    settings                                  = Ime::ConvertConfigurationToSettings(configuration);
    EXPECT_EQ(settings.appearance.schemeConfig.sourceColor,
              ImGuiEx::M3::GetDefaultSchemeConfig(configuration.appearance.themeDarkMode).sourceColor);
}

TEST(SettingsToConfigurationTest, should_write_theme_style_string_from_variant)
{
    Ime::Settings settings = Ime::GetDefaultSettings();
    settings.appearance.schemeConfig.variant = ImGuiEx::M3::ThemeVariant::Default;
    auto config                              = Ime::ConvertSettingsToConfiguration(settings);
    EXPECT_STREQ(config.appearance.themeStyle.c_str(), "default");

    settings.appearance.schemeConfig.variant = ImGuiEx::M3::ThemeVariant::TonalSpot;
    config                                   = Ime::ConvertSettingsToConfiguration(settings);
    EXPECT_STREQ(config.appearance.themeStyle.c_str(), "material");
}

TEST(SettingsToConfigurationTest, should_convert_shortcut_ImGuiKeyChord_to_string)
{
    Ime::Settings settings{};

    settings.shortcut = ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_F1;
    auto config       = Ime::ConvertSettingsToConfiguration(settings);
    EXPECT_STREQ(config.shortcut.c_str(), "ctrl+alt+f1") << "should convert ImGuiKeyChord to string with modifier and key, and all in lower case";

    settings.shortcut = ImGuiMod_Ctrl | ImGuiMod_Super | ImGuiMod_Alt | ImGuiKey_F1;
    config            = Ime::ConvertSettingsToConfiguration(settings);
    EXPECT_STREQ(config.shortcut.c_str(), "ctrl+alt+super+f1") << "should support all modifiers.";

    settings.shortcut = ImGuiMod_Ctrl | ImGuiMod_Alt;
    config            = Ime::ConvertSettingsToConfiguration(settings);
    EXPECT_STREQ(config.shortcut.c_str(), "f2") << "should unsupport if the shortcut does not contain any named key, and fallback to default value";

    settings.shortcut = ImGuiKey_A;
    config            = Ime::ConvertSettingsToConfiguration(settings);
    EXPECT_STREQ(config.shortcut.c_str(), "a") << "should convert normal key without modifier to string, and keep the case";
}

TEST(SettingsToConfigurationTest, should_apply_rgb_mask_to_source_color)
{
    Ime::Settings settings{};
    settings.appearance.schemeConfig.sourceColor = 0x12345678; // the alpha channel should be ignored

    auto config = Ime::ConvertSettingsToConfiguration(settings);
    EXPECT_EQ(config.appearance.themeSourceColor, 0x00345678) << "should apply RGB mask to source color when convert settings to configuration";
}

// The shipped contrib/config/SimpleIME.toml (copied next to the test binary by
// test.cmake) is what a fresh install starts from. It pins documented values
// for fields the compiled default leaves invalid; edited independently of the
// code, new users would silently run defaults nobody compared. The behavioral
// invariant: loaded through the real loader and converter it must produce
// exactly GetDefaultSettings().
TEST(ShippedConfigurationTest, shipped_toml_converts_to_default_settings)
{
    const Ime::ConfigSerializer::ConfigStatus status =
        Ime::ConfigSerializer::ValidateConfiguration("SimpleIME.toml");
    ASSERT_EQ(status.kind, Ime::ConfigSerializer::ConfigStatusKind::Ok) << status.detail;
    ASSERT_TRUE(status.ignoredKeys.empty()) << "shipped config carries mistyped keys: "
                                            << [&] {
                                                   std::string keys;
                                                   for (const auto &key : status.ignoredKeys)
                                                   {
                                                       keys += key + " ";
                                                   }
                                                   return keys;
                                               }();

    const Ime::Settings shipped  = Ime::ConvertConfigurationToSettings(Ime::ConfigSerializer::LoadConfiguration("SimpleIME.toml"));
    const Ime::Settings expected = Ime::GetDefaultSettings();

    EXPECT_EQ(shipped.shortcut, expected.shortcut);
    EXPECT_EQ(shipped.enableMod, expected.enableMod);
    EXPECT_EQ(shipped.enableTsf, expected.enableTsf);
    EXPECT_EQ(shipped.fixInconsistentTextEntryCount, expected.fixInconsistentTextEntryCount);
    EXPECT_EQ(shipped.autoToggleKeyboard, expected.autoToggleKeyboard);
    EXPECT_EQ(shipped.switchEnglishLayoutOnDisable, expected.switchEnglishLayoutOnDisable);
    EXPECT_EQ(shipped.forceDpiAwareness, expected.forceDpiAwareness);

    EXPECT_EQ(shipped.logging.level, expected.logging.level);
    EXPECT_EQ(shipped.logging.flushLevel, expected.logging.flushLevel);

    EXPECT_EQ(shipped.resources.translationDir, expected.resources.translationDir);
    EXPECT_EQ(shipped.resources.fontPathList, expected.resources.fontPathList);

    EXPECT_EQ(shipped.appearance.schemeConfig.variant, expected.appearance.schemeConfig.variant);
    EXPECT_EQ(shipped.appearance.schemeConfig.sourceColor, expected.appearance.schemeConfig.sourceColor);
    EXPECT_EQ(shipped.appearance.schemeConfig.contrastLevel, expected.appearance.schemeConfig.contrastLevel);
    EXPECT_EQ(shipped.appearance.schemeConfig.darkMode, expected.appearance.schemeConfig.darkMode);
    EXPECT_EQ(shipped.appearance.language, expected.appearance.language);
    EXPECT_EQ(shipped.appearance.zoom, expected.appearance.zoom);
    EXPECT_EQ(shipped.appearance.errorDisplayDuration, expected.appearance.errorDisplayDuration);
    EXPECT_EQ(shipped.appearance.verticalCandidateList, expected.appearance.verticalCandidateList);
    EXPECT_EQ(shipped.appearance.autoToggleLanguageBar, expected.appearance.autoToggleLanguageBar);

    EXPECT_EQ(shipped.input.posUpdatePolicy, expected.input.posUpdatePolicy);
    EXPECT_EQ(shipped.input.enableUnicodePaste, expected.input.enableUnicodePaste);
    EXPECT_EQ(shipped.input.keepImeOpen, expected.input.keepImeOpen);
    EXPECT_EQ(shipped.input.meridianSupport, expected.input.meridianSupport);
    EXPECT_EQ(shipped.input.prismaAvoidance, expected.input.prismaAvoidance);
    EXPECT_EQ(shipped.input.skseMenuFrameworkSupport, expected.input.skseMenuFrameworkSupport);
    EXPECT_EQ(shipped.input.imguiSurfaceInput, expected.input.imguiSurfaceInput);
    EXPECT_EQ(shipped.input.pauseGameWhileSettingsOpen, expected.input.pauseGameWhileSettingsOpen);
    EXPECT_EQ(shipped.input.lastNativeConversion, expected.input.lastNativeConversion);
}
