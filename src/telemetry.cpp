#include "telemetry.h"
#include <Arduino.h>
#include <math.h>

Telemetry g_telemetry = {};
FrameMonEntry g_frame_mon[2][FRAME_MON_SLOTS] = {};
uint32_t g_frame_mon_dropped[2] = {};

// Side table remembering which dropped IDs have already been counted, so
// g_frame_mon_dropped[] counts DISTINCT dropped IDs rather than every frame
// that arrives once the table is full. Bounded (not a hash set) — cheap and
// enough in practice: real buses have a small, fixed set of distinct IDs, so
// overflowing FRAME_MON_SLOTS(64) *and* this table implies a genuinely
// pathological bus. Beyond capacity we stop deduping (an already-rare edge
// case) rather than pay for unbounded tracking; the counter still only ever
// undercounts, never over.
#define DROPPED_ID_TRACK_SLOTS 32
static uint32_t g_dropped_id_seen[2][DROPPED_ID_TRACK_SLOTS];
static uint32_t g_dropped_id_seen_count[2];

void telemetry_begin() {
  g_telemetry.battery_bus     = -1;   // unknown until 0x55B is seen
  g_telemetry.usable_soc_pct  = -1;   // absent on ZE0
  g_telemetry.gids            = -1;   // not yet seen / muxed-out
  g_telemetry.vcm_awake       = -1;   // unknown until 0x50B is seen
  g_telemetry.ac_voltage_v     = -1;  // no AC measurement until 0x380 seen
  g_telemetry.evse_limit_a     = -1;  // no EVSE until 0x5BF seen
  g_telemetry.obc_charge_status = -1; // not yet seen
  g_telemetry.ac_relay         = -1;  // never seen
  g_telemetry.qc_relay         = -1;  // never seen
  g_telemetry.mg_error         = -1;  // never seen
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
    if (free_slot < 0) {
      // Table full — this ID has no slot. Count it once per distinct ID.
      uint32_t *seen = g_dropped_id_seen[bus];
      uint32_t seen_count = g_dropped_id_seen_count[bus];
      for (uint32_t i = 0; i < seen_count; i++) {
        if (seen[i] == f.id) return;  // already counted
      }
      if (seen_count < DROPPED_ID_TRACK_SLOTS) {
        seen[seen_count] = f.id;
        g_dropped_id_seen_count[bus] = seen_count + 1;
      }
      g_frame_mon_dropped[bus]++;
      return;
    }
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
  // Debounce state for the non-driving branch only (Idle/Charging/Discharging).
  // DRIVING always adopts immediately in every path below — can_tx_safe()/
  // car_write_safe() gate on car_state != STATE_DRIVING, so that transition
  // must never lag by even one cycle. Both early-return paths below also
  // reset `pending`/`pending_since` to the state they just wrote, so a stale
  // debounce candidate can never survive a gap and get adopted instantly the
  // moment that gate lifts.
  static int32_t  pending       = STATE_IDLE;
  static uint32_t pending_since = 0;

  const bool vehicle_stale = (g_telemetry.t_vehicle_ms == 0) ||
                             (now - g_telemetry.t_vehicle_ms > 5000);
  if (g_telemetry.vcm_awake == 0 || vehicle_stale) {
    g_telemetry.car_state = STATE_IDLE;  // asleep/stale collapses to idle, immediate — unchanged from before
    pending = STATE_IDLE; pending_since = now;
    return;
  }

  // DRIVING: still adopted immediately, now checked before the current-sign
  // classification — which forces one change from the old condition: the
  // current-based fallback must be discharge-direction only (< -3 A), because
  // |current| > 3 would classify AC charging (+4.5 A observed) as DRIVING now
  // that this branch runs first. Driving discharges; the sign is known
  // (owner-confirmed, see below). Corner case knowingly accepted: regen with a
  // dead speed signal reads as charging, not driving — regen implies motion,
  // motion implies 0x284 speed frames, and can_tx_safe()'s gear==P clause
  // still gates independently.
  // The current fallback is additionally gated on gear != P (live-found
  // 2026-08-15): a parked-but-READY car runs its HV accessories (A/C, heater,
  // DC-DC) off the pack, and that drain alone exceeded 3 A — which read as
  // DRIVING while stationary in P with the handbrake on, wrongly locking every
  // settings control. Driving in P is impossible, so gear != P keeps the
  // dead-speed backstop for real driving without the accessory false trigger.
  if (fabsf(g_telemetry.speed_kmh) > 1.0f ||
      (g_telemetry.pack_current_a < -3.0f && g_telemetry.gear > 1 /* not P, see gear in telemetry.h */)) {
    g_telemetry.car_state = STATE_DRIVING;
    pending = STATE_DRIVING; pending_since = now;
    return;
  }

  // Not driving: classify from pack-current sign. Owner-confirmed 2026-08-15
  // against wire data (+4.5 A while charging, -0.5 A parked): on this car
  // POSITIVE pack current flows INTO the battery (charging) — the DBC's
  // "+=discharge" label for this signal is wrong for this car. +/-1.0 A
  // deadband absorbs idle noise (measured ~0.5 A) so a parked car doesn't
  // flap between Idle and Discharging.
  int32_t candidate;
  if (g_telemetry.pack_current_a > 1.0f)       candidate = STATE_CHARGING;
  else if (g_telemetry.pack_current_a < -1.0f) candidate = STATE_DISCHARGING;
  else                                          candidate = STATE_IDLE;

  // 3 s debounce across all of Idle/Charging/Discharging: commanded charge
  // power and pack current both hover near their thresholds during charge
  // tapering/wait phases, which flapped the dashboard state chip (user
  // report 2026-08-15). DRIVING above is exempt — see comment at the top.
  if (candidate != pending) { pending = candidate; pending_since = now; }
  if (now - pending_since >= 3000) {
    g_telemetry.car_state = pending;
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
      // Both @1+ (little-endian) in the DBC — were wrongly read big-endian
      // until 2026-08-15, which blended in the two MSBs of LB_Total_Voltage
      // and made the FS tile show a constant 2 (3 above 384 V). User-caught.
      g_telemetry.lb_failsafe_status    = (int32_t)get_le(f.data, 8, 3);
      g_telemetry.lb_relay_cut_request  = (int32_t)get_le(f.data, 11, 2);
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
      // DBC: EV-can_ZE0.dbc, message 0x1DA (474, sender INVmc).
      // MG_OutputRevolution 39|15@0+ (1,0), range [-16382|16382] — the "+"
      // marks it unsigned in the DBC, but the signed range shows it's
      // actually signed; sign-extend it like the rest of this file does.
      g_telemetry.motor_rpm = sign_extend(get_be(f.data, 39, 15), 15);
      // MG_EffectiveTorque 18|11@0+ (0.5,0), range [-300|300] — same signed
      // treatment as MG_OutputRevolution above.
      g_telemetry.motor_torque_nm = (float)sign_extend(get_be(f.data, 18, 11), 11) * 0.5f;
      // MG_ErrorCodes 50|6@1+ — raw enum, no DBC value table.
      g_telemetry.mg_error = (int32_t)get_le(f.data, 50, 6);
      // Wire sanity check: frame C2 00 18 00 00 01 03 27, captured at
      // standstill on the real car, decodes to rpm=0, torque=0, errors=0
      // with the bit extraction above — hand-verified. Weak check: at
      // standstill all three fields are genuinely zero, so this confirms
      // bit alignment doesn't pull stray bits into the window, but does NOT
      // validate scaling or sign handling. Those remain unverified against
      // live (non-zero) traffic.
      g_telemetry.t_vehicle_ms = now;
      break;
    }

    case 0x55A: {  // vehicle, 10 ms, sender INVmc — motor/inverter temperature
      // The ZE0 DBC does define this frame, but its interpretation (x0.5 degC
      // scale, byte4 = motor) failed wire validation — see below. Formula used
      // instead is the OVMS project's vehicle_nissanleaf.cpp, case 0x55a: raw
      // bytes are degrees Fahrenheit, converted 5.0/9.0*(d[1]-32) and
      // 5.0/9.0*(d[2]-32) — independently verified against that source.
      g_telemetry.motor_temp_c    = ((float)f.data[1] - 32.0f) * 5.0f / 9.0f;
      g_telemetry.inverter_temp_c = ((float)f.data[2] - 32.0f) * 5.0f / 9.0f;
      // Wire-validated 2026-08-10 on the real car: cold-start after a 17
      // degC night read 18.3/15.6 degC (the DBC's x0.5 degC scale would have
      // implied 30+ degC — refuted); after a short drive byte1 warmed
      // +3.3 degC and held while byte2 stayed near-static, confirming
      // byte1=motor, byte2=inverter electronics.
      //
      // The DBC's 0x55A signal map is WRONG for this car: its
      // "MotorTemperature" byte4 field is a constant (0x5F under all
      // conditions) on this car and is deliberately NOT decoded here.
      g_telemetry.t_55a_ms = now;
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

    case 0x380: {  // vehicle, 10 ms — ZE0-era OBC charge power / AC voltage
                    // DBC: EV-can_ZE0.dbc, message 0x380
      g_telemetry.obc_power_kw_ze0 = (float)get_be(f.data, 16, 9) * 0.1f;  // Charger_Output_Power 16|9@0+
      uint32_t ac_raw = get_be(f.data, 42, 9);  // AC_Voltage 42|9@0+
      g_telemetry.ac_voltage_v = (ac_raw == 0) ? -1.0f : ((float)ac_raw * 0.5f + 70.0f);
      // AC/QC relay status, same 0x380 frame. EV-can_ZE0.dbc message 0x380
      // (message number 896): Normal_Charger_Relay_Status_Flag 38|1@1+,
      // Quick_Charger_Relay_Status_Flag 37|1@1+ — both signals confirmed
      // present at these bit positions in the DBC. Wire-validated on a real
      // ZE0 car during AC charging: byte4 == 0x50 -> AC relay=1, QC relay=0.
      g_telemetry.ac_relay = (int32_t)get_le(f.data, 38, 1);  // Normal_Charger_Relay_Status_Flag
      g_telemetry.qc_relay = (int32_t)get_le(f.data, 37, 1);  // Quick_Charger_Relay_Status_Flag
      g_telemetry.t_380_ms = now;
      g_telemetry.t_vehicle_ms = now;
      break;
    }

    case 0x5BF: {  // vehicle, 10 ms — ZE0-era OBC EVSE (J1772) current limit
                    // DBC: EV-can_ZE0.dbc, message 0x5BF, signal J1772CurrentLimiter 16|8@1+
      uint32_t lim_raw = get_le(f.data, 16, 8);
      g_telemetry.evse_limit_a = (lim_raw == 0) ? -1.0f : ((float)lim_raw * 0.5f);
      // QC_Voltage 24|8@1+, DBC: EV-can_ZE0.dbc, message 0x5BF. The DBC's own
      // comment on this signal reads "offset unknown, set to +257 for now" —
      // treat the +257 offset as tentative. Stored unconditionally every
      // 0x5BF frame (no absent sentinel — raw value is always "valid data",
      // just meaningless unless QC is actually active). NOT yet wire-
      // validated: no DC/QC charging session has been observed on the bench car.
      g_telemetry.qc_voltage_v = (float)get_le(f.data, 24, 8) + 257.0f;
      g_telemetry.t_5bf_ms = now;
      g_telemetry.t_vehicle_ms = now;
      break;
    }

    case 0x390: {  // vehicle — AZE0-era OBC charge power / status
                    // DBC: EV-can_AZE0.dbc, message 0x390
      g_telemetry.obc_power_kw_aze0 = (float)get_be(f.data, 0, 9) * 0.1f;    // OBC_Charge_Power 0|9@0+
      g_telemetry.obc_charge_status = (int32_t)get_be(f.data, 46, 6);       // OBC_Charge_Status 46|6@0+
      // QC relay: EV-can_AZE0.dbc, message 0x390 (message number 912),
      // signal OBC_Flag_QC_Relay_On_Announcemen 38|1@1+.
      g_telemetry.qc_relay = (int32_t)get_le(f.data, 38, 1);
      // AC relay: NOT in the DBC. Per OVMS vehicle_nissanleaf.cpp (not in DBC)
      // — independently verified against a local copy of that source file
      // (lines ~1180-1202: `bool ac_state = (d[3] & 0x20) == 0x20;`), i.e.
      // byte 3 bit 5 = get_le(f.data, 29, 1).
      g_telemetry.ac_relay = (int32_t)get_le(f.data, 29, 1);
      g_telemetry.t_390_ms = now;
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
         (g_telemetry.gear <= 1 /* P: 0=startup, 1=re-engaged — see gear in telemetry.h */);
}

bool can_tx_safe() {
  const uint32_t now = millis();
  // Unlike car_write_safe(), a silent bus is NOT treated as bench/bringup
  // here: this is the interlock for the one path that actually puts frames
  // on the vehicle CAN bus, so "cannot confirm" must mean "refuse", not
  // "allow". A parked car with the ignition off may itself go CAN-silent —
  // that case, too, must refuse (we have no way to distinguish it from a
  // live car whose vehicle-side messages have simply stopped arriving).
  const uint32_t t_veh = g_telemetry.t_vehicle_ms;
  const uint32_t veh_age = t_veh ? (now - t_veh) : UINT32_MAX;
  if (veh_age > 3000) return false;  // no fresh vehicle data -> can't confirm parked

  return (g_telemetry.car_state != STATE_DRIVING) &&
         (g_telemetry.speed_kmh < 2.0f) &&
         (g_telemetry.gear <= 1 /* P: 0=startup, 1=re-engaged — see gear in telemetry.h */);
}
