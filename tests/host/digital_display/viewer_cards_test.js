'use strict';
// S3 (C-A04 B8): the viewer's sensor card (renderCard in VIEWER_DASHBOARD_HTML) shows a float
// (st === 'digital') as ON/OFF with no unit and no 24 h change, titled "Float Switch" like the
// server dashboard (CR-10b); other cards are unchanged.
// Usage: node viewer_cards_test.js [path/to/Viewer.ino]
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const SKETCH = process.argv[2] ||
  path.resolve(__dirname, '../../../TankAlarm-112025-Viewer-BluesOpta/TankAlarm-112025-Viewer-BluesOpta.ino');

let checks = 0;
let failures = 0;
function check(name, ok, detail) {
  checks++;
  if (!ok) { failures++; console.log('FAIL ' + name + (detail !== undefined ? ': ' + detail : '')); }
}

function pageText() {
  const text = fs.readFileSync(SKETCH, 'utf8');
  const open = 'static const char VIEWER_DASHBOARD_HTML[] PROGMEM = R"HTML(';
  const s = text.indexOf(open);
  if (s < 0) throw new Error('VIEWER_DASHBOARD_HTML not found');
  const e = text.indexOf(')HTML";', s);
  if (e < 0) throw new Error('VIEWER_DASHBOARD_HTML not terminated');
  return text.slice(s + open.length, e).split(')HTML" R"HTML(').join('');
}

// Index just past the '}' closing the body that opens at the first '{' at or after `from`;
// skips quoted strings (renderCard builds its HTML with '...' strings) and block comments.
function bodyEnd(src, from) {
  let depth = 0;
  for (let i = src.indexOf('{', from); i < src.length; i++) {
    const c = src[i];
    if (c === '"' || c === "'" || c === '`') {
      for (i++; i < src.length && src[i] !== c; i++) {
        if (src[i] === '\\') i++;
        else if (c === '`' && src[i] === '$' && src[i + 1] === '{') throw new Error('template substitution in renderCard');
      }
    } else if (c === '/' && src[i + 1] === '*') {
      i = src.indexOf('*/', i + 2) + 1;
    } else if (c === '{') {
      depth++;
    } else if (c === '}') {
      depth--;
      if (depth === 0) return i + 1;
    }
  }
  throw new Error('unbalanced braces');
}

const page = pageText();
const marker = 'function renderCard(';
const at = page.indexOf(marker);
if (at < 0) throw new Error('renderCard not found');
if (page.indexOf(marker, at + 1) >= 0) throw new Error('more than one renderCard');
const src = page.slice(at, bodyEnd(page, page.indexOf(')', at)));

const ctx = {
  document: { createElement: () => ({ className: '', classList: { add() {} }, innerHTML: '' }) },
  unitLabel: (mu) => mu || 'in',
  escapeHtml: (s) => String(s),
  typeLabel: (ot) => ot,
  timeAgo: () => '5m ago',
};
vm.createContext(ctx);
vm.runInContext(src, ctx);
const html = (t) => ctx.renderCard(t, false).innerHTML;

const on = html({ st: 'digital', l: 1, mu: 'in', d: 1, n: 'High Float', a: true, at: 'triggered', u: 1 });
check('float ON value', on.includes('<div class="dc-value">ON <small></small></div>'), on);
check('float has no 24 h change', !on.includes('/24h'), on);
check('float titled Float Switch like the server dashboard', on.includes('<div class="dc-type">Float Switch</div>'), on);
const off = html({ st: 'digital', l: 0, mu: 'in', n: 'Low Float', u: 1 });
check('float OFF value', off.includes('<div class="dc-value">OFF <small></small></div>'), off);
const none = html({ st: 'digital', n: 'New Float' });
check('float without a value', none.includes('<div class="dc-value">- <small></small></div>'), none);
const analog = html({ st: 'analog', l: 43.8, mu: 'in', d: 1.2, n: 'Tank', u: 1 });
check('analog value unchanged', analog.includes('<div class="dc-value">43.8 <small>in</small></div>'), analog);
check('analog 24 h change unchanged', analog.includes('+1.2 in/24h'), analog);
check('analog titled by object type', analog.includes('<div class="dc-type">tank</div>'), analog);
const legacy = html({ l: 12.34, mu: 'in', n: 'Old record' });
check('record without st unchanged', legacy.includes('<div class="dc-value">12.3 <small>in</small></div>'), legacy);

console.log(failures ? failures + ' of ' + checks + ' checks FAILED' : 'viewer cards: all ' + checks + ' checks passed');
process.exit(failures ? 1 : 0);
