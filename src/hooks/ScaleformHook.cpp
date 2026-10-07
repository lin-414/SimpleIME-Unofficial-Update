//
// Created by jamie on 2025/3/2.
//

#include "hooks/ScaleformHook.h"

#include "RE/ControlMap.h"
#include "core/State.h"
#include "hook.h"
#include "hooks/Hooks.hpp"
#include "hooks/MeridianBridge.h"
#include "ime/ImeController.h"
#include "log.h"

#include <atomic>
#include <chrono>
#include <memory>

namespace Hooks::Scaleform
{
namespace
{
constexpr const char *SKSE_ORIGINAL_FN_AllowTextInput = "AllowTextInput";
constexpr const char *SKSE_BACKUP_FN_AllowTextInput   = "_skse_simple_bk_AllowTextInput";

class Scaleform_SetScaleModeTypeHookData : public HookData<void(RE::GFxMovieView *, RE::GFxMovieView::ScaleModeType)>
{
public:
    // NOLINTBEGIN(*-magic-numbers)
    explicit Scaleform_SetScaleModeTypeHookData(func_type *ptr)
        : HookData(
              REL::RelocationID(80302, 82325),        //
              REL::VariantOffset(0x1D9, 0x1DD, 0x00), //
              ptr, true
          )
    {
        if (GetAddress() != 0)
        {
            logger::debug("Installed {}: {}", __func__, ToString());
        }
        else
        {
            logger::error("Failed to install {}: resolved address is 0", __func__);
        }
    }

    // NOLINTEND(*-magic-numbers)
};

class Scaleform_AllowTextInput : public FunctionHook<uint8_t(Ime::ControlMap *, bool)>
{
public:
    // NOLINTBEGIN(*-magic-numbers)
    explicit Scaleform_AllowTextInput(func_type *ptr) : FunctionHook(REL::RelocationID(67252, 68552), ptr)
    {
        if (Detoured())
        {
            logger::debug("Installed {}: {}", __func__, ToString());
        }
    }

    // NOLINTEND(*-magic-numbers)
};

// Timestamp of the last 1->0 (disable) transition, used to debounce the
// enable/disable jitter that ESC produces while a mod window is closing
// (AllowTextInput toggles 0->1->0 in quick succession). Without this, the
// IME is re-enabled (restoring WeChat/Pinyin) right before the final
// disable lands, leaving the system TIP and SimpleIME in a half-cleaned
// state ("still typing after ESC").
std::chrono::steady_clock::time_point g_lastDisableTime{};
// Set when a 0->1 re-enable lands inside the debounce window above.
// DEFERRED, not dropped: the ESC-close churn this debounce targets ends
// with the counter back at 0 (which clears the latch), but a lease that
// STAYS must still enable eventually — the SKSEMF bridge re-acquires one
// frame after its stall watchdog releases, and PMCM-style menus toggle
// AllowTextInput around field focus. Dropping the enable left the IME
// permanently off while the field stayed focused: no further 0->1
// transition ever fires, so nothing retried ("candidate bar flashes once,
// then nothing responds" until the user re-clicked the field). Committed
// by CommitPendingTextEntryEnable from the game thread's frame poll.
std::atomic<bool> g_pendingTextEntryEnable{false};

class SKSE_AllowTextInputFnHandler final : public RE::GFxFunctionHandler
{
    static inline std::uint8_t g_prevTextEntryCount = 0;

public:
    void Call(Params &params) override;

    // call SKSE_AllowTextInput to allow and return its result
    static auto AllowTextInput(bool allow) -> std::uint8_t;
    // use our text-entry-count
    static void OnTextEntryCountChanged(std::uint8_t entryCount);

    // Sync the cached previous count with the game's CURRENT text-entry count.
    // Called right after the hooks are installed: if a text entry is already
    // open at that point (e.g. another menu opened one during plugin load),
    // g_prevTextEntryCount must start from the real value, otherwise the first
    // 1->0 transition would be ignored by the `entryCount == oldValue` guard
    // in OnTextEntryCountChanged and the IME would stay enabled (stuck keys).
    static void SyncBaseline()
    {
        if (auto *controlMap = Ime::ControlMap::GetSingleton(); controlMap != nullptr)
        {
            g_prevTextEntryCount = controlMap->GetTextEntryCount();
        }
    }
};

struct Scaleform_SetScaleModeTypeHook
{
    // This unique_ptr wrap is fine because HookData can't uninstall by itself,
    // and we never uninstall this hook, so no need to worry about the order of static destruction.
    // But we need to promise `SKSE_AllowTextInputFnHandler` can be release because it means ImeApp already released,
    // and the hook won't call `SKSE_AllowTextInputFnHandler::OnTextEntryCountChanged` anymore.
    static inline std::unique_ptr<Scaleform_SetScaleModeTypeHookData> hookData = nullptr;

    static auto FnHandler() -> SKSE_AllowTextInputFnHandler *&
    {
        // Leaked on purpose — movie-created GFx functions may outlive us, so
        // the handler must never be deleted (see the hookData note above).
        // Uninstall() nulls the pointer; the next call re-creates it so a
        // re-install after a failed init gets a live handler back.
        static SKSE_AllowTextInputFnHandler *fnHandler = nullptr;
        if (fnHandler == nullptr)
        {
            fnHandler = new SKSE_AllowTextInputFnHandler();
        }
        return fnHandler;
    }

    static auto SetScaleModeType(RE::GFxMovieView *pMovieView, RE::GFxMovieView::ScaleModeType scaleMode)
    {
        hookData->Original(pMovieView, scaleMode);

        if (pMovieView == nullptr || FnHandler() == nullptr)
        {
            return;
        }

        RE::GFxValue skse;
        if (!pMovieView->GetVariable(&skse, "_global.skse") || !skse.IsObject())
        {
            logger::error("Can't get _global.skse");
            return;
        }
        if (skse.HasMember(SKSE_BACKUP_FN_AllowTextInput))
        {
            return;
        }

        RE::GFxValue skse_fn_AllowTextInput;
        if (skse.GetMember(SKSE_ORIGINAL_FN_AllowTextInput, &skse_fn_AllowTextInput))
        {
            skse.SetMember(SKSE_BACKUP_FN_AllowTextInput, skse_fn_AllowTextInput);

            RE::GFxValue fn_AllowTextInput;
            pMovieView->CreateFunction(&fn_AllowTextInput, FnHandler());
            skse.SetMember(SKSE_ORIGINAL_FN_AllowTextInput, fn_AllowTextInput);

            logger::debug(
                "Successfully hooked skse.AllowTextInput for movie: {}",
                pMovieView->GetMovieDef() != nullptr ? pMovieView->GetMovieDef()->GetFileURL() : "Unknown"
            );
        }
    }

    // The detour goes live inside the ctor, before hookData is assigned — safe
    // only because Install() runs on the main thread during startup, before the
    // hooked function can fire concurrently.
    static void Install()
    {
        // A live trampoline hook cannot be re-created; after an Uninstall only
        // the handler is missing, so a re-install (D3DInit retry) restores it.
        if (hookData == nullptr)
        {
            hookData = std::make_unique<Scaleform_SetScaleModeTypeHookData>(SetScaleModeType);
        }
    }

    static void Uninstall() { FnHandler() = nullptr; }
};

struct Scaleform_AllowTextInputHook
{
    static inline std::unique_ptr<Scaleform_AllowTextInput> hookData = nullptr;

    static auto AllowTextInput(Ime::ControlMap *self, bool allow)
    {
        logger::debug("Scaleform_AllowTextInputHook");
        auto result = hookData->Original(self, allow);

        SKSE_AllowTextInputFnHandler::OnTextEntryCountChanged(result);
        // A Meridian UI focusing its DOM text field calls AllowTextInput(true):
        // probe the field right away so the DOM leak baseline (and the candidate
        // panel anchor) exists before the user's first keystroke — at
        // view-focus time the field is not focused yet and the probe reports
        // no-field.
        if (allow && MeridianBridge::HasFocus())
        {
            MeridianBridge::RequestCapture();
        }
        return result;
    }

    // Re-entrant: Uninstall reset the hook, a re-install (D3DInit retry)
    // recreates it.
    static void Install() { hookData = std::make_unique<Scaleform_AllowTextInput>(AllowTextInput); }

    static void Uninstall() { hookData.reset(); }
};

} // namespace

auto SKSE_AllowTextInputFnHandler::AllowTextInput(bool allow) -> std::uint8_t
{
    const std::uint8_t entryCount = Ime::ControlMap::GetSingleton()->SKSE_AllowTextInput(allow);
    OnTextEntryCountChanged(entryCount);
    logger::trace("Text entry count: {}", g_prevTextEntryCount);
    return g_prevTextEntryCount;
}

void SKSE_AllowTextInputFnHandler::OnTextEntryCountChanged(std::uint8_t entryCount)
{
    const uint8_t oldValue = g_prevTextEntryCount;
    logger::trace("OnTextEntryCountChanged: prev {}, curr {}", oldValue, entryCount);
    if (entryCount == oldValue)
    {
        return;
    }

    g_prevTextEntryCount = entryCount;
    auto *imeManager     = Ime::ImeController::GetInstance();
    if (oldValue == 0)
    {
        // Debounce: ESC-closing a mod window can toggle 0->1->0 within a few
        // milliseconds (menu stack churn). Re-enabling in between restores the
        // Chinese TIP right before the final disable lands, leaving a messy
        // half-cleaned state. 50ms only eats that churn — a human re-opening
        // another text field takes far longer, so their enable still runs.
        const auto elapsed = std::chrono::steady_clock::now() - g_lastDisableTime;
        if (elapsed < std::chrono::milliseconds(50))
        {
            logger::info(
                "Text entry re-enabled {}ms after disable, deferring the IME enable to the frame poll",
                std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
            );
            g_pendingTextEntryEnable.store(true, std::memory_order_release);
            // Return BEFORE SyncImeStateIfDirty: a dirty flag, if any, survives
            // and a later sync trigger retries — by design.
            return;
        }
        imeManager->SyncImeStateIfDirty();
        imeManager->EnableIme(true);
    }
    else if (entryCount == 0)
    {
        // The churn the debounce defers against resolved to "closed": the
        // deferred enable must not fire.
        g_pendingTextEntryEnable.store(false, std::memory_order_release);
        g_lastDisableTime = std::chrono::steady_clock::now();
        imeManager->SyncImeStateIfDirty();
        imeManager->EnableIme(false);
    }
}

void SKSE_AllowTextInputFnHandler::Call(Params &params)
{
    if (params.argCount < 1)
    {
        logger::error("AllowInput called with insufficient args");
        return;
    }
    auto      *fxMovieView = reinterpret_cast<RE::GFxMovieView *>(params.movie);
    const bool enable      = params.args[0].GetBool(); // NOLINT(*-pro-bounds-pointer-arithmetic)

    RE::GFxValue skse;
    bool         calledOriginal = false;
    if (fxMovieView->GetVariable(&skse, "_global.skse") && skse.IsObject())
    {
        RE::GFxValue backupFn;
        if (skse.GetMember(SKSE_BACKUP_FN_AllowTextInput, &backupFn))
        {
            RE::GFxValue result; // this is AS return value, meaningless.
            calledOriginal = skse.Invoke(SKSE_BACKUP_FN_AllowTextInput, &result, params.args, params.argCount);

            if (calledOriginal)
            {
                const auto entryCount = Ime::ControlMap::GetSingleton()->GetTextEntryCount();
                OnTextEntryCountChanged(entryCount);
            }
            logger::trace("Called backup skse fn AllowTextInput.");
        }
    }
    else
    {
        if (!skse.IsObject())
        {
            logger::warn("Already installed SKSE extension function: AllowTextInput, but _global.skse missing!");
        }
    }
    if (!calledOriginal)
    {
        AllowTextInput(enable);
    }
}

void Install()
{
    Scaleform_SetScaleModeTypeHook::Install();
    Scaleform_AllowTextInputHook::Install();
    // Make sure the cached text-entry count starts from the real value, so a
    // text entry that is already open when we load is not mistaken for a fresh
    // 0->1 transition (and its eventual close is not swallowed).
    SKSE_AllowTextInputFnHandler::SyncBaseline();
}

void ResetTextEntryCountCache()
{
    // The counter was corrected outside the hook (leak repair in EventHandler);
    // re-sync the cached previous value so the next transition detects properly.
    SKSE_AllowTextInputFnHandler::SyncBaseline();
}

void CommitPendingTextEntryEnable()
{
    // Game thread, every frame: commit an enable that landed inside the
    // re-enable debounce window and was deferred rather than dropped. Once the
    // window expires, the FINAL stable state wins: if the counter is still >0
    // the enable runs (respecting every gate inside EnableIme — Prisma
    // avoidance, keepImeOpen, state dedup); if the churn ended closed the
    // latch was already cleared by the disable transition.
    if (!g_pendingTextEntryEnable.load(std::memory_order_acquire))
    {
        return;
    }
    if (std::chrono::steady_clock::now() - g_lastDisableTime < std::chrono::milliseconds(50))
    {
        return; // still inside the churn window; stay latched for a later frame
    }
    if (!g_pendingTextEntryEnable.exchange(false, std::memory_order_acq_rel))
    {
        return;
    }
    auto *controlMap = Ime::ControlMap::GetSingleton();
    if (controlMap == nullptr || !controlMap->HasTextEntry())
    {
        logger::info("Deferred text-entry enable dropped: counter no longer open");
        return;
    }
    if (!Ime::Core::State::GetInstance().Has(Ime::Core::State::IME_DISABLED))
    {
        return; // already enabled; nothing deferred remains
    }
    logger::info("Deferred IME enable committed after the re-enable debounce window");
    Ime::ImeController::GetInstance()->EnableIme(true);
}

void Uninstall()
{
    Scaleform_AllowTextInputHook::Uninstall();
    Scaleform_SetScaleModeTypeHook::Uninstall();
}
} // namespace Hooks::Scaleform
