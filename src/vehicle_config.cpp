#include "vehicle_config.h"
#include "telemetry.h"
#include <Arduino.h>
#include <Preferences.h>

static Preferences prefs;
static Vehicle     g_active = VEHICLE_MONITOR;   // in effect this session (set at boot)
static Vehicle     g_stored = VEHICLE_MONITOR;   // persisted choice (may differ until reboot)

const char *vehicle_name(Vehicle v) {
  if (v == VEHICLE_ENV200)  return "e-NV200";
  if (v == VEHICLE_MONITOR) return "Monitor";
  return "LEAF";
}

void vehicle_begin(void) {
  prefs.begin("bridge", false);
  uint8_t stored = prefs.getUChar("vehicle", VEHICLE_MONITOR);
  if      (stored == VEHICLE_ENV200)  g_active = VEHICLE_ENV200;
  else if (stored == VEHICLE_LEAF)    g_active = VEHICLE_LEAF;
  else                                g_active = VEHICLE_MONITOR;
  g_stored = g_active;
  Serial.printf("[vehicle] active: %s  (send 'leaf', 'env200' or 'monitor' to change, then reboot)\n",
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
      bool known = (buf == "leaf") || (buf == "env200") || (buf == "monitor");
      if      (known && !car_write_safe()) Serial.println("[vehicle] locked: car must be parked (or CAN silent)");
      else if (buf == "leaf")    vehicle_store(VEHICLE_LEAF);
      else if (buf == "env200")  vehicle_store(VEHICLE_ENV200);
      else if (buf == "monitor") vehicle_store(VEHICLE_MONITOR);
      else if (buf.length())     Serial.println("[vehicle] unknown; send 'leaf', 'env200' or 'monitor'");
      buf = "";
    } else if (buf.length() < 16) {
      buf += c;
    }
  }
}
