//
// Created by jamie on 2026/2/5.
//

#pragma once

#include "RE/B/BSTEvent.h"
#include "RE/B/BSTSingleton.h"
#include "RE/C/ControlMap.h"
#include "RE/U/UserEventEnabled.h"

#include <cstdint>

namespace Ime
{
class ControlMap : public RE::BSTSingletonSDM<ControlMap>,         // 00
                   public RE::BSTEventSource<RE::UserEventEnabled> // 08
{
    using CM = RE::ControlMap;

public:
    // NOLINTBEGIN( misc-non-private-member-variables-in-classes)
    CM::InputContext                *controlMap[CM::InputContextID::kTotal]; // 060
    RE::BSTArray<CM::LinkedMapping>  linkedMappings;                         // 0E8
    RE::BSTArray<CM::InputContextID> contextPriorityStack;                   // 100
    uint32_t                         enabledControls;                        // 118
    uint32_t                         unk11C;                                 // 11C
    std::uint8_t                     textEntryCount;                         // 120
    bool                             ignoreKeyboardMouse;                    // 121
    bool                             ignoreActivateDisabledEvents;           // 122
    std::uint8_t                     pad123;                                 // 123
    uint32_t                         gamePadMapType;                         // 124
    uint8_t                          allowTextInput;                         // 128
    uint8_t                          unk129;                                 // 129
    uint8_t                          unk12A;                                 // 12A
    uint8_t                          pad12B;                                 // 12B
    uint32_t                         unk12C;                                 // 12C
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    // ⚠ LAYOUT TRAP — read before "fixing" anything here.
    //   * The member comments above are the SE offsets. In the SE engine
    //     struct: textEntryCount @ 0x120, allowTextInput @ 0x128.
    //   * The AE engine struct has NO dedicated allowTextInput byte: there,
    //     0x128 IS textEntryCount (CommonLibSSE-NG: linkedMappings @ 0xF0,
    //     enabledControls @ 0x120, textEntryCount @ 0x128).
    //   * This mirror keeps the SE member names, so on AE the code paths use
    //     the member NAMED `allowTextInput` — which at runtime on AE is the
    //     real text-entry counter. Every reader/writer (SKSE_AllowTextInput,
    //     GetTextEntryCount, the per-frame counter poll) branches on IsSE()
    //     and both branches hit their runtime's true counter; the counters
    //     have been verified consistent end to end.
    //   * A "cleanup" that renames `allowTextInput` → `textEntryCount`, unifies
    //     the branches, or reorders members makes AE increment/decrement the
    //     enabledControls bit-field region instead of the counter — instant
    //     text-input corruption game-wide. The static_asserts below pin the
    //     assumption; if one fires, the engine layout changed, not this class.


    // A SKSE copy: InputManager::AllowTextInput
    auto SKSE_AllowTextInput(bool allow) -> uint8_t;

    auto GetTextEntryCount() const -> uint8_t;

    auto HasTextEntry() const -> bool
    {
        return GetTextEntryCount() > 0;
    }

    static auto GetSingleton() -> ControlMap *;

private:
    static void DoAllowTextInput(bool allow, std::uint8_t &entryCount);
};

// offsetof needs the complete type, so these live outside the class body.
static_assert(RE::ControlMap::InputContextID::kTotal == 17, "ControlMap mirror assumes the flat 17-context layout");
static_assert(offsetof(ControlMap, textEntryCount) == 0x120, "SE/AE mirror layout changed");
static_assert(offsetof(ControlMap, allowTextInput) == 0x128, "AE textEntryCount alias moved");
} // namespace Ime
