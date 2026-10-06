#pragma once

#include "hooks/SupportState.h"

#include <Windows.h>

#include <string_view>

namespace Hooks::PrismaBridge
{
/// Query PrismaUI.dll's *public* V1 focus interface and coordinate text entry
/// with Prisma-based UIs (PMCM, Outfit Wheeler, ...). Prisma views take input
/// capture over their own IME association; when one is focused SimpleIME
/// TAKES OVER composition (its own candidate window shows) and delivers
/// committed text through the game window's WM_CHAR stream, which PrismaUI's
/// subclass feeds into the focused view. Safe to call when Prisma is not
/// installed. Runs on the main thread (SKSE kPostPostLoad).
void Install();
void Uninstall();

/// Install-time outcome (atomic; written by Install on the main thread,
/// readable from any thread). Backs the settings UI's compatibility caption.
[[nodiscard]] SupportState State();

/// Game-thread per-frame refresh of the cached focus state (call from
/// ImeMenu::PostDisplay). Throttled internally: HasAnyActiveFocus may wait on
/// Ultralight, so it must never be called from a window procedure.
void Refresh();

/// The game window received Prisma's `PrismaUI.ImeAssociation` registered
/// message. `associated` mirrors the message's wParam: Prisma is associating
/// (or disassociating) its own IME context. On association the IME takes over
/// text entry immediately; on disassociation the decision is re-evaluated.
void OnAssociationMessage(bool associated);

/// True while a Prisma view holds input capture (atomic; game thread writes,
/// IME thread reads).
[[nodiscard]] bool OwnsInput();

/// PrismaUI.dll is present but its public V1 API failed to negotiate. There
/// is then no reliable focus signal for (nor commit channel into) its views:
/// the IME must fail safe and stand down, letting Prisma's own native IME
/// pipeline keep the keyboard.
[[nodiscard]] bool IsUnavailable();

/// True while committed text must be delivered through the game window's
/// character stream — a Prisma view owns input and the V1 API negotiated.
[[nodiscard]] bool ShouldRoute();

/// Deliver committed UTF-16 text into the focused Prisma view by posting
/// WM_CHAR messages at the game window (PrismaUI's subclass queues them into
/// the view, recombining surrogates itself). IME thread.
void QueueText(std::wstring_view text);

/// The game window handle, provided by ImeWnd once created (its parent). The
/// commit route posts character messages at this window.
void SetGameHwnd(HWND hwnd);

/// Session-sticky "input field" anchor for the candidate window. Ultralight
/// exposes no caret rect, and PrismaUI's public API cannot name the focused
/// view (ids are random 64-bit NanoIDs with no enumeration query), so the
/// best proxy is the interaction point: seeded from the engine cursor while
/// the engine cursor menu keeps it fresh, refreshed on every left click
/// inside the view (a click on a text field IS the field). Held for the whole
/// session so the candidate window does not chase the live cursor. Writers
/// are on the game thread; the coordinates travel bit-packed through a single
/// atomic so the render-thread reader never sees a mixed X/Y pair.
void UpdateFieldAnchor(float x, float y);
/// Render-thread read; false when no Prisma session anchor is available.
[[nodiscard]] bool GetFieldAnchor(float &x, float &y);

/// The registered window message id for Prisma's IME association handshake,
/// or 0 before Install.
[[nodiscard]] unsigned AssociationMessage();
} // namespace Hooks::PrismaBridge
