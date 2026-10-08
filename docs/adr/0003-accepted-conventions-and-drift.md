# ADR-0003: Accepted conventions and known drift

**Status:** Accepted
**Date:** 2026-10-08
**Context files:**

- `src/hooks/MeridianBridge.cpp`, `src/hooks/SkseMenuFrameworkBridge.cpp`, `src/hooks/NirnLabBridge.cpp` (lock idioms, statics, predicates)
- `include/ScaleformHook.h` (include guard style)
- `include/ui/Settings.h`, `include/ui/panels/AppearancePanel.h` (zoom scales)
- `include/tsf/TextStore.h`, `include/ime/ITextService.h` (out-parameter naming)
- `docs/CLEANUP-PLAN.md` §5 (the audit that inventoried this list)

---

## Background

The 2026-10-07 cleanup audit found the codebase stylistically non-uniform in
several recurring ways. The question was: unify by mass rename, or accept and
document the drift?

## Decision

**Accept and document. Do not mass-rename.** A rename touches every call site
for zero observable change — an unreviewable diff with real regression risk —
and several of the "inconsistencies" carry no meaning worth the churn. New
code should follow the convention of the file it lands in.

The accepted drift, each verified as harmless:

1. **Lock idioms.** `std::lock_guard` (Meridian, NirnLab) and `std::scoped_lock`
   (Sksemf) coexist. For single-mutex scopes they are equivalent; the choice
   signals nothing.
2. **Static naming.** `s_` prefix (all four bridges), `g_` (two constructor log
   sites in ScaleformHook.cpp), and unprefixed file-local statics in anonymous
   namespaces (Hooks.cpp) coexist.
3. **Integer spellings.** `std::uint8_t` and plain `uint8_t` (via Windows
   headers) mix. New code prefers `std::`.
4. **Out-parameter naming.** `a_x` (SkseMenuFrameworkBridge::GetFieldAnchor),
   bare `x`, and `indexOut` (MeridianBridgeLogic) are all in use.
5. **Include guards.** `ScaleformHook.h` uses `#ifndef`; everything else uses
   `#pragma once`.
6. **Predicate names.** `ShouldRoute` / `HasFocus` / `SessionActive` /
   `OwnsInput` / `OwnsCandidateUi` are five per-bridge names for overlapping
   "this bridge owns input" concepts. Each has distinct per-backend semantics
   (read-only avoidance vs routing vs candidate-surface ownership); do not
   alias them into one.
7. **Two zoom scales.** `Settings::ZOOM_MIN/ZOOM_MAX/ZOOM_STEP`
   (factor form: 0.5F / 2.0F / 25) and the appearance panel's percent
   constants (50 / 200 / 25 / 100) coexist. Both are correct in their own
   unit (factor vs percent); the conversion happens at the combo row.

## Fixed instead of documented

- **`NirnLabBridge` `s_state` was a plain (non-atomic) `SupportState`** written
  by the main-thread messaging handshake and read cross-thread through
  `MeridianBridge::State()` (settings UI, diagnostics). That is a data race by
  the letter of the standard — unlike the items above, a real defect. Fixed in
  commit `62e826a` (`std::atomic<SupportState>`).

## Deferred

- **Mass clang-format** (61 files drifted at audit time): must land as its own
  whitespace-only commit on a quiet tree. Deferred while the working tree
  carries in-progress feature work; see CLEANUP-PLAN §5.
