// ─────────────────────────────────────────────────────────────────────────────
// Nissan LEAF battery-upgrade translation (ported from dalathegreat
// can-bridge-firmware.c). Called by the bridge for every frame when the selected
// vehicle is LEAF. Modifies `f` in place; returns true to forward, false to drop.
//
// Scope (all implemented):
//   0x1ED / 0x5EB / 0x50A  – battery-size + generation auto-detect
//   0x55B                  – SOC capture + ALU answer
//   0x1DB                  – startup counter, dash-SOC (AZE0), relay bits (ZE0)
//   0x5BC                  – the GID / capacity engine (ZE0 template + AZE0 rewrite)
//   0x50B / 0x50C          – ALU + PDM handling
//   0x59E                  – AZE0/ZE1 QC capacity passthrough/rewrite
//   0x5C0                  – muxed temperature passthrough/rewrite
//   generated DTC-clearing frames, and the AZE0 charge-time estimate — all ported.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include "can_frame.h"

bool leaf_translate(BridgeBus from, BridgeFrame &f);

// 1 s housekeeping tick (dala TCC0 ISR): ZE0 resets state if 0x1F2 goes missing.
void leaf_tick(void);
void leaf_reset_state(void);

// What the translation layer has auto-detected from live traffic (UI readout).
// `confirmed` is false while the value is still the boot assumption.
const char *leaf_detected_vehicle(bool *confirmed);
const char *leaf_detected_battery(bool *confirmed);
