//
// Created by jamie on 2026/1/2.
//
#include "menu/ToolWindowMenu.h"

#include "ImeApp.h"
#include "ime/ImeController.h"
#include "log.h"
#include "menu/MenuNames.h"
#include "utils/Utils.h"

namespace Ime
{
namespace
{
/// True while this process holds the engine pause for the settings window.
bool s_settingsWindowPauseHeld = false;

auto OnUserEvent(RE::BSUIMessageData *uiMessageData) -> RE::UI_MESSAGE_RESULTS
{
    const RE::UserEvents *userEvents = RE::UserEvents::GetSingleton();

    auto &runtimeData = ImeApp::GetInstance().GetSettings().runtimeData;
    if (uiMessageData->fixedStr == userEvents->cancel)
    {
        runtimeData.requestCloseTopWindow = true;
        return RE::UI_MESSAGE_RESULTS::kHandled;
    }
    return RE::UI_MESSAGE_RESULTS::kPassOn;
}

} // namespace

void ToolWindowMenu::RegisterMenu()
{
    logger::info("Registering ToolWindowMenu...");
    if (auto *ui = RE::UI::GetSingleton(); ui != nullptr)
    {
        ui->Register(ToolWindowMenuName, Creator);
    }
}

auto ToolWindowMenu::ProcessMessage(RE::UIMessage &a_message) -> RE::UI_MESSAGE_RESULTS
{
    if (!ImeApp::GetInstance().GetState().IsInitialized())
    {
        return RE::UI_MESSAGE_RESULTS::kPassOn;
    }
    RE::UI_MESSAGE_RESULTS results = RE::UI_MESSAGE_RESULTS::kPassOn;
    switch (a_message.type.get())
    {
        case RE::UI_MESSAGE_TYPE::kShow: {
            results = RE::UI_MESSAGE_RESULTS::kHandled;
            break;
        }
        case RE::UI_MESSAGE_TYPE::kHide: {
            ImeController::GetInstance()->SyncImeState(); // TODO: really need this?
            if (!ImeApp::GetInstance().SaveSettings())
            {
                logger::error("ToolWindowMenu: settings were not saved on hide");
            }
            results = RE::UI_MESSAGE_RESULTS::kHandled;
            break;
        }
        case RE::UI_MESSAGE_TYPE::kUserEvent: {
            auto *uiMessageData = reinterpret_cast<RE::BSUIMessageData *>(a_message.data);
            if (uiMessageData == nullptr)
            {
                break; // matches the kScaleformEvent null-guard in ImeMenu
            }
            results = OnUserEvent(uiMessageData);
            break;
        }
        default:;
    }
    return results;
}

auto ToolWindowMenu::Creator() -> IMenu *
{
    auto *pMenu = new ToolWindowMenu();
    // Input routing does not need a pause: the MenuMode context ImeMenu pushes
    // while toolWindowShowing is what delivers keys.
    pMenu->menuFlags.set(Flag::kUsesCursor, Flag::kAllowSaving);
    // And this menu must NEVER carry kPausesGame. It is pushed with the LANGUAGE
    // BAR (kUsesCursor is what gives the bar the game cursor), not with the
    // settings window, so the engine — which counts the bit once, at push time —
    // would pause the game every time the bar appears. The bar appears exactly
    // when a mod's text field gains focus (the bridges acquire their lease then),
    // and a pause at that moment tears the host's field down: observed
    // 2026-10-12 with SKSE Menu Framework + its freeze-time option — the field
    // deactivated two frames in and the bar flashed off. The settings-window
    // pause is held on RE::UI::numPausesGame instead, exactly while the window is
    // on screen (SyncSettingsWindowPause).
    pMenu->depthPriority = 11;

    return pMenu;
}

void ToolWindowMenu::SyncSettingsWindowPause(const bool pauseWanted)
{
    auto *ui = RE::UI::GetSingleton();
    if (ui == nullptr)
    {
        return;
    }
    if (pauseWanted)
    {
        if (!s_settingsWindowPauseHeld)
        {
            ++ui->numPausesGame;
            s_settingsWindowPauseHeld = true;
            logger::debug("Settings window pause held (numPausesGame={})", ui->numPausesGame);
        }
        else if (ui->numPausesGame == 0)
        {
            // The engine recounts the counter when the menu stack is rebuilt
            // (save load, game reset), which drops our increment: re-apply it
            // while the window is still up, or the game silently unpauses.
            ++ui->numPausesGame;
        }
        return;
    }
    if (!s_settingsWindowPauseHeld)
    {
        return;
    }
    // Only ever give back what we took: the engine's own menus share this
    // counter, and a decrement past its floor would wrap to ~4 billion and
    // pause the game for good.
    if (ui->numPausesGame > 0)
    {
        --ui->numPausesGame;
    }
    s_settingsWindowPauseHeld = false;
    logger::debug("Settings window pause released (numPausesGame={})", ui->numPausesGame);
}

void ToolWindowMenu::ReleaseSettingsWindowPause()
{
    SyncSettingsWindowPause(false);
}

} // namespace Ime
