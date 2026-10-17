//
// Headless tests for the 中/英 conversion-memory logic (ConversionMemoryLogic.h):
// which compartment read counts as the IME's own word, how the raw mode bits map
// onto the persisted native flag, and which source the persisted prior comes from.
//
#include "utils/ConversionMemoryLogic.h"

#include <gtest/gtest.h>

using namespace Ime::ConversionMemory;

TEST(ConversionMemoryImePublishedValue, our_own_publish_is_not_evidence)
{
    // We pushed 0x0 at the session start (or mirrored it): reading it back is our
    // own value, not the IME's — an IME that publishes nothing (WeChat) only ever
    // holds this one.
    EXPECT_FALSE(IsImePublishedValue(0x0, 0x0));
    EXPECT_FALSE(IsImePublishedValue(0x401, 0x401));
    // Anything else came from the IME (Microsoft Pinyin writes 0x401 on Shift).
    EXPECT_TRUE(IsImePublishedValue(0x401, 0x0));
    EXPECT_TRUE(IsImePublishedValue(0x0, 0x401));
    EXPECT_TRUE(IsImePublishedValue(0x11, 0x1)); // ROMAN bit alone is a real change
}

TEST(ConversionMemoryNativeMode, only_the_native_bit_is_expressible)
{
    EXPECT_TRUE(IsNativeMode(0x1));   // NATIVE — the 中/あ side
    EXPECT_TRUE(IsNativeMode(0x401)); // NATIVE | SYMBOL — Microsoft Pinyin's 中
    EXPECT_TRUE(IsNativeMode(0x11));  // NATIVE | ROMAN — Japanese ローマ字入力
    EXPECT_FALSE(IsNativeMode(0x0));  // ALPHANUMERIC — the 英 side
    EXPECT_FALSE(IsNativeMode(0x400)); // SYMBOL alone: what the bare-Shift prediction
                                       // leaves behind when it clears NATIVE
    EXPECT_FALSE(IsNativeMode(0x10)); // ROMAN alone (半角英数 with the roman bit)
    EXPECT_FALSE(IsNativeMode(0x8));  // FULLSHAPE alone
}

TEST(ConversionMemoryPriorSource, remembered_mode_wins_over_the_live_display)
{
    // The normal case: the stamp ran at least once, so the confirmed mode decides.
    EXPECT_EQ(ChoosePriorSource(true, true), PriorSource::Remembered);
    EXPECT_EQ(ChoosePriorSource(true, false), PriorSource::Remembered);
    // A session that never stamped one (no commit/insert/forwarded letter) falls
    // back to the live display value.
    EXPECT_EQ(ChoosePriorSource(false, true), PriorSource::Display);
    // Nothing observed at all: keep the loaded value instead of overwriting the
    // previous session's prior with an untouched default.
    EXPECT_EQ(ChoosePriorSource(false, false), PriorSource::KeepLoaded);
}

TEST(ConversionMemorySelfPublishedValue, any_value_we_wrote_counts_as_our_own)
{
    const std::uint32_t history[] = {0x1, 0x401, 0x409};
    EXPECT_TRUE(IsSelfPublishedValue(0x1, history, 3));
    EXPECT_TRUE(IsSelfPublishedValue(0x401, history, 3));
    EXPECT_TRUE(IsSelfPublishedValue(0x409, history, 3));
    // Never written by us: that is the IME's own word.
    EXPECT_FALSE(IsSelfPublishedValue(0x0, history, 3));
    EXPECT_FALSE(IsSelfPublishedValue(0x8, history, 3));
    // Empty history: nothing was published, so nothing can be ours.
    EXPECT_FALSE(IsSelfPublishedValue(0x0, history, 0));
}

TEST(ConversionMemorySelfPublishedValue, the_reported_regression_is_caught)
{
    // In-game 2026-10-17: the session-end stamp read 0x401 — a value we had
    // mirrored earlier — while m_lastMirroredMode was 0x0 (the disable path had
    // just flattened it). The single-value test called 0x401 foreign and
    // recorded it as the IME's own word while the display said 0x0; the history
    // recognises it.
    const std::uint32_t history[] = {0x401, 0x0};
    EXPECT_TRUE(IsImePublishedValue(0x401, 0x0)); // the old single-value answer: "foreign"
    EXPECT_TRUE(IsSelfPublishedValue(0x401, history, 2));
}
