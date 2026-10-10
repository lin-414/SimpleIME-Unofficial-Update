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
    // Input routing does not need the pause: the MenuMode context ImeMenu pushes
    // while toolWindowShowing is what delivers keys, and the pause was itself the
    // documented cause of swallowed key-up events (see HealStuckShortcutKeys).
    pMenu->menuFlags.set(Flag::kUsesCursor, Flag::kAllowSaving);
    // Opt-in only. RE::UI::numPausesGame is what host-side liveness watchdogs read,
    // so a paused game closes mod menus that treat it as focus loss (Tailor 3.x's
    // native ImGui UI among them). ImeMenu::PostDisplay keeps this bit in step with
    // the setting while the menu is off the stack, so the panel switch is live from
    // the next open; this initial value only covers the very first push.
    if (ImeApp::GetInstance().GetSettings().input.pauseGameWhileSettingsOpen)
    {
        pMenu->menuFlags.set(Flag::kPausesGame);
    }
    pMenu->depthPriority = 11;

    return pMenu;
}

} // namespace Ime
