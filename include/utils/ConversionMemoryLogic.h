//
// Pure decision logic for the 中/英 "remembered mode" memory: which observed
// value counts as evidence of the mode the user is typing in, and which value
// the next session's prior (input.last_native_conversion) is persisted from.
// Header-only and free of Windows/SKSE dependencies, so the headless suite
// covers it.
//

#pragma once

#include "core/State.h"

#include <cstdint>

namespace Ime::ConversionMemory
{
/**
 * True when a conversion-mode value read out of a TSF compartment can be taken
 * as the IME's own word about its mode.
 *
 * Every value WE publish goes into the same compartments the IME publishes into
 * (SetSessionImeEnabled's session-start push, MirrorConversionMode), and a
 * read-back cannot tell our value from the IME's. For an IME that publishes
 * nothing at all (WeChat: all six watchpoints stay unwritten) the compartment
 * only ever holds that push, so treating it as evidence freezes the memory at
 * the session-start value forever — the exact opposite of what the stamp is
 * for. The caller passes the last value it published itself; anything else was
 * published by the IME.
 */
[[nodiscard]] constexpr auto IsImePublishedValue(const std::uint32_t value, const std::uint32_t lastSelfPublishedValue) -> bool
{
    return value != lastSelfPublishedValue;
}

/**
 * True when `value` is one WE published at some point this session.
 *
 * The single "last published" value above is the right test for a synchronous
 * bounce (the sink echoing the write that is running), but the compartment
 * stamp needs a wider net: it reads a compartment minutes later, and by then
 * the client has published several distinct values (the session-start push,
 * then one mirror per observed toggle). In-game 2026-10-17 the session-end stamp
 * read 0x401 — a value we had mirrored earlier — judged it foreign because the
 * last write had been 0x0, and recorded it as the IME's own word while the live
 * display said 0x0.
 */
[[nodiscard]] constexpr auto
    IsSelfPublishedValue(const std::uint32_t value, const std::uint32_t *history, const std::size_t count) -> bool
{
    for (std::size_t i = 0; i < count; ++i)
    {
        if (history[i] == value)
        {
            return true;
        }
    }
    return false;
}

/**
 * The NATIVE (中/あ side) bit of a raw TF_CONVERSIONMODE value — the only part
 * of the mode the persisted `input.last_native_conversion` flag can express.
 * ROMAN/かな etc. are lost by that key by design; see the comment on the key.
 */
[[nodiscard]] constexpr auto IsNativeMode(const std::uint32_t rawMode) -> bool
{
    return (rawMode & static_cast<std::uint32_t>(Core::State::ConversionMode::Flags::NATIVE)) != 0U;
}

/// Where the persisted prior is taken from — see ChoosePriorSource.
enum class PriorSource
{
    /// Nothing was observed this session: keep the loaded value (the boot-time
    /// WM_NCACTIVATE churn saves before any text input, and writing the
    /// untouched default over the previous session's prior would erase it).
    KeepLoaded,
    /// The mode text was last confirmed in, stamped at every confirmation from
    /// the IME's own live value (State::GetRememberedConversionMode).
    Remembered,
    /// The live display value — only for a session that never stamped a
    /// confirmed mode (the ctor seeds one from the config, so this is the
    /// defensive path).
    Display,
};

/**
 * Which value the persisted prior is derived from.
 *
 * The remembered ("the mode text was last confirmed in") value wins: it is
 * stamped at every text confirmation from the IME's live compartment, so it
 * follows what the user actually typed. The live display value must NOT be the
 * primary source — it is not a record of the user's typing: the disable path
 * flattens it to ALPHANUMERIC on every console close (and whether the save runs
 * before or after that flattening is a cross-thread race), and the bare-Shift
 * prediction flips it for IMEs that publish no toggle of their own. Persisting
 * the display is what handed the next session a first text entry in the
 * opposite mode from the one the user last typed in.
 */
[[nodiscard]] constexpr auto ChoosePriorSource(const bool hasRememberedMode, const bool hasObservedMode) -> PriorSource
{
    if (hasRememberedMode)
    {
        return PriorSource::Remembered;
    }
    if (hasObservedMode)
    {
        return PriorSource::Display;
    }
    return PriorSource::KeepLoaded;
}
} // namespace Ime::ConversionMemory
