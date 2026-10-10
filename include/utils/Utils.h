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

/// 把上屏文字投递给当前的文本目标。
/// @param eraseLetters 组字期间被引擎原样读进字面的可打印字母数，仅在投递走引擎输入
///        队列时先退格抹掉它们（原生 ImGui 界面没有我们能拦截输入的层次）。
void SendUiString(std::wstring_view wstringView, std::size_t eraseLetters = 0);

/// 游戏线程每帧调用：判定当前文本目标是否为"无 Scaleform 字段"的界面。
void PollExternalImGuiSurface();

/// 游戏线程每帧调用：与宿主界面握手（窗口属性，同进程）。
/// - `SimpleIME.Composing`：组字期间存在，值是最后一次刷新的 GetTickCount64()。
///   界面方应在它"新鲜"（<500ms）期间丢弃 ASCII 字符事件——那些字母是输入法的
///   预编辑串，上屏文字由我们另行投递。我们异常退出时属性停止刷新，界面自动恢复。
/// - `SimpleIME.ImeAware`：界面方初始化时挂上，声明它实现了上面的约定；我们随即
///   不再做退格补偿（否则会把玩家真正写下的文字删掉）。
void PublishCompositionState();

/// 组字一开始（以及每次组字内容变化）由文本服务同步调用，IME 线程。
/// 只靠 PostDisplay 的每帧刷新会晚一帧：第一个字母已经作为 CharEvent 被宿主界面
/// 收进字面了。SetPropW 可以跨线程调用，所以直接在组字起点打这个标记。
void MarkCompositionActive();

/// 游戏线程每帧调用：把排队的上屏文字投成引擎输入事件（先退格、再字符，按帧分批）。
void DrainPendingCommittedText();

} // namespace Skyrim
} // namespace Ime
