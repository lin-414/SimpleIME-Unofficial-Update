#include "hooks/WinHooks.h"

#include "FakeDirectInputDevice.h"
#include "detours/detours.h"
#include "log.h"

#include <dinput.h>

#include <atomic>

namespace Hooks
{
namespace
{
    /// Wrapper arm state. The detour itself is never detached: a live
    /// trampoline cannot be safely torn down while another thread may be
    /// executing inside it (same lifetime policy as the Present hook), so
    /// Uninstall() disarms the wrapper instead of detaching.
    std::atomic<bool> s_active{false};
}
void WinHooks::Install()
{
    // A live trampoline hook cannot be re-created; a re-install (D3DInit
    // retry) only re-arms the wrapper on the still-attached detour.
    if (DirectInput8Create == nullptr)
    {
        if (PVOID realFuncPtr = ::DetourFindFunction(MODULE_DINPUT8_STRING, "DirectInput8Create"); realFuncPtr != nullptr)
        {
            // Detour live inside the ctor; safe — see ScaleformHook::Install.
            DirectInput8Create = std::make_unique<DirectInput8CreateHook>(realFuncPtr, MyDirectInput8CreateHook);
        }
        else
        {
            logger::error("DetourFindFunction failed: {}!DirectInput8Create — the DirectInput8 hook is not installed", MODULE_DINPUT8_STRING);
        }
    }
    s_active.store(DirectInput8Create != nullptr, std::memory_order_release);
}

void WinHooks::Uninstall()
{
    // Dormant, not detached: requests pass straight through to the original
    // DirectInput8Create, exactly as if the hook had never been installed.
    s_active.store(false, std::memory_order_release);
}

HRESULT WinHooks::MyDirectInput8CreateHook(
    HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, LPVOID *ppvOut, LPUNKNOWN punkOuter
)
{
    // The wrapper implements the A layout only. A W request would reinterpret
    // the W vtable/structs through A wrappers (BuildActionMap etc. differ) and
    // corrupt memory — pass the request straight through instead. A dormant
    // (uninstalled) hook passes everything through for the same reason.
    if (!s_active.load(std::memory_order_acquire) || riidltf != IID_IDirectInput8A)
    {
        return DirectInput8Create->Original(hinst, dwVersion, riidltf, ppvOut, punkOuter);
    }

    IDirectInput8A *dinput;
    const HRESULT   hresult =
        DirectInput8Create->Original(hinst, dwVersion, riidltf, reinterpret_cast<void **>(&dinput), punkOuter);
    if (hresult != DI_OK)
    {
        return hresult;
    }

    *reinterpret_cast<IDirectInput8A **>(ppvOut) = new FakeDirectInput(dinput);
    return DI_OK;
}
} // namespace Hooks
