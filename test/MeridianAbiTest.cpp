//
// ABI guard for the Meridian View/1 integration.
//
// The bridge hooks vtable slot 9 of the *published* IViewAPI header. These
// tests verify, for the compiler that builds this plugin, that the slot
// assumption holds (a mock implementing the header is compiled by the same
// MSVC-compatible ABI and its own vtable is probed) and that the negotiation
// helper accepts exactly the published extension name/version.
//
#include "hooks/MeridianApi.h"

#include "MeridianUI/ViewAPI.h"

#include <gtest/gtest.h>

namespace
{
using namespace Meridian::UI::View;

constexpr ViewHandle kSentinelView = 123;

// Every virtual returns a benign default; TryFocus carries the sentinel that
// the raw vtable probe below asserts on.
struct MockViewApi final : IViewAPI
{
    ViewHandle CreateView(const ViewCreateInfo *) override { return 0; }
    void       DestroyView(ViewHandle) override {}
    bool       IsValid(ViewHandle) const override { return true; }
    bool       IsReady(ViewHandle) const override { return true; }
    bool       RegisterListener(ViewHandle, const char *, ListenerCallback) override { return true; }
    bool       ExecuteJavaScript(ViewHandle, const char *) override { return true; }
    bool       Show(ViewHandle) override { return true; }
    bool       Hide(ViewHandle) override { return true; }
    FocusResult TryFocus(const ViewHandle view, const FocusMode mode) override
    {
        return view == kSentinelView && mode == FocusMode::PauseGame ? FocusResult::AlreadyFocused
                                                                     : FocusResult::InvalidView;
    }
    void Unfocus(ViewHandle) override {}
    bool HasFocus(ViewHandle) const override { return true; }
    bool HasAnyFocus() const override { return true; }
};

MockViewApi g_negotiated;
bool        g_acceptQuery = true;

bool QueryExtension(const char *name, std::uint32_t version, void **outInterface, Meridian::UI::Settings *, const char *)
{
    if (!g_acceptQuery || !IsSupported(name, version))
    {
        return false;
    }
    *outInterface = &g_negotiated;
    return true;
}
} // namespace

TEST(MeridianAbi, published_vtable_slot9_is_tryfocus)
{
    // MSVC x64 (and clang-cl in MSVC mode) layout: slot 0 deleting dtor,
    // slot 9 TryFocus — the single slot the bridge hooks.
    auto **table = *reinterpret_cast<void ***>(&g_negotiated);
    using TryFocusFn = FocusResult (*)(IViewAPI *, ViewHandle, FocusMode);
    const auto tryFocus = reinterpret_cast<TryFocusFn>(table[9]);
    EXPECT_EQ(tryFocus(&g_negotiated, kSentinelView, FocusMode::PauseGame), FocusResult::AlreadyFocused);
    EXPECT_EQ(tryFocus(&g_negotiated, 456, FocusMode::PauseGame), FocusResult::InvalidView);
    EXPECT_EQ(tryFocus(&g_negotiated, kSentinelView, FocusMode::Unpaused), FocusResult::InvalidView);
}

TEST(MeridianAbi, negotiation_returns_negotiated_interface)
{
    g_acceptQuery = true;
    EXPECT_EQ(Hooks::MeridianBridge::RequestMeridianView(QueryExtension), &g_negotiated);
}

TEST(MeridianAbi, negotiation_rejects_null_query)
{
    EXPECT_EQ(Hooks::MeridianBridge::RequestMeridianView(nullptr), nullptr);
}

TEST(MeridianAbi, negotiation_rejects_refused_query)
{
    g_acceptQuery = false;
    EXPECT_EQ(Hooks::MeridianBridge::RequestMeridianView(QueryExtension), nullptr);
    g_acceptQuery = true;
}
