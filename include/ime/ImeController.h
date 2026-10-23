#pragma once

#include "IImeModule.h"
#include "ime/ImeManager.h"
#include "ime/InputMethodChoice.h"
#include "ui/Settings.h"
#include "ui/TaskQueue.h"

#include <atomic>
#include <mutex>

namespace Ime
{

class ImeWnd;

struct SettingsConfig;

class ImeController final
{
public:
    void ApplySettings();

    void SaveSettings(Settings &settings) const
    {
        if (IsReady())
        {
            settings.enableMod = m_fEnabledMod.load();
        }
    }

    void SyncImeStateIfDirty()
    {
        if (m_fDirty.load())
        {
            m_fDirty.store(false);
            SyncImeState();
        }
    }

    void MarkDirty() { m_fDirty.store(true); }

    auto IsReady() const -> bool { return m_fInited.load() && (m_imeWnd.load(std::memory_order_acquire) != nullptr); }

    auto IsModEnabled() const -> bool { return m_fEnabledMod.load(); }

    /// Whether disabling the IME must move the session to the English keyboard
    /// profile (see Settings::switchEnglishLayoutOnDisable). The game-thread
    /// WndProc watchdog reads it to tell "drifted away from the English layout
    /// we deliberately set" apart from "the user's own input method, which we
    /// never switched away and must not fight".
    [[nodiscard]] auto SwitchesEnglishLayoutOnDisable() const -> bool
    {
        return m_settings != nullptr && m_settings->switchEnglishLayoutOnDisable;
    }

    /// No-op unless the controller is ready — the shared shape of every
    /// bridge's "re-evaluate the IME decision" hop.
    auto SyncImeStateIfReady() -> void
    {
        if (IsReady())
        {
            SyncImeState();
        }
    }

    /**
     * notify @c ImeWnd activate a @c LangProfile by specify guid.
     * @param guidProfile the @c LangProfile guid
     */
    void ActivateLangProfile(const GUID &guidProfile) const;
    /// Activate a profile by its position in the language bar's list. Keyboard
    /// layouts all share GUID_NULL, so an index is the only unambiguous handle
    /// for them (see InputMethodManager::ActivateProfileByIndex).
    void ActivateLangProfileByIndex(std::uint32_t index) const;
    /// Hand the user's [core] default_input_method to the IME thread's copy.
    /// The value is captured by value: Settings stays render-thread-owned.
    void SetDefaultInputMethod(std::string defaultInputMethod, bool reapplyOnEnable) const;
    /// Ask the IME thread to re-enumerate the selectable input methods and
    /// publish the result through @c GetInputMethodChoices. Dispatched, not
    /// waited on: the settings window draws the previous snapshot meanwhile.
    void RequestInputMethodChoices() const;
    /// Thread-safe snapshot of the last enumeration (empty before the first
    /// request completes, or when the process has no input method to offer).
    [[nodiscard]] auto GetInputMethodChoices() const -> std::vector<InputMethodChoice>;
    /// The actual commit runs on the IME thread; a failure there is reported
    /// through the ErrorNotifier. SUCCESS here means the task was dispatched.
    auto CommitCandidate(DWORD index) const -> IImeModule::Result;
    auto SetConversionMode(DWORD conversionMode) const -> void;
    /// Ask the IME to move its candidate window by `delta` pages (+1 next, -1
    /// previous). Dispatched to the IME thread; the request itself is
    /// best-effort (a TIP may refuse the forwarded key).
    auto ChangeCandidatePage(int delta) const -> void;

    //////////////////////////////////////////////////

    auto EnableIme(bool enable) const -> void;
    auto ForceFocusIme() const -> void;
    auto SyncImeState() -> void;
    auto TryFocusIme() const -> void;
    auto EnableMod(bool enable) -> void;

    static auto GetInstance() -> ImeController *
    {
        static ImeController g_instance;
        return &g_instance;
    }

    void Init(ImeWnd *imeWnd, HWND gameHwnd, Settings &settings);
    void Shutdown();

private:
    Settings                   *m_settings    = nullptr;
    std::unique_ptr<ImeManager> m_delegate    = nullptr;
    /// Atomic because the game thread reads it in AddTask while Shutdown
    /// (IME thread) nulls it — a torn pointer read would be UB. The ImeWnd
    /// object itself outlives every reader (member of ImeApp); only the
    /// pointer write needs synchronizing.
    std::atomic<ImeWnd *>       m_imeWnd      = nullptr;
    HWND                        m_gameHwnd    = nullptr;
    HIMC                        m_gameHIMC    = nullptr;
    std::atomic_bool            m_fDirty      = false;
    std::atomic_bool            m_fEnabledMod = false;
    std::atomic_bool            m_fInited     = false;
    /// Guards m_choices: written on the IME thread, read by the render thread
    /// (and the game thread, should a bridge ever ask) — never shared otherwise.
    mutable std::mutex          m_choicesMutex;
    /// Mutable so the const request method can publish on the IME thread.
    mutable std::vector<InputMethodChoice> m_choices; ///< guarded by m_choicesMutex

    auto DoEnableMod(bool enable) -> IImeModule::Result;
    auto DoEnableIme(bool enable, bool honorKeepImeOpen = true) const -> IImeModule::Result;
    auto DoForceFocusIme() const -> IImeModule::Result;
    auto DoSyncImeState() -> IImeModule::Result;
    auto DoTryFocusIme() const -> IImeModule::Result;

    auto SetKeyboardCooperativeLevel(bool restore) const -> bool;

    void AddTask(TaskQueue::Task &&task) const;

public:
    /// Queue `body` on the IME thread with the standard readiness re-check:
    /// Shutdown may null the members between queueing (any thread) and
    /// execution (IME thread). Public wrappers keep their own outer IsReady()
    /// gate — failing fast without a queue round-trip.
    ///
    /// Public for components that own their own IME-thread-affine objects: the
    /// TSF text service defers work with it (re-activating the input profile
    /// out of a compartment notification must not re-enter TSF from inside the
    /// notification). AddTask above stays private — it is the raw enqueue,
    /// without the readiness check.
    template <typename Body>
    void PostToImeThread(Body &&body) const
    {
        AddTask([this, body = std::forward<Body>(body)]() -> void {
            if (!IsReady()) return; // may run after Shutdown nulls the members
            body();
        });
    }
};
} // namespace Ime
