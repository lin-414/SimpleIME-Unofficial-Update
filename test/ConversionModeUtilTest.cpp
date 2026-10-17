//
// Headless tests for the language-bar labels (tsf/ConversionModeUtil.h): which
// glyph the mode accessor returns, and — the reported complaint — that a
// Chinese full-width toggle no longer replaces the 中/英 glyph.
//
#include <windows.h> // before ConversionModeUtil.h: it only pulls <winnt.h>, which needs the SDK preamble.

#include "tsf/ConversionModeUtil.h"

#include <gtest/gtest.h>

namespace
{
using ConversionMode = Ime::Core::State::ConversionMode;

constexpr LANGID LANG_JA = MAKELANGID(LANG_JAPANESE, SUBLANG_JAPANESE_JAPAN);
constexpr LANGID LANG_ZH = MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED);

constexpr ConversionMode::UnderlyingType NATIVE    = 0x1;
constexpr ConversionMode::UnderlyingType KATAKANA  = 0x2;
constexpr ConversionMode::UnderlyingType FULLSHAPE = 0x8;
constexpr ConversionMode::UnderlyingType ROMAN     = 0x10;
constexpr ConversionMode::UnderlyingType SYMBOL    = 0x400;

auto Mode(const ConversionMode::UnderlyingType raw) -> ConversionMode
{
    ConversionMode mode;
    mode.Set(raw);
    return mode;
}

// Universal-character escapes on purpose: the test target is built without
// /utf-8, so a literal 「あ」 here would be a compile error.
constexpr std::string_view HIRAGANA      = "\u3042";             // あ
constexpr std::string_view KATAKANA_FULL = "\u30AB";             // カ
constexpr std::string_view KATAKANA_HALF = "\uFF76";             // ｶ
constexpr std::string_view ALPHA_FULL    = "\uFF21";             // Ａ
constexpr std::string_view ALPHA_HALF    = "\uFF41";             // ａ
constexpr std::string_view ZHONG         = "\u4E2D";             // 中
constexpr std::string_view YING          = "\u82F1";             // 英
constexpr std::string_view QUANJIAO      = "\u5168\u89D2";      // 全角
constexpr std::string_view BANJIAO       = "\u534A\u89D2";      // 半角
} // namespace

TEST(ConversionModeNameJapanese, glyphs_carry_the_width_themselves)
{
    // Microsoft IME reports hiragana as NATIVE alone; Google as NATIVE|ROMAN.
    // ROMAN is cleared before mapping, so both land on the same glyph.
    EXPECT_EQ(Ime::GetConversionModeNameShort(LANG_JA, Mode(NATIVE), true), HIRAGANA);
    EXPECT_EQ(Ime::GetConversionModeNameShort(LANG_JA, Mode(NATIVE | ROMAN), true), HIRAGANA);
    EXPECT_EQ(Ime::GetConversionModeNameShort(LANG_JA, Mode(NATIVE | FULLSHAPE), true), HIRAGANA);
    EXPECT_EQ(Ime::GetConversionModeNameShort(LANG_JA, Mode(KATAKANA | FULLSHAPE), true), KATAKANA_FULL);
    EXPECT_EQ(Ime::GetConversionModeNameShort(LANG_JA, Mode(NATIVE | KATAKANA), true), KATAKANA_HALF);
    // The reported bug's two states: full-width vs half-width alphanumeric.
    EXPECT_EQ(Ime::GetConversionModeNameShort(LANG_JA, Mode(FULLSHAPE), true), ALPHA_FULL);
    EXPECT_EQ(Ime::GetConversionModeNameShort(LANG_JA, Mode(0x0), true), ALPHA_HALF);
}

TEST(ConversionModeNameJapanese, width_is_reported_by_the_glyph_not_a_second_label)
{
    // 「Ａ」 already says full-width; a separate 「全角」 would repeat it.
    EXPECT_TRUE(Ime::GetConversionShapeNameShort(LANG_JA, Mode(FULLSHAPE)).empty());
    EXPECT_TRUE(Ime::GetConversionShapeNameShort(LANG_JA, Mode(NATIVE | FULLSHAPE)).empty());
    EXPECT_TRUE(Ime::GetConversionShapeNameShort(LANG_JA, Mode(0x0)).empty());
}

TEST(ConversionModeNameChinese, full_width_no_longer_replaces_the_language_glyph)
{
    // Before: 0x409 displayed 「全角」 and 0x8 displayed 「英(全角)」 — the 中/英
    // state disappeared exactly while the user was checking it.
    EXPECT_EQ(Ime::GetConversionModeNameShort(LANG_ZH, Mode(NATIVE | SYMBOL), false), ZHONG);
    EXPECT_EQ(Ime::GetConversionModeNameShort(LANG_ZH, Mode(NATIVE | SYMBOL | FULLSHAPE), false), ZHONG);
    EXPECT_EQ(Ime::GetConversionModeNameShort(LANG_ZH, Mode(0x0), false), YING);
    EXPECT_EQ(Ime::GetConversionModeNameShort(LANG_ZH, Mode(FULLSHAPE), false), YING);
    EXPECT_EQ(Ime::GetConversionModeNameShort(LANG_ZH, Mode(SYMBOL), false), YING);
}

TEST(ConversionModeNameChinese, width_gets_its_own_label)
{
    EXPECT_EQ(Ime::GetConversionShapeNameShort(LANG_ZH, Mode(NATIVE | SYMBOL | FULLSHAPE)), QUANJIAO);
    EXPECT_EQ(Ime::GetConversionShapeNameShort(LANG_ZH, Mode(FULLSHAPE)), QUANJIAO);
    // Half-width used to return "" — the label looked like it had vanished
    // instead of switched. Both values must be spelled out.
    EXPECT_EQ(Ime::GetConversionShapeNameShort(LANG_ZH, Mode(NATIVE | SYMBOL)), BANJIAO);
    EXPECT_EQ(Ime::GetConversionShapeNameShort(LANG_ZH, Mode(0x0)), BANJIAO);
}

TEST(ConversionModeNameOther, unknown_language_has_no_labels)
{
    constexpr LANGID LANG_KO = MAKELANGID(LANG_KOREAN, SUBLANG_KOREAN);
    EXPECT_TRUE(Ime::GetConversionModeNameShort(LANG_KO, Mode(NATIVE), true).empty());
    EXPECT_TRUE(Ime::GetConversionShapeNameShort(LANG_KO, Mode(FULLSHAPE)).empty());
}
