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

// Bring the buses up and print status. Call once from setup().
void bridge_begin();

// Pump both directions once. Call repeatedly from loop().
void bridge_task();
