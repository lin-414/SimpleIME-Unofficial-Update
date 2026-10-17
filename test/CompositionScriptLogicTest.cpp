//
// Headless tests for the composition-script mode inference (CompositionScriptLogic.h):
// what a live composition's text proves about the TIP's conversion mode, and the
// Japanese-only gate that decides whether that evidence may be applied at all.
//
// The regression this guards: Microsoft IME composes 全角英数 too. OnStartComposition
// used to assert NATIVE unconditionally, so a 全角英数 composition turned the display
// into NATIVE|FULLSHAPE — 「あ」 on the language bar while the IME typed 「ａ」 — and no
// path ever took it back.
//
#include "utils/CompositionScriptLogic.h"

#include <gtest/gtest.h>

#include <string>

using namespace Ime::CompositionScript;

namespace
{
constexpr wchar_t HIRAGANA_A = L'\u3042';   // あ
constexpr wchar_t HIRAGANA_TE = L'\u3066';  // て
constexpr wchar_t KATAKANA_KA = L'\u30AB';  // カ
constexpr wchar_t KANJI_KAN = L'\u6F22';    // 漢
constexpr wchar_t FULLWIDTH_A = L'\uFF41';  // ａ
constexpr wchar_t FULLWIDTH_T = L'\uFF54';  // Ｔ
constexpr wchar_t HALF_A = L'a';
constexpr wchar_t HALF_T = L'T';
constexpr wchar_t FULLWIDTH_COMMA = L'\u3001'; // 、
} // namespace

TEST(CompositionScriptClassify, kana_and_han_are_native)
{
    EXPECT_EQ(Classify(std::wstring(1, HIRAGANA_A)), Script::KanaOrHan);
    EXPECT_EQ(Classify(std::wstring(1, KATAKANA_KA)), Script::KanaOrHan);
    EXPECT_EQ(Classify(std::wstring(1, KANJI_KAN)), Script::KanaOrHan);
    // A multi-character preedit with any kana is still decisive.
    EXPECT_EQ(Classify(std::wstring{HIRAGANA_TE, HIRAGANA_A}), Script::KanaOrHan);
}

TEST(CompositionScriptClassify, full_width_ascii_is_full_shape)
{
    EXPECT_EQ(Classify(std::wstring(1, FULLWIDTH_A)), Script::FullWidthAscii);
    EXPECT_EQ(Classify(std::wstring(1, FULLWIDTH_T)), Script::FullWidthAscii);
    // The exact preedit Microsoft IME produced in the report: 'ａａａａ'.
    EXPECT_EQ(Classify(std::wstring(4, FULLWIDTH_A)), Script::FullWidthAscii);
}

TEST(CompositionScriptClassify, half_width_ascii_is_alphanumeric)
{
    EXPECT_EQ(Classify(std::wstring(1, HALF_A)), Script::HalfWidthAscii);
    EXPECT_EQ(Classify(std::wstring{HALF_T, HALF_A}), Script::HalfWidthAscii);
}

TEST(CompositionScriptClassify, kana_wins_over_ascii)
{
    // A Japanese IME keeps romanised ASCII in the preedit before conversion; the
    // moment a kana appears the mode is native regardless of the ASCII around it.
    EXPECT_EQ(Classify(std::wstring{HALF_A, HIRAGANA_A}), Script::KanaOrHan);
    EXPECT_EQ(Classify(std::wstring{FULLWIDTH_A, HIRAGANA_A}), Script::KanaOrHan);
}

TEST(CompositionScriptClassify, full_width_wins_over_half_width)
{
    EXPECT_EQ(Classify(std::wstring{HALF_A, FULLWIDTH_A}), Script::FullWidthAscii);
    EXPECT_EQ(Classify(std::wstring{FULLWIDTH_A, HALF_A}), Script::FullWidthAscii);
}

TEST(CompositionScriptClassify, empty_and_neutral_content_is_undecided)
{
    EXPECT_EQ(Classify(L""), Script::Other);
    EXPECT_EQ(Classify(std::wstring(1, L' ')), Script::Other); // the conversion key
    EXPECT_EQ(Classify(std::wstring(1, FULLWIDTH_COMMA)), Script::Other);
    // A lone surrogate half (emoji preedit) proves nothing either. Spelled as a
    // cast because \uD83D is not a valid universal character name.
    EXPECT_EQ(Classify(std::wstring(1, static_cast<wchar_t>(0xD83D))), Script::Other);
}

TEST(CompositionScriptEvidence, kana_proves_native_but_not_full_shape)
{
    const auto evidence = EvidenceFor(Script::KanaOrHan);
    EXPECT_TRUE(evidence.decisive);
    EXPECT_TRUE(evidence.native);
    // Half-width katakana is NATIVE without FULLSHAPE: the existing bit must
    // survive instead of being forced.
    EXPECT_FALSE(evidence.fullShapeKnown);
}

TEST(CompositionScriptEvidence, full_width_ascii_proves_full_shape_alphanumeric)
{
    const auto evidence = EvidenceFor(Script::FullWidthAscii);
    EXPECT_TRUE(evidence.decisive);
    EXPECT_FALSE(evidence.native);
    EXPECT_TRUE(evidence.fullShapeKnown);
    EXPECT_TRUE(evidence.fullShape);
}

TEST(CompositionScriptEvidence, half_width_ascii_proves_plain_alphanumeric)
{
    const auto evidence = EvidenceFor(Script::HalfWidthAscii);
    EXPECT_TRUE(evidence.decisive);
    EXPECT_FALSE(evidence.native);
    EXPECT_TRUE(evidence.fullShapeKnown);
    EXPECT_FALSE(evidence.fullShape);
}

TEST(CompositionScriptEvidence, undecided_content_changes_nothing)
{
    const auto evidence = EvidenceFor(Script::Other);
    EXPECT_FALSE(evidence.decisive);
    EXPECT_FALSE(evidence.native);
    EXPECT_FALSE(evidence.fullShapeKnown);
}

TEST(CompositionScriptEvidence, the_reported_regression_becomes_full_width_alphanumeric)
{
    // State after the old OnStartComposition asserted NATIVE on a 全角英数
    // composition: NATIVE|FULLSHAPE = 0x9. The committed text must retract NATIVE
    // and keep FULLSHAPE, i.e. exactly the 0x8 the IME published.
    const auto evidence = EvidenceFor(Classify(std::wstring(4, FULLWIDTH_A)));
    EXPECT_TRUE(evidence.decisive);
    EXPECT_FALSE(evidence.native);
    EXPECT_TRUE(evidence.fullShapeKnown);
    EXPECT_TRUE(evidence.fullShape);
}

TEST(CompositionScriptGate, only_japanese_language_may_infer_from_content)
{
    EXPECT_TRUE(ShouldInferFromComposition(0x11));  // LANG_JAPANESE
    EXPECT_FALSE(ShouldInferFromComposition(0x04)); // LANG_CHINESE — pinyin preedit is ASCII
    EXPECT_FALSE(ShouldInferFromComposition(0x09)); // LANG_ENGLISH
    EXPECT_FALSE(ShouldInferFromComposition(0x12)); // LANG_KOREAN
    EXPECT_FALSE(ShouldInferFromComposition(0x00)); // unresolved
}
