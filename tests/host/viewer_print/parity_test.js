'use strict';
// The printed daily report and the dashboard card must state a reading the same way.
// Runs the dashboard's own formatters (isNum, formatLevel, unitLabel, formatSwitch,
// formatValue from VIEWER_DASHBOARD_HTML) and parity_driver (TankAlarm_ViewerPrint.h) on
// the same grid of object types, sensor types, units, values and changes, and compares:
//   value: the card's value and unit; a feet-and-inches row has no unit after the value
//   change: the card's "+1.2 psi"; a feet-and-inches row prints inches as +1.2"
// Usage: node parity_test.js path/to/parity_driver [path/to/Viewer.ino]
const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');

const DRIVER = process.argv[2];
const SKETCH = process.argv[3] ||
  path.resolve(__dirname, '../../../TankAlarm-112025-Viewer-BluesOpta/TankAlarm-112025-Viewer-BluesOpta.ino');
if (!DRIVER) throw new Error('usage: node parity_test.js path/to/parity_driver [Viewer.ino]');

const text = fs.readFileSync(SKETCH, 'utf8');
const open = 'static const char VIEWER_DASHBOARD_HTML[] PROGMEM = R"HTML(';
const at = text.indexOf(open);
if (at < 0) throw new Error('VIEWER_DASHBOARD_HTML not found');
const page = text.slice(at + open.length, text.indexOf(')HTML";', at));

// Each formatter is a one-line function in the page; a missing or reshaped one fails loudly.
function pick(name) {
  const start = page.indexOf('function ' + name + '(');
  if (start < 0) throw new Error(name + ' not found in VIEWER_DASHBOARD_HTML');
  const end = page.indexOf('\n', start);
  return page.slice(start, end < 0 ? undefined : end);
}
const names = ['isNum', 'formatLevel', 'unitLabel', 'formatSwitch', 'formatValue'];
const js = new Function(names.map(pick).join('\n') + '\nreturn {' + names.join(',') + '};')();

const objectTypes = ['tank', 'gas', 'rpm', 'flow', 'engine', 'pump', ''];
const sensorTypes = ['', 'digital', 'currentLoop', 'analog'];
const units = ['', 'inches', 'Inches', 'psi', 'PSI', 'rpm', 'GPM', 'cm', 'CM'];
// The server rounds readings and changes to 0.1 before they reach the Viewer
// (publishViewerSummary), so exact binary ties such as 120.25, which C's printf and JS's
// toFixed round differently, never arrive and are left out of the grid.
const values = [0, 0.04, 0.5, 1, 7, 11.94, 12, 13.05, 50.5, 120.3, -1, 41.26, 1800, 3.5];
const changes = [0, 1.24, -2.04, -0.04, 12.5];

const cases = [];
for (const ot of objectTypes) for (const st of sensorTypes) for (const mu of units) {
  values.forEach((v, i) => cases.push({ ot, st, mu, v, d: changes[i % changes.length] }));
}
const input = cases.map((c) => [c.ot || '-', c.st || '-', c.mu || '-', String(c.v), String(c.d)].join('\t')).join('\n') + '\n';
const rows = execFileSync(DRIVER, { input }).toString().split('\n').filter((l) => l.length);
if (rows.length !== cases.length) throw new Error('driver returned ' + rows.length + ' rows for ' + cases.length + ' cases');

let failures = 0;
cases.forEach((c, i) => {
  // renderDataCard: objectType defaults to tank; a float has no unit and no change line.
  const ot = c.ot || 'tank';
  const digital = c.st === 'digital';
  const feetInches = !digital && !c.mu && ot === 'tank';
  const unit = digital ? '' : js.unitLabel(c.mu, ot);
  const val = digital ? js.formatSwitch(c.v) : js.formatValue(c.v, c.mu, ot);
  const wantValue = digital || feetInches ? val : val + ' ' + unit;
  const sign = c.d >= 0 ? '+' : '';
  const wantChange = digital ? '' : sign + c.d.toFixed(1) + (feetInches ? '"' : ' ' + unit);
  const [gotValue, gotChange, gotFeet] = rows[i].split('\t');
  if (gotValue !== wantValue || gotChange !== wantChange || (gotFeet === '1') !== feetInches) {
    failures++;
    if (failures <= 20) {
      console.log('FAIL ' + JSON.stringify(c) + ': print [' + gotValue + '] [' + gotChange + '] feet=' + gotFeet +
        ', dashboard [' + wantValue + '] [' + wantChange + '] feet=' + feetInches);
    }
  }
});

console.log(failures ? failures + ' of ' + cases.length + ' parity cases FAILED'
  : 'viewer print parity: all ' + cases.length + ' cases match the dashboard');
process.exit(failures ? 1 : 0);
