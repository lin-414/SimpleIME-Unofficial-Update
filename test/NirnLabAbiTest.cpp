//
// ABI guard for the NirnLabUIPlatform (UIPlatform) backend of the Meridian
// bridge.
//
// The backend hooks vtable slots of the *published* NirnLabUIPlatformAPI
// headers: IUIPlatformAPI slots 1 (AddOrGetBrowser) and 2
// (ReleaseBrowserHandle) — plus slot 3 (the settings overload, API 2.0+,
// version-gated) — and the IBrowser slot 6 (SetBrowserFocused). These tests
// verify, for the compiler that builds this plugin, that the slot assumptions
// hold: a mock implementing each header is compiled with the same MSVC-
// compatible ABI and its own vtable is probed. A second mock shaped like the
// 1.1-era IUIPlatformAPI (three virtuals) checks the slots 1/2 stability
// across API versions that the backend claims to support.
//
#include "hooks/NirnLabApi.h"

#include "NirnLabUIPlatform/API.h"

#include <gtest/gtest.h>

#include <string>

namespace
{
using namespace NL::UI;
using ::NL::CEF::IBrowser;
using Browser          = ::NL::CEF::IBrowser;
using BrowserRefHandle = ::NL::UI::IUIPlatformAPI::BrowserRefHandle;

constexpr BrowserRefHandle kSentinelHandle = 4711;

// --- 3.3-shaped mocks (the vendored headers) ----------------------------

struct MockBrowser final : IBrowser
{
    bool focusValue                 = false;
    bool setFocusedCalled           = false;

    bool   IsBrowserReady() override { return true; }
    bool   IsPageLoaded() override { return true; }
    void   SetBrowserVisible(bool) override {}
    bool   IsBrowserVisible() override { return true; }
    void   ToggleBrowserVisibleByKeys(const std::uint32_t, const std::uint32_t) override {}
    void   SetBrowserFocused(const bool value) override
    {
        setFocusedCalled = true;
        focusValue       = value;
    }
    bool IsBrowserFocused() override { return focusValue; }
    void ToggleBrowserFocusByKeys(const std::uint32_t, const std::uint32_t) override {}
    void LoadBrowserURL(const char *, bool) override {}
    void ExecuteJavaScript(const char *, const char *) override {}
    void AddFunctionCallback(const ::NL::JS::JSFuncInfo &) override {}
    void RemoveFunctionCallback(const char *, const char *) override {}
    void RemoveFunctionCallback(const ::NL::JS::JSFuncInfo &) override {}
    void ExecEventFunction(const char *, const char *) override {}
};

struct MockPlatformApi final : IUIPlatformAPI
{
    Browser *addOrGetOut          = nullptr;
    Browser *addOrGetSettingsOut  = nullptr;
    BrowserRefHandle releasedHandle = 0;

    BrowserRefHandle AddOrGetBrowser(const char *, ::NL::JS::JSFuncInfo *const *, const std::uint32_t, const char *,
                                     Browser *&a_outBrowser) override
    {
        a_outBrowser = addOrGetOut;
        return kSentinelHandle;
    }
    void ReleaseBrowserHandle(const BrowserRefHandle handle) override { releasedHandle = handle; }
    BrowserRefHandle AddOrGetBrowser(const char *, ::NL::JS::JSFuncInfo *const *, const std::uint32_t, const char *,
                                     BrowserSettings *, Browser *&a_outBrowser) override
    {
        a_outBrowser = addOrGetSettingsOut;
        return kSentinelHandle + 1;
    }
    void RegisterOnShutdown(OnShutdownFunc_t) override {}
};

// --- 1.1-shaped mock (three virtuals, as the ver1.1 header published) ----

struct MockPlatformApi11
{
    Browser *out                 = nullptr;
    BrowserRefHandle released    = 0;

    virtual ~MockPlatformApi11() = default;
    virtual BrowserRefHandle __cdecl AddOrGetBrowser(const char *, ::NL::JS::JSFuncInfo *const *,
                                                     const std::uint32_t, const char *, Browser *&a_outBrowser)
    {
        a_outBrowser = out;
        return kSentinelHandle;
    }
    virtual void __cdecl ReleaseBrowserHandle(const BrowserRefHandle handle) { released = handle; }
};

MockBrowser     g_browser;
MockPlatformApi g_api;
MockPlatformApi11 g_api11;
} // namespace

TEST(NirnLabAbi, browser_vtable_slot6_is_set_browser_focused)
{
    auto **table = *reinterpret_cast<void ***>(&g_browser);
    using SetFocusedFn = void (*)(IBrowser *, bool);
    const auto setFocused = reinterpret_cast<SetFocusedFn>(table[Hooks::NirnLabBridge::SLOT_SET_BROWSER_FOCUSED]);
    setFocused(&g_browser, true);
    EXPECT_TRUE(g_browser.setFocusedCalled);
    EXPECT_TRUE(g_browser.focusValue);
    setFocused(&g_browser, false);
    EXPECT_FALSE(g_browser.focusValue);
}

TEST(NirnLabAbi, api33_slot1_is_the_settings_overload)
{
    // Empirically verified layout (MSVC cl AND clang-cl): the two
    // AddOrGetBrowser overloads sit in REVERSE declaration order — the
    // settings overload first. See the layout note in NirnLabApi.h.
    auto **table = *reinterpret_cast<void ***>(&g_api);
    using AddOrGetSettingsFn = BrowserRefHandle (*)(IUIPlatformAPI *, const char *, ::NL::JS::JSFuncInfo *const *,
                                                    std::uint32_t, const char *, BrowserSettings *, Browser *&);
    MockBrowser      outBrowser;
    g_api.addOrGetSettingsOut = &outBrowser;
    const auto addOrGet =
        reinterpret_cast<AddOrGetSettingsFn>(table[Hooks::NirnLabBridge::SlotsFor(Hooks::NirnLabBridge::MakeVersionInt(3, 3)).addOrGetBrowserSettings]);
    Browser *out = nullptr;
    BrowserSettings settings{};
    EXPECT_EQ(addOrGet(&g_api, "n", nullptr, 0, "https://x", &settings, out), kSentinelHandle + 1);
    EXPECT_EQ(out, &outBrowser);
}

TEST(NirnLabAbi, api33_slot2_is_the_5arg_overload_and_slot3_release)
{
    auto **table = *reinterpret_cast<void ***>(&g_api);
    const auto slots = Hooks::NirnLabBridge::SlotsFor(Hooks::NirnLabBridge::MakeVersionInt(3, 3));
    using AddOrGetFn = BrowserRefHandle (*)(IUIPlatformAPI *, const char *, ::NL::JS::JSFuncInfo *const *,
                                            std::uint32_t, const char *, Browser *&);
    using ReleaseFn  = void (*)(IUIPlatformAPI *, BrowserRefHandle);

    MockBrowser outBrowser;
    g_api.addOrGetOut = &outBrowser;
    const auto addOrGet = reinterpret_cast<AddOrGetFn>(table[slots.addOrGetBrowser]);
    Browser *out        = nullptr;
    EXPECT_EQ(addOrGet(&g_api, "n", nullptr, 0, "https://x", out), kSentinelHandle);
    EXPECT_EQ(out, &outBrowser);

    const auto release = reinterpret_cast<ReleaseFn>(table[slots.releaseBrowserHandle]);
    release(&g_api, 7);
    EXPECT_EQ(g_api.releasedHandle, 7U);
}

TEST(NirnLabAbi, one_x_api_keeps_declaration_order)
{
    // The 1.1-era interface has no overloads: plain declaration order —
    // slot 1 the 5-arg AddOrGetBrowser, slot 2 ReleaseBrowserHandle. The
    // version gate must pick this table, not the 2.0+ one.
    auto **table = *reinterpret_cast<void ***>(&g_api11);
    const auto slots = Hooks::NirnLabBridge::SlotsFor(Hooks::NirnLabBridge::MakeVersionInt(1, 1));
    using AddOrGetFn = BrowserRefHandle (*)(MockPlatformApi11 *, const char *, ::NL::JS::JSFuncInfo *const *,
                                            std::uint32_t, const char *, Browser *&);
    using ReleaseFn  = void (*)(MockPlatformApi11 *, BrowserRefHandle);

    EXPECT_EQ(slots.addOrGetBrowser, 1U);
    EXPECT_EQ(slots.addOrGetBrowserSettings, 0U);
    EXPECT_EQ(slots.releaseBrowserHandle, 2U);

    MockBrowser outBrowser;
    g_api11.out = &outBrowser;
    const auto addOrGet = reinterpret_cast<AddOrGetFn>(table[slots.addOrGetBrowser]);
    Browser *out        = nullptr;
    EXPECT_EQ(addOrGet(&g_api11, "n", nullptr, 0, "https://x", out), kSentinelHandle);
    EXPECT_EQ(out, &outBrowser);

    const auto release = reinterpret_cast<ReleaseFn>(table[slots.releaseBrowserHandle]);
    release(&g_api11, 9);
    EXPECT_EQ(g_api11.released, 9U);
}

TEST(NirnLabVersionGate, accepts_covered_majors_and_refuses_unknowns)
{
    using namespace Hooks::NirnLabBridge;
    EXPECT_TRUE(IsSupportedApiVersion(MakeVersionInt(1, 1)));
    EXPECT_TRUE(IsSupportedApiVersion(MakeVersionInt(2, 0)));
    EXPECT_TRUE(IsSupportedApiVersion(MakeVersionInt(3, 3)));
    EXPECT_FALSE(IsSupportedApiVersion(MakeVersionInt(4, 0)));
    EXPECT_FALSE(IsSupportedApiVersion(MakeVersionInt(0, 9)));
}

TEST(NirnLabVersionGate, settings_overload_appears_with_major2)
{
    using namespace Hooks::NirnLabBridge;
    EXPECT_FALSE(HasSettingsOverload(MakeVersionInt(1, 1)));
    EXPECT_TRUE(HasSettingsOverload(MakeVersionInt(2, 0)));
    EXPECT_TRUE(HasSettingsOverload(MakeVersionInt(3, 3)));
}

TEST(NirnLabVersionGate, version_strings_and_parts)
{
    using namespace Hooks::NirnLabBridge;
    EXPECT_EQ(VersionMajor(MakeVersionInt(3, 3)), 3U);
    EXPECT_EQ(VersionMinor(MakeVersionInt(3, 3)), 3U);
    EXPECT_EQ(VersionString(MakeVersionInt(1, 1)), "1.1");
}
