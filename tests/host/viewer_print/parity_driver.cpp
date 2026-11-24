// Driver for parity_test.js: reads one case per line from stdin as
//   objectType <TAB> sensorType <TAB> measurementUnit <TAB> value <TAB> change
// ("-" stands for an empty field) and prints, per case,
//   value line <TAB> change line ("" when the row has none) <TAB> 1 if feet-and-inches else 0
// using TankAlarm_ViewerPrint.h, so the node side can compare it with the dashboard's own
// formatters taken from VIEWER_DASHBOARD_HTML.
#include "TankAlarm_ViewerPrint.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char *field(char *s) {
  return strcmp(s, "-") == 0 ? "" : s;
}

int main() {
  char line[256];
  while (fgets(line, sizeof(line), stdin)) {
    line[strcspn(line, "\r\n")] = '\0';
    char *parts[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    char *save = nullptr;
    int n = 0;
    for (char *tok = strtok_r(line, "\t", &save); tok && n < 5; tok = strtok_r(nullptr, "\t", &save)) {
      parts[n++] = tok;
    }
    if (n != 5) {
      fprintf(stderr, "bad case line\n");
      return 2;
    }
    const char *ot = field(parts[0]);
    const char *st = field(parts[1]);
    const char *mu = field(parts[2]);
    const float value = strtof(parts[3], nullptr);
    const float change = strtof(parts[4], nullptr);
    char label[12];
    char valueOut[64];
    char changeOut[32] = "";
    viewerPrintValueLine(ot, st, mu, value, "", label, sizeof(label), valueOut, sizeof(valueOut));
    if (!viewerPrintChangeLine(ot, st, mu, change, changeOut, sizeof(changeOut))) changeOut[0] = '\0';
    printf("%s\t%s\t%d\n", valueOut, changeOut, viewerPrintFeetInches(ot, st, mu) ? 1 : 0);
  }
  return 0;
}
