#include "utils/InputMethodIdentity.h"

#include <gtest/gtest.h>

namespace IM = Ime::InputMethodIdentity;

namespace
{
using Kind = IM::Identity::Kind;

constexpr auto kClsid   = "{81D4E9C9-1D3B-41BC-9E6C-4B40BF79E35E}";
constexpr auto kProfile = "{FA550B04-5AD7-411F-A5AC-CA038EC515D7}";
constexpr auto kOther   = "{00000000-0000-0000-0000-000000000001}";

/// The shipped default: nothing configured, the input method must not be touched.
TEST(InputMethodIdentityTest, is_unset_covers_the_shipped_default)
{
    EXPECT_TRUE(IM::IsUnset(""));
    EXPECT_TRUE(IM::IsUnset("   "));
    EXPECT_TRUE(IM::IsUnset("auto"));
    EXPECT_TRUE(IM::IsUnset("AUTO"));
    EXPECT_TRUE(IM::IsUnset(" system "));
    EXPECT_TRUE(IM::IsUnset("none"));

    EXPECT_FALSE(IM::IsUnset("tip:" + std::string{kClsid} + "/" + kProfile));
    EXPECT_FALSE(IM::IsUnset("微信输入法"));

    EXPECT_EQ(IM::Parse("").kind, Kind::Unset);
    EXPECT_EQ(IM::Parse("  Auto\n").kind, Kind::Unset);
}

TEST(InputMethodIdentityTest, parses_a_tip_identity)
{
    const auto identity = IM::Parse(std::string{"tip:"} + kClsid + "/" + kProfile);

    ASSERT_EQ(identity.kind, Kind::Tip);
    EXPECT_EQ(identity.clsid, kClsid);
    EXPECT_EQ(identity.profile, kProfile);
    EXPECT_EQ(identity.langid, 0U);
}

TEST(InputMethodIdentityTest, formats_and_round_trips_a_tip)
{
    const auto text = IM::FormatFromTip(kClsid, kProfile, 0x0804);
    EXPECT_EQ(text, std::string{"tip:"} + kClsid + "/" + kProfile + "/0804");

    const auto identity = IM::Parse(text);
    ASSERT_EQ(identity.kind, Kind::Tip);
    EXPECT_EQ(identity.clsid, kClsid);
    EXPECT_EQ(identity.profile, kProfile);
    EXPECT_EQ(identity.langid, 0x0804);

    // No langid suffix when unconstrained.
    EXPECT_EQ(IM::FormatFromTip(kClsid, kProfile), std::string{"tip:"} + kClsid + "/" + kProfile);
}

TEST(InputMethodIdentityTest, rejects_malformed_tip)
{
    EXPECT_EQ(IM::Parse("tip:").kind, Kind::Unknown);
    EXPECT_EQ(IM::Parse("tip:" + std::string{kClsid}).kind, Kind::Unknown); // no profile segment
    EXPECT_EQ(IM::Parse("tip:not-a-guid/" + std::string{kProfile}).kind, Kind::Unknown);
    EXPECT_EQ(IM::Parse(std::string{"tip:"} + kClsid + "/not-a-guid").kind, Kind::Unknown);
    EXPECT_EQ(IM::Parse(std::string{"tip:"} + kClsid + "/" + kProfile + "/zzzz").kind, Kind::Unknown);
    EXPECT_EQ(IM::Parse(std::string{"tip:"} + kClsid + "/" + kProfile + "/12345").kind, Kind::Unknown);
    EXPECT_EQ(IM::Parse("keyboard:1234").kind, Kind::Unknown);
}

TEST(InputMethodIdentityTest, falls_back_to_display_name)
{
    // Anything that is not an identity prefix is matched as the profile's display
    // name: a hand-written name in the TOML still does something sensible.
    const auto identity = IM::Parse("  微信输入法 ");
    ASSERT_EQ(identity.kind, Kind::Desc);
    EXPECT_EQ(identity.desc, "微信输入法");

    EXPECT_EQ(IM::Parse("keyboard:00000804").kind, Kind::Keyboard);
}

TEST(InputMethodIdentityTest, matches_only_the_same_tip)
{
    const auto configured = IM::Parse(std::string{"tip:"} + kClsid + "/" + kProfile);
    const auto same       = IM::Parse(std::string{"tip:"} + kClsid + "/" + kProfile);
    const auto otherGuid  = IM::Parse(std::string{"tip:"} + kClsid + "/" + kOther);
    const auto otherClsid = IM::Parse(std::string{"tip:"} + kOther + "/" + kProfile);

    EXPECT_TRUE(IM::Matches(configured, same));
    EXPECT_FALSE(IM::Matches(configured, otherGuid));
    EXPECT_FALSE(IM::Matches(configured, otherClsid));

    // GUID text is case-insensitive so a hand-edited value still matches.
    IM::Identity lower = configured;
    for (auto &c : lower.clsid)
    {
        c = IM::Detail::AsciiLower(c);
    }
    EXPECT_TRUE(IM::Matches(configured, lower));
}

TEST(InputMethodIdentityTest, langid_constrains_only_when_set)
{
    auto candidate   = IM::Parse(std::string{"tip:"} + kClsid + "/" + kProfile);
    candidate.langid = 0x0409;

    EXPECT_TRUE(IM::Matches(IM::Parse(std::string{"tip:"} + kClsid + "/" + kProfile), candidate));

    const auto constrainedZh = IM::Parse(std::string{"tip:"} + kClsid + "/" + kProfile + "/0804");
    EXPECT_FALSE(IM::Matches(constrainedZh, candidate));

    const auto constrainedEn = IM::Parse(std::string{"tip:"} + kClsid + "/" + kProfile + "/409");
    EXPECT_TRUE(IM::Matches(constrainedEn, candidate));
}

/// The "already active" shortcut rule (InputMethodManager::ActivateProfile): a
/// multi-language TIP enumerates once per language with the SAME (clsid,
/// guidProfile), so the langid alone tells the variants apart — a GUID-only
/// compare would skip switching to the other variant and typing would silently
/// keep the wrong IME.
TEST(InputMethodIdentityTest, same_tip_variant_requires_the_same_language)
{
    const auto zh      = IM::Parse(std::string{"tip:"} + kClsid + "/" + kProfile + "/0804");
    const auto ja      = IM::Parse(std::string{"tip:"} + kClsid + "/" + kProfile + "/0411");
    const auto anyLang = IM::Parse(std::string{"tip:"} + kClsid + "/" + kProfile);

    EXPECT_TRUE(IM::SameTipVariant(zh, zh));
    EXPECT_TRUE(IM::SameTipVariant(anyLang, zh)); // unconstrained candidate matches any variant
    EXPECT_FALSE(IM::SameTipVariant(zh, ja));     // same GUID, other language variant live
    EXPECT_FALSE(IM::SameTipVariant(ja, zh));

    EXPECT_FALSE(IM::SameTipVariant(zh, IM::Parse(std::string{"tip:"} + kClsid + "/" + kOther + "/0804")));
    EXPECT_FALSE(IM::SameTipVariant(zh, IM::Parse(std::string{"tip:"} + kOther + "/" + kProfile + "/0804")));

    // GUID text is case-insensitive, mirroring Matches.
    IM::Identity lower = zh;
    for (auto &c : lower.clsid)
    {
        c = IM::Detail::AsciiLower(c);
    }
    EXPECT_TRUE(IM::SameTipVariant(lower, zh));

    // A non-TIP is never "the same TIP": the stale-cache stub resolves to a
    // keyboard layout, which must force a real activation instead of a skip.
    EXPECT_FALSE(IM::SameTipVariant(zh, IM::Parse("keyboard:00000409")));
    EXPECT_FALSE(IM::SameTipVariant(IM::Parse("keyboard:00000409"), zh));
    EXPECT_FALSE(IM::SameTipVariant(zh, IM::Parse("微软拼音")));
}

TEST(InputMethodIdentityTest, keyboard_layouts_match_by_klid)
{
    const auto configured = IM::Parse("keyboard:00000804");

    IM::Identity candidate = IM::Parse("keyboard:00000804");
    EXPECT_TRUE(IM::Matches(configured, candidate));

    candidate.klid = "00000409";
    EXPECT_FALSE(IM::Matches(configured, candidate));

    // A keyboard target never matches a TIP candidate, and vice versa.
    IM::Identity tip = IM::Parse(std::string{"tip:"} + kClsid + "/" + kProfile);
    EXPECT_FALSE(IM::Matches(configured, tip));
    EXPECT_FALSE(IM::Matches(tip, IM::Parse("keyboard:00000804")));

    // An unresolved candidate (KLID empty) matches nothing, not even another
    // unresolved one: SameText("", "") would otherwise be a false positive.
    IM::Identity unresolvable;
    unresolvable.kind = Kind::Keyboard;
    EXPECT_FALSE(IM::Matches(configured, unresolvable));
    EXPECT_FALSE(IM::Matches(unresolvable, unresolvable));
}

TEST(InputMethodIdentityTest, unset_and_unknown_match_nothing)
{
    const IM::Identity unset   = IM::Parse("");
    const IM::Identity unknown = IM::Parse("tip:broken");
    const IM::Identity tip     = IM::Parse(std::string{"tip:"} + kClsid + "/" + kProfile);

    EXPECT_FALSE(IM::Matches(unset, tip));
    EXPECT_FALSE(IM::Matches(unknown, tip));
    EXPECT_FALSE(IM::Matches(tip, unset));
    EXPECT_FALSE(IM::Matches(tip, unknown));
}

TEST(InputMethodIdentityTest, display_name_matching_uses_the_candidate_desc)
{
    const auto configured = IM::Parse("Microsoft Pinyin");

    IM::Identity candidate; // a TIP as the manager would build it, plus its display name
    candidate.kind    = Kind::Tip;
    candidate.clsid   = kClsid;
    candidate.profile = kProfile;
    candidate.desc    = "Microsoft Pinyin";
    EXPECT_TRUE(IM::Matches(configured, candidate));

    candidate.desc = "Microsoft Wubi";
    EXPECT_FALSE(IM::Matches(configured, candidate));

    // A candidate without a display name must not match anything by name.
    candidate.desc.clear();
    EXPECT_FALSE(IM::Matches(configured, candidate));
}

TEST(InputMethodIdentityTest, guid_text_validation_is_strict)
{
    EXPECT_TRUE(IM::Detail::IsGuidText(kClsid));
    EXPECT_FALSE(IM::Detail::IsGuidText("81D4E9C9-1D3B-41BC-9E6C-4B40BF79E35E"));   // no braces
    EXPECT_FALSE(IM::Detail::IsGuidText("{81D4E9C9-1D3B-41BC-9E6C-4B40BF79E35"));   // one char short
    EXPECT_FALSE(IM::Detail::IsGuidText("{81D4E9C9-1D3B41BC-9E6C-4B40BF79E35E}"));  // missing dash
    EXPECT_FALSE(IM::Detail::IsGuidText("{81D4E9C9Z1D3B-41BC-9E6C-4B40BF79E35E}")); // bad hex digit
    EXPECT_FALSE(IM::Detail::IsGuidText(""));
}

TEST(InputMethodIdentityTest, keyboard_layout_and_hex_helpers)
{
    EXPECT_TRUE(IM::Detail::IsKlidText("00000804"));
    EXPECT_FALSE(IM::Detail::IsKlidText("0000804"));
    EXPECT_FALSE(IM::Detail::IsKlidText("0000080Z"));

    std::uint16_t value = 0;
    EXPECT_TRUE(IM::Detail::ParseHex16("0804", value));
    EXPECT_EQ(value, 0x0804);
    EXPECT_TRUE(IM::Detail::ParseHex16("409", value));
    EXPECT_EQ(value, 0x0409);
    EXPECT_FALSE(IM::Detail::ParseHex16("", value));
    EXPECT_FALSE(IM::Detail::ParseHex16("12345", value));
    EXPECT_FALSE(IM::Detail::ParseHex16("0x04", value));
}
} // namespace
