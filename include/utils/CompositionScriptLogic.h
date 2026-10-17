//
// Pure decision logic for "what the text in a live composition proves about the
// TIP's conversion mode".
//
// A Chinese TIP only starts composing in native mode: raw letters never begin a
// composition there, and a pinyin preedit is romanised ASCII all the way
// through. A Japanese IME is different — Microsoft IME composes ひらがな,
// 全角英数 AND 半角英数 — so on Japanese "a composition is running" proves
// nothing about the mode, and only the characters in it do. Assuming NATIVE on
// every composition start is what displayed 「あ」 while the IME actually typed
// 「ａ」 (in-game report, 2026-10-17).
//
// Header-only and free of Windows/SKSE dependencies, so the headless suite
// covers it.
//

#pragma once

#include <cstdint>
#include <string_view>

namespace Ime::CompositionScript
{
/// What a composition's current text proves. Ordered by strength: kana/Han wins
/// over everything else because a single kana is decisive, while ASCII may still
/// be the romanised input of a Japanese IME before conversion.
enum class Script : std::uint8_t
{
    /// Nothing decisive: empty preedit, punctuation, emoji, symbols.
    Other = 0,
    /// Hiragana / katakana / Han → native input mode.
    KanaOrHan,
    /// U+FF01..U+FF5E → 全角英数.
    FullWidthAscii,
    /// U+0021..U+007E → 半角英数.
    HalfWidthAscii,
};

[[nodiscard]] constexpr auto IsKanaOrHan(const wchar_t c) -> bool
{
    return (c >= 0x3040 && c <= 0x30FF) || // hiragana + katakana
           (c >= 0x31F0 && c <= 0x31FF) || // katakana phonetic extensions
           (c >= 0x3400 && c <= 0x4DBF) || // CJK extension A
           (c >= 0x4E00 && c <= 0x9FFF) || // CJK unified ideographs
           (c >= 0xF900 && c <= 0xFAFF);   // CJK compatibility ideographs
}

[[nodiscard]] constexpr auto IsFullWidthAscii(const wchar_t c) -> bool { return c >= 0xFF01 && c <= 0xFF5E; }

/// Space is deliberately excluded: it is the candidate-conversion key, not a
/// character the user typed into the preedit, so a lone space proves nothing.
[[nodiscard]] constexpr auto IsHalfWidthAscii(const wchar_t c) -> bool { return c >= 0x0021 && c <= 0x007E; }

/// Classify a composition's current text.
[[nodiscard]] constexpr auto Classify(const std::wstring_view text) -> Script
{
    bool fullWidth = false;
    bool halfWidth = false;
    for (const wchar_t c : text)
    {
        if (IsKanaOrHan(c))
        {
            // Decisive: no later character can turn kana back into ASCII.
            return Script::KanaOrHan;
        }
        if (IsFullWidthAscii(c))
        {
            fullWidth = true;
        }
        else if (IsHalfWidthAscii(c))
        {
            halfWidth = true;
        }
    }
    if (fullWidth)
    {
        return Script::FullWidthAscii;
    }
    if (halfWidth)
    {
        return Script::HalfWidthAscii;
    }
    return Script::Other;
}

/// The conversion-mode bits a composition's script proves.
struct ModeEvidence
{
    /// False when the content proves nothing and the mode must be left alone.
    bool decisive = false;
    /// Whether NATIVE should be asserted (true) or retracted (false).
    bool native = false;
    /// Whether the script also proves FULLSHAPE. Kana/Han leaves it unknown:
    /// half-width katakana is NATIVE without FULLSHAPE, so the existing bit must
    /// survive there.
    bool fullShapeKnown = false;
    /// Only meaningful when fullShapeKnown.
    bool fullShape = false;
};

[[nodiscard]] constexpr auto EvidenceFor(const Script script) -> ModeEvidence
{
    switch (script)
    {
    case Script::KanaOrHan:
        return ModeEvidence{.decisive = true, .native = true, .fullShapeKnown = false, .fullShape = false};
    case Script::FullWidthAscii:
        return ModeEvidence{.decisive = true, .native = false, .fullShapeKnown = true, .fullShape = true};
    case Script::HalfWidthAscii:
        return ModeEvidence{.decisive = true, .native = false, .fullShapeKnown = true, .fullShape = false};
    case Script::Other:
        return ModeEvidence{};
    }
    return ModeEvidence{};
}

/// Japanese is the only language this inference applies to: a Chinese IME
/// composes romanised pinyin, so ASCII in the preedit proves nothing there — its
/// native mode is exactly what produces it. The caller passes PRIMARYLANGID().
constexpr std::uint16_t LANG_JAPANESE_PRIMARY = 0x11;

[[nodiscard]] constexpr auto ShouldInferFromComposition(const std::uint16_t primaryLangId) -> bool
{
    return primaryLangId == LANG_JAPANESE_PRIMARY;
}
} // namespace Ime::CompositionScript
