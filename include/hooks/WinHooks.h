#pragma once

#include "hooks/Hooks.hpp"
#include "log.h"

#include <Unknwnbase.h>
#include <guiddef.h>

namespace Hooks
{
class DirectInput8CreateHook : public FunctionHook<HRESULT(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN)>
{
public:
    explicit DirectInput8CreateHook(void *&realFuncPtr, func_type *ptr) : FunctionHook(realFuncPtr, ptr)
    {
        if (Detoured())
        {
            logger::debug("Installed {}: {}", __func__, ToString());
        }
    }
};

class WinHooks
{
    static inline std::unique_ptr<DirectInput8CreateHook> DirectInput8Create = nullptr;

    static constexpr auto MODULE_DINPUT8_STRING = "dinput8.dll";

public:
    /// Idempotent: the detour lives for the whole process, so a re-install
    /// only re-arms the wrapper on the still-attached hook.
    static void Install();

    /// Disarms the wrapper without detaching the trampoline — detaching while
    /// another thread may be inside the hook would free code under its feet.
    static void Uninstall();

private:
    static HRESULT MyDirectInput8CreateHook(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
};
} // namespace Hooks
