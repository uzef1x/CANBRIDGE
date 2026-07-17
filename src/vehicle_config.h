// ─────────────────────────────────────────────────────────────────────────────
// Runtime vehicle selection, persisted in NVS (ESP32 Preferences).
// One binary serves both cars; the choice survives reboots.
//
// SAFETY: the selection is read ONCE at boot and used for the whole session.
// Changing it persists to NVS and takes effect on the next reboot — the bridge
// never swaps translation logic mid-run (that would risk stale state on a live
// CAN bus in a moving car).
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

enum Vehicle { VEHICLE_LEAF = 0, VEHICLE_ENV200 = 1 };

void        vehicle_begin(void);        // load persisted choice; call in setup()
Vehicle     vehicle_active(void);       // the vehicle in effect this session
const char *vehicle_name(Vehicle v);
void        vehicle_serial_task(void);  // poll Serial for "leaf" / "env200"
