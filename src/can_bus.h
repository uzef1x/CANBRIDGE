// ─────────────────────────────────────────────────────────────────────────────
// CAN driver layer — the ONLY hardware-specific module.
// Hides the two different controllers behind a uniform BridgeFrame API:
//   BUS_A = MCP2518FD via ACAN2517FD (classic CAN 2.0B)
//   BUS_B = native ESP32-S3 TWAI
// If the board/controller ever changes, only this file changes.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include "can_frame.h"

// Bring both buses up at 500 kbit/s classic. Returns true only if both start OK.
// CAN-A's MCP2518FD oscillator (20 or 40 MHz — the board ships with either) is
// taken from NVS if it was ever detected; otherwise this starts the detection
// state machine and returns without waiting for it.
bool canbus_begin();

// Drive the CAN-A oscillator detection state machine; no-op once the
// oscillator is known. CAN-pump task ONLY — it owns SPI to the MCP2518FD.
void canbus_poll();

// Persist a newly detected CAN-A oscillator. Arduino loop task ONLY: this is an
// NVS flash write, which must never stall the CAN-pump task.
void canbus_housekeeping();

// Non-blocking receive. Returns true and fills `out` if a frame was waiting.
bool canbus_receive(BridgeBus bus, BridgeFrame &out);

// Queue a frame for transmission on `bus`.
void canbus_send(BridgeBus bus, const BridgeFrame &in);
