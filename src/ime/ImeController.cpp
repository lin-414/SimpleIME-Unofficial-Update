//
// Created by jamie on 2025/5/6.
//
#include "ime/ImeController.h"

#include "FakeDirectInputDevice.h"
#include "ImeWnd.hpp"
#include "RE/ControlMap.h"
#include "WCharUtils.h"
#include "configs/CustomMessage.h"
#include "hooks/Hooks.hpp"
#include "imguiex/ErrorNotifier.h"
#include "ui/Settings.h"
#include "ui/TaskQueue.h"

namespace Ime
{

void ImeController::ApplySettings()
{
    if (!IsReady())
    {
        ErrorNotifier::GetInstance().Error("Fatal error: IME manager is not initialized.");
        return;
    }
    EnableMod(m_settings->enableMod);

    m_fDirty.store(true);
    if (m_fEnabledMod.load())
    {
        SyncImeStateIfDirty();
    }
}

auto ImeController::EnableMod(bool enable) -> void
{
    if (!IsReady()) return;

    // NOTE: no fast-path dedup here. m_fEnabledMod is updated asynchronously on
    // the IME thread, so a game-thread check races it: in a disable→enable
    // burst (Steam overlay quick toggle) the enable used to read the stale
    // value, decide "no change", and never get queued — leaving the mod
    // disabled until the next unrelated event. Dedup at EXECUTION time instead
    // (the task body), where the flag is authoritative.
    PostToImeThread([this, enable] -> void {
        const bool prev = m_fEnabledMod.load();
        if (prev == enable)
        {
            return; // an already-queued task reached this state — do not re-run
        }
        if (!prev)
        {
            // DoEnableIme refuses to run while the mod flag is false; raise it
            // early so the enable path can proceed.
            m_fEnabledMod.store(true);
        }
        if (IImeModule::IsSuccess(DoEnableMod(enable)))
        {
            m_fEnabledMod.store(enable);
            m_fDirty.store(enable);
            return;
        }
        m_fEnabledMod.store(prev);
        ErrorNotifier::GetInstance().Debug(std::format("Unexpected error: EnableMod({}) failed.", enable));
    });
}

void ImeController::ActivateLangProfile(const GUID &guidProfile) const
{
    if (!IsReady()) return;

    // FIX: capture guidProfile BY VALUE. AddTask defers execution to the IME
    // thread; `[&]` would bind a reference to the caller's GUID, which may
    // already be gone by the time the task runs (use-after-free).
    PostToImeThread([this, guidProfile] -> void {
        ImeWnd *imeWnd = m_imeWnd.load(std::memory_order_acquire);
        if (IsModEnabled() && FAILED(imeWnd->ActivateLanguageProfile(guidProfile)))
        {
            const auto strGuid = WCharUtils::ToString(ToStringFromGUID2(guidProfile));
            ErrorNotifier::GetInstance().Warning(std::format("Can't switch Input Method, profile index {}", strGuid));
        }
    });
}

auto ImeController::CommitCandidate(DWORD index) const -> IImeModule::Result
{
    if (!IsReady()) return IImeModule::Result::DISABLED;

    PostToImeThread([this, index] -> void {
        ImeWnd *imeWnd = m_imeWnd.load(std::memory_order_acquire);
        if (IsModEnabled() && imeWnd != nullptr && !imeWnd->CommitCandidate(index))
        {
            logger::error("CommitCandidate({}) failed on the IME thread", index);
            ErrorNotifier::GetInstance().Warning("Failed to commit the selected candidate");
        }
    });
    return IImeModule::Result::SUCCESS;
}

auto ImeController::SetConversionMode(DWORD conversionMode) const -> void
{
    if (!IsReady()) return;

    PostToImeThread([this, conversionMode] -> void {
        ImeWnd *imeWnd = m_imeWnd.load(std::memory_order_acquire);
        if (IsModEnabled() && imeWnd != nullptr)
        {
            imeWnd->SetConversionMode(conversionMode);
        }
    });
}

void ImeController::EnableIme(bool enable) const
{
    if (!IsReady()) return;

    PostToImeThread([this, enable] -> void {
        if (IImeModule::IsFailed(DoEnableIme(enable)))
        {
            ErrorNotifier::GetInstance().Warning("Unexpected error: EnableIme failed.");
        }
    });
}

void ImeController::ForceFocusIme() const
{
    if (!IsReady()) return;

    PostToImeThread([this] -> void {
        if (IImeModule::IsFailed(DoForceFocusIme()))
        {
            ErrorNotifier::GetInstance().Warning("Unexpected error: ForceFocusIme failed.");
        }
    });
}

void ImeController::SyncImeState()
{
    if (!IsReady()) return;

    PostToImeThread([this] -> void {
        if (IImeModule::IsFailed(DoSyncImeState()))
        {
            ErrorNotifier::GetInstance().Warning("Unexpected error: SyncImeState failed");
        }
    });
}

void ImeController::TryFocusIme() const
{
    if (!IsReady()) return;

    PostToImeThread([this] -> void {
        if (IImeModule::IsFailed(DoTryFocusIme()))
        {
            ErrorNotifier::GetInstance().Warning("Unexpected error: TryFocusIme failed");
        }
    });
}

// FIXME: if some operations failed, SimpleIME may be in an inconsistent state.
auto ImeController::DoEnableMod(const bool enable) -> IImeModule::Result
{
    const bool shouldEnableIme = enable && (m_settings->input.keepImeOpen || ControlMap::GetSingleton()->HasTextEntry());

    // When disabling the mod (e.g. Steam overlay opens, user unticks enableMod),
    // the IME must be turned OFF unconditionally — keepImeOpen must not keep it
    // alive, otherwise the game keeps eating keystrokes while the overlay shows.
    auto result = DoEnableIme(shouldEnableIme, /*honorKeepImeOpen=*/enable);

    bool fResult = IImeModule::IsSuccess(result);
    if (fResult)
    {
        fResult = enable ? SetKeyboardCooperativeLevel(/*restore=*/false) : SetKeyboardCooperativeLevel(/*restore=*/true);
    }
    if (fResult)
    {
        if (enable)
        {
            if (m_gameHIMC == nullptr)
            {
                m_gameHIMC = ImmAssociateContext(m_gameHwnd, nullptr);
            }
            else
            {
                // A previous enable already detached and saved the original
                // HIMC. Re-detaching is harmless, but the second call returns
                // nullptr — storing it (the old behavior) would overwrite the
                // saved original and lose it forever (the disable path would
                // then never restore the game's system IME).
                ImmAssociateContext(m_gameHwnd, nullptr);
            }
        }
        else if (m_gameHIMC != nullptr)
        {
            auto *lastHIMC = ImmAssociateContext(m_gameHwnd, std::exchange(m_gameHIMC, nullptr));
            // Don't assert here (assertions are compiled out of release builds).
            // If another mod changed the game's HIMC while we were disabled,
            // restore the one we saved and warn instead of losing it forever.
            if (lastHIMC != nullptr)
            {
                logger::warn(
                    "Unexpected non-null HIMC ({:p}) when restoring. Another mod may have "
                    "associated a new context while SimpleIME was disabled; re-associating our saved one.",
                    static_cast<void *>(lastHIMC)
                );
            }
        }
    }

    if (!fResult)
    {
        logger::error("Can't enable/disable mod. last error {}", GetLastError());
    }
    return fResult ? IImeModule::Result::SUCCESS : IImeModule::Result::FAILED;
}

auto ImeController::DoEnableIme(const bool enable, const bool honorKeepImeOpen) const -> IImeModule::Result
{
    if (!m_fEnabledMod.load())
    {
        return IImeModule::Result::DISABLED;
    }
    // keepImeOpen means "keep the IME active even when no text entry is open",
    // which is desirable while the mod is enabled (e.g. typing into the
    // console). But an explicit disable request (honorKeepImeOpen=false, from
    // EnableMod(false)) must win — otherwise disabling the mod does nothing.
    const bool target = honorKeepImeOpen && m_settings->input.keepImeOpen ? true : enable;
    const auto result = m_delegate->EnableIme(target);
    if (!IImeModule::IsSuccess(result))
    {
        ErrorNotifier::GetInstance().Warning(std::format("Unexpected error: EnableIme({}) failed.", enable));
    }
    // Note: the language-bar overlay show/hide requests are driven by
    // ImeManager::EnableIme (the single funnel every enable/disable path goes
    // through) — NOT here, since the SyncImeState paths bypass DoEnableIme.
    return result;
}

auto ImeController::DoForceFocusIme() const -> IImeModule::Result
{
    if (!m_fEnabledMod.load())
    {
        return IImeModule::Result::DISABLED;
    }
    const auto result = m_delegate->ForceFocusIme();
    if (!IImeModule::IsSuccess(result))
    {
        ErrorNotifier::GetInstance().Warning("Unexpected error: ForceFocusIme failed");
    }
    return result;
}

auto ImeController::DoTryFocusIme() const -> IImeModule::Result
{
    if (!m_fEnabledMod.load())
    {
        return IImeModule::Result::DISABLED;
    }
    return m_delegate->TryFocusIme();
}

auto ImeController::DoSyncImeState() -> IImeModule::Result
{
    if (!m_fEnabledMod.load())
    {
        return IImeModule::Result::DISABLED;
    }
    const auto result = m_delegate->SyncImeState();
    if (!IImeModule::IsSuccess(result))
    {
        ErrorNotifier::GetInstance().Error("Unexpected error: SyncImeState failed.");
        // Keep the dirty flag set so a later SyncImeStateIfDirty() retries.
        // Clearing it before the delegate call would lose the pending state
        // change forever once the sync fails.
        m_fDirty.store(true);
        return result;
    }
    m_fDirty.store(false);
    return result;
}

/// Hand the game's keyboard back (restore) or take it away (unlock) through
/// the DirectInput shim's cooperative level. The two paths differ only in the
/// DI flags, the log wording and the shim call.
auto ImeController::SetKeyboardCooperativeLevel(const bool restore) const -> bool
{
    const char *const what = restore ? "RestoreKeyboard" : "UnlockKeyboard";
    if (m_gameHwnd == nullptr)
    {
        logger::error("{}: game HWND is null.", what);
        return false;
    }
    // Keep the game window foreground so the DI cooperative-level change
    // applies. Failure here is not fatal (another app may own the foreground
    // lock), so only log.
    if (FALSE == SetForegroundWindow(m_gameHwnd))
    {
        logger::debug("{}: SetForegroundWindow returned FALSE; continuing anyway", what);
    }
    logger::debug(restore ? "Restore keyboard: EXCLUSIVE + FOREGROUND + NOWINKEY." : "Unlock keyboard: NONEXCLUSIVE + BACKGROUND.");
    HRESULT hr = E_FAIL;
    if (auto *keyboard = Hooks::FakeDirectInputDevice::GetInstance(); keyboard != nullptr)
    {
        hr = restore ? keyboard->TryRestoreCooperativeLevel(m_gameHwnd) : keyboard->TryUnlockCooperativeLevel(m_gameHwnd);
    }
    else
    {
        logger::error("{}: FakeDirectInputDevice is not initialized.", what);
    }
    if (FAILED(hr))
    {
        logger::error(restore ? "Failed lock keyboard." : "Failed unlock keyboard.");
    }
    return SUCCEEDED(hr);
}

void ImeController::AddTask(TaskQueue::Task &&task) const
{
    // Copy the window handle before queueing: Shutdown (IME thread) may null
    // m_imeWnd between this load and the PostMessage — the atomic load at
    // least makes the pointer read itself well-defined.
    const ImeWnd *imeWnd = m_imeWnd.load(std::memory_order_acquire);
    const HWND    imeHwnd = (imeWnd != nullptr) ? imeWnd->GetHWND() : nullptr;

    TaskQueue::GetInstance().AddImeThreadTask(std::move(task));

    if (imeHwnd == nullptr || FALSE == PostMessageA(imeHwnd, CM_EXECUTE_TASK, 0, 0))
    {
        logger::error("Failed to post CM_EXECUTE_TASK to ImeWnd.");
    }
}

void ImeController::Init(ImeWnd *imeWnd, HWND gameHwnd, Settings &settings)
{
    if (m_fInited)
    {
        return;
    }
    m_settings = &settings;
    m_delegate = std::make_unique<ImeManager>(gameHwnd, imeWnd, settings);
    m_gameHwnd = gameHwnd;
    m_imeWnd   = imeWnd;
    m_fInited  = true;
}

void ImeController::Shutdown()
{
    // Flip the readiness flag FIRST: game-thread callers check IsReady() before
    // dereferencing the members below (and before AddTask copies the IME window
    // handle), so this must be observed before anything is nulled. Shutdown
    // runs on the IME thread (ImeWnd::OnDestroy) after the task queue was
    // drained there — see ImeWnd::OnDestroy.
    m_fInited  = false;
    m_settings = nullptr;
    m_delegate.reset();
    m_gameHwnd = nullptr;
    m_imeWnd   = nullptr;
}

} // namespace Ime
