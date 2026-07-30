// ─────────────────────────────────────────────────────────────────────────────
// See leaf_diag.h for the concurrency contract and source attribution.
//
// Every group-parsing branch below is ported line-for-line from dala's
// Battery-Emulator NISSAN-LEAF-BATTERY.cpp `handle_incoming_can_frame()` case
// 0x7BB (L372-612), with two intentional deviations, both called out inline:
//   1. The group-0x04 four-sensor min search: upstream assigns raw_2 where
//      raw_3 is clearly meant (its L510-512) — fixed here, not ported.
//   2. Defensive bounds checks were added around the group-0x02 cell-index
//      walk (upstream trusts the frame count exactly fills 96 slots; we clamp
//      instead of risking a buffer overrun from a malformed/truncated chain).
// ─────────────────────────────────────────────────────────────────────────────
#include "leaf_diag.h"
#include "telemetry.h"
#include "can_bus.h"
#include <Arduino.h>
#include <string.h>

LeafDiag g_leaf_diag = {};

void leaf_diag_begin() {
  memset(&g_leaf_diag, 0, sizeof(g_leaf_diag));
  for (int i = 0; i < 4; i++) g_leaf_diag.temp_c[i] = -1000;  // sentinel until first group-4 poll
  g_leaf_diag.temp_poll_min_c = -1000;
  g_leaf_diag.temp_poll_max_c = -1000;
  g_leaf_diag.cell_min_idx = -1;
  g_leaf_diag.cell_max_idx = -1;
  // Hold off the first poll for ~20s after boot so the LBC contactor-closing
  // window isn't disturbed by an active-diag request right at power-up.
  g_leaf_diag.paused_until_ms = millis() + 20000;
}

// ── Temp_fromRAW_to_F — ported VERBATIM from Battery-Emulator ──────────────────
// "This function feels horrible, but apparently works well" — dala,
// NISSAN-LEAF-BATTERY.cpp L868. Kept exactly as-is, thresholds and all.
static int32_t Temp_fromRAW_to_F(int32_t temperature) {
  if (temperature == 1021) {
    return 10;
  } else if (temperature == 65535) {  // Value unavailable, sensor does not exist
    return 718;                       // 0*C final calculation
  } else if (temperature >= 589) {
    return (int32_t)(1620 - temperature * 1.81);
  } else if (temperature >= 569) {
    return (int32_t)(572 + (579 - temperature) * 1.80);
  } else if (temperature >= 558) {
    return (int32_t)(608 + (558 - temperature) * 1.6363636363636364);
  } else if (temperature >= 548) {
    return (int32_t)(626 + (548 - temperature) * 1.80);
  } else if (temperature >= 537) {
    return (int32_t)(644 + (537 - temperature) * 1.6363636363636364);
  } else if (temperature >= 447) {
    return (int32_t)(662 + (527 - temperature) * 1.8);
  } else if (temperature >= 438) {
    return (int32_t)(824 + (438 - temperature) * 2);
  } else if (temperature >= 428) {
    return (int32_t)(842 + (428 - temperature) * 1.80);
  } else if (temperature >= 365) {
    return (int32_t)(860 + (419 - temperature) * 2.0);
  } else if (temperature >= 357) {
    return (int32_t)(986 + (357 - temperature) * 2.25);
  } else if (temperature >= 348) {
    return (int32_t)(1004 + (348 - temperature) * 2);
  } else if (temperature >= 316) {
    return (int32_t)(1022 + (340 - temperature) * 2.25);
  }
  return (int32_t)(1094 + (309 - temperature) * 2.5714285714285715);
}

// Raw sensor reading -> degC, tenths-of-degF-from-freezing convention as dala's.
static int32_t raw_to_c(uint16_t raw) {
  return ((Temp_fromRAW_to_F(raw) - 320) * 5) / 9;
}

// ── Group-response parse state (main loop task only) ────────────────────────
static uint8_t  g_group_7bb = 0;

// group 0x02 — cell voltages
static uint16_t g_cell_scratch[96];
static uint16_t g_cell_idx = 0;

// group 0x04 — temperatures
static uint16_t g_temp_raw1 = 718, g_temp_raw2 = 718, g_temp_raw3 = 718, g_temp_raw4 = 718;
static uint8_t  g_temp_raw2_hi = 0;

// group 0x06 — balancing shunts
static bool     g_shunt_scratch[96];

// identity groups — scratch + "seen once" latches (never reset)
static char g_part_scratch[8];
static char g_serial_scratch[16];
static char g_bmsid_scratch[9];
static bool g_part_done = false, g_serial_done = false, g_bmsid_done = false;

// Post-increment-style helpers matching the reference's
// `battery_cell_voltages[battery_request_idx++] = ...` pattern, with added
// bounds clamping (see file banner, deviation 2).
static void cell_put(uint16_t v) {
  if (g_cell_idx < 96) g_cell_scratch[g_cell_idx] = v;
  if (g_cell_idx < 96) g_cell_idx++;
}
static void cell_orput(uint8_t low_byte) {
  if (g_cell_idx < 96) g_cell_scratch[g_cell_idx] |= low_byte;
  if (g_cell_idx < 96) g_cell_idx++;
}
static void cell_puthi(uint8_t hi_byte) {
  // No index advance here — matches the reference: this half-cell is completed
  // by the *next* frame's cell_orput() call.
  if (g_cell_idx < 96) g_cell_scratch[g_cell_idx] = ((uint16_t)hi_byte) << 8;
}

static const uint8_t GROUP_ROTATION[7] = {0x01, 0x02, 0x04, 0x06, 0x83, 0x84, 0x90};
static uint8_t g_group_rot_idx = 6;  // pick_next_group() starts the rotation at 0x01

static bool identity_group_done(uint8_t g) {
  switch (g) {
    case 0x83: return g_part_done;
    case 0x84: return g_serial_done;
    case 0x90: return g_bmsid_done;
    default:   return false;
  }
}

static uint8_t pick_next_group() {
  for (int tries = 0; tries < 7; tries++) {
    g_group_rot_idx = (uint8_t)((g_group_rot_idx + 1) % 7);
    uint8_t g = GROUP_ROTATION[g_group_rot_idx];
    if (!identity_group_done(g)) return g;
  }
  return GROUP_ROTATION[0];  // unreachable: group 0x01 is never "done"
}

// ── 0x7BB parse — mirrors handle_incoming_can_frame() case 0x7BB ────────────
static void parse_7bb(const uint8_t *d, uint32_t now) {
  if (d[0] == 0x10) g_group_7bb = d[3];  // first frame of a new group carries the group echo

  switch (g_group_7bb) {

    case 0x01: {  // insulation (frame 0x23) + Hx (frame 0x24) — L402-417
      if (d[0] == 0x23) {
        g_leaf_diag.insulation_raw = ((uint32_t)d[5] << 8) | d[6];
      }
      if (d[0] == 0x24) {
        g_leaf_diag.hx_pct = ((float)(((uint32_t)d[4] << 8) | d[5])) / 102.4f;
        g_leaf_diag.last_poll_ok_ms = now;
      }
      break;
    }

    case 0x02: {  // 96 cell voltages, mV — L420-461
      if (d[0] == 0x10) {  // first frame is anomalous: 2 full cells
        g_cell_idx = 0;
        cell_put(((uint16_t)d[4] << 8) | d[5]);
        cell_put(((uint16_t)d[6] << 8) | d[7]);
        break;
      }
      if (d[0] == 0x2C && d[6] == 0xFF) {  // last frame — no cell data, finalize
        uint32_t sum = 0, mn = 0xFFFFFFFFu, mx = 0;
        int32_t mni = -1, mxi = -1;
        for (int i = 0; i < 96; i++) {
          uint16_t mv = g_cell_scratch[i];
          sum += mv;
          if (mv < mn) { mn = mv; mni = i; }
          if (mv > mx) { mx = mv; mxi = i; }
        }
        memcpy(g_leaf_diag.cells_mv, g_cell_scratch, sizeof(g_cell_scratch));  // publish atomically-by-generation
        g_leaf_diag.cell_min_mv    = mn;
        g_leaf_diag.cell_max_mv    = mx;
        g_leaf_diag.cell_avg_mv    = sum / 96;
        g_leaf_diag.cell_spread_mv = mx - mn;
        g_leaf_diag.cell_min_idx   = mni;
        g_leaf_diag.cell_max_idx   = mxi;
        g_leaf_diag.cells_generation++;  // bump AFTER the full copy — never expose a half-parsed set
        g_leaf_diag.last_poll_ok_ms = now;
        break;
      }
      if ((d[0] % 2) == 0) {  // even frames: complete the straddle, then 3 full cells
        cell_orput(d[1]);
        cell_put(((uint16_t)d[2] << 8) | d[3]);
        cell_put(((uint16_t)d[4] << 8) | d[5]);
        cell_put(((uint16_t)d[6] << 8) | d[7]);
      } else {  // odd frames: 3 full cells, then a straddled half
        cell_put(((uint16_t)d[1] << 8) | d[2]);
        cell_put(((uint16_t)d[3] << 8) | d[4]);
        cell_put(((uint16_t)d[5] << 8) | d[6]);
        cell_puthi(d[7]);
      }
      break;
    }

    case 0x04: {  // temperatures — L463-518
      if (d[0] == 0x10) {  // first message
        g_temp_raw1     = ((uint16_t)d[4] << 8) | d[5];
        g_temp_raw2_hi  = d[7];
      }
      if (d[0] == 0x21) {  // second message
        g_temp_raw2 = ((uint16_t)g_temp_raw2_hi << 8) | d[1];
        g_temp_raw3 = ((uint16_t)d[3] << 8) | d[4];
        g_temp_raw4 = ((uint16_t)d[6] << 8) | d[7];
      }
      if (d[0] == 0x22) {  // third message — all raws in, compute
        const bool three_sensor = (g_temp_raw3 == 65535);  // 2013+ pack, no sensor 3
        uint16_t raw_max, raw_min;
        if (three_sensor) {
          raw_max = g_temp_raw1;
          if (g_temp_raw2 > raw_max) raw_max = g_temp_raw2;
          if (g_temp_raw4 > raw_max) raw_max = g_temp_raw4;
          raw_min = g_temp_raw1;
          if (g_temp_raw2 < raw_min) raw_min = g_temp_raw2;
          if (g_temp_raw4 < raw_min) raw_min = g_temp_raw4;
        } else {
          raw_max = g_temp_raw1;
          if (g_temp_raw2 > raw_max) raw_max = g_temp_raw2;
          if (g_temp_raw3 > raw_max) raw_max = g_temp_raw3;
          if (g_temp_raw4 > raw_max) raw_max = g_temp_raw4;
          raw_min = g_temp_raw1;
          if (g_temp_raw2 < raw_min) raw_min = g_temp_raw2;
          // Deliberate deviation from upstream (NISSAN-LEAF-BATTERY.cpp L510-512):
          //   upstream: if (battery_temp_raw_3 < battery_temp_raw_min) { battery_temp_raw_min = battery_temp_raw_2; }
          //   here:     if (raw3 < raw_min)                            {              raw_min = raw3;             }
          // raw_2 was clearly a copy-paste bug — raw_3 is the value that was just tested.
          if (g_temp_raw3 < raw_min) raw_min = g_temp_raw3;
          if (g_temp_raw4 < raw_min) raw_min = g_temp_raw4;
        }

        g_leaf_diag.temp_c[0] = raw_to_c(g_temp_raw1);
        g_leaf_diag.temp_c[1] = raw_to_c(g_temp_raw2);
        g_leaf_diag.temp_c[2] = three_sensor ? -1000 : raw_to_c(g_temp_raw3);
        g_leaf_diag.temp_c[3] = raw_to_c(g_temp_raw4);
        g_leaf_diag.temp_poll_min_c = raw_to_c(raw_min);
        g_leaf_diag.temp_poll_max_c = raw_to_c(raw_max);
        g_leaf_diag.last_poll_ok_ms = now;
      }
      break;
    }

    case 0x06: {  // 96 balancing-shunt bits — L520-550
      if (d[0] == 0x10) {  // first frame: cells 0-31 (bytes 4-7)
        for (int i = 0; i < 8; i++)
          for (int byte_i = 0; byte_i < 4; byte_i++)
            g_shunt_scratch[byte_i * 8 + i] = (d[4 + byte_i] & (1 << i)) != 0;
      }
      if (d[0] == 0x21) {  // second frame: cells 32-87 (bytes 1-7)
        for (int i = 0; i < 8; i++)
          for (int byte_i = 0; byte_i < 7; byte_i++)
            g_shunt_scratch[32 + byte_i * 8 + i] = (d[1 + byte_i] & (1 << i)) != 0;
      }
      if (d[0] == 0x22) {  // third frame: cells 88-95 (byte 1)
        for (int i = 0; i < 8; i++)
          g_shunt_scratch[88 + i] = (d[1] & (1 << i)) != 0;

        uint32_t packed[3] = {0, 0, 0};
        for (int i = 0; i < 96; i++)
          if (g_shunt_scratch[i]) packed[i / 32] |= (1u << (i % 32));
        memcpy(g_leaf_diag.shunts, packed, sizeof(packed));
        g_leaf_diag.shunts_generation++;
        g_leaf_diag.last_poll_ok_ms = now;
      }
      break;
    }

    case 0x83: {  // battery part number, 7 ASCII chars — L552-570
      if (d[0] == 0x10) {
        g_part_scratch[0] = (char)d[4];
        g_part_scratch[1] = (char)d[5];
        g_part_scratch[2] = (char)d[6];
        g_part_scratch[3] = (char)d[7];
      }
      if (d[0] == 0x21) {
        g_part_scratch[4] = (char)d[1];
        g_part_scratch[5] = (char)d[2];
        g_part_scratch[6] = (char)d[3];
        memcpy(g_leaf_diag.part_number, g_part_scratch, 7);
        g_leaf_diag.part_number[7] = '\0';
        g_part_done = true;
        g_leaf_diag.last_poll_ok_ms = now;
      }
      break;
    }

    case 0x84: {  // battery serial number, 15 ASCII chars — L571-595
      if (d[0] == 0x10) {
        g_serial_scratch[0] = (char)d[7];
      }
      if (d[0] == 0x21) {
        for (int i = 0; i < 7; i++) g_serial_scratch[1 + i] = (char)d[1 + i];
      }
      if (d[0] == 0x22) {
        for (int i = 0; i < 7; i++) g_serial_scratch[8 + i] = (char)d[1 + i];
        memcpy(g_leaf_diag.serial, g_serial_scratch, 15);
        g_leaf_diag.serial[15] = '\0';
        g_serial_done = true;
        g_leaf_diag.last_poll_ok_ms = now;
      }
      break;
    }

    case 0x90: {  // BMS ID code, 8 ASCII chars — L597-610
      if (d[0] == 0x10) {
        g_bmsid_scratch[0] = (char)d[4];
        g_bmsid_scratch[1] = (char)d[5];
        g_bmsid_scratch[2] = (char)d[6];
        g_bmsid_scratch[3] = (char)d[7];
      }
      if (d[0] == 0x21) {
        g_bmsid_scratch[4] = (char)d[1];
        g_bmsid_scratch[5] = (char)d[2];
        g_bmsid_scratch[6] = (char)d[3];
        g_bmsid_scratch[7] = (char)d[4];
        memcpy(g_leaf_diag.bms_id, g_bmsid_scratch, 8);
        g_leaf_diag.bms_id[8] = '\0';
        g_bmsid_done = true;
        g_leaf_diag.last_poll_ok_ms = now;
      }
      break;
    }

    default:
      break;
  }
}

// ── capture: called from pump() for every RX frame ──────────────────────────
static uint32_t g_last_activity_ms = 0;  // last TX/RX in the current group transaction

void leaf_diag_capture(BridgeBus from, const BridgeFrame &f) {
  const uint32_t now = millis();

  if (f.id == 0x79B) {
    // Any RECEIVED 0x79B is by definition from an external tool — our own
    // requests are sent via canbus_send() and never loop back into RX. A real
    // OBD diagnostic dongle is on the bus: go quiet to avoid two ISO-TP
    // conversations colliding on the same request ID (mirrors the reference's
    // stop_battery_query, NISSAN-LEAF-BATTERY.cpp L368-371).
    g_leaf_diag.paused_until_ms = now + DIAG_EXTERNAL_PAUSE_MS;
    return;
  }

  if (f.id != 0x7BB) return;
  if (g_telemetry.battery_bus < 0 || (int32_t)from != g_telemetry.battery_bus) return;
  if ((int32_t)(now - g_leaf_diag.paused_until_ms) < 0) return;  // external tool active — stay fully silent

  g_last_activity_ms = now;
  parse_7bb(f.data, now);

  // Flow control: block-size-1, unconditionally request the next line after
  // every accepted 0x7BB frame (reference L400 / LEAF_NEXT_LINE_REQUEST).
  BridgeFrame fc = {};
  fc.id  = 0x79B;
  fc.ext = false;
  fc.dlc = 8;
  fc.data[0] = 0x30; fc.data[1] = 0x01; fc.data[2] = 0x00;
  fc.data[3] = fc.data[4] = fc.data[5] = fc.data[6] = fc.data[7] = 0xFF;
  canbus_send((BridgeBus)g_telemetry.battery_bus, fc);
}

// ── task: poll state machine, main loop task only ───────────────────────────
static bool     g_waiting = false;
static uint32_t g_next_poll_ms = 0;

void leaf_diag_task() {
  const uint32_t now = millis();

  if (g_waiting && (now - g_last_activity_ms > DIAG_TIMEOUT_MS)) {
    g_waiting = false;  // in-flight group abandoned — next tick moves on
  }

  const bool paused = (int32_t)(now - g_leaf_diag.paused_until_ms) < 0;
  const bool battery_ok = (g_telemetry.battery_bus >= 0) &&
                           (now - g_telemetry.last_battery_frame_ms < 2000);
  if (paused || !battery_ok) return;
  if (g_waiting) return;
  if (g_next_poll_ms != 0 && (now - g_next_poll_ms < DIAG_POLL_INTERVAL_MS)) return;

  g_next_poll_ms = now;
  const uint8_t grp = pick_next_group();

  BridgeFrame req = {};
  req.id  = 0x79B;
  req.ext = false;
  req.dlc = 8;
  req.data[0] = 0x02; req.data[1] = 0x21; req.data[2] = grp;
  req.data[3] = req.data[4] = req.data[5] = req.data[6] = req.data[7] = 0;
  canbus_send((BridgeBus)g_telemetry.battery_bus, req);

  g_group_7bb   = 0;  // fresh parse state for this group's response chain
  g_cell_idx    = 0;
  g_waiting     = true;
  g_last_activity_ms = now;
}
