// Ported from dalathegreat/Nissan-env200-Battery-Upgrade
//   leaf-can-bridge-3-port-env200/can-bridge-env200.c   (line refs Lnnn below)
#include "env200_translation.h"
#include "nissan_crc.h"
#include "can_bus.h"
#include <Arduino.h>

// ── State — L15-26 ───────────────────────────────────────────────────────────
static uint16_t  main_battery_soc = 0;   // 0-100 (after /10)
static uint16_t  GIDS             = 0;
static uint8_t   ticks10ms        = 0;
static BridgeBus battery_bus      = BUS_A;   // set from 0x55B (L317)

// ── Generated-frame templates — exact bytes from L22-27 ──────────────────────
static BridgeFrame e5E3 = {0x5E3, false, 5, {0x8E,0x00,0x00,0x00,0x80,0,0,0}};          // L22
static BridgeFrame e603 = {0x603, false, 1, {0x00,0,0,0,0,0,0,0}};                       // L23
static BridgeFrame e605 = {0x605, false, 1, {0x00,0,0,0,0,0,0,0}};                       // L24
static BridgeFrame e355 = {0x355, false, 8, {0x14,0x0a,0x13,0x97,0x10,0x00,0x40,0x00}};  // L25
static BridgeFrame e5C5 = {0x5C5, false, 8, {0x84,0x01,0x06,0x79,0x00,0x0C,0x00,0x00}};  // L27 (differs from LEAF)

static inline void inject(const BridgeFrame &g) { canbus_send(battery_bus, g); }

bool env200_translate(BridgeBus from, BridgeFrame &f) {
  switch (f.id) {

    case 0x1F2:  // L303-312 : every 4th (~40ms) inject 0x355 to battery
      ticks10ms++;
      if (ticks10ms > 3) { ticks10ms = 0; inject(e355); }
      break;

    case 0x55B:  // L313-323 : capture SOC, detect battery bus, inject 0x5E3 + 0x5C5
      main_battery_soc = (f.data[0] << 2) | ((f.data[1] & 0xC0) >> 6);
      main_battery_soc /= 10;                                   // 0-100.0 → 0-100
      battery_bus = from;
      inject(e5E3);
      inject(e5C5);
      break;

    case 0x5BC:  // L324-334 : GID rewrite + clear power-limit-reason/MaxGIDS
      if ((f.data[5] & 0x10) == 0x00)
        GIDS = (f.data[0] << 2) | ((f.data[1] & 0xC0) >> 6);
      f.data[0] = GIDS >> 2;
      f.data[1] = (GIDS << 6) & 0xC0;
      f.data[5] = f.data[5] & 0x03;
      break;

    case 0x59E: {  // L335-341 : QC capacity rescale for 40 kWh
      uint16_t temp = (230 * main_battery_soc) / 100;
      f.data[3] = (f.data[3] & 0xF0) | ((temp >> 5) & 0xF);
      f.data[4] = ((temp & 0x1F) << 3) | (f.data[4] & 0x07);
      calc_crc8(f);
      break;
    }

    case 0x679:  // L342-345 : inject ZE1 startup frames to battery
      inject(e603);
      inject(e605);
      break;

    default: break;
  }
  return true;  // env200 block list is only the 0xABC example → nothing blocked
}
