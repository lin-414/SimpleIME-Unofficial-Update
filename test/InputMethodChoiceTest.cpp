#include "ime/InputMethodChoice.h"

#include <gtest/gtest.h>

namespace IM = Ime::InputMethodIdentity;

namespace
{
constexpr auto kClsid    = "{81D4E9C9-1D3B-41BC-9E6C-4B40BF79E35E}";
constexpr auto kProfile  = "{FA550B04-5AD7-411F-A5AC-CA038EC515D7}";
constexpr auto kClsid2   = "{00000000-0000-0000-0000-0000000000AA}";
constexpr auto kProfile2 = "{00000000-0000-0000-0000-0000000000BB}";

/// Two entries with distinct identities, as the IME thread would enumerate them.
auto MakeChoices() -> std::vector<Ime::InputMethodChoice>
{
    return {
        {IM::FormatFromTip(kClsid,  kProfile,  0x0804), "\u5fae\u8f6f\u62fc\u97f3"},
        {IM::FormatFromTip(kClsid2, kProfile2, 0x0409), "English layout"          },
    };
}
} // namespace

/// The picker writes an identity string; reading it back must select that row.
TEST(InputMethodChoiceTest, finds_a_stored_identity)
{
    const auto choices = MakeChoices();
    EXPECT_EQ(Ime::FindChoiceIndex(choices, IM::FormatFromTip(kClsid, kProfile, 0x0804)), 0U);
    EXPECT_EQ(Ime::FindChoiceIndex(choices, IM::FormatFromTip(kClsid2, kProfile2, 0x0409)), 1U);
}

/// A langid-less identity still selects the entry (the suffix only constrains).
TEST(InputMethodChoiceTest, identity_without_langid_still_matches)
{
    const auto choices = MakeChoices();
    EXPECT_EQ(Ime::FindChoiceIndex(choices, IM::FormatFromTip(kClsid, kProfile)), 0U);
}

/// Hand-written display names are matched loosely, exactly like the activation
/// path, so the picker highlights what the code would actually activate.
TEST(InputMethodChoiceTest, finds_by_display_name)
{
    const auto choices = MakeChoices();
    EXPECT_EQ(Ime::FindChoiceIndex(choices, "\u5fae\u8f6f\u62fc\u97f3"), 0U);
    EXPECT_EQ(Ime::FindChoiceIndex(choices, "layout"), 1U);
    EXPECT_EQ(Ime::FindChoiceIndex(choices, "LAYOUT"), 1U);
}

/// Nothing configured (the shipped default) selects no row at all.
TEST(InputMethodChoiceTest, unset_selects_nothing)
{
    const auto choices = MakeChoices();
    EXPECT_EQ(Ime::FindChoiceIndex(choices, ""), std::string::npos);
    EXPECT_EQ(Ime::FindChoiceIndex(choices, "auto"), std::string::npos);
    EXPECT_EQ(Ime::FindChoiceIndex(choices, "tip:not-a-guid"), std::string::npos);
}

TEST(InputMethodChoiceTest, unknown_identity_selects_nothing)
{
    const auto choices = MakeChoices();
    EXPECT_EQ(Ime::FindChoiceIndex(choices, IM::FormatFromTip(kClsid, kProfile2, 0x0804)), std::string::npos);
    EXPECT_EQ(Ime::FindChoiceIndex({}, IM::FormatFromTip(kClsid, kProfile, 0x0804)), std::string::npos);
}

/// Keyboard layouts are not selectable yet, so a stored keyboard: value shows as
/// "missing" instead of silently highlighting an unrelated TIP.
TEST(InputMethodChoiceTest, keyboard_identity_selects_nothing)
{
    const auto choices = MakeChoices();
    EXPECT_EQ(Ime::FindChoiceIndex(choices, IM::FormatFromKeyboard("00000804")), std::string::npos);
}

TEST(InputMethodChoiceTest, label_is_the_plain_name_when_unique)
{
    const auto choices = MakeChoices();
    EXPECT_EQ(Ime::ChoiceLabel(choices, 0), "\u5fae\u8f6f\u62fc\u97f3");
    EXPECT_EQ(Ime::ChoiceLabel(choices, 1), "English layout");
}

/// Microsoft ships the same localized name for zh/ja/ko; without the language
/// suffix the combo would show two identical rows.
TEST(InputMethodChoiceTest, label_appends_the_language_on_a_name_clash)
{
    const std::vector<Ime::InputMethodChoice> choices{
        {IM::FormatFromTip(kClsid,  kProfile,  0x0804), "\u5fae\u8f6f\u8f93\u5165\u6cd5"},
        {IM::FormatFromTip(kClsid2, kProfile2, 0x0411), "\u5fae\u8f6f\u8f93\u5165\u6cd5"},
    };
    EXPECT_EQ(Ime::ChoiceLabel(choices, 0), "\u5fae\u8f6f\u8f93\u5165\u6cd5 (0804)");
    EXPECT_EQ(Ime::ChoiceLabel(choices, 1), "\u5fae\u8f6f\u8f93\u5165\u6cd5 (0411)");
}

TEST(InputMethodChoiceTest, label_falls_back_to_the_identity_when_nameless)
{
    const std::vector<Ime::InputMethodChoice> choices{
        {IM::FormatFromTip(kClsid, kProfile, 0x0804), ""}
    };
    EXPECT_EQ(Ime::ChoiceLabel(choices, 0), IM::FormatFromTip(kClsid, kProfile, 0x0804));
    EXPECT_EQ(Ime::ChoiceLabel(choices, 3), "");
}
