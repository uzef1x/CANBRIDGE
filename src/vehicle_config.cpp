#include "vehicle_config.h"
#include <Arduino.h>
#include <Preferences.h>

static Preferences prefs;
static Vehicle     g_active = VEHICLE_LEAF;   // in effect this session (set at boot)

const char *vehicle_name(Vehicle v) {
  return (v == VEHICLE_ENV200) ? "e-NV200" : "LEAF";
}

void vehicle_begin(void) {
  prefs.begin("bridge", false);
  uint8_t stored = prefs.getUChar("vehicle", VEHICLE_LEAF);
  g_active = (stored == VEHICLE_ENV200) ? VEHICLE_ENV200 : VEHICLE_LEAF;
  Serial.printf("[vehicle] active: %s  (send 'leaf' or 'env200' to change, then reboot)\n",
                vehicle_name(g_active));
}

Vehicle vehicle_active(void) { return g_active; }

// Persist a new choice (takes effect next boot — never swaps logic mid-run).
static void vehicle_store(Vehicle v) {
  prefs.putUChar("vehicle", (uint8_t)v);
  Serial.printf("[vehicle] stored: %s — reboot to apply\n", vehicle_name(v));
}

void vehicle_serial_task(void) {
  static String buf;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      buf.trim(); buf.toLowerCase();
      if      (buf == "leaf")   vehicle_store(VEHICLE_LEAF);
      else if (buf == "env200") vehicle_store(VEHICLE_ENV200);
      else if (buf.length())    Serial.println("[vehicle] unknown; send 'leaf' or 'env200'");
      buf = "";
    } else if (buf.length() < 16) {
      buf += c;
    }
  }
}
