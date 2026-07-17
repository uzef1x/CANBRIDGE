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
bool canbus_begin();

// Non-blocking receive. Returns true and fills `out` if a frame was waiting.
bool canbus_receive(BridgeBus bus, BridgeFrame &out);

// Queue a frame for transmission on `bus`.
void canbus_send(BridgeBus bus, const BridgeFrame &in);
