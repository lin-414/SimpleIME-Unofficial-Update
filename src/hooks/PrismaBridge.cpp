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
#include "core/State.h"
#include "ime/ImeController.h"
#include "log.h"
#include "PrismaUI/PrismaUI_API.h"
#include "RE/C/CursorMenu.h"
#include "RE/M/MenuCursor.h"
#include "RE/U/UI.h"

#include <Windows.h>

#include <atomic>
#include <bit>

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
std::atomic<HWND>           s_gameHwnd{nullptr}; ///< set by ImeWnd once created; the WM_CHAR commit target
std::atomic<std::uint64_t>  s_fieldAnchor{0}; ///< both coordinates bit-packed (X low, Y high); see UpdateFieldAnchor
std::atomic<bool>           s_fieldAnchorValid{false};
std::atomic<bool>           s_anchorPinned{false}; ///< a click was observed on the view: the anchor IS the field, stop tracking the cursor

/// Track the engine cursor into the field anchor — but only while the engine
/// cursor menu is open and keeping MenuCursor fresh (Prisma's FocusMenu sets
/// kUsesCursor, so this holds for PMCM-style takeovers). Reading the
/// singleton without that guard would write a stale position, which is worse
/// than no anchor: a valid garbage anchor suppresses every fallback.
void TrackFieldAnchorFromCursor()
{
    auto *ui = RE::UI::GetSingleton();
    if (ui == nullptr || !ui->IsMenuOpen(RE::CursorMenu::MENU_NAME))
    {
        return;
    }
    if (const auto *cursor = RE::MenuCursor::GetSingleton(); cursor != nullptr)
    {
        if (!s_fieldAnchorValid.load(std::memory_order_acquire))
        {
            logger::info(
                "Prisma field anchor starts tracking the cursor at ({:.0f},{:.0f})",
                cursor->cursorPosX,
                cursor->cursorPosY);
        }
        UpdateFieldAnchor(cursor->cursorPosX, cursor->cursorPosY);
    }
}

/// Per-frame anchor tracking for a live Prisma session, before the first
/// observed click. The one-shot seed this replaces captured the cursor once
/// at focus-gain — for PMCM that is the pre-open mouse position
/// (HasAnyActiveFocus flips before the user has steered the cursor onto a
/// field), and the click refresh in ImeMenu::OnMouseEvent cannot repair it
/// because the engine routes Scaleform mouse events to the top menu, where
/// Prisma's FocusMenu (kModal, equal depth priority) takes them. Tracking
/// every frame means the anchor is wherever the user is pointing when a
/// composition starts — good enough while no click has been observed (e.g. a
/// field the menu auto-focused). Once NotifyLeftPress pins the anchor to a
/// click, tracking stands down: the click point IS the field, and the mouse
/// wandering off afterwards must not drag the anchor away with it. Paused
/// while a composition is showing so the value under the candidate window's
/// appearance-frame capture cannot drift mid-typing; that capture is
/// one-shot anyway (m_caretAnchorLocked), so tracking never chases the
/// cursor beneath a showing candidate window. PrismaUI itself draws its
/// cursor sprite and feeds its views from this same MenuCursor position
/// (ViewRenderer::DrawCursor / InputHandler::MouseEventListener), so the
/// coordinate spaces agree by construction.
void TrackFieldAnchor()
{
    if (!ShouldRoute() || s_anchorPinned.load(std::memory_order_acquire) ||
        Ime::Core::State::GetInstance().IsImeInputting())
    {
        return;
    }
    TrackFieldAnchorFromCursor();
}
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
    // Cheap per-frame work first, ahead of the throttled focus query: the
    // field anchor must reflect the cursor at composition start, which can
    // land inside a single 500ms focus-query window.
    TrackFieldAnchor();

    const std::uint64_t now = GetTickCount64();
    if (now - s_lastRefreshMs.load() < REFRESH_INTERVAL_MS)
    {
        return;
    }
    s_lastRefreshMs = now;
    const bool focus = s_unavailable || s_api->HasAnyActiveFocus();
    const bool previous = s_hasActiveFocus.exchange(focus, std::memory_order_acq_rel);
    const bool latch = s_associationLatch.exchange(false);
    if (!focus && latch)
    {
        // The association handshake latched us off, but Prisma no longer has
        // any focused view: re-evaluate the IME decision (a keepImeOpen user
        // gets the IME back without touching anything).
        logger::info("Prisma released the keyboard, re-syncing IME state");
        Ime::ImeController::GetInstance()->SyncImeStateIfReady();
    }
    if (focus != previous)
    {
        // Focus transition — THE takeover trigger. PMCM and friends never lease
        // the text-entry counter (no AllowTextInput call of their own was ever
        // observed), so the counter path cannot start a session here; a view
        // gaining/losing focus is the only reliable signal that a Prisma UI
        // took (or released) the keyboard. SyncImeState consults ShouldRoute()
        // (see IsShouldEnableIme): focus gained -> the IME takes over; focus
        // lost -> the counter (closed) turns it back off.
        logger::info("Prisma view focus {} — re-evaluating IME state", focus ? "GAINED" : "lost");
        if (!focus)
        {
            // Session over: drop the anchor (and its click pin) so the next
            // session re-seeds.
            s_fieldAnchorValid.store(false, std::memory_order_release);
            s_anchorPinned.store(false, std::memory_order_release);
        }
        Ime::ImeController::GetInstance()->SyncImeStateIfReady();
    }
}

void OnAssociationMessage(bool associated)
{
    if (!s_enabled.load())
    {
        return;
    }
    if (!associated)
    {
        // Prisma disassociated its context (input capture released). Re-evaluate
        // instead of latching: the counter path and the next Refresh decide.
        logger::info("PrismaUI disassociated its IME context; re-evaluating IME state");
        Ime::ImeController::GetInstance()->SyncImeStateIfReady();
        return;
    }
    s_associationLatch = true;
    logger::info("PrismaUI associated its IME context; taking over text entry");
    // Take the focus decision immediately instead of waiting for the next
    // Refresh: Prisma is about to own the keyboard.
    s_hasActiveFocus = true;
    Ime::ImeController::GetInstance()->SyncImeStateIfReady();
}

bool OwnsInput()
{
    return s_enabled.load() && (s_unavailable || s_hasActiveFocus.load() || s_associationLatch.load());
}

bool IsUnavailable()
{
    return s_unavailable;
}

bool ShouldRoute()
{
    return s_enabled.load() && !s_unavailable &&
           (s_hasActiveFocus.load(std::memory_order_acquire) || s_associationLatch.load(std::memory_order_acquire));
}

void QueueText(std::wstring_view text)
{
    if (text.empty())
    {
        return;
    }
    const HWND hwnd = s_gameHwnd.load(std::memory_order_acquire);
    if (hwnd == nullptr)
    {
        logger::warn("Prisma commit route dropped {} unit(s): game window not known yet", text.size());
        return;
    }
    std::size_t posted = 0;
    for (const wchar_t c : text)
    {
        // Same strip list as the other commit routes: grave would toggle the
        // console when echoed back, and the middle dot is the CJK list
        // separator the engine treats as a hotkey.
        constexpr wchar_t GRAVE_ACCENT = L'`';
        constexpr wchar_t MIDDLE_DOT   = L'·';
        if (c == GRAVE_ACCENT || c == MIDDLE_DOT)
        {
            continue;
        }
        // PrismaUI's game-window subclass turns WM_CHAR payloads (its own
        // surrogate recombination included) into text inside the focused view.
        // Posting works regardless of which window holds the Win32 focus,
        // which matters: while our IME composes, that is ImeWnd.
        if (PostMessageW(hwnd, WM_CHAR, static_cast<WPARAM>(c), 0) != FALSE)
        {
            ++posted;
        }
    }
    logger::info("Prisma commit route: posted {} of {} unit(s) as WM_CHAR", posted, text.size());
}

void SetGameHwnd(HWND hwnd)
{
    s_gameHwnd.store(hwnd, std::memory_order_release);
}

void UpdateFieldAnchor(float x, float y)
{
    // One atomic holds both coordinates (X in the low 32 bits, Y in the high
    // 32): the game thread writes while the render thread reads, and a split
    // store could hand a reader a new X paired with an old Y for a frame.
    const auto packed = static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(x)) |
                        (static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(y)) << 32);
    s_fieldAnchor.store(packed, std::memory_order_release);
    s_fieldAnchorValid.store(true, std::memory_order_release);
}

bool GetFieldAnchor(float &x, float &y)
{
    if (!s_fieldAnchorValid.load(std::memory_order_acquire))
    {
        return false;
    }
    const auto packed = s_fieldAnchor.load(std::memory_order_acquire);
    x = std::bit_cast<float>(static_cast<std::uint32_t>(packed));
    y = std::bit_cast<float>(static_cast<std::uint32_t>(packed >> 32));
    return true;
}

void NotifyLeftPress()
{
    // Game thread, from the input-event sink. Scaleform mouse events never
    // reach ImeMenu under PMCM-style takeovers (the engine routes them to the
    // top menu, Prisma's kModal FocusMenu), so the engine input stream is the
    // only click observation we get — the same stream Prisma's own
    // MouseEventListener feeds its views from. A left press while a Prisma
    // view owns input is the strongest field proxy there is (the user just
    // clicked where they are about to type), so pin the anchor to it: the
    // per-frame cursor tracking stands down until the session ends, and the
    // mouse wandering off after the click cannot drag the anchor away.
    if (!s_enabled.load() || s_unavailable || !ShouldRoute())
    {
        return;
    }
    if (Ime::Core::State::GetInstance().IsImeInputting())
    {
        // A press during composition goes to the candidate window (selection)
        // or aborts it — either way it must not move the field anchor onto
        // the candidate window.
        return;
    }
    auto *ui = RE::UI::GetSingleton();
    if (ui == nullptr || !ui->IsMenuOpen(RE::CursorMenu::MENU_NAME))
    {
        return; // MenuCursor would be stale — see TrackFieldAnchorFromCursor
    }
    const auto *cursor = RE::MenuCursor::GetSingleton();
    if (cursor == nullptr)
    {
        return;
    }
    const bool firstPin = !s_anchorPinned.load(std::memory_order_acquire);
    UpdateFieldAnchor(cursor->cursorPosX, cursor->cursorPosY);
    s_anchorPinned.store(true, std::memory_order_release);
    if (firstPin)
    {
        logger::info(
            "Prisma field anchor pinned to left press at ({:.0f},{:.0f})",
            cursor->cursorPosX,
            cursor->cursorPosY);
    }
}

unsigned AssociationMessage()
{
    return s_associationMessage;
}
} // namespace Hooks::PrismaBridge
