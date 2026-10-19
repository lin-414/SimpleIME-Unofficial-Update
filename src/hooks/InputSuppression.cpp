#include "hooks/InputSuppression.h"

#include "ImeApp.h"
#include "core/State.h"
#include "log.h"

#include <RE/B/BSWin32KeyboardDevice.h>
#include <RE/Offsets_VTABLE.h>
#include <REL/Relocation.h>

#include <Windows.h>

#include <cstring>

namespace Hooks
{
namespace
{
using PollFn = void (*)(RE::BSWin32KeyboardDevice *, float);

/// Both null means "not installed"; IsSuppressing() must never mistake a failed
/// install for "suppression off right now".
PollFn  g_originalPoll = nullptr;
PollFn *g_pollSlot     = nullptr;

/// The keyboard is muted only while the setting is on AND a composition actually
/// owns the keyboard. A merely focused text field must NOT mute it:
/// HasTextEntry() stays true for as long as the console's input line has focus,
/// and IN_CAND_CHOOSING alone does not mean "still composing" (Google 日本語入力
/// keeps a candidate UI element cached for the whole session) — either condition
/// kept the keyboard dead after the candidate list closed (in-game report,
/// 2026-10-19). IN_COMPOSING tracks exactly the window in which PgUp/arrows are
/// meant for the IME.
auto ShouldSuppress() -> bool
{
    if (!Ime::ImeApp::GetInstance().GetSettings().input.suppressInputWhileTyping)
    {
        return false;
    }
    return Ime::Core::State::GetInstance().Has(Ime::Core::State::IN_COMPOSING);
}

/// Sits on BSWin32KeyboardDevice's vtable slot 0x02 (BSIInputDevice::Poll).
/// After the original poll, curState is copied onto prevState: the engine's own
/// key-diff then sees no state change and emits no keyboard event, while the
/// manager keeps polling the mouse and gamepad normally. prevState is left
/// holding the real current state, so lifting the suppression later does not
/// replay keys typed while it was on.
void HookedPoll(RE::BSWin32KeyboardDevice *a_this, float a_delta)
{
    g_originalPoll(a_this, a_delta);
    if (!ShouldSuppress())
    {
        return;
    }
    auto &runtime = a_this->GetRuntimeData();
    std::memcpy(runtime.prevState, runtime.curState, sizeof(runtime.curState));
}

/// BSIInputDevice: 00 dtor, 01 Initialize, 02 Poll, 03 Shutdown, ...
constexpr std::ptrdiff_t POLL_VTABLE_SLOT = 0x02;
} // namespace

void InputSuppression::Install()
{
    if (g_originalPoll != nullptr)
    {
        return;
    }
    REL::Relocation<std::uintptr_t> vtable{RE::VTABLE_BSWin32KeyboardDevice[0]};
    auto *slot = reinterpret_cast<PollFn *>(vtable.address() + POLL_VTABLE_SLOT * sizeof(std::uintptr_t));
    DWORD oldProtect = 0;
    if (VirtualProtect(slot, sizeof(PollFn), PAGE_EXECUTE_READWRITE, &oldProtect) == 0)
    {
        logger::error("InputSuppression: cannot unprotect the keyboard Poll vtable slot; suppression stays off");
        return;
    }
    g_pollSlot     = slot;
    g_originalPoll = *slot;
    *slot          = &HookedPoll;
    VirtualProtect(slot, sizeof(PollFn), oldProtect, &oldProtect);
    logger::debug("InputSuppression: keyboard Poll hook installed");
}

void InputSuppression::Uninstall()
{
    if (g_originalPoll == nullptr || g_pollSlot == nullptr)
    {
        return;
    }
    DWORD oldProtect = 0;
    if (VirtualProtect(g_pollSlot, sizeof(PollFn), PAGE_EXECUTE_READWRITE, &oldProtect) != 0)
    {
        *g_pollSlot = g_originalPoll;
        VirtualProtect(g_pollSlot, sizeof(PollFn), oldProtect, &oldProtect);
    }
    g_originalPoll = nullptr;
    g_pollSlot     = nullptr;
}

auto InputSuppression::IsSuppressing() -> bool
{
    return g_originalPoll != nullptr && ShouldSuppress();
}
} // namespace Hooks
