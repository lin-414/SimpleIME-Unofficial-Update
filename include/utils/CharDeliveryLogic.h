//
// Pure decision logic for the character-delivery paths (the WM_CHAR forwarding
// in ImeWnd, and the "which mods are loaded" question behind it). Header-only
// and free of Windows/SKSE dependencies so the headless suite covers it.
//

#pragma once

#include <string_view>

namespace Ime::CharDelivery
{
/// ASCII letters are the only characters every supported layout applies the
/// Caps Lock toggle to, and the console/editor text this mod delivers is ASCII
/// in practice.
[[nodiscard]] constexpr auto IsAsciiLetter(const wchar_t c) -> bool
{
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
}

/**
 * Re-derive the case of an ASCII letter with the Caps Lock TOGGLE ignored:
 * Shift still selects upper case, everything else lower case.
 *
 * Windows resolved the character we forward against the OS Caps Lock state
 * (TranslateMessage -> ToUnicode), while the game's own character generation
 * tracks caps itself from DirectInput — and a caps-blocking mod patches exactly
 * that toggle out (Kill Caps Lock NG replaces the `sete al` of the game's caps
 * flip-flop, so its console text never turns upper case). While the IME owns the
 * keyboard the game's char events are swallowed and our path is the only source
 * of letters, so with such a mod loaded the two disagree and the user's Caps
 * Lock comes back to life in the console. Applying this keeps our path in line
 * with the patched game. Callers gate it on IsCapsBlockingModuleName: without
 * such a mod the game honors caps, and so must we (a user may want "SWORD").
 */
[[nodiscard]] constexpr auto NormalizeCapsCase(const wchar_t c, const bool shiftDown) -> wchar_t
{
    if (!IsAsciiLetter(c))
    {
        return c;
    }
    if (shiftDown)
    {
        return (c >= L'a' && c <= L'z') ? static_cast<wchar_t>(c - L'a' + L'A') : c;
    }
    return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
}

/**
 * True for a loaded module name that looks like a "Caps Lock must not toggle"
 * mod — Kill Caps Lock NG ships `KillCapsLockNG.dll` (Nexus 99090). Case,
 * spaces, underscores and dashes are ignored, so `Kill Caps Lock NG.dll`,
 * `kill_capslock.dll` and `KillCapsLockNG.dll` all match.
 */
[[nodiscard]] inline auto IsCapsBlockingModuleName(const std::wstring_view moduleName) -> bool
{
    // Fold to bare lower-case alphanumerics, then look for the marker. Matching
    // a substring (not a full name) keeps working when the DLL is renamed or a
    // sibling mod ships the same block under its own name.
    constexpr std::wstring_view MARKER = L"capslock";
    std::wstring_view::size_type matched = 0;
    for (const wchar_t c : moduleName)
    {
        wchar_t folded = c;
        if (folded >= L'A' && folded <= L'Z')
        {
            folded = static_cast<wchar_t>(folded - L'A' + L'a');
        }
        else if (!((folded >= L'a' && folded <= L'z') || (folded >= L'0' && folded <= L'9')))
        {
            continue; // separators (' ', '_', '-', '.') never interrupt the marker
        }
        matched = (folded == MARKER[matched]) ? matched + 1 : (folded == MARKER[0] ? 1 : 0);
        if (matched == MARKER.size())
        {
            return true;
        }
    }
    return false;
}
} // namespace Ime::CharDelivery
