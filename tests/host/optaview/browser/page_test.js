// Opens OptaView's web page in Chromium and uses it the way a person would: chooses a PIN,
// reads the controller, changes a setting through the confirmation dialog, restarts it, loses
// the session. The page is served by the firmware's own code on this PC, against the simulated
// SunSaver (../optaview_preview.cpp), and with the Content-Security-Policy the Opta sends.
//
//   make browser-test ARDUINOJSON_DIR=/path/to/ArduinoJson/src
//
// or, with the preview already built:
//
//   node browser/page_test.js build/optaview_preview [--shots <folder>]
//
// It needs the "playwright" package and its Chromium. --shots also saves pictures of the views,
// which is how the ones in OptaView/screenshots are made.
//
// What the C++ suites (optaview_test.cpp, optaview_web_test.cpp) already settle is not repeated
// here: the rules about writing, the HTTP limits, the login waits. This file is about the page:
// that its script runs under its policy, shows what the API says and sends what the buttons mean.

'use strict';

const { chromium } = require('playwright');
const { spawn } = require('child_process');
const fs = require('fs');
const net = require('net');
const path = require('path');

const PIN = '2468';
const args = process.argv.slice(2);
let shotsDir = null;
let binary = null;
for (let i = 0; i < args.length; ++i) {
  if (args[i] === '--shots' && i + 1 < args.length) {
    shotsDir = path.resolve(args[++i]);
  } else if (!binary) {
    binary = path.resolve(args[i]);
  }
}
if (!binary || !fs.existsSync(binary)) {
  console.error('usage: node page_test.js <path to optaview_preview> [--shots <folder>]');
  process.exit(2);
}

// ---------------------------------------------------------------------------------------------
// A minimal test framework, like the one of the C++ suites
// ---------------------------------------------------------------------------------------------

let checks = 0;
const failures = [];
let scenario = '';

function check(ok, what) {
  ++checks;
  if (!ok) {
    failures.push(`${scenario}: ${what}`);
    console.error(`CHECK failed in "${scenario}": ${what}`);
  }
}

function checkEq(actual, expected, what) {
  check(actual === expected, `${what}: got ${JSON.stringify(actual)}, expected ${JSON.stringify(expected)}`);
}

function checkHas(text, needle, what) {
  check(String(text).includes(needle), `${what}: ${JSON.stringify(needle)} is not in ${JSON.stringify(String(text).slice(0, 600))}`);
}

function checkHasnt(text, needle, what) {
  check(!String(text).includes(needle), `${what}: ${JSON.stringify(needle)} is in ${JSON.stringify(String(text).slice(0, 600))}`);
}

const pause = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// ---------------------------------------------------------------------------------------------
// The preview: the firmware on this PC. Its terminal is the USB console.
// ---------------------------------------------------------------------------------------------

class Preview {
  static async start(extra, port) {
    const preview = new Preview();
    preview.out = '';
    preview.child = spawn(binary, ['--port', String(port || 0)].concat(extra || []), { stdio: ['pipe', 'pipe', 'inherit'] });
    preview.exited = new Promise((resolve) => preview.child.on('exit', resolve));
    preview.child.stdout.on('data', (data) => { preview.out += data.toString(); });
    const found = await preview.waitFor(/http:\/\/127\.0\.0\.1:(\d+)\//, 0);
    preview.port = Number(found[1]);
    preview.base = `http://127.0.0.1:${preview.port}/`;
    return preview;
  }

  async waitFor(pattern, from) {
    const started = Date.now();
    for (;;) {
      const found = pattern.exec(this.out.slice(from));
      if (found) {
        return found;
      }
      if (Date.now() - started > 10000) {
        throw new Error(`the preview did not print ${pattern}; it printed: ${this.out.slice(from).slice(-400)}`);
      }
      await pause(10);
    }
  }

  // Types a line on the USB console (or a "sim ..." line) and waits for what it must print.
  async line(text, expect) {
    const from = this.out.length;
    this.child.stdin.write(text + '\n');
    return this.waitFor(expect, from);
  }

  // Requests that change something, as the simulated controller has counted them.
  async writes() {
    return Number((await this.line('sim writes', /sim: (\d+) writes so far/))[1]);
  }

  async kill() {
    this.child.kill('SIGKILL');
    await this.exited;
  }
}

// ---------------------------------------------------------------------------------------------
// A browser window on the page, with everything that must not happen written down
// ---------------------------------------------------------------------------------------------

// Chromium reports every answer with an error status on its console. These are the ones the
// scenarios cause on purpose (a wrong PIN, a session that has ended, a PIN chosen meanwhile).
const EXPECTED_CONSOLE = /^Failed to load resource: the server responded with a status of (401|409|429) /;

async function openWindow(browser, preview, options) {
  const context = await browser.newContext(Object.assign({
    viewport: { width: 1280, height: 900 },
    deviceScaleFactor: shotsDir ? 2 : 1,
    acceptDownloads: true,
  }, options || {}));
  // Runs before the page's own script and is not subject to its policy.
  await context.addInitScript(() => {
    window.__violations = [];
    document.addEventListener('securitypolicyviolation', (event) => {
      window.__violations.push(`${event.violatedDirective} ${event.blockedURI}`);
    });
  });
  const page = await context.newPage();
  const win = { context, page, preview, requests: [], problems: [], allow: null };
  page.on('console', (message) => {
    if (message.type() !== 'error' && message.type() !== 'warning') {
      return;
    }
    if (EXPECTED_CONSOLE.test(message.text()) || (win.allow && win.allow.test(message.text()))) {
      return;
    }
    win.problems.push(`console ${message.type()}: ${message.text()}`);
  });
  page.on('pageerror', (error) => win.problems.push(`uncaught: ${error.message}`));
  page.on('request', (request) => {
    win.requests.push(`${request.method()} ${request.url()}`);
    // Nothing is fetched from anywhere but the Opta.
    if (!request.url().startsWith(preview.base) && !/^(data|blob):/.test(request.url())) {
      win.problems.push(`a request to elsewhere: ${request.url()}`);
    }
  });
  win.text = async (selector) => ((await page.textContent(selector)) || '').trim();
  win.reads = () => win.requests.filter((request) => request.includes('/api/read')).length;
  // Call at the end of a scenario: nothing went wrong behind the scenes.
  win.settle = async () => {
    const violations = await page.evaluate(() => window.__violations.splice(0));
    checkEq(violations.join('; '), '', 'nothing was blocked by the page\'s policy');
    checkEq(win.problems.splice(0).join('; '), '', 'the browser reported no error');
  };
  return win;
}

let shotNumber = 0;
async function shot(win, name, fullPage) {
  if (!shotsDir) {
    return;
  }
  fs.mkdirSync(shotsDir, { recursive: true });
  ++shotNumber;
  await win.page.screenshot({ path: path.join(shotsDir, `${name}.png`), fullPage: fullPage !== false });
}

async function logIn(win, pin) {
  const page = win.page;
  await page.waitForSelector('#view-login:not(.hidden)');
  await page.waitForFunction(() => !document.querySelector('#loginBtn').disabled);
  await page.fill('#pin', pin);
  if (await page.isVisible('#pin2')) {
    await page.fill('#pin2', pin);
  }
  await page.click('#loginBtn');
  await page.waitForSelector('#liveTiles .data-card');
}

async function openView(win, name) {
  await win.page.click(`#nav a[data-view="${name}"]`);
  await win.page.waitForSelector(`#view-${name}:not(.hidden)`);
}

// --- the confirmation dialog ---

const dialogText = (win) => win.page.textContent('#dialogText');
const dialogHidden = (win) => win.page.waitForSelector('#dialog.hidden', { state: 'attached' });

// The dialog shows a question, with Yes still locked for a moment.
async function question(win) {
  await win.page.waitForFunction(() => {
    const yes = document.querySelector('#dialogYes');
    return !yes.classList.contains('hidden') && !document.querySelector('#dialog').classList.contains('hidden');
  });
  return dialogText(win);
}

// The dialog shows a result and offers only Close.
async function result(win, timeout) {
  await win.page.waitForSelector('#dialogClose:not(.hidden)', { timeout: timeout || 30000 });
  check(await win.page.isHidden('#dialogYes'), 'a result offers no Yes');
  return dialogText(win);
}

async function yes(win) {
  await win.page.waitForFunction(() => !document.querySelector('#dialogYes').disabled);
  await win.page.click('#dialogYes');
}

async function close(win) {
  await win.page.click('#dialogClose');
  await dialogHidden(win);
}

const toast = (win) => win.text('#toast');
const toastShown = (win) => win.page.evaluate(() => document.querySelector('#toast').classList.contains('show'));
const nowCell = (win, name) => win.text(`#settingsGroups td[data-now="${name}"]`);
const newValue = (name) => `#settingsGroups input[aria-label="New value for ${name}"]`;
const changeButton = (name) => `#settingsGroups tr[data-register="${name}"] button`;
const tile = (win, name) => win.text(`#liveTiles .data-card[data-register="${name}"] .dc-value`);
const notice = (key) => `#notices .notice[data-notice="${key}"]`;

async function waitTile(win, name, value) {
  await win.page.waitForFunction(([register, wanted]) => {
    const node = document.querySelector(`#liveTiles .data-card[data-register="${register}"] .dc-value`);
    return node && node.textContent.trim() === wanted;
  }, [name, value], { timeout: 20000 });
}

async function waitNow(win, name, value) {
  await win.page.waitForFunction(([register, wanted]) => {
    const node = document.querySelector(`#settingsGroups td[data-now="${register}"]`);
    return node && node.textContent.trim() === wanted;
  }, [name, value], { timeout: 20000 });
}

// ---------------------------------------------------------------------------------------------
// Scenarios
// ---------------------------------------------------------------------------------------------

// The first visitor chooses the PIN. The page runs under its policy, and only its own script does.
async function firstVisit(win) {
  const page = win.page;
  const response = await page.goto(win.preview.base);
  checkEq(response.status(), 200, 'the page is served');
  const policy = response.headers()['content-security-policy'] || '';
  checkHas(policy, "default-src 'none'", 'the page\'s policy');
  check(/script-src 'sha256-[A-Za-z0-9+/]{43}=';/.test(policy), 'the policy names the script by its hash');
  check(/style-src 'sha256-[A-Za-z0-9+/]{43}=';/.test(policy), 'the policy names the style sheet by its hash');
  checkHasnt(policy, 'unsafe', 'the policy');

  await page.waitForSelector('#view-login:not(.hidden)');
  checkEq(await win.text('#loginTitle'), 'Choose a PIN', 'without a PIN the first visitor chooses one');
  check(await page.isVisible('#pin2'), 'a new PIN is asked for twice');
  check(await page.isHidden('#nav'), 'no views are offered before the login');
  check(await page.isHidden('#logoutBtn'), 'nothing to log out of before the login');
  check(/^OptaView \d+\.\d+\.\d+ · Morningstar SunSaver MPPT/.test(await win.text('#foot')), 'the footer names version and controller');
  checkEq(await page.evaluate(() => getComputedStyle(document.querySelector('.brand')).fontWeight), '700',
          'the style sheet is applied');
  await shot(win, 'login', false);

  // The policy is in force: a script that is not the page's own does not run.
  await page.evaluate(() => {
    const script = document.createElement('script');
    script.textContent = 'window.__injected = true;';
    document.body.appendChild(script);
  });
  checkEq(await page.evaluate(() => window.__injected === true), false, 'an injected script does not run');
  const blocked = await page.evaluate(() => window.__violations.splice(0));
  checkEq(blocked.length, 1, 'the injected script was blocked by the policy');
  win.problems.splice(0);  // Chromium also says so on its console

  // What cannot be a PIN is said by the page, and nothing is sent.
  const sent = win.requests.length;
  await page.fill('#pin', '12');
  await page.click('#loginBtn');
  checkEq(await win.text('#loginMessage'), 'A PIN is 4 to 8 digits.', 'a PIN that is too short');
  await page.fill('#pin', PIN);
  await page.fill('#pin2', '2469');
  await page.click('#loginBtn');
  checkEq(await win.text('#loginMessage'), 'The two PINs are not the same.', 'two different PINs');
  checkEq(win.requests.length, sent, 'neither was sent to the Opta');

  await page.fill('#pin2', PIN);
  await page.click('#loginBtn');
  await page.waitForSelector('#liveTiles .data-card');
  check(await page.isVisible('#nav'), 'the views are offered after the login');
  checkEq(await page.getAttribute('#nav a[data-view="live"]', 'aria-current'), 'page', 'Live is the first view');
  checkEq(await win.text('#linkPill'), 'Slave 1, 9600 8N2', 'the link in the header');

  // The session is a cookie that scripts cannot read and other sites cannot send.
  const cookies = await win.context.cookies();
  checkEq(cookies.length, 1, 'one cookie');
  checkEq(cookies[0].name, 'optaview_session', 'the cookie\'s name');
  check(/^[0-9a-f]{32}$/.test(cookies[0].value), 'the cookie is 32 hex digits');
  checkEq(cookies[0].httpOnly, true, 'the cookie is HttpOnly');
  checkEq(cookies[0].sameSite, 'Strict', 'the cookie is SameSite=Strict');
  checkEq(await page.evaluate(() => document.cookie), '', 'the page\'s script cannot read it');

  // It survives a reload, and logging out ends it.
  await page.reload();
  await page.waitForSelector('#liveTiles .data-card');
  check(await page.isHidden('#view-login'), 'a reload stays logged in');
  await page.click('#logoutBtn');
  await page.waitForSelector('#view-login:not(.hidden)');
  checkEq(await win.text('#loginTitle'), 'Enter the PIN', 'the PIN exists now');
  check(await page.isHidden('#pin2'), 'an existing PIN is asked for once');
  check(await page.isHidden('#nav'), 'the views are gone after the logout');
  checkEq((await win.context.cookies()).length, 0, 'the cookie is gone after the logout');
  await page.reload();
  await page.waitForSelector('#view-login:not(.hidden)');

  // Without the Opta's random number generator the login works all the same, because OptaView is
  // meant for a network that is trusted. The USB console says that it happened.
  await win.preview.line('sim random off', /sim: the random number generator fails/);
  await logIn(win, PIN);
  check(await page.isVisible('#nav'), 'a login while the random number generator fails is let in');
  const weak = await win.context.cookies();
  check(weak.length === 1 && /^[0-9a-f]{32}$/.test(weak[0].value), 'with a cookie of 32 hex digits');
  check(weak.length === 1 && weak[0].value !== cookies[0].value, 'that is not the one before');
  await win.preview.line('web', /The random number generator failed 1 time, so a session cookie or question id was made/);
  await win.preview.line('sim random on', /sim: the random number generator works/);
  await win.settle();
}

// The Live view: what it shows, that it reads by itself, and what it does when the controller
// stops answering.
async function liveView(win) {
  const page = win.page;
  const profile = await page.evaluate(() => fetch('/api/profile').then((answer) => answer.json()));
  const liveGroups = profile.groups.filter((group) => group.live).map((group) => group.name);
  check(liveGroups.length >= 2, 'the register map names live groups');
  const first = profile.registers.filter((reg) => reg.group === liveGroups[0]);
  checkEq(await page.locator('#liveTiles .data-card').count(), first.length, 'one tile per register of the first live group');
  checkEq(await tile(win, 'adc_vb_f'), '13.68 V', 'the battery voltage');
  checkEq(await win.text('#liveTiles .data-card[data-register="adc_vb_f"] .dc-type'), 'Battery voltage', 'a register confirmed on the bench');
  checkEq(await win.text('#liveTiles .data-card[data-register="T_hs"] .dc-type'), 'Heatsink temperature ~', 'a register from the specification only');
  checkEq(await win.text('#liveTables tr[data-register="charge_state"] td:nth-child(2)'), 'FLOAT (7)', 'a state by its name');
  checkHas(await win.text('#liveLegend'), '~ taken from the specification', 'the legend');
  checkHas(await win.text(notice('scale')), 'has not been confirmed on this controller', 'the note on the voltage scale');
  check(/^Read at /.test(await win.text('#liveStamp')), 'when it was read');
  await shot(win, 'live');

  // OptaView ends every answer by closing the connection, so one that broke off on the way still
  // has its status 200. The page says that it did not arrive whole and keeps what it showed.
  const stamp = await win.text('#liveStamp');
  await page.route('**/api/read?*', (route) => route.fulfill({
    status: 200,
    contentType: 'application/json',
    body: '{"values": [{"name": "adc_vb_f", "raw": [44',
  }));
  await page.click('#liveBtn');
  await page.waitForFunction(() => document.querySelector('#toast').classList.contains('show'));
  checkEq(await toast(win), 'The answer from OptaView did not arrive whole, so nothing was taken from it. Try again.',
          'an answer that broke off says so');
  checkEq(await tile(win, 'adc_vb_f'), '13.68 V', 'and the reading it had stays');
  checkEq(await win.text('#liveStamp'), stamp, 'with the time it was read');
  await page.unroute('**/api/read?*');
  await Promise.all([
    page.waitForResponse((answer) => answer.url().includes('/api/read') && answer.status() === 200),
    page.click('#liveBtn'),
  ]);

  // It reads again by itself, and marks those requests so that they do not keep a session alive.
  await win.preview.line('sim battery 12.40', /sim: the battery is at 12\.40 V/);
  await waitTile(win, 'adc_vb_f', '12.40 V');
  check(win.requests.some((request) => request.includes('/api/read?') && request.endsWith('&auto=1')),
        'reads the page makes by itself are marked');

  // With the box cleared it does not.
  await page.uncheck('#autoBox');
  await pause(300);  // a read that was on its way
  const reads = win.reads();
  await win.preview.line('sim battery 12.80', /sim: the battery is at 12\.80 V/);
  await pause(6000);
  checkEq(win.reads(), reads, 'nothing is read while the box is cleared');
  checkEq(await tile(win, 'adc_vb_f'), '12.40 V', 'and the old reading stays');
  await page.click('#liveBtn');
  await waitTile(win, 'adc_vb_f', '12.80 V');
  check(!win.requests[win.requests.length - 1].includes('auto=1'), 'a read that was asked for is not marked');
  await page.check('#autoBox');

  // The controller loses its power.
  await win.preview.line('sim power off', /sim: the controller has no power/);
  await page.click('#liveBtn');
  await page.waitForSelector(notice('link'), { timeout: 20000 });
  checkHas(await win.text(notice('link')), 'The controller does not answer on the RS-485 link.', 'the link notice');
  checkEq(await tile(win, 'adc_vb_f'), '--', 'no value is shown for a register that was not read');
  checkEq(await win.text('#liveTiles .data-card[data-register="adc_vb_f"] .dc-alarm'), 'no reply', 'why there is none');
  checkEq(await win.text('#liveTiles .data-card[data-register="adc_va_f"] .dc-alarm'), 'not read', 'the rest was not asked for');
  await shot(win, 'live-no-answer');
  const down = win.reads();
  await pause(5600);
  checkEq(win.reads(), down, 'a controller that does not answer is not asked again by itself');

  // It is back.
  await win.preview.line('sim power on', /sim: the controller has power/);
  await page.click(`${notice('link')} button`);
  await page.waitForSelector(notice('link'), { state: 'detached', timeout: 20000 });
  checkEq(await tile(win, 'adc_vb_f'), '12.80 V', 'the readings are back');
  await win.preview.line('sim battery 13.68', /sim: the battery is at 13\.68 V/);
  await waitTile(win, 'adc_vb_f', '13.68 V');  // ...and so is the reading by itself
  await win.settle();
  return profile;
}

// The Settings view: every change goes through the dialog, and the dialog is the console's.
async function settingsView(win, profile) {
  const page = win.page;
  const writable = profile.registers.filter((reg) => reg.writable);
  await openView(win, 'settings');
  await page.waitForFunction((count) => {
    const cells = document.querySelectorAll('#settingsGroups td[data-now]');
    return cells.length === count && Array.from(cells).every((cell) => cell.textContent !== '');
  }, writable.length, { timeout: 30000 });
  checkEq(await page.locator('#settingsGroups tr[data-register]').count(), writable.length, 'one row per setting');
  checkEq(await nowCell(win, 'EV_lvd'), '11.50 V', 'what the controller holds');
  checkEq(await win.text('#settingsGroups tr[data-register="EV_lvd"] td:nth-child(3)'), '9.00 to 15.00 V', 'what is allowed');
  checkEq(await win.text('#settingsGroups tr[data-register="EV_float"] td:nth-child(3)'), '10.00 to 17.00 V, or 0', 'a setting that 0 switches off');
  checkEq(await page.locator('#writeNotes li').count(), profile.writeNotes.length, 'the notes on writing');
  checkHas(await win.text(notice('scale')), 'Run the scale check under Tools before you change a voltage', 'the warning on the voltage scale');
  check(await page.isHidden(notice('reset')), 'no restart is due yet');
  const before = await win.preview.writes();

  // Not a number: said by the page, nothing is sent.
  await page.fill(newValue('EV_lvd'), '13,1');
  await page.click(changeButton('EV_lvd'));
  checkHas(await toast(win), 'Give the new value as a number', 'a value that is not a number');
  check(await toastShown(win), 'the message is on the screen');
  check(await page.isHidden('#dialog'), 'and no dialog for it');

  // A change, and Cancel.
  await page.fill(newValue('EV_lvd'), '13.10');
  await page.click(changeButton('EV_lvd'));
  // Yes is locked while the question is new: a second click meant for Change must not confirm.
  await page.waitForFunction(() => !document.querySelector('#dialogYes').classList.contains('hidden') &&
                                   document.querySelector('#dialogYes').disabled);
  let asked = await question(win);
  check(!(await toastShown(win)), 'the message about the earlier try is not left on top of the dialog');
  checkEq(await win.text('#dialogTitle'), 'Change Low voltage disconnect', 'the dialog\'s title');
  checkHas(asked, 'now  11.50 V', 'the value the controller holds');
  checkHas(asked, 'new  13.10 V', 'the value that would be written');
  checkHas(asked, 'the scale of stored settings (vset, x100/32768) is not confirmed', 'the console\'s warning');
  checkHas(asked, 'Low voltage reconnect (EV_lvr) must be above low voltage disconnect (EV_lvd)', 'the pair rule');
  checkHas(asked, 'Type yes to write it. Anything else cancels.', 'the console\'s question');
  checkEq(await page.evaluate(() => document.activeElement.id), 'dialogNo', 'Cancel has the focus, not Yes');
  check(await page.isHidden('#dialogClose'), 'a question offers Yes and Cancel only');
  checkEq(await page.evaluate(() => ['header', 'main', 'footer'].every((tag) => document.querySelector(tag).hasAttribute('inert'))),
          true, 'what is behind the dialog cannot be used');
  await page.waitForFunction(() => !document.querySelector('#dialogYes').disabled);
  await shot(win, 'settings-confirm', false);
  await page.click('#dialogNo');
  await dialogHidden(win);
  checkEq(await toast(win), 'Cancelled. Nothing was sent.', 'what the console said to Cancel');
  checkEq(await page.inputValue(newValue('EV_lvd')), '13.10', 'what was typed is still there');
  checkEq(await page.evaluate(() => document.querySelector('main').hasAttribute('inert')), false, 'the page can be used again');
  checkEq(await win.preview.writes(), before, 'Cancel sent nothing to the controller');

  // The same with the keyboard: Enter in the field asks, Tab never reaches what is behind the
  // dialog (between its last and its first button the focus passes through the browser's own
  // controls, which is the page's body here), Escape cancels.
  await page.press(newValue('EV_lvd'), 'Enter');
  await question(win);
  await page.waitForFunction(() => !document.querySelector('#dialogYes').disabled);
  const visited = [];
  for (let i = 0; i < 7; ++i) {
    await page.keyboard.press('Tab');
    visited.push(await page.evaluate(() => {
      const at = document.activeElement;
      return document.querySelector('#dialog').contains(at) ? at.id : at === document.body ? 'body' : `outside: ${at.outerHTML.slice(0, 80)}`;
    }));
  }
  checkEq(visited.filter((id) => id.startsWith('outside')).join('; '), '', 'Tab never leaves the dialog for the page behind it');
  check(visited.includes('dialogYes') && visited.includes('dialogNo'), `Tab reaches both buttons (it went ${visited.join(', ')})`);
  await page.keyboard.press('Escape');
  await dialogHidden(win);
  checkEq(await win.preview.writes(), before, 'Escape sent nothing to the controller');
  checkEq(await page.evaluate(() => document.activeElement.getAttribute('aria-label')), 'New value for EV_lvd',
          'the focus is back where it was');

  // Yes.
  await page.click(changeButton('EV_lvd'));
  await question(win);
  await yes(win);
  let done = await result(win);
  checkHas(done, 'Written and read back: EV_lvd = 13.10 V [0x10C5]', 'the console read the value back');
  await close(win);
  await waitNow(win, 'EV_lvd', '13.10 V');
  checkEq(await page.inputValue(newValue('EV_lvd')), '', 'the field is empty after a change');
  checkEq(await win.preview.writes(), before + 1, 'one write reached the controller');
  await page.waitForSelector(notice('reset'));
  checkHas(await win.text(notice('reset')), 'Settings were changed. The controller uses them after a restart.', 'a restart is due');

  // Above the limit: the console refuses, and there is nothing to confirm.
  await page.fill(newValue('EV_lvr'), '19');
  await page.click(changeButton('EV_lvr'));
  done = await result(win);
  checkHas(done, 'That is above what this profile allows for EV_lvr: from 9.00 V to 15.00 V.', 'a value above the limit');
  await close(win);
  checkEq(await page.inputValue(newValue('EV_lvr')), '19', 'a value that was refused is still there');

  // No restart while the reconnect voltage is below the disconnect voltage.
  await page.click(`${notice('reset')} button`);
  done = await result(win);
  checkEq(await win.text('#dialogTitle'), 'Restart the controller', 'the dialog\'s title');
  checkHas(done, 'Problem: Low voltage reconnect (EV_lvr) must be above low voltage disconnect (EV_lvd).', 'the pair rule stops the restart');
  checkHas(done, 'Now EV_lvd = 13.10 V and EV_lvr = 12.60 V.', 'with the values that break it');
  await close(win);
  checkEq(await win.preview.writes(), before + 1, 'neither reached the controller');

  // The other half of the pair, then the restart.
  await page.fill(newValue('EV_lvr'), '13.25');
  await page.click(changeButton('EV_lvr'));
  asked = await question(win);
  checkHas(asked, 'new  13.25 V', 'the reconnect voltage');
  checkHasnt(asked, 'must be above', 'the pair is in order with this value');
  checkHas(asked, "('profile' repeats the notes on writing.)", 'the notes on writing are shown once');
  await yes(win);
  checkHas(await result(win), 'Written and read back: EV_lvr = 13.25 V [0x10F6]', 'the second write');
  await close(win);
  await waitNow(win, 'EV_lvr', '13.25 V');
  await shot(win, 'settings');

  await page.click(`${notice('reset')} button`);
  asked = await question(win);
  checkHas(asked, 'This restarts the controller (coil 0x00FF).', 'what a restart does');
  await yes(win);
  // The answer is shown while it arrives: the restart takes seconds.
  await page.waitForFunction(() => document.querySelector('#dialogText').textContent.includes('Restarting the controller.') &&
                                   document.querySelector('#dialogClose').classList.contains('hidden'));
  checkEq(await win.text('#dialogHint'), 'Working...', 'the dialog says that it is still at work');
  done = await result(win);
  checkHas(done, 'Reset acknowledged.', 'the restart');
  checkHas(done, 'The controller answers again after', 'the controller is back');
  checkHasnt(done, 'Press a key', 'there are no keys on a page');
  check(!/[\u001e\u001f]/.test(done), 'the bytes that frame an answer are not shown');
  await close(win);
  await page.waitForSelector(notice('reset'), { state: 'detached' });
  checkEq(await win.preview.writes(), before + 3, 'two writes and the restart reached the controller');

  // A change that gets no answer at all: the controller loses its power between the question
  // and Yes. Nothing says that the write failed, because nothing is known. The page says that a
  // setting may have changed, until the setting has been read again.
  await page.fill(newValue('EV_lvr'), '13.40');
  await page.click(changeButton('EV_lvr'));
  await question(win);
  await win.preview.line('sim power off', /sim: the controller has no power/);
  await yes(win);
  done = await result(win);
  checkHas(done, 'The controller did not acknowledge the write (no reply).', 'silence is not called a refusal');
  checkHas(done, 'Reading it back failed too (no reply): whether it was written is not known.', 'what is not known is said');
  checkHasnt(done, 'The write failed', 'nothing is called failed that may have worked');
  await close(win);
  await page.waitForSelector(notice('reset'), { timeout: 20000 });
  checkHas(await win.text(notice('reset')), 'a setting may or may not have been changed', 'a restart may be due');
  checkHasnt(await win.text(notice('reset')), 'Settings were changed.', 'and it is not said to be due');
  await page.waitForSelector(notice('link'), { timeout: 20000 });
  // The controller is back and the settings are read again. It holds what it held, so nothing
  // is due and the notice goes.
  await win.preview.line('sim power on', /sim: the controller has power/);
  await page.click(`${notice('link')} button`);
  await page.waitForSelector(notice('reset'), { state: 'detached', timeout: 30000 });
  await waitNow(win, 'EV_lvr', '13.25 V');
  checkEq(await page.inputValue(newValue('EV_lvr')), '', 'the field was emptied when Yes was pressed');

  // The same silence from a controller that did store the value: only its answers were lost.
  // Reading the settings again shows the new value, and now the restart is known to be due.
  await page.fill(newValue('EV_lvr'), '13.40');
  await page.click(changeButton('EV_lvr'));
  await question(win);
  await win.preview.line('sim drop 3', /sim: the next 3 replies are lost/);
  await yes(win);
  done = await result(win);
  checkHas(done, 'whether it was written is not known.', 'the controller answered nothing');
  await close(win);
  await waitNow(win, 'EV_lvr', '13.40 V');
  await page.waitForFunction((selector) => {
    const node = document.querySelector(selector);
    return node && node.textContent.includes('Settings were changed.');
  }, notice('reset'), { timeout: 20000 });
  checkHasnt(await win.text(notice('reset')), 'may or may not', 'the doubt is gone');
  // Back to the value it had, and the restart.
  await page.fill(newValue('EV_lvr'), '13.25');
  await page.click(changeButton('EV_lvr'));
  await question(win);
  await yes(win);
  checkHas(await result(win), 'Written and read back: EV_lvr = 13.25 V [0x10F6]', 'the value is put back');
  await close(win);
  await waitNow(win, 'EV_lvr', '13.25 V');
  await page.click(`${notice('reset')} button`);
  await question(win);
  await yes(win);
  checkHas(await result(win), 'The controller answers again after', 'the restart');
  await close(win);
  await page.waitForSelector(notice('reset'), { state: 'detached' });
  checkEq(await win.preview.writes(), before + 7,
          'the lost write, the write whose answer was lost, the write that put it back, and the restart');
  await win.settle();
}

// The Controller view: switches, any group of registers, the register map.
async function controllerView(win, profile) {
  const page = win.page;
  const before = await win.preview.writes();
  await openView(win, 'controller');
  await page.waitForSelector('#coilsBox table');
  const shown = await page.locator('#coilsBox tr[data-coil]').evaluateAll((rows) => rows.map((row) => row.getAttribute('data-coil')));
  checkEq(shown.join(','), profile.coils.filter((coil) => !coil.reset).map((coil) => coil.name).join(','),
          'every coil but the restart has a row');
  const row = '#coilsBox tr[data-coil="load_disconnect"]';
  checkEq(await win.text(`${row} td:nth-child(2)`), 'off', 'a switch that is off');
  check(await page.isEnabled(`${row} button:has-text("Switch on")`), 'it can be switched on');
  check(await page.isDisabled(`${row} button:has-text("Switch off")`), 'it cannot be switched off');

  await page.click(`${row} button:has-text("Switch on")`);
  const asked = await question(win);
  checkHas(asked, 'Switch it on (coil 0x0001)?', 'the console\'s question for a switch');
  await yes(win);
  checkHas(await result(win), 'Sent and acknowledged. load_disconnect now reads on.', 'the switch was thrown');
  await close(win);
  await page.waitForFunction((selector) => document.querySelector(selector).textContent === 'on', `${row} td:nth-child(2)`);
  check(await page.isDisabled(`${row} button:has-text("Switch on")`), 'now it cannot be switched on');
  await page.click(`${row} button:has-text("Switch off")`);
  await question(win);
  await yes(win);
  await result(win);
  await close(win);
  await page.waitForFunction((selector) => document.querySelector(selector).textContent === 'off', `${row} td:nth-child(2)`);
  checkEq(await win.preview.writes(), before + 2, 'on and off reached the controller');

  // An action, cancelled.
  await page.click('#coilsBox tr[data-coil="clear_kwh"] button:has-text("Do it")');
  checkHas(await question(win), 'clear_kwh', 'the console\'s question for an action');
  await page.click('#dialogNo');
  await dialogHidden(win);
  checkEq(await win.preview.writes(), before + 2, 'a cancelled action sent nothing');

  // An action, carried out: the counter it clears reads 0 when it is read again.
  const kwh = '#groupBox tr[data-register="kWhc"] td:nth-child(4)';
  await page.selectOption('#groupPick', 'counters');
  await page.click('#groupBtn');
  await page.waitForSelector(kwh);
  checkEq(await win.text(kwh), '8.5 kWh', 'the kWh counter before the action');
  await page.click('#coilsBox tr[data-coil="clear_kwh"] button:has-text("Do it")');
  await question(win);
  await yes(win);
  checkHas(await result(win), 'Sent and acknowledged.', 'the action was carried out');
  await close(win);
  checkEq(await win.preview.writes(), before + 3, 'and reached the controller');
  await page.click('#groupBtn');
  await page.waitForFunction((selector) => document.querySelector(selector).textContent === '0.0 kWh', kwh);

  // Any group, with addresses and raw values.
  await page.selectOption('#groupPick', 'load');
  await page.click('#groupBtn');
  await page.waitForSelector('#groupBox tr[data-register="EV_lvd"]');
  const cells = await page.locator('#groupBox tr[data-register="EV_lvd"] td').allTextContents();
  checkEq(cells.join(' | '), '0xE022 | EV_lvd ~ (W) | Low voltage disconnect | 13.10 V | 0x10C5', 'a register with address and raw value');
  checkHas(await win.text('#aboutBox'), profile.title, 'which register map this is');
  checkEq(await page.locator('#aboutNotes li').count(), profile.notes.length, 'its notes');
  await shot(win, 'controller');
  await win.settle();
}

// The Tools view: the link, the scale check, the snapshot and the console.
async function toolsView(win, profile) {
  const page = win.page;
  await openView(win, 'tools');
  await page.waitForFunction(() => document.querySelector('#linkCounters').textContent !== '');
  checkHas(await win.text('#linkText'), 'Slave address 1, 9600 baud 8N2, read function 4, reply timeout 1000 ms', 'the link settings');
  check(/^Requests \d+, answered \d+, timeouts \d+,/.test(await win.text('#linkCounters')), 'the link counters');
  checkEq((await win.text('#scaleText')).split('not confirmed').length - 1, 2, 'both scales start unconfirmed');
  checkHas(await win.text('#wiringText'), 'MRC-1 terminal B', 'the wiring checklist');

  // The scale check with a multimeter reading and a setting whose value is known.
  await page.fill('#meterVolts', '13,68');
  await page.click('#scaleForm button');
  checkHas(await toast(win), 'Give the multimeter reading as a number', 'a reading that is not a number');
  const names = await page.$$eval('#knownName option', (options) => options.map((option) => option.value));
  checkEq(names[0], '', 'the list of settings starts with none');
  check(names.includes('EV_float') && names.includes('EV_lvd') && names.includes('EV_soc_r2y'),
        'it lists the stored voltage settings');
  check(!names.includes('adc_vb_f') && !names.includes('Et_float') && !names.includes('EV_tempcomp'),
        'and nothing on another scale');
  checkEq(names.length, 1 + profile.registers.filter((reg) => reg.scale === 'vset').length,
          'every register on the scale of stored settings is offered');
  await page.fill('#meterVolts', '13.68');
  await page.selectOption('#knownName', 'EV_float');
  await page.fill('#knownVolts', '13.7 V');
  await page.click('#scaleForm button');
  checkHas(await toast(win), 'Give the value of EV_float as a number', 'a value that is not a number');
  await page.selectOption('#knownName', '');
  await page.fill('#knownVolts', '13.70');
  await page.click('#scaleForm button');
  checkHas(await toast(win), 'Pick the setting whose value you know', 'a value without a setting');
  await page.selectOption('#knownName', 'EV_float');
  await page.click('#scaleForm button');
  const verdict = await result(win);
  checkHas(verdict, 'Multimeter: 13.68 V measured', 'the reading reached the console');
  checkHas(verdict, 'A setting you know: EV_float = 13.70 V', 'and so did the setting');
  checkHas(verdict, 'Stored settings: x100, which is what OptaView uses now.', 'the verdict');
  checkHas(verdict, 'Vb_f reads 13.68 V at x100', 'the slow battery reading follows the battery');
  await close(win);
  await page.waitForFunction(() => !document.querySelector('#scaleText').textContent.includes('not confirmed'));
  checkEq((await win.text('#scaleText')).split('confirmed on this controller').length - 1, 2, 'both scales are confirmed now');
  await openView(win, 'settings');
  await waitNow(win, 'EV_lvd', '13.10 V');
  check(await page.isHidden(notice('scale')), 'and the warning in Settings is gone');
  await openView(win, 'tools');

  // The snapshot is a file.
  const [download] = await Promise.all([page.waitForEvent('download'), page.click('#snapBtn')]);
  checkEq(download.suggestedFilename(), 'optaview-snapshot.json', 'the snapshot\'s file name');
  const snapshot = JSON.parse(fs.readFileSync(await download.path(), 'utf8'));
  checkEq(snapshot.profile, profile.id, 'the snapshot names the register map');
  check(JSON.stringify(snapshot).includes('"EV_lvd"'), 'and holds the settings');
  checkEq(snapshot.missing, 0, 'and every register in it was read');
  await snapshotChecks(win, snapshot);

  // The console: the same commands as on USB.
  await page.fill('#consoleLine', 'help');
  await page.press('#consoleLine', 'Enter');
  const help = await result(win);
  checkEq(await win.text('#dialogTitle'), 'help', 'the command is the title');
  checkHas(help, 'Changing settings (each one asks for \'yes\' first)', 'the console\'s help');
  checkHasnt(help, 'watch', 'a command that needs a key press is not offered');
  await close(win);
  checkEq(await page.inputValue('#consoleLine'), '', 'the line is empty after a command');
  await page.press('#consoleLine', 'ArrowUp');
  checkEq(await page.inputValue('#consoleLine'), 'help', 'the arrow keys bring back earlier commands');
  await page.fill('#consoleLine', '');

  // A change typed as a command is asked about in the same way.
  const before = await win.preview.writes();
  await page.fill('#consoleLine', 'set EV_lvr 13.30');
  await page.press('#consoleLine', 'Enter');
  checkHas(await question(win), 'new  13.30 V', 'a command that changes something asks first');
  await page.click('#dialogNo');
  await dialogHidden(win);
  checkEq(await win.preview.writes(), before, 'and Cancel sends nothing');
  // "yes" typed as a command finds nothing to agree to.
  await page.fill('#consoleLine', 'yes');
  await page.press('#consoleLine', 'Enter');
  checkHasnt(await result(win), 'Written', 'a typed yes confirms nothing');
  await close(win);
  checkEq(await win.preview.writes(), before, 'and nothing is written');

  // Whatever is typed is shown as text, never run or built into the page.
  await page.fill('#consoleLine', '<img id="planted" src="x"><b>get</b>');
  await page.press('#consoleLine', 'Enter');
  checkHas(await result(win), '<img', 'markup in a command is shown as it was typed');
  await close(win);
  checkEq(await page.locator('#planted').count(), 0, 'and does not become part of the page');
  const log = await page.textContent('#consoleLog');
  checkHas(log, '> help\n', 'the log has the commands');
  checkHas(log, '> set EV_lvr 13.30\n', 'the changes');
  checkHas(log, '> no\n', 'and the answers');
  checkHas(log, '> <img id="planted" src="x"><b>get</b>\n', 'as text');
  await shot(win, 'tools');

  // An answer that does not arrive whole, or does not arrive at all. The page cannot tell then
  // whether the command was carried out. It has to say so and keep the dialog open, not close it
  // as if all were well. (The Opta ends an answer early when the browser stops taking it. A
  // browser cannot be made to do that, so the test stands in the way of the answer instead.)
  await page.route('**/api/console', (route) => route.fulfill({
    status: 200,
    contentType: 'text/plain; charset=utf-8',
    body: 'adc_vb_f                13.68 V\r\nadc_va',
  }));
  await page.fill('#consoleLine', 'live');
  await page.press('#consoleLine', 'Enter');
  checkHas(await result(win), 'adc_vb_f                13.68 V\nadc_va', 'what arrived of a cut-off answer is shown');
  checkHas(await win.text('#dialogHint'), 'The answer broke off before its end.', 'and the page says that it broke off');
  await close(win);
  await page.unroute('**/api/console');
  win.allow = /net::ERR_FAILED/;
  await page.route('**/api/console', (route) => route.abort());
  await page.fill('#consoleLine', 'live');
  await page.press('#consoleLine', 'Enter');
  await result(win);
  checkHas(await win.text('#dialogHint'), 'No answer from OptaView. Whether the command was carried out is not known',
           'a command that gets no answer says so');
  checkEq(await dialogText(win), '', 'and shows nothing as its result');
  await close(win);
  await page.unroute('**/api/console');
  // The next command is answered as usual.
  await page.fill('#consoleLine', 'live');
  await page.press('#consoleLine', 'Enter');
  checkHas(await result(win), 'led_state', 'the command after it is answered in full');
  checkEq(await win.text('#dialogHint'), '', 'with nothing to add');
  await close(win);
  win.allow = null;
  await win.settle();
}

// A snapshot is saved whole or not at all, and one with registers missing only when the person
// wants it so. OptaView does not announce the length of a snapshot, so one that broke off ends
// like one that did not. The test stands in the way of the answer to make the ones it needs, from
// the whole one that `whole` holds.
async function snapshotChecks(win, whole) {
  const page = win.page;
  let downloads = [];
  const counter = (download) => downloads.push(download);
  page.on('download', counter);
  const serve = async (body, held) => {
    await page.unroute('**/api/snapshot');
    await page.route('**/api/snapshot', async (route) => {
      if (held) { await held; }
      await route.fulfill({ status: 200, contentType: 'application/json', body });
    });
  };
  const toastSays = (text) => page.waitForFunction((wanted) => {
    const node = document.querySelector('#toast');
    return node.classList.contains('show') && node.textContent.includes(wanted);
  }, text);
  const copy = () => JSON.parse(JSON.stringify(whole));
  const unread = (entry) => ({ addr: entry.addr, name: entry.name, error: 'no reply' });
  const count = whole.registers.length;
  check(count > 8, 'the snapshot has registers to take away');
  // What the page has not saved by now, it does not save later.
  const nothingSaved = async (what) => {
    await pause(300);
    checkEq(downloads.length, 0, what);
  };

  // Broken off, anywhere: not saved.
  const text = JSON.stringify(whole, null, 2) + '\n';
  for (const cut of [text.length - 3, Math.floor(text.length / 2), 1]) {
    await serve(text.slice(0, cut));
    await page.click('#snapBtn');
    await toastSays('The snapshot did not arrive whole, so it was not saved. Try again.');
    await nothingSaved(`a snapshot cut off after ${cut} of ${text.length} bytes is not saved`);
    check(await page.isHidden('#dialog'), 'and asks nothing');
    await page.waitForFunction(() => !document.querySelector('#snapBtn').disabled);
  }
  // Whole JSON that is not a snapshot: not saved either.
  await serve('{"registers": []}');
  await page.click('#snapBtn');
  await toastSays('did not arrive whole');
  await nothingSaved('JSON without its count of missing registers is not saved');

  // Two registers could not be read. The page says so before it saves anything.
  const two = copy();
  two.registers[1] = unread(two.registers[1]);
  two.registers[4] = unread(two.registers[4]);
  two.missing = 2;
  await serve(JSON.stringify(two));
  await page.click('#snapBtn');
  await page.waitForFunction(() => !document.querySelector('#dialogYes').classList.contains('hidden') &&
                                   document.querySelector('#dialogYes').disabled);
  const asked = await question(win);
  check(!(await toastShown(win)), 'the message about the snapshot before is not left on top of the dialog');
  checkEq(asked, '2 registers could not be read, so this snapshot is not a full record of how the controller is set. ' +
          'Each one is marked with its "error".', 'a snapshot with registers missing is asked about');
  checkEq(await win.text('#dialogTitle'), 'Snapshot', 'the dialog\'s title');
  checkEq(await win.text('#dialogYes'), 'Save anyway', 'the button that saves it');
  checkEq(await win.text('#dialogHint'), 'Save anyway keeps what was read. Cancel saves nothing.', 'what the buttons do');
  checkEq(await page.evaluate(() => document.activeElement.id), 'dialogNo', 'Cancel has the focus');
  await page.click('#dialogNo');
  await dialogHidden(win);
  await nothingSaved('Cancel saves nothing');
  checkEq(await page.evaluate(() => document.activeElement.id), 'snapBtn', 'the focus is back on the button');
  // Escape is Cancel.
  await page.waitForFunction(() => !document.querySelector('#snapBtn').disabled);
  await page.click('#snapBtn');
  await question(win);
  await page.keyboard.press('Escape');
  await dialogHidden(win);
  await nothingSaved('Escape saves nothing');
  // Save anyway: the file is what OptaView sent.
  await page.click('#snapBtn');
  await question(win);
  await yes(win);
  await dialogHidden(win);
  await page.waitForFunction(() => document.querySelector('#dialogYes').textContent === 'Yes');
  for (let i = 0; i < 100 && downloads.length === 0; ++i) {
    await pause(50);
  }
  checkEq(downloads.length, 1, 'Save anyway saves the snapshot');
  checkEq(fs.readFileSync(await downloads[0].path(), 'utf8'), JSON.stringify(two), 'as OptaView sent it');
  downloads = [];

  // One register.
  const one = copy();
  one.registers[2] = unread(one.registers[2]);
  one.missing = 1;
  await serve(JSON.stringify(one));
  await page.click('#snapBtn');
  checkEq(await question(win), '1 register could not be read, so this snapshot is not a full record of how the ' +
          'controller is set. It is marked with its "error".', 'one register missing');
  await page.click('#dialogNo');
  await dialogHidden(win);

  // OptaView ran out of time: the registers it did not reach are counted, but not listed.
  const late = copy();
  late.registers = late.registers.slice(0, 5);
  late.registers[4] = unread(late.registers[4]);
  late.missing = count - 4;
  await serve(JSON.stringify(late));
  await page.click('#snapBtn');
  checkEq(await question(win), `${count - 4} registers could not be read, so this snapshot is not a full record of ` +
          `how the controller is set. OptaView ran out of time before it reached ${count - 5} of them.`,
          'a snapshot that ran out of time');
  await page.click('#dialogNo');
  await dialogHidden(win);

  // Nothing could be read.
  const none = copy();
  none.registers = none.registers.map(unread);
  none.missing = count;
  await serve(JSON.stringify(none));
  await page.click('#snapBtn');
  checkEq(await question(win), 'No register could be read, so this snapshot holds nothing of the controller. Is it ' +
          'connected?', 'a snapshot without a single value');
  await page.click('#dialogNo');
  await dialogHidden(win);
  await nothingSaved('none of them was saved');

  // A command typed while the snapshot is read has its dialog open when the snapshot arrives:
  // the page does not put a second one on top, and does not save it without asking either.
  let release = null;
  await serve(JSON.stringify(two), new Promise((resolve) => { release = resolve; }));
  await page.click('#snapBtn');
  await page.waitForFunction(() => document.querySelector('#snapBtn').disabled);
  await page.fill('#consoleLine', 'help');
  await page.press('#consoleLine', 'Enter');
  await page.waitForSelector('#dialog:not(.hidden)');
  release();
  await toastSays('The snapshot is missing 2 of its registers, so it was not saved while another dialog was open. ' +
                  'Try again.');
  checkHas(await result(win), 'Changing settings', 'the command is answered after it');
  await close(win);
  await nothingSaved('and the snapshot was not saved');

  await page.unroute('**/api/snapshot');
  page.off('download', counter);
}

// A phone: nothing sticks out of the screen, and the dialog can be answered.
async function phone(browser, preview, desktop) {
  const win = await openWindow(browser, preview, { viewport: { width: 390, height: 844 }, deviceScaleFactor: shotsDir ? 2 : 1,
                                                   isMobile: true, hasTouch: true });
  const page = win.page;
  await page.goto(preview.base);
  await logIn(win, PIN);
  const fits = () => page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth);
  for (const name of ['live', 'settings', 'controller', 'tools']) {
    await openView(win, name);
    await pause(200);
    check(await fits(), `the ${name} view fits a phone's width`);
  }
  await shot(win, 'phone-tools', false);
  await openView(win, 'settings');
  await waitNow(win, 'EV_lvd', '13.10 V');
  await page.locator(changeButton('EV_lvd')).scrollIntoViewIfNeeded();
  await shot(win, 'phone-settings', false);
  await page.fill(newValue('EV_lvd'), '13.00');
  await page.tap(changeButton('EV_lvd'));
  await question(win);
  check(await fits(), 'the dialog fits a phone\'s width');
  const box = await page.locator('#dialogNo').boundingBox();
  check(box && box.x >= 0 && box.x + box.width <= 390, 'Cancel is on the screen');
  await page.waitForFunction(() => !document.querySelector('#dialogYes').disabled);
  await shot(win, 'phone-confirm', false);
  await page.tap('#dialogNo');
  await dialogHidden(win);
  await openView(win, 'live');
  await page.waitForSelector('#liveTiles .data-card');
  await shot(win, 'phone-live', false);
  await win.settle();

  // One browser is logged in at a time: the desktop window has lost its session to this one.
  await desktop.page.click('#statusBtn');
  await desktop.page.waitForSelector('#view-login:not(.hidden)');
  checkEq(await desktop.text('#loginMessage'), 'The session has ended. Log in again.', 'the first browser is told');
  check(await desktop.page.isHidden('#dialog'), 'and no dialog is left open');
  await desktop.settle();
  await win.context.close();
}

// A PIN that was set before anybody came, and wrong ones.
async function wrongPin(win) {
  const page = win.page;
  // What the login view shows once it tells somebody to wait, taken in one look: the wait may be
  // over before a second look.
  const waiting = async () => (await page.waitForFunction(() => {
    const message = document.querySelector('#loginMessage').textContent;
    return message.includes('Try again in') && {
      message,
      disabled: document.querySelector('#loginBtn').disabled,
      pin: document.querySelector('#pin').value,
    };
  })).jsonValue();
  const waitOver = async () => {
    await page.waitForFunction(() => !document.querySelector('#loginBtn').disabled, null, { timeout: 20000 });
    checkEq(await win.text('#loginMessage'), '', 'the message goes when the wait is over');
  };

  await page.goto(win.preview.base);
  await page.waitForSelector('#view-login:not(.hidden)');
  checkEq(await win.text('#loginTitle'), 'Enter the PIN', 'a PIN that exists is asked for');
  check(await page.isHidden('#pin2'), 'once');
  await page.fill('#pin', '1111');
  await page.click('#loginBtn');
  let shown = await waiting();
  checkEq(shown.message, 'That is not the PIN. Try again in 1 s.', 'the first wrong PIN costs a second');
  check(shown.disabled, 'and the button waits with it');
  checkEq(shown.pin, '', 'the wrong PIN is cleared');
  await waitOver();

  await page.fill('#pin', '2222');
  await page.click('#loginBtn');
  shown = await waiting();
  checkEq(shown.message, 'That is not the PIN. Try again in 2 s.', 'the second one costs two');
  await waitOver();

  await page.fill('#pin', '3333');
  await page.click('#loginBtn');
  shown = await waiting();
  checkEq(shown.message, 'That is not the PIN. Try again in 4 s.', 'the third one costs four');
  // A page opened during the wait is told about it too.
  await page.reload();
  shown = await waiting();
  check(/^A wrong PIN was entered\. Try again in [1-4] s\.$/.test(shown.message),
        `a new visitor learns why there is a wait: ${JSON.stringify(shown.message)}`);
  check(shown.disabled, 'and waits as well');
  await logIn(win, PIN);
  await win.settle();
}

// The ways a session ends without the page's doing.
async function sessionEnds(browser, win) {
  const page = win.page;
  const preview = win.preview;
  const ended = async (how) => {
    await page.waitForSelector('#view-login:not(.hidden)', { timeout: 20000 });
    checkEq(await win.text('#loginMessage'), 'The session has ended. Log in again.', how);
    check(await page.isHidden('#nav'), `${how}: the views are gone`);
    check(await page.isHidden(notice('scale')), `${how}: so are their notices`);
  };

  // Somebody ends it on the USB console.
  await preview.line('web logout', /The browser's session is ended/);
  await page.click('#liveBtn');
  await ended('ended on the USB console');
  await logIn(win, PIN);

  // Half an hour without anybody doing anything. The page's own reading does not count, and
  // the next one finds the session gone.
  await preview.line('sim skip 1860', /sim: 1860 s have passed/);
  await ended('ended by half an hour of nothing');
  await logIn(win, PIN);

  // The same time does not end a session that is used.
  const readNow = () => Promise.all([
    page.waitForResponse((answer) => answer.url().includes('/api/read') && !answer.url().includes('auto=1')),
    page.click('#liveBtn'),
  ]);
  await preview.line('sim skip 1500', /sim: 1500 s have passed/);
  await readNow();
  await preview.line('sim skip 1500', /sim: 1500 s have passed/);
  const answers = await readNow();
  checkEq(answers[0].status(), 200, 'a session that is used lasts');
  check(await page.isHidden('#view-login'), 'and the page stays where it is');

  // The PIN is taken away on the USB console: the next visitor chooses a new one.
  await preview.line('web pin clear', /There is no PIN now/);
  await page.click('#liveBtn');
  await ended('ended with the PIN');
  checkEq(await win.text('#loginTitle'), 'Choose a PIN', 'and a new PIN is to be chosen');
  await logIn(win, PIN);

  // A second window that tries to choose a PIN now is sent to the login.
  const late = await openWindow(browser, preview);
  await late.page.goto(preview.base);
  await late.page.waitForSelector('#view-login:not(.hidden)');
  checkEq(await late.text('#loginTitle'), 'Enter the PIN', 'the PIN that was just chosen is asked for');
  await late.settle();
  await late.context.close();
  await win.settle();
}

// The Opta goes away and comes back.
async function optaGone(win) {
  const page = win.page;
  const port = win.preview.port;
  win.allow = /ERR_CONNECTION_REFUSED|ERR_CONNECTION_RESET|ERR_EMPTY_RESPONSE/;
  await win.preview.kill();
  await page.click('#liveBtn');
  await page.waitForSelector(notice('offline'), { timeout: 30000 });
  checkHas(await win.text(notice('offline')), 'No answer from OptaView.', 'the page says that the Opta is gone');
  const reads = win.reads();
  await pause(5600);
  checkEq(win.reads(), reads, 'and stops reading by itself');

  // It comes back as after a power cut: without the session, and with the PIN it starts with.
  win.preview = await Preview.start(['--pin', PIN], port);
  await page.click(`${notice('offline')} button`);
  await page.waitForSelector('#view-login:not(.hidden)');
  check(await page.isHidden(notice('offline')), 'the notice goes when the Opta answers again');
  checkEq(await win.text('#loginTitle'), 'Enter the PIN', 'the old session is not known to it');
  await logIn(win, PIN);
  win.allow = null;
  await win.settle();
  return win.preview;
}

// One scenario is about the preview and not about the page. After a request it cannot read, the
// firmware takes what is still arriving for half a second and then closes (ovHttpDrain, which
// optaview_web_test.cpp tests on a clock that only counts). The preview has a clock of its own,
// and it keeps that limit only if its clock moves while the firmware works as well as while it
// waits. With a clock that stands still in a loop that never waits, one sender that never
// pauses holds the page and the console for as long as it keeps sending.
async function neverPauses(preview) {
  const started = performance.now();
  const socket = net.connect(preview.port, '127.0.0.1');
  const chunk = Buffer.alloc(16384, 0x61);
  let sending = true;
  let answer = '';
  const send = () => {
    while (sending && socket.write(chunk)) { /* until the socket takes no more for now */ }
  };
  socket.on('connect', () => {
    socket.write('GET / HTTP/1.1\r\nA: be\x01ll\r\n\r\n');  // refused at the control character
    send();
  });
  socket.on('drain', send);
  socket.on('data', (data) => { answer += data.toString(); });
  // A connection that is closed with bytes unread ends in a reset. 'close' follows either way.
  socket.on('error', () => {});
  const heldMs = await new Promise((resolve) => {
    const giveUp = setTimeout(() => socket.destroy(), 6000);  // the sender's own patience
    socket.on('close', () => {
      sending = false;
      clearTimeout(giveUp);
      resolve(Math.round(performance.now() - started));
    });
  });
  checkHas(answer, 'HTTP/1.1 400 ', 'a request that cannot be read is refused');
  // Half a second by the firmware's limit. The bound leaves room for a slow machine: without a
  // clock that moves, this connection is held until the sender gives up, six seconds from now.
  check(heldMs < 3000, `a sender that never pauses is let go of after half a second: it was held for ${heldMs} ms`);
  const asked = performance.now();
  await preview.line('timeout', /Reply timeout: \d+ ms/);
  check(performance.now() - asked < 2000, 'and the console answers again at once');
}

// ---------------------------------------------------------------------------------------------

async function main() {
  const browser = await chromium.launch();
  const previews = [];
  let failed = null;
  try {
    // An Opta that nobody has visited yet.
    scenario = 'first visit';
    const fresh = await Preview.start();
    previews.push(fresh);
    const desktop = await openWindow(browser, fresh);
    await firstVisit(desktop);
    scenario = 'live view';
    const profile = await liveView(desktop);
    scenario = 'settings view';
    await settingsView(desktop, profile);
    scenario = 'controller view';
    await controllerView(desktop, profile);
    scenario = 'tools view';
    await toolsView(desktop, profile);
    scenario = 'phone';
    await phone(browser, fresh, desktop);
    await desktop.context.close();
    scenario = 'a sender that never pauses';
    await neverPauses(fresh);

    // An Opta that starts with a PIN, as OptaViewConfig.h gives it one.
    scenario = 'wrong PIN';
    const guarded = await Preview.start(['--pin', PIN]);
    previews.push(guarded);
    const visitor = await openWindow(browser, guarded);
    await wrongPin(visitor);
    scenario = 'session ends';
    await sessionEnds(browser, visitor);
    scenario = 'Opta gone';
    previews.push(await optaGone(visitor));
    await visitor.context.close();
  } catch (error) {
    failed = error;
  }
  await browser.close();
  for (const preview of previews) {
    try { preview.child.kill('SIGKILL'); } catch (ignored) { /* it has stopped already */ }
  }
  if (failed) {
    console.error(`"${scenario}" stopped: ${failed.stack || failed}`);
    process.exit(1);
  }
  if (failures.length > 0) {
    console.error(`optaview page: ${failures.length} of ${checks} checks failed`);
    process.exit(1);
  }
  console.log(`optaview page: all ${checks} checks passed`);
}

main();
