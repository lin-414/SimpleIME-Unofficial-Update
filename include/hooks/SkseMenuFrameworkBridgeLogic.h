#pragma once

#include <cstdint>

// Pure, SKSE-free decision logic of the SKSE Menu Framework bridge, extracted
// so it can be unit-tested headless (the .cpp keeps all the Windows/ImGui-ABI
// interaction). Same split the Meridian bridge uses with MeridianBridgeLogic.h.
namespace Hooks::SkseMenuFrameworkBridgeLogic
{
/// Printable ASCII is exactly the codepoint range the game's CharEvents carry
/// for plain typing. Everything below 0x20 (Tab/Enter/Backspace as control
/// codes) and above 0x7E must keep flowing — the IME never claims them.
inline bool IsPrintableAscii(const std::uint32_t codepoint)
{
    return codepoint >= 0x20U && codepoint <= 0x7EU;
}

/// Non-ASCII codepoints eligible for ImGui injection: any Unicode scalar value
/// (surrogates are recombined upstream in FlushPending). Committed composition
/// text never reaches the field through the raw events, so it always needs
/// injecting; printable ASCII goes through ShouldInjectCodepoint instead.
inline bool IsValidUnicodeScalar(const std::uint32_t codepoint)
{
    if (codepoint < 0x80U || codepoint > 0x10FFFFU)
    {
        return false;
    }
    return !(codepoint >= 0xD800U && codepoint <= 0xDFFFU);
}

/// Whether one codepoint may be injected into the host's ImGui.
///
/// Printable ASCII needs a second signal: the engine reads the physical keyboard
/// regardless of the Win32 focus, so every printable key also produces a
/// game-side CharEvent. While the IME owns the keyboard that event is a pure echo
/// and the neutralizer zeroes it (ShouldNeutralizeAscii) — the character then has
/// to come from us. Where the echo flows instead (IME disabled, keyboard closed)
/// the engine's own event IS the delivery, and injecting on top would double-type.
/// `rawAsciiEchoSuppressed` is that same neutralizer decision, read from the same
/// live state, so exactly one of the two paths carries each character.
///
/// Characters the TIP passed through — English typing while the keyboard stays
/// open (Microsoft Pinyin does not close it in EN mode), the space, symbols like
/// - and + — are the ones this exists for: the echo is suppressed and the raw
/// path therefore delivers nothing, so rejecting them here dropped them twice
/// (observed 2026-10-13: "cannot type English or symbols into MCM Bridge's field").
inline bool ShouldInjectCodepoint(const std::uint32_t codepoint, const bool rawAsciiEchoSuppressed)
{
    if (IsPrintableAscii(codepoint))
    {
        return rawAsciiEchoSuppressed;
    }
    return IsValidUnicodeScalar(codepoint);
}

/// Decide whether one input batch's printable ASCII is IME composition input
/// (neutralize) or the user's deliberate English typing (pass through).
/// All inputs come from Ime::Core::State and the caller's session flag, read
/// live per batch — no cached mode, no toggle-grace bookkeeping.
///
/// - Not in a session (no field the echoes could land in): never interfere
///   with any other UI.
/// - IME disabled (Steam overlay, English keyboard user): raw keys are the
///   only input the field gets — pass. This wins over a stale composing flag
///   (the IMM32 path does not always clear IN_COMPOSING on focus loss; the
///   same precedence ImeMenu::OnKeyEvent applies to Scaleform menus) — unless
///   we are inside the fresh-enable transition window (see below).
/// - An active composition: everything printable belongs to the IME.
/// - Fresh-enable transition (the session began while the IME was still
///   disabled, within its short grace window): the enable is async (~20ms
///   observed) and the conversion-mode flags land even later, so every
///   keystroke here produces an echo the mode predicate cannot classify yet —
///   the observed artifact is a stray leading "d" in front of the committed
///   "但是". Neutralize the window; it is bounded, so a genuinely English
///   user only ever loses characters inside it.
/// - Keyboard closed (the Shift CN/EN toggle): English passthrough — pass.
/// - Keyboard open + NATIVE conversion: Chinese/Japanese composition mode.
///   The composition consumes these keys at the Win32 level; the CharEvents
///   the game still generates from DirectInput are pure echoes — neutralize.
inline bool ShouldNeutralizeAscii(
    const bool sessionActive,
    const bool imeComposing,
    const bool imeDisabled,
    const bool keyboardOpen,
    const bool conversionNative,
    const bool freshEnableTransition = false,
    const bool inputProcessorActive  = false)
{
    if (!sessionActive)
    {
        return false;
    }
    if (imeDisabled && !freshEnableTransition)
    {
        return false;
    }
    if (imeComposing)
    {
        return true;
    }
    if (freshEnableTransition)
    {
        return true;
    }
    if (!keyboardOpen)
    {
        return false;
    }
    // A real TIP (WeChat/Microsoft Pinyin) with the keyboard open is in IME
    // input mode: every printable keystroke goes to the composition at the
    // Win32 level and its game-side CharEvent is a pure echo. Do NOT require
    // conversionNative here — the conversion compartment is read once at TIP
    // activation and can still be ALPHANUMERIC for a few frames while the TIP
    // finishes initializing, which is exactly the window the first pinyin
    // letter races through (the recurring leading "d" of "d但是").
    // English typing under such a TIP goes through the Shift toggle, which
    // closes the keyboard (KEYBOARD_OPEN=false) and returns above.
    return conversionNative || inputProcessorActive;
}

/// Whether the host's WantTextInput means a field is really waiting for text.
///
/// ImGui keeps WantTextInput set for a frame or two around a field's whole
/// lifecycle — including the frame the field deactivates on — so the flag alone
/// can be true while no item holds focus at all. Acting on that residue costs a
/// full IME enable/disable cycle (focus grab, then the release that hands the
/// focus back), which the user sees as the candidate bar flashing for ~30 ms and
/// then nothing happening: in-game 2026-10-21, right after an Enter commit, three
/// sessions of 31/31/32 ms began with the host reporting NO active item
/// (beginActiveId=0x0) while a real field session in the same log always carried
/// one. Only a session that started on a live field id is a field the IME can
/// type into.
///
/// A host that does not export GetActiveId cannot answer the question, so
/// `hostExportsActiveId == false` keeps the flag-only behaviour.
inline bool ShouldStartSession(const bool rawWants, const bool hostExportsActiveId, const std::uint32_t hostActiveId)
{
    if (!rawWants)
    {
        return false;
    }
    if (!hostExportsActiveId)
    {
        return true;
    }
    return hostActiveId != 0U;
}

/// How long a session outlives the host's want.
///
/// In the shapes that open a NUMERIC field the host reports WantTextInput for a
/// single frame: on, then off, while the item stays hovered and the pointer stays
/// valid — in-game 2026-10-22 the ring shows "+0 a=<id> w=0 | +31 a=<id> w=1 |
/// +78 a=0 w=0" with 0 mouse presses and 0 returns in between. Ending the session
/// on that frame releases the text-entry lease and disables the IME one frame
/// after it was enabled, so the field never gets a chance to stay open and the
/// user only sees the candidate bar blink.
///
/// A hangover keeps the session — and the lease, and the IME — alive across those
/// single-frame gaps. It costs nothing when the host is honest: a real field keeps
/// the want set for as long as it is open, and a field that closes for good ends
/// the session one hangover later (then everything settles at once).
inline bool ShouldHoldSession(const bool wantThisFrame, const int msSinceWant, const int hangoverMs = 250)
{
    if (wantThisFrame)
    {
        return true;
    }
    return msSinceWant >= 0 && msSinceWant < hangoverMs;
}
} // namespace Hooks::SkseMenuFrameworkBridgeLogic
