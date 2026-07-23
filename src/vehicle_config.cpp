#include "vehicle_config.h"
#include <Arduino.h>
#include <Preferences.h>

static Preferences prefs;
static Vehicle     g_active = VEHICLE_LEAF;   // in effect this session (set at boot)
static Vehicle     g_stored = VEHICLE_LEAF;   // persisted choice (may differ until reboot)

const char *vehicle_name(Vehicle v) {
  return (v == VEHICLE_ENV200) ? "e-NV200" : "LEAF";
}

void vehicle_begin(void) {
  prefs.begin("bridge", false);
  uint8_t stored = prefs.getUChar("vehicle", VEHICLE_LEAF);
  g_active = (stored == VEHICLE_ENV200) ? VEHICLE_ENV200 : VEHICLE_LEAF;
  g_stored = g_active;
  Serial.printf("[vehicle] active: %s  (send 'leaf' or 'env200' to change, then reboot)\n",
                vehicle_name(g_active));
}

Vehicle vehicle_active(void) { return g_active; }

// Persist a new choice (takes effect next boot — never swaps logic mid-run).
// Call ONLY from the Arduino loop task (vehicle_serial_task here, or
// webui_housekeeping's drain). NOT from the AsyncTCP/web callback or the CAN
// task: this does an NVS flash write, which disables the cache and briefly
// stalls both cores — harmless on the loop task, but it would hitch CAN
// forwarding if run on the CAN task. The web path stashes a request instead.
void vehicle_store(Vehicle v) {
  prefs.putUChar("vehicle", (uint8_t)v);
  g_stored = v;
  Serial.printf("[vehicle] stored: %s — reboot to apply\n", vehicle_name(v));
}

Vehicle vehicle_stored(void) { return g_stored; }

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
