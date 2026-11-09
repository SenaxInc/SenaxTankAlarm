// Host test for the Google Apps Script email bridge that the server's /email-setup page
// hands out (the `var CODE=[...]` array inside EMAIL_SETUP_HTML).
//
//   node email_bridge_test.js        (or: make test)
//
// The script is read from the sketch, so the test covers exactly what operators paste into
// Apps Script. It runs in a node `vm` context with stubs for the Apps Script services it
// uses: ContentService, CacheService, LockService, MailApp, Utilities, console and Date.now.
//
// Time is a fake clock (env.clock, read by the script's Date.now()). Utilities.sleep moves
// it on, a busy lock makes tryLock wait, and env.serviceMs makes cache calls and taking the
// lock cost time. By default those cost nothing.
//
// Two executions overlap by nesting: `duringSend` runs a second call inside the first
// call's MailApp.sendEmail. The first call cannot move on until the second returns, so
// while the second call waits (Utilities.sleep), `onSleep` applies the first call's send
// outcome to the cache the way the script does (see firstCallOutcome).

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
    onSleep: null,          // run after each Utilities.sleep: what other executions did meanwhile
    sleptHoldingLock: 0,    // Utilities.sleep calls made while holding the script lock
    logs: [],               // console.log lines
    clock: 1790000000000,   // Date.now() in the script, ms
    serviceMs: 0,           // time each cache call, and taking a free lock, takes
    lockFreeAt: 0,          // another execution holds the script lock until this time
    busyAfterSleep: 0,      // after each Utilities.sleep, another execution holds the lock this long
  };
  function maybeFail(method) {
    if (env.failing.indexOf(method) >= 0) throw new Error(method + ': service unavailable');
  }
  function cacheCall(method) {
    maybeFail(method);
    if (!env.lockHeld) env.unlockedCacheOps++;
    env.clock += env.serviceMs;
  }
  const cache = {
    getAll(keys) {
      cacheCall('getAll');
      const found = {};
      keys.forEach(k => { if (env.cache.has(k)) found[k] = env.cache.get(k); });
      return found;
    },
    putAll(values, ttl) {
      cacheCall('putAll');
      env.ttls.push(ttl);
      Object.keys(values).forEach(k => env.cache.set(k, values[k]));
    },
    removeAll(keys) {
      cacheCall('removeAll');
      keys.forEach(k => env.cache.delete(k));
    },
  };
  const lock = {
    tryLock(ms) {
      env.lockWaits.push(ms);
      maybeFail('tryLock');
      // How long another execution still holds the lock; always, when !lockOk
      const busy = env.lockOk ? Math.max(0, env.lockFreeAt - env.clock) : Infinity;
      if (env.lockHeld || busy >= ms) {
        env.clock += ms;  // gives up after waiting the whole timeout
        return false;
      }
      env.clock += busy + env.serviceMs;
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
    Utilities: {
      sleep: ms => {
        env.sleeps.push(ms);
        if (env.lockHeld) env.sleptHoldingLock++;
        env.clock += ms;
        if (env.busyAfterSleep) env.lockFreeAt = env.clock + env.busyAfterSleep;
        if (env.onSleep) env.onSleep(ms);
      },
    },
    Date: { now: () => env.clock },
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

// What the first call's send outcome does to its claim: marks it 'sent', or on failure
// removes the keys that are still 'sending'.
function firstCallOutcome(br, keys, sent) {
  keys.forEach(k => {
    if (sent) br.cache.set(k, 'sent');
    else if (br.cache.get(k) === 'sending') br.cache.delete(k);
  });
}

// Runs a second call inside the first call's send. Its own send works (failSends is for
// the first call), and once it has slept `after` times, outcome() runs.
function repeatDuringSend(br, payload, after, outcome) {
  const out = {};
  br.duringSend = () => {
    const fails = br.failSends;
    br.failSends = 0;
    br.onSleep = () => { if (br.sleeps.length === after && outcome) outcome(); };
    out.answer = br.post(payload);
    br.onSleep = null;
    br.failSends = fails;
  };
  return out;
}

function cacheValues(br, keys) {
  return JSON.stringify(keys.map(k => br.cache.get(k)));
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
  checkEq('(a) both marked sent after the send', cacheValues(br, ['ev:ev-0001', 'id:' + id1]), '["sent","sent"]');
  checkEq('(a) sent mark is kept 6 hours', br.ttls[1], 21600);
  checkEq('(a) no wait without another call sending', br.sleeps.length, 0);
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
  checkEq('(e) claim released under the lock', br.unlockedCacheOps, 0);
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

// removeAll throwing after a failed send: the send error is still the one reported, and
// the claim left behind does not stop a repeat from sending (it waits 20 s, then sends).
// The services take no time here, so the wait is 20 sleeps of 1 s.
{
  const br = makeBridge(source, { failing: ['removeAll'] });
  br.failSends = 2;
  checkEq('removeAll throws: send error reported', br.post(routed('ev-0310', alarmBody('x-4'))),
          'error: Error: Service invoked too many times for one day: email.');
  checkEq('removeAll throws: lock not left held', br.lockHeld, false);
  checkEq('removeAll throws: claim left behind', cacheValues(br, ['ev:ev-0310', 'id:x-4']), '["sending","sending"]');
  br.failing = [];
  br.sleeps = [];
  checkEq('removeAll throws: repeat sends after waiting', br.post(routed('ev-0310', alarmBody('x-4'))), 'ok');
  checkEq('removeAll throws: repeat waited 20 x 1 s', JSON.stringify(br.sleeps), JSON.stringify(Array(20).fill(1000)));
  checkEq('removeAll throws: repeat sent', br.sent.length, 1);
  checkEq('removeAll throws: then marked sent', cacheValues(br, ['ev:ev-0310', 'id:x-4']), '["sent","sent"]');
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

// (i2) S3: a float (sensorType "digital") prints ON/OFF and no mA; analog lines are unchanged
{
  const br = makeBridge(source);
  checkEq('(i2) daily with floats ok', br.post(routed('ev-0502', {
    to: 'ops@example.com', subject: 'Daily Sensor Summary - 2026-09-24', id: 'dev:1-1790000420-9',
    sensors: [
      { client: 'dev:4', site: 'East', label: 'High Float', sensorIndex: 3, levelInches: 1,
        sensorMa: 0, alarm: true, alarmType: 'triggered', sensorType: 'digital' },
      { client: 'dev:4', site: 'East', label: 'Low Float', sensorIndex: 4, levelInches: 0,
        sensorMa: 0, alarm: false, alarmType: 'clear', sensorType: 'digital' },
      { client: 'dev:2', site: 'Silas', label: 'Cox Wellhead', sensorIndex: 1, levelInches: 43.8,
        sensorMa: 8.2, alarm: false, alarmType: 'clear' },
    ],
  })), 'ok');
  checkEq('(i2) float lines', (br.sent[0] || {}).body, [
    'East High Float #3: ON  ** ALARM: triggered **',
    'East Low Float #4: OFF',
    'Silas Cox Wellhead #1: 43.8 (8.2 mA)',
  ].join('\n'));
}

// (k) a repeat that arrives while the first call is sending waits, and is a duplicate
// once that send has worked
{
  const br = makeBridge(source);
  const id = 'dev:864475000000001-1790000360-7';
  const keys = ['ev:ev-0600', 'id:' + id];
  const repeat = repeatDuringSend(br, routed('ev-0600', alarmBody(id)), 2, () => firstCallOutcome(br, keys, true));
  checkEq('(k) slow first call returns ok', br.post(routed('ev-0600', alarmBody(id))), 'ok');
  checkEq('(k) repeat during the send returns duplicate', repeat.answer, 'duplicate');
  checkEq('(k) repeat checked again every second until sent', JSON.stringify(br.sleeps), '[1000,1000]');
  checkEq('(k) lock waits: 10 s to claim or check, 2 s per check while waiting',
          JSON.stringify(br.lockWaits), '[10000,10000,2000,2000,10000]');
  checkEq('(k) repeat did not hold the lock while waiting', br.sleptHoldingLock, 0);
  checkEq('(k) one email', br.sent.length, 1);
  checkEq('(k) marked sent', cacheValues(br, keys), '["sent","sent"]');
}

// (l) a repeat that arrives while the first call is sending, and that send fails twice:
// the claim goes, so the repeat sends the email
{
  const br = makeBridge(source);
  const id = 'dev:864475000000001-1790000420-8';
  const keys = ['ev:ev-0700', 'id:' + id];
  br.failSends = 2;
  const repeat = repeatDuringSend(br, routed('ev-0700', alarmBody(id)), 3, () => firstCallOutcome(br, keys, false));
  const first = br.post(routed('ev-0700', alarmBody(id)));
  checkEq('(l) repeat sends once the first call has failed', repeat.answer, 'ok');
  check('(l) first call reports its error', typeof first === 'string' && first.indexOf('error: ') === 0, first);
  checkEq('(l) waits: 3 checks by the repeat, 2 s before the second attempt',
          JSON.stringify(br.sleeps), '[1000,1000,1000,2000]');
  checkEq('(l) one email', br.sent.length, 1);
  checkEq('(l) failed first call left the repeat\'s sent mark alone', cacheValues(br, keys), '["sent","sent"]');
  checkEq('(l) later call is a duplicate', br.post(routed('ev-0700', alarmBody(id))), 'duplicate');
  checkEq('(l) still one email', br.sent.length, 1);
  checkEq('(l) cache used only under the lock', br.unlockedCacheOps, 0);
}

// (m) the first call is still sending after the repeat has waited 20 s: the repeat sends
// anyway (a rare duplicate beats a lost alert). Here the first call then fails twice.
{
  const br = makeBridge(source);
  const id = 'dev:864475000000001-1790000480-9';
  const keys = ['ev:ev-0800', 'id:' + id];
  br.failSends = 2;
  const repeat = repeatDuringSend(br, routed('ev-0800', alarmBody(id)));
  const first = br.post(routed('ev-0800', alarmBody(id)));
  checkEq('(m) repeat sends after waiting 20 s', repeat.answer, 'ok');
  checkEq('(m) repeat waited 20 x 1 s', JSON.stringify(br.sleeps.slice(0, 20)), JSON.stringify(Array(20).fill(1000)));
  checkEq('(m) repeat logs why it sent', br.logs[0], 'still sending elsewhere after 20 s, sent anyway: ' + keys.join(' '));
  check('(m) first call reports its error', typeof first === 'string' && first.indexOf('error: ') === 0, first);
  checkEq('(m) one email', br.sent.length, 1);
  checkEq('(m) failed first call left the repeat\'s sent mark alone', cacheValues(br, keys), '["sent","sent"]');
  checkEq('(m) later call is a duplicate', br.post(routed('ev-0800', alarmBody(id))), 'duplicate');
}

// (n) as (m), but the first call's send then works: two emails
{
  const br = makeBridge(source);
  const id = 'dev:864475000000001-1790000540-10';
  const keys = ['ev:ev-0900', 'id:' + id];
  const repeat = repeatDuringSend(br, routed('ev-0900', alarmBody(id)));
  checkEq('(n) first call returns ok', br.post(routed('ev-0900', alarmBody(id))), 'ok');
  checkEq('(n) repeat sends after waiting 20 s', repeat.answer, 'ok');
  checkEq('(n) two emails', br.sent.length, 2);
  checkEq('(n) marked sent', cacheValues(br, keys), '["sent","sent"]');
}

// (o) the server's own retry (new event, same message id) during the first send waits on
// the message id, and sends if the first send fails
{
  const br = makeBridge(source);
  const id = 'dev:864475000000001-1790000600-11';
  br.failSends = 2;
  const repeat = repeatDuringSend(br, routed('ev-1001', alarmBody(id)), 1,
                                  () => firstCallOutcome(br, ['ev:ev-1000', 'id:' + id], false));
  const first = br.post(routed('ev-1000', alarmBody(id)));
  checkEq('(o) second note sends once the first has failed', repeat.answer, 'ok');
  check('(o) first note reports its error', typeof first === 'string' && first.indexOf('error: ') === 0, first);
  checkEq('(o) one email', br.sent.length, 1);
  checkEq('(o) retry of the first event is a duplicate', br.post(routed('ev-1000', alarmBody(id))), 'duplicate');
}

const STILL_SENDING = 'still sending elsewhere after 20 s, sent anyway: ';

// (p) the wait is timed by the clock from before the first check, not by counting checks:
// the first check waits 9 s for the lock and the services take 300 ms a call, and the
// repeat stops checking once 20 s have passed, and still sends
{
  const br = makeBridge(source);
  const id = 'dev:864475000000001-1790000660-12';
  const keys = ['ev:ev-1100', 'id:' + id];
  keys.forEach(k => br.cache.set(k, 'sending'));  // left behind by a call whose cleanup failed
  br.serviceMs = 300;
  const start = br.clock;
  br.lockFreeAt = start + 9000;
  checkEq('(p) slow start: repeat sends after waiting', br.post(routed('ev-1100', alarmBody(id))), 'ok');
  const took = br.clock - start;
  checkEq('(p) slow start: first check allows 10 s for the lock', br.lockWaits[0], 10000);
  check('(p) slow start: fewer than 20 checks', br.sleeps.length < 20, br.sleeps.length);
  check('(p) slow start: waited 20 s and answered within 23 s', took >= 20000 && took <= 23000, took);
  checkEq('(p) slow start: logs why it sent', br.logs[0], STILL_SENDING + keys.join(' '));
  checkEq('(p) slow start: sent', br.sent.length, 1);
  checkEq('(p) slow start: then marked sent', cacheValues(br, keys), '["sent","sent"]');
}

// (q) other executions hold the lock for the whole wait: each check gives up after 2 s and
// counts as still sending, so the repeat still answers within about 23 s, and sends
{
  const br = makeBridge(source);
  const id = 'dev:864475000000001-1790000720-13';
  const keys = ['ev:ev-1200', 'id:' + id];
  keys.forEach(k => br.cache.set(k, 'sending'));
  br.busyAfterSleep = Infinity;
  const start = br.clock;
  checkEq('(q) busy lock: repeat sends after waiting', br.post(routed('ev-1200', alarmBody(id))), 'ok');
  const took = br.clock - start;
  check('(q) busy lock: waited 20 s and answered within 23 s', took >= 20000 && took <= 23000, took);
  checkEq('(q) busy lock: 10 s for the first check, then 2 s for each later lock',
          JSON.stringify(br.lockWaits), JSON.stringify([10000].concat(Array(8).fill(2000))));
  checkEq('(q) busy lock: logs why it sent', br.logs[0], STILL_SENDING + keys.join(' '));
  checkEq('(q) busy lock: sent', br.sent.length, 1);
  checkEq('(q) busy lock: lock not left held', br.lockHeld, false);
}

// (r) the lock is busy for 1.5 s at each check: the check still gets it within its 2 s, and
// the repeat is a duplicate once the first call's send has worked
{
  const br = makeBridge(source);
  const id = 'dev:864475000000001-1790000780-14';
  const keys = ['ev:ev-1300', 'id:' + id];
  br.busyAfterSleep = 1500;
  const repeat = repeatDuringSend(br, routed('ev-1300', alarmBody(id)), 3, () => firstCallOutcome(br, keys, true));
  const start = br.clock;
  checkEq('(r) slow lock: first call returns ok', br.post(routed('ev-1300', alarmBody(id))), 'ok');
  checkEq('(r) slow lock: repeat returns duplicate', repeat.answer, 'duplicate');
  checkEq('(r) slow lock: repeat checked 3 times', JSON.stringify(br.sleeps), '[1000,1000,1000]');
  checkEq('(r) slow lock: each check got the lock after 1.5 s', br.clock - start, 3 * 2500);
  checkEq('(r) slow lock: lock waits of 2 s while waiting', JSON.stringify(br.lockWaits),
          '[10000,10000,2000,2000,2000,10000]');
  checkEq('(r) slow lock: one email', br.sent.length, 1);
  checkEq('(r) slow lock: marked sent', cacheValues(br, keys), '["sent","sent"]');
}

console.log('email_bridge: ' + checks + ' checks, ' + failures + ' failures');
process.exit(failures === 0 ? 0 : 1);
