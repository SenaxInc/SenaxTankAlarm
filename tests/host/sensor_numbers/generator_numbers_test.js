'use strict';
// Host test for stable sensor numbers on the server's Config Generator page (S-T01/S1).
//
//   node generator_numbers_test.js [path/to/TankAlarm-112025-Server-BluesOpta.ino]
//
// Needs node 18 or later and no packages. Reads the CONFIG_GENERATOR_HTML raw string from the
// server sketch, runs its numbering helpers (nextSensorNumber, sensorNumbersValid,
// loadSensorNumbers, cardSensorNumber) in a vm context, runs addSensor and startCommissioning
// against a small fake DOM, runs cachedSensorHigh, the import handler, submitConfig and the Device
// UID change handler against a stub fetch, and checks that the page uses the helpers where a
// sensor's number is created, sent, loaded and turned into an alarm-contact id.

const fs = require('fs');
const path = require('path');
const vm = require('vm');

const SKETCH = process.argv[2] ||
  path.resolve(__dirname, '../../../TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino');

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
  const a = JSON.stringify(actual);
  const e = JSON.stringify(expected);
  check(name, a === e, 'got ' + a + ', expected ' + e);
}

function generatorPage() {
  const text = fs.readFileSync(SKETCH, 'utf8');
  const open = 'static const char CONFIG_GENERATOR_HTML[] PROGMEM = R"HTML(';
  const s = text.indexOf(open);
  if (s < 0) throw new Error('CONFIG_GENERATOR_HTML not found');
  const e = text.indexOf(')HTML";', s);
  if (e < 0) throw new Error('CONFIG_GENERATOR_HTML not terminated');
  return text.slice(s + open.length, e).split(')HTML" R"HTML(').join('');
}

// Index just past the '}' that closes the body opening at the first '{' at or after `from`.
// Skips quoted strings, template literals and block comments. The extracted helpers use no ${}
// inside templates; the test fails loudly if that ever changes.
function bodyEnd(src, from) {
  let depth = 0;
  for (let i = src.indexOf('{', from); i < src.length; i++) {
    const c = src[i];
    if (c === '"' || c === "'" || c === '`') {
      for (i++; i < src.length && src[i] !== c; i++) {
        if (src[i] === '\\') i++;
        else if (c === '`' && src[i] === '$' && src[i + 1] === '{') throw new Error('template substitution in an extracted helper');
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

function extractFunction(page, name) {
  const marker = 'function ' + name + '(';
  const at = page.indexOf(marker);
  if (at < 0) throw new Error(name + ' not found in CONFIG_GENERATOR_HTML');
  if (page.indexOf(marker, at + 1) >= 0) throw new Error('more than one ' + name);
  return page.slice(at, bodyEnd(page, page.indexOf(')', at)));
}

// The same for an async function: the source must say "async function <name>(".
function extractAsyncFunction(page, name) {
  if (!page.includes('async function ' + name + '(')) throw new Error(name + ' is not an async function');
  return 'async ' + extractFunction(page, name);
}

// The arrow function that follows `marker` (which must occur once), e.g. an event handler.
function extractArrow(page, marker) {
  const at = page.indexOf(marker);
  if (at < 0) throw new Error(marker + ' not found in CONFIG_GENERATOR_HTML');
  if (page.indexOf(marker, at + 1) >= 0) throw new Error('more than one ' + marker);
  const start = at + marker.length;
  return page.slice(start, bodyEnd(page, page.indexOf('=>', start)));
}

const page = generatorPage();
const ctx = {};
vm.createContext(ctx);
for (const n of ['nextSensorNumber', 'sensorNumbersValid', 'loadSensorNumbers', 'cardSensorNumber']) {
  vm.runInContext(extractFunction(page, n), ctx);
}
const { nextSensorNumber, sensorNumbersValid, loadSensorNumbers, cardSensorNumber } = ctx;
// Results cross the vm boundary; compare them as plain JSON.
const load = (sensors, snh, floor) => JSON.parse(JSON.stringify(loadSensorNumbers(sensors, snh, floor)));
const card = (n) => ({ dataset: { sensorNumber: String(n) } });

// nextSensorNumber: max(numbers in use, high-water mark) + 1
checkEq('next of an empty client', nextSensorNumber([], 0), 1);
checkEq('next of [1,2,3]', nextSensorNumber([1, 2, 3], 3), 4);
checkEq('next after removing #2', nextSensorNumber([1, 3], 3), 4);
checkEq('next after removing the newest (#4)', nextSensorNumber([1, 3], 4), 5);
checkEq('next ignores an invalid high-water mark', nextSensorNumber([2], 999), 3);
checkEq('next ignores a fractional high-water mark', nextSensorNumber([2], 7.5), 3);
checkEq('next ignores card numbers of 0', nextSensorNumber([0, 0], 0), 1);
checkEq('next can pass 255 (addSensor refuses it)', nextSensorNumber([255], 0), 256);

// The page flow: add three, remove #2, add, remove the newest, add.
{
  let high = 0;
  let nums = [];
  const add = () => { const n = nextSensorNumber(nums, high); high = n; nums.push(n); return n; };
  add(); add(); add();
  checkEq('three new sensors', nums, [1, 2, 3]);
  nums = nums.filter((n) => n !== 2);
  checkEq('remove #2 and add: #3 keeps its number', (add(), nums), [1, 3, 4]);
  nums = nums.filter((n) => n !== 4);
  checkEq('remove the newest and add: its number is not reused', (add(), nums), [1, 3, 5]);
}

// sensorNumbersValid: every number an integer 1-255, no duplicates
check('valid [1,3,4]', sensorNumbersValid([1, 3, 4]));
check('valid [255]', sensorNumbersValid([255]));
check('duplicate is invalid', !sensorNumbersValid([1, 1]));
check('0 is invalid', !sensorNumbersValid([0]));
check('256 is invalid', !sensorNumbersValid([256]));
check('1.5 is invalid', !sensorNumbersValid([1.5]));
check('"1" is invalid', !sensorNumbersValid(['1']));
check('undefined is invalid', !sensorNumbersValid([undefined]));

// loadSensorNumbers: explicit numbers 1-255 first; with snh, anything else is renumbered past the
// high mark and reported; without snh (older configs), a missing or unusable number is the position
// when free (as the client runs it), and 0 or a duplicate is renumbered past the mark.
checkEq('legacy config (index+1, no snh)', load([{ number: 1 }, { number: 2 }, { number: 3 }]),
  { nums: [1, 2, 3], high: 3, repaired: [] });
checkEq('gaps and snh kept', load([{ number: 1 }, { number: 3 }, { number: 4 }], 5),
  { nums: [1, 3, 4], high: 5, repaired: [] });
checkEq('snh below the numbers in use', load([{ number: 6 }], 2), { nums: [6], high: 6, repaired: [] });
checkEq('missing number without snh is the position', load([{}, {}]), { nums: [1, 2], high: 2, repaired: [] });
checkEq('missing number without snh: position taken, so renumbered', load([{ number: 1 }, {}, { number: 2 }]),
  { nums: [1, 3, 2], high: 3, repaired: [{ position: 2, number: 3 }] });
checkEq('missing number with snh is renumbered past it', load([{ number: 1 }, {}], 5),
  { nums: [1, 6], high: 6, repaired: [{ position: 2, number: 6 }] });
checkEq('null number with snh is renumbered past it', load([{ number: null }], 3),
  { nums: [4], high: 4, repaired: [{ position: 1, number: 4 }] });
checkEq('not an object with snh is renumbered past it', load([5, { number: 1 }], 2),
  { nums: [3, 1], high: 3, repaired: [{ position: 1, number: 3 }] });
// A sensor without a number never takes an explicit number from a later sensor (it used to take
// #1 by position, push the real #1 to #6 and name the wrong sensor in the warning).
checkEq('explicit #1 kept ahead of a missing number (snh)', load([{}, { number: 1 }], 5),
  { nums: [6, 1], high: 6, repaired: [{ position: 1, number: 6 }] });
checkEq('explicit #1 kept ahead of a missing number (no snh)', load([{}, { number: 1 }]),
  { nums: [2, 1], high: 2, repaired: [{ position: 1, number: 2 }] });
checkEq('explicit #2 kept ahead of a duplicate #2', load([{ number: 3 }, { number: 3 }, { number: 2 }], 3),
  { nums: [3, 4, 2], high: 4, repaired: [{ position: 2, number: 4 }] });
checkEq('duplicate repaired', load([{ number: 2 }, { number: 2 }]),
  { nums: [2, 3], high: 3, repaired: [{ position: 2, number: 3 }] });
checkEq('default that collides is repaired', load([{ number: 2 }, {}], 4),
  { nums: [2, 5], high: 5, repaired: [{ position: 2, number: 5 }] });
checkEq('zero repaired', load([{ number: 0 }]), { nums: [1], high: 1, repaired: [{ position: 1, number: 1 }] });
// A repair never goes past 255: it takes the lowest number not on the page and says so.
checkEq('repair at 255 takes the lowest free number', load([{ number: 255 }, { number: 1 }, { number: 255 }]),
  { nums: [255, 1, 2], high: 255, repaired: [{ position: 3, number: 2, reused: true }] });
checkEq('repair when snh is 255', load([{ number: 3 }, { number: 3 }], 255),
  { nums: [3, 1], high: 255, repaired: [{ position: 2, number: 1, reused: true }] });
checkEq('repair up to 255, then reuse', load([{ number: 254 }, { number: 0 }, { number: 0 }]),
  { nums: [254, 255, 1], high: 255, repaired: [{ position: 2, number: 255 }, { position: 3, number: 1, reused: true }] });
check('repaired numbers are always sendable',
  sensorNumbersValid(load([{ number: 255 }, { number: 255 }, { number: 0 }, {}], 255).nums));
// With snh, a number that is present but unusable is renumbered past the mark and reported.
checkEq('snh 5 and number 300: renumbered past snh', load([{ number: 300 }], 5),
  { nums: [6], high: 6, repaired: [{ position: 1, number: 6 }] });
for (const bad of [1.5, '3', -1, 256, true]) {
  checkEq('snh 5 and number ' + JSON.stringify(bad) + ': renumbered past snh', load([{ number: bad }], 5),
    { nums: [6], high: 6, repaired: [{ position: 1, number: 6 }] });
}
// Without snh, the client runs a number it cannot use (is<uint8_t>() fails) as its position, so the
// page keeps the position (the sensor's history, contacts and calibration stay on it) and warns.
checkEq('number 300 without snh is reported', load([{ number: 300 }]),
  { nums: [1], high: 1, repaired: [{ position: 1, number: 1 }] });
checkEq('number 300 without snh keeps the position the client runs', load([{ number: 300 }, { number: 2 }, { number: 3 }]),
  { nums: [1, 2, 3], high: 3, repaired: [{ position: 1, number: 1 }] });
for (const bad of [-1, 1.5, '3', null, 256, true]) {
  checkEq('number ' + JSON.stringify(bad) + ' without snh keeps the position', load([{ number: 1 }, { number: bad }]),
    { nums: [1, 2], high: 2, repaired: [{ position: 2, number: 2 }] });
}
checkEq('number "2" without snh is reported, not taken as 2', load([{ number: 1 }, { number: '2' }]),
  { nums: [1, 2], high: 2, repaired: [{ position: 2, number: 2 }] });
checkEq('number 300 without snh: position taken, so renumbered', load([{ number: 300 }, { number: 1 }]),
  { nums: [2, 1], high: 2, repaired: [{ position: 1, number: 2 }] });
// 0 is a number the client does run (k=0), and a duplicate is really in use, so neither takes the
// position: both are renumbered past the mark, as before.
checkEq('zero without snh does not take the position', load([{ number: 0 }, { number: 5 }]),
  { nums: [6, 5], high: 6, repaired: [{ position: 1, number: 6 }] });
checkEq('duplicate without snh does not take the position', load([{ number: 3 }, { number: 3 }, { number: 1 }]),
  { nums: [3, 4, 1], high: 4, repaired: [{ position: 2, number: 4 }] });
checkEq('warnings are listed in page order', load([{ number: 0 }, { number: 300 }]),
  { nums: [3, 2], high: 3, repaired: [{ position: 1, number: 3 }, { position: 2, number: 2 }] });
checkEq('snh 0 counts as present', load([{}], 0), { nums: [1], high: 1, repaired: [{ position: 1, number: 1 }] });
checkEq('no sensors keep snh', load(undefined, 4), { nums: [], high: 4, repaired: [] });
checkEq('invalid snh ignored', load([{ number: 1 }], 'x'), { nums: [1], high: 1, repaired: [] });

// The floor (the mark the server holds for the client) raises the high mark, so a sensor added
// or repaired after loading an older file never reuses a number the server has already used.
checkEq('floor above the file: kept numbers, high mark raised', load([{ number: 1 }, { number: 2 }], undefined, 5),
  { nums: [1, 2], high: 5, repaired: [] });
checkEq('floor above the file: next number passes it', nextSensorNumber([1, 2], load([{ number: 1 }, { number: 2 }], undefined, 5).high), 6);
checkEq('floor above snh: a repair lands past the floor', load([{ number: 1 }, { number: 0 }], 2, 5),
  { nums: [1, 6], high: 6, repaired: [{ position: 2, number: 6 }] });
checkEq('floor above snh: a duplicate lands past the floor', load([{ number: 2 }, { number: 2 }], 2, 5),
  { nums: [2, 6], high: 6, repaired: [{ position: 2, number: 6 }] });
// hasSnh still comes from the file: without snh the position rule is unchanged under a floor.
checkEq('no snh under a floor: missing and unusable numbers keep the position', load([{}, { number: 'x' }], undefined, 5),
  { nums: [1, 2], high: 5, repaired: [{ position: 2, number: 2 }] });
checkEq('no snh under a floor: missing numbers are the positions', load([{}, {}], undefined, 5),
  { nums: [1, 2], high: 5, repaired: [] });
checkEq('no snh under a floor: 0 lands past the floor', load([{ number: 0 }, { number: 1 }], undefined, 5),
  { nums: [6, 1], high: 6, repaired: [{ position: 1, number: 6 }] });
checkEq('no snh under a floor: a taken position lands past the floor', load([{ number: 1 }, {}, { number: 2 }], undefined, 5),
  { nums: [1, 6, 2], high: 6, repaired: [{ position: 2, number: 6 }] });
checkEq('floor below snh never lowers it', load([{ number: 1 }], 7, 5), { nums: [1], high: 7, repaired: [] });
checkEq('floor below the numbers in use never lowers it', load([{ number: 9 }], undefined, 5), { nums: [9], high: 9, repaired: [] });
checkEq('floor with no sensors', load(undefined, undefined, 5), { nums: [], high: 5, repaired: [] });
checkEq('floor 255: a repair reuses the lowest free number', load([{ number: 3 }, { number: 3 }], undefined, 255),
  { nums: [3, 1], high: 255, repaired: [{ position: 2, number: 1, reused: true }] });
for (const bad of [0, -1, 256, 5.5, '5', null, undefined, true]) {
  checkEq('floor ' + JSON.stringify(bad) + ' ignored', load([{ number: 1 }, { number: 2 }], undefined, bad),
    { nums: [1, 2], high: 2, repaired: [] });
}

// cardSensorNumber: the number stored on the card, 0 when there is none
checkEq('card number', cardSensorNumber(card(7)), 7);
checkEq('card number 0', cardSensorNumber(card(0)), 0);
checkEq('card number 256', cardSensorNumber(card(256)), 0);
checkEq('card without a number', cardSensorNumber({ dataset: {} }), 0);
checkEq('no card', cardSensorNumber(null), 0);

// startCommissioning: a client with no stored config starts at #1, even when the page still holds
// another client's cards as a template. Runs the page's addSensor and startCommissioning against a
// fake DOM; addSensorCard, showToast and renderMsgContacts are stubs.
let flowTests = null;
{
  let cards = [];
  let nextId = 0;
  const rendered = [];
  const fakeCard = (num, msgOpen) => {
    const title = { textContent: 'Sensor #' + num };
    return {
      id: 'sensor-' + nextId++,
      dataset: { sensorNumber: String(num) },
      querySelector(sel) {
        if (sel === '.sensor-title') return title;
        if (sel === '.msg-section.visible') return msgOpen ? {} : null;
        return null;
      },
    };
  };
  const container = {
    querySelectorAll(sel) {
      if (sel !== '.sensor-card') throw new Error('unexpected container query ' + sel);
      return cards.slice();
    },
  };
  Object.assign(ctx, {
    CLIENT_MAX_MONITORS: 8,
    els: { clientUid: { value: 'dev:A' } },
    document: {
      getElementById: (id) => (id === 'sensorsContainer' ? container : null),
      querySelectorAll(sel) {
        if (sel !== '#sensorsContainer .sensor-card') throw new Error('unexpected query ' + sel);
        return cards.slice();
      },
      querySelector(sel) { return this.querySelectorAll(sel)[0] || null; },
    },
    showToast: () => {},
    renderMsgContacts: (id) => rendered.push(id),
    addSensorCard: (num) => cards.push(fakeCard(num, false)),
  });
  vm.runInContext('let sensorNumberHigh=0;', ctx);
  for (const n of ['addSensor', 'startCommissioning']) vm.runInContext(extractFunction(page, n), ctx);
  const high = () => vm.runInContext('sensorNumberHigh', ctx);
  const nums = () => cards.map(cardSensorNumber);

  // Client A is loaded: #1, #3 (contact list open), #4, and it once used #6.
  cards = [fakeCard(1, false), fakeCard(3, true), fakeCard(4, false)];
  vm.runInContext('sensorNumberHigh=6;', ctx);
  ctx.startCommissioning('dev:B');
  checkEq('commissioning renumbers template cards from 1', nums(), [1, 2, 3]);
  checkEq('commissioning retitles template cards', cards.map((c) => c.querySelector('.sensor-title').textContent),
    ['Sensor #1', 'Sensor #2', 'Sensor #3']);
  checkEq('commissioning resets the high-water mark', high(), 3);
  checkEq('commissioning sets the new client', ctx.els.clientUid.value, 'dev:B');
  checkEq('commissioning redraws the open contact list', rendered, [1]);
  ctx.addSensor();
  checkEq('a sensor added after commissioning is next', nums(), [1, 2, 3, 4]);

  // Every card removed, then a client with no stored config: its first sensor is #1.
  cards = [];
  ctx.startCommissioning('dev:C');
  checkEq('commissioning an empty page starts at #1', nums(), [1]);
  checkEq('high-water mark after commissioning an empty page', high(), 1);

  // The page learns the server's mark (P323 review): importing a file and sending a config both
  // read GET /api/client, so Add Sensor never reuses a number the server has already used. Runs the
  // page's cachedSensorHigh, the import handler, submitConfig and the Device UID change handler
  // against a stub fetch. loadConfig is a stub that runs the loader's two numbering statements.
  const toasts = [];
  let stored = {};        // uid -> GET /api/client reply body; a missing uid is a 404
  let getFails = false;   // GET /api/client throws (server unreachable)
  let postStatus = 200;
  let postText = 'OK';
  let onGet = null;       // runs while a GET is in flight
  const gets = [];
  const posts = [];
  ctx.showToast = (msg, kind) => toasts.push({ msg: String(msg), kind });
  ctx.fetch = async (url, opts) => {
    if (url === '/api/config') {
      posts.push(JSON.parse(opts.body));
      return { status: postStatus, statusText: '', text: async () => postText };
    }
    const prefix = '/api/client?uid=';
    if (!url.startsWith(prefix)) throw new Error('unexpected fetch ' + url);
    gets.push(url);
    if (onGet) { const f = onGet; onGet = null; f(); }
    if (getFails) throw new Error('network down');
    const uid = decodeURIComponent(url.slice(prefix.length));
    if (!(uid in stored)) return { ok: false, status: 404, json: async () => { throw new Error('no body'); } };
    return { ok: true, status: 200, json: async () => JSON.parse(JSON.stringify(stored[uid])) };
  };
  ctx.__nums = nums;
  ctx.__setCards = (list) => { cards = list.map((n) => fakeCard(n, false)); };
  ctx.retryBtn = { style: {} };
  ctx.syncBtn = { style: {} };
  vm.runInContext(extractAsyncFunction(page, 'cachedSensorHigh'), ctx);
  vm.runInContext(extractAsyncFunction(page, 'submitConfig'), ctx);
  // The loader's numbering, as window.loadConfig runs it (pinned below); null throws like the page.
  vm.runInContext('var loadConfig=function(c,floor){if(c===null)throw new TypeError("null config");' +
    'const loadedNumbers=loadSensorNumbers(c.sensors,c.snh,floor);sensorNumberHigh=loadedNumbers.high;' +
    '__setCards(loadedNumbers.nums);};', ctx);
  // collectConfig's number handling: each card's number, snh = max(mark, numbers in use).
  vm.runInContext('var collectConfig=function(){const ns=__nums();return {sensors:ns.map(n=>({number:n})),' +
    'snh:Math.max(sensorNumberHigh,...ns)};};', ctx);
  const onload = vm.runInContext('(' + extractArrow(page, 'const r=new FileReader();r.onload=') + ')', ctx);
  const onUidChange = vm.runInContext('(' + extractArrow(page, "if(els.clientUid)els.clientUid.addEventListener('change',") + ')', ctx);
  const setHigh = (n) => vm.runInContext('sensorNumberHigh=' + n + ';', ctx);
  const importFile = (text) => onload({ target: { result: text } });
  const submit = () => ctx.submitConfig({ preventDefault() {} });
  const reset = () => {
    stored = {}; getFails = false; postStatus = 200; postText = 'OK'; onGet = null;
    gets.length = 0; posts.length = 0; toasts.length = 0;
    cards = []; setHigh(0); ctx.els.clientUid.value = 'dev:A';
  };
  // Client dev:A once used #1, #2, #4 and #5; #3 and #5 were removed (the server's mark is 5).
  const clientA = { config: { snh: 5, sensors: [{ number: 1 }, { number: 2 }, { number: 4 }] } };

  flowTests = async () => {
    // cachedSensorHigh
    reset();
    stored = { 'dev:A': clientA };
    checkEq('cached mark is the stored snh', await ctx.cachedSensorHigh('dev:A'), 5);
    checkEq('cached mark reads the client by its encoded UID', gets, ['/api/client?uid=dev%3AA']);
    stored = { 'dev:A': { config: { snh: 3, sensors: [{ number: 7 }, 'x', null] } } };
    checkEq('cached mark is the highest stored number above snh', await ctx.cachedSensorHigh('dev:A'), 7);
    stored = { 'dev:A': { config: { sensors: [{ number: 255 }] } } };
    checkEq('cached mark at 255', await ctx.cachedSensorHigh('dev:A'), 255);
    stored = { 'dev:A': { config: { snh: 999, sensors: 'x' } } };
    checkEq('cached mark ignores a bad stored config', await ctx.cachedSensorHigh('dev:A'), 0);
    stored = { 'dev:A': {} };
    checkEq('cached mark without a stored config', await ctx.cachedSensorHigh('dev:A'), 0);
    stored = {};
    checkEq('cached mark on 404', await ctx.cachedSensorHigh('dev:A'), 0);
    getFails = true;
    checkEq('cached mark when the server cannot be reached', await ctx.cachedSensorHigh('dev:A'), 0);
    gets.length = 0;
    checkEq('cached mark without a UID', await ctx.cachedSensorHigh(''), 0);
    checkEq('no request without a UID', gets, []);

    // Import an older file (no snh) of client A, then Add Sensor: #6, not #3 or #5.
    reset();
    stored = { 'dev:A': clientA };
    await importFile(JSON.stringify({ deviceUid: ' dev:A ', sensors: [{ number: 1 }, { number: 2 }] }));
    checkEq('import reads the file client\'s mark', gets, ['/api/client?uid=dev%3AA']);
    checkEq('import keeps the file\'s numbers', nums(), [1, 2]);
    checkEq('import raises the mark to the server\'s', high(), 5);
    ctx.addSensor();
    checkEq('import then Add Sensor passes the server\'s mark', nums(), [1, 2, 6]);

    // A repair made while loading also lands past the server's mark.
    reset();
    stored = { 'dev:A': clientA };
    await importFile(JSON.stringify({ deviceUid: 'dev:A', snh: 2, sensors: [{ number: 1 }, { number: 0 }] }));
    checkEq('import repairs past the server\'s mark', nums(), [1, 6]);

    // A file whose mark is above the server's keeps its own.
    reset();
    stored = { 'dev:A': clientA };
    await importFile(JSON.stringify({ deviceUid: 'dev:A', snh: 7, sensors: [{ number: 1 }] }));
    checkEq('import never lowers the file\'s mark', high(), 7);

    // 404, an unreachable server and a file without a UID behave as before the fix.
    for (const [label, setup, file] of [
      ['404', () => { stored = {}; }, { deviceUid: 'dev:A', sensors: [{ number: 1 }, { number: 2 }] }],
      ['server unreachable', () => { stored = { 'dev:A': clientA }; getFails = true; }, { deviceUid: 'dev:A', sensors: [{ number: 1 }, { number: 2 }] }],
      ['no UID in the file', () => { stored = { 'dev:A': clientA }; }, { sensors: [{ number: 1 }, { number: 2 }] }],
    ]) {
      reset();
      setup();
      await importFile(JSON.stringify(file));
      ctx.addSensor();
      checkEq('import, ' + label + ': numbered from the file as before', [nums(), high()], [[1, 2, 3], 3]);
    }
    reset();
    stored = { 'dev:A': clientA };
    await importFile(JSON.stringify({ deviceUid: 'dev:A', sensors: [{ number: 1 }, { number: 2 }] }).replace('{', '{,'));
    checkEq('import of invalid JSON', [toasts.map((t) => t.msg), gets.length, nums()], [['Invalid JSON'], 0, []]);
    reset();
    await importFile('null');
    checkEq('import of null', [toasts.map((t) => t.msg), gets.length], [['Invalid JSON'], 0]);

    // Send a config loaded while the server was unreachable, then Add Sensor: #6.
    for (const [status, text] of [[200, 'OK'], [200, 'WARNING: saved, but ...'], [202, 'Config saved locally']]) {
      reset();
      stored = { 'dev:A': clientA };
      getFails = true;
      await importFile(JSON.stringify({ deviceUid: 'dev:A', sensors: [{ number: 1 }, { number: 2 }] }));
      checkEq('submit ' + status + ' ' + text.slice(0, 7) + ': starts from the file\'s mark', high(), 2);
      getFails = false;
      postStatus = status;
      postText = text;
      gets.length = 0;
      await submit();
      checkEq('submit ' + status + ' ' + text.slice(0, 7) + ': sends to the client', [posts.length, posts[0] && posts[0].client], [1, 'dev:A']);
      checkEq('submit ' + status + ' ' + text.slice(0, 7) + ': reads the server\'s mark', gets, ['/api/client?uid=dev%3AA']);
      checkEq('submit ' + status + ' ' + text.slice(0, 7) + ': raises the mark', high(), 5);
      ctx.addSensor();
      checkEq('submit ' + status + ' ' + text.slice(0, 7) + ' then Add Sensor passes the server\'s mark', nums(), [1, 2, 6]);
    }
    // A refused send leaves the mark and makes no extra request.
    reset();
    stored = { 'dev:A': clientA };
    ctx.__setCards([1, 2]);
    setHigh(2);
    postStatus = 400;
    postText = 'Sensor numbers must be unique';
    await submit();
    checkEq('refused submit leaves the mark', [high(), gets.length], [2, 0]);
    // 404 or an unreachable server after a good send: the sent mark, as before.
    for (const [label, setup] of [['404', () => { stored = {}; }], ['server unreachable', () => { getFails = true; }]]) {
      reset();
      stored = { 'dev:A': clientA };
      ctx.__setCards([1, 2]);
      setHigh(3);
      setup();
      await submit();
      checkEq('submit, ' + label + ': mark unchanged', high(), 3);
    }
    // The send never lowers the mark, even when Add Sensor runs while the GET is in flight.
    reset();
    stored = { 'dev:A': clientA };
    ctx.__setCards([1, 2]);
    setHigh(7);
    await submit();
    checkEq('submit never lowers the mark', high(), 7);
    reset();
    stored = { 'dev:A': { config: { snh: 2, sensors: [{ number: 1 }, { number: 2 }] } } };
    ctx.__setCards([1, 2]);
    setHigh(2);
    onGet = () => ctx.addSensor();
    await submit();
    checkEq('Add Sensor during the send keeps its number', [nums(), high()], [[1, 2, 3], 3]);

    // A Device UID typed in raises the mark to that client's; never lowers it.
    reset();
    stored = { 'dev:A': clientA };
    ctx.__setCards([1, 2]);
    setHigh(2);
    ctx.els.clientUid.value = ' dev:A ';
    await onUidChange();
    checkEq('typed UID raises the mark', [gets, high()], [['/api/client?uid=dev%3AA'], 5]);
    setHigh(7);
    await onUidChange();
    checkEq('typed UID never lowers the mark', high(), 7);
    reset();
    ctx.__setCards([1, 2]);
    setHigh(2);
    ctx.els.clientUid.value = 'dev:new';
    await onUidChange();
    checkEq('typed UID of a new client leaves the mark', high(), 2);
  };
}

// Where the page uses them
check('card carries its number',
  page.includes('function createSensorHtml(id,num){return `<div class="sensor-card" id="sensor-${id}" data-sensor-number="${num}">') &&
  page.includes('<span class="sensor-title">Sensor #${num}</span>'));
check('addSensor allocates the next number',
  page.includes('const num=nextSensorNumber(cards.map(cardSensorNumber),sensorNumberHigh);') &&
  page.includes('sensorNumberHigh=num;addSensorCard(num);}'));
check('serializer writes the card number', page.includes('number:cardSensorNumber(card),') && !page.includes('number:index+1'));
check('config is refused with bad numbers and carries snh',
  page.includes('if(!sensorNumbersValid(sensorNums)){') && page.includes('cfg.snh=Math.max(sensorNumberHigh,...sensorNums);'));
check('alarm-contact id uses the number', page.includes("const num=cardSensorNumber(card);if(!num)return '';return uid+'_'+num;};"));
check('loader keeps numbers',
  page.includes('window.loadConfig=function(c,floor){') &&
  page.includes('const loadedNumbers=loadSensorNumbers(c.sensors,c.snh,floor);sensorNumberHigh=loadedNumbers.high;') &&
  page.includes('addSensorCard(loadedNumbers.nums[ti])') && !page.includes('forEach(t=>{addSensor();'));
check('loader reports repaired numbers', page.includes('if(loadedNumbers.repaired.length){') &&
  page.includes("sensor numbers repaired (missing, duplicate, 0 or invalid): '") &&
  page.includes("(r.reused?' (an old number: every number up to 255 has been used)':'')") &&
  page.includes("Check their contacts and history before sending.',true,15000);"));
// A blank name is the type word alone. Texts and emails add " #<Display Number>" when one is set;
// the internal sensor number, the Display Number and the position never go into the name.
check('default names are the type word only',
  page.includes("if(!name)name=monitorType==='gas'?'Gas System':(monitorType==='rpm'?'Engine':'Tank');"));
{
  // Every statement between reading the Name box and the next field of the sensor: none may put a
  // number (internal number, Display Number or position) into the name.
  const from = "let name=card.querySelector('.tank-name').value.trim();";
  const to = 'const sensor=sensorKeyFromValue(type);';
  const at = page.indexOf(from);
  const end = at < 0 ? -1 : page.indexOf(to, at);
  check('collectConfig name handling found', at >= 0 && page.indexOf(from, at + 1) < 0 && end > at);
  const region = end > at ? page.slice(at + from.length, end).replace(/\/\*[\s\S]*?\*\//g, '') : '';
  check('default names carry no number',
    region.includes('name=') && !/cardSensorNumber|userNum|index|\$\{/.test(region), region);
  check('no numbered default name anywhere on the page', !/(Tank|Gas System|Engine) \$\{/.test(page));
}
// The sensor name is trimmed like the site, so a name of only spaces is the type word.
check('sensor name is trimmed',
  page.split("let name=card.querySelector('.tank-name').value.trim();").length === 2 &&
  !page.includes("let name=card.querySelector('.tank-name').value;"));
// P323: the page learns the server's mark where a file is imported, a config is sent and a Device
// UID is typed; loading from the server passes no floor (the stored config carries its own snh).
check('import handler loads with the server\'s mark',
  page.includes("r.onload=async(evt)=>{let c;try{c=JSON.parse(evt.target.result);}catch(err){showToast('Invalid JSON',true);return;}" +
    "const floor=await cachedSensorHigh(String((c&&c.deviceUid)||'').trim());try{loadConfig(c,floor);}catch(err){showToast('Invalid JSON',true);}};"));
check('submitConfig raises the mark after 200 or 202',
  extractFunction(page, 'submitConfig').includes(
    'if(res.status===200||res.status===202){const h=await cachedSensorHigh(clientUid);sensorNumberHigh=Math.max(sensorNumberHigh,cfg.snh,h);}'));
check('Device UID change raises the mark',
  page.includes("if(els.clientUid)els.clientUid.addEventListener('change',async()=>{const h=await cachedSensorHigh(els.clientUid.value.trim());if(h>sensorNumberHigh)sensorNumberHigh=h;});"));
check('loading from the server passes no floor',
  page.includes('if(c&&c.config)loadConfig(c.config);else startCommissioning(uid);') && page.split('loadConfig(').length === 3);
// Every place the mark is set: only the declaration and commissioning can lower it; a new one
// must be added here after checking that it never reuses a number.
checkEq('every assignment of the mark',
  (page.match(/sensorNumberHigh=(?!=)[^;]*;/g) || []).sort(),
  ['sensorNumberHigh=0;', 'sensorNumberHigh=Math.max(sensorNumberHigh,cfg.snh,h);', 'sensorNumberHigh=h;',
    'sensorNumberHigh=loadedNumbers.high;', 'sensorNumberHigh=num;', 'sensorNumberHigh=tpl.length;'].sort());
check('commissioning restarts numbering at 1',
  page.includes("function startCommissioning(uid){if(els.clientUid)els.clientUid.value=uid;") &&
  page.includes('card.dataset.sensorNumber=String(i+1);') && page.includes('sensorNumberHigh=tpl.length;if(!tpl.length)addSensor();'));

// The server registry takes every number the page can send (1-255). Numbers are never reused, so
// they pass MAX_SENSOR_RECORDS (which caps the record count, not the number); a note with no "k"
// (0) is still refused.
{
  const sketch = fs.readFileSync(SKETCH, 'utf8');
  const sig = 'static SensorRecord *upsertSensorRecord(const char *clientUid, uint8_t sensorIndex) {';
  const at = sketch.indexOf(sig);
  const end = at < 0 ? -1 : sketch.indexOf('\n}\n', at);
  check('upsertSensorRecord found', at >= 0 && sketch.indexOf(sig, at + 1) < 0 && end > at);
  const upsert = end > at ? sketch.slice(at, end) : '';
  check('registry accepts numbers past MAX_SENSOR_RECORDS',
    !/sensorIndex\s*>=?\s*MAX_SENSOR_RECORDS/.test(upsert) && !/sensorIndex\s*>=?\s*MAX_SENSOR_RECORDS/.test(sketch));
  check('registry refuses sensor 0', /if \(sensorIndex == 0\) \{\s*Serial\.print\(F\("ERROR: Sensor index out of range: "\)\);[^}]*return nullptr;/.test(upsert));
  check('registry still caps the record count', upsert.includes('if (gSensorRecordCount >= MAX_SENSOR_RECORDS) {'));
}

// CR-5: handleConfigPost never lowers the high mark. After the number check and before dispatch it
// stores the largest of the posted snh, the cached snapshot's mark and the highest posted number
// (sensorNumbersHighMark, host-tested in sensor_numbers_test.cpp).
{
  const sketch = fs.readFileSync(SKETCH, 'utf8');
  const sig = 'static void handleConfigPost(EthernetClient &client, const String &body) {';
  const at = sketch.indexOf(sig);
  const end = at < 0 ? -1 : sketch.indexOf('\n}\n', at);
  check('handleConfigPost found', at >= 0 && sketch.indexOf(sig, at + 1) < 0 && end > at);
  const post = end > at ? sketch.slice(at, end) : '';
  const checkAt = post.indexOf('if (sensorNumbersCheck(doc["config"]["sensors"], numbersMsg, sizeof(numbersMsg)) != SENSOR_NUMBERS_OK) {');
  const markAt = post.indexOf(
    '      if (doc["config"].is<JsonObject>()) {\n' +
    '        const ClientConfigSnapshot *prevSnap = findClientConfigSnapshot(clientUid);\n' +
    '        const uint8_t cachedHigh = prevSnap ? sensorNumbersCachedHigh(prevSnap->payload) : 0;\n' +
    '        const uint8_t highMark = sensorNumbersHighMark(doc["config"]["snh"], cachedHigh, doc["config"]["sensors"]);\n' +
    '        if (highMark > 0) {\n' +
    '          doc["config"]["snh"] = highMark;\n' +
    '        }\n' +
    '      }\n' +
    '      ConfigDispatchStatus status = dispatchClientConfig(clientUid, doc["config"]);');
  check('config post keeps the high mark, after the number check and before dispatch',
    checkAt >= 0 && markAt > checkAt && post.split('dispatchClientConfig(').length === 2);
}

flowTests().then(() => {
  console.log(failures ? failures + ' of ' + checks + ' checks FAILED' : 'generator numbers: all ' + checks + ' checks passed');
  process.exit(failures ? 1 : 0);
}, (err) => {
  console.log('FAIL the server-mark flow threw: ' + (err && err.stack || err));
  process.exit(1);
});
