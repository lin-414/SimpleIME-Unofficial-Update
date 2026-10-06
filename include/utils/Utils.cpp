//
// Created by jamie on 2026/3/11.
//
#include "Utils.h"

#include "ImeApp.h"
#include "RE/B/BSTDerivedCreator.h"
#include "RE/B/BSUIScaleformData.h"
#include "RE/G/GFxEvent.h"
#include "RE/GFxCharEvent.h"
#include "RE/I/InterfaceStrings.h"
#include "RE/U/UIMessageQueue.h"
#include "hooks/MeridianBridge.h"
#include "hooks/PrismaBridge.h"
#include "hooks/SkseMenuFrameworkBridge.h"
#include "menu/MenuNames.h"

namespace Ime::Skyrim
{
namespace
{
constexpr auto ASCII_GRAVE_ACCENT = 0x60U; // `
constexpr auto ASCII_MIDDLE_DOT   = 0xB7U; // ·

using ScaleformMessageCreator = RE::BSTDerivedCreator<RE::BSUIScaleformData, RE::IUIMessageData>;

auto Send(RE::BSUIScaleformData *scaleformData, const uint32_t code, RE::UIMessageQueue *messageQueue, const RE::BSFixedString &menuName) -> bool
{
    auto *charEvent               = new GFxCharEvent(code);
    scaleformData->scaleformEvent = charEvent;

    logger::debug("send code {:#x} to Skyrim", code);
    messageQueue->AddMessage(menuName, RE::UI_MESSAGE_TYPE::kScaleformEvent, scaleformData);
    return true;
}
} // namespace

void SendUiString(std::wstring_view wstringView)
{
    if (wstringView.empty()) return;

    // While our own ToolWindow (the settings overlay) is showing, it is the
    // text target: the bridge routes below would steal its keystrokes into an
    // underlying mod view (PMCM search box, Meridian DOM field, SKSEMF ImGui
    // field). Fall through to the Scaleform fallback, which feeds ImeMenu's
    // ImGui via its own char events. The flag is written on the render thread
    // and read here on the IME thread; a one-frame staleness only misroutes a
    // keystroke across an open/close boundary, which is unobservable.
    const bool toolWindowShowing = ImeApp::GetInstance().GetSettings().runtimeData.toolWindowShowing;

    if (!toolWindowShowing)
    {
        // Meridian views (CEF) live outside the menu stack: GFx char events never
        // reach their DOM fields. While a Meridian view is focused, hand the text
        // to the bridge instead — it queues here (IME thread) and commits into
        // the focused DOM field from the game thread's frame tick.
        if (Hooks::MeridianBridge::ShouldRoute())
        {
            Hooks::MeridianBridge::QueueText(wstringView);
            return;
        }

        // Prisma views (Ultralight — PMCM, Outfit Wheeler, ...) live outside the
        // menu stack too, and their fields are fed exclusively by PrismaUI's
        // game-window subclass reading the WM_CHAR stream. While a Prisma view
        // owns input, post the committed text there: the subclass queues it into
        // the view (surrogate recombination included), Win32 focus notwithstanding.
        if (Hooks::PrismaBridge::ShouldRoute())
        {
            Hooks::PrismaBridge::QueueText(wstringView);
            return;
        }

        // SKSE Menu Framework fields are ImGui, not Scaleform: same routing idea,
        // but the injection happens inside the framework's own render-event
        // callback (the thread its ImGui frames run on).
        if (Hooks::SkseMenuFrameworkBridge::ShouldRoute())
        {
            Hooks::SkseMenuFrameworkBridge::QueueText(wstringView);
            return;
        }
    }

    auto             *messageQueue  = RE::UIMessageQueue::GetSingleton();
    const auto *const strings       = RE::InterfaceStrings::GetSingleton();
    auto             *msgFactoryMgr = RE::MessageDataFactoryManager::GetSingleton();
    if (strings == nullptr || messageQueue == nullptr || msgFactoryMgr == nullptr)
    {
        logger::warn("Can't send string to Skyrim. May game already closed?");
        return;
    }
    const auto *scaleformDataCreator = msgFactoryMgr->GetCreator<RE::BSUIScaleformData>(strings->bsUIScaleformData);
    if (scaleformDataCreator == nullptr)
    {
        logger::warn("Unexpected error. Can't create scaleform message data creator!");
        return;
    }

    for (const wchar_t c : wstringView)
    {
        const auto wcharCode = static_cast<uint32_t>(c);
        if (wcharCode == ASCII_GRAVE_ACCENT || wcharCode == ASCII_MIDDLE_DOT)
        {
            continue;
        }
        auto *scaleformData = scaleformDataCreator->Create();
        if (scaleformData != nullptr)
        {
            Send(scaleformData, wcharCode, messageQueue, ImeMenuName);
        }
        else
        {
            logger::error("Unexpected error. Can't create scaleform message data!");
            break;
        }
    }
}

} // namespace Ime::Skyrim
