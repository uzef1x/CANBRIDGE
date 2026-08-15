// ─────────────────────────────────────────────────────────────────────────────
// Read-only telemetry tap for the web dashboard. telemetry_capture() is called
// from the bridge pump for every received frame, BEFORE translate() — it must
// stay cheap (id switch + bit extraction only) since it runs on the safety
// -critical forwarding path.
//
// Concurrency: g_telemetry / g_frame_mon are WRITTEN only by the dedicated
// CAN-pump task (telemetry_capture(), via bridge.cpp's pump()). Two kinds of
// reader exist:
//   1. The AsyncTCP (web) task reads only SCALAR fields — via car_write_safe(),
//      can_tx_safe(), and webui.cpp's resolve_bus(). Every scalar here is a
//      plain 32-bit type (int32_t/uint32_t/float) written with a single store;
//      aligned 32-bit read/write is atomic on Xtensa, so these cross-task
//      scalar reads cannot tear (as long as no field grows past 32 bits or is
//      written non-atomically).
//   2. The multi-word g_frame_mon rows (memcpy'd, not single-store) are read
//      by build_snapshot(), called from webui_broadcast() on the Arduino loop
//      task — a DIFFERENT task from the CAN-pump task that writes them (moved
//      there deliberately so an lwip/WS stall can't trip the CAN task's panic
//      watchdog; see webui.h). This IS a cross-task read of un-locked
//      multi-word state: a torn row (e.g. id/data updated but not yet its
//      timestamp) is possible. Accepted trade-off — this is display-only data
//      (frame monitor / cell voltages), never consulted by car_write_safe()/
//      can_tx_safe() (which read only the single-word scalars above), so the
//      worst case is a rare glitchy dashboard row, not a safety issue.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <stdint.h>
#include "can_frame.h"

// ── Decoded telemetry snapshot ──────────────────────────────────────────────
struct Telemetry {
  // battery bus identity
  int32_t battery_bus;          // -1 = unknown, else BUS_A/BUS_B
  uint32_t last_battery_frame_ms;
  uint32_t first_battery_frame_ms;  // 0 until the battery is first heard
  uint32_t last_rx_ms;          // any frame, either bus — 0 until the first one

  // 0x1DB (battery, 10 ms)
  float    pack_voltage_v;
  float    pack_current_a;      // + = discharge per DBC label, but wire-observed on this car
                                 // (2026-08-15) to actually be + = charging (into battery) —
                                 // see update_car_state() for detail; DBC label is wrong for this car
  int32_t  usable_soc_pct;      // -1 = invalid/absent (ZE0)
  int32_t  lb_failsafe_status;
  int32_t  lb_relay_cut_request;
  int32_t  lb_main_relay_on;
  float    pack_power_kw;
  uint32_t t_1db_ms;

  // 0x55B (battery, 100 ms)
  int32_t  soc_tenth_pct;       // 0-1000
  uint32_t t_55b_ms;

  // 0x5BC (battery, 100-500 ms)
  int32_t  gids;                // -1 = not yet seen / muxed-out
  int32_t  soh_pct;
  uint32_t t_5bc_ms;

  // 0x59E (battery, AZE0/ZE1 only)
  int32_t  full_cap_qc_wh;
  int32_t  remain_cap_qc_wh;
  uint32_t t_59e_ms;

  // 0x5C0 (battery, muxed MAX/AVG/MIN per leaf_5c0.h: 1=MAX,2=AVG,3=MIN)
  int32_t  temp_max_c;
  int32_t  temp_avg_c;
  int32_t  temp_min_c;
  int32_t  battery_dtc;
  uint32_t t_5c0_ms;

  // 0x1DC (battery, 10 ms)
  float    max_discharge_kw;
  float    max_charge_kw;
  uint32_t t_1dc_ms;

  // vehicle side
  float    torque_nm;                 // 0x1D4
  // 0x1DA (vehicle, 10 ms, inverter input voltage + motor/inverter status)
  float    inverter_voltage_v;        // Inverter input voltage, raw byte0 x2
  int32_t  motor_rpm;                 // MG_OutputRevolution 39|15@0+ (signed despite "+")
  float    motor_torque_nm;           // MG_EffectiveTorque 18|11@0+ (signed despite "+")
  int32_t  mg_error;                  // MG_ErrorCodes 50|6@1+; -1 = never seen
  int32_t  gear;                      // 0x11A  0=P,2=R,3=N,4=D/B
  int32_t  eco_on;                    // 0x11A
  float    speed_kmh;                 // 0x284 (approx)
  float    charge_power_kw;           // 0x1F2
  int32_t  target_soc_80;             // 0x1F2
  int32_t  vcm_awake;                 // 0x50B  1=awake,0=asleep,-1=unknown
  uint32_t t_vehicle_ms;              // last of any vehicle-side frame above

  // 0x55A (vehicle, 10 ms, sender INVmc — motor/inverter temperature)
  float    motor_temp_c;        // byte1, degF->degC; see telemetry.cpp for provenance
  float    inverter_temp_c;     // byte2, degF->degC; see telemetry.cpp for provenance
  uint32_t t_55a_ms;            // 0 = never seen; absence is judged from this, NOT from
                                 // the float values (winter temps can legitimately be < 0)

  // 0x380 (vehicle, ZE0-era OBC, 10 ms)
  float    obc_power_kw_ze0;    // Charger_Output_Power 16|9@0+ x0.1 -> kW
  float    ac_voltage_v;        // AC_Voltage 42|9@0+ x0.5+70 -> V; raw 0 = no AC measurement -> -1
  int32_t  ac_relay;            // Normal_Charger_Relay_Status_Flag 38|1@1+; -1 = never seen, else 0/1
  int32_t  qc_relay;            // Quick_Charger_Relay_Status_Flag 37|1@1+; -1 = never seen, else 0/1
  uint32_t t_380_ms;

  // 0x5BF (vehicle, ZE0-era OBC, 10 ms)
  float    evse_limit_a;        // J1772CurrentLimiter 16|8@1+ x0.5 -> A; raw 0 = no EVSE -> -1
  float    qc_voltage_v;        // QC_Voltage 24|8@1+ x1+257 -> V; always the latest raw decode,
                                 // meaningless unless qc_relay==1 — no absent sentinel here
  uint32_t t_5bf_ms;

  // 0x390 (vehicle, AZE0-era OBC)
  float    obc_power_kw_aze0;   // OBC_Charge_Power 0|9@0+ x0.1 -> kW
  int32_t  obc_charge_status;   // OBC_Charge_Status 46|6@0+ raw enum; label map lives in webui_page.h; -1 = not yet seen
  uint32_t t_390_ms;            // ac_relay/qc_relay above are also updated from this frame (AZE0-era)

  // derived
  int32_t  car_state;                 // CarState enum below
};

enum CarState { STATE_IDLE = 0, STATE_DRIVING = 1, STATE_CHARGING = 2, STATE_DISCHARGING = 3 };

extern Telemetry g_telemetry;

// ── Frame monitor ───────────────────────────────────────────────────────────
struct FrameMonEntry {
  uint32_t id;
  uint32_t count;
  uint32_t last_seen_ms;
  uint32_t interval_ms;   // estimated period
  uint32_t dlc;
  uint32_t data_lo;       // data[0..3] little-endian packed
  uint32_t data_hi;       // data[4..7] little-endian packed
  uint32_t used;          // 0 = free slot
};

#define FRAME_MON_SLOTS 64
extern FrameMonEntry g_frame_mon[2][FRAME_MON_SLOTS];  // [BUS_A/BUS_B][slot]

// Count of distinct CAN IDs seen (per bus, on top of the FRAME_MON_SLOTS
// already tracked) that could NOT be given a slot because the table was full
// — i.e. IDs silently dropped from the frame monitor. 0 when nothing has been
// dropped. Same lock-free style as the rest of this file: a plain 32-bit
// counter, written only by the CAN-pump task, read cross-task as a scalar.
extern uint32_t g_frame_mon_dropped[2];  // [BUS_A/BUS_B]

// One-time sentinel init (-1 = "not yet known" fields). Call once from
// bridge_begin(), before any frames are pumped.
void telemetry_begin();

// Called from bridge pump for every received frame, before translate().
void telemetry_capture(BridgeBus from, const BridgeFrame &f);

// Safety interlock: true iff it is currently safe to write to the vehicle
// bus (custom TX, reboot, settings changes). Callable from any task — reads
// only plain scalar fields (see concurrency note above). If CAN has gone
// quiet (age > 3000 ms) we assume bench/bringup and allow; otherwise the
// car must be confirmed stationary and in Park.
bool car_write_safe();

// Stricter interlock for the ONLY path that actually writes CAN frames to the
// vehicle bus (the dashboard's custom-CAN-transmit command). Unlike
// car_write_safe(), a silent/absent CAN bus does NOT unlock this one — with
// no live traffic we cannot confirm the car is parked, so we must refuse.
// Returns true only on POSITIVE confirmation: fresh vehicle-side data
// (t_vehicle_ms within 3000 ms) AND car_state != STATE_DRIVING AND
// speed_kmh < 2.0 AND gear == P. Callable from any task — reads only plain
// scalar fields (see concurrency note above).
bool can_tx_safe();
