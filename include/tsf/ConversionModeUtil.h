//
// Created by jamie on 2026/3/17.
//

#pragma once

#include "core/State.h"

#include <string>
#include <winnt.h>

namespace Ime
{
/// The input-mode glyph for the language bar — the single dimension every
/// Windows IME exposes there.
///
/// Japanese folds character width into the glyph itself (「あ」/「ア」/「カ」/
/// 「ｶ」/「Ａ」/「ａ」), exactly like the system language bar, so a Japanese
/// mode change legitimately replaces the glyph. Chinese IMEs instead treat
/// 中/英 and 全角/半角 as two independent dimensions; this function reports
/// only the first, and GetConversionShapeNameShort reports the second, so a
/// width change never hides which language mode is active.
inline auto GetConversionModeNameShort(LANGID langId, Core::State::ConversionMode conversionMode, bool keyboardOpen) -> std::string
{
    conversionMode.Clear(Core::State::ConversionMode::Flags::ROMAN);
    if (PRIMARYLANGID(langId) == LANG_JAPANESE)
    {
        if (!keyboardOpen)
        {
            if (conversionMode.IsAlphanumeric())
            {
                return "ａ"; ///< 半角英数
            }
            if ((conversionMode.IsFullShape()))
            {
                return "Ａ"; ///< 全角英数
            }
        }
        else
        {
            if (conversionMode.IsAlphanumeric())
            {
                return "ａ"; ///< 半角英数
            }
            if (conversionMode.IsNative() && conversionMode.IsFullShape())
            {
                return "あ"; ///< ひらがな
            }
            if ((conversionMode.IsKatakana() && conversionMode.IsFullShape()))
            {
                return "カ"; ///< 全角カタカナ
            }
            if ((conversionMode.IsFullShape()))
            {
                return "Ａ"; ///< 全角英数
            }
            if ((conversionMode.IsNative() && conversionMode.IsKatakana()))
            {
                return "ｶ"; ///< 半角カタカナ
            }
            if (conversionMode.IsNative())
            {
                // Half-width hiragana (NATIVE without FULLSHAPE) used to fall
                // through every branch and return "" — no standard IME UI has a
                // dedicated glyph for it, so display plain hiragana instead.
                return "あ"; ///< 半角ひらがな
            }
        }
    }
    if (PRIMARYLANGID(langId) == LANG_CHINESE)
    {
        // Only the language mode: FULLSHAPE used to be folded in here
        // (「全角」/「英(全角)」), which hid the 中/英 state exactly while the
        // user was checking it — the reported complaint. Width has its own
        // accessor now.
        return conversionMode.IsNative() ? "中" : "英";
    }
    return "";
}

/// The character-width marker shown next to the mode glyph.
///
/// Empty only for Japanese, where the glyph itself already carries the width
/// (「Ａ」 vs 「ａ」) and a second label would just repeat it. Chinese is the
/// family where width is independent of the language mode (Shift+Space toggles
/// it without touching 中/英), so both values are spelled out — leaving
/// half-width blank made the label look like it had disappeared rather than
/// switched.
inline auto GetConversionShapeNameShort(LANGID langId, Core::State::ConversionMode conversionMode) -> std::string
{
    if (PRIMARYLANGID(langId) != LANG_CHINESE)
    {
        return "";
    }
    return conversionMode.IsFullShape() ? "全角" : "半角";
}
} // namespace Ime
