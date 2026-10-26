'use strict';
// The viewer's dashboard (VIEWER_DASHBOARD_HTML) renders /api/sensors the way the server's
// dashboard renders /api/clients: site sections with client status dots, sensor cards
// ordered by type, Float Switch ON/OFF, FAULT, mA, contents, stale badge, snoozed
// reminders, and an Active Alarms section on top. The page script runs under node with a
// small DOM stub and a canned /api/sensors response.
// Usage: node viewer_dashboard_test.js [path/to/Viewer.ino]
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

// Minimal element: enough of the DOM for the page script. innerHTML is kept as a string;
// appended children are kept in a list, and html() flattens both for assertions.
class El {
  constructor(id) {
    this.id = id || '';
    this.className = '';
    this.textContent = '';
    this.innerHTML = '';
    this.title = '';
    this.type = '';
    this.disabled = false;
    this.style = {};
    this.attrs = {};
    this.dataset = {};
    this.children = [];
    this.listeners = {};
    const self = this;
    this.classList = {
      add(c) { const s = new Set(self.className.split(' ').filter(Boolean)); s.add(c); self.className = [...s].join(' '); },
      remove(c) { self.className = self.className.split(' ').filter((x) => x && x !== c).join(' '); },
      toggle(c, on) { if (on) this.add(c); else this.remove(c); },
      contains(c) { return self.className.split(' ').includes(c); },
    };
  }
  set innerHTML(v) { this._html = v; this.children = []; }
  get innerHTML() { return this._html; }
  appendChild(c) { this.children.push(c); return c; }
  setAttribute(k, v) { this.attrs[k] = String(v); }
  addEventListener(t, fn) { (this.listeners[t] = this.listeners[t] || []).push(fn); }
  focus() { focused = this; }
  contains(el) { return this.all().includes(el); }
  // Class selectors only ('.name'), which is all the page asks for.
  querySelectorAll(sel) { const c = sel.replace(/^\./, ''); return this.all().slice(1).filter((e) => e.classList.contains(c)); }
  click() { (this.listeners.click || []).forEach((fn) => fn.call(this, {})); }
  html() { return this._html + this.textContent + this.children.map((c) => c.html()).join(''); }
  all() { return [this].concat(...this.children.map((c) => c.all())); }
}

let focused = null;
const NOW = 1_800_000_000;  // fixed clock (seconds)
const sensors = [
  // Site A: one client with a float in alarm (reminders snoozed), a gas sensor and a tank.
  { c: 'dev:A1', s: 'Site A', n: 'High Float', k: 3, ot: 'tank', st: 'digital', l: 1, mu: 'inches', d: 1, a: true, at: 'triggered', sz: NOW - 600, u: NOW - 300, v: 12.6 },
  { c: 'dev:A1', s: 'Site A', n: 'Line Pressure', k: 2, ot: 'gas', l: 41.25, mu: 'psi', a: false, at: 'clear', u: NOW - 300, v: 12.6 },
  { c: 'dev:A1', s: 'Site A', n: 'Main <Tank>', un: 7, k: 1, ot: 'tank', st: 'currentLoop', l: 50.5, ma: 11.234, ct: 'Diesel', d: -2.04, a: false, at: 'clear', u: NOW - 300, v: 12.6 },
  // Site B: a client that has not reported for three days, with a faulted sensor and no label.
  { c: 'dev:B1', s: 'Site B', n: '', k: 1, ot: 'tank', l: 0, mu: 'inches', flt: 'open', a: false, at: 'clear', u: NOW - 3 * 86400 },
];
const apiSensors = { vn: 'Shop Viewer', vi: 'dev:V', sn: 'HQ Server', si: 'dev:S', ge: NOW - 1800, nf: NOW + 5400, rs: 21600, sensors };

const elements = {};
const document = {
  getElementById: (id) => (elements[id] = elements[id] || new El(id)),
  createElement: () => new El(),
  get activeElement() { return focused; },
};
const fetched = [];
function fetchStub(url, opts) {
  fetched.push({ url, method: (opts && opts.method) || 'GET' });
  const body = url === '/api/sensors' ? apiSensors
    : url === '/api/github/update' ? { available: true, latestVersion: '9.9.9', releaseUrl: 'javascript:alert(1)' }
      : url === '/api/request-update' ? { success: true }
        : {};
  return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(body) });
}
const timers = [];
const intervals = [];
const ctx = {
  document,
  fetch: fetchStub,
  AbortController: class { constructor() { this.signal = {}; } abort() {} },
  setTimeout: (fn) => { timers.push(fn); return timers.length; },
  clearTimeout: () => {},
  setInterval: (fn, ms) => { intervals.push({ fn, ms }); return intervals.length; },
  Date: class extends Date { static now() { return NOW * 1000; } },
  console,
  Promise,
  Set,
  Object,
  Array,
  Math,
  String,
  Number,
  isFinite,
  isNaN,
};
vm.createContext(ctx);

const page = pageText();
const scripts = [...page.matchAll(/<script>([\s\S]*?)<\/script>/g)].map((m) => m[1]);
check('page has the overlay failsafe and the main script', scripts.length === 2, scripts.length);
vm.runInContext(scripts[scripts.length - 1], ctx);

(async () => {
  for (let i = 0; i < 5; i++) await new Promise((r) => setImmediate(r));

  check('loads /api/sensors', fetched.some((f) => f.url === '/api/sensors'));
  check('stat clients', elements.statClients.textContent === 2, elements.statClients.textContent);
  check('stat sensors', elements.statSensors.textContent === 4, elements.statSensors.textContent);
  check('stat alarms', elements.statAlarms.textContent === 1, elements.statAlarms.textContent);
  check('alarm stat highlighted', elements.statAlarmCard.classList.contains('alarm'));
  check('stat stale counts the 3-day-old sensor', elements.statStale.textContent === 1, elements.statStale.textContent);
  check('stale label names the threshold', elements.statStaleLabel.textContent === 'Stale (>49h)', elements.statStaleLabel.textContent);
  check('summary age', elements.statAge.textContent === '30m ago', elements.statAge.textContent);
  check('summary age not flagged inside two periods', !elements.statAgeCard.classList.contains('warn'));
  check('next fetch', elements.statNext.textContent === 'in 1h 30m', elements.statNext.textContent);
  check('meta line', elements.metaServer.textContent === 'Server: HQ Server (dev:S)', elements.metaServer.textContent);

  const sections = elements.siteSections.children;
  check('one section per site, sorted', sections.length === 2 && sections[0].html().includes('Site A'), sections.length);
  const siteA = sections[0];
  const cardsA = siteA.children[2].children.map((c) => c.innerHTML);
  // Grouped by object type like the server (tank before gas); a float on a tank sorts with the tanks.
  check('site A cards ordered by type', cardsA.length === 3 &&
    cardsA[0].includes('Float Switch') && cardsA[1].includes('Tank Level') && cardsA[2].includes('Gas Pressure'), cardsA.join('\n'));
  const [float, tank, gas] = cardsA;
  check('label escaped and Display Number shown', tank.includes('Main &lt;Tank&gt; #7'), tank);
  check('contents shown', tank.includes('<div class="dc-contents">Diesel</div>'), tank);
  check('tank with no unit in feet and inches, like the server', tank.includes('<div class="dc-value">4\' 2.5" <small></small></div>'), tank);
  check('mA shown', tank.includes('11.23 mA'), tank);
  check('24 h change', tank.includes('<span class="dc-change neg">-2.0 in/24h</span>'), tank);
  check('VIN shown', tank.includes('VIN: 12.60V'), tank);
  check('gas unit', gas.includes('41.3 <small>psi</small>'), gas);
  check('float ON with no unit', float.includes('<div class="dc-value">ON <small></small></div>'), float);
  check('float has no 24 h change', !float.includes('/24h'), float);
  check('float alarm with snoozed reminders', float.includes('ALARM: triggered - reminders snoozed 10m ago'), float);
  check('site with an alarm gets the red edge', siteA.children[0].style.borderLeft === '4px solid var(--danger)');

  const siteB = sections[1];
  const faultCard = siteB.children[2].children[0].innerHTML;
  check('fault shown instead of a value', faultCard.includes('FAULT <small>open</small>') && !faultCard.includes('dc-ma'), faultCard);
  check('unlabelled sensor is "Sensor"', faultCard.includes('<div class="dc-name">Sensor</div>'), faultCard);
  check('stale badge', faultCard.includes('<span class="status-pill stale">Stale</span>'), faultCard);

  const dots = siteA.children[0].children[1].children;
  check('one status dot per client', dots.length === 1 && dots[0].className === 'cdot red', dots.map((d) => d.className).join());
  check('site B dot is yellow (stale)', siteB.children[0].children[1].children[0].className === 'cdot yellow');
  dots[0].click();
  const infoA = elements.siteSections.children[0].children[1].innerHTML;
  check('focus stays on the clicked dot', focused && focused.dataset.key === 'Site A|dev:A1' && focused.attrs['aria-expanded'] === 'true');
  check('dot opens client details', infoA.includes('dev:A1') && infoA.includes('3 sensors') && infoA.includes('VIN: 12.60 V'), infoA);
  elements.siteSections.children[0].children[0].children[1].children[0].click();
  check('dot click again closes details', elements.siteSections.children[0].children[1].innerHTML === '', elements.siteSections.children[0].children[1].innerHTML);

  // The 60 s refresh re-renders every dot; a dot that had focus hands it to its replacement.
  const refresh = intervals.find((t) => t.ms === 60000);
  check('page refreshes every 60 s', !!refresh);
  const before = elements.siteSections.children[1].children[0].children[1].children[0];
  before.focus();
  refresh.fn();
  for (let i = 0; i < 5; i++) await new Promise((r) => setImmediate(r));
  check('auto-refresh keeps focus on the same client dot', focused !== before && focused.dataset.key === 'Site B|dev:B1' &&
    elements.siteSections.contains(focused), focused && focused.dataset.key);
  // Focus elsewhere on the page is left alone.
  elements.requestUpdateBtn.focus();
  refresh.fn();
  for (let i = 0; i < 5; i++) await new Promise((r) => setImmediate(r));
  check('auto-refresh leaves other focus alone', focused === elements.requestUpdateBtn);

  check('alarm section visible', elements.alarmSection.classList.contains('visible'));
  const alarmCards = elements.alarmGrid.children.map((c) => c.innerHTML);
  check('alarm section lists the alarm with its site', alarmCards.length === 1 && alarmCards[0].includes('Site A &mdash; High Float'), alarmCards.join('\n'));
  check('alarm count', elements.alarmCount.textContent === '1 alarm', elements.alarmCount.textContent);

  check('update banner shown', elements.updBanner.style.display === 'flex' && elements.updVersion.textContent === 'v9.9.9');
  check('non-https release link hidden', elements.updLinkWrap.style.display === 'none' && elements.updLink.href === undefined);

  elements.requestUpdateBtn.click();
  for (let i = 0; i < 5; i++) await new Promise((r) => setImmediate(r));
  check('Request Update posts', fetched.some((f) => f.url === '/api/request-update' && f.method === 'POST'));
  check('Request Update confirms', elements.toast.textContent.startsWith('Update requested'), elements.toast.textContent);

  // A snooze set before the server's clock synced is stored as epoch 1: no age is shown for it.
  apiSensors.sensors = [Object.assign({}, sensors[0], { sz: 1 })];
  vm.runInContext(scripts[scripts.length - 1], ctx);
  for (let i = 0; i < 5; i++) await new Promise((r) => setImmediate(r));
  const pre = elements.alarmGrid.children.map((c) => c.innerHTML).join('');
  check('pre-sync snooze shows no age', pre.includes('ALARM: triggered - reminders snoozed</div>'), pre);

  // A summary older than two publish periods is flagged.
  apiSensors.ge = NOW - 3 * 21600;
  apiSensors.sensors = [];
  fetched.length = 0;
  // Re-run the refresh through a fresh copy of the script (its refresh function is private).
  vm.runInContext(scripts[scripts.length - 1], ctx);
  for (let i = 0; i < 5; i++) await new Promise((r) => setImmediate(r));
  check('old summary flagged', elements.statAgeCard.classList.contains('warn'));
  check('empty summary message', elements.siteSections.innerHTML.includes('No sensor data yet'), elements.siteSections.innerHTML);
  check('no alarms hides the section', !elements.alarmSection.classList.contains('visible'));

  console.log(failures ? failures + ' of ' + checks + ' checks FAILED' : 'viewer dashboard: all ' + checks + ' checks passed');
  process.exit(failures ? 1 : 0);
})().catch((e) => { console.log('FAIL ' + (e && e.stack || e)); process.exit(1); });
