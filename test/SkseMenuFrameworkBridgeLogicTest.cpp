//
// Headless tests for the SKSE Menu Framework bridge's pure decision logic
// (extracted into SkseMenuFrameworkBridgeLogic.h so no SKSE/Windows machinery
// is needed here).
//
#include "hooks/SkseMenuFrameworkBridgeLogic.h"

#include <gtest/gtest.h>

using namespace Hooks::SkseMenuFrameworkBridgeLogic;

TEST(SkseMenuFrameworkPrintableAscii, covers_exactly_the_typing_range)
{
    EXPECT_FALSE(IsPrintableAscii(0x00));
    EXPECT_FALSE(IsPrintableAscii(0x09)); // Tab
    EXPECT_FALSE(IsPrintableAscii(0x0D)); // Enter
    EXPECT_FALSE(IsPrintableAscii(0x1F));
    EXPECT_TRUE(IsPrintableAscii(0x20));  // Space
    EXPECT_TRUE(IsPrintableAscii(0x30));  // '0'
    EXPECT_TRUE(IsPrintableAscii(0x41));  // 'A'
    EXPECT_TRUE(IsPrintableAscii(0x7E));  // '~'
    EXPECT_FALSE(IsPrintableAscii(0x7F));
    EXPECT_FALSE(IsPrintableAscii(0xB7)); // middle dot (never neutralized)
    EXPECT_FALSE(IsPrintableAscii(0x4E2D)); // 中
}

TEST(SkseMenuFrameworkValidUnicodeScalar, rejects_ascii_and_surrogates)
{
    EXPECT_FALSE(IsValidUnicodeScalar(0x0000));
    EXPECT_FALSE(IsValidUnicodeScalar(0x41));   // ASCII is delivered by the raw path
    EXPECT_FALSE(IsValidUnicodeScalar(0x7F));
    EXPECT_TRUE(IsValidUnicodeScalar(0x80));
    EXPECT_TRUE(IsValidUnicodeScalar(0x4E2D));  // 中
    EXPECT_TRUE(IsValidUnicodeScalar(0xFF0C));  // ，full-width comma
    EXPECT_FALSE(IsValidUnicodeScalar(0xD800)); // high surrogate
    EXPECT_FALSE(IsValidUnicodeScalar(0xDC00)); // low surrogate
    EXPECT_FALSE(IsValidUnicodeScalar(0xDFFF));
    EXPECT_TRUE(IsValidUnicodeScalar(0x10000)); // astral plane
    EXPECT_TRUE(IsValidUnicodeScalar(0x1F600)); // 😀
    EXPECT_FALSE(IsValidUnicodeScalar(0x110000));
}

TEST(SkseMenuFrameworkAsciiFilter, never_interferes_outside_a_session)
{
    // Even mid-composition with a Chinese TIP, no session means no filtering.
    EXPECT_FALSE(ShouldNeutralizeAscii(false, true, false, true, true));
    EXPECT_FALSE(ShouldNeutralizeAscii(false, false, false, true, true));
    EXPECT_FALSE(ShouldNeutralizeAscii(false, false, false, false, false));
}

TEST(SkseMenuFrameworkAsciiFilter, neutralizes_composition_input)
{
    // Active composition: everything printable belongs to the IME.
    EXPECT_TRUE(ShouldNeutralizeAscii(true, true, false, false, false));
    EXPECT_TRUE(ShouldNeutralizeAscii(true, true, false, true, true));
    // Chinese/Japanese mode (keyboard open + NATIVE), even between
    // compositions: the engine's DirectInput-derived CharEvents are echoes.
    EXPECT_TRUE(ShouldNeutralizeAscii(true, false, false, true, true));
    // Katakana/full-shape variants are still NATIVE composition.
    EXPECT_TRUE(ShouldNeutralizeAscii(true, false, false, true, true));
}

TEST(SkseMenuFrameworkAsciiFilter, passes_deliberate_english_typing)
{
    // IME disabled (Steam overlay, mod off, English keyboard user): raw keys
    // are the only input the field gets.
    EXPECT_FALSE(ShouldNeutralizeAscii(true, false, true, false, false));
    EXPECT_FALSE(ShouldNeutralizeAscii(true, true, true, true, true)); // disabled wins over composing
    // Keyboard closed = the Shift CN/EN toggle put the IME in English mode.
    EXPECT_FALSE(ShouldNeutralizeAscii(true, false, false, false, true));
    EXPECT_FALSE(ShouldNeutralizeAscii(true, false, false, false, false));
    // Keyboard open but ALPHANUMERIC conversion: half-width direct input.
    EXPECT_FALSE(ShouldNeutralizeAscii(true, false, false, true, false));
}

TEST(SkseMenuFrameworkAsciiFilter, fresh_enable_transition_neutralizes_everything)
{
    // The session began while the IME was still disabled; within the grace
    // window the mode flags are stale, so every printable echo is neutralized
    // regardless of state (the observed "d但是" first-letter leak).
    EXPECT_TRUE(ShouldNeutralizeAscii(true, false, true, false, false, true));
    EXPECT_TRUE(ShouldNeutralizeAscii(true, false, false, false, false, true));
    EXPECT_TRUE(ShouldNeutralizeAscii(true, false, false, true, false, true));
    EXPECT_TRUE(ShouldNeutralizeAscii(true, true, true, true, true, true));
}

TEST(SkseMenuFrameworkAsciiFilter, active_tip_with_open_keyboard_neutralizes_despite_stale_native)
{
    // The conversion compartment is read once at TIP activation and can still
    // read ALPHANUMERIC while the TIP finishes initializing — the window the
    // first pinyin letter races through. A real TIP with the keyboard open is
    // in IME input mode regardless of the (possibly stale) NATIVE flag.
    EXPECT_TRUE(ShouldNeutralizeAscii(true, false, false, true, false, false, true));
    EXPECT_TRUE(ShouldNeutralizeAscii(true, false, false, true, true, false, true));
    // English mode under the same TIP closes the keyboard — passthrough wins.
    EXPECT_FALSE(ShouldNeutralizeAscii(true, false, false, false, false, false, true));
    // No TIP (English keyboard profile): unchanged behaviour.
    EXPECT_FALSE(ShouldNeutralizeAscii(true, false, false, true, false, false, false));
}
