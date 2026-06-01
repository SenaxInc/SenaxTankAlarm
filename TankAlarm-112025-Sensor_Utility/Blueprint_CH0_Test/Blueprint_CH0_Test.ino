#include "OptaBlue.h"

using namespace Opta;

static const unsigned long READ_MS = 1000UL;
static unsigned long gLastRead = 0;

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("=== Blueprint All-Channel 4-20mA Test ===");

  OptaController.begin();

  while (OptaController.getExpansionNum() == 0) {
    OptaController.update();
    Serial.println("Waiting for expansion...");
    delay(500);
  }

  Serial.print("Expansions: ");
  Serial.println(OptaController.getExpansionNum());

  // Configure ALL 8 channels (CH0..CH7 == I1..I8) as external-powered current ADC.
  for (uint8_t ch = 0; ch < 8; ++ch) {
    AnalogExpansion::beginChannelAsCurrentAdc(OptaController, 0, ch);
  }

  Serial.println("Ready! Reading all 8 channels every 1s. Let's see if the wire correction gets a reading!");
}

void loop() {
  OptaController.update();

  if (millis() - gLastRead < READ_MS) {
    return;
  }
  gLastRead = millis();

  AnalogExpansion exp = OptaController.getExpansion(0);
  if (!exp) {
    Serial.println("Expansion 0 unavailable");
    return;
  }

  exp.updateAnalogInputs();

  for (uint8_t ch = 0; ch < 8; ++ch) {
    uint16_t adc = exp.getAdc(ch, false);
    float ma = exp.pinCurrent(ch, false);
    
    // Check if the channel is registering an actual loop current (typically >= 3.5mA up to 21mA)
    bool active = (ma >= 3.5f && ma <= 21.5f);
    
    Serial.print("CH");
    Serial.print(ch);
    Serial.print(" adc=");
    Serial.print(adc);
    if (active) {
      Serial.print(" mA=");
      Serial.print(ma, 3);
      Serial.print(" [ACTIVE!]");
    } else {
      Serial.print(" mA=");
      Serial.print(ma, 3);
    }
    if (ch < 7) {
      Serial.print(" | ");
    }
  }
  Serial.println();
}
