#pragma once

#include "IImeModule.h"
#include "core/State.h"
#include "ui/Settings.h"

#include <windows.h>

namespace Ime
{

class ImeWnd;

class ImeManager final : public IImeModule
{
    using State = Core::State;
    HWND            m_gameHwnd;
    ImeWnd         *m_imeWnd;
    bool            m_isForceUpdate;
    Settings       &m_settings; ///< non-const: EnableIme writes runtimeData overlay requests (IME thread only, by design).
    GUID            m_lastActiveProfile{GUID_NULL}; ///< the IME profile active before disable, restored on re-enable

public:
    ImeManager(HWND hwnd, ImeWnd *imeWnd, Settings &settings) : m_gameHwnd(hwnd), m_imeWnd(imeWnd), m_isForceUpdate(false), m_settings(settings) {}

    ~ImeManager() override              = default;
    ImeManager(const ImeManager &other) = delete;
    ImeManager(ImeManager &&other)      = delete;

    auto EnableIme(bool enable) -> Result override;
    auto ForceFocusIme() -> Result override;
    auto SyncImeState() -> Result override;
    /// <summary>
    /// Try to focus IME when mod is enabled
    /// </summary>
    /// <returns>true if mod enabled and focus success, otherwise false</returns>
    auto TryFocusIme() -> Result override;

    auto GetGameHwnd() const -> HWND { return m_gameHwnd; }

    static auto Focus(HWND hwnd) -> bool;

private:
    auto IsShouldEnableIme() const -> bool;
    auto ForceEnglishKeyboardOnGameThread() const -> void;
};
} // namespace Ime
