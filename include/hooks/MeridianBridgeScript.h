//
// The page-side half of the Meridian bridge script.
//
// Lives in its own header because the raw-string terminator hazard is a
// release-grade one (VALIDATION.md: a terminator that swallowed the IIFE
// closing paren once shipped a broken script inside the DLL) — and because
// test/MeridianBridge.cjs extracts exactly this file's literal to exercise
// the shipped bytes in Node.
//
#pragma once

namespace Hooks::MeridianBridge
{
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
inline constexpr char BridgeScript[] = R"IMEJS((function(){
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
} // namespace Hooks::MeridianBridge
