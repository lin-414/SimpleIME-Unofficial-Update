#pragma once

#include <cstdint>
#include <string>

#include "NirnLabUIPlatform/API.h"

// Constants and pure helpers for the NirnLabUIPlatform (a.k.a. UIPlatform)
// backend of the Meridian input bridge. Header-only so the ABI and version
// gates can be unit-tested against mocks compiled with the same compiler
// (MeridianAbiTest.cpp / NirnLabAbiTest.cpp pattern).
namespace Hooks::NirnLabBridge
{
// ---- version helpers ---------------------------------------------------

constexpr std::uint32_t MakeVersionInt(const std::uint32_t major, const std::uint32_t minor)
{
    return major * 100000U + minor;
}

constexpr std::uint32_t VersionMajor(const std::uint32_t version)
{
    return version / 100000U;
}

constexpr std::uint32_t VersionMinor(const std::uint32_t version)
{
    return version % 100000U;
}

/// Human-readable "3.3" form for logs and the diagnostics clipboard.
inline std::string VersionString(const std::uint32_t version)
{
    return std::to_string(VersionMajor(version)) + "." + std::to_string(VersionMinor(version));
}

// ---- vtable slots -------------------------------------------------------

// MSVC x64 vtable layout of the *published* headers. The implementation class
// (NL::Controllers::PublicAPIController : public IUIPlatformAPI, ...) and
// (NL::CEF::DefaultBrowser : public IBrowser, public RE::MenuEventHandler)
// both put the interface first, so the interface's slots are the object's
// primary-vtable prefix. MSVC emits ONE destructor slot (the vector deleting
// destructor), which is what the View/1 hook layout already relies on.
//
// CAUTION, verified empirically against mocks compiled by BOTH MSVC cl and
// clang-cl (probe: probe_abi*.cpp, 2026-10-05): virtual OVERLOADS are laid
// out in REVERSE declaration order within the vtable, while everything else
// stays in declaration order. For the 3.3 header
// (declared: AddOrGetBrowser-5, ReleaseBrowserHandle, AddOrGetBrowser-6,
// RegisterOnShutdown) that yields:
//   IUIPlatformAPI 2.0+: [1] AddOrGetBrowser(..., BrowserSettings*, out),
//                        [2] AddOrGetBrowser(name, funcs, size, url, out),
//                        [3] ReleaseBrowserHandle, [4] RegisterOnShutdown
// For 1.x (no overloads) it is plain declaration order:
//   IUIPlatformAPI 1.x:  [1] AddOrGetBrowser(name, funcs, size, url, out),
//                        [2] ReleaseBrowserHandle
// NL::CEF::IBrowser (1.1 through 3.3 — no overloads, identical prefix; 3.3
// appends ExecEventFunction at the end):
//   [0] dtor, [1] IsBrowserReady, [2] IsPageLoaded, [3] SetBrowserVisible,
//   [4] IsBrowserVisible, [5] ToggleBrowserVisibleByKeys,
//   [6] SetBrowserFocused, [7] IsBrowserFocused, ...
struct ApiSlots
{
    std::size_t addOrGetBrowser;         ///< 5-arg overload (vtable slot)
    std::size_t addOrGetBrowserSettings; ///< 6-arg overload; 0 = does not exist on this API version
    std::size_t releaseBrowserHandle;
};

/// Slot table for the negotiated API major version (see the layout note
/// above: the two AddOrGetBrowser overloads swap places between 1.x and 2.0+).
constexpr ApiSlots SlotsFor(const std::uint32_t apiVersion)
{
    if (VersionMajor(apiVersion) >= 2U)
    {
        return {2, 1, 3};
    }
    return {1, 0, 2};
}

constexpr std::size_t SLOT_SET_BROWSER_FOCUSED = 6; ///< on IBrowser, all versions

// ---- protocol gates ------------------------------------------------------

/// API versions this build's vtable assumptions cover. 1.x hooks the 5-arg
/// AddOrGetBrowser at slot 1 and ReleaseBrowserHandle at slot 2; 2.0+ swapped
/// the overloads and moved ReleaseBrowserHandle to slot 3 (see SlotsFor).
/// Major 4+ would need re-probing — refuse instead of hooking into the unknown.
constexpr bool IsSupportedApiVersion(const std::uint32_t apiVersion)
{
    const auto major = VersionMajor(apiVersion);
    return major >= 1U && major <= 3U;
}

/// The 6-argument AddOrGetBrowser overload only exists since API 2.0.
constexpr bool HasSettingsOverload(const std::uint32_t apiVersion)
{
    return VersionMajor(apiVersion) >= 2U;
}

// ---- page→host callback channel ------------------------------------------

/// JS-side name of the page→host callback channel (the page calls
/// `SimpleIME.result(payload)`; the host registers the binding via
/// IBrowser::AddFunctionCallback). Payloads are the same `<id>:<status>`
/// words the View/1 named listener carries.
inline constexpr char FUNCTION_OBJECT_NAME[] = "SimpleIME";
inline constexpr char FUNCTION_RESULT_NAME[] = "result";

/// SKSE messaging labels (NirnLabUIPlatformAPI/API.h APIMessageType).
constexpr std::uint32_t MSG_REQUEST_VERSION  = 2250;
constexpr std::uint32_t MSG_RESPONSE_VERSION = 2251;
constexpr std::uint32_t MSG_REQUEST_API      = 2252;
constexpr std::uint32_t MSG_RESPONSE_API     = 2253;
} // namespace Hooks::NirnLabBridge
