//
// Meridian UI (CEF) text-input bridge.
//
// Meridian-based UIs (e.g. Tailor) render through CEF and live entirely
// outside Skyrim's menu stack, so Scaleform char-event injection never reaches
// their DOM text fields. This bridge observes Meridian's *public* interfaces
// through two backends that feed one session machinery:
//
//   * NirnLabUIPlatform ("UIPlatform", the current Meridian successor) —
//     NirnLabBridge negotiates IUIPlatformAPI over SKSE messaging, tracks
//     browsers via AddOrGetBrowser and reports SetBrowserFocused events into
//     this unit. PRIMARY backend; covers UIPlatform-based UIs (SkipQuestNG
//     etc.) that View/1 cannot see.
//   * Meridian.View/1 (MeridianUI.dll, Meridian 1.0-era) — negotiates the
//     extension and hooks the `TryFocus` vtable slot. FALLBACK backend for
//     older UIs.
//
// The focused view/browser is captured and committed IME text is delivered
// into the focused DOM field via ExecuteJavaScript + document.execCommand,
// which emits the native `input` events the web UI listens for.
//
#include "hooks/MeridianBridge.h"

#include "ImeApp.h"
#include "ImeWnd.hpp"
#include "core/State.h"
#include "hooks/MeridianApi.h"
#include "hooks/MeridianBridgeLogic.h"
#include "hooks/NirnLabApi.h"
#include "hooks/NirnLabBridge.h"
#include "hooks/ScopeFlag.h"
#include "ime/ImeController.h"
#include "log.h"
#include "MeridianUI/ViewAPI.h"
#include "NirnLabUIPlatform/API.h"

#include <Windows.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

namespace Hooks::MeridianBridge
{
namespace
{
using namespace ::Meridian::UI::View;

using API     = IViewAPI;
using View    = ViewHandle;
using Result  = FocusResult;
using Mode    = FocusMode;
using FocusFn = Result (*)(API *, View, Mode);

/// REL::safe_write's assert is compiled out in NDEBUG, so a failed
/// VirtualProtect silently no-ops: verify the slot before the write and read
/// it back afterwards.
bool PatchVtableSlot(void *slot, std::uintptr_t hook, std::uintptr_t expected, const char *what)
{
    const auto slotAddress = reinterpret_cast<std::uintptr_t>(slot);
    if (std::memcmp(slot, &expected, sizeof(expected)) != 0)
    {
        logger::error("Vtable verify failed before write: {} slot at {:#x}", what, slotAddress);
        return false;
    }
    REL::safe_write(slotAddress, hook);
    if (std::memcmp(slot, &hook, sizeof(hook)) != 0)
    {
        logger::error("Vtable patch did not stick: {} slot at {:#x}", what, slotAddress);
        return false;
    }
    return true;
}

/// Log-friendly name of a backend install state (the settings UI renders the
/// full tokens from the same enum).
const char *StateToken(const SupportState state)
{
    switch (state)
    {
        case SupportState::Pending:
            return "pending";
        case SupportState::Off:
            return "off";
        case SupportState::Standoff:
            return "standoff";
        case SupportState::NotDetected:
            return "not-detected";
        case SupportState::Failed:
            return "failed";
        case SupportState::Active:
            return "active";
    }
    return "unknown";
}

// MSVC x64 View/1 vtable layout: [0] deleting dtor, [1] CreateView,
// [2] DestroyView, [3] IsValid, [4] IsReady, [5] RegisterListener,
// [6] ExecuteJavaScript, [7] Show, [8] Hide, [9] TryFocus, [10] Unfocus.
// Only TryFocus is hooked (the slot the first-generation technique ships on),
// keeping the layout-assumption exposure minimal. Focus loss and destroyed
// views are caught by the Tick() HasFocus backstop instead of an Unfocus hook.
constexpr std::size_t SLOT_TRY_FOCUS = 9;

// ---- state ------------------------------------------------------------
// The Meridian API calls happen inside the hooks (game thread) and inside
// Tick, which has TWO callers: ImeMenu::PostDisplay (game thread) and
// ImeApp::PresentHook (render thread, the fallback frame driver). Tick is
// written for a single execution at a time — the single-flight guard at its
// top makes the second caller skip while one Tick body runs, so the two
// frame drivers never interleave capture/flush/overlay work. The JS listener
// callback arrives on Meridian's CEF thread and only touches atomics; the
// IME thread only appends to the mutex-guarded pending queue.
API *                   s_api          = nullptr; ///< written once on the main thread, never null-ed while live
FocusFn                 s_originalTryFocus = nullptr;
std::atomic<View>       s_focusedView{0};
std::atomic<bool>       s_captureReady{false};
std::atomic<bool>       s_enabled{false};    ///< config gate, latched at Install
std::atomic<SupportState> s_state{SupportState::Pending}; ///< View/1 backend install outcome
std::atomic<std::uint64_t> s_sequence{0};    ///< session id; validated in every listener payload
std::atomic<std::uint64_t> s_lastCaptureAttemptMs{0};
std::atomic<bool>       s_listenerAlive{true};
std::atomic<std::uint32_t> s_silentCaptureAttempts{0};
std::atomic<bool>       s_warnedNoField{false};
std::atomic<bool>       s_tickInFlight{false};   ///< single-flight guard: one Tick body at a time
View                    s_listenerView = 0;      ///< Tick body only (single-flight guard in Tick)
::NL::CEF::IBrowser *   s_callbackBrowser = nullptr; ///< Tick body only (single-flight guard in Tick)
std::mutex              s_pendingMutex;
std::deque<char16_t>    s_pending;               ///< UTF-16 units awaiting the DOM
// Tick-thread only (single-flight guard in Tick): when the session's final
// (empty) push last ran. The commit
// keystroke's own char is forwarded right after the composition cleared (the
// WM_CHAR gate is open again by then) and lands as a separate one-space chunk
// within milliseconds — dropped inside this window, kept for later deliberate
// spaces. The DOM leak-strip pass in the empty push handles the space when the
// host's own leak path delivered it first.
std::uint64_t           s_lastCompositionEndMs = 0;
constexpr std::uint64_t TRAIL_SPACE_DROP_MS = 300;
// Set when a Meridian view gains focus: one capture probe right away, so the
// DOM leak baseline exists before the user's first keystroke (the first
// composition otherwise races the capture round-trip and its first leaked
// character escapes the strip).
std::atomic<bool>       s_initialCapturePending{false};
// Written by FlushPending (game thread) and acknowledged/failed from the CEF
// listener thread — every access is guarded by s_pendingMutex. A commit with
// no "inserted" acknowledgment inside COMMIT_ACK_TIMEOUT_MS is dropped and
// the field re-captured (the SessionGate-style deadline from Skyrim-Text-
// Bridge: a lost callback must never wedge the queue).
std::u16string          s_inFlight;              ///< commit sent, unacknowledged
std::uint64_t           s_inFlightMs = 0;        ///< 0 = nothing in flight
constexpr std::size_t   MAX_PENDING_UNITS  = 8192;
constexpr std::size_t   MAX_COMMIT_UNITS   = 1024;
constexpr std::uint64_t CAPTURE_RETRY_MS   = 250;
constexpr std::uint64_t COMMIT_ACK_TIMEOUT_MS = 2000;
constexpr std::uint32_t LISTENER_SILENCE_LIMIT = 4; ///< capture attempts before re-registering

// Composition/candidate overlay state. Tick has two callers (game-thread
// PostDisplay, render-thread present hook) kept single-flight by the guard in
// Tick, and the overlay bookkeeping additionally gets its own mutex so a
// transition-frame overlap can never race the push cache.
// s_lastUiScript doubles as the change detector: the freshly built script for
// the current composition/candidate state is compared against it, and the DOM
// is only touched when they differ. s_lastUiActive says the panel is showing
// content, so Tick keeps pushing after the composition ends — until the empty
// script lands once and hides it.
std::mutex        s_uiMutex;
std::string       s_lastUiScript;
std::atomic<bool> s_lastUiActive{false};
constexpr char    UI_CLEAR_SCRIPT[] = "if(window.__simpleIME)window.__simpleIME.ui(\"\",[],-1);";

// M3 theme mirror for the DOM panel. The ImGui/M3 styles live on the render
// thread only; ImeWnd::Draw consumes the request and stores the CSS here, and
// Tick pushes it once per session (and whenever the theme/scale changes).
std::atomic<bool> s_themeRefreshPending{false};
std::string       s_themeCss;
std::string       s_themeCssPushed;

// The text never becomes code: JsString() (MeridianBridgeLogic.h) emits every
// UTF-16 unit as a \uXXXX escape, so the payload is a plain JS string literal
// regardless of its content.

// The UIPlatform backend's focused browser (NirnLabBridge owns its lifetime:
// a focused browser is pinned with a host reference, so the pointer stays
// valid for as long as it is stored here — see NirnLabBridge.cpp).
std::atomic<::NL::CEF::IBrowser *> s_focusedBrowser{nullptr};

// One session target at a time: whichever of the two backends last reported
// focus. Focus transitions clear the other backend's marker first, so the
// atomics are never both live.
struct Target
{
    View                view    = 0;
    ::NL::CEF::IBrowser *browser = nullptr;

    [[nodiscard]] bool Valid() const { return view != 0 || browser != nullptr; }
};

Target ResolveTarget()
{
    Target target;
    target.browser = s_focusedBrowser.load(std::memory_order_acquire);
    target.view    = target.browser == nullptr ? s_focusedView.load(std::memory_order_acquire) : 0;
    return target;
}

// Run one script on the target. Returns false only for the View/1 backend's
// synchronous rejection; the UIPlatform ExecuteJavaScript is fire-and-forget
// (the host caches scripts until the page has loaded and replays them), so
// the session deadline backstop covers a stalled delivery there.
bool ExecuteJs(const Target &target, const char *script)
{
    if (target.browser != nullptr)
    {
        target.browser->ExecuteJavaScript(script);
        return true;
    }
    return s_api != nullptr && target.view != 0 && s_api->ExecuteJavaScript(target.view, script);
}

// Backstop: is the target still alive and focused? For a browser this reads
// the host's focus flag (a plain bool under the host's own mutex); for a view
// it asks the View/1 API, which validates the handle internally.
bool TargetAlive(const Target &target)
{
    if (target.browser != nullptr)
    {
        return target.browser->IsBrowserFocused();
    }
    return s_api != nullptr && target.view != 0 && s_api->IsReady(target.view) && s_api->HasFocus(target.view);
}

// The page-side half of the bridge, injected before every capture (fully
// idempotent: re-running it on an existing install is a no-op). The
// host-side contract is:
//
//   window.__simpleIME.capture(id)      → status "<id>:ready" / "no-field"
//   window.__simpleIME.commit(id, text) → "<id>:inserted" / "stale-field"
//                                            / "insert-rejected" / ...
//   window.__simpleIME.cancel(id)       (no report)
//   window.__simpleIME.theme(css)       replace the panel stylesheet
//   window.__simpleIME.ui(comp, candidates, selection, vertical)
//                                          render the composition overlay
//   reports go to window.SimpleIME.result (UIPlatform binding) or to the
//   View/1 named listener window.simpleIMEResult — whichever exists.
//
// The `ui` entry renders the composition string and candidate list into a
// floating panel inside the page, anchored to the captured field. The ImGui
// candidate window cannot serve Meridian sessions — it draws into the game's
// swap chain, underneath Meridian's own CEF composition (blurred by its
// backdrop, flickering between compositors). A same-document panel sits above
// the page's own UI by construction.
//
// Leak compensation: Meridian's host feeds the focused DOM field the same
// physical keystrokes the IME is composing with, through its own
// message-independent input path (AllowTextInput text mode -> char events ->
// CEF) — the composition's raw pinyin lands in the field next to the
// committed text. The TIP consumes the keys upstream of any hook SimpleIME
// owns, so the leak cannot be blocked at the source; instead the panel's
// update pass deletes what appeared at the insertion point since the previous
// push (nothing else writes to the field during a composition), which is
// exact regardless of how the TIP decorates its composition string.
constexpr char BridgeScript[] = R"IMEJS((function(){
    if (window.__simpleIME) return;

    // --- status channel --------------------------------------------------
    // UIPlatform hosts expose the SimpleIME.result binding; View/1 hosts
    // register the named simpleIMEResult listener. Either carries
    // "<id>:<status>" words only — typed text never crosses back.
    function report(id, status) {
        var message = id + ':' + status;
        if (window.SimpleIME && typeof window.SimpleIME.result === 'function') {
            window.SimpleIME.result(message);
            return;
        }
        if (typeof window.simpleIMEResult === 'function') {
            window.simpleIMEResult(message);
        }
    }

    // --- the focused editable field ---------------------------------------
    // Walks into iframes and refuses anything the IME has no business in:
    // disabled/readonly/password fields and non-text input types.
    function editableField() {
        var doc = document, node = doc.activeElement;
        while (node && node.tagName === 'IFRAME') {
            doc = node.contentDocument;
            if (!doc) return null;
            node = doc.activeElement;
        }
        if (!node || node.disabled || node.readOnly || node.type === 'password') return null;
        var takesText = node.tagName === 'TEXTAREA' ||
            (node.tagName === 'INPUT' && /^(text|search|email|url|tel)$/.test(node.type || 'text'));
        return takesText ? {doc: doc, element: node} : null;
    }

    function caretOf(e) {
        try { return e.selectionStart != null ? e.selectionStart : (e.value != null ? e.value.length : -1); }
        catch (_) { return -1; }
    }
    function make(doc, tag, cls, text) {
        var n = doc.createElement(tag);
        if (cls) n.className = cls;
        if (text != null) n.textContent = text;
        return n;
    }

    window.addEventListener('blur', function () {
        session = null;
        hidePanel();
        leak = null;
        leakTracking = false;
    });

    // --- the keystroke-leak guard ------------------------------------------
    // During a composition the host feeds the field the IME's own keystrokes
    // (printable keys insert at the insertion point, Backspace deletes before
    // it) on top of the text SimpleIME commits. The guard's mark remembers
    // where the insertion point sat at the previous pass, so every pass can
    // delete exactly what the leak inserted — or restore what a leak
    // backspace took from real text (the previous value snapshot). At a
    // composition START the mark is re-anchored by matching the composition
    // string (raw keystrokes, no separators yet) against the field tail, so
    // stale marks from English-mode backspacing cannot shift it.
    // composing=false is the session's final pass: one last strip, then stop.
    function tendField(element, doc, composing, comp) {
        if (!element || element.value == null) return;
        try {
            var pos = caretOf(element);
            if (pos < 0) return;
            var tracked = composing && leakTracking && leak && leak.element === element;
            if (tracked) {
                var delta = pos - leak.pos;
                if (delta !== 0 && (delta > 16 || delta < -16)) {
                    // The insertion point jumped — a click, not the leak.
                    leak = {element: element, pos: pos, value: element.value};
                    return;
                }
                if (delta > 0) {
                    element.setSelectionRange(leak.pos, pos);
                    doc.execCommand('delete', false, null);
                    pos = leak.pos;
                } else if (delta < 0 && leak.value != null) {
                    var taken = leak.value.slice(pos, leak.pos);
                    if (taken) {
                        element.setSelectionRange(pos, pos);
                        doc.execCommand('insertText', false, taken);
                        pos = leak.pos;
                    }
                }
                leak = {element: element, pos: pos, value: element.value};
            } else if (composing) {
                // Composition start: anchor before the leak that already
                // landed — the first push's composition string is exactly the
                // raw keystrokes so far.
                var landed = comp && pos >= comp.length && element.value.slice(pos - comp.length, pos) === comp;
                leak = {element: element, pos: landed ? pos - comp.length : pos, value: element.value};
            } else if (leak && leak.element === element) {
                var leftover = pos - leak.pos;
                if (leftover > 0 && leftover <= 16) {
                    element.setSelectionRange(leak.pos, pos);
                    doc.execCommand('delete', false, null);
                }
                leak = null;
            }
            leakTracking = composing;
        } catch (_) {}
    }

    // --- the composition/candidate panel -----------------------------------
    // Default dark styling, mirrored shape-for-shape on the M3 dark palette;
    // keeps the panel readable until the host pushes the live theme().
    var themeCss = '.simpleime-panel{position:fixed;z-index:2147483647;box-sizing:border-box;width:max-content;background:rgba(28,28,32,.96);color:#eee;user-select:none;pointer-events:auto;font-family:"Microsoft YaHei","Segoe UI",sans-serif;font-size:14px;border-radius:8px;overflow:hidden}.simpleime-comp{padding:4px 12px;line-height:20px;white-space:pre-wrap}.simpleime-caret{display:inline-block;vertical-align:text-bottom;width:1px;height:16px;background:#7aa7ff;margin-left:1px;animation:simpleime-blink 1.2s step-end infinite}@keyframes simpleime-blink{0%,66%{opacity:1}67%,100%{opacity:0}}.simpleime-divider{height:1px;background:rgba(255,255,255,.24);margin:0 12px}.simpleime-chips{display:flex;flex-wrap:nowrap;gap:10px;padding:8px 12px}.simpleime-chip{box-sizing:border-box;display:inline-flex;align-items:center;height:28px;padding:0 8px;border-radius:8px;color:#eee;cursor:pointer;white-space:nowrap;background:transparent}.simpleime-chip .simpleime-num{color:rgba(255,255,255,.62)}.simpleime-chip:hover{background:rgba(255,255,255,.10)}.simpleime-chip.simpleime-sel{background:#4a6fa5;color:#fff}.simpleime-chip.simpleime-sel .simpleime-num{color:#fff}.simpleime-vert{min-width:160px}.simpleime-vert .simpleime-chips{flex-direction:column;gap:2px;padding:8px 4px;align-items:stretch}.simpleime-vert .simpleime-chip{width:100%;height:44px;display:block;line-height:44px;padding:0 12px;border-radius:4px;color:#eee;text-align:left}.simpleime-vert .simpleime-chip.simpleime-sel{background:#4a6fa5;color:#fff;border-radius:8px}';

    var panel = null;

    function hidePanel() {
        if (panel) panel.style.display = 'none';
    }
    function dropPanel() {
        if (panel) panel.remove();
        panel = null;
    }
    // Mirror the host-pushed stylesheet into the field's document (a fresh
    // session may live in a document the panel never touched).
    function applyTheme(doc) {
        if (!themeCss) return;
        var st = doc.getElementById('simpleime-style');
        if (!st) {
            st = doc.createElement('style');
            st.id = 'simpleime-style';
            (doc.head || doc.body).appendChild(st);
        }
        if (st.textContent !== themeCss) st.textContent = themeCss;
    }

    function showPanel(comp, cands, sel, vert) {
        var field = editableField();
        if (!session || !field || field.element !== session.element ||
            !session.doc.documentElement.contains(session.element)) {
            // The field moved on: hide instead of following it.
            hidePanel();
            return;
        }
        var doc = session.doc;
        applyTheme(doc);
        if (!panel || panel.ownerDocument !== doc) {
            dropPanel();
            panel = make(doc, 'div', 'simpleime-panel');
            panel.style.display = 'none';
            doc.body.appendChild(panel);
        }
        panel.className = 'simpleime-panel' + (vert ? ' simpleime-vert' : '');
        panel.textContent = '';
        if (comp) {
            var line = make(doc, 'div', 'simpleime-comp');
            line.appendChild(make(doc, 'span', null, comp));
            line.appendChild(make(doc, 'span', 'simpleime-caret'));
            panel.appendChild(line);
            panel.appendChild(make(doc, 'div', 'simpleime-divider'));
        }
        if (cands && cands.length) {
            var row = make(doc, 'div', 'simpleime-chips');
            cands.forEach(function (c, i) {
                var chip = make(doc, 'span', 'simpleime-chip' + (i === sel ? ' simpleime-sel' : ''));
                // Horizontal rows split the data layer's baked-in "{n}. {word}"
                // prefix so the digit renders dim with a small gap before the
                // word — the same Win11-style look as the ImGui candidate
                // window. Vertical rows keep the full label, like the ImGui
                // menu-item list.
                var m = (!vert && /^(\d+)\. (.*)$/.exec(c)) || null;
                if (m) {
                    chip.appendChild(make(doc, 'span', 'simpleime-num', m[1]));
                    chip.appendChild(make(doc, 'span', 'simpleime-word', m[2]));
                } else {
                    chip.textContent = c;
                }
                chip.addEventListener('click', function () { report(session.id, 'pick:' + i); });
                row.appendChild(chip);
            });
            panel.appendChild(row);
        }
        panel.style.display = 'block';
        var r = session.element.getBoundingClientRect();
        var w = panel.offsetWidth, h = panel.offsetHeight;
        var vw = doc.documentElement.clientWidth, vh = doc.documentElement.clientHeight;
        var x = Math.min(Math.max(8, r.left), Math.max(8, vw - w - 8));
        var y = r.bottom + 6;
        if (y + h > vh - 8) y = Math.max(8, r.top - h - 6);
        panel.style.left = x + 'px';
        panel.style.top = y + 'px';
    }

    // --- public entry points (see the block comment in MeridianBridge.cpp) --
    var session = null, leak = null, leakTracking = false;

    window.__simpleIME = {
        capture: function (id) {
            session = null;
            try {
                var field = editableField();
                if (field) {
                    session = {id: id, doc: field.doc, element: field.element};
                    // Baseline for the leak guard: where the insertion point
                    // sits before this capture. An existing mark for the same
                    // element survives (a mid-composition re-capture must not
                    // adopt already-leaked characters); the next composition
                    // start re-anchors it against the composition string.
                    if (!leak || leak.element !== field.element) {
                        leak = {element: field.element, pos: caretOf(field.element), value: field.element.value};
                    }
                    leakTracking = false;
                    report(id, 'ready');
                } else report(id, 'no-field');
            }
            catch (_) { report(id, 'capture-error'); }
        },
        commit: function (id, text) {
            try {
                var field = editableField();
                if (!session || session.id !== id || !field || field.element !== session.element ||
                    !session.doc.documentElement.contains(session.element)) {
                    report(id, 'stale-field');
                    return;
                }
                if (!session.doc.execCommand('insertText', false, text)) {
                    report(id, 'insert-rejected');
                    return;
                }
                // The caret moved by text.length; keep the leak baseline in
                // step so a composition starting right after strips from here.
                if (leak && leak.element === field.element) leak.pos = caretOf(field.element);
                report(id, 'inserted');
            } catch (_) { report(id, 'insert-error'); }
        },
        cancel: function (id) {
            if (session && session.id === id) {
                session = null;
                hidePanel();
                leak = null;
                leakTracking = false;
            }
        },
        theme: function (css) {
            try {
                themeCss = css || '';
                if (session) applyTheme(session.doc);
            } catch (_) {}
        },
        ui: function (comp, cands, sel, vert) {
            try {
                if (session) {
                    var f = editableField();
                    if (f && f.element === session.element) tendField(f.element, session.doc, !!comp, comp);
                    else {
                        leak = null;
                        leakTracking = false;
                    }
                }
                if (!comp && (!cands || !cands.length)) {
                    hidePanel();
                    return;
                }
                showPanel(comp, cands, sel, vert);
            } catch (_) { hidePanel(); }
        }
    };
})()
)IMEJS";

// ---- focus tracking ---------------------------------------------------

// Defined in the CEF-thread listener section below; both backends converge
// here (View/1 named-listener payloads, UIPlatform SimpleIME.result args).
void OnListenerPayload(const char *payload);

// Re-evaluate the IME decision on the IME thread: IsShouldEnableIme() now
// includes Meridian focus, so a sync enables the IME when a view or browser
// gains focus and disables it when the last one loses it (unless a Scaleform
// text entry or keepImeOpen keeps it on).
void RequestImeSync()
{
    if (auto *controller = Ime::ImeController::GetInstance(); controller->IsReady())
    {
        controller->SyncImeState();
    }
}

/// Arm the capture probe and the theme re-push for a target about to
/// (re)start a session. Deliberately does NOT touch the DOM panel caches —
/// BeginSession adds that on top; RequestCapture must not churn a live panel.
void ArmCapture()
{
    s_captureReady          = false;
    s_initialCapturePending = true;
    s_themeRefreshPending   = true;
}

/// Reset the per-session pieces a new focus target starts from: the capture
/// probe is re-armed, the DOM panel anchor dropped and the theme mirror is
/// pushed again once the snapshot is in (the new session may live in a fresh
/// document).
void BeginSession()
{
    ArmCapture();
    std::lock_guard lock(s_uiMutex);
    s_themeCssPushed.clear();
}

/// Drop everything a live session owns: queued text, the unacknowledged
/// commit, the DOM overlay cache. Called when a target goes away.
void ClearSessionState()
{
    s_captureReady = false;
    {
        std::lock_guard lock(s_pendingMutex);
        s_inFlight.clear();
        s_inFlightMs = 0;
        s_pending.clear();
    }
    {
        std::lock_guard lock(s_uiMutex);
        s_lastUiScript.clear();
    }
    s_lastUiActive = false;
}

/// Which backend a focus event belongs to (the two report different target
/// types but share one session spine).
enum class FocusBackend
{
    View,    ///< Meridian.View/1 vtable path (handle-based)
    Browser, ///< UIPlatform path (pinned IBrowser*)
};

/// Shared spine of the two focus-gained transitions: enable gate, same-target
/// early-exit, clear the other backend's marker, session begin, IME sync.
void HandleFocusGained(const FocusBackend backend, const View view, ::NL::CEF::IBrowser *browser)
{
    // The vtable detour is never removed, so after Uninstall (app teardown) it
    // keeps firing: a late focus event must not resurrect session state or
    // spam IME-sync tasks into a shutting-down controller.
    if (!s_enabled.load() || (backend == FocusBackend::Browser && browser == nullptr))
    {
        return;
    }
    // Repeat TryFocus / SetBrowserFocused for the already-focused target (a UI
    // re-asserting focus): the session state for it is live and re-arming it
    // (capture probe, theme push, IME sync) would only churn.
    const bool sameTarget = backend == FocusBackend::View ? s_focusedView.load() == view
                                                          : s_focusedBrowser.load() == browser;
    if (sameTarget)
    {
        return;
    }
    if (backend == FocusBackend::View)
    {
        if (s_focusedBrowser.exchange(nullptr) != nullptr)
        {
            logger::info("Meridian focus moved from a UIPlatform browser to view {:x}", view);
        }
        s_focusedView = view;
    }
    else
    {
        if (s_focusedView.exchange(0) != 0)
        {
            logger::info("Meridian focus moved from a View/1 view to a UIPlatform browser");
        }
        s_focusedBrowser = browser;
    }
    BeginSession();
    if (backend == FocusBackend::View)
    {
        logger::info("Meridian view {:x} gained focus, IME sync requested", view);
    }
    else
    {
        logger::info("UIPlatform browser {:x} gained focus, IME sync requested",
                     reinterpret_cast<std::uintptr_t>(browser));
    }
    RequestImeSync();
}

/// Shared spine of the two focus-lost transitions: owner CAS, session state
/// teardown, DOM panel clear, IME sync. A stale or duplicate report for a
/// session that already ended leaves everything untouched.
void HandleFocusLost(const FocusBackend backend, const View view, ::NL::CEF::IBrowser *browser)
{
    if (backend == FocusBackend::View)
    {
        View expected = view;
        if (!s_focusedView.compare_exchange_strong(expected, 0))
        {
            return;
        }
    }
    else
    {
        if (browser == nullptr)
        {
            return;
        }
        NL::CEF::IBrowser *expected = browser;
        if (!s_focusedBrowser.compare_exchange_strong(expected, nullptr))
        {
            return;
        }
    }
    ClearSessionState();
    // Best effort: the view may already be destroyed (ExecuteJavaScript just
    // returns false); the pin (NirnLabBridge) keeps a browser pointer valid
    // through this call. Either way the DOM panel must not survive the
    // session into a later focus of the same target.
    ExecuteJs(backend == FocusBackend::View ? Target{.view = view} : Target{.browser = browser}, UI_CLEAR_SCRIPT);
    if (backend == FocusBackend::View)
    {
        logger::info("Meridian view {:x} lost focus, IME sync requested", view);
    }
    else
    {
        logger::info("UIPlatform browser {:x} lost focus, IME sync requested",
                     reinterpret_cast<std::uintptr_t>(browser));
    }
    RequestImeSync();
}

void OnViewFocused(const View view)
{
    HandleFocusGained(FocusBackend::View, view, nullptr);
}

void OnViewFocusGone(const View view)
{
    // Called from the Tick backstop (game thread) when Meridian no longer
    // reports the view as focused — covers both a clean Unfocus we chose not
    // to hook and a destroyed view.
    HandleFocusLost(FocusBackend::View, view, nullptr);
}

// The UIPlatform focus entry points (OnBrowserFocused / OnBrowserFocusGone /
// OnBackendListenerPayload) are public namespace functions defined right after
// this anonymous namespace — NirnLabBridge calls them across translation-unit
// boundaries, and the anonymous-namespace helpers they use (BeginSession,
// ClearSessionState, ExecuteJs, OnListenerPayload) stay file-local.

// Vtable detour. Installed once, never removed (Uninstall only stops using
// the API pointer); it is passthrough-safe without it.
Result HookedTryFocus(API *self, const View view, const Mode mode)
{
    const Result result = s_originalTryFocus(self, view, mode);
    if (result == Result::Granted || result == Result::AlreadyFocused)
    {
        OnViewFocused(view);
    }
    return result;
}

// ---- CEF-thread listener ---------------------------------------------

void OnListenerPayload(const char *payload)
{
    using MeridianBridgeLogic::ListenerOutcome;
    const auto seq = s_sequence.load();
    // Candidate picks (<id>:pick:<n> from the DOM overlay) must be matched
    // before the generic status parse, which would classify them as Failed
    // and churn a pointless re-capture.
    std::uint32_t pickIndex = 0;
    if (MeridianBridgeLogic::TryParsePickPayload(seq, payload, pickIndex))
    {
        std::lock_guard lock(s_pendingMutex);
        s_silentCaptureAttempts = 0;
        s_listenerAlive         = true;
        // CommitCandidate marshals to the IME thread via AddTask (task queue
        // + PostMessage), so calling it from this CEF callback thread is safe;
        // AddTask touches none of this section's locks.
        if (const auto result = Ime::ImeController::GetInstance()->CommitCandidate(pickIndex);
            !Ime::IImeModule::IsSuccess(result))
        {
            logger::error("Meridian candidate pick {} was not dispatched", pickIndex);
        }
        return;
    }
    ListenerOutcome outcome = ListenerOutcome::Ignore;
    {
        // ParseListenerPayload is pure and lock-free; the lock only covers the
        // inFlight bookkeeping, which the CEF thread and the game thread both
        // touch.
        std::lock_guard lock(s_pendingMutex);
        outcome = MeridianBridgeLogic::ParseListenerPayload(seq, payload);
        if (outcome == ListenerOutcome::Ignore)
        {
            return; // stale session (listener outlives captures)
        }
        // Any answer proves the listener is alive; payloads are status words
        // only — typed text never crosses this boundary, so nothing is logged
        // or stored here.
        s_silentCaptureAttempts = 0;
        s_listenerAlive         = true;
        switch (outcome)
        {
            case ListenerOutcome::Ready:
                s_captureReady = true;
                {
                    // A fresh capture gave the DOM panel its anchor: any ui()
                    // pushed before this was a no-op (no state in JS). Clear
                    // the cache so the very next Tick pushes the current
                    // composition state for real. s_uiMutex nests inside
                    // s_pendingMutex here; no path takes them the other way.
                    std::lock_guard lock(s_uiMutex);
                    s_lastUiScript.clear();
                }
                logger::debug("Meridian capture session {} ready", seq);
                break;
            case ListenerOutcome::Inserted:
                s_inFlight.clear();
                s_inFlightMs = 0;
                break;
            case ListenerOutcome::NoField:
                s_captureReady = false;
                break;
            case ListenerOutcome::Failed:
            default:
                // stale-field / insert-rejected / capture-error / insert-error:
                // the in-flight chunk was not delivered — put it back at the
                // front of the queue and re-capture on the next Tick.
                s_captureReady = false;
                if (!s_inFlight.empty())
                {
                    s_pending.insert(s_pending.begin(), s_inFlight.begin(), s_inFlight.end());
                    s_inFlight.clear();
                    s_inFlightMs = 0;
                }
                break;
        }
    }
    if (outcome == ListenerOutcome::NoField)
    {
        if (!s_warnedNoField.exchange(true))
        {
            logger::info("Meridian view has no focused text field; committed text is dropped until one is focused");
        }
    }
    else if (outcome == ListenerOutcome::Failed)
    {
        logger::debug("Meridian capture session {} reported a failure; re-capturing", seq);
    }
}

// ---- game-thread Tick helpers ----------------------------------------

// The page→host channel differs per backend: View/1 registers a named
// listener per view (its callback list is per view handle), while a
// UIPlatform browser gets one process-wide JS binding (SimpleIME.result).
// Registration is idempotent on both hosts, and the silence-detector in
// TryCapture re-registers whenever payloads stop arriving (a binding can die
// with a destroyed view or a page navigation that cleared the callbacks).

bool EnsureViewListenerRegistered(const View view)
{
    if (s_listenerView == view && s_listenerAlive.load())
    {
        return true;
    }
    if (!s_api->RegisterListener(view, "simpleIMEResult", [](const char *payload) { OnListenerPayload(payload); }))
    {
        logger::warn("Meridian RegisterListener failed for view {:x}", view);
        return false;
    }
    s_listenerView = view;
    return true;
}

bool EnsureBrowserListenerRegistered(NL::CEF::IBrowser *browser)
{
    if (s_callbackBrowser == browser && s_listenerAlive.load())
    {
        return true;
    }
    ::NL::JS::JSFuncInfo info{};
    info.objectName                      = NirnLabBridge::FUNCTION_OBJECT_NAME;
    info.funcName                        = NirnLabBridge::FUNCTION_RESULT_NAME;
    info.callbackData.callback           = [](const char **args, const int argCount) {
        if (args == nullptr || argCount < 1 || args[0] == nullptr)
        {
            return;
        }
        // NirnLab serializes each JS argument as a JSON value; our payload is
        // the string literal inside it.
        std::string decoded;
        if (!MeridianBridgeLogic::TryDecodeJsonStringArg(args[0], decoded))
        {
            return;
        }
        OnListenerPayload(decoded.c_str());
    };
    info.callbackData.executeInGameThread = false; // host's CEF thread, like the View/1 listener
    info.callbackData.isEventFunction     = false;
    browser->AddFunctionCallback(info);
    s_callbackBrowser = browser;
    return true;
}

bool EnsureListenerRegistered(const Target &target)
{
    if (target.browser != nullptr)
    {
        return EnsureBrowserListenerRegistered(target.browser);
    }
    return EnsureViewListenerRegistered(target.view);
}

void TryCapture(const Target &target, const std::uint64_t now)
{
    const auto last = s_lastCaptureAttemptMs.load();
    if (now - last < CAPTURE_RETRY_MS)
    {
        return;
    }
    s_lastCaptureAttemptMs = now;
    if (!EnsureListenerRegistered(target))
    {
        return;
    }
    const auto id = ++s_sequence;
    std::string script(BridgeScript);
    script += ";window.__simpleIME.capture(" + std::to_string(id) + ");";
    if (ExecuteJs(target, script.c_str()))
    {
        logger::debug("Meridian capture requested (session {})", id);
        // If no status ever arrives, the listener binding is dead: re-register.
        if (++s_silentCaptureAttempts >= LISTENER_SILENCE_LIMIT)
        {
            s_silentCaptureAttempts = 0;
            s_listenerAlive         = false;
        }
    }
    else
    {
        logger::warn("Meridian ExecuteJavaScript(capture) failed for view {:x}", target.view);
    }
}

void FlushPending(const Target &target, const std::uint64_t now)
{
    {
        std::lock_guard lock(s_pendingMutex);
        if (!s_inFlight.empty())
        {
            return; // previous commit still unacknowledged
        }
    }
    std::u16string chunk;
    {
        std::lock_guard lock(s_pendingMutex);
        while (!s_pending.empty() && chunk.size() < MAX_COMMIT_UNITS)
        {
            chunk.push_back(s_pending.front());
            s_pending.pop_front();
        }
        // Never split a surrogate pair across two commits: a long paste cut at
        // the chunk limit between the two halves of a character would enter the
        // DOM as a lone surrogate (U+FFFD) with the other half starting the
        // next chunk. Only step back when the boundary really cuts a pair.
        if (chunk.size() == MAX_COMMIT_UNITS && !s_pending.empty() &&
            (chunk.back() & 0xFC00) == 0xD800 && (s_pending.front() & 0xFC00) == 0xDC00)
        {
            s_pending.push_front(chunk.back());
            chunk.pop_back();
        }
    }
    if (chunk.empty())
    {
        return;
    }
    // The commit keystroke's own char is forwarded right after the composition
    // cleared (the WM_CHAR gate is open again by then): a one-space chunk
    // arriving just after a session's final push is the commit trail, not text.
    if (chunk == u" " && now - s_lastCompositionEndMs <= TRAIL_SPACE_DROP_MS)
    {
        logger::debug("Dropped the commit-trail space after a Meridian composition");
        return;
    }
    const std::string script = "if(window.__simpleIME)window.__simpleIME.commit(" + std::to_string(s_sequence.load()) +
                               "," + MeridianBridgeLogic::JsString(chunk) + ");";
    if (!ExecuteJs(target, script.c_str()))
    {
        logger::warn("Meridian ExecuteJavaScript(commit) failed for view {:x}", target.view);
        std::lock_guard lock(s_pendingMutex);
        s_captureReady = false; // re-capture; the chunk goes back on failure report
        s_pending.insert(s_pending.begin(), chunk.begin(), chunk.end());
        return;
    }
    std::lock_guard lock(s_pendingMutex);
    s_inFlight   = std::move(chunk);
    s_inFlightMs = now;
}

// ---- game-thread overlay push -----------------------------------------

std::string JsStringOf(const std::wstring &text)
{
    static_assert(sizeof(wchar_t) == sizeof(char16_t));
    const auto *const data = reinterpret_cast<const char16_t *>(text.data());
    return MeridianBridgeLogic::JsString({data, data + text.size()});
}

std::string JsStringOf(const std::string &utf8)
{
    if (utf8.empty())
    {
        return MeridianBridgeLogic::JsString({});
    }
    const int wideLength = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (wideLength <= 0)
    {
        return MeridianBridgeLogic::JsString({});
    }
    std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), wideLength);
    return JsStringOf(wide);
}

/// Build the `__simpleIME.ui(...)` script for one composition/candidate
/// snapshot. The script doubles as the change-detector key: identical state
/// produces an identical script, and pushing the empty-state script is what
/// hides the DOM panel when the composition ends.
std::string BuildUiScript(const Ime::CompositionInfo &composition, const Ime::CandidateUi &candidates, const bool vertical)
{
    std::string script = "if(window.__simpleIME&&window.__simpleIME.ui)window.__simpleIME.ui(";
    script += JsStringOf(composition.documentText);
    script += ",[";
    const auto &list = candidates.CandidateList();
    for (std::size_t i = 0; i < list.size(); ++i)
    {
        if (i != 0)
        {
            script += ',';
        }
        script += JsStringOf(list[i]);
    }
    script += "],";
    script += std::to_string(candidates.Selection());
    script += ',';
    script += vertical ? "true" : "false";
    script += ");";
    return script;
}

/// Percent-encode a UTF-8 path into a file:// URL (CEF loads local font files
/// through it for the panel's @font-face; spaces/CJK become %XX sequences).
std::string FileUrlFromUtf8Path(const std::string &utf8Path)
{
    constexpr char hex[] = "0123456789ABCDEF";
    std::string url = "file:///";
    for (const unsigned char c : utf8Path)
    {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                                c == '-' || c == '_' || c == '.' || c == '~' || c == '/' || c == ':';
        if (unreserved)
        {
            url += static_cast<char>(c);
        }
        else
        {
            url += '%';
            url += hex[c >> 4];
            url += hex[c & 15];
        }
    }
    return url;
}

/// Font stack for the panel: the configured primary font (via @font-face,
/// matching the ImGui atlas) with the Microsoft YaHei default as fallback —
/// the same family the ImGui candidate window renders with out of the box.
std::string FontFamilyChain(const std::string &primaryFontPath)
{
    std::string chain;
    if (!primaryFontPath.empty())
    {
        chain += "'SimpleIME Primary',";
    }
    chain += "'Microsoft YaHei','Segoe UI',sans-serif";
    return chain;
}

void PushCandidateOverlay(const Target &target)
{
    // ImeWnd::GetTextService null-guards its own teardown latch; the snapshot
    // locks the service mutex, so running next to the render thread's frame
    // copy is safe. Never touches UpdateIfDirty/GetCompositionInfo — those are
    // the render thread's unlocked, sole-owner copies.
    auto *service = Ime::ImeApp::GetInstance().GetImeWnd().GetTextService();
    if (service == nullptr)
    {
        return;
    }
    Ime::CompositionInfo composition;
    Ime::CandidateUi     candidates;
    service->SnapshotCompositionAndCandidates(composition, candidates);
    const bool panelWanted = !composition.documentText.empty() || !candidates.empty();
    // Follow the mod's horizontal/vertical candidate setting (read like the
    // other appearance fields the UI thread owns — benign torn-free bool).
    const bool vertical = Ime::ImeApp::GetInstance().GetSettings().appearance.verticalCandidateList;
    const std::string script = BuildUiScript(composition, candidates, vertical);
    std::lock_guard   lock(s_uiMutex);
    if (script == s_lastUiScript)
    {
        return;
    }
    if (ExecuteJs(target, script.c_str()))
    {
        s_lastUiScript = script;
        s_lastUiActive = panelWanted;
        if (!panelWanted)
        {
            // The strip pass inside this push ran last for the session; the
            // commit keystroke's own space (forwarded as a separate chunk) is
            // suppressed from here on.
            s_lastCompositionEndMs = GetTickCount64();
        }
    }
    // On an ExecuteJavaScript failure the cache stays stale, so the push is
    // retried on the next Tick until it lands or the state changes.
}
} // namespace

// ---- UIPlatform backend entry points (see NirnLabBridge.cpp) ----------
// Public: the UIPlatform focus events funnel into the same session machinery
// the View/1 hooks drive. Only the session state (atomics above) is shared;
// everything they touch lives in the anonymous namespace.

void OnBrowserFocused(NL::CEF::IBrowser *browser)
{
    HandleFocusGained(FocusBackend::Browser, 0, browser);
}

void OnBrowserFocusGone(NL::CEF::IBrowser *browser)
{
    HandleFocusLost(FocusBackend::Browser, 0, browser);
}

void OnBackendListenerPayload(const char *payload)
{
    OnListenerPayload(payload);
}

void Install()
{
    if (s_api != nullptr)
    {
        return;
    }
    s_enabled = Ime::ImeApp::GetInstance().GetSettings().input.meridianSupport;
    if (!s_enabled.load())
    {
        s_state = SupportState::Off;
        logger::info("Meridian support disabled by configuration");
        return;
    }
    // Mutual exclusion: another plugin may hook the same View/1 TryFocus slot
    // and run its own commit bridge; two plugins typing into Meridian at once
    // would double-handle every focus change and every character. When that
    // DLL (checked below) is present, this backend stands down. The UIPlatform
    // backend is untouched, and Prisma avoidance is unaffected (read-only).
    if (GetModuleHandleW(L"SkyrimTextBridge.dll") != nullptr)
    {
        s_state = SupportState::Standoff;
        logger::warn("Skyrim-Text-Bridge detected; SimpleIME's Meridian View/1 input support stays off to avoid double text injection");
        return;
    }
    const HMODULE module = GetModuleHandleW(L"MeridianUI.dll");
    if (module == nullptr)
    {
        // No MeridianUI.dll: only the UIPlatform backend can serve Meridian
        // UIs here; its outcome decides the combined State() below.
        s_state = SupportState::NotDetected;
        logger::info("MeridianUI.dll not loaded; Meridian View/1 support stays off (UIPlatform backend: {})",
                     StateToken(NirnLabBridge::State()));
        return;
    }
    const auto query = reinterpret_cast<QueryMeridianExtensionFn>(GetProcAddress(module, "QueryMeridianExtension"));
    if (query == nullptr)
    {
        s_state = SupportState::Failed;
        logger::warn("MeridianUI.dll has no QueryMeridianExtension export; Meridian View/1 support unavailable");
        return;
    }
    s_api = RequestMeridianView(query);
    if (s_api == nullptr)
    {
        s_state = SupportState::Failed;
        logger::warn("Meridian.View/1 negotiation failed; Meridian View/1 support unavailable");
        return;
    }
    // Hook the PUBLIC interface's vtable. This relies on the MSVC x64 layout
    // of the published View/1 header (the single slot the first-generation
    // technique ships on); a layout mismatch would divert an unrelated slot.
    std::uintptr_t **table = *reinterpret_cast<std::uintptr_t ***>(s_api);
    s_originalTryFocus = reinterpret_cast<FocusFn>(table[SLOT_TRY_FOCUS]);
    if (!PatchVtableSlot(&table[SLOT_TRY_FOCUS], reinterpret_cast<std::uintptr_t>(&HookedTryFocus),
                         reinterpret_cast<std::uintptr_t>(s_originalTryFocus), "Meridian View/1 TryFocus"))
    {
        s_state = SupportState::Failed;
        return;
    }
    s_state = SupportState::Active;
    logger::info("Meridian View/1 focus observer installed (fallback backend; UIPlatform backend: {})",
                 StateToken(NirnLabBridge::State()));
}

SupportState State()
{
    // The combined install outcome for the settings UI: the UIPlatform
    // backend (primary) and the View/1 backend (fallback) report separately,
    // and this folds them into one token. Anything live counts as Active; a
    // View/1 standoff (competing hook present) without a live UIPlatform
    // backend stays Standoff.
    const auto uiPlatform = NirnLabBridge::State();
    const auto view       = s_state.load(std::memory_order_acquire);
    if (view == SupportState::Off || uiPlatform == SupportState::Off)
    {
        return SupportState::Off;
    }
    if (view == SupportState::Active || uiPlatform == SupportState::Active)
    {
        return SupportState::Active;
    }
    if (view == SupportState::Standoff)
    {
        return SupportState::Standoff;
    }
    if (view == SupportState::Failed || uiPlatform == SupportState::Failed)
    {
        return SupportState::Failed;
    }
    if (view == SupportState::Pending || uiPlatform == SupportState::Pending)
    {
        return SupportState::Pending;
    }
    return SupportState::NotDetected;
}

SupportState ViewBackendState()
{
    // The View/1 fallback backend's own install outcome, un-folded; see
    // State() for the combined token.
    return s_state.load(std::memory_order_acquire);
}

void Uninstall()
{
    s_enabled         = false;
    s_focusedView     = 0;
    s_focusedBrowser  = nullptr;
    s_captureReady    = false;
    s_listenerView    = 0;
    s_callbackBrowser = nullptr;
    ClearSessionState();
    {
        std::lock_guard lock(s_uiMutex);
        s_themeCssPushed.clear();
    }
}

bool HasFocus()
{
    return s_focusedView.load() != 0 || s_focusedBrowser.load() != nullptr;
}

bool ShouldRoute()
{
    return s_enabled.load() && HasFocus();
}

void RequestCapture()
{
    ArmCapture();
}

void RequestUiThemeRefresh()
{
    s_themeRefreshPending = true;
}

bool ConsumeUiThemeRefreshRequested()
{
    return s_themeRefreshPending.exchange(false);
}

bool ConsumeUiThemeRefresh(const UiThemePalette &palette)
{
    const auto rgba = [](const std::array<float, 4> &c) {
        return std::format("rgba({},{},{},{:.3f})", static_cast<int>(c[0] * 255.0F + 0.5F), static_cast<int>(c[1] * 255.0F + 0.5F),
                           static_cast<int>(c[2] * 255.0F + 0.5F), c[3]);
    };
    // Mirrors the ImGui candidate window's styling (see ImeWindow.cpp's
    // DrawCandidates / DrawVerticalCandidates, matched against the Win11-style
    // row): window background + rounding, LabelLarge 14px text on a 20px line,
    // caret in the primary color, 1px outlineVariant divider, a single row of
    // border-less candidates (dim digit with a small gap before the word;
    // hovered = surfaceContainerHigh, selected = primary pill with onPrimary
    // text), or the vertical menu-item list (44px rows, same primary pill
    // selection) following the mod's vertical setting. The primary font is
    // loaded via file URL so the panel matches the configured ImGui font; if
    // the browser refuses local fonts the family chain falls back to Microsoft
    // YaHei. --simpleime-scale carries the M3 dp→px factor (DPI × user zoom) so the
    // panel rescales with the game UI.
    std::string css;
    css += ":root{--simpleime-scale:" + std::format("{:.4f}", palette.scale) + ";";
    css += "--simpleime-bg:" + rgba(palette.windowBg) + ";";
    css += "--simpleime-text:" + rgba(palette.text) + ";";
    css += "--simpleime-caret:" + rgba(palette.caret) + ";";
    css += "--simpleime-divider:" + rgba(palette.divider) + ";";
    css += "--simpleime-number:" + rgba(palette.numberText) + ";";
    css += "--simpleime-chip-hover:" + rgba(palette.chipHoverBg) + ";";
    css += "--simpleime-chip-sel-bg:" + rgba(palette.chipSelectedBg) + ";";
    css += "--simpleime-chip-sel-text:" + rgba(palette.chipSelectedText) + ";";
    css += "--simpleime-row-sel-bg:" + rgba(palette.rowSelectedBg) + ";";
    css += "--simpleime-row-sel-text:" + rgba(palette.rowSelectedText) + ";}";
    if (!palette.primaryFontPath.empty())
    {
        css += "@font-face{font-family:'SimpleIME Primary';src:url(\"" + FileUrlFromUtf8Path(palette.primaryFontPath) + "\")}";
    }
    css += ".simpleime-panel{position:fixed;z-index:2147483647;box-sizing:border-box;width:max-content;"
           "background:var(--simpleime-bg);color:var(--simpleime-text);user-select:none;pointer-events:auto;"
           "font-family:" + FontFamilyChain(palette.primaryFontPath) + ";font-size:calc(14px*var(--simpleime-scale));"
           "border-radius:calc(" + std::format("{:.1f}", palette.windowRounding) + "px*var(--simpleime-scale));overflow:hidden;}";
    css += ".simpleime-comp{padding:calc(4px*var(--simpleime-scale)) calc(12px*var(--simpleime-scale));"
           "line-height:calc(20px*var(--simpleime-scale));white-space:pre-wrap;}";
    css += ".simpleime-caret{display:inline-block;vertical-align:text-bottom;width:1px;"
           "height:calc(16px*var(--simpleime-scale));background:var(--simpleime-caret);margin-left:1px;"
           "animation:simpleime-blink 1.2s step-end infinite;}";
    css += "@keyframes simpleime-blink{0%,66%{opacity:1}67%,100%{opacity:0}}";
    css += ".simpleime-divider{height:1px;background:var(--simpleime-divider);margin:0 calc(12px*var(--simpleime-scale));}";
    css += ".simpleime-chips{display:flex;flex-wrap:nowrap;gap:calc(10px*var(--simpleime-scale));"
           "padding:calc(8px*var(--simpleime-scale)) calc(12px*var(--simpleime-scale));}";
    // Candidate row: 28px (20px line + 2×4dp padding) border-less items, dim
    // digit with a small gap before the word; hovered = surfaceContainerHigh,
    // selected = primary pill. Same geometry as the ImGui window's
    // InvisibleButton items.
    css += ".simpleime-chip{box-sizing:border-box;display:inline-flex;align-items:center;"
           "height:calc(28px*var(--simpleime-scale));padding:0 calc(8px*var(--simpleime-scale));"
           "border-radius:calc(8px*var(--simpleime-scale));"
           "color:var(--simpleime-text);cursor:pointer;white-space:nowrap;background:transparent;}";
    css += ".simpleime-chip .simpleime-num{color:var(--simpleime-number);margin-right:calc(6px*var(--simpleime-scale));}";
    css += ".simpleime-chip:hover{background:var(--simpleime-chip-hover);}";
    css += ".simpleime-chip.simpleime-sel{background:var(--simpleime-chip-sel-bg);color:var(--simpleime-chip-sel-text);}";
    css += ".simpleime-chip.simpleime-sel .simpleime-num{color:var(--simpleime-chip-sel-text);}";
    // Vertical mode (settings.appearance.verticalCandidateList): full-width
    // 44px menu-item rows — the selected row uses the same primary pill as the
    // horizontal chip row (see ImeWindow.cpp DrawVerticalCandidates), full
    // "{n}. {word}" label (no digit split).
    css += ".simpleime-vert{min-width:calc(160px*var(--simpleime-scale));}";
    css += ".simpleime-vert .simpleime-chips{flex-direction:column;gap:calc(2px*var(--simpleime-scale));"
           "padding:calc(8px*var(--simpleime-scale)) calc(4px*var(--simpleime-scale));align-items:stretch;}";
    css += ".simpleime-vert .simpleime-chip{width:100%;height:calc(44px*var(--simpleime-scale));"
           "display:block;line-height:calc(44px*var(--simpleime-scale));padding:0 calc(12px*var(--simpleime-scale));"
           "border-radius:calc(4px*var(--simpleime-scale));"
           "color:var(--simpleime-text);text-align:left;}";
    css += ".simpleime-vert .simpleime-chip.simpleime-sel{background:var(--simpleime-row-sel-bg);color:var(--simpleime-row-sel-text);"
           "border-radius:calc(8px*var(--simpleime-scale));}";
    std::lock_guard lock(s_uiMutex);
    s_themeCss = std::move(css);
    return true;
}

bool OwnsCandidateUi()
{
    // Only while a text field is actually captured: a session without a field
    // (capture reporting no-field) keeps the ImGui candidate window as the
    // fallback surface, matching how committed text is dropped without a
    // field anyway.
    return s_enabled.load() && (s_focusedView.load() != 0 || s_focusedBrowser.load() != nullptr) &&
           s_captureReady.load();
}

void QueueText(const std::wstring_view text)
{
    if (text.empty())
    {
        return;
    }
    std::lock_guard lock(s_pendingMutex);
    for (const wchar_t c : text)
    {
        if (s_pending.size() >= MAX_PENDING_UNITS)
        {
            logger::warn("Meridian text queue overflow; dropping the rest of this commit");
            break;
        }
        s_pending.push_back(static_cast<char16_t>(c));
    }
}

void Tick()
{
    if (!s_enabled.load() || (s_api == nullptr && s_focusedBrowser.load() == nullptr))
    {
        return;
    }
    // Single-flight: Tick's state below (s_listenerView, s_callbackBrowser,
    // s_lastCompositionEndMs, the capture/flush throttles) is written for one
    // execution at a time. The two frame drivers (game-thread PostDisplay,
    // render-thread present hook) can overlap around a session transition; the
    // second caller skips and the running one does that frame's work. RAII
    // reset so an exception can never wedge the flag and permanently kill the
    // tick.
    const ScopeFlag tickInFlight(s_tickInFlight);
    if (!tickInFlight.owned())
    {
        return;
    }
    const Target target = ResolveTarget();
    if (!target.Valid())
    {
        // No Meridian view or UIPlatform browser focused: nothing may survive
        // into a later session.
        {
            std::lock_guard lock(s_pendingMutex);
            s_pending.clear();
        }
        {
            std::lock_guard lock(s_uiMutex);
            s_lastUiScript.clear();
        }
        s_lastUiActive = false;
        return;
    }
    // Backstop for a view or browser that lost or was destroyed without
    // notice.
    if (!TargetAlive(target))
    {
        if (target.browser != nullptr)
        {
            logger::info("UIPlatform browser {:x} is gone; cleaning up session",
                         reinterpret_cast<std::uintptr_t>(target.browser));
            OnBrowserFocusGone(target.browser);
        }
        else
        {
            logger::info("Meridian view {:x} is gone; cleaning up session", target.view);
            OnViewFocusGone(target.view);
        }
        return;
    }
    const auto now = GetTickCount64();
    {
        // Deadline discipline (SessionGate-style): a commit whose "inserted"
        // acknowledgment never arrived must not wedge the queue forever.
        // Drop the chunk conservatively (it may or may not have landed) and
        // re-capture, so later text keeps flowing.
        std::lock_guard lock(s_pendingMutex);
        if (s_inFlightMs != 0 && now - s_inFlightMs >= COMMIT_ACK_TIMEOUT_MS)
        {
            logger::warn(
                "Meridian commit acknowledgment timed out ({} characters may not have been delivered); forcing re-capture",
                s_inFlight.size());
            s_inFlight.clear();
            s_inFlightMs = 0;
            s_captureReady = false;
        }
    }
    // Composition/candidate overlay: push while a composition is active, and
    // keep pushing after it ends until the empty script has landed once (that
    // is what hides the DOM panel). Runs before the capture branch so a
    // capture retry never starves the panel updates; without a captured field
    // the JS side no-ops.
    const bool composing = Ime::Core::State::GetInstance().IsImeInputting();
    if (composing || s_lastUiActive.load())
    {
        PushCandidateOverlay(target);
    }
    // Theme mirror: push the candidate window's M3 styling to the panel once
    // per session (and whenever the snapshot changed on the render thread).
    {
        std::lock_guard lock(s_uiMutex);
        if (s_captureReady.load() && !s_themeCss.empty() && s_themeCss != s_themeCssPushed)
        {
            const std::string script = "if(window.__simpleIME)window.__simpleIME.theme(" +
                                       JsStringOf(s_themeCss) + ");";
            if (ExecuteJs(target, script.c_str()))
            {
                s_themeCssPushed = s_themeCss;
            }
        }
    }
    if (!s_captureReady.load())
    {
        // Capture when there is something to deliver, while a composition is
        // active (the DOM panel needs its anchor before the first commit, and
        // the ImGui candidate window is suppressed for the whole session), or
        // once right after a focus gain so the DOM leak baseline exists before
        // the user's first keystroke. Avoids injecting a probe script every
        // frame while the user browses the view without typing. The first
        // commit after a focus gain pays a ~1 frame capture round-trip (CEF
        // thread) before it is flushed.
        const bool initialCapture = s_initialCapturePending.exchange(false);
        {
            std::lock_guard lock(s_pendingMutex);
            if (s_pending.empty() && !composing && !initialCapture)
            {
                return;
            }
        }
        TryCapture(target, now);
        return;
    }
    FlushPending(target, now);
}
} // namespace Hooks::MeridianBridge
