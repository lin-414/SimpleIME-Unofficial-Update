# Draft: upstream feature request to PrismaUI-SKSE/framework

> 待用户确认后手动提交到 https://github.com/PrismaUI-SKSE/framework/issues
> （外部发布动作，未擅自代发。）

---

**Title:** Public API: no way to identify the focused view — blocks external IME / caret-aware integrations

**Body:**

Hi, I maintain SimpleIME, an IME helper mod that integrates with PrismaUI-based menus (PMCM, Outfit Wheeler, ...) through the public `IVPrismaUI1` interface. Thanks for building PrismaUI — the `HasAnyActiveFocus()` query and the `PrismaUI.ImeAssociation` handshake are what make IME coexistence possible at all.

**Problem**

To position an external candidate window at the focused text field, my plugin needs to ask the focused view's page where the focused element is, e.g. via `Invoke(view, script, callback)`. That requires the view id — and there is currently no public way to obtain it:

- View ids are random 64-bit values (`NanoIdGenerator`, `uniform_int_distribution<uint64_t>(1, UINT64_MAX)` in `src/Utils/NanoID.h`), so scanning candidate ids is impossible.
- Neither `IVPrismaUI1` nor `IVPrismaUI2` exposes any enumeration or "which view is focused" query. `HasAnyActiveFocus()` answers *whether* a view is focused, but not *which one*.
- The `PrismaUI.ImeAssociation` registered message carries only a bool and the HIMC (`ImeHelper::SetAssociation`), not a view id.

The only integrations that can work today are ones that never need to address a specific view. Anything caret-aware (candidate window placement, field tracking, per-view logging) is locked out.

**Request**

One of the following would unlock this class of integrations while staying read-only:

1. `PrismaView GetFocusedView() const noexcept` — cheapest for us; or
2. a view id enumeration API, e.g. `std::uint32_t GetViewIds(PrismaView* out, std::uint32_t max) const noexcept` (we can then poll `HasFocus(id)`); or
3. an optional focus-change notification, e.g. an overload of `RegisterJSListener`-style callback `OnFocusChanged(PrismaView view, bool focused)`.

Option 1 alone would be enough for our use case, and a V3 interface (or an addition to V2) would both work. We only ever use the value as an argument to the existing public `Invoke`/`HasFocus` calls — no private vtables.

Happy to test a build against PMCM + our plugin if that helps.
