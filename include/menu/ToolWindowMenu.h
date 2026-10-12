//
// Created by jamie on 2026/1/2.
//

#pragma once

#include "RE/I/IMenu.h"

namespace Ime
{
//! Menu-stack citizen for the language bar and the settings window: it is what
//! gives the bar the game cursor (kUsesCursor) and what receives the cancel
//! user event (Esc closes the settings window, then the bar). It is pushed
//! whenever the bar is up, so it must never carry kPausesGame — see Creator.
class ToolWindowMenu final : public RE::IMenu
{
public:
    static void RegisterMenu();

    /// Game thread, every frame (ImeMenu::PostDisplay): hold the engine pause
    /// while the settings window is on screen, release it when it closes. The
    /// caller owns the decision (`pauseWanted` = setting && window on screen &&
    /// no host surface owns the keyboard); this owns the counter bookkeeping.
    static void SyncSettingsWindowPause(bool pauseWanted);

    /// Give the pause back on shutdown, so a plugin reload cannot leave the
    /// game paused with no window to explain it.
    static void ReleaseSettingsWindowPause();

    auto ProcessMessage(RE::UIMessage &a_message) -> RE::UI_MESSAGE_RESULTS override;

private:
    static auto Creator() -> IMenu *;
};
} // namespace Ime
