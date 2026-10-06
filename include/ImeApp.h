//
// Created by jamie on 25-1-22.
//
#pragma once

#include "ImeWnd.hpp"
#include "hook.h"
#include "ui/Settings.h"

namespace Ime
{
void D3DInit();

class ImeApp
{
public:
    struct State
    {
        enum class StateKey : std::uint8_t
        {
            UNINITIALIZED,
            INITIALIZING,
            INITIALIZED,
            INITIALIZE_FAILED,
            SHUTDOWN,
            DORMANCY
        };

    private:
        std::atomic<StateKey> m_stateKey = StateKey::UNINITIALIZED;

    public:
        static constexpr auto GetStateKetText(StateKey stateKey) -> std::string
        {
            switch (stateKey)
            {
                case StateKey::INITIALIZED:
                    return "Initialized";
                case StateKey::INITIALIZING:
                    return "Initializing";
                case StateKey::INITIALIZE_FAILED:
                    return "Initialization failed";
                case StateKey::UNINITIALIZED:
                    return "Uninitialized";
                case StateKey::SHUTDOWN:
                    return "Shutdown";
                case StateKey::DORMANCY:
                    return "Dormancy";
            }
            return "Unknown state";
        }

        constexpr auto GetStateKetText() const -> std::string { return GetStateKetText(m_stateKey); }

        void SetState(const StateKey stateKey)
        {
            // CAS loop instead of check-then-exchange: two racing SetState
            // calls must each atomically decide forward-vs-rollback. With the
            // old code, a state advanced between the check and the exchange
            // was silently overwritten (or a stale rollback logged as fine).
            StateKey current = m_stateKey.load(std::memory_order_acquire);
            while (current < stateKey)
            {
                if (m_stateKey.compare_exchange_weak(current, stateKey, std::memory_order_acq_rel, std::memory_order_acquire))
                {
                    return;
                }
                // CAS failed: `current` holds the latest value, loop re-checks.
                // Losing to a concurrent forward move is a benign no-op.
            }
            logger::error("The state cannot be rolled back from [{}] to [{}]!", GetStateKetText(current), GetStateKetText(stateKey));
        }

        constexpr auto IsUnInitialized() const { return m_stateKey == StateKey::UNINITIALIZED; }

        constexpr auto IsInitializing() const { return m_stateKey == StateKey::INITIALIZING; }

        constexpr auto IsInitializeFailed() const { return m_stateKey == StateKey::INITIALIZE_FAILED; }

        constexpr auto IsInitialized() const { return m_stateKey == StateKey::INITIALIZED; }
    };

    explicit ImeApp();
    ~ImeApp() = default;

    ImeApp(const ImeApp &other)                   = delete;
    ImeApp(ImeApp &&other)                        = delete;
    auto operator=(const ImeApp &other) -> ImeApp = delete;
    auto operator=(ImeApp &&other) -> ImeApp      = delete;

    static auto GetInstance() -> ImeApp &;

    void OnInputLoaded();
    void Draw();
    void Uninitialize();
    void SaveSettings();

    /// Fallback frame driver for Meridian sessions: the engine stops calling
    /// ImeMenu::PostDisplay (the normal ImGui frame source) while a Meridian
    /// view holds focus, so the swapchain present — which still fires every
    /// frame — drives the frame, the bridge tick and the candidate panel.
    /// Installed together with the other hooks; a no-op unless the PostDisplay
    /// path has gone silent (frame token) and a Meridian session is active.
    static void PresentHook(std::uint32_t a_unk);

    /// The real IDXGISwapChain::Present. PrismaUI renders its Ultralight views
    /// from a present CALL-SITE hook that always runs after our PostDisplay
    /// overlay, so during a Prisma takeover session their views cover the
    /// candidate window no matter what we do at the call site. Drawing here —
    /// into the backbuffer right before the actual flip — is the only point
    /// guaranteed to be after every such draw. Driven only while a Prisma view
    /// holds input (ShouldRoute); everything else keeps the normal layering.
    static auto SwapChainPresentHook(void *swapChain, std::uint32_t syncInterval, std::uint32_t flags) -> long;

    constexpr auto GetGameHWND() const -> HWND { return m_hWnd; }

    constexpr auto GetImeWnd() -> ImeWnd & { return m_imeWnd; }

    constexpr auto GetState() const -> const State & { return m_state; }

    constexpr auto GetSettings() const -> const Settings & { return m_settings; }

    constexpr auto GetSettings() -> Settings & { return m_settings; }

private:
    void OnD3DInit();
    void Start(const RE::BSGraphics::RendererData &renderData);
    void Shutdown();

    static void InstallHooks();
    static void UninstallHooks();

    Settings m_settings;
    ImeWnd   m_imeWnd{};
    HWND     m_hWnd = nullptr;
    /// Thread id of the IME message-loop worker (set by the worker itself on
    /// start). ImeApp::Shutdown posts WM_QUIT to it — a window-targeted
    /// WM_QUIT reaches the WndProc instead, which ignores it, and the loop
    /// (which only exits on a thread-queue WM_QUIT) would keep running.
    std::atomic<DWORD> m_imeThreadId{0};
    /// Set by the IME worker when its full teardown (WM_DESTROY → OnDestroy →
    /// UnInitialize) completed. Shutdown waits for it (bounded) so the
    /// game-thread Uninitialize does not race the IME-thread teardown.
    std::atomic<bool> m_imeTeardownDone{false};
    State              m_state;

    friend void           Ime::D3DInit();
    void                  DoD3DInit();
    static auto           MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT;
    static inline WNDPROC RealWndProc;
};
} // namespace Ime
