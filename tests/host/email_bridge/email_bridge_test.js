// Host test for the Google Apps Script email bridge that the server's /email-setup page
// hands out (the `var CODE=[...]` array inside EMAIL_SETUP_HTML).
//
//   node email_bridge_test.js        (or: make test)
//
// The script is read from the sketch, so the test covers exactly what operators paste into
// Apps Script. It runs in a node `vm` context with stubs for the Apps Script services it
// uses: ContentService, CacheService, LockService, MailApp, Utilities and console.

'use strict';

const fs = require('fs');
const path = require('path');
const vm = require('vm');

const SKETCH = path.resolve(__dirname, '../../../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino');
const SECRET = 'CHANGE-ME';

let checks = 0;
let failures = 0;

function check(name, ok, detail) {
  checks++;
  if (!ok) {
    failures++;
    console.log('FAIL ' + name + (detail !== undefined ? ': ' + detail : ''));
  }
}

function checkEq(name, actual, expected) {
  check(name, actual === expected, 'got ' + JSON.stringify(actual) + ', expected ' + JSON.stringify(expected));
}

// ---------------------------------------------------------------------------
// Extract the script from the sketch
// ---------------------------------------------------------------------------

// Returns the index just past the `]` that closes the array literal opening at `open`.
// Skips JS string literals so brackets inside the script lines do not count.
function arrayLiteralEnd(text, open) {
  let depth = 0;
  for (let i = open; i < text.length; i++) {
    const c = text[i];
    if (c === '"' || c === "'") {
      for (i++; i < text.length && text[i] !== c; i++) {
        if (text[i] === '\\') i++;
      }
    } else if (c === '[') {
      depth++;
    } else if (c === ']') {
      depth--;
      if (depth === 0) return i + 1;
    }
  }
  throw new Error('unterminated array literal');
}

function loadBridgeScript() {
  const text = fs.readFileSync(SKETCH, 'utf8');
  const start = text.indexOf('static const char EMAIL_SETUP_HTML[] PROGMEM = R"HTML(');
  if (start < 0) throw new Error('EMAIL_SETUP_HTML not found in ' + SKETCH);
  const end = text.indexOf(')HTML"', start);
  if (end < 0) throw new Error('EMAIL_SETUP_HTML is not terminated');
  const page = text.slice(start, end);
  const marker = 'var CODE=[';
  const at = page.indexOf(marker);
  if (at < 0) throw new Error('var CODE=[ not found in EMAIL_SETUP_HTML');
  if (page.indexOf(marker, at + 1) >= 0) throw new Error('more than one var CODE=[ in EMAIL_SETUP_HTML');
  const open = at + marker.length - 1;
  const literal = page.slice(open, arrayLiteralEnd(page, open));
  const lines = vm.runInNewContext('(' + literal + ')');
  if (!Array.isArray(lines) || !lines.every(l => typeof l === 'string')) {
    throw new Error('CODE is not an array of strings');
  }
  return lines.join('\n');
}

// ---------------------------------------------------------------------------
// Apps Script stubs
// ---------------------------------------------------------------------------

function makeBridge(source, opts) {
  opts = opts || {};
  const env = {
    sent: [],               // MailApp.sendEmail arguments, in order
    cache: new Map(),       // CacheService script cache (TTL ignored)
    ttls: [],               // expirationInSeconds passed to putAll
    lockWaits: [],          // timeoutInMillis passed to tryLock
    lockHeld: false,
    lockReleases: 0,
    lockOk: opts.lockOk !== false,
    failing: opts.failing || [],  // service methods that throw, e.g. ['tryLock']
    unlockedCacheOps: 0,    // getAll/putAll calls made without the script lock
    failSends: 0,           // this many sendEmail calls throw before one succeeds
    duringSend: null,       // run once inside the next sendEmail: a call that arrives meanwhile
    sleeps: [],             // Utilities.sleep arguments
    logs: [],               // console.log lines
  };
  function maybeFail(method) {
    if (env.failing.indexOf(method) >= 0) throw new Error(method + ': service unavailable');
  }
  const cache = {
    getAll(keys) {
      maybeFail('getAll');
      if (!env.lockHeld) env.unlockedCacheOps++;
      const found = {};
      keys.forEach(k => { if (env.cache.has(k)) found[k] = env.cache.get(k); });
      return found;
    },
    putAll(values, ttl) {
      maybeFail('putAll');
      if (!env.lockHeld) env.unlockedCacheOps++;
      env.ttls.push(ttl);
      Object.keys(values).forEach(k => env.cache.set(k, values[k]));
    },
    removeAll(keys) {
      maybeFail('removeAll');
      keys.forEach(k => env.cache.delete(k));
    },
  };
  const lock = {
    tryLock(ms) {
      env.lockWaits.push(ms);
      maybeFail('tryLock');
      if (!env.lockOk || env.lockHeld) return false;  // busy, or held by another execution
      env.lockHeld = true;
      return true;
    },
    releaseLock() {
      env.lockHeld = false;
      env.lockReleases++;
      maybeFail('releaseLock');
    },
  };
  const context = vm.createContext({
    ContentService: { createTextOutput: t => ({ text: t }) },
    CacheService: { getScriptCache: () => { maybeFail('getScriptCache'); return cache; } },
    LockService: { getScriptLock: () => { maybeFail('getScriptLock'); return lock; } },
    MailApp: {
      sendEmail(msg) {
        const during = env.duringSend;
        env.duringSend = null;
        if (during) during();
        if (env.failSends > 0) {
          env.failSends--;
          throw new Error('Service invoked too many times for one day: email.');
        }
        env.sent.push(msg);
      },
    },
    Utilities: { sleep: ms => { env.sleeps.push(ms); } },
    console: { log: line => { env.logs.push(String(line)); } },
  });
  vm.runInContext(source, context, { filename: 'Code.gs' });
  if (typeof context.doPost !== 'function') throw new Error('script does not define doPost');
  env.post = (payload, key) => {
    const out = context.doPost({
      parameter: key === null ? {} : { key: key === undefined ? SECRET : key },
      postData: { contents: JSON.stringify(payload) },
    });
    return out && out.text;
  };
  return env;
}

// A routed Notehub event (no transform): the note body sits under `body`.
function routed(eventUid, body) {
  const ev = { event: eventUid, device: 'dev:864475000000001', file: 'email.qo', req: 'note.add', body: body };
  if (eventUid === undefined) delete ev.event;
  return ev;
}

function alarmBody(id) {
  const b = { to: 'ops@example.com', subject: 'TankAlarm Alert', message: 'Silas #1 high alarm 43.8 in', type: 'alarm', _sv: 1 };
  if (id !== undefined) b.id = id;
  return b;
}

// ---------------------------------------------------------------------------
// Cases
// ---------------------------------------------------------------------------

let source;
try {
  source = loadBridgeScript();
} catch (err) {
  console.log('FAIL extract bridge script: ' + err.message);
  console.log('email_bridge: 1 checks, 1 failures');
  process.exit(1);
}

// (j) the script parses (the equivalent of node --check)
{
  let parsed = true;
  let detail;
  try {
    new vm.Script(source, { filename: 'Code.gs' });
  } catch (err) {
    parsed = false;
    detail = err.message;
  }
  check('(j) bridge script parses', parsed, detail);
  if (!parsed) {
    console.log('email_bridge: ' + checks + ' checks, ' + failures + ' failures');
    process.exit(1);
  }
  checkEq('(j) SECRET placeholder is CHANGE-ME', /var SECRET = 'CHANGE-ME';/.test(source), true);
}

// (a)-(d) one bridge, one cache, several calls
{
  const br = makeBridge(source);
  const id1 = 'dev:864475000000001-1790000000-1';
  checkEq('(a) first call returns ok', br.post(routed('ev-0001', alarmBody(id1))), 'ok');
  checkEq('(a) first call sends once', br.sent.length, 1);
  checkEq('(a) claim waits up to 10 s for the lock', br.lockWaits[0], 10000);
  checkEq('(a) claim is kept 6 hours', br.ttls[0], 21600);
  check('(a) event and message id are both claimed', br.cache.has('ev:ev-0001') && br.cache.has('id:' + id1),
        JSON.stringify([...br.cache.keys()]));
  checkEq('(a) lock released', br.lockHeld, false);

  checkEq('(b) same event again returns duplicate', br.post(routed('ev-0001', alarmBody(id1))), 'duplicate');
  checkEq('(b) same event again does not send', br.sent.length, 1);
  checkEq('(b) lock released after a duplicate', br.lockHeld, false);
  checkEq('(b) duplicate is logged for the Executions view',
          JSON.stringify(br.logs), JSON.stringify(['duplicate, not sent: ev:ev-0001 id:' + id1]));

  checkEq('(c) new event, same message id returns duplicate', br.post(routed('ev-0002', alarmBody(id1))), 'duplicate');
  checkEq('(c) new event, same message id does not send', br.sent.length, 1);

  const id2 = 'dev:864475000000001-1790000060-2';
  checkEq('(d) new event and new id returns ok', br.post(routed('ev-0003', alarmBody(id2))), 'ok');
  checkEq('(d) new event and new id sends', br.sent.length, 2);

  checkEq('(d) event id alone still dedupes (body without id)',
          br.post(routed('ev-0003', alarmBody())), 'duplicate');
  checkEq('(d) message id alone still dedupes (no event field)',
          br.post(routed(undefined, alarmBody(id2))), 'duplicate');
  checkEq('(d) nothing else sent', br.sent.length, 2);
  checkEq('(d) every lock taken was released', br.lockReleases, br.lockWaits.length);
  checkEq('(d) cache checked and claimed only under the lock', br.unlockedCacheOps, 0);
}

// (e) a send that throws twice releases the claim, so a retry of the same event can send
{
  const br = makeBridge(source);
  const id = 'dev:864475000000001-1790000120-3';
  br.failSends = 2;
  const first = br.post(routed('ev-0100', alarmBody(id)));
  check('(e) failed send reports the error', typeof first === 'string' && first.indexOf('error: ') === 0, first);
  checkEq('(e) failed send was tried again after 2 s', JSON.stringify(br.sleeps), '[2000]');
  checkEq('(e) failed send made both attempts', br.failSends, 0);
  checkEq('(e) failed send sent nothing', br.sent.length, 0);
  checkEq('(e) failed send released the claim', br.cache.size, 0);
  checkEq('(e) failed send released the lock', br.lockHeld, false);
  checkEq('(e) retry of the same event returns ok', br.post(routed('ev-0100', alarmBody(id))), 'ok');
  checkEq('(e) retry of the same event sends', br.sent.length, 1);
  checkEq('(e) a third call is a duplicate', br.post(routed('ev-0100', alarmBody(id))), 'duplicate');
  checkEq('(e) still one email', br.sent.length, 1);
}

// (f) no event field and no id: nothing to dedupe on, every call sends
{
  const br = makeBridge(source);
  checkEq('(f) routed, no event, no id: first call ok', br.post(routed(undefined, alarmBody())), 'ok');
  checkEq('(f) routed, no event, no id: second call ok', br.post(routed(undefined, alarmBody())), 'ok');
  checkEq('(f) bare body, no id: call ok', br.post(alarmBody()), 'ok');
  checkEq('(f) bare body, no id: repeat ok', br.post(alarmBody()), 'ok');
  checkEq('(f) every call sent', br.sent.length, 4);
  checkEq('(f) lock not taken without keys', br.lockWaits.length, 0);
  checkEq('(f) nothing cached', br.cache.size, 0);
}

// (g) lock unavailable: send anyway, without dedupe
{
  const br = makeBridge(source, { lockOk: false });
  const id = 'dev:864475000000001-1790000180-4';
  checkEq('(g) lock busy: first call ok', br.post(routed('ev-0200', alarmBody(id))), 'ok');
  checkEq('(g) lock busy: same event ok', br.post(routed('ev-0200', alarmBody(id))), 'ok');
  checkEq('(g) lock busy: both sent', br.sent.length, 2);
  checkEq('(g) lock busy: nothing cached', br.cache.size, 0);
  checkEq('(g) lock busy: no release without the lock', br.lockReleases, 0);

  br.cache.set('ev:ev-0201', '1');  // claimed by a concurrent call that holds the lock
  br.failSends = 2;
  const failed = br.post(routed('ev-0201', alarmBody('x-0')));
  check('(g) lock busy: failed send reports the error', typeof failed === 'string' && failed.indexOf('error: ') === 0, failed);
  checkEq('(g) lock busy: a failed send leaves other claims alone', br.cache.has('ev:ev-0201'), true);
}

// Lock or cache service throwing: send anyway, as when the lock is busy
[['getScriptCache'], ['getScriptLock'], ['tryLock'], ['releaseLock'], ['getAll', 'putAll']].forEach(failing => {
  const br = makeBridge(source, { failing: failing });
  const name = failing.join('/') + ' throws';
  checkEq(name + ': call ok', br.post(routed('ev-0300', alarmBody('x-1'))), 'ok');
  checkEq(name + ': sent', br.sent.length, 1);
  checkEq(name + ': lock not left held', br.lockHeld, false);
});

// removeAll throwing after a failed send: the send error is still the one reported
{
  const br = makeBridge(source, { failing: ['removeAll'] });
  br.failSends = 2;
  checkEq('removeAll throws: send error reported', br.post(routed('ev-0310', alarmBody('x-4'))),
          'error: Error: Service invoked too many times for one day: email.');
}

// (h) wrong or missing key: forbidden, no send, nothing claimed
{
  const br = makeBridge(source);
  checkEq('(h) wrong key returns forbidden', br.post(routed('ev-0400', alarmBody('x-2')), 'wrong'), 'forbidden');
  checkEq('(h) missing key returns forbidden', br.post(routed('ev-0400', alarmBody('x-2')), null), 'forbidden');
  checkEq('(h) forbidden sends nothing', br.sent.length, 0);
  checkEq('(h) forbidden claims nothing', br.cache.size, 0);
  checkEq('(h) right key afterwards still sends', br.post(routed('ev-0400', alarmBody('x-2'))), 'ok');
  checkEq('(h) no recipients is still reported',
          br.post(routed('ev-0401', { subject: 'x', message: 'y', id: 'x-3' })), 'no recipients');
  checkEq('(h) one email in total', br.sent.length, 1);
}

// (i) both body shapes render as before
{
  const br = makeBridge(source);
  checkEq('(i) alarm shape ok', br.post(routed('ev-0500', {
    to: 'ops@example.com, owner@example.com', subject: 'TankAlarm Reminder',
    message: 'Silas #1 high alarm still active', type: 'alarm', id: 'dev:1-1790000240-5',
  })), 'ok');
  const alarm = br.sent[0] || {};
  checkEq('(i) alarm recipients', alarm.to, 'ops@example.com,owner@example.com');
  checkEq('(i) alarm subject', alarm.subject, 'TankAlarm Reminder');
  checkEq('(i) alarm body is the message', alarm.body, 'Silas #1 high alarm still active');

  checkEq('(i) daily shape ok', br.post(routed('ev-0501', {
    to: 'ops@example.com', subject: 'Daily Sensor Summary - 2026-09-23', id: 'dev:1-1790000300-6',
    company: 'Acme Oil', serverName: 'Tank Alarm Server',
    sensors: [
      { client: 'dev:2', site: 'Silas', label: 'Cox Wellhead', sensorIndex: 1, levelInches: 43.8,
        sensorMa: 8.2, alarm: false, alarmType: 'clear' },
      { client: 'dev:3', site: 'North', label: 'Tank 2', sensorIndex: 2, levelInches: 10,
        alarm: true, alarmType: 'high' },
    ],
    fmt: { groupBySite: true },
  })), 'ok');
  const daily = br.sent[1] || {};
  checkEq('(i) daily recipients', daily.to, 'ops@example.com');
  checkEq('(i) daily subject', daily.subject, 'Daily Sensor Summary - 2026-09-23');
  checkEq('(i) daily body', daily.body, [
    'Acme Oil',
    'Tank Alarm Server',
    '',
    'Silas Cox Wellhead #1: 43.8 (8.2 mA)',
    'North Tank 2 #2: 10  ** ALARM: high **',
  ].join('\n'));
  checkEq('(i) two emails', br.sent.length, 2);
}

// (k) a repeat that arrives while the first call is still sending is a duplicate
{
  const br = makeBridge(source);
  const id = 'dev:864475000000001-1790000360-7';
  let repeat;
  br.duringSend = () => { repeat = br.post(routed('ev-0600', alarmBody(id))); };
  checkEq('(k) slow first call returns ok', br.post(routed('ev-0600', alarmBody(id))), 'ok');
  checkEq('(k) repeat during the send returns duplicate', repeat, 'duplicate');
  checkEq('(k) one email', br.sent.length, 1);
}

// (l) a send that fails once is tried again, so a repeat that came in during the failed
// attempt and was answered 'duplicate' loses nothing
{
  const br = makeBridge(source);
  const id = 'dev:864475000000001-1790000420-8';
  let repeat;
  br.failSends = 1;
  br.duringSend = () => { repeat = br.post(routed('ev-0700', alarmBody(id))); };
  checkEq('(l) first call returns ok after a second attempt', br.post(routed('ev-0700', alarmBody(id))), 'ok');
  checkEq('(l) repeat during the failed attempt returns duplicate', repeat, 'duplicate');
  checkEq('(l) waited 2 s before the second attempt', JSON.stringify(br.sleeps), '[2000]');
  checkEq('(l) one email', br.sent.length, 1);
  checkEq('(l) claim kept after the second attempt sent', br.cache.has('ev:ev-0700'), true);
}

console.log('email_bridge: ' + checks + ' checks, ' + failures + ' failures');
process.exit(failures === 0 ? 0 : 1);
