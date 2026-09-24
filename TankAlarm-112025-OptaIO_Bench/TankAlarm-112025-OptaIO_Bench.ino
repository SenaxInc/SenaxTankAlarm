/*
  TankAlarm-112025 Opta I/O bench (CL-1)

  Serial tool for relay/float bench stage 1: drives the relay coils, relay LEDs and (Opta WiFi
  only) LED_USER one at a time and reads the I1-I8 terminals as analog or digital inputs, using the same pin tables
  as the firmware (TankAlarm_OptaIo.h).

  BENCH UNITS ONLY. Flashing this by USB replaces the TankAlarm client firmware on that Opta.
  Reflash the client firmware by USB when the bench session is over.

  Safety: no process loads on the relay terminals. Put a continuity meter on each NO/COM pair and
  judge the contacts by the meter, not by the LEDs (v2.2.16 drove the LEDs, not the coils).

  Serial monitor at 115200 baud, line ending Newline, CR or both; commands are case-insensitive.
  Relays and terminals are typed 1-based as printed on the front panel (R1-R4, I1-I8).

    h | ?             help
    i                 pin table, terminal modes, outputs-LOW time, coil pin state        A7
    c <n> <0|1>       drive relay n's coil (RELAYn)                                     A2
    l <n> <0|1>       drive relay n's LED only                                          A2
    u <0|1>           drive LED_USER (Opta WiFi only; the Lite has no USER LED)         L1
    x <n> <0|1>       v2.2.16 relay command: pin 7+(n-1) (LED_D0 + index)               A1, A2
    p <0|1>           1: INPUT_PULLUP on the coil pins (not driven); 0: OUTPUT LOW      A3
    m <t|*> <a|d|u>   read terminal t (or all) as analog / digital INPUT / INPUT_PULLUP A4, A5
    a                 read all eight terminals once                                     A4
    s [ms]            stream all eight terminals (default 250 ms, minimum 50); s stops  A4, A7
    sweep [t]         guided threshold sweep on I<t> (or all), CSV at the end           A4, D6
    f <t> [s]         count falling edges on I<t> for s seconds (default 10; blocks)    A6
    alt <t> [n]       n cycles (default 20) of analogRead then digitalRead on I<t>      A5
    r                 reset (NVIC_SystemReset)                                          A4, A7

  Stage 1 (plan v2 section 4):
    A7  Watch the meter and LEDs from power-on to the banner; i shows when outputs went LOW.
    A1  x 1 1 .. x 4 1: no contact closes; the LEDs of R1, R3, R2 light; x 4 (pin 10) nothing.
    A2  c <n> 1 / c <n> 0 close and open only contact n; l <n> 1 / l <n> 0 light LEDs only.
    L1  Opta WiFi only, optional: u 1 / u 0 shows which level lights LED_USER. The firmware
        never drives LED_USER (D1: no alarm light); skip L1 on an Opta Lite.
    A3  p 1: does any relay energise with the coil pins as INPUT_PULLUP? Then p 0.
    A4  sweep (analog), m * d then sweep, m * u then sweep (r between classes); I1/I2 separately.
    A5  alt 1, alt 2, alt 3, each after r; then m <t> d and alt <t> again.
    A6  m <t> u, then f <t> with a 24 V square wave at rising rates, and a 3.3/5 V hall output.
*/

#include <Arduino.h>
#include <TankAlarm_OptaIo.h>

#ifndef SERIAL_BAUD
#define SERIAL_BAUD 115200
#endif

static const uint32_t STREAM_DEFAULT_MS = 250;  // 4 Hz
static const uint32_t STREAM_MIN_MS = 50;
static const uint8_t ANALOG_SAMPLES = 5;        // median of 5 analogRead calls
static const uint8_t MAX_WORDS = 4;

static const char *const kSweepLevels[] = {"open", "0", "3", "3.3", "5", "7", "10", "12", "24"};
static const uint8_t SWEEP_LEVELS = sizeof(kSweepLevels) / sizeof(kSweepLevels[0]);
static const int SWEEP_SKIPPED = -1;

static uint32_t gOutputsLowAtMs = 0;
static char gMode[OPTA_INPUT_COUNT];           // 'a' analogRead, 'd' INPUT, 'u' INPUT_PULLUP
static uint8_t gFirstClass[OPTA_INPUT_COUNT];  // OptaIoClass first used this boot
static bool gLegacyPinReady[OPTA_RELAY_COUNT];
static bool gCoilPullup = false;
static bool gStreaming = false;
static uint32_t gStreamPeriodMs = STREAM_DEFAULT_MS;
static uint32_t gStreamLastMs = 0;

static char gLine[64];
static uint8_t gLineLen = 0;
static bool gLineTooLong = false;
static bool gLastWasCr = false;

static int gSweepStep = -1;      // -1: no sweep running
static int gSweepTerminal = -1;  // -1: all terminals
static int gSweepValue[SWEEP_LEVELS][OPTA_INPUT_COUNT];

static void printBanner();
static void printHelp();

void setup() {
  // A7 contract (CL-2's initRelayOutputs copies it): every coil and LED is an output driven LOW
  // before anything else runs.
  for (uint8_t r = 0; r < OPTA_RELAY_COUNT; ++r) {
    pinMode(optaRelayCoilPin(r), OUTPUT);
    digitalWrite(optaRelayCoilPin(r), LOW);
    pinMode(optaRelayLedPin(r), OUTPUT);
    digitalWrite(optaRelayLedPin(r), LOW);
  }
  pinMode(OPTA_LED_USER_PIN, OUTPUT);
  digitalWrite(OPTA_LED_USER_PIN, LOW);  // WiFi model only; no LED on the Lite
  gOutputsLowAtMs = millis();

  for (uint8_t t = 0; t < OPTA_INPUT_COUNT; ++t) {
    gMode[t] = 'a';
    gFirstClass[t] = OPTA_CLASS_NONE;
  }

  Serial.begin(SERIAL_BAUD);
  while (!Serial && millis() < 3000) {
    delay(10);
  }
  analogReadResolution(12);  // as the client, for the /4095 scale

  printBanner();
  printHelp();
}

// ---------------------------------------------------------------------------------------------
// Readings
// ---------------------------------------------------------------------------------------------

// Records a hardware use of terminal t; true when it is not the class first used this boot, so
// readings may be wrong until a reset (the firmware's RESTART rule).
static bool noteClass(uint8_t t, uint8_t cls) {
  if (gFirstClass[t] == OPTA_CLASS_NONE) {
    gFirstClass[t] = cls;
    return false;
  }
  return gFirstClass[t] != cls;
}

// Median, min and max of 5 analogRead(t): channel t = I(t+1), the call the client's
// readAnalogSensor and readVinVoltage make.
static int readAnalogMedian(uint8_t t, int &minRaw, int &maxRaw) {
  int s[ANALOG_SAMPLES];
  for (uint8_t i = 0; i < ANALOG_SAMPLES; ++i) s[i] = analogRead(t);
  noteClass(t, OPTA_CLASS_ADC);
  for (uint8_t i = 1; i < ANALOG_SAMPLES; ++i) {
    const int v = s[i];
    uint8_t j = i;
    while (j > 0 && s[j - 1] > v) {
      s[j] = s[j - 1];
      --j;
    }
    s[j] = v;
  }
  minRaw = s[0];
  maxRaw = s[ANALOG_SAMPLES - 1];
  return s[ANALOG_SAMPLES / 2];
}

static float rawToVolts(int raw) { return (float)raw / 4095.0f * 10.0f; }  // the firmware's scale

static int readDigital(uint8_t t) {
  noteClass(t, OPTA_CLASS_GPIO);
  return digitalRead(optaInputPin(t)) == HIGH ? 1 : 0;
}

// Terminal t in its current mode: analog median raw, or digital 0/1.
static int readTerminal(uint8_t t) {
  if (gMode[t] != 'a') return readDigital(t);
  int minRaw, maxRaw;
  return readAnalogMedian(t, minRaw, maxRaw);
}

static void printCell(uint8_t t, int value) {
  Serial.print('I');
  Serial.print(t + 1);
  Serial.print(' ');
  Serial.print(gMode[t]);
  Serial.print(' ');
  Serial.print(value);
  if (gMode[t] == 'a') {
    Serial.print(' ');
    Serial.print(rawToVolts(value), 2);
    Serial.print('V');
  }
}

static void printStreamRow() {
  Serial.print(F("t="));
  Serial.print(millis());
  for (uint8_t t = 0; t < OPTA_INPUT_COUNT; ++t) {
    Serial.print(t == 0 ? " " : " | ");
    printCell(t, readTerminal(t));
  }
  Serial.println();
}

static void printAllOnce() {
  for (uint8_t t = 0; t < OPTA_INPUT_COUNT; ++t) {
    Serial.print('I');
    Serial.print(t + 1);
    Serial.print(' ');
    Serial.print(gMode[t]);
    if (gMode[t] == 'a') {
      int minRaw, maxRaw;
      const int raw = readAnalogMedian(t, minRaw, maxRaw);
      Serial.print(F("  median "));
      Serial.print(raw);
      Serial.print(F("  min "));
      Serial.print(minRaw);
      Serial.print(F("  max "));
      Serial.print(maxRaw);
      Serial.print(F("  "));
      Serial.print(rawToVolts(raw), 2);
      Serial.println(F(" V"));
    } else {
      Serial.print(F("  "));
      Serial.println(readDigital(t));
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------------------------

static const char *className(uint8_t cls) {
  return cls == OPTA_CLASS_ADC ? "adc" : cls == OPTA_CLASS_GPIO ? "gpio" : "-";
}

static void printPins() {
  for (uint8_t r = 0; r < OPTA_RELAY_COUNT; ++r) {
    Serial.print(F("  R"));
    Serial.print(r + 1);
    Serial.print(F(" coil D"));
    Serial.print(optaRelayCoilPin(r));
    Serial.print(F("  LED pin "));
    Serial.print(optaRelayLedPin(r));
    Serial.print(F("  (v2.2.16 drove pin "));
    Serial.print(optaLegacyRelayPin(r));
    Serial.println(')');
  }
  Serial.print(F("  LED_USER pin "));
  Serial.print(OPTA_LED_USER_PIN);
  Serial.println(F(" (Opta WiFi only)"));
  for (uint8_t t = 0; t < OPTA_INPUT_COUNT; ++t) {
    Serial.print(F("  I"));
    Serial.print(t + 1);
    Serial.print(F(" pin "));
    Serial.print(optaInputPin(t));
    Serial.print(F("  mode "));
    Serial.print(gMode[t]);
    Serial.print(F("  first use "));
    Serial.println(className(gFirstClass[t]));
  }
}

static void printInfo() {
  printPins();
  Serial.print(F("  Outputs driven LOW at "));
  Serial.print(gOutputsLowAtMs);
  Serial.println(F(" ms after boot"));
  Serial.println(gCoilPullup ? F("  Coil pins: INPUT_PULLUP (p 1), coils not driven")
                             : F("  Coil pins: OUTPUT"));
  Serial.println(gStreaming ? F("  Stream: on") : F("  Stream: off"));
}

static void printBanner() {
  Serial.println();
  Serial.println(F("=============================================="));
  Serial.println(F(" TankAlarm-112025 Opta I/O bench (CL-1)"));
  Serial.println(F("=============================================="));
  Serial.println(F("Build: " __DATE__ " " __TIME__));
  Serial.println(F("BENCH UNITS ONLY: this replaces the client firmware; reflash the client by USB afterwards."));
  Serial.println(F("No process loads on relay terminals; a continuity meter on each NO/COM."));
  printInfo();
}

static void printHelp() {
  Serial.println(F("Commands (R1-R4 and I1-I8 are 1-based):"));
  Serial.println(F("  h | ?            help"));
  Serial.println(F("  i                pin table, modes, outputs-LOW time, coil pin state"));
  Serial.println(F("  c <n> <0|1>      relay n coil"));
  Serial.println(F("  l <n> <0|1>      relay n LED"));
  Serial.println(F("  u <0|1>          LED_USER (Opta WiFi only)"));
  Serial.println(F("  x <n> <0|1>      v2.2.16 relay command (pin 7+(n-1))"));
  Serial.println(F("  p <0|1>          1: coil pins INPUT_PULLUP (not driven); 0: OUTPUT LOW"));
  Serial.println(F("  m <t|*> <a|d|u>  read as analog / digital INPUT / digital INPUT_PULLUP"));
  Serial.println(F("  a                read all terminals once"));
  Serial.println(F("  s [ms]           stream on (default 250, min 50) / off"));
  Serial.println(F("  sweep [t]        guided threshold sweep, CSV at the end"));
  Serial.println(F("  f <t> [s]        count falling edges (default 10 s; blocks)"));
  Serial.println(F("  alt <t> [n]      n x (analogRead, digitalRead) on one terminal"));
  Serial.println(F("  r                reset"));
}

// ---------------------------------------------------------------------------------------------
// Sweep (a state machine: loop() keeps running while it waits for Enter)
// ---------------------------------------------------------------------------------------------

static void sweepPrompt() {
  Serial.print(F("Sweep "));
  Serial.print(gSweepStep + 1);
  Serial.print('/');
  Serial.print(SWEEP_LEVELS);
  const bool open = strcmp(kSweepLevels[gSweepStep], "open") == 0;
  Serial.print(open ? F(": leave ") : F(": apply "));
  if (!open) {
    Serial.print(kSweepLevels[gSweepStep]);
    Serial.print(F(" V to "));
  }
  if (gSweepTerminal < 0) {
    Serial.print(F("all terminals"));
  } else {
    Serial.print('I');
    Serial.print(gSweepTerminal + 1);
  }
  Serial.println(open ? F(" open (nothing connected); Enter = read, s = skip, q = quit")
                      : F("; Enter = read, s = skip, q = quit"));
}

static void printSweepCsv(int rows) {
  Serial.println(F("--- CSV for the bench record ---"));
  Serial.print(F("level"));
  for (uint8_t t = 0; t < OPTA_INPUT_COUNT; ++t) {
    Serial.print(F(",I"));
    Serial.print(t + 1);
    Serial.print(' ');
    Serial.print(gMode[t]);
  }
  Serial.println();
  for (int row = 0; row < rows; ++row) {
    Serial.print(kSweepLevels[row]);
    for (uint8_t t = 0; t < OPTA_INPUT_COUNT; ++t) {
      Serial.print(',');
      const int v = gSweepValue[row][t];
      if (v == SWEEP_SKIPPED) continue;
      Serial.print(v);
      if (gMode[t] == 'a') {
        Serial.print('/');
        Serial.print(rawToVolts(v), 2);
      }
    }
    Serial.println();
  }
  Serial.println(F("--- end ---"));
}

static void sweepInput(const char *line) {
  if (strcmp(line, "q") == 0) {
    printSweepCsv(gSweepStep);
    gSweepStep = -1;
    return;
  }
  if (line[0] == '\0') {
    for (uint8_t t = 0; t < OPTA_INPUT_COUNT; ++t) {
      gSweepValue[gSweepStep][t] = readTerminal(t);
      Serial.print(t == 0 ? "  " : " | ");
      printCell(t, gSweepValue[gSweepStep][t]);
    }
    Serial.println();
  } else if (strcmp(line, "s") == 0) {
    for (uint8_t t = 0; t < OPTA_INPUT_COUNT; ++t) gSweepValue[gSweepStep][t] = SWEEP_SKIPPED;
  } else {
    sweepPrompt();
    return;
  }
  if (++gSweepStep >= SWEEP_LEVELS) {
    printSweepCsv(SWEEP_LEVELS);
    gSweepStep = -1;
  } else {
    sweepPrompt();
  }
}

// ---------------------------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------------------------

static bool parseLong(const char *s, long &out) {
  if (s == nullptr || *s == '\0') return false;
  char *end = nullptr;
  out = strtol(s, &end, 10);
  return end != s && *end == '\0';
}

// A 1-based number typed by a person -> 0-based index, or -1.
static int parseIndex(const char *s, long count) {
  long v;
  return (parseLong(s, v) && v >= 1 && v <= count) ? (int)(v - 1) : -1;
}

static int parseLevel(const char *s) {
  long v;
  return (parseLong(s, v) && (v == 0 || v == 1)) ? (int)v : -1;
}

static void printWrite(const __FlashStringHelper *what, int pin, int level) {
  Serial.print(what);
  Serial.print(pin);
  Serial.println(level ? F(" -> HIGH") : F(" -> LOW"));
}

static void setMode(uint8_t t, char mode) {
  const uint8_t cls = mode == 'a' ? OPTA_CLASS_ADC : OPTA_CLASS_GPIO;
  if (mode != 'a') pinMode(optaInputPin(t), mode == 'u' ? INPUT_PULLUP : INPUT);  // never on 'a'
  gMode[t] = mode;
  const bool changed = (mode == 'a') ? (gFirstClass[t] == OPTA_CLASS_GPIO) : noteClass(t, cls);
  Serial.print('I');
  Serial.print(t + 1);
  Serial.print(F(" mode "));
  Serial.println(mode);
  if (changed) Serial.println(F("  class change without reboot: use r for a clean reading"));
}

static void countEdges(uint8_t t, uint32_t seconds) {
  const int pin = optaInputPin(t);
  noteClass(t, OPTA_CLASS_GPIO);
  Serial.print(F("Counting falling edges on I"));
  Serial.print(t + 1);
  Serial.print(F(" for "));
  Serial.print(seconds);
  Serial.println(F(" s..."));
  Serial.flush();
  uint32_t edges = 0, polls = 0;
  PinStatus last = digitalRead(pin);
  const uint32_t startMs = millis();
  const uint32_t durationMs = seconds * 1000UL;
  while (millis() - startMs < durationMs) {  // polled, like the firmware's pulse sampler
    const PinStatus v = digitalRead(pin);
    if (last == HIGH && v == LOW) ++edges;
    last = v;
    ++polls;
  }
  Serial.print(F("  edges "));
  Serial.print(edges);
  Serial.print(F("  rate "));
  Serial.print((float)edges / (float)seconds, 2);
  Serial.print(F(" Hz  polling "));
  Serial.print(polls / seconds);
  Serial.println(F(" reads/s"));
}

static void alternate(uint8_t t, uint32_t cycles) {
  const int pin = optaInputPin(t);
  noteClass(t, OPTA_CLASS_ADC);
  noteClass(t, OPTA_CLASS_GPIO);
  Serial.println(F("analogRead then digitalRead on the same terminal; r afterwards for clean readings"));
  for (uint32_t i = 0; i < cycles; ++i) {
    const int raw = analogRead(t);
    const int level = digitalRead(pin) == HIGH ? 1 : 0;  // the one deliberate digitalRead in 'a' mode
    Serial.print(F("  "));
    Serial.print(i + 1);
    Serial.print(F(": analog "));
    Serial.print(raw);
    Serial.print(' ');
    Serial.print(rawToVolts(raw), 2);
    Serial.print(F(" V | digital "));
    Serial.println(level);
    delay(100);
  }
}

static void handleCommand(char *words[], uint8_t n) {
  const char *cmd = words[0];
  const char *a1 = n > 1 ? words[1] : nullptr;
  const char *a2 = n > 2 ? words[2] : nullptr;

  if (!strcmp(cmd, "h") || !strcmp(cmd, "?")) {
    printHelp();
  } else if (!strcmp(cmd, "i")) {
    printInfo();
  } else if (!strcmp(cmd, "c") || !strcmp(cmd, "l") || !strcmp(cmd, "x")) {
    const int r = parseIndex(a1, OPTA_RELAY_COUNT);
    const int v = parseLevel(a2);
    if (r < 0 || v < 0) {
      Serial.println(F("usage: c|l|x <1-4> <0|1>"));
    } else if (cmd[0] == 'c') {
      if (gCoilPullup) {
        Serial.println(F("coil pins are INPUT_PULLUP: run p 0 first"));
        return;
      }
      digitalWrite(optaRelayCoilPin(r), v ? HIGH : LOW);
      Serial.print('R');
      Serial.print(r + 1);
      printWrite(F(" coil D"), optaRelayCoilPin(r), v);
    } else if (cmd[0] == 'l') {
      digitalWrite(optaRelayLedPin(r), v ? HIGH : LOW);
      Serial.print('R');
      Serial.print(r + 1);
      printWrite(F(" LED pin "), optaRelayLedPin(r), v);
    } else {
      const int pin = optaLegacyRelayPin(r);
      if (!gLegacyPinReady[r]) {  // as v2.2.16 initializeRelays, then setRelayState
        pinMode(pin, OUTPUT);
        gLegacyPinReady[r] = true;
      }
      digitalWrite(pin, v ? HIGH : LOW);
      Serial.print(F("v2.2.16 relay "));
      Serial.print(r + 1);
      printWrite(F(": pin "), pin, v);
    }
  } else if (!strcmp(cmd, "u")) {
    const int v = parseLevel(a1);
    if (v < 0) {
      Serial.println(F("usage: u <0|1>"));
      return;
    }
    digitalWrite(OPTA_LED_USER_PIN, v ? HIGH : LOW);
    printWrite(F("LED_USER pin "), OPTA_LED_USER_PIN, v);
  } else if (!strcmp(cmd, "p")) {
    const int v = parseLevel(a1);
    if (v < 0) {
      Serial.println(F("usage: p <0|1>"));
      return;
    }
    for (uint8_t r = 0; r < OPTA_RELAY_COUNT; ++r) {
      pinMode(optaRelayCoilPin(r), v ? INPUT_PULLUP : OUTPUT);
      if (!v) digitalWrite(optaRelayCoilPin(r), LOW);
    }
    gCoilPullup = v != 0;
    Serial.println(v ? F("WARNING: coil pins D0-D3 are INPUT_PULLUP and not driven; watch the meter (A3). p 0 restores.")
                     : F("Coil pins D0-D3: OUTPUT LOW"));
  } else if (!strcmp(cmd, "m")) {
    const char mode = a2 ? a2[0] : '\0';
    if (!a1 || (mode != 'a' && mode != 'd' && mode != 'u') || a2[1] != '\0') {
      Serial.println(F("usage: m <1-8|*> <a|d|u>"));
    } else if (!strcmp(a1, "*")) {
      for (uint8_t t = 0; t < OPTA_INPUT_COUNT; ++t) setMode(t, mode);
    } else {
      const int t = parseIndex(a1, OPTA_INPUT_COUNT);
      if (t < 0) {
        Serial.println(F("usage: m <1-8|*> <a|d|u>"));
      } else {
        setMode((uint8_t)t, mode);
      }
    }
  } else if (!strcmp(cmd, "a")) {
    printAllOnce();
  } else if (!strcmp(cmd, "s")) {
    long ms = (long)STREAM_DEFAULT_MS;
    if (a1 && !parseLong(a1, ms)) {
      Serial.println(F("usage: s [ms]"));
      return;
    }
    if (gStreaming && !a1) {
      gStreaming = false;
      Serial.println(F("stream off"));
      return;
    }
    gStreamPeriodMs = ms < (long)STREAM_MIN_MS ? STREAM_MIN_MS : (uint32_t)ms;
    gStreaming = true;
    gStreamLastMs = millis() - gStreamPeriodMs;
    Serial.print(F("stream every "));
    Serial.print(gStreamPeriodMs);
    Serial.println(F(" ms; s stops"));
  } else if (!strcmp(cmd, "sweep")) {
    gSweepTerminal = -1;
    if (a1) {
      gSweepTerminal = parseIndex(a1, OPTA_INPUT_COUNT);
      if (gSweepTerminal < 0) {
        Serial.println(F("usage: sweep [1-8]"));
        return;
      }
    }
    gSweepStep = 0;
    sweepPrompt();
  } else if (!strcmp(cmd, "f")) {
    const int t = parseIndex(a1, OPTA_INPUT_COUNT);
    long seconds = 10;
    if (t < 0 || (a2 && (!parseLong(a2, seconds) || seconds < 1 || seconds > 600))) {
      Serial.println(F("usage: f <1-8> [1-600 s]"));
    } else if (gMode[t] == 'a') {
      Serial.println(F("terminal is in analog mode: m <t> d or m <t> u first"));
    } else {
      countEdges((uint8_t)t, (uint32_t)seconds);
    }
  } else if (!strcmp(cmd, "alt")) {
    const int t = parseIndex(a1, OPTA_INPUT_COUNT);
    long cycles = 20;
    if (t < 0 || (a2 && (!parseLong(a2, cycles) || cycles < 1 || cycles > 1000))) {
      Serial.println(F("usage: alt <1-8> [1-1000]"));
    } else {
      alternate((uint8_t)t, (uint32_t)cycles);
    }
  } else if (!strcmp(cmd, "r")) {
    Serial.println(F("Resetting..."));
    Serial.flush();
    delay(100);
    NVIC_SystemReset();
  } else {
    Serial.print(F("Unknown command: "));
    Serial.println(cmd);
  }
}

static void handleLine(char *line) {
  for (char *p = line; *p; ++p) {
    if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
  }
  char *words[MAX_WORDS];
  uint8_t n = 0;
  for (char *p = line; *p && n < MAX_WORDS;) {
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '\0') break;
    words[n++] = p;
    while (*p && *p != ' ' && *p != '\t') ++p;
    if (*p) *p++ = '\0';
  }
  if (gSweepStep >= 0) {
    sweepInput(n > 0 ? words[0] : "");
  } else if (n > 0) {
    handleCommand(words, n);
  }
}

// Non-blocking line reader: CR, LF or CR LF ends a line.
static void readSerial() {
  while (Serial.available() > 0) {
    const int c = Serial.read();
    if (c < 0) break;
    if (c == '\r' || c == '\n') {
      if (c == '\n' && gLastWasCr) {
        gLastWasCr = false;
        continue;
      }
      gLastWasCr = (c == '\r');
      gLine[gLineLen] = '\0';
      if (gLineTooLong) {
        Serial.println(F("line too long, ignored"));
      } else {
        handleLine(gLine);
      }
      gLineLen = 0;
      gLineTooLong = false;
      continue;
    }
    gLastWasCr = false;
    if (gLineLen < sizeof(gLine) - 1) {
      gLine[gLineLen++] = (char)c;
    } else {
      gLineTooLong = true;
    }
  }
}

void loop() {
  readSerial();
  if (gStreaming && gSweepStep < 0 && millis() - gStreamLastMs >= gStreamPeriodMs) {
    gStreamLastMs = millis();
    printStreamRow();
  }
}
