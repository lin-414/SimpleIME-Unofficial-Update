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

/// Codepoints eligible for ImGui injection. ASCII is deliberately rejected:
/// in English mode the game's own CharEvents already deliver it (they pass the
/// raw-input filter), so injecting the committed copy would double-type.
inline bool IsValidUnicodeScalar(const std::uint32_t codepoint)
{
    if (codepoint < 0x80U || codepoint > 0x10FFFFU)
    {
        return false;
    }
    return !(codepoint >= 0xD800U && codepoint <= 0xDFFFU);
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
} // namespace Hooks::SkseMenuFrameworkBridgeLogic
