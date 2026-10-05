#pragma once

namespace Ime
{
class ImeWnd;

namespace Events
{
void InstallEventSinks();
void UnInstallEventSinks();
/// Every-frame (game thread) repair for a leaked text-entry counter: when the
/// counter is > 0 but no menu that could own it remains on the stack (stable
/// for a grace period), heal the counter and force the IME off. Catches leaks
/// whose AllowTextInput(true) lands AFTER the menu-close event, where the
/// event-based repair can no longer see them.
void PollTextEntryCountConsistency();
} // namespace Events

} // namespace Ime
