// ─────────────────────────────────────────────────────────────────────────────
// Nissan e-NV200 battery-upgrade translation (24 kWh → 40 kWh).
// Ported faithfully from dalathegreat/Nissan-env200-Battery-Upgrade
//   leaf-can-bridge-3-port-env200/can-bridge-env200.c
// This bridge is hardcoded for the 24→40 kWh upgrade — no generation/battery
// autodetection (unlike the LEAF bridge). Distinct from LEAF: different 0x5C5
// payload, a leaner 0x59E, and 0x5BC clears data[5] low bits.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include "can_frame.h"

bool env200_translate(BridgeBus from, BridgeFrame &f);
