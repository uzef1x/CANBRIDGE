// ─────────────────────────────────────────────────────────────────────────────
// Nissan 0x5BC (LB battery status: GIDs / full capacity / SOH / dash bars /
// charge-time) message model + (de)serialization.
//
// Ported from dalathegreat's Leaf_2011_5BC_message + convert_5bc_to_array /
// convert_array_to_5bc, with two deliberate changes:
//   1. Plain integer members instead of `int:N` bitfields — the AVR bitfield
//      layout is compiler/ABI-specific; explicit packing below is portable and
//      produces byte-identical output on the ESP32.
//   2. leaf5bc_unpack_capr fixes the upstream operator-precedence bug
//      `src[1] & 0xC0 >> 6` (which evaluates as `src[1] & 0x03`).
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <stdint.h>

struct Leaf5BC {
  uint16_t LB_CAPR;        // 10-bit  LB_Remain_Capacity (GIDs, 1 GID = 80 Wh)
  uint16_t LB_FULLCAP;     // 10-bit  LB_New_Full_Capacity
  uint8_t  LB_CAPSEG;      // 4-bit   remaining/full capacity bars (0-12)
  uint8_t  LB_AVET;        // 8-bit   average battery temp (offset -40)
  uint8_t  LB_SOH;         // 7-bit   capacity deterioration rate (SOH %)
  uint8_t  LB_CAPSW;       // 1-bit   selects LB_CAPSEG meaning
  uint8_t  LB_RLIMIT;      // 3-bit   output power limit reason
  uint8_t  LB_CAPBALCOMP;  // 1-bit   capacity balancing complete
  uint8_t  LB_RCHGTCON;    // 5-bit   remain-charge-time condition (mux)
  uint16_t LB_RCHGTIM;     // 13-bit  remain charge time (minutes)
};

// Pack into 8 CAN bytes — identical layout to dalathegreat convert_5bc_to_array.
static inline void leaf5bc_pack(const Leaf5BC &s, uint8_t d[8]) {
  d[0] = (uint8_t)(s.LB_CAPR >> 2);
  d[1] = (uint8_t)(((s.LB_CAPR << 6) & 0xC0) | ((s.LB_FULLCAP >> 4) & 0x1F));
  d[2] = (uint8_t)(((s.LB_FULLCAP << 4) & 0xF0) | (s.LB_CAPSEG & 0x0F));
  d[3] = (uint8_t)(s.LB_AVET);
  d[4] = (uint8_t)(((s.LB_SOH << 1) & 0xFE) | (s.LB_CAPSW & 1));
  d[5] = (uint8_t)(((s.LB_RLIMIT << 5) & 0xE0) | ((s.LB_CAPBALCOMP << 2) & 4) |
                   ((s.LB_RCHGTCON >> 3) & 3));
  d[6] = (uint8_t)(((s.LB_RCHGTCON << 5) & 0xE0) | ((s.LB_RCHGTIM >> 8) & 0x1F));
  d[7] = (uint8_t)(s.LB_RCHGTIM);
}

// Extract just LB_CAPR (GIDs) from 8 CAN bytes (bug-fixed vs upstream).
static inline uint16_t leaf5bc_unpack_capr(const uint8_t d[8]) {
  return (uint16_t)((d[0] << 2) | ((d[1] & 0xC0) >> 6));
}
