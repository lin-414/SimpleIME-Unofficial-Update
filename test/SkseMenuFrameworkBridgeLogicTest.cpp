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

TEST(SkseMenuFrameworkInjection, printable_ascii_rides_the_suppressed_echo_only)
{
    // The IME owns the keyboard and its echoes are zeroed: the character has to
    // come from the injection, or it is lost. Microsoft Pinyin keeps the keyboard
    // open in EN mode, so English typing, the space and - / + all take this path.
    EXPECT_TRUE(ShouldInjectCodepoint(0x41, true)); // 'A'
    EXPECT_TRUE(ShouldInjectCodepoint(0x20, true)); // space
    EXPECT_TRUE(ShouldInjectCodepoint(0x2D, true)); // '-'
    EXPECT_TRUE(ShouldInjectCodepoint(0x2B, true)); // '+'
    EXPECT_TRUE(ShouldInjectCodepoint(0x7E, true)); // '~'
    // The echo flows (IME disabled, keyboard closed): the engine's own CharEvent
    // delivers it, so injecting on top would type the character twice.
    EXPECT_FALSE(ShouldInjectCodepoint(0x41, false));
    EXPECT_FALSE(ShouldInjectCodepoint(0x20, false));
    // Control codes are not printable ASCII and the engine delivers them raw.
    EXPECT_FALSE(ShouldInjectCodepoint(0x08, true)); // backspace
    EXPECT_FALSE(ShouldInjectCodepoint(0x0D, true)); // enter
    EXPECT_FALSE(ShouldInjectCodepoint(0x7F, true)); // DEL
}

TEST(SkseMenuFrameworkInjection, non_ascii_needs_a_valid_scalar)
{
    // Committed composition text only ever comes from us — the echo flag does
    // not apply to it.
    EXPECT_TRUE(ShouldInjectCodepoint(0x4E2D, false));   // 中
    EXPECT_TRUE(ShouldInjectCodepoint(0xFF0C, true));    // ，full-width comma
    EXPECT_TRUE(ShouldInjectCodepoint(0x1F600, false));  // 😀
    EXPECT_FALSE(ShouldInjectCodepoint(0xD800, true));   // high surrogate
    EXPECT_FALSE(ShouldInjectCodepoint(0xDFFF, false));  // low surrogate
    EXPECT_FALSE(ShouldInjectCodepoint(0x110000, true)); // out of range
    EXPECT_FALSE(ShouldInjectCodepoint(0x0000, true));
}

TEST(SkseMenuFrameworkSessionStart, needs_the_host_to_want_text)
{
    EXPECT_FALSE(ShouldStartSession(false, true, 0x1234));
    EXPECT_FALSE(ShouldStartSession(false, true, 0));
    EXPECT_FALSE(ShouldStartSession(false, false, 0x1234));
    EXPECT_TRUE(ShouldStartSession(true, true, 0x1234));
}

TEST(SkseMenuFrameworkSessionStart, refuses_a_want_without_an_active_item)
{
    // The 2026-10-21 shape: right after an Enter commit the host still reports
    // WantTextInput for a frame or two while no item holds focus. Starting a
    // session there only grabs the focus and flashes the candidate bar.
    EXPECT_FALSE(ShouldStartSession(true, true, 0));
}

TEST(SkseMenuFrameworkSessionStart, a_want_of_one_frame_still_opens_a_session)
{
    // The 2026-10-22 field shape is want-on for exactly one frame. Refusing it
    // left the user unable to type at all, so the frame is enough.
    EXPECT_TRUE(ShouldHoldSession(true, -1));
    EXPECT_TRUE(ShouldHoldSession(true, 5000));
}

TEST(SkseMenuFrameworkSessionStart, a_session_outlives_the_want_for_one_hangover)
{
    // Gaps shorter than the hangover keep the lease and the IME alive; that is
    // what gives a numeric field time to actually open.
    EXPECT_TRUE(ShouldHoldSession(false, 0));
    EXPECT_TRUE(ShouldHoldSession(false, 100));
    EXPECT_TRUE(ShouldHoldSession(false, 249));
    EXPECT_FALSE(ShouldHoldSession(false, 250));
    EXPECT_FALSE(ShouldHoldSession(false, 1000));
    // Never wanted: no session, and no hangover to invent one from.
    EXPECT_FALSE(ShouldHoldSession(false, -1));
    // The window is a parameter, not a constant baked into the call site.
    EXPECT_FALSE(ShouldHoldSession(false, 80, 50));
    EXPECT_TRUE(ShouldHoldSession(false, 80, 200));
}

TEST(SkseMenuFrameworkSessionStart, host_without_get_active_id_keeps_the_old_behaviour)
{
    // Nothing to ask: the flag alone decides, exactly as before the gate.
    EXPECT_TRUE(ShouldStartSession(true, false, 0));
    EXPECT_TRUE(ShouldStartSession(true, false, 0xDEADBEEF));
    // Still no session when the host does not even want text.
    EXPECT_FALSE(ShouldStartSession(false, false, 0));
}
