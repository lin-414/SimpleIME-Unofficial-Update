//
// Headless tests for the character-delivery logic (CharDeliveryLogic.h): the
// caps-lock normalization applied to forwarded WM_CHAR letters, the module
// name matcher that decides whether it applies at all, and whether a passthrough
// WM_CHAR is forwarded to the game in the first place.
//
#include "utils/CharDeliveryLogic.h"

#include <gtest/gtest.h>

using namespace Ime::CharDelivery;

TEST(CharDeliveryAsciiLetter, accepts_only_ascii_letters)
{
    EXPECT_TRUE(IsAsciiLetter(L'a'));
    EXPECT_TRUE(IsAsciiLetter(L'z'));
    EXPECT_TRUE(IsAsciiLetter(L'A'));
    EXPECT_TRUE(IsAsciiLetter(L'Z'));
    EXPECT_FALSE(IsAsciiLetter(L'0'));
    EXPECT_FALSE(IsAsciiLetter(L' '));
    EXPECT_FALSE(IsAsciiLetter(L'-'));
    EXPECT_FALSE(IsAsciiLetter(L'{'));
    EXPECT_FALSE(IsAsciiLetter(L'@'));
    EXPECT_FALSE(IsAsciiLetter(0x00E9)); // é
    EXPECT_FALSE(IsAsciiLetter(0x4E2D)); // 中
    EXPECT_FALSE(IsAsciiLetter(0x0410)); // Cyrillic А
}

TEST(CharDeliveryNormalizeCapsCase, shift_selects_case_and_caps_is_ignored)
{
    // Caps off: the character Windows produced already matches Shift.
    EXPECT_EQ(NormalizeCapsCase(L'a', false), L'a');
    EXPECT_EQ(NormalizeCapsCase(L'A', true), L'A');
    // Caps on: Windows produced the opposite case, Shift still wins.
    EXPECT_EQ(NormalizeCapsCase(L'A', false), L'a');
    EXPECT_EQ(NormalizeCapsCase(L'a', true), L'A');
    // No separate "Caps on + Shift" case: the function only sees the resolved
    // character plus the Shift state, so that input pair is identical to the
    // Caps-off one above.
}

TEST(CharDeliveryNormalizeCapsCase, leaves_non_letters_alone)
{
    EXPECT_EQ(NormalizeCapsCase(L'1', true), L'1');
    EXPECT_EQ(NormalizeCapsCase(L'!', false), L'!');
    EXPECT_EQ(NormalizeCapsCase(L' ', false), L' ');
    EXPECT_EQ(NormalizeCapsCase(0x00E9, false), 0x00E9); // é keeps its case
    EXPECT_EQ(NormalizeCapsCase(0x4E2D, false), 0x4E2D); // 中
    EXPECT_EQ(NormalizeCapsCase(0x0000, false), 0x0000);
}

TEST(CharDeliveryCapsBlockingModule, matches_kill_caps_lock_ng)
{
    EXPECT_TRUE(IsCapsBlockingModuleName(L"KillCapsLockNG.dll"));
    EXPECT_TRUE(IsCapsBlockingModuleName(L"Kill Caps Lock NG.dll"));
    EXPECT_TRUE(IsCapsBlockingModuleName(L"kill_capslock.dll"));
    EXPECT_TRUE(IsCapsBlockingModuleName(L"KILLCAPSLOCKNG.DLL"));
    EXPECT_TRUE(IsCapsBlockingModuleName(L"CapsLockFix.dll"));
    EXPECT_TRUE(IsCapsBlockingModuleName(L"CapsLock.dll"));
    // The marker may sit behind separators that are stripped while folding.
    EXPECT_TRUE(IsCapsBlockingModuleName(L"kill-caps-lock-ng.dll"));
}

TEST(CharDeliveryCapsBlockingModule, ignores_unrelated_modules)
{
    EXPECT_FALSE(IsCapsBlockingModuleName(L"SkyrimSE.exe"));
    EXPECT_FALSE(IsCapsBlockingModuleName(L"SimpleIME.dll"));
    EXPECT_FALSE(IsCapsBlockingModuleName(L"caps.dll")); // "caps" alone is not the marker
    EXPECT_FALSE(IsCapsBlockingModuleName(L""));
    EXPECT_FALSE(IsCapsBlockingModuleName(L"d3d11.dll"));
    EXPECT_FALSE(IsCapsBlockingModuleName(L"KeyboardShortcutsFixLoader.dll"));
}

TEST(CharDeliveryPassthroughForward, skips_only_engine_queue_hosts)
{
    // The engine's own DirectInput echo already delivers the key to an
    // EngineEvents-channel external ImGui host (Tailor 3.x): forwarding our
    // WM_CHAR copy would type the character twice.
    EXPECT_FALSE(ShouldForwardPassthroughChar(true));
    // Scaleform menus never see the engine's echo (ImeMenu::OnCharEvent
    // swallows it) and the bridges own their delivery channels — the forward
    // stays their only source of letters.
    EXPECT_TRUE(ShouldForwardPassthroughChar(false));
}
