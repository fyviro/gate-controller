#include "relay.h"
#include <Arduino.h>
#include "config.h"

static bool gateBusy = false;

void relayInit() {
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);
}

void triggerRelay() {
  if (gateBusy) return;

  gateBusy = true;

  digitalWrite(RELAY_PIN, LOW);
  delay(500);
  digitalWrite(RELAY_PIN, HIGH);

  delay(3000);
  gateBusy = false;
}
