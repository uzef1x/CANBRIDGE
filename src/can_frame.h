// ─────────────────────────────────────────────────────────────────────────────
// Neutral CAN frame + bus identity used throughout the bridge.
// The two CAN drivers (native TWAI, MCP2518FD/ACAN2517FD) each convert to/from
// this type at their boundary, so the bridge and translation layers never touch
// a library-specific frame struct.  Classic CAN 2.0B only → max 8 data bytes.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <stdint.h>

struct BridgeFrame {
  uint32_t id;        // 11-bit (or 29-bit if ext) identifier
  bool     ext;       // extended identifier?
  uint8_t  dlc;       // data length, 0..8
  uint8_t  data[8];
};

// Which physical bus a frame came from / goes to.
// BUS_A = MCP2518FD (CAN-A),  BUS_B = native TWAI (CAN-B).
// In the car one side faces the vehicle VCM, the other the battery (LBC);
// which is which is a wiring/config detail decided later.
enum BridgeBus : uint8_t { BUS_A = 0, BUS_B = 1 };
