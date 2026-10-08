#include "ime/ImeManager.h"

#include "FakeDirectInputDevice.h"
#include "ImeWnd.hpp"
#include "RE/ControlMap.h"
#include "hooks/MeridianBridge.h"
#include "hooks/PrismaBridge.h"
#include "imguiex/ErrorNotifier.h"
#include "log.h"

#include <processthreadsapi.h>
#include <utility>
#include <windows.h>

namespace Ime
{

auto ImeManager::Focus(const HWND hwnd) -> bool
{
    auto        hwndThread      = GetWindowThreadProcessId(hwnd, nullptr);
    const DWORD currentThreadId = GetCurrentThreadId();
    bool        success         = true;
    if (hwndThread != currentThreadId)
    {
        success = AttachThreadInput(hwndThread, currentThreadId, TRUE) != FALSE;
        if (success)
        {
            SetFocus(hwnd);
            // Always detach, even if SetFocus throws or returns early.
            AttachThreadInput(hwndThread, currentThreadId, FALSE);
        }
    }
    else
    {
        SetFocus(hwnd);
    }
    return success;
}

auto ImeManager::ForceEnglishKeyboardOnGameThread() const -> void
{
    // TSF profile activation and ActivateKeyboardLayout(KLF_SETFORPROCESS)
    // (both done in ActivateKeyboardEng on the IME thread) never change the
    // layout of the already-running game main thread: keyboard layouts are
    // per-thread, and keystrokes for the game window are translated on the
    // thread that owns it. AttachThreadInput does not help either — it only
    // shares focus/capture state, not layouts. The documented way to switch
    // another thread's layout is to ask the window itself: DefWindowProc
    // handles WM_INPUTLANGCHANGEREQUEST by activating the layout on the
    // owning thread (the same thing the taskbar language bar does). The game
    // window is foreground at this point (the disable path restored focus),
    // and our hooked MainWndProc forwards the message to the original
    // WndProc/DefWindowProc, so this reaches the game thread.
    if (m_gameHwnd == nullptr)
    {
        return;
    }
    const DWORD gameThreadId = GetWindowThreadProcessId(m_gameHwnd, nullptr);
    if (gameThreadId == 0)
    {
        return;
    }
    // Load (without activating — ActivateKeyboardEng already activated it on
    // the IME thread) to get a valid HKL. "00000409" is the en-US layout,
    // present on practically every system; it is installed if missing.
    const HKL hklEnglish = LoadKeyboardLayout(L"00000409", 0);
    if (hklEnglish == nullptr)
    {
        logger::warn("ForceEnglishKeyboardOnGameThread: LoadKeyboardLayout failed, error {}", GetLastError());
        return;
    }
    const auto layoutBefore = GetKeyboardLayout(gameThreadId);
    if (layoutBefore == hklEnglish)
    {
        logger::debug("Game thread ({}) already uses the English layout {:p}", gameThreadId, static_cast<void *>(hklEnglish));
        return;
    }
    if (PostMessageW(m_gameHwnd, WM_INPUTLANGCHANGEREQUEST, 0, reinterpret_cast<LPARAM>(hklEnglish)) == FALSE)
    {
        logger::warn("ForceEnglishKeyboardOnGameThread: PostMessage(WM_INPUTLANGCHANGEREQUEST) failed, error {}", GetLastError());
        return;
    }
    logger::info(
        "Requested English layout {:p} for the game thread {} (was {:p}); WM_INPUTLANGCHANGE in the log confirms the switch",
        static_cast<void *>(hklEnglish),
        gameThreadId,
        static_cast<void *>(layoutBefore)
    );
}

auto ImeManager::EnableIme(bool enable) -> Result
{
    logger::debug("ImeManager::{} {}", __func__, enable ? "enable" : "disable");

    // Consume the force flag up front: the early returns below must not leave
    // it armed, or the next sync of ANY state would force-run the whole body —
    // including the disable path's AbortIme + English-profile + focus dance.
    const bool forceUpdate = std::exchange(m_isForceUpdate, false);

    // Prisma UI coordination. When a Prisma view (PMCM, Outfit Wheeler, ...)
    // holds input capture, SimpleIME TAKES OVER text entry: the enable below
    // moves the Win32 focus to our ImeWnd (the candidate window shows, the
    // composition runs on our TSF document), committed text is delivered
    // through the game window's WM_CHAR stream (PrismaUI's subclass feeds the
    // focused view — see PrismaBridge::QueueText), and the disable path
    // returns the focus so Prisma's own associated IME context serves any
    // further native typing. The two pipelines never hold the keyboard at the
    // same time. The one remaining stand-down is the fail-safe: PrismaUI is
    // present but its public V1 API failed to negotiate — there is then no
    // commit channel into its views, so letting it keep the keyboard (native
    // composition, visible only through the view's own IME UI if it renders
    // one) beats fighting it blind.
    if (enable && m_settings.input.prismaAvoidance && Hooks::PrismaBridge::IsUnavailable())
    {
        logger::info("IME enable suppressed: PrismaUI's V1 API is unavailable; its native IME pipeline keeps the keyboard");
        return Result::SUCCESS;
    }

    // Do nothing if caller want to disable IME.
    if (enable)
    {
        if (const auto result = TryFocusIme(); IImeModule::IsFailed(result))
        {
            ErrorNotifier::GetInstance().Error("Can't focus to IME thread. IME can't work!");
            return result;
        }
    }

    auto &state = State::GetInstance();
    if (forceUpdate || (state.Has(State::IME_DISABLED) && enable) || (state.NotHas(State::IME_DISABLED) && !enable))
    {

        // Drive the language-bar overlay from THIS single funnel: every
        // enable/disable path (text-entry count hook, WM_NCACTIVATE sync,
        // FixInconsistentTextEntryCount, mod toggle, ToolWindowMenu close) ends up
        // here. The requests used to live in ImeController::DoEnableIme, which the
        // SyncImeState paths bypass — a disable following an enable (the ESC-close
        // enable/disable churn) could then leave the overlay stuck visible with the
        // pausing ToolWindowMenu shown: language bar stuck on screen and the game
        // paused ("keys do not respond"). Writes happen on the IME thread, which is
        // the documented writer of these runtime fields.
        if (m_settings.appearance.autoToggleLanguageBar)
        {
            if (enable)
            {
                m_settings.runtimeData.requestShowOverlay = true;
                m_settings.runtimeData.requestHideOverlay = false;
            }
            else
            {
                m_settings.runtimeData.requestShowOverlay = false;
                m_settings.runtimeData.requestHideOverlay = true;
            }
        }

        const bool success = enable ? DoEnable() : DoDisable();

        if (m_settings.autoToggleKeyboard)
        {
            logger::debug("Toggle the keyboard state...");
            m_imeWnd->ToggleKeyboard(enable);
        }

        if (!success)
        {
            state.Set(State::IME_DISABLED);
            logger::error("Enable IME failed! last error {}", GetLastError());
        }

        return ToResult(success);
    }
    return Result::SUCCESS;
}

/// The enable arm of EnableIme: clear IME_DISABLED, focus the TSF document
/// and restore the input method the user was on (or pick the user's own IME
/// when nothing was remembered — see the branch comments).
auto ImeManager::DoEnable() -> bool
{
    auto &state   = State::GetInstance();
    bool  success = false;
    logger::info("IME enabled: clearing IME_DISABLED, focusing TSF");
    state.Clear(State::IME_DISABLED);
    success = m_imeWnd->FocusTextService(true);
    // Restore the input method that was active before IME was disabled.
    // Without this, the system TIP stays on the English keyboard after
    // EnableIme(false) switched it away, and typing in a text field
    // would be stuck in English mode.
    if (m_lastActiveProfile != GUID_NULL)
    {
        if (FAILED(m_imeWnd->ActivateLanguageProfile(m_lastActiveProfile)))
        {
            logger::warn("Failed to restore input method profile after enabling IME");
        }
        m_lastActiveProfile = GUID_NULL;
    }
    else if (m_imeWnd->GetActiveLangProfile().dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR)
    {
        // A real TIP is already active on the IME thread (e.g. the
        // user's IME, activated by an earlier fallback this session):
        // keep it instead of re-activating the same profile on every
        // forced sync (WM_NCACTIVATE etc.).
        logger::debug("No input method remembered, but a TIP is already active; keeping it");
    }
    else
    {
        // Nothing was remembered (fresh session: the boot-time
        // WM_NCACTIVATE churn disables the IME before the user ever
        // typed with a Chinese TIP, so the sink never captured one).
        // The current TIP is then usually the English keyboard left by
        // the earlier disable — silently keeping it made the first
        // enable of a session type English only (no composition, no
        // candidate window), e.g. right after a Meridian view gained
        // focus. Fall back to the user's own IME from the enumeration.
        if (FAILED(m_imeWnd->ActivatePreferredImeProfile()))
        {
            logger::debug("No input method remembered and no non-English TIP available, keeping the current one");
        }
    }
    return success;
}

/// The disable arm of EnableIme: abort the composition, clear TSF focus,
/// hand the Win32 focus back to the game window and switch the system TIP
/// to the English keyboard (WASD must reach the game raw).
auto ImeManager::DoDisable() -> bool
{
    auto &state   = State::GetInstance();
    bool  success = false;
    logger::info("IME disabled: setting IME_DISABLED, clearing TSF focus");
    // Explicitly abort any active composition/candidate UI BEFORE clearing
    // TSF focus. This is the same cleanup the status bar click performs
    // (ImeMenu::OnMouseEvent -> AbortIme): without it, EnableIme(false)
    // relied solely on the TSF composition-end callback chain, which does
    // not reliably fire when WeChat/Microsoft Pinyin own the composition,
    // leaving the candidate window up and IN_COMPOSING set ("still typing"
    // after ESC). SendNotifyMessage to our own IME window dispatches
    // synchronously, so m_textService->AbortIme() runs inline here.
    m_imeWnd->AbortIme();
    state.Set(State::IME_DISABLED);
    success = m_imeWnd->FocusTextService(false);
    // Return the Win32 focus to the game window. EnableIme(true) always
    // focuses the ImeWnd via TryFocusIme(); the disable path must be
    // symmetric, otherwise the hidden 0x0 ImeWnd keeps the keyboard
    // focus and the game never receives WM_CHAR again ("keyboard stuck").
    Focus(m_gameHwnd);

    // The system input method (e.g. WeChat/Microsoft Pinyin) follows the
    // Win32 focus back to the game window and intercepts WASD/keys there,
    // popping its candidate window or swallowing input. Switch the system
    // TIP back to the English keyboard so the game receives raw keys. The
    // profile switch (TF_IPPMF_FORPROCESS) only affects this process; the
    // user's global input method is untouched. InputMethodManager's
    // OnActivated callback will clear IN_COMPOSING/IN_CAND_CHOOSING as a
    // side effect, which also fixes a stuck typing state after closing a
    // mod window while composing. Uses the real English keyboard profile
    // from the TSF enumeration (with actual CLSID/GUID), not the stub
    // DEFAULT_LANG_PROFILE which may fail to activate.
    {
        // Remember the IME the user was using, so EnableIme(true) can restore it.
        // Use the last real TIP seen by the activation sink instead of the
        // active-profile cache: after a previous disable the cache points at
        // the English keyboard (GUID_NULL), and a second disable in the same
        // close (count churn / WM_NCACTIVATE sync) would then save nothing,
        // silently losing the restore (observed in the game log as an
        // "IME enabled" with no profile restore following it).
        const GUID lastTipGuid = m_imeWnd->GetLastTipProfileGuid();
        if (lastTipGuid != GUID_NULL)
        {
            m_lastActiveProfile = lastTipGuid;
        }
        if (FAILED(m_imeWnd->ActivateEnglishProfile()))
        {
            logger::warn("Failed to switch back to the English keyboard after disabling IME");
        }
        // ActivateKeyboardLayout(KLF_SETFORPROCESS) inside ActivateEnglishProfile
        // only affects the IME thread; ask the game window to switch its own
        // thread's layout (its keyboard state is what the game window uses).
        ForceEnglishKeyboardOnGameThread();
    }
    return success;
}

auto ImeManager::ForceFocusIme() -> Result
{
    logger::debug("ImeManager::{}", __func__);
    m_imeWnd->Focus();
    return Result::SUCCESS;
}

auto ImeManager::SyncImeState() -> Result
{
    logger::debug("ImeManager::{}", __func__);
    m_isForceUpdate = true;
    return EnableIme(IsShouldEnableIme());
}

auto ImeManager::TryFocusIme() -> Result
{
    logger::debug("ImeManager::{}", __func__);

    // FIX: promote the game's TOP-LEVEL window to foreground first.
    // SetCooperativeLevel(NONEXCLUSIVE|BACKGROUND) fails silently
    // (GetLastError() == 0) unless the target window belongs to the
    // foreground process. SetForegroundWindow promotes it, so the
    // AttachThreadInput / SetFocus in Focus() actually take effect.
    // (Focus() itself is static and only receives the hidden WS_CHILD ImeWnd
    // handle, so the foreground promotion must happen here.)
    // Note: SetForegroundWindow may legitimately return FALSE (e.g. when
    // another process owns the foreground lock) - that is not fatal by
    // itself, so we proceed but log it for diagnosability.
    if (FALSE == SetForegroundWindow(m_gameHwnd))
    {
        logger::debug("SetForegroundWindow(game {:p}) returned FALSE; continuing anyway", static_cast<void *>(m_gameHwnd));
    }

    return ToResult(Focus(m_imeWnd->GetHWND()));
}

auto ImeManager::IsShouldEnableIme() const -> bool
{
    // A focused Meridian view (CEF) is a text-input target exactly like a
    // Scaleform text entry: the bridge observes Meridian's public focus API
    // and this atomic is updated from its TryFocus/Unfocus vtable hooks.
    // A focused Prisma view (PMCM, Outfit Wheeler, ...) likewise: SimpleIME
    // has TAKEN OVER text entry for it (the takeover triggers on the focus
    // transition, because PMCM never leases the text-entry counter), and
    // every WM_NCACTIVATE-style sync must not kill that session just because
    // the counter is closed.
    return m_settings.input.keepImeOpen || ControlMap::GetSingleton()->HasTextEntry() ||
           (m_settings.input.meridianSupport && Hooks::MeridianBridge::HasFocus()) ||
           (m_settings.input.prismaAvoidance && Hooks::PrismaBridge::ShouldRoute());
}

} // namespace Ime
