//
// ImGui input-state repairs, moved out of ImeWnd.cpp so the window procedure /
// theme / rendering / debug overlay TU stops growing: these are frame drivers
// over ImGui's io and the physical keyboard, not ImeWnd state (zero member
// references in the moved block).
//
#include "ui/ImGuiInputState.h"

#include "RE/C/CursorMenu.h"
#include "RE/U/UI.h"
#include "log.h"

#include <windows.h>

namespace Ime
{
namespace
{
//! Win32 virtual key for an ImGui named key, for the physical-keyboard
//! cross-check in HealStuckShortcutKeys. Returns 0 for keys with no single VK
//! (mouse, gamepad, reserved mod aliases, keypad equal).
auto ImGuiKeyToVirtualKey(ImGuiKey key) -> UINT
{
    switch (key)
    {
        case ImGuiKey_Tab: return VK_TAB;
        case ImGuiKey_LeftArrow: return VK_LEFT;
        case ImGuiKey_RightArrow: return VK_RIGHT;
        case ImGuiKey_UpArrow: return VK_UP;
        case ImGuiKey_DownArrow: return VK_DOWN;
        case ImGuiKey_PageUp: return VK_PRIOR;
        case ImGuiKey_PageDown: return VK_NEXT;
        case ImGuiKey_Home: return VK_HOME;
        case ImGuiKey_End: return VK_END;
        case ImGuiKey_Insert: return VK_INSERT;
        case ImGuiKey_Delete: return VK_DELETE;
        case ImGuiKey_Backspace: return VK_BACK;
        case ImGuiKey_Space: return VK_SPACE;
        case ImGuiKey_Enter: return VK_RETURN;
        case ImGuiKey_Escape: return VK_ESCAPE;
        case ImGuiKey_Menu: return VK_APPS;
        case ImGuiKey_Apostrophe: return VK_OEM_7;
        case ImGuiKey_Comma: return VK_OEM_COMMA;
        case ImGuiKey_Minus: return VK_OEM_MINUS;
        case ImGuiKey_Period: return VK_OEM_PERIOD;
        case ImGuiKey_Slash: return VK_OEM_2;
        case ImGuiKey_Semicolon: return VK_OEM_1;
        case ImGuiKey_Equal: return VK_OEM_PLUS;
        case ImGuiKey_LeftBracket: return VK_OEM_4;
        case ImGuiKey_Backslash: return VK_OEM_5;
        case ImGuiKey_RightBracket: return VK_OEM_6;
        case ImGuiKey_GraveAccent: return VK_OEM_3;
        case ImGuiKey_CapsLock: return VK_CAPITAL;
        case ImGuiKey_ScrollLock: return VK_SCROLL;
        case ImGuiKey_NumLock: return VK_NUMLOCK;
        case ImGuiKey_PrintScreen: return VK_SNAPSHOT;
        case ImGuiKey_Pause: return VK_PAUSE;
        case ImGuiKey_KeypadDecimal: return VK_DECIMAL;
        case ImGuiKey_KeypadDivide: return VK_DIVIDE;
        case ImGuiKey_KeypadMultiply: return VK_MULTIPLY;
        case ImGuiKey_KeypadSubtract: return VK_SUBTRACT;
        case ImGuiKey_KeypadAdd: return VK_ADD;
        case ImGuiKey_KeypadEnter: return VK_RETURN;
        case ImGuiKey_AppBack: return VK_BROWSER_BACK;
        case ImGuiKey_AppForward: return VK_BROWSER_FORWARD;
        case ImGuiKey_Oem102: return VK_OEM_102;
        default: break;
    }
    if (key >= ImGuiKey_A && key <= ImGuiKey_Z) return static_cast<UINT>('A' + (key - ImGuiKey_A));
    if (key >= ImGuiKey_0 && key <= ImGuiKey_9) return static_cast<UINT>('0' + (key - ImGuiKey_0));
    if (key >= ImGuiKey_F1 && key <= ImGuiKey_F24) return VK_F1 + (key - ImGuiKey_F1);
    if (key >= ImGuiKey_Keypad0 && key <= ImGuiKey_Keypad9) return VK_NUMPAD0 + (key - ImGuiKey_Keypad0);
    return 0;
}

//! Physical keyboard state of an ImGui key: 1 down, 0 up, -1 unverifiable.
//! Mod aliases map to their Win32 modifier VKs (GetAsyncKeyState is agnostic
//! of left/right, matching how ImeMenu forwards GFx's undistinguished kControl).
auto GetPhysicalKeyState(ImGuiKey key) -> int
{
    UINT vk = 0;
    switch (key)
    {
        case ImGuiMod_Ctrl: vk = VK_CONTROL; break;
        case ImGuiMod_Shift: vk = VK_SHIFT; break;
        case ImGuiMod_Alt: vk = VK_MENU; break;
        case ImGuiMod_Super: vk = VK_LWIN; break;
        default: vk = ImGuiKeyToVirtualKey(key); break;
    }
    if (vk == 0)
    {
        return -1;
    }
    if (key == ImGuiMod_Super)
    {
        return ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) != 0 ? 1 : 0;
    }
    return (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0 ? 1 : 0;
}
} // namespace

void UpdateMouseCursorVisual()
{
    auto      &io             = ImGui::GetIO();
    auto       *ui            = RE::UI::GetSingleton();
    const bool cursorMenuOpen = ui != nullptr && ui->IsMenuOpen(RE::CursorMenu::MENU_NAME);
    io.MouseDrawCursor        = io.WantCaptureMouse && !cursorMenuOpen;
}

void HealStuckShortcutKeys(ImGuiKeyChord chord)
{
    if (ImGui::GetIO().WantTextInput)
    {
        return;
    }
    const auto mainKey = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
    if (mainKey == ImGuiKey_None)
    {
        return;
    }

    bool anyDownInImGui = false;
    const auto allPhysicallyUp = [&anyDownInImGui](const ImGuiKey key) -> bool {
        if (ImGui::IsKeyDown(key))
        {
            anyDownInImGui = true;
        }
        return GetPhysicalKeyState(key) == 0; // unverifiable (-1) counts as "not all up": stay safe
    };

    if ((chord & ImGuiMod_Ctrl) != 0 && !allPhysicallyUp(ImGuiMod_Ctrl)) return;
    if ((chord & ImGuiMod_Shift) != 0 && !allPhysicallyUp(ImGuiMod_Shift)) return;
    if ((chord & ImGuiMod_Alt) != 0 && !allPhysicallyUp(ImGuiMod_Alt)) return;
    if ((chord & ImGuiMod_Super) != 0 && !allPhysicallyUp(ImGuiMod_Super)) return;
    if (!allPhysicallyUp(mainKey)) return;

    if (anyDownInImGui)
    {
        logger::info("Shortcut chord keys released physically but stuck down in ImGui; clearing input state.");
        ImGui::GetIO().ClearInputKeys();
    }
}

void HealStuckMouseButtons()
{
    auto                &io            = ImGui::GetIO();
    constexpr UINT        vkButtons[3] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON};
    static std::uint64_t  s_stuckSinceMs[3] = {};
    for (int i = 0; i < 3; ++i)
    {
        const bool stuckNow = io.MouseDown[i] && (GetAsyncKeyState(vkButtons[i]) & 0x8000) == 0;
        if (!stuckNow)
        {
            s_stuckSinceMs[i] = 0;
            continue;
        }
        const auto now = GetTickCount64();
        if (s_stuckSinceMs[i] == 0)
        {
            s_stuckSinceMs[i] = now; // arm: could still be the release-lag window
            continue;
        }
        if (now - s_stuckSinceMs[i] > 300)
        {
            logger::info("Mouse button {} stuck down in ImGui while physically up; releasing", i);
            io.AddMouseButtonEvent(i, false);
            s_stuckSinceMs[i] = 0;
        }
    }
}
} // namespace Ime
