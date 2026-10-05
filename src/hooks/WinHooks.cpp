#include "hooks/WinHooks.h"

#include "FakeDirectInputDevice.h"
#include "detours/detours.h"
#include "log.h"

#include <dinput.h>

namespace Hooks
{
void WinHooks::Install()
{
    if (PVOID realFuncPtr = ::DetourFindFunction(MODULE_DINPUT8_STRING, "DirectInput8Create"); realFuncPtr != nullptr)
    {
        // Detour live inside the ctor; safe — see ScaleformHook::Install.
        DirectInput8Create = std::make_unique<DirectInput8CreateHook>(realFuncPtr, MyDirectInput8CreateHook);
    }
}

void WinHooks::Uninstall()
{
    DirectInput8Create = nullptr;
}

HRESULT WinHooks::MyDirectInput8CreateHook(
    HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, LPVOID *ppvOut, LPUNKNOWN punkOuter
)
{
    // The wrapper implements the A layout only. A W request would reinterpret
    // the W vtable/structs through A wrappers (BuildActionMap etc. differ) and
    // corrupt memory — pass the request straight through instead.
    if (riidltf != IID_IDirectInput8A)
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
