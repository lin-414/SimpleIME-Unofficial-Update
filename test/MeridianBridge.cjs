//
// Node-side regression test for the Meridian bridge script embedded in
// src/hooks/MeridianBridge.cpp.
//
// The script is extracted from the raw-string literal in the .cpp so the test
// always exercises exactly what ships. Run from the repository root:
//
//   node test/MeridianBridge.cjs
//
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('node:assert/strict');

const cpp = fs.readFileSync(path.join(__dirname, '..', 'src', 'hooks', 'MeridianBridge.cpp'), 'utf8');
// Extract with plain indexOf — semantics identical to C++ raw-string delimiters.
const startMarker = 'R"IMEJS(';
const endMarker = ')IMEJS"';
const start = cpp.indexOf(startMarker);
const end = cpp.indexOf(endMarker, start);
assert.ok(start >= 0 && end > start, 'BridgeScript raw string not found in MeridianBridge.cpp');
const source = cpp.slice(start + startMarker.length, end);

let calls = [];
let status = [];
let listeners = {};
const field = { tagName: 'INPUT', type: 'search', disabled: false, readOnly: false };
const doc = {
    activeElement: field,
    documentElement: { contains: (e) => e === field },
    execCommand: (cmd, show, text) => { calls.push([cmd, text]); return true; },
};
const window = {
    simpleIMEResult: (s) => status.push(s),
    addEventListener: (event, callback) => { listeners[event] = callback; },
};
const context = vm.createContext({ document: doc, window });
vm.runInContext(source, context);
const api = window.__simpleIME;

// capture → ready, commit → execCommand insertText
api.capture(1);
assert.equal(status.at(-1), '1:ready');
api.commit(1, '中文');
assert.deepEqual(calls.pop(), ['insertText', '中文']);

// Injection attempt must land as literal text only.
api.commit(1, '";window.attack=1;//😀');
assert.equal(window.attack, undefined);
assert.equal(calls.pop()[1], '";window.attack=1;//😀');

// Stale session id and stale field are rejected.
api.commit(0, 'old');
assert.equal(status.at(-1), '0:stale-field');
const other = { ...field };
doc.activeElement = other;
api.commit(1, 'wrong');
assert.equal(calls.length, 0);

// Readonly and password fields never receive text.
doc.activeElement = field;
field.readOnly = true;
api.commit(1, 'readonly');
assert.equal(calls.length, 0);
field.readOnly = false;
field.type = 'password';
api.capture(2);
assert.equal(status.at(-1), '2:no-field');
field.type = 'search';

// cancel() blocks pending commits for the session.
api.capture(3);
api.cancel(2);
api.commit(3, '有效');
assert.equal(calls.pop()[1], '有效');
api.cancel(3);
api.commit(3, 'cancelled');
assert.equal(calls.length, 0);

// A window blur invalidates the captured field.
api.capture(4);
listeners.blur();
api.commit(4, 'blur');
assert.equal(calls.length, 0);

// iframes are traversed to find the active field.
const iframe = { tagName: 'IFRAME', contentDocument: doc };
const outer = { activeElement: iframe };
context.document = outer;
api.capture(5);
api.commit(5, 'iframe');
assert.equal(calls.pop()[1], 'iframe');

// execCommand failure is reported, not swallowed.
doc.execCommand = () => false;
api.commit(5, 'reject');
assert.equal(status.at(-1), '5:insert-rejected');

// Re-injection is idempotent (guard against duplicate state objects).
vm.runInContext(source, context);
assert.equal(window.__simpleIME, api);

// --- composition/candidate overlay (ui) with a small DOM mock -------------

function makeElement(tag, ownerDoc) {
    return {
        tagName: String(tag).toUpperCase(),
        style: {},
        children: [],
        handlers: {},
        textContent: '',
        ownerDocument: ownerDoc,
        value: null,
        selectionStart: null,
        selectionEnd: null,
        appendChild(child) {
            this.children.push(child);
            this.textContent += child.textContent ?? '';
            return child;
        },
        remove() {},
        addEventListener(type, callback) { (this.handlers[type] ||= []).push(callback); },
        setSelectionRange(start, end) { this.selectionStart = start; this.selectionEnd = end; },
        getBoundingClientRect: () => ({ left: 100, top: 100, right: 300, bottom: 140 }),
        offsetWidth: 200,
        offsetHeight: 50,
    };
}
const uiField = makeElement('input');
uiField.type = 'search';
const uiCommands = [];
let uiStyleEl = null;
const uiDoc = {
    activeElement: uiField,
    documentElement: { contains: (e) => e === uiField, clientWidth: 1000, clientHeight: 800 },
    execCommand: (cmd, show, text) => { uiCommands.push([cmd, text]); return true; },
    getElementById: (id) => (uiStyleEl && uiStyleEl.id === id ? uiStyleEl : null),
};
uiDoc.createElement = (tag) => {
    const n = makeElement(tag, uiDoc);
    if (String(tag) === 'style') uiStyleEl = n;
    return n;
};
uiDoc.head = makeElement('head', uiDoc);
uiDoc.body = makeElement('body', uiDoc);
const uiStatus = [];
const uiWindow = {
    simpleIMEResult: (s) => uiStatus.push(s),
    addEventListener: () => {},
};
const uiContext = vm.createContext({ document: uiDoc, window: uiWindow });
vm.runInContext(source, uiContext);
const uiApi = uiWindow.__simpleIME;

// ui() before any capture must be a harmless no-op.
uiApi.ui('ailisi', ['爱丽丝'], 0);
assert.equal(uiDoc.body.children.length, 0);

// With a captured field the panel is created, anchored and populated, and the
// leak maintenance keeps the field clean: the host feeds the composition's
// keystrokes (inserts AND backspaces) into the field through its own input
// path.
uiField.value = 'ab';
uiField.selectionStart = 2;
uiApi.capture(7);
assert.equal(uiStatus.at(-1), '7:ready');
assert.deepEqual(uiCommands, []); // the baseline-setting capture edits nothing

// Composition start: the leaked 'q' already landed at the insertion point.
// The first push re-anchors to the pre-leak position by matching the
// composition string (raw keystrokes, no separators yet) — no edit yet.
uiField.value = 'abq';
uiField.selectionStart = 3;
uiApi.ui('q', ['1. 轻甲', '2. 裙甲'], 0);
assert.deepEqual(uiCommands, []);
const panel = uiDoc.body.children.at(-1);
assert.equal(panel.style.display, 'block');
assert.equal(panel.className, 'simpleime-panel');
assert.equal(panel.children[0].textContent, 'q');
// The M3 theme mirror: the default dark stylesheet ships with the bridge and
// is injected into the field's document; a host-pushed theme() replaces it.
assert.ok(uiStyleEl && uiStyleEl.textContent.includes('.simpleime-panel'));
assert.equal(uiStyleEl.ownerDocument, uiDoc);
uiApi.theme('.simpleime-panel{--simpleime-scale:1.25}');
assert.equal(uiStyleEl.textContent, '.simpleime-panel{--simpleime-scale:1.25}');
const chips = panel.children.find((c) => c.className === 'simpleime-chips').children;
assert.equal(chips.length, 2);
// Horizontal rows split the data layer's baked-in "{n}. {word}" prefix into a
// dim digit span hugging the word span — the ImGui window's Win11-style row.
assert.equal(chips[0].className, 'simpleime-chip simpleime-sel');
assert.equal(chips[1].className, 'simpleime-chip');
assert.deepEqual(chips[0].children.map((n) => [n.className, n.textContent]), [['simpleime-num', '1'], ['simpleime-word', '轻甲']]);
assert.deepEqual(chips[1].children.map((n) => [n.className, n.textContent]), [['simpleime-num', '2'], ['simpleime-word', '裙甲']]);
// A chip click reports pick:<index> on the session id.
chips[1].handlers.click[0]();
assert.equal(uiStatus.at(-1), '7:pick:1');

// Ongoing composition: leaked characters are deleted from the anchored point.
uiCommands.length = 0;
uiField.value = 'abqi';
uiField.selectionStart = 4;
uiApi.ui('qi', ['1. 轻甲'], 0);
assert.deepEqual(uiCommands, [['delete', null]]);

// A leak backspace deleted a REAL character (the leaked pinyin was already
// stripped) — it is restored from the previous value snapshot.
uiCommands.length = 0;
uiField.value = 'a';
uiField.selectionStart = 1;
uiApi.ui('q', ['1. 轻甲'], 0);
assert.deepEqual(uiCommands, [['insertText', 'b']]);

// English-mode backspacing between compositions leaves the mark stale; the
// next composition start re-anchors through the tail match, so the real text
// before the caret is never eaten.
uiApi.ui('', [], -1); // session ends: final pass clears the tracking state
uiCommands.length = 0;
uiField.value = 'aq';
uiField.selectionStart = 2;
uiApi.ui('q', ['1. 轻甲'], 0);
assert.deepEqual(uiCommands, []);
uiCommands.length = 0;
uiField.value = 'aqi';
uiField.selectionStart = 3;
uiApi.ui('qi', ['1. 轻甲'], 0);
assert.deepEqual(uiCommands, [['delete', null]]);

// A large insertion-point jump is a click, not the leak — re-anchor only.
uiCommands.length = 0;
uiField.value = '0123456789abcdefghijkl';
uiField.selectionStart = 20;
uiApi.ui('q', ['1. 轻甲'], 0);
assert.deepEqual(uiCommands, []);

// The commit inserts the committed text without touching the field further.
uiCommands.length = 0;
uiField.value = '';
uiField.selectionStart = 0;
uiApi.commit(7, '轻甲');
assert.deepEqual(uiCommands, [['insertText', '轻甲']]);
assert.equal(uiStatus.at(-1), '7:inserted');

// Vertical candidate mode: the ui push carries the mod's vertical setting and
// the panel switches to the menu-item look, keeping the full "{n}. {word}"
// label (no digit split on vertical rows).
uiCommands.length = 0;
uiField.value = 'abq';
uiField.selectionStart = 3;
uiApi.ui('q', ['1. 轻甲'], 0, true);
assert.equal(panel.className, 'simpleime-panel simpleime-vert');
const vertRow = panel.children.filter((c) => c.className === 'simpleime-chips').at(-1);
assert.deepEqual(vertRow.children[0].children.map((n) => [n.className, n.textContent]), []);
assert.equal(vertRow.children[0].textContent, '1. 轻甲');
uiApi.ui('q', ['1. 轻甲'], 0, false);
assert.equal(panel.className, 'simpleime-panel');

// The theme mirror can carry the configured primary font: a @font-face with a
// file URL plus the YaHei fallback chain.
uiApi.theme('@font-face{font-family:\'SimpleIME Primary\';src:url("file:///C:/WINDOWS/FONTS/MSYH.TTC")}' +
            '.simpleime-panel{font-family:\'SimpleIME Primary\',\'Microsoft YaHei\',sans-serif}');
assert.ok(uiStyleEl.textContent.includes('@font-face'));
assert.ok(uiStyleEl.textContent.includes('file:///C:/WINDOWS/FONTS/MSYH.TTC'));

// The session's final push runs one last strip pass (catches the commit key's
// own leaked space) and stops tracking.
uiCommands.length = 0;
uiField.value = ' ';
uiField.selectionStart = 1;
uiApi.ui('', [], -1);
assert.deepEqual(uiCommands, [['delete', null]]);
uiCommands.length = 0;
uiField.value = 'x';
uiField.selectionStart = 1;
uiApi.ui('', [], -1);
assert.deepEqual(uiCommands, []); // tracking ended, nothing further is deleted

// Empty state hides the panel (this is what the bridge pushes on commit).
uiField.value = '';
uiField.selectionStart = 0;
uiApi.ui('', [], -1);
assert.equal(panel.style.display, 'none');

// A stale field (focus moved on) hides the panel instead of following it.
uiApi.ui('qingjia', ['1. 轻甲'], 0);
assert.equal(panel.style.display, 'block');
uiDoc.activeElement = makeElement('input');
uiApi.ui('qingjia', ['1. 轻甲'], 0);
assert.equal(panel.style.display, 'none');

// --- status channels --------------------------------------------------------
// The View/1 mock above only has simpleIMEResult; the UIPlatform backend
// exposes SimpleIME.result instead, and the script must prefer it when both
// exist (a host never exposes both, but the preference must be deterministic).

function channelCheck(name, windowMock, expectedSink) {
    const channelDoc = {
        activeElement: field,
        documentElement: { contains: (e) => e === field },
        execCommand: (cmd, show, text) => true,
    };
    const channelContext = vm.createContext({ document: channelDoc, window: windowMock });
    vm.runInContext(source, channelContext);
    const api = windowMock.__simpleIME;
    api.capture(31);
    assert.equal(expectedSink.at(-1), '31:ready', name);
    api.commit(31, '好');
    assert.equal(expectedSink.at(-1), '31:inserted', name);
    // Idempotent re-injection on the second channel flavor as well.
    vm.runInContext(source, channelContext);
    assert.equal(windowMock.__simpleIME, api, name);
}

// UIPlatform-only host: the binding lives on window.SimpleIME.result.
{
    const seen = [];
    const win = {
        SimpleIME: { result: (s) => seen.push(s) },
        addEventListener: () => {},
    };
    channelCheck('SimpleIME.result channel', win, seen);
}

// Host with both channels: the UIPlatform binding wins.
{
    const viaBinding = [];
    const viaListener = [];
    const win = {
        SimpleIME: { result: (s) => viaBinding.push(s) },
        simpleIMEResult: (s) => viaListener.push(s),
        addEventListener: () => {},
    };
    channelCheck('SimpleIME.result preferred', win, viaBinding);
    assert.equal(viaListener.length, 0, 'simpleIMEResult untouched while the binding exists');
}

// Host with neither channel: reports are silently dropped, capture/commit
// still work (the bridge-side silence detector handles the dead listener).
{
    const win = { addEventListener: () => {} };
    const channelDoc = {
        activeElement: field,
        documentElement: { contains: (e) => e === field },
        execCommand: (cmd, show, text) => true,
    };
    const channelContext = vm.createContext({ document: channelDoc, window: win });
    vm.runInContext(source, channelContext);
    win.__simpleIME.capture(32); // must not throw
    win.__simpleIME.commit(32, '字'); // must not throw
}

console.log('Meridian DOM checks passed: Unicode, literal text, stale field/session, readonly/password, cancel, blur, iframe, insertion failure, overlay ui/pick/hide');
