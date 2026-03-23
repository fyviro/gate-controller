/**
 * Gate controller — ESP32 + Arduino framework.
 *
 * Sketch entry: hardware init, Wi-Fi AP, RTC, HTTP server.
 * Logic is split across .h/.cpp modules in this folder (Arduino IDE compiles all .cpp).
 */

#include <WiFi.h>
#include <Wire.h>
#include "RTClib.h"

#include "config.h"
#include "relay.h"
#include "rtc_device.h"
#include "users.h"
#include "web_server.h"

RTC_DS3231 rtc;

void setup() {
  Serial.begin(115200);

  relayInit();

  WiFi.softAP(AP_SSID, AP_PASSWORD);

  if (!rtc.begin()) {
    Serial.println("RTC not found");
    while (1)
      ;
  }

  Serial.println("RTC OK");
  Serial.println(WiFi.softAPIP());

  usersInit();
  registerWebRoutes();
  webServerBegin();
}

void loop() {
  server.handleClient();
}
