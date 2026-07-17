// ─────────────────────────────────────────────────────────────────────────────
// Nissan LEAF battery-upgrade translation (ported from dalathegreat
// can-bridge-firmware.c). Called by the bridge for every frame when the selected
// vehicle is LEAF. Modifies `f` in place; returns true to forward, false to drop.
//
// Scope so far (this increment):
//   0x1ED / 0x5EB / 0x50A  – battery-size + generation auto-detect
//   0x55B                  – SOC capture + ALU answer
//   0x1DB                  – startup counter, dash-SOC (AZE0), relay bits (ZE0)
//   0x5BC                  – the GID / capacity engine (ZE0 template + AZE0 rewrite)
// Not yet ported (later increments): 0x50B/0x50C ALU+PDM, 0x59E, 0x5C0, generated
// DTC-clearing frames, and the AZE0 charge-time estimate (upstream marks it WIP).
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include "can_frame.h"

bool leaf_translate(BridgeBus from, BridgeFrame &f);

// 1 s housekeeping tick (dala TCC0 ISR): ZE0 resets state if 0x1F2 goes missing.
void leaf_tick(void);
void leaf_reset_state(void);
