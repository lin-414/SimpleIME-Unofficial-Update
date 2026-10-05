//
// Prisma UI avoidance bridge.
//
// Prisma-based UIs (e.g. Outfit Wheeler) run on Ultralight and implement
// their own IME handling: when one of their text fields takes the keyboard,
// Prisma associates its own IME context with the game window and posts the
// `PrismaUI.ImeAssociation` registered message. SimpleIME must stand down
// while that is happening, or the two IMEs fight over the keyboard.
//
// Coexistence rules:
//   * only the PUBLIC IVPrismaUI1 interface is negotiated, and only its
//     HasAnyActiveFocus() query is used — no private vtables;
//   * if PrismaUI.dll is present but the V1 interface cannot be negotiated,
//     fail safe: treat Prisma as owning the keyboard permanently (SimpleIME's
//     IME stays off) rather than risk double handling;
//   * HasAnyActiveFocus may wait on Ultralight — it is only ever called from
//     the game thread's frame path (Refresh), never from a window procedure.
//
#include "hooks/PrismaBridge.h"

#include "ImeApp.h"
#include "ime/ImeController.h"
#include "log.h"
#include "PrismaUI/PrismaUI_API.h"

#include <Windows.h>

#include <atomic>

namespace Hooks::PrismaBridge
{
namespace
{
constexpr auto        ASSOCIATION_MESSAGE_NAME = L"PrismaUI.ImeAssociation";
constexpr std::uint64_t REFRESH_INTERVAL_MS     = 500;

PRISMA_UI_API::IVPrismaUI1 *s_api          = nullptr;
bool                        s_unavailable  = false; ///< Prisma present but V1 unsupported -> fail safe
unsigned                    s_associationMessage = 0;
std::atomic<bool>           s_hasActiveFocus{false};
std::atomic<bool>           s_associationLatch{false};
std::atomic<bool>           s_enabled{false}; ///< config gate, latched at Install
std::atomic<SupportState>   s_state{SupportState::Pending}; ///< install outcome, for the settings UI
std::atomic<std::uint64_t>  s_lastRefreshMs{0};
} // namespace

void Install()
{
    s_api = nullptr;
    s_unavailable = false;
    s_hasActiveFocus = false;
    s_associationLatch = false;
    s_enabled = Ime::ImeApp::GetInstance().GetSettings().input.prismaAvoidance;
    if (!s_enabled.load())
    {
        s_state = SupportState::Off;
        logger::info("Prisma avoidance disabled by configuration");
        return;
    }
    s_associationMessage = RegisterWindowMessageW(ASSOCIATION_MESSAGE_NAME);
    const HMODULE module = GetModuleHandleW(L"PrismaUI.dll");
    if (module == nullptr)
    {
        s_state = SupportState::NotDetected;
        logger::info("PrismaUI.dll not loaded; nothing to avoid");
        return;
    }
    // Negotiate the documented interface version (not a framework release
    // number): Prisma returns nullptr if V1 is unsupported.
    const auto request =
        reinterpret_cast<PRISMA_UI_API::RequestPluginAPIFunc>(GetProcAddress(module, "RequestPluginAPI"));
    s_api = request != nullptr ? static_cast<PRISMA_UI_API::IVPrismaUI1 *>(
                                     request(PRISMA_UI_API::InterfaceVersion::V1))
                               : nullptr;
    s_unavailable    = s_api == nullptr;
    s_hasActiveFocus = s_unavailable; // fail safe: assume Prisma owns the keyboard
    s_state          = s_unavailable ? SupportState::Failed : SupportState::Active;
    if (s_unavailable)
    {
        logger::warn(
            "PrismaUI.dll is present but its public V1 API is unavailable; SimpleIME will stay out of its way "
            "(update PrismaUI to restore SimpleIME input)");
    }
    else
    {
        logger::info("Prisma V1 focus query ready (read-only avoidance)");
    }
}

SupportState State()
{
    return s_state.load(std::memory_order_acquire);
}

void Uninstall()
{
    s_api       = nullptr;
    s_enabled   = false;
    s_hasActiveFocus   = false;
    s_associationLatch = false;
}

void Refresh()
{
    if (!s_enabled.load() || s_api == nullptr)
    {
        return;
    }
    const std::uint64_t now = GetTickCount64();
    if (now - s_lastRefreshMs.load() < REFRESH_INTERVAL_MS)
    {
        return;
    }
    s_lastRefreshMs = now;
    const bool focus = s_unavailable || s_api->HasAnyActiveFocus();
    s_hasActiveFocus = focus;
    const bool latch = s_associationLatch.exchange(false);
    if (!focus && latch)
    {
        // The association handshake latched us off, but Prisma no longer has
        // any focused view: re-evaluate the IME decision (a keepImeOpen user
        // gets the IME back without touching anything).
        logger::info("Prisma released the keyboard, re-syncing IME state");
        if (auto *controller = Ime::ImeController::GetInstance(); controller->IsReady())
        {
            controller->SyncImeState();
        }
    }
}

void OnAssociationMessage()
{
    if (!s_enabled.load())
    {
        return;
    }
    s_associationLatch = true;
    logger::info("PrismaUI associated its IME context; standing down");
    // Take the focus decision immediately instead of waiting for the next
    // Refresh: Prisma is about to own the keyboard.
    s_hasActiveFocus = true;
    if (auto *controller = Ime::ImeController::GetInstance(); controller->IsReady())
    {
        controller->SyncImeState();
    }
}

bool OwnsInput()
{
    return s_enabled.load() && (s_unavailable || s_hasActiveFocus.load() || s_associationLatch.load());
}

unsigned AssociationMessage()
{
    return s_associationMessage;
}
} // namespace Hooks::PrismaBridge
