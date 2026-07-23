// ─────────────────────────────────────────────────────────────────────────────
// Nissan LEAF battery-upgrade translation.
// Ported faithfully from dalathegreat/Nissan-LEAF-Battery-Upgrade
//   Software/CANBRIDGE-3port/leaf-can-bridge-3-port/can-bridge-firmware.c
// Line references (Lnnn) below point into that file. Every constant / byte value
// is taken from dala's source — nothing invented.
//
// Architecture mapping (AVR 3-port → ESP32 2-bus):
//   dala: can_handler() modifies frame, injects generated frames to
//         battery_can_bus, then forwards frame to the OTHER bus unless blocked.
//   here: leaf_translate() modifies `f`, injects via canbus_send(battery_bus,…),
//         and returns false to block (the bridge forwards to the other bus).
// ─────────────────────────────────────────────────────────────────────────────
#include "leaf_translation.h"
#include "leaf_5bc.h"
#include "leaf_5c0.h"
#include "nissan_crc.h"
#include "can_bus.h"
#include <Arduino.h>

// ── Generation / battery (auto-detected) — L3-13 ─────────────────────────────
#define MY_LEAF_ZE0   0
#define MY_LEAF_AZE0  1
#define MY_BATTERY_24 0
#define MY_BATTERY_30 1
#define MY_BATTERY_40 2
#define MY_BATTERY_62 3

// ── Charge states / charge-time mux — dala can-bridge-firmware.h L41-50 ───────
#define CHARGING_SLOW           0x20
#define CHARGING_QUICK          0xC0
#define CHARGING_IDLE           0x60
#define QUICK_CHARGE            0b00000
#define NORMAL_CHARGE_200V_100  0b01001
#define NORMAL_CHARGE_200V_80   0b01010
#define NORMAL_CHARGE_100V_100  0b10001
#define NORMAL_CHARGE_100V_80   0b10010

// ── Dash SOC window — L15-16, L54-55 ─────────────────────────────────────────
#define LB_MIN_SOC     0
#define LB_MAX_SOC     1000
#define MINPERCENTAGE  50
#define MAXPERCENTAGE  950

// ── Charge-timer minutes — L21-27 ────────────────────────────────────────────
#define time_100_with_200V_in_minutes 430
#define time_80_with_200V_in_minutes  340
#define time_100_with_100V_in_minutes 700
#define time_80_with_100V_in_minutes  560
#define time_100_with_66kW_in_minutes 190
#define time_80_with_66kW_in_minutes  150

// ── Battery temp LUT (offset -40C), index data[3]/20 — L46 ───────────────────
static const uint8_t temp_lut[13] = {25,28,31,34,37,50,63,76,80,82,85,87,90};

// ── State (dala globals, L8-92) ──────────────────────────────────────────────
static uint8_t  My_Leaf    = MY_LEAF_AZE0;   // L13 boot AZE0
static uint8_t  My_Battery = MY_BATTERY_24;  // L8 boot 24kWh
// UI-only: true once the value above was set from observed traffic rather than
// the boot assumption. Never read by translation logic.
static bool     My_Leaf_confirmed    = false;
static bool     My_Battery_confirmed = false;

static uint8_t  charging_state          = 0;
static uint8_t  max_charge_80_requested = 0;
static uint8_t  starting_up             = 0;
static uint8_t  VCM_WakeUpSleepCommand  = 0;
static uint8_t  ALU_question            = 0;
static uint8_t  CANMASK                 = 0;
static uint8_t  Byte2_50B               = 0;
static uint8_t  seen_1da                = 0;
static uint8_t  seconds_without_1f2     = 0;
static uint8_t  ticker40ms              = 0;
static uint8_t  content_3B8             = 0;
static uint8_t  flip_3B8                = 0;
static uint8_t  PRUN_39X                = 0;
static uint8_t  startup_counter_39X     = 0;

static uint16_t GIDS               = 0;
static uint8_t  battery_soc         = 0;
static int16_t  dash_soc            = 0;
static uint16_t battery_soc_pptt    = 0;
static uint8_t  cmr_idx             = QUICK_CHARGE;
static uint8_t  skip_5bc            = 1;
static uint8_t  alternate_5bc       = 0;
static uint16_t current_capacity_wh = 0;
static uint8_t  main_battery_temp   = 0;
static uint16_t startup_counter_1DB = 0;
static uint8_t  swap_5c0_idx        = 0;
static BridgeBus battery_bus        = BUS_A;   // set from 0x1DB (L339)

// ── 0x5BC swap templates — L84-85 ────────────────────────────────────────────
static Leaf5BC swap_5bc_remaining = {0x12C, 0x32, 0,  50, 99, 0, 0, 1, 0b01001, 0};
static Leaf5BC swap_5bc_full      = {0x12C, 0x32, 12, 50, 99, 1, 0, 1, 0b01001, 0};
static uint8_t saved_5bc_data[8]  = {0};

// ── 0x5C0 swap templates — L88-90 ────────────────────────────────────────────
static Leaf5C0 swap_5c0_max = {1, 0, 66, 66, 0, 0x02, 0x39, 0};
static Leaf5C0 swap_5c0_avg = {2, 0, 64, 64, 0, 0x02, 0x28, 0};
static Leaf5C0 swap_5c0_min = {3, 0, 64, 64, 0, 0x02, 0x15, 0};

// ── Generated-frame templates — exact bytes from L95-111 ─────────────────────
static BridgeFrame f355 = {0x355, false, 8, {0x14,0x0a,0x13,0x97,0x10,0x00,0x40,0x00}}; // L98
static BridgeFrame f625 = {0x625, false, 6, {0x02,0x00,0xff,0x1d,0x20,0x00,0,0}};        // L95 kills U215B
static BridgeFrame f5C5 = {0x5C5, false, 8, {0x40,0x01,0x2F,0x5E,0x00,0x00,0x00,0x00}};  // L96 kills U214E
static BridgeFrame f5EC = {0x5EC, false, 1, {0x00,0,0,0,0,0,0,0}};                        // L97
static BridgeFrame f3B8 = {0x3B8, false, 5, {0x7F,0xE8,0x01,0x07,0xFF,0,0,0}};            // L100 kills U1000/P318E
static BridgeFrame f605 = {0x605, false, 1, {0,0,0,0,0,0,0,0}};                           // L104
static BridgeFrame f607 = {0x607, false, 1, {0,0,0,0,0,0,0,0}};                           // L105
static BridgeFrame f45E = {0x45E, false, 1, {0x00,0,0,0,0,0,0,0}};                        // L106
static BridgeFrame f481 = {0x481, false, 2, {0x40,0x00,0,0,0,0,0,0}};                     // L107
static BridgeFrame f390 = {0x390, false, 8, {0x04,0x00,0x00,0x00,0x00,0x80,0x3c,0x07}};   // L110 kills P3196
static BridgeFrame f393 = {0x393, false, 8, {0x00,0x10,0x00,0x00,0x20,0x00,0x00,0x02}};   // L111 kills P3196

static inline void inject(const BridgeFrame &g) { canbus_send(battery_bus, g); }

static inline uint8_t tempFromByte(uint8_t d3) {  // L541-542 / L662-663
  uint8_t idx = d3 / 20;
  if (idx > 12) idx = 12;                          // guard temp_lut[13] bounds
  return temp_lut[idx] + 1;
}

void leaf_reset_state() {   // L206-210
  charging_state = 0;
  startup_counter_1DB = 0;
  startup_counter_39X = 0;
}

// 1 s tick (dala TCC0 ISR L222-241): on ZE0, reset state if 0x1F2 goes missing.
void leaf_tick() {
  static uint32_t last = 0;
  const uint32_t now = millis();
  if (now - last < 1000) return;
  last = now;
  if (My_Leaf == MY_LEAF_ZE0) {
    seconds_without_1f2++;
    if (seconds_without_1f2 > 1) leaf_reset_state();
  }
}

bool leaf_translate(BridgeBus from, BridgeFrame &f) {
  switch (f.id) {

    case 0x1ED:  My_Battery = MY_BATTERY_62; My_Battery_confirmed = true; break;  // L285-288
    case 0x5EB:  if (My_Battery != MY_BATTERY_62) My_Battery = MY_BATTERY_40;
                 My_Battery_confirmed = true; break;                              // L307-317

    case 0x5B9:  // L289-306
      inject(f45E);
      inject(f481);
      if (My_Leaf == MY_LEAF_ZE0) {
        f.dlc = 7;
        f.data[5] = (charging_state == CHARGING_SLOW) ? 0xFF : 0x4A;
        f.data[6] = 0x80;
      }
      break;

    case 0x1DA:  if (My_Leaf == MY_LEAF_ZE0) seen_1da = 10; break;        // L318-325

    case 0x284:  // L326-337 : every 2nd (40ms) inject 0x355
      ticker40ms++;
      if (ticker40ms > 1) { ticker40ms = 0; inject(f355); }
      break;

    case 0x50A:  // L394-407 : generation detect
      if (f.dlc == 6)      { My_Leaf = MY_LEAF_ZE0; My_Leaf_confirmed = true; f.dlc = 8; f.data[6] = 0x04; f.data[7] = 0x00; }
      else if (f.dlc == 8) { My_Leaf = MY_LEAF_AZE0; My_Leaf_confirmed = true; }
      break;

    case 0x380:  // L408-418
      if (from != battery_bus) { My_Leaf = MY_LEAF_ZE0; My_Leaf_confirmed = true; }  // ZE0 OBC on vehicle side
      break;

    case 0x50B:  // L419-431
      VCM_WakeUpSleepCommand = (f.data[3] & 0xC0) >> 6;
      Byte2_50B = f.data[2];
      if (My_Leaf == MY_LEAF_ZE0) {
        CANMASK = (f.data[2] & 0x04) >> 2;
        f.dlc = 7;
        f.data[6] = 0x00;
      }
      break;

    case 0x50C:  // L433-492 : ALU capture + inject 100ms battery frames
      ALU_question = f.data[4];
      inject(f625);
      inject(f5C5);
      inject(f3B8);
      if (My_Leaf == MY_LEAF_ZE0) {  // synthesize AZE0 PDM 0x390/0x393
        if (startup_counter_39X < 250) startup_counter_39X++;
        f390.data[0] = 0x04; f390.data[3] = 0x00; f390.data[5] = 0x80; f390.data[6] = 0x3C;
        f393.data[1] = 0x10;
        if (startup_counter_39X > 13) { f390.data[3] = 0x02; f390.data[6] = 0x00; f393.data[1] = 0x20; }
        if (startup_counter_39X > 15) { f390.data[3] = 0x03; f390.data[6] = 0x00; }
        if (startup_counter_39X > 20 && Byte2_50B == 0) {     // shutdown
          f390.data[0] = 0x08; f390.data[3] = 0x01; f390.data[5] = 0x90; f390.data[6] = 0x00;
          f393.data[1] = 0x70;
        }
        PRUN_39X = (PRUN_39X + 1) % 3;
        f390.data[7] = (PRUN_39X << 4);
        f393.data[7] = (PRUN_39X << 4);
        calc_checksum4(f390);
        calc_checksum4(f393);
        inject(f390);
        inject(f393);
      }
      content_3B8 = (content_3B8 + 1) % 15;
      f3B8.data[2] = content_3B8;
      if (flip_3B8) { flip_3B8 = 0; f3B8.data[1] = 0xC8; }
      else          { flip_3B8 = 1; f3B8.data[1] = 0xE8; }
      break;

    case 0x55B:  // L494-522 : ALU answer + SOC capture
      f.data[2] = (ALU_question == 0xB2) ? 0xAA : 0x55;
      if (My_Leaf == MY_LEAF_ZE0)
        f.data[6] = (f.data[6] & 0xCF) | (CANMASK == 0 ? 0x20 : 0x10);
      battery_soc_pptt = (uint16_t)((f.data[0] << 2) | ((f.data[1] & 0xC0) >> 6));
      battery_soc      = (uint8_t)(battery_soc_pptt / 10);
      calc_crc8(f);
      break;

    case 0x1DB:  // L338-393
      battery_bus = from;                                        // L339
      if (f.data[2] == 0xFF) starting_up |= 1; else starting_up &= ~1;
      if (My_Leaf == MY_LEAF_ZE0) {
        if (VCM_WakeUpSleepCommand == 3) f.data[3] = (f.data[3] & 0xD7) | 0x28;   // FRLYON+INTERLOCK
        if (startup_counter_1DB >= 100 && startup_counter_1DB <= 300) f.data[3] = (f.data[3] | 0x10);
        if (startup_counter_1DB < 1000) startup_counter_1DB++;
      }
      if (My_Leaf == MY_LEAF_AZE0) {
        dash_soc = LB_MIN_SOC + (LB_MAX_SOC - LB_MIN_SOC) *
                   (1.0f * battery_soc_pptt - MINPERCENTAGE) / (MAXPERCENTAGE - MINPERCENTAGE);
        if (dash_soc < 0)    dash_soc = 0;
        if (dash_soc > 1000) dash_soc = 1000;
        dash_soc /= 10;
        f.data[4] = (uint8_t)dash_soc;
      }
      if (max_charge_80_requested && charging_state == CHARGING_SLOW && battery_soc > 80) {
        f.data[1] = (f.data[1] & 0xE0) | 2;
        f.data[3] = (f.data[3] & 0xEF) | 0x10;
      }
      calc_crc8(f);
      break;

    case 0x5BC: {  // L523-736 : GID / capacity engine
      if (f.data[0] != 0xFF) starting_up &= ~4; else starting_up |= 4;   // L524-528
      if (My_Leaf == MY_LEAF_ZE0) {
        uint16_t soh = (f.data[4] & 0xFE) >> 1;
        if (f.data[0] != 0xFF) {
          if ((f.data[5] & 0x10) == 0x00) {
            uint16_t capr = leaf5bc_unpack_capr(f.data);
            swap_5bc_remaining.LB_CAPR = capr; swap_5bc_full.LB_CAPR = capr;
            swap_5bc_remaining.LB_SOH  = soh;  swap_5bc_full.LB_SOH  = soh;
            current_capacity_wh = swap_5bc_full.LB_CAPR * 80;
            main_battery_temp   = tempFromByte(f.data[3]);
          }
        } else {
          swap_5bc_remaining.LB_CAPR = 0x3FF; swap_5bc_full.LB_CAPR = 0x3FF;
          swap_5bc_remaining.LB_RCHGTIM = 0;  swap_5bc_remaining.LB_RCHGTCON = 0;
        }
        if (startup_counter_1DB < 600) {                          // L554-575 boot GID max
          uint16_t g = 220;
          switch (My_Battery) {
            case MY_BATTERY_24: g = 220; break;
            case MY_BATTERY_30: g = 310; break;
            case MY_BATTERY_40: g = 420; break;
            case MY_BATTERY_62: g = 630; break;
          }
          swap_5bc_remaining.LB_CAPR = g; swap_5bc_full.LB_CAPR = g;
        }
        if (skip_5bc) skip_5bc--;
        if (skip_5bc == 0) {                                      // L578-646 rebuild every 5th
          switch (cmr_idx) {
            case QUICK_CHARGE:
              swap_5bc_full.LB_RCHGTIM = swap_5bc_remaining.LB_RCHGTIM;
              swap_5bc_remaining.LB_RCHGTCON = cmr_idx; swap_5bc_full.LB_RCHGTCON = cmr_idx;
              cmr_idx = NORMAL_CHARGE_200V_100; break;
            case NORMAL_CHARGE_200V_100:
              swap_5bc_remaining.LB_RCHGTIM = time_100_with_200V_in_minutes -
                  ((time_100_with_200V_in_minutes * battery_soc) / 100);
              swap_5bc_full.LB_RCHGTIM = swap_5bc_remaining.LB_RCHGTIM;
              swap_5bc_remaining.LB_RCHGTCON = cmr_idx; swap_5bc_full.LB_RCHGTCON = cmr_idx;
              cmr_idx = NORMAL_CHARGE_200V_80; break;
            case NORMAL_CHARGE_200V_80:
              swap_5bc_remaining.LB_RCHGTIM = (battery_soc > 80) ? 0 :
                  (time_80_with_200V_in_minutes - ((time_80_with_200V_in_minutes * battery_soc) / 100));
              swap_5bc_full.LB_RCHGTIM = swap_5bc_remaining.LB_RCHGTIM;
              swap_5bc_remaining.LB_RCHGTCON = cmr_idx; swap_5bc_full.LB_RCHGTCON = cmr_idx;
              cmr_idx = NORMAL_CHARGE_100V_100; break;
            case NORMAL_CHARGE_100V_100:
              swap_5bc_remaining.LB_RCHGTIM = time_100_with_100V_in_minutes -
                  ((time_100_with_100V_in_minutes * battery_soc) / 100);
              swap_5bc_full.LB_RCHGTIM = swap_5bc_remaining.LB_RCHGTIM;
              swap_5bc_remaining.LB_RCHGTCON = cmr_idx; swap_5bc_full.LB_RCHGTCON = cmr_idx;
              cmr_idx = NORMAL_CHARGE_100V_80; break;
            case NORMAL_CHARGE_100V_80:
              swap_5bc_remaining.LB_RCHGTIM = (battery_soc > 80) ? 0 :
                  (time_80_with_100V_in_minutes - ((time_80_with_100V_in_minutes * battery_soc) / 100));
              swap_5bc_full.LB_RCHGTIM = swap_5bc_remaining.LB_RCHGTIM;
              swap_5bc_remaining.LB_RCHGTCON = cmr_idx; swap_5bc_full.LB_RCHGTCON = cmr_idx;
              cmr_idx = QUICK_CHARGE; break;
          }
          swap_5bc_remaining.LB_AVET = main_battery_temp;
          swap_5bc_full.LB_AVET      = main_battery_temp;
          if (My_Battery != MY_BATTERY_24 && charging_state == CHARGING_QUICK) {  // L625-634 ZE0 QC clamp
            uint16_t scaled = (uint16_t)((battery_soc * 300) / 100);
            swap_5bc_remaining.LB_CAPR = scaled; swap_5bc_full.LB_CAPR = scaled;
          }
          if (alternate_5bc) { leaf5bc_pack(swap_5bc_remaining, saved_5bc_data); alternate_5bc = 0; }
          else               { leaf5bc_pack(swap_5bc_full,      saved_5bc_data); alternate_5bc = 1; }
          skip_5bc = 5;
        }
        for (uint8_t i = 0; i < 8; i++) f.data[i] = saved_5bc_data[i];  // L648 frame = saved_5bc_frame
        f.dlc = 8;
      }
      else {  // MY_LEAF_AZE0 — L650-733
        if ((f.data[5] & 0x10) == 0x00)
          GIDS = (uint16_t)((f.data[0] << 2) | ((f.data[1] & 0xC0) >> 6));
        f.data[0] = (uint8_t)(GIDS >> 2);
        f.data[1] = (GIDS << 6) & 0xC0;
        main_battery_temp = tempFromByte(f.data[3]);
        // Charge-timer estimate (L665-731) — upstream comment marks it WIP.
        cmr_idx = ((f.data[5] & 0x03) << 3) | ((f.data[6] & 0xE0) >> 5);
        uint16_t t;
        switch (cmr_idx) {
          case 5:  t = time_100_with_66kW_in_minutes - ((time_100_with_66kW_in_minutes * battery_soc) / 100);
                   f.data[6] = (f.data[6] & 0xE0) | (t >> 8); f.data[7] = (t & 0xFF); break;
          case 8:  t = time_100_with_200V_in_minutes - ((time_100_with_200V_in_minutes * battery_soc) / 100);
                   f.data[6] = (f.data[6] & 0xE0) | (t >> 8); f.data[7] = (t & 0xFF); break;
          case 11: t = time_100_with_100V_in_minutes - ((time_100_with_100V_in_minutes * battery_soc) / 100);
                   f.data[6] = (f.data[6] & 0xE0) | (t >> 8); f.data[7] = (t & 0xFF); break;
          case 18: t = (battery_soc < 80) ? (time_80_with_66kW_in_minutes -
                       ((time_80_with_66kW_in_minutes * (battery_soc + 20)) / 100)) : 0;
                   f.data[6] = (f.data[6] & 0xE0) | (t >> 8); f.data[7] = (t & 0xFF); break;
          case 21: t = (battery_soc < 80) ? (time_80_with_200V_in_minutes -
                       ((time_80_with_200V_in_minutes * (battery_soc + 20)) / 100)) : 0;
                   f.data[6] = (f.data[6] & 0xE0) | (t >> 8); f.data[7] = (t & 0xFF); break;
          case 24: t = (battery_soc < 80) ? (time_80_with_100V_in_minutes -
                       ((time_80_with_100V_in_minutes * (battery_soc + 20)) / 100)) : 0;
                   f.data[6] = (f.data[6] & 0xE0) | (t >> 8); f.data[7] = (t & 0xFF); break;
          default: break;  // 0 (QC) and 31 (unavailable): no change
        }
      }
      break;
    }

    case 0x5C0:  // L737-761 : inject 0x5EC + fake temperature history
      inject(f5EC);
      swap_5c0_max.LB_HIS_TEMP = main_battery_temp; swap_5c0_max.LB_HIS_TEMP_WUP = main_battery_temp;
      swap_5c0_avg.LB_HIS_TEMP = main_battery_temp; swap_5c0_avg.LB_HIS_TEMP_WUP = main_battery_temp;
      swap_5c0_min.LB_HIS_TEMP = main_battery_temp; swap_5c0_min.LB_HIS_TEMP_WUP = main_battery_temp;
      if      (swap_5c0_idx == 0) leaf5c0_pack(swap_5c0_max, f.data);
      else if (swap_5c0_idx == 1) leaf5c0_pack(swap_5c0_avg, f.data);
      else                        leaf5c0_pack(swap_5c0_min, f.data);
      swap_5c0_idx = (swap_5c0_idx + 1) % 3;
      break;

    case 0x1F2:  // L762-786
      charging_state = f.data[2];
      max_charge_80_requested = (f.data[0] & 0x80) >> 7;
      if (My_Leaf == MY_LEAF_ZE0) {
        seconds_without_1f2 = 0;
        if (seen_1da && charging_state == CHARGING_IDLE) { f.data[2] = 0; seen_1da--; }
        if (My_Battery == MY_BATTERY_40 || My_Battery == MY_BATTERY_62) {
          f.data[3] = 0xA0;
          calc_sum2(f);
        }
      }
      break;

    case 0x59E:  // L788-801 : AZE0 QC capacity rescale
      if (My_Leaf == MY_LEAF_AZE0) {
        f.data[2] = 0x0E;
        f.data[3] = 0x60;
        uint16_t t = (230 * battery_soc) / 100;
        f.data[3] = (f.data[3] & 0xF0) | ((t >> 5) & 0xF);
        f.data[4] = (uint8_t)((t & 0x1F) << 3) | (f.data[4] & 0x07);
        calc_crc8(f);
      }
      break;

    case 0x68C:
    case 0x603:  // L804-810 : VCM startup
      leaf_reset_state();
      inject(f605);
      inject(f607);
      break;

    default: break;
  }

  // ── Block list (L816-846): drop from forwarding ──────────────────────────
  switch (f.id) {
    case 0x633:                                     // new 40kWh msg
    case 0x625: case 0x355: case 0x5EC:
    case 0x5C5: case 0x3B8:                          // our own generated copies
      return false;
    case 0x59E:
      if (My_Leaf == MY_LEAF_ZE0) return false;
      break;
    default: break;
  }
  return true;  // forward (possibly modified) to the other bus
}

// ── Detection readout (UI only) ──────────────────────────────────────────────
const char *leaf_detected_vehicle(bool *confirmed) {
  if (confirmed) *confirmed = My_Leaf_confirmed;
  return (My_Leaf == MY_LEAF_ZE0) ? "ZE0 (2011-12)" : "AZE0 (2013-17)";
}

const char *leaf_detected_battery(bool *confirmed) {
  if (confirmed) *confirmed = My_Battery_confirmed;
  switch (My_Battery) {
    case MY_BATTERY_30: return "30 kWh";
    case MY_BATTERY_40: return "40 kWh";
    case MY_BATTERY_62: return "62 kWh";
    default:            return "24 kWh";
  }
}
