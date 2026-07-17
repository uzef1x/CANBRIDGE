// ─────────────────────────────────────────────────────────────────────────────
// Nissan 0x5C0 (battery temperature history, muxed MAX/AVG/MIN) model + pack.
// Ported from dalathegreat Leaf_2011_5C0_message (nissan_can_structs.h) and
// convert_5c0_to_array (helper_functions.c). De-bitfielded to plain members for
// AVR→ESP32 portability; byte layout identical.
//
// NOTE (faithful to source): convert_5c0_to_array does NOT write byte 6 — the
// incoming frame's data[6] is preserved. leaf5c0_pack does the same.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <stdint.h>

struct Leaf5C0 {
  uint8_t LB_HIS_DATA_SW;    // 2-bit  mux (1=MAX, 2=AVG, 3=MIN)
  uint8_t LB_HIS_HLVOL_TIMS; // 4-bit
  uint8_t LB_HIS_TEMP_WUP;   // 7-bit  temp at wake-up
  uint8_t LB_HIS_TEMP;       // 7-bit  temp
  uint8_t LB_HIS_INTG_CUR;   // 8-bit
  uint8_t LB_HIS_DEG_REGI;   // 7-bit
  uint8_t LB_HIS_CELL_VOL;   // 6-bit
  uint8_t LB_DTC;            // 8-bit  diagnostic trouble code
};

// Pack into CAN bytes, leaving d[6] untouched (matches dala convert_5c0_to_array).
static inline void leaf5c0_pack(const Leaf5C0 &s, uint8_t d[8]) {
  d[0] = (uint8_t)((s.LB_HIS_DATA_SW << 6) | s.LB_HIS_HLVOL_TIMS);
  d[1] = (uint8_t)(s.LB_HIS_TEMP_WUP << 1);
  d[2] = (uint8_t)(s.LB_HIS_TEMP << 1);
  d[3] = (uint8_t)(s.LB_HIS_INTG_CUR);
  d[4] = (uint8_t)(s.LB_HIS_DEG_REGI << 1);
  d[5] = (uint8_t)(s.LB_HIS_CELL_VOL << 2);
  // d[6] intentionally left unchanged
  d[7] = (uint8_t)(s.LB_DTC);
}
