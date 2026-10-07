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
    static void Install();

    static void Uninstall();

private:
    static HRESULT MyDirectInput8CreateHook(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
};
} // namespace Hooks
