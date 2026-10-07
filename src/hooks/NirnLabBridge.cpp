//
// NirnLabUIPlatform (a.k.a. UIPlatform) focus backend for the Meridian
// input bridge.
//
// UIPlatform-based UIs (e.g. SkipQuestNG) render CEF browsers outside the
// Meridian.View/1 extension — they negotiate `NL::UI::IUIPlatformAPI` over
// SKSE messaging, create browsers with AddOrGetBrowser and take the keyboard
// with IBrowser::SetBrowserFocused. Views created that way never show up in
// the View/1 TryFocus observer, so this backend watches the UIPlatform side:
//
//   1. kPostPostLoad — dispatch RequestVersion (2250), accept the Response
//      (2251) only from our own sender label; the API major version gates
//      everything else (1.x–3.x layouts are covered, see NirnLabApi.h).
//   2. kInputLoaded — dispatch RequestAPI (2252); the synchronous response
//      (2253) hands us the IUIPlatformAPI singleton pointer.
//   3. Hook the public interface's vtable: AddOrGetBrowser (slot 1, plus the
//      settings overload slot 3 on API 2.0+) tracks every browser any
//      consumer mod creates; ReleaseBrowserHandle (slot 2) tracks the ref
//      counts; the first tracked browser's IBrowser vtable gets its
//      SetBrowserFocused slot (6) hooked for instant focus events.
//
// A focused browser is pinned with our own AddOrGetBrowser reference: the
// host destroys the browser object when the last reference goes away, and
// the bridge Tick (render thread on the fallback frame driver) must be able
// to call IsBrowserFocused/ExecuteJavaScript on a focused browser without
// racing a destruction on the game thread. External ref counts are tracked
// from the hooks so a mod releasing its last handle while focused still ends
// the session (and unpins) instead of pinning a dead UI forever.
//
#include "hooks/NirnLabBridge.h"

#include "ImeApp.h"
#include "hooks/MeridianBridge.h"
#include "hooks/MeridianBridgeLogic.h"
#include "hooks/NirnLabApi.h"
#include "hooks/ScopeFlag.h"
#include "log.h"

#include "SKSE/SKSE.h"

#include <Windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace Hooks::NirnLabBridge
{
namespace
{
using namespace ::NL::UI;
using Browser          = ::NL::CEF::IBrowser;
using BrowserRefHandle = IUIPlatformAPI::BrowserRefHandle;

/// REL::safe_write's assert is compiled out in NDEBUG, so a failed
/// VirtualProtect silently no-ops: verify the slot before the write and read
/// it back afterwards.
bool PatchVtableSlot(void *slot, std::uintptr_t hook, std::uintptr_t expected, const char *what)
{
    const auto slotAddress = reinterpret_cast<std::uintptr_t>(slot);
    if (std::memcmp(slot, &expected, sizeof(expected)) != 0)
    {
        logger::error("Vtable verify failed before write: {} slot at {:#x}", what, slotAddress);
        return false;
    }
    REL::safe_write(slotAddress, hook);
    if (std::memcmp(slot, &hook, sizeof(hook)) != 0)
    {
        logger::error("Vtable patch did not stick: {} slot at {:#x}", what, slotAddress);
        return false;
    }
    return true;
}

using AddOrGetBrowserFn = BrowserRefHandle (__cdecl *)(IUIPlatformAPI *, const char *, ::NL::JS::JSFuncInfo *const *,
                                                       const std::uint32_t, const char *, Browser *&);
using AddOrGetBrowserSettingsFn = BrowserRefHandle (__cdecl *)(IUIPlatformAPI *, const char *,
                                                               ::NL::JS::JSFuncInfo *const *, const std::uint32_t,
                                                               const char *, BrowserSettings *, Browser *&);
using ReleaseBrowserHandleFn = void (__cdecl *)(IUIPlatformAPI *, BrowserRefHandle);
using SetBrowserFocusedFn    = void (__cdecl *)(Browser *, bool);

// Detour thunks — defined at the bottom of this namespace; TrackBrowser
// installs the IBrowser one on first sight.
BrowserRefHandle HookedAddOrGetBrowser(IUIPlatformAPI *, const char *, ::NL::JS::JSFuncInfo *const *, std::uint32_t,
                                       const char *, Browser *&);
BrowserRefHandle HookedAddOrGetBrowserSettings(IUIPlatformAPI *, const char *, ::NL::JS::JSFuncInfo *const *,
                                               std::uint32_t, const char *, BrowserSettings *, Browser *&);
void             HookedReleaseBrowserHandle(IUIPlatformAPI *, BrowserRefHandle);
void             HookedSetBrowserFocused(Browser *, bool);

// ---- state ------------------------------------------------------------

// All hook/negotiation contexts are the game thread (SKSE messaging, and
// consumer mods calling the API from their menu/game code); the bridge Tick
// reads only s_focusedBrowser inside MeridianBridge, so plain fields here
// need no atomics. The mutex guards the registry against the theoretical
// non-game-thread API caller.
IUIPlatformAPI *s_api = nullptr;            ///< the process-wide UIPlatform singleton, once negotiated
std::uint32_t   s_apiVersion = 0;           ///< from ResponseVersion; gates the slot-3 hook
SupportState    s_state      = SupportState::Pending;
std::string     s_libVersionString = "-";
std::string     s_apiVersionString = "-";

std::atomic<bool> s_versionAccepted{false};

AddOrGetBrowserFn         s_originalAddOrGetBrowser = nullptr;
AddOrGetBrowserSettingsFn s_originalAddOrGetBrowserSettings = nullptr;
ReleaseBrowserHandleFn    s_originalReleaseBrowserHandle = nullptr;
SetBrowserFocusedFn       s_originalSetBrowserFocused = nullptr;
bool                      s_browserFocusHooked = false; ///< IBrowser slot 6 is hooked once, on the shared vtable

// Re-entrancy guard for the bridge's own AddOrGetBrowser/ReleaseBrowserHandle
// calls (the pin): the hooks must not count our own reference as an external
// one. Every call site here runs on the game thread.
bool s_internalCall = false;

std::mutex s_registryMutex; // game thread in practice; kept for safety
struct BrowserRecord
{
    std::string        name;
    std::uint32_t      externalRefs = 0;
    BrowserRefHandle   pinHandle    = 0; ///< our own AddOrGetBrowser ref while focused; 0 = not pinned
};
std::unordered_map<BrowserRefHandle, Browser *> s_handleToBrowser; ///< every live external handle
std::unordered_map<Browser *, BrowserRecord>    s_browsers;

// ---- registry / pin helpers (game thread) -----------------------------

void TrackBrowser(const BrowserRefHandle handle, const char *name, Browser *browser)
{
    if (handle == IUIPlatformAPI::InvalidBrowserRefHandle || browser == nullptr)
    {
        return;
    }
    std::lock_guard lock(s_registryMutex);
    auto &record            = s_browsers[browser];
    record.name             = name != nullptr ? name : "";
    record.externalRefs    += 1;
    s_handleToBrowser.emplace(handle, browser);
    if (!s_browserFocusHooked)
    {
        // Every IBrowser shares one vtable (a single implementation class),
        // so the first browser seen is the moment to install the focus
        // observer; the detour dispatches on the `this` argument.
        auto **table = *reinterpret_cast<std::uintptr_t ***>(browser);
        s_originalSetBrowserFocused = reinterpret_cast<SetBrowserFocusedFn>(table[SLOT_SET_BROWSER_FOCUSED]);
        if (!PatchVtableSlot(&table[SLOT_SET_BROWSER_FOCUSED], reinterpret_cast<std::uintptr_t>(&HookedSetBrowserFocused),
                             reinterpret_cast<std::uintptr_t>(s_originalSetBrowserFocused),
                             "UIPlatform IBrowser::SetBrowserFocused"))
        {
            // Stay unhooked so the next browser retries the patch.
            return;
        }
        s_browserFocusHooked = true;
        logger::info("UIPlatform browser focus observer installed (IBrowser slot {})", SLOT_SET_BROWSER_FOCUSED);
    }
}

/// Release one external reference (from the ReleaseBrowserHandle hook).
/// Returns the browser whose last external reference went away — pinned or
/// not. The caller ends the session and UnpinBrowser hands the pin back and
/// closes the record; the pin must not gate the return here, or a browser
/// released while focused would keep its pin (and its session) forever.
Browser *UntrackBrowser(const BrowserRefHandle handle)
{
    std::lock_guard lock(s_registryMutex);
    const auto handleIt = s_handleToBrowser.find(handle);
    if (handleIt == s_handleToBrowser.end())
    {
        return nullptr;
    }
    Browser *browser = handleIt->second;
    s_handleToBrowser.erase(handleIt);
    const auto recordIt = s_browsers.find(browser);
    if (recordIt == s_browsers.end())
    {
        return nullptr;
    }
    if (recordIt->second.externalRefs > 0)
    {
        recordIt->second.externalRefs -= 1;
    }
    if (recordIt->second.externalRefs != 0)
    {
        return nullptr; // still referenced by another external handle
    }
    if (recordIt->second.pinHandle != 0)
    {
        return browser; // pinned: UnpinBrowser releases it and drops the record
    }
    s_browsers.erase(recordIt);
    return browser; // last external handle released and not pinned
}

/// Hold our own reference so the host cannot destroy the browser while the
/// bridge session is live on it. Must run on the game thread.
void PinBrowser(Browser *browser)
{
    std::string name;
    {
        std::lock_guard lock(s_registryMutex);
        const auto recordIt = s_browsers.find(browser);
        if (recordIt == s_browsers.end())
        {
            // A browser that never passed through our AddOrGetBrowser hook
            // (created before the hooks installed): focus it, but there is no
            // name to pin with — its lifetime is the host's business then.
            logger::debug("UIPlatform browser {:x} is not tracked by name; skipping the lifetime pin",
                          reinterpret_cast<std::uintptr_t>(browser));
            return;
        }
        if (recordIt->second.pinHandle != 0)
        {
            return; // already pinned
        }
        name = recordIt->second.name;
    }
    Browser *pinned = nullptr;
    // Get-path only in practice (the mod holds references while focusing),
    // and the get-path ignores the url argument entirely.
    auto handle = IUIPlatformAPI::InvalidBrowserRefHandle;
    {
        const ScopeFlag internalCall(s_internalCall);
        if (s_api != nullptr)
        {
            handle = s_api->AddOrGetBrowser(name.c_str(), nullptr, 0, nullptr, pinned);
        }
    }
    if (handle == IUIPlatformAPI::InvalidBrowserRefHandle || pinned != browser)
    {
        logger::warn("UIPlatform browser pin failed for {:x}", reinterpret_cast<std::uintptr_t>(browser));
        return;
    }
    std::lock_guard lock(s_registryMutex);
    if (auto recordIt = s_browsers.find(browser); recordIt != s_browsers.end())
    {
        recordIt->second.pinHandle = handle;
    }
    else
    {
        // Raced with the release hook: give the reference straight back.
        const ScopeFlag internalCall(s_internalCall);
        s_api->ReleaseBrowserHandle(handle);
    }
}

void UnpinBrowser(Browser *browser)
{
    BrowserRefHandle pin = 0;
    {
        std::lock_guard lock(s_registryMutex);
        const auto recordIt = s_browsers.find(browser);
        if (recordIt == s_browsers.end() || recordIt->second.pinHandle == 0)
        {
            return;
        }
        pin                        = recordIt->second.pinHandle;
        recordIt->second.pinHandle = 0;
    }
    {
        const ScopeFlag internalCall(s_internalCall);
        if (s_api != nullptr)
        {
            s_api->ReleaseBrowserHandle(pin);
        }
    }
    // If that was the very last reference the host destroyed the browser;
    // nobody may touch the pointer afterwards — s_focusedBrowser is already
    // cleared by the time this runs (focus end is processed first). Only the
    // pointer VALUE is used here, so the map lookup is safe on a dead browser.
    std::lock_guard lock(s_registryMutex);
    if (auto recordIt = s_browsers.find(browser);
        recordIt != s_browsers.end() && recordIt->second.externalRefs == 0 && recordIt->second.pinHandle == 0)
    {
        s_browsers.erase(recordIt);
    }
}

// ---- vtable detours ---------------------------------------------------

BrowserRefHandle HookedAddOrGetBrowser(IUIPlatformAPI *self, const char *name, ::NL::JS::JSFuncInfo *const *funcs,
                                       const std::uint32_t funcCount, const char *url, Browser *&outBrowser)
{
    const auto handle = s_originalAddOrGetBrowser(self, name, funcs, funcCount, url, outBrowser);
    if (!s_internalCall)
    {
        TrackBrowser(handle, name, outBrowser);
    }
    return handle;
}

BrowserRefHandle HookedAddOrGetBrowserSettings(IUIPlatformAPI *self, const char *name, ::NL::JS::JSFuncInfo *const *funcs,
                                               const std::uint32_t funcCount, const char *url,
                                               BrowserSettings *settings, Browser *&outBrowser)
{
    const auto handle = s_originalAddOrGetBrowserSettings(self, name, funcs, funcCount, url, settings, outBrowser);
    if (!s_internalCall)
    {
        TrackBrowser(handle, name, outBrowser);
    }
    return handle;
}

void HookedReleaseBrowserHandle(IUIPlatformAPI *self, const BrowserRefHandle handle)
{
    s_originalReleaseBrowserHandle(self, handle);
    if (s_internalCall)
    {
        return;
    }
    const auto released = UntrackBrowser(handle);
    if (released == nullptr)
    {
        return;
    }
    // The mod dropped its last reference without unfocusing: end the session
    // and give our pin back (which may be what finally destroys the browser —
    // the bridge focus state is cleared before this returns).
    logger::info("UIPlatform browser's last external reference was released; ending its session");
    MeridianBridge::OnBrowserFocusGone(released);
    UnpinBrowser(released);
}

void HookedSetBrowserFocused(Browser *self, const bool value)
{
    s_originalSetBrowserFocused(self, value);
    if (value)
    {
        PinBrowser(self);
        MeridianBridge::OnBrowserFocused(self);
    }
    else
    {
        MeridianBridge::OnBrowserFocusGone(self);
        UnpinBrowser(self);
    }
}

// ---- page→host listener ----------------------------------------------

// Runs on the host's CEF callback thread (executeInGameThread = false, the
// same contract as the View/1 named listener). NirnLab serializes each JS
// argument to a JSON value; our payload is a string literal inside it.
void OnJsFunctionArgs(const char **args, const int argCount)
{
    if (args == nullptr || argCount < 1 || args[0] == nullptr)
    {
        return;
    }
    std::string payload;
    if (!MeridianBridgeLogic::TryDecodeJsonStringArg(args[0], payload))
    {
        return;
    }
    MeridianBridge::OnBackendListenerPayload(payload.c_str());
}

// ---- SKSE messaging (main thread) ------------------------------------

constexpr const char *SENDER_LABEL = "SimpleIME"; ///< our request label; responses are routed back to it

void OnResponseVersion(const ResponseVersionMessage *version)
{
    s_libVersionString = VersionString(version->libVersion);
    s_apiVersionString = VersionString(version->apiVersion);
    s_apiVersion       = version->apiVersion;
    if (!IsSupportedApiVersion(version->apiVersion))
    {
        s_state = SupportState::Failed;
        logger::warn("NirnLabUIPlatform API version {} is not supported (library {}); the UIPlatform focus backend stays off",
                     s_apiVersionString, s_libVersionString);
        return;
    }
    s_versionAccepted = true;
    logger::info("NirnLabUIPlatform {} detected (API {})", VersionString(version->libVersion), s_apiVersionString);
}

void OnResponseApi(const ResponseAPIMessage *response)
{
    if (response->API == nullptr)
    {
        s_state = SupportState::Failed;
        logger::warn("NirnLabUIPlatform returned a null API pointer; the UIPlatform focus backend stays off");
        return;
    }
    // The two AddOrGetBrowser overloads swap vtable places between API 1.x
    // and 2.0+ (see NirnLabApi.h for the empirically verified layout), so the
    // slot table is picked from the negotiated version, never assumed.
    s_api = response->API;
    const auto slots = SlotsFor(s_apiVersion);
    auto **table = *reinterpret_cast<std::uintptr_t ***>(s_api);
    s_originalAddOrGetBrowser = reinterpret_cast<AddOrGetBrowserFn>(table[slots.addOrGetBrowser]);
    s_originalReleaseBrowserHandle = reinterpret_cast<ReleaseBrowserHandleFn>(table[slots.releaseBrowserHandle]);
    if (!PatchVtableSlot(&table[slots.addOrGetBrowser], reinterpret_cast<std::uintptr_t>(&HookedAddOrGetBrowser),
                         reinterpret_cast<std::uintptr_t>(s_originalAddOrGetBrowser),
                         "IUIPlatformAPI::AddOrGetBrowser") ||
        !PatchVtableSlot(&table[slots.releaseBrowserHandle], reinterpret_cast<std::uintptr_t>(&HookedReleaseBrowserHandle),
                         reinterpret_cast<std::uintptr_t>(s_originalReleaseBrowserHandle),
                         "IUIPlatformAPI::ReleaseBrowserHandle"))
    {
        s_state = SupportState::Failed;
        return;
    }
    if (slots.addOrGetBrowserSettings != 0)
    {
        s_originalAddOrGetBrowserSettings =
            reinterpret_cast<AddOrGetBrowserSettingsFn>(table[slots.addOrGetBrowserSettings]);
        if (!PatchVtableSlot(&table[slots.addOrGetBrowserSettings],
                             reinterpret_cast<std::uintptr_t>(&HookedAddOrGetBrowserSettings),
                             reinterpret_cast<std::uintptr_t>(s_originalAddOrGetBrowserSettings),
                             "IUIPlatformAPI::AddOrGetBrowserSettings"))
        {
            // Unwind the two patches above: a half-hooked API would keep
            // tracking browsers while the backend reports Failed.
            PatchVtableSlot(&table[slots.addOrGetBrowser], reinterpret_cast<std::uintptr_t>(s_originalAddOrGetBrowser),
                            reinterpret_cast<std::uintptr_t>(&HookedAddOrGetBrowser),
                            "IUIPlatformAPI::AddOrGetBrowser (restore)");
            PatchVtableSlot(&table[slots.releaseBrowserHandle],
                            reinterpret_cast<std::uintptr_t>(s_originalReleaseBrowserHandle),
                            reinterpret_cast<std::uintptr_t>(&HookedReleaseBrowserHandle),
                            "IUIPlatformAPI::ReleaseBrowserHandle (restore)");
            s_state = SupportState::Failed;
            return;
        }
    }
    s_state = SupportState::Active;
    logger::info("UIPlatform focus backend installed (IUIPlatformAPI slots {},{}{}); browsers created before this point are not tracked",
                 slots.addOrGetBrowser, slots.releaseBrowserHandle,
                 slots.addOrGetBrowserSettings != 0
                     ? std::string(",") + std::to_string(slots.addOrGetBrowserSettings)
                     : std::string());
}
} // namespace

void InstallMessaging()
{
    if (s_state != SupportState::Pending)
    {
        return;
    }
    if (!Ime::ImeApp::GetInstance().GetSettings().input.meridianSupport)
    {
        s_state = SupportState::Off;
        logger::info("UIPlatform focus backend disabled by configuration");
        return;
    }
    // Without the module the dispatch would only produce a failed-dispatch
    // warning on every NirnLab-less machine.
    if (GetModuleHandleW(L"NirnLabUIPlatform.dll") == nullptr)
    {
        s_state = SupportState::NotDetected;
        logger::info("NirnLabUIPlatform.dll not loaded; the UIPlatform focus backend stays off");
        return;
    }
    // Wildcard listener: it receives every dispatch, including SKSE's own
    // lifecycle messages (filtered by sender below) and the responses, which
    // the host routes back to the requester's dispatch label. This mirrors
    // the host's own controller listener.
    SKSE::GetMessagingInterface()->RegisterListener(nullptr, [](SKSE::MessagingInterface::Message *a_msg) {
        if (a_msg->sender == nullptr || std::strcmp(a_msg->sender, SENDER_LABEL) != 0)
        {
            return;
        }
        if (a_msg->type == MSG_RESPONSE_VERSION && a_msg->dataLen >= sizeof(ResponseVersionMessage))
        {
            OnResponseVersion(reinterpret_cast<const ResponseVersionMessage *>(a_msg->data));
        }
        else if (a_msg->type == MSG_RESPONSE_API && a_msg->dataLen >= sizeof(ResponseAPIMessage))
        {
            if (s_versionAccepted.load())
            {
                OnResponseApi(reinterpret_cast<const ResponseAPIMessage *>(a_msg->data));
            }
        }
    });
    SKSE::GetMessagingInterface()->Dispatch(MSG_REQUEST_VERSION, nullptr, 0, SENDER_LABEL);
    // The dispatch is synchronous: the version verdict is final after this
    // line. A refusal is latched by OnResponseVersion; silence (no responder)
    // is resolved at RequestApi time.
    if (!s_versionAccepted.load() && s_state == SupportState::Pending)
    {
        s_state = SupportState::NotDetected;
        logger::info("NirnLabUIPlatform did not answer the version request; the UIPlatform focus backend stays off");
    }
}

void RequestApi()
{
    if (!s_versionAccepted.load() || s_api != nullptr)
    {
        return;
    }
    if (HasSettingsOverload(s_apiVersion))
    {
        // 2.0+ validate the payload size. The published default settings are
        // passed through: the host initializes from the FIRST requester, and
        // a consumer mod that asked before us has already set its own.
        const RequestAPIMessage request{};
        SKSE::GetMessagingInterface()->Dispatch(MSG_REQUEST_API, const_cast<RequestAPIMessage *>(&request),
                                                sizeof(request), SENDER_LABEL);
    }
    else
    {
        // 1.x takes no payload.
        SKSE::GetMessagingInterface()->Dispatch(MSG_REQUEST_API, nullptr, 0, SENDER_LABEL);
    }
    // Synchronous as well: by this line the hooks are live or the failure is
    // latched in s_state.
    if (s_state == SupportState::Pending)
    {
        s_state = SupportState::Failed;
        logger::warn("NirnLabUIPlatform did not answer the API request; the UIPlatform focus backend stays off");
    }
}

SupportState State()
{
    return s_state;
}

std::string VersionDescription()
{
    return s_libVersionString + " (API " + s_apiVersionString + ")";
}
} // namespace Hooks::NirnLabBridge
