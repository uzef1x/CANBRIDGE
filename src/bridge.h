// ─────────────────────────────────────────────────────────────────────────────
// Bridge core — pumps frames between the two buses.
//
// Every received frame passes through translate() before being forwarded. In
// Milestone 1 translate() is a pass-through (transparent bridge). The LEAF and
// e-NV200 battery-upgrade logic will live behind translate() in later phases,
// so the pumping/forwarding code here never needs to change.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include "can_frame.h"

// Bring the buses up and print status. Call once from setup(), before
// bridge_start_can_task().
void bridge_begin();

// Create the dedicated, watchdog-monitored CAN-pump task (the only task that
// touches the CAN drivers / telemetry state). Call once from setup() after
// bridge_begin(). Forwarding runs on this task from here on — loop() no longer
// pumps CAN.
void bridge_start_can_task();
