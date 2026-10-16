//
// Created by jamie on 2025/3/1.
//

#pragma once

#include "RE/GFxCharEvent.h"
#include "core/State.h"
#include "log.h"

#include <cstdint>

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

/// IME 线程调用：提交回调之后本线程上的第一个 WM_CHAR 就是那个提交按键自己的回显，
/// 把它记下来（见 Utils.cpp 的实现注释）。提交键（空格、数字选词）同样会被引擎读成一个
/// CharEvent 漏进宿主字面，而它不在组字串里——退格补偿不算上它，字段里就会剩一个字符
/// （实测 2026-10-12：打 "mei" 按空格提交"没"，字段变成 "m没"）。
void NoteCommitKeyEcho(std::uint32_t charCode);

/// 游戏线程每帧调用：判定当前文本目标是否为"无 Scaleform 字段"的界面。
void PollExternalImGuiSurface();

/// 当前文本目标是否由模组自己的 ImGui 界面持有（`PollExternalImGuiSurface` 的结论）。
/// 设置窗暂停（ToolWindowMenu::SyncSettingsWindowPause）在它成立期间让位：这类界面
/// 把游戏暂停当作"自己已失效"，一暂停就会被宿主关掉。
[[nodiscard]] auto IsExternalImGuiSurface() -> bool;

/// 由 `ControlMap::AllowTextInput` 的 detour 调用：告知是谁把文本输入计数认领/释放了。
/// 认领者模块是泄漏治愈器唯一能用来区分"模组自己的 ImGui 界面持着租约"和"没人认领的
/// 残留计数"的依据——这两类宿主在引擎状态上完全同形（计数 > 0、菜单栈上只剩 always-open
/// 菜单），差别只在认领者是谁。见 IsTextEntryCountOwnedByImGuiHost。
/// @param callerAddress detour 里的 `_ReturnAddress()`，即 AllowTextInput 的调用点
void NoteTextEntryCountChange(const void *callerAddress, bool allow, std::uint8_t count);

/// 当前文本输入计数是否由"菜单栈上没有代表"的声明模组 ImGui 宿主持有（声明表见 Utils.cpp）。
/// 成立时泄漏治愈器不得清零：这类宿主的认领只发生在字段焦点跳变的那一刻，计数被我们清零
/// 后它不会重新认领，字段不重新点开就再也输不进去（SKSEMF 2026-10-04 实测的同一形状）。
[[nodiscard]] auto IsTextEntryCountOwnedByImGuiHost() -> bool;

/// 是否加载了"屏蔽 Caps Lock 大小写切换"的模组（Kill Caps Lock NG / KillCapsLockNG.dll，
/// Nexus 99090；判定见 CharDeliveryLogic::IsCapsBlockingModuleName）。这类模组打掉的是
/// **游戏自己的**大小写翻转指令（实测 AE 1.6.1170 在 `SkyrimSE.exe+0xCDE200` 的 `sete al`），
/// 而输入法掌键盘期间引擎的字符事件被 ImeMenu 吞掉、字母由我们转发（Windows 生成的，
/// 带 Caps 效果），补丁便不再生效——玩家在控制台里一开 Caps Lock 又变大写了。判定成立时
/// 转发路径按同样语义去掉 Caps 效果（保留 Shift），见 ImeWnd::HandleCharMessage。
/// 结果缓存：SKSE 插件在启动期全部加载完毕，首次文本输入远在其后。
[[nodiscard]] auto IsCapsLockToggleBlocked() -> bool;

/// 投递前对 ASCII 字母做大小写归一化：**仅在检测到上面这类模组时**生效
/// （没有它时游戏本身认 Caps，用户可能就想打 "SWORD"）。Shift 取左右两键分别判定，
/// 泛化 VK_SHIFT 在 GetAsyncKeyState 下语义有歧义。两条"按键直通"投递路径共用它：
/// ImeWnd 的 WM_CHAR 转发与 TextStore 的无组字直写投递（Google 日本語入力半角英数），
/// 保证同一份语义——报告者可能只走其中一条。
[[nodiscard]] auto NormalizeCapsCaseIfBlockerLoaded(wchar_t c) -> wchar_t;

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
