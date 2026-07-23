#include "telemetry.h"
#include <Arduino.h>
#include <math.h>

Telemetry g_telemetry = {};
FrameMonEntry g_frame_mon[2][FRAME_MON_SLOTS] = {};

void telemetry_begin() {
  g_telemetry.battery_bus     = -1;   // unknown until 0x55B is seen
  g_telemetry.usable_soc_pct  = -1;   // absent on ZE0
  g_telemetry.gids            = -1;   // not yet seen / muxed-out
  g_telemetry.vcm_awake       = -1;   // unknown until 0x50B is seen
}

// ── DBC bit extraction ──────────────────────────────────────────────────────
// Motorola/big-endian (@0): start_bit is the MSB of the signal, in the classic
// Vector DBC zig-zag numbering (byte0 bits 7..0, byte1 bits 15..8, ...).
static uint32_t get_be(const uint8_t *d, int start_bit, int len) {
  uint32_t value = 0;
  int bitnum = start_bit;
  for (int i = 0; i < len; i++) {
    int byte_idx = bitnum / 8;
    int bit_idx  = bitnum % 8;
    uint32_t bit = (d[byte_idx] >> bit_idx) & 1;
    value = (value << 1) | bit;
    if (bit_idx == 0) bitnum = byte_idx * 8 + 15;  // jump to next byte's MSB
    else               bitnum--;
  }
  return value;
}

// Intel/little-endian (@1): start_bit is the LSB of the signal, bit numbers
// increase continuously across bytes.
static uint32_t get_le(const uint8_t *d, int start_bit, int len) {
  uint32_t value = 0;
  for (int i = 0; i < len; i++) {
    int bitnum = start_bit + i;
    int byte_idx = bitnum / 8;
    int bit_idx  = bitnum % 8;
    uint32_t bit = (d[byte_idx] >> bit_idx) & 1;
    value |= bit << i;
  }
  return value;
}

// Sign-extend a `len`-bit unsigned value.
static int32_t sign_extend(uint32_t v, int len) {
  uint32_t mask = 1u << (len - 1);
  return (int32_t)((v ^ mask) - mask);
}

// ── Frame monitor ────────────────────────────────────────────────────────────
static void mon_update(BridgeBus bus, const BridgeFrame &f, uint32_t now) {
  FrameMonEntry *tbl = g_frame_mon[bus];
  int free_slot = -1;
  int slot = -1;
  for (int i = 0; i < FRAME_MON_SLOTS; i++) {
    if (tbl[i].used && tbl[i].id == f.id) { slot = i; break; }
    if (!tbl[i].used && free_slot < 0) free_slot = i;
  }
  if (slot < 0) {
    if (free_slot < 0) return;  // table full — drop new IDs
    slot = free_slot;
    tbl[slot].id = f.id;
    tbl[slot].count = 0;
    tbl[slot].interval_ms = 0;
    tbl[slot].used = 1;
  }
  FrameMonEntry &e = tbl[slot];
  uint32_t dt = (e.count > 0) ? (now - e.last_seen_ms) : 0;
  if (dt > 0) e.interval_ms = dt;  // simple last-sample estimate, no smoothing
  e.last_seen_ms = now;
  e.count++;
  e.dlc = f.dlc;
  uint32_t lo = 0, hi = 0;
  for (int i = 0; i < 4 && i < f.dlc; i++) lo |= ((uint32_t)f.data[i]) << (8 * i);
  for (int i = 4; i < 8 && i < f.dlc; i++) hi |= ((uint32_t)f.data[i]) << (8 * (i - 4));
  e.data_lo = lo;
  e.data_hi = hi;
}

// ── Derived car state ────────────────────────────────────────────────────────
static void update_car_state(uint32_t now) {
  const bool vehicle_stale = (g_telemetry.t_vehicle_ms == 0) ||
                             (now - g_telemetry.t_vehicle_ms > 5000);
  if (g_telemetry.vcm_awake == 0 || vehicle_stale) {
    g_telemetry.car_state = STATE_IDLE;  // "asleep" collapses to idle in the enum;
    return;                              // webui renders ASLEEP from vcm_awake/staleness directly
  }
  if (g_telemetry.charge_power_kw > 0.2f || g_telemetry.pack_current_a < -2.0f) {
    g_telemetry.car_state = STATE_CHARGING;
  } else if (fabsf(g_telemetry.speed_kmh) > 1.0f || fabsf(g_telemetry.pack_current_a) > 3.0f) {
    g_telemetry.car_state = STATE_DRIVING;
  } else {
    g_telemetry.car_state = STATE_IDLE;
  }
}

void telemetry_capture(BridgeBus from, const BridgeFrame &f) {
  const uint32_t now = millis();
  mon_update(from, f, now);
  g_telemetry.last_rx_ms = now;

  switch (f.id) {

    case 0x55B: {  // battery, 100 ms — identifies the battery bus + coarse SOC
      g_telemetry.battery_bus = (int32_t)from;
      g_telemetry.last_battery_frame_ms = now;
      if (!g_telemetry.first_battery_frame_ms) g_telemetry.first_battery_frame_ms = now;
      g_telemetry.soc_tenth_pct = (int32_t)get_be(f.data, 7, 10);
      g_telemetry.t_55b_ms = now;
      break;
    }

    case 0x1DB: {  // battery, 10 ms — pack voltage/current/SOC/relay bits
      g_telemetry.last_battery_frame_ms = now;
      uint32_t v_raw = get_be(f.data, 23, 10);
      g_telemetry.pack_voltage_v = (float)v_raw * 0.5f;
      int32_t i_raw = sign_extend(get_be(f.data, 7, 11), 11);
      g_telemetry.pack_current_a = (float)i_raw * 0.5f;
      uint32_t soc_raw = get_le(f.data, 32, 7);
      g_telemetry.usable_soc_pct = (soc_raw > 100) ? -1 : (int32_t)soc_raw;
      g_telemetry.lb_failsafe_status    = (int32_t)get_be(f.data, 8, 3);
      g_telemetry.lb_relay_cut_request  = (int32_t)get_be(f.data, 11, 2);
      g_telemetry.lb_main_relay_on      = (int32_t)get_le(f.data, 29, 1);
      g_telemetry.pack_power_kw = g_telemetry.pack_voltage_v * g_telemetry.pack_current_a / 1000.0f;
      g_telemetry.t_1db_ms = now;
      break;
    }

    case 0x5BC: {  // battery, 100-500 ms — GIDs (muxed) / SOH
      g_telemetry.last_battery_frame_ms = now;
      uint32_t mux = get_le(f.data, 32, 1);  // LB_Remain_Cap_Segment_Switch
      if (mux == 0) {  // 0 = remaining GIDs (per spec; capture only in this state)
        g_telemetry.gids = (int32_t)get_be(f.data, 7, 10);
      }
      g_telemetry.soh_pct = (int32_t)get_le(f.data, 33, 7);
      g_telemetry.t_5bc_ms = now;
      break;
    }

    case 0x59E: {  // battery, AZE0/ZE1 only — QC capacities
      g_telemetry.last_battery_frame_ms = now;
      g_telemetry.full_cap_qc_wh   = (int32_t)get_be(f.data, 20, 9) * 100;
      g_telemetry.remain_cap_qc_wh = (int32_t)get_be(f.data, 27, 9) * 100;
      g_telemetry.t_59e_ms = now;
      break;
    }

    case 0x5C0: {  // battery, 500 ms, muxed MAX/AVG/MIN (per leaf_5c0.h: 1=MAX,2=AVG,3=MIN)
      g_telemetry.last_battery_frame_ms = now;
      uint32_t mux = get_le(f.data, 6, 2);
      // Raw temp is offset -40; 1 degC/bit on ZE0/ZE1, 0.5 degC/bit on AZE0 —
      // stored here using the 1 degC/bit convention (best-effort; see header note).
      int32_t temp_c = (int32_t)get_le(f.data, 17, 7) - 40;
      if      (mux == 1) g_telemetry.temp_max_c = temp_c;
      else if (mux == 2) g_telemetry.temp_avg_c = temp_c;
      else if (mux == 3) g_telemetry.temp_min_c = temp_c;
      g_telemetry.battery_dtc = (int32_t)get_le(f.data, 56, 8);
      g_telemetry.t_5c0_ms = now;
      break;
    }

    case 0x1DC: {  // battery, 10 ms — charge/discharge power limits
      g_telemetry.last_battery_frame_ms = now;
      g_telemetry.max_discharge_kw = (float)get_be(f.data, 7, 10) * 0.25f;
      g_telemetry.max_charge_kw    = (float)get_be(f.data, 13, 10) * 0.25f;
      g_telemetry.t_1dc_ms = now;
      break;
    }

    case 0x1D4: {  // vehicle, 10 ms — commanded torque
      int32_t raw = sign_extend(get_be(f.data, 23, 12), 12);
      g_telemetry.torque_nm = (float)raw * 0.25f;
      g_telemetry.t_vehicle_ms = now;
      break;
    }

    case 0x1DA: {  // vehicle, 10 ms — inverter input voltage (AZE0/ZE1)
      g_telemetry.inverter_voltage_v = (float)f.data[0] * 2.0f;
      g_telemetry.t_vehicle_ms = now;
      break;
    }

    case 0x11A: {  // vehicle, 10 ms — gear + ECO
      g_telemetry.gear    = (f.data[0] >> 4) & 0x0F;
      g_telemetry.eco_on  = (f.data[1] >> 4) & 0x01;
      g_telemetry.t_vehicle_ms = now;
      break;
    }

    case 0x284: {  // vehicle, 20 ms — wheel-speed-derived vehicle speed (approx)
      if (f.dlc >= 6) {
        uint32_t raw = ((uint32_t)f.data[4] << 8) | f.data[5];
        g_telemetry.speed_kmh = (float)raw / 100.0f;
      }
      g_telemetry.t_vehicle_ms = now;
      break;
    }

    case 0x1F2: {  // vehicle, 10 ms — charge power / target SOC flag
      uint32_t raw = get_be(f.data, 1, 10);
      g_telemetry.charge_power_kw = (float)raw * 0.1f - 10.0f;
      g_telemetry.target_soc_80   = (int32_t)get_be(f.data, 7, 1);
      g_telemetry.t_vehicle_ms = now;
      break;
    }

    case 0x50B: {  // vehicle, 100 ms — VCM wake/sleep
      uint32_t raw = get_le(f.data, 30, 2);
      g_telemetry.vcm_awake = (raw == 3) ? 1 : (raw == 0) ? 0 : -1;
      g_telemetry.t_vehicle_ms = now;
      break;
    }

    default:
      break;
  }

  update_car_state(now);
}

bool car_write_safe() {
  const uint32_t now = millis();
  const uint32_t last_rx = g_telemetry.last_rx_ms;
  const uint32_t age = last_rx ? (now - last_rx) : UINT32_MAX;
  if (age > 3000) return true;  // no live CAN at all = bench/bringup, safe to allow

  // CAN is live. "Stationary/parked" is judged from vehicle-side fields
  // (gear/speed/car_state). If those frames have gone stale while the battery
  // bus keeps last_rx fresh, we CANNOT confirm the car is parked — refuse to
  // write (lock) rather than trust frozen gear=P/speed=0 defaults. This closes
  // the "battery live, vehicle link dropped mid-drive" hole.
  const uint32_t t_veh = g_telemetry.t_vehicle_ms;
  const uint32_t veh_age = t_veh ? (now - t_veh) : UINT32_MAX;
  if (veh_age > 3000) return false;  // no fresh vehicle data -> can't confirm parked

  return (g_telemetry.car_state != STATE_DRIVING) &&
         (g_telemetry.speed_kmh < 2.0f) &&
         (g_telemetry.gear == 0 /* P */);
}
