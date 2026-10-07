#include "hooks/Hooks.hpp"

#include "configs/CustomMessage.h"
#include "detours/detours.h"
#include "log.h"

#include <errhandlingapi.h>
#include <processthreadsapi.h>
#include <windows.h>

namespace Hooks
{
namespace
{
/// Text for the Win32 error the Detours transaction steps report.
std::string_view DetourErrorMessage(LONG error)
{
    switch (error)
    {
        case ERROR_INVALID_OPERATION:
            return "No pending or Already exists transaction.";
        case ERROR_INVALID_HANDLE:
            return "The ppPointer parameter is NULL or points to a NULL pointer.";
        case ERROR_INVALID_BLOCK:
            return "The function referenced is too small to be detoured.";
        case ERROR_NOT_ENOUGH_MEMORY:
            return "Not enough memory exists to complete the operation.";
        case ERROR_INVALID_DATA:
            return "Target function was changed by third party between steps of the transaction.";
        default:
            return "unexpected error when detour.";
    }
}

/// One Detours transaction attaching or detaching a single target.
auto RunTransaction(PVOID *original, PVOID hook, bool attach) -> bool
{
    LONG error = NO_ERROR;
    if (error = DetourTransactionBegin(); error == NO_ERROR)
    {
        if (error = DetourUpdateThread(GetCurrentThread()); error == NO_ERROR)
        {
            if (error = attach ? ::DetourAttach(original, hook) : ::DetourDetach(original, hook); error == NO_ERROR)
            {
                error = DetourTransactionCommit();
            }
        }
    }

    if (error != NO_ERROR)
    {
        DetourTransactionAbort();
        logger::error(
            "Failed detour {} (target {:#x}, hook {:#x}): {}",
            attach ? "attach" : "detach",
            reinterpret_cast<std::uintptr_t>(*original),
            reinterpret_cast<std::uintptr_t>(hook),
            DetourErrorMessage(error));
    }

    return error == NO_ERROR;
}
} // namespace

auto DetourUtil::DetourAttach(void **original, void *hook) -> bool
{
    return RunTransaction(original, hook, true);
}

auto DetourUtil::DetourDetach(void **original, void *hook) -> bool
{
    return RunTransaction(original, hook, false);
}

} // namespace Hooks
