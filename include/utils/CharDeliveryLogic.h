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

/**
 * Whether a passthrough WM_CHAR — a key the TIP released uncomposed, i.e. the
 * IME is open in half-width alphanumeric mode — must still be forwarded to the
 * game (see ImeWnd::HandleCharMessage).
 *
 * On a surface that reads RE::InputEvents off the engine's input queue (an
 * EngineEvents-channel external ImGui host, e.g. Tailor 3.x) the engine's own
 * DirectInput echo of the same physical key is already in that queue, so our
 * forward would type the character a second time (2026-10-24: every letter
 * landed doubled in Tailor's rename field in native mode). Everywhere else the
 * forward is the only source of letters: Scaleform menus never see the engine's
 * echo (ImeMenu::OnCharEvent swallows it) and the bridges (Meridian, Prisma,
 * SKSEMF) own their delivery channels.
 *
 * WM_CHARs that do not originate from a physical keypress the engine also
 * reads (Alt+numpad entry, dead-key accents) share this decision; commit
 * strings never pass through here.
 */
[[nodiscard]] constexpr auto ShouldForwardPassthroughChar(const bool surfaceReadsEngineQueue) -> bool
{
    return !surfaceReadsEngineQueue;
}
} // namespace Ime::CharDelivery
