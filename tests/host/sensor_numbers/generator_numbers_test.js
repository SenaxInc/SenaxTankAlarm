'use strict';
// Host test for stable sensor numbers on the server's Config Generator page (S-T01/S1).
//
//   node generator_numbers_test.js [path/to/TankAlarm-112025-Server-BluesOpta.ino]
//
// Needs node 18 or later and no packages. Reads the CONFIG_GENERATOR_HTML raw string from the
// server sketch, runs its numbering helpers (nextSensorNumber, sensorNumbersValid,
// loadSensorNumbers, cardSensorNumber) in a vm context, and checks that the page uses them where a
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

const page = generatorPage();
const ctx = {};
vm.createContext(ctx);
for (const n of ['nextSensorNumber', 'sensorNumbersValid', 'loadSensorNumbers', 'cardSensorNumber']) {
  vm.runInContext(extractFunction(page, n), ctx);
}
const { nextSensorNumber, sensorNumbersValid, loadSensorNumbers, cardSensorNumber } = ctx;
// Results cross the vm boundary; compare them as plain JSON.
const load = (sensors, snh) => JSON.parse(JSON.stringify(loadSensorNumbers(sensors, snh)));
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

// loadSensorNumbers: numbers as the client resolves them, 0 and duplicates renumbered past high
checkEq('legacy config (index+1, no snh)', load([{ number: 1 }, { number: 2 }, { number: 3 }]),
  { nums: [1, 2, 3], high: 3, repaired: [] });
checkEq('gaps and snh kept', load([{ number: 1 }, { number: 3 }, { number: 4 }], 5),
  { nums: [1, 3, 4], high: 5, repaired: [] });
checkEq('snh below the numbers in use', load([{ number: 6 }], 2), { nums: [6], high: 6, repaired: [] });
checkEq('missing number is the position', load([{}, {}]), { nums: [1, 2], high: 2, repaired: [] });
checkEq('duplicate repaired', load([{ number: 2 }, { number: 2 }]),
  { nums: [2, 3], high: 3, repaired: [{ position: 2, number: 3 }] });
checkEq('default that collides is repaired', load([{ number: 2 }, {}], 4),
  { nums: [2, 5], high: 5, repaired: [{ position: 2, number: 5 }] });
checkEq('zero repaired', load([{ number: 0 }]), { nums: [1], high: 1, repaired: [{ position: 1, number: 1 }] });
checkEq('300 is the position, like the client', load([{ number: 300 }]), { nums: [1], high: 1, repaired: [] });
checkEq('no sensors keep snh', load(undefined, 4), { nums: [], high: 4, repaired: [] });
checkEq('invalid snh ignored', load([{ number: 1 }], 'x'), { nums: [1], high: 1, repaired: [] });

// cardSensorNumber: the number stored on the card, 0 when there is none
checkEq('card number', cardSensorNumber(card(7)), 7);
checkEq('card number 0', cardSensorNumber(card(0)), 0);
checkEq('card number 256', cardSensorNumber(card(256)), 0);
checkEq('card without a number', cardSensorNumber({ dataset: {} }), 0);
checkEq('no card', cardSensorNumber(null), 0);

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
  page.includes('const loadedNumbers=loadSensorNumbers(c.sensors,c.snh);sensorNumberHigh=loadedNumbers.high;') &&
  page.includes('addSensorCard(loadedNumbers.nums[ti])') && !page.includes('forEach(t=>{addSensor();'));
check('loader reports repaired numbers', page.includes('if(loadedNumbers.repaired.length){'));

console.log(failures ? failures + ' of ' + checks + ' checks FAILED' : 'generator numbers: all ' + checks + ' checks passed');
process.exit(failures ? 1 : 0);
