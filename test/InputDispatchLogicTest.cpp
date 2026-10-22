//
// Headless tests for the input-dispatch hook's pure decision logic (extracted
// into InputDispatchLogic.h so no SKSE/Windows machinery is needed here).
//
#include "hooks/InputDispatchLogic.h"

#include <gtest/gtest.h>

using namespace Hooks::InputDispatchLogic;

TEST(InputDispatchCommitKey, swallowed_only_mid_composition_on_a_self_drawn_host)
{
    EXPECT_TRUE(ShouldSwallowCommitKey(true, false, true));
}

TEST(InputDispatchCommitKey, left_alone_outside_a_composition)
{
    // No composition: Enter is the user's key, both on an ImGui field (submit
    // the form) and anywhere else.
    EXPECT_FALSE(ShouldSwallowCommitKey(false, false, true));
    EXPECT_FALSE(ShouldSwallowCommitKey(false, false, false));
    EXPECT_FALSE(ShouldSwallowCommitKey(false, true, true));
}

TEST(InputDispatchCommitKey, left_alone_when_the_tip_is_disabled)
{
    // A stale IN_COMPOSING left over from a torn-down TIP must not keep eating
    // Enter: with the IME off the key belongs to the game again.
    EXPECT_FALSE(ShouldSwallowCommitKey(true, true, true));
    EXPECT_FALSE(ShouldSwallowCommitKey(true, true, false));
}

TEST(InputDispatchCommitKey, left_alone_on_scaleform_fields_and_the_console)
{
    // Scaleform text fields and the console have no ImGui input state to lose,
    // and Enter there means "execute the command": never swallow it.
    EXPECT_FALSE(ShouldSwallowCommitKey(true, false, false));
}

TEST(InputDispatchFocusGrabDefer, waits_while_a_button_is_physically_down)
{
    // The press that opened the field is still down: moving the focus now would
    // replay it into the host's ImGui field and close the field instantly.
    EXPECT_TRUE(ShouldDeferFocusGrab(true, 0, 600));
    EXPECT_TRUE(ShouldDeferFocusGrab(true, 350, 600));
}

TEST(InputDispatchFocusGrabDefer, releases_as_soon_as_the_button_is_up)
{
    // Nobody is holding anything: grab the focus immediately, no waiting.
    EXPECT_FALSE(ShouldDeferFocusGrab(false, 0, 600));
    EXPECT_FALSE(ShouldDeferFocusGrab(false, 350, 600));
}

TEST(InputDispatchFocusGrabDefer, gives_up_at_the_ceiling)
{
    // A long drag (or a stuck key) must not keep the IME off: past the ceiling the
    // enable runs anyway and the replay scrub covers the rest.
    EXPECT_FALSE(ShouldDeferFocusGrab(true, 600, 600));
    EXPECT_FALSE(ShouldDeferFocusGrab(true, 5000, 600));
}

TEST(InputDispatchFocusGrabReplay, scrubbed_right_after_the_grab)
{
    // The batch(es) that land within the window are the engine re-delivering the
    // buttons the host's ClearInputKeys()/ClearInputMouse() just forgot, not the
    // user pressing anything: they must be scrubbed or the numeric field that is
    // opening right now validates itself on the replayed Enter and dies.
    EXPECT_TRUE(IsFocusGrabReplay(0, 300));
    EXPECT_TRUE(IsFocusGrabReplay(16, 300));
    EXPECT_TRUE(IsFocusGrabReplay(299, 300));
}

TEST(InputDispatchFocusGrabReplay, every_key_and_click_after_the_window_gets_through)
{
    EXPECT_FALSE(IsFocusGrabReplay(300, 300));
    EXPECT_FALSE(IsFocusGrabReplay(5000, 300));
}

TEST(InputDispatchFocusGrabReplay, no_scrubbing_before_any_grab)
{
    // -1 is "has never grabbed the focus": a fresh game session must not eat the
    // user's Enter or clicks.
    EXPECT_FALSE(IsFocusGrabReplay(-1, 300));
}

TEST(InputDispatchFocusGrabReplay, a_zero_window_disables_the_scrub)
{
    EXPECT_FALSE(IsFocusGrabReplay(0, 0));
}
