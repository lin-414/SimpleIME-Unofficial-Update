//
// Created by jamie on 2025/3/1.
//

#pragma once

#include "RE/GFxCharEvent.h"
#include "core/State.h"
#include "log.h"

namespace Ime
{
inline auto align_to(int value, int alignment) -> int
{
    return ((value + alignment - 1) / alignment) * alignment;
}

/// Committed-text strip list shared by the Scaleform and WM_CHAR/ImGui commit
/// routes: grave would toggle the console when echoed back, and the middle dot
/// is the CJK list separator the engine treats as a hotkey. (Meridian's DOM
/// route deliberately does NOT strip — see MeridianBridge::QueueText.)
inline bool ShouldStripCommittedChar(const wchar_t c)
{
    return c == L'`' || c == L'·';
}

namespace Skyrim
{
inline void ShowMenu(const RE::BSFixedString &a_menuName)
{
    if (auto *const messageQueue = RE::UIMessageQueue::GetSingleton(); messageQueue != nullptr)
    {
        messageQueue->AddMessage(a_menuName, RE::UI_MESSAGE_TYPE::kShow, nullptr);
    }
}

inline void HideMenu(const RE::BSFixedString &a_menuName)
{
    if (auto *const messageQueue = RE::UIMessageQueue::GetSingleton(); messageQueue != nullptr)
    {
        messageQueue->AddMessage(a_menuName, RE::UI_MESSAGE_TYPE::kHide, nullptr);
    }
}

void SendUiString(std::wstring_view wstringView);

} // namespace Skyrim
} // namespace Ime
