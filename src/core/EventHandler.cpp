#include "core/EventHandler.h"

#include "ImeApp.h"
#include "ImeWnd.hpp"
#include "RE/ControlMap.h"
#include "hooks/PrismaBridge.h"
#include "hooks/ScaleformHook.h"
#include "hooks/SkseMenuFrameworkBridge.h"
#include "ime/ImeController.h"
#include "log.h"
#include "menu/MenuNames.h"
#include "utils/InputFocusAnchor.h"
#include "utils/Utils.h"

#include <RE/B/BSInputDeviceManager.h>
#include <RE/B/BSTEvent.h>
#include <RE/B/ButtonEvent.h>
#include <RE/C/CursorMenu.h>
#include <RE/I/InputDevices.h>
#include <RE/I/InputEvent.h>
#include <RE/M/MainMenu.h>
#include <RE/U/UI.h>

#include <chrono>

namespace Ime::Events
{
class MenuOpenCloseEventSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
{
    using Event = RE::MenuOpenCloseEvent;

public:
    auto ProcessEvent(const Event *a_event, RE::BSTEventSource<Event> * /*a_eventSource*/) -> RE::BSEventNotifyControl override;

private:
    static void FixInconsistentTextEntryCount(const Event *event);
};

/// Engine input stream observer. Scaleform mouse events only reach the top
/// menu (Prisma's kModal FocusMenu under PMCM), so ImeMenu never sees clicks
/// there — this sink watches the same raw button stream PrismaUI's own
/// MouseEventListener feeds its views from, and hands left presses to the
/// Prisma bridge so it can pin the candidate-window field anchor to the
/// click. Cheap gate first: this fires for every input event batch in the
/// game.
class InputEventSink final : public RE::BSTEventSink<RE::InputEvent *>
{
public:
    auto ProcessEvent(RE::InputEvent *const *a_event, RE::BSTEventSource<RE::InputEvent *> * /*a_eventSource*/)
        -> RE::BSEventNotifyControl override
    {
        if (a_event == nullptr || *a_event == nullptr || !Hooks::PrismaBridge::ShouldRoute())
        {
            return RE::BSEventNotifyControl::kContinue;
        }
        for (auto *event = *a_event; event != nullptr; event = event->next)
        {
            const auto *button = event->AsButtonEvent();
            if (button == nullptr || button->GetDevice() != RE::INPUT_DEVICE::kMouse)
            {
                continue;
            }
            if (button->GetIDCode() == 0 && button->IsPressed())
            {
                Hooks::PrismaBridge::NotifyLeftPress();
            }
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};

namespace
{
auto GetMenuOpenCloseEventSink() -> std::unique_ptr<MenuOpenCloseEventSink> &
{
    static std::unique_ptr<MenuOpenCloseEventSink> instance = nullptr;
    return instance;
}

auto GetInputEventSink() -> std::unique_ptr<InputEventSink> &
{
    static std::unique_ptr<InputEventSink> instance = nullptr;
    return instance;
}

void OnFirstOpenMainMenu()
{
    Skyrim::ShowMenu(ImeMenuName);
    ImeController::GetInstance()->ApplySettings();
}

void OnLoadingMenuClose()
{
    Skyrim::ShowMenu(ImeMenuName);
}

void OnSteamOverlayMenu(bool open)
{
    static bool prevModEnabled = false;

    auto *manager = ImeController::GetInstance();
    if (open)
    {
        prevModEnabled = manager->IsModEnabled();
        manager->EnableMod(false);
    }
    else if (prevModEnabled)
    {
        manager->EnableMod(true);
    }
}

/// True when any real (non always-open) menu is on the stack. A legitimate text
/// field always lives inside such a menu. Our own ToolWindowMenu never owns a
/// text-entry count (its text input goes through ImeWnd, not AllowTextInput) —
/// and since it is pushed whenever the language bar is visible, counting it as
/// "real" would blind the leak repair exactly while the leaked state keeps the
/// bar (and thus that menu) alive: a self-sustaining stuck state.
auto HasRealMenuOnStack() -> bool
{
    if (auto *ui = RE::UI::GetSingleton(); ui != nullptr)
    {
        const auto toolWindowMenu = ui->GetMenu(ToolWindowMenuName).get();
        for (const auto &menu : ui->menuStack)
        {
            if (menu && menu.get() != toolWindowMenu && !menu->AlwaysOpen())
            {
                return true;
            }
        }
    }
    return false;
}

/// Decrement the text-entry counter to 0 and re-sync the Scaleform hook's cache.
/// The counter leaks whenever a menu calls AllowTextInput(true) without the
/// matching false (churn around ESC-closes); a leaked counter keeps the IME and
/// the language bar stuck on and desynchronizes every later 0->1 transition.
void HealLeakedTextEntryCount()
{
    auto *controlMap = ControlMap::GetSingleton();
    if (controlMap == nullptr)
    {
        return;
    }
    while (controlMap->GetTextEntryCount() > 0)
    {
        (void)controlMap->SKSE_AllowTextInput(false);
    }
    Hooks::Scaleform::ResetTextEntryCountCache();
    // The SKSE Menu Framework bridge may have owned the (now zeroed) count via
    // its lease. Let it drop its bookkeeping, otherwise its next session would
    // try to release a lease the healer already consumed.
    Hooks::SkseMenuFrameworkBridge::OnTextEntryCountHealed();
}

} // namespace

void InstallEventSinks()
{
    if (auto &menuSink = GetMenuOpenCloseEventSink(); menuSink == nullptr)
    {
        if (auto *ui = RE::UI::GetSingleton(); ui)
        {
            menuSink = std::make_unique<MenuOpenCloseEventSink>();
            ui->AddEventSink(menuSink.get());
        }
    }
    if (auto &inputSink = GetInputEventSink(); inputSink == nullptr)
    {
        if (auto *devices = RE::BSInputDeviceManager::GetSingleton(); devices)
        {
            inputSink = std::make_unique<InputEventSink>();
            devices->AddEventSink(inputSink.get());
        }
    }
}

void UnInstallEventSinks()
{
    if (auto &menuSink = GetMenuOpenCloseEventSink(); menuSink != nullptr)
    {
        if (const auto &ui = RE::UI::GetSingleton(); ui)
        {
            ui->RemoveEventSink(menuSink.get());
        }
        menuSink.reset();
    }
    if (auto &inputSink = GetInputEventSink(); inputSink != nullptr)
    {
        if (auto *devices = RE::BSInputDeviceManager::GetSingleton(); devices)
        {
            devices->RemoveEventSink(inputSink.get());
        }
        inputSink.reset();
    }
}

//////////////////////////////////////////////////////////////////////////
// MenuOpenCloseEventSink
//////////////////////////////////////////////////////////////////////////

// Called from main thread(UI thread).
auto MenuOpenCloseEventSink::ProcessEvent(const Event *event, RE::BSTEventSource<Event> * /*eventSource*/) -> RE::BSEventNotifyControl
{
    logger::debug("Menu {} open {}", event->menuName.c_str(), event->opening);
    InputFocusAnchor::GetInstance().InvalidateCachedMenuIndex();

    static bool firstOpenMainMenu = true;
    // before game load, all menus will be closed;
    if (event->menuName == RE::LoadingMenu::MENU_NAME)
    {
        if (!event->opening)
        {
            OnLoadingMenuClose();
        }
    }
    else if (event->menuName == RE::MainMenu::MENU_NAME)
    {
        if (firstOpenMainMenu && event->opening)
        {
            OnFirstOpenMainMenu();
            firstOpenMainMenu = false;
        }
    }
    else if (event->menuName == RE::ConsoleNativeUIMenu::MENU_NAME) // Steam Overlay
    {
        OnSteamOverlayMenu(event->opening);
    }
    else
    {
        // If some 3rd menu call RE::ControlMap::AllowTextInput but `CursorMenu` hided, this fix may cause IME accident disable.
        if (ImeApp::GetInstance().GetSettings().fixInconsistentTextEntryCount)
        {
            FixInconsistentTextEntryCount(event);
        }
    }
    return RE::BSEventNotifyControl::kContinue;
}

void MenuOpenCloseEventSink::FixInconsistentTextEntryCount(const Event *event)
{
    // fix: if a menu hides but the text-entry counter is still > 0 while no
    // other real (non always-open) menu remains open, some 3rd-party menu
    // called ControlMap::AllowTextInput(true) without the matching false.
    // Treat that leaked count as stale: heal the counter to 0 and disable the
    // IME, otherwise the game keeps swallowing keys ("keyboard still in typing
    // mode") and the language bar stays up.
    if (event->opening || !ControlMap::GetSingleton()->HasTextEntry())
    {
        return;
    }
    // Same SKSEMF exemption as PollTextEntryCountConsistency: the framework
    // owns the count via the bridge lease while never appearing on the menu
    // stack, so this menu-close event must not heal it away.
    if (Hooks::SkseMenuFrameworkBridge::SessionActive())
    {
        return;
    }
    if (auto *ui = RE::UI::GetSingleton(); ui != nullptr)
    {
        // The menu that just closed may still be on the stack when the close
        // event is dispatched (the engine pops it right after), so skip it:
        // otherwise this check bails on the very menu we are reacting to and
        // the leaked text-entry count is never fixed. Our own ToolWindowMenu is
        // skipped too — see HasRealMenuOnStack: it is pushed whenever the
        // language bar is visible and never owns a text-entry count.
        const auto closingMenu    = ui->GetMenu(event->menuName).get();
        const auto toolWindowMenu = ui->GetMenu(ToolWindowMenuName).get();
        for (auto &menu : ui->menuStack)
        {
            // ImeMenu / CursorMenu etc. are always-open; a real menu (other
            // than the one that is closing) still on the stack means the
            // counter may be legitimately owned by it.
            if (menu && menu.get() != closingMenu && menu.get() != toolWindowMenu && !menu->AlwaysOpen())
            {
                return;
            }
        }
    }
    logger::info("No real menu left on the stack but text-entry count is still > 0, healing counter and forcing IME off");
    HealLeakedTextEntryCount();
    const ImeController *manager = ImeController::GetInstance();
    manager->EnableIme(false);
}

void PollTextEntryCountConsistency()
{
    if (!ImeApp::GetInstance().GetSettings().fixInconsistentTextEntryCount)
    {
        return;
    }
    auto *controlMap = ControlMap::GetSingleton();
    if (controlMap == nullptr || !controlMap->HasTextEntry())
    {
        return;
    }
    // The SKSE Menu Framework bridge holds a text-entry lease while its ImGui
    // field is focused — and SKSEMF renders through a D3D hook WITHOUT ever
    // registering an IMenu, so "no real menu on the stack" is its steady
    // state. The leak heuristics below would read that as an ownerless count
    // and heal it every frame, killing the session (observed 2026-10-04:
    // begin/heal oscillating 1137 times in 35s). While the bridge session is
    // active, the lease IS the owner.
    if (Hooks::SkseMenuFrameworkBridge::SessionActive())
    {
        return;
    }
    // Stability grace: ShowMenu is queued — a menu that just called
    // AllowTextInput(true) may not be pushed onto the stack for another frame
    // or two. Only treat the state as leaked once the count has been stable
    // (and no owner menu ever showed up) for a while.
    static std::uint8_t                          s_lastSeenCount = 0;
    static std::chrono::steady_clock::time_point s_lastChange    = std::chrono::steady_clock::now();
    const auto                                   count           = controlMap->GetTextEntryCount();
    if (count != s_lastSeenCount)
    {
        s_lastSeenCount = count;
        s_lastChange    = std::chrono::steady_clock::now();
        return;
    }
    if (std::chrono::steady_clock::now() - s_lastChange < std::chrono::seconds(2))
    {
        return;
    }
    if (HasRealMenuOnStack())
    {
        return;
    }
    // The event-based repair above only runs on menu-close events; when the
    // leaking AllowTextInput(true) lands AFTER its menu's close event (the
    // counter was already 0 at event time), no further event may fire for a
    // long time — the IME and the language bar then stay stuck "on" until the
    // user happens to open another menu. Poll instead and repair here.
    logger::info("Text-entry count {} survived with no menu to own it — healing counter and forcing IME off", count);
    HealLeakedTextEntryCount();
    ImeController::GetInstance()->EnableIme(false);
}
} // namespace Ime::Events
