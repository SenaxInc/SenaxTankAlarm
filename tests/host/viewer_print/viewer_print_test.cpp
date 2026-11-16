// Host tests for TankAlarm-112025-Viewer-BluesOpta/TankAlarm_ViewerPrint.h: the printed
// daily report states each reading and status by the dashboard card rules.
#include "TankAlarm_ViewerPrint.h"

#include <cmath>
#include <cstdio>
#include <cstring>

static int checks = 0;
static int failures = 0;

static void expectStr(const char *name, const char *got, const char *want) {
  checks++;
  if (strcmp(got, want) != 0) {
    failures++;
    printf("FAIL %s: got [%s] want [%s]\n", name, got, want);
  }
}

static void expectTrue(const char *name, bool ok) {
  checks++;
  if (!ok) {
    failures++;
    printf("FAIL %s\n", name);
  }
}

static void value(const char *name, const char *ot, const char *st, const char *mu, float v,
                  const char *fault, const char *wantLabel, const char *wantValue) {
  char label[12];
  char out[40];
  viewerPrintValueLine(ot, st, mu, v, fault, label, sizeof(label), out, sizeof(out));
  char n[96];
  snprintf(n, sizeof(n), "%s label", name);
  expectStr(n, label, wantLabel);
  snprintf(n, sizeof(n), "%s value", name);
  expectStr(n, out, wantValue);
}

static void change(const char *name, const char *ot, const char *st, const char *mu, float d,
                   bool wantLine, const char *want) {
  char out[32] = "";
  bool line = viewerPrintChangeLine(ot, st, mu, d, out, sizeof(out));
  char n[96];
  snprintf(n, sizeof(n), "%s has line", name);
  expectTrue(n, line == wantLine);
  if (wantLine) {
    snprintf(n, sizeof(n), "%s text", name);
    expectStr(n, out, want);
  }
}

int main() {
  // Tank with no unit: feet and inches; under a foot, inches alone (formatLevel).
  value("tank no unit", "tank", "currentLoop", "", 50.5f, "", "Level:  ", "4' 2.5\"");
  value("tank exactly a foot", "tank", "", "", 12.0f, "", "Level:  ", "1' 0.0\"");
  value("untyped under a foot", "", "", "", 7.0f, "", "Level:  ", "7.0\"");
  value("tank empty", "tank", "", "", 0.0f, "", "Level:  ", "0.0\"");
  value("tank negative", "tank", "", "", -1.0f, "", "Level:  ", "--");
  value("tank NaN", "tank", "", "", NAN, "", "Level:  ", "--");
  // Tank with a unit: value and unit, "inches" shortened.
  value("tank inches", "tank", "analog", "inches", 43.84f, "", "Level:  ", "43.8 in");
  value("tank cm", "tank", "analog", "cm", 120.0f, "", "Level:  ", "120.0 cm");
  // Other sensors read their unit, or the type default.
  value("gas psi", "gas", "currentLoop", "psi", 41.26f, "", "Value:  ", "41.3 psi");
  value("gas default unit", "gas", "", "", 12.0f, "", "Value:  ", "12.0 psi");
  value("rpm default unit", "rpm", "pulse", "", 1800.0f, "", "Value:  ", "1800.0 rpm");
  value("flow default unit", "flow", "", "", 3.5f, "", "Value:  ", "3.5 gpm");
  value("gas negative", "gas", "", "psi", -2.0f, "", "Value:  ", "-2.0 psi");
  // Unit case: inches/psi/rpm/gpm in any case print like the dashboard; others as sent.
  value("legacy PSI", "gas", "", "PSI", 41.26f, "", "Value:  ", "41.3 psi");
  value("Inches", "tank", "", "Inches", 43.84f, "", "Level:  ", "43.8 in");
  value("RPM", "rpm", "", "RPM", 1800.0f, "", "Value:  ", "1800.0 rpm");
  value("GPM", "flow", "", "GPM", 3.5f, "", "Value:  ", "3.5 gpm");
  value("other unit as sent", "tank", "", "CM", 120.0f, "", "Level:  ", "120.0 CM");
  // Float switch: ON/OFF, whatever the unit.
  value("float on", "tank", "digital", "inches", 1.0f, "", "Switch: ", "ON");
  value("float off", "tank", "digital", "", 0.0f, "", "Switch: ", "OFF");
  value("float NaN", "tank", "digital", "", NAN, "", "Switch: ", "--");
  // A fault replaces the value.
  value("fault", "tank", "currentLoop", "", 0.0f, "open", "Level:  ", "FAULT (open)");
  value("fault on gas", "gas", "currentLoop", "psi", 0.0f, "short", "Value:  ", "FAULT (short)");
  // Null strings are tolerated.
  value("nulls", nullptr, nullptr, nullptr, 12.0f, nullptr, "Level:  ", "1' 0.0\"");
  // An out-of-range reading prints without integer overflow, and a small buffer is not overrun.
  {
    char label[12];
    char out[40];
    viewerPrintValueLine("tank", "", "", 3.0e9f, "", label, sizeof(label), out, sizeof(out));
    expectTrue("huge reading in feet", strchr(out, '\'') != nullptr && out[0] == '2');
    char small[6];
    viewerPrintValueLine("tank", "", "", 3.0e9f, "", label, sizeof(label), small, sizeof(small));
    expectTrue("huge reading truncated", strlen(small) == sizeof(small) - 1);
  }
  // Long fault tag is truncated, not overrun.
  {
    char label[12];
    char out[8];
    viewerPrintValueLine("tank", "", "", 0.0f, "abcdefghijklmnop", label, sizeof(label), out, sizeof(out));
    expectTrue("small buffer terminated", strlen(out) == sizeof(out) - 1);
  }

  change("tank no unit", "tank", "", "", 1.24f, true, "+1.2\"");
  change("tank inches", "tank", "", "inches", -2.04f, true, "-2.0 in");
  change("gas", "gas", "", "psi", 0.5f, true, "+0.5 psi");
  change("legacy PSI", "gas", "", "PSI", 0.5f, true, "+0.5 psi");
  change("float has none", "tank", "digital", "", 1.0f, false, "");
  change("NaN has none", "tank", "", "", NAN, false, "");

  // Status line.
  {
    char out[64];
    viewerPrintStatusLine(true, "high", false, "", out, sizeof(out));
    expectStr("status alarm", out, "high  *ALARM*");
    viewerPrintStatusLine(true, "triggered", true, "", out, sizeof(out));
    expectStr("status snoozed", out, "triggered  *ALARM* (reminders snoozed)");
    viewerPrintStatusLine(true, "", false, nullptr, out, sizeof(out));
    expectStr("status alarm with no type", out, "ALARM  *ALARM*");
    viewerPrintStatusLine(false, "clear", false, "open", out, sizeof(out));
    expectStr("status fault without alarm", out, "Sensor fault");
    viewerPrintStatusLine(true, "sensor-fault", false, "open", out, sizeof(out));
    expectStr("status alarm wins over fault", out, "sensor-fault  *ALARM*");
    viewerPrintStatusLine(false, "clear", true, "", out, sizeof(out));
    expectStr("status normal", out, "Normal");
    // Longest alarm type the record holds (23 chars) still fits with the snooze note.
    viewerPrintStatusLine(true, "abcdefghijklmnopqrstuvw", true, "", out, sizeof(out));
    expectStr("status longest", out, "abcdefghijklmnopqrstuvw  *ALARM* (reminders snoozed)");
  }

  if (failures) {
    printf("%d of %d checks FAILED\n", failures, checks);
    return 1;
  }
  printf("viewer print: all %d checks passed\n", checks);
  return 0;
}
