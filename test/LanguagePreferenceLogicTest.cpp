//
// Headless tests for the "language = auto" decision logic
// (utils/LanguagePreference.h): OS UI locale -> bundled translation name, and
// the rule that an explicit choice always beats the system language.
//
#include "utils/LanguagePreference.h"

#include <gtest/gtest.h>

#include <array>
#include <string_view>

namespace
{
using Ime::LanguagePreference::FromLocaleName;
using Ime::LanguagePreference::Resolve;
using Ime::LanguagePreference::kAutoLanguage;

struct LocaleCase
{
    std::string_view locale;
    std::string_view expected;
};

// Real Windows locale names: the MUI form carries script and region subtags.
constexpr std::array<LocaleCase, 10> kLocaleCases{
    LocaleCase{"zh", "chinese"},     LocaleCase{"zh-CN", "chinese"},     LocaleCase{"zh-Hans-CN", "chinese"},
    LocaleCase{"zh-SG", "chinese"},  LocaleCase{"ja-JP", "japanese"},    LocaleCase{"ko-KR", "korean"},
    LocaleCase{"ru-RU", "russian"},  LocaleCase{"de-DE", "german"},      LocaleCase{"en-US", "english"},
    LocaleCase{"en-GB", "english"},
};
} // namespace

TEST(LanguagePreferenceFromLocaleName, maps_the_primary_subtag_of_every_bundled_locale)
{
    for (const auto &[locale, expected] : kLocaleCases)
    {
        EXPECT_EQ(FromLocaleName(locale), expected) << "locale: " << locale;
    }
}

TEST(LanguagePreferenceFromLocaleName, is_case_insensitive)
{
    EXPECT_EQ(FromLocaleName("ZH-Hans-CN"), "chinese");
    EXPECT_EQ(FromLocaleName("Ja-jp"), "japanese");
}

TEST(LanguagePreferenceFromLocaleName, returns_empty_for_unbundled_or_empty_locales)
{
    for (const auto locale : {"pt-BR", "fr-FR", "es-ES", "it", ""})
    {
        EXPECT_TRUE(FromLocaleName(locale).empty()) << "locale: " << locale;
    }
}

TEST(LanguagePreferenceResolve, explicit_choice_always_beats_the_system_locale)
{
    EXPECT_EQ(Resolve("english", "zh-CN", "english"), "english");
    EXPECT_EQ(Resolve("german", "ja-JP", "english"), "german");
}

TEST(LanguagePreferenceResolve, auto_maps_the_system_locale_onto_a_bundled_translation)
{
    EXPECT_EQ(Resolve(kAutoLanguage, "ja-JP", "english"), "japanese");
    EXPECT_EQ(Resolve(kAutoLanguage, "zh-Hans-CN", "english"), "chinese");
}

TEST(LanguagePreferenceResolve, auto_falls_back_when_the_system_locale_is_not_bundled)
{
    EXPECT_EQ(Resolve(kAutoLanguage, "pt-BR", "english"), "english");
}

TEST(LanguagePreferenceResolve, auto_falls_back_when_locale_detection_failed)
{
    // GetSystemLocaleName returns "" when both the MUI list and the LCID lookup
    // fail; that must land on the caller's fallback, not on an empty filename.
    EXPECT_EQ(Resolve(kAutoLanguage, "", "english"), "english");
}

TEST(LanguagePreferenceResolve, unknown_explicit_values_pass_through_untouched)
{
    // ConfigSerializerTest round-trips arbitrary language strings, and the
    // loader's own english fallback decides what an unknown name shows.
    EXPECT_EQ(Resolve("klingon", "zh-CN", "english"), "klingon");
}

TEST(LanguagePreferenceResolve, traditional_chinese_shares_the_simplified_file)
{
    // Documented deviation: only one Chinese file ships. Changing this requires
    // adding translate_chinese_traditional.toml and updating this test.
    EXPECT_EQ(Resolve(kAutoLanguage, "zh-TW", "english"), "chinese");
    EXPECT_EQ(Resolve(kAutoLanguage, "zh-Hant-HK", "english"), "chinese");
}

TEST(LanguagePreferenceSentinel, auto_is_the_value_the_shipped_config_stores)
{
    // Pinned because contrib/config/SimpleIME.toml and Settings.h:207 must carry
    // the same literal; renaming the sentinel silently breaks follow-the-system.
    EXPECT_EQ(kAutoLanguage, "auto");
}
