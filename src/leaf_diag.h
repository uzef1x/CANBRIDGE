// ─────────────────────────────────────────────────────────────────────────────
// OBD-style battery diagnostic polling (0x79B/0x7BB) for the web dashboard.
//
// Ported (with one deliberate bug fix, noted below) from dalathegreat's
// Battery-Emulator, GPL-3.0:
//   Software/src/battery/NISSAN-LEAF-BATTERY.cpp / .h
//   (handle_incoming_can_frame() case 0x79B/0x7BB, transmit_can() 10s block,
//   Temp_fromRAW_to_F())
// https://github.com/dalathegreat/Battery-Emulator
//
// This polls the LBC exactly the way an OBD diagnostic dongle does: one multi-frame
// ISO-TP-style group request every DIAG_POLL_INTERVAL_MS, answered with a chain
// of 0x7BB frames each acknowledged by a block-size-1 flow-control frame. If a
// real external tool is seen polling (any 0x79B on either bus — our own TX never
// appears in RX), we go quiet for DIAG_EXTERNAL_PAUSE_MS to avoid two competing
// ISO-TP conversations on the same request ID.
//
// Concurrency: leaf_diag_capture() and leaf_diag_task() both run on the
// dedicated CAN-pump task (capture from pump(), task from can_task()), so the
// two never race and no locking is used between them. leaf_diag_capture() may
// (and does) call canbus_send() directly for the flow-control frame, unlike
// telemetry_capture() which is strictly read-only. All published LeafDiag
// fields are WRITTEN only from that CAN task. Scalar fields (uint32_t/int32_t/
// float) are plain aligned types written with a single store — atomic on
// Xtensa — so the web (AsyncTCP) task may read those without a lock. The arrays
// (cells_mv[96], shunts[3], temp_c[4]) are memcpy'd, NOT single-store; they are
// read ONLY by build_cells_message()/build_snapshot() inside webui_broadcast(),
// which runs on the CAN task itself (same task as the writer) — so they cannot
// tear. Moving any array read to the loop task or an async handler would tear
// and would need a seqlock/lock.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <stdint.h>
#include "can_frame.h"

#define DIAG_POLL_INTERVAL_MS   3000u   // one group request per tick
#define DIAG_TIMEOUT_MS         500u    // in-flight group abandoned after this much silence
#define DIAG_EXTERNAL_PAUSE_MS  100000u // pause when a real OBD diagnostic tool is detected (~dala's ~100s margin)

// ── Published diagnostic snapshot (main loop writes, web task reads) ───────────
struct LeafDiag {
  // Group 0x02 — 96 cell voltages, mV. cells_generation bumps only after a full,
  // successfully-parsed set (never expose a half-parsed array).
  uint16_t cells_mv[96];
  uint32_t cells_generation;
  uint32_t cell_min_mv, cell_max_mv, cell_avg_mv, cell_spread_mv;
  int32_t  cell_min_idx, cell_max_idx;

  // Group 0x06 — 96 balancing-shunt bits, packed 32 cells/word, bit i = cell
  // (32*word + i), LSB-first per byte exactly as the reference decodes them.
  uint32_t shunts[3];
  uint32_t shunts_generation;

  // Group 0x04 — per-sensor pack temperatures, degC. Sentinel -1000 marks a
  // sensor absent (sensor 3 on 2013+ / 3-sensor packs).
  int32_t temp_c[4];
  // Aggregate min/max across the available sensors — computed with the upstream
  // min-search bug fixed (see leaf_diag.cpp), not currently surfaced on the
  // dashboard but kept for fidelity / future use.
  int32_t temp_poll_min_c, temp_poll_max_c;

  // Group 0x01
  float    hx_pct;          // internal resistance-derived Hx, %
  uint32_t insulation_raw;  // raw insulation resistance reading

  // Identity groups 0x83/0x84/0x90 — NUL-terminated ASCII
  char part_number[8];  // 7 chars + NUL
  char serial[16];      // 15 chars + NUL
  char bms_id[9];       // 8 chars + NUL

  uint32_t last_poll_ok_ms;   // millis() of the last fully-parsed group response
  uint32_t paused_until_ms;   // millis() until which polling is paused (0 = never paused yet)
  uint32_t ext_pause;         // 1 = the current pause was caused by an external OBD tool,
                              // 0 = startup hold-off (or not paused). Lets the dashboard
                              // state the reason instead of guessing it from uptime.
};

extern LeafDiag g_leaf_diag;

// One-time init. Call once from bridge_begin(), after telemetry_begin().
void leaf_diag_begin();

// Called from pump() for every RX frame, main loop task, right after
// telemetry_capture(). Handles 0x7BB parsing + flow control (battery bus only)
// and 0x79B external-tool detection (any bus).
void leaf_diag_capture(BridgeBus from, const BridgeFrame &f);

// Poll state machine. Call from the CAN-pump task (can_task()), after
// webui_drain_tx(). May call canbus_send() directly.
void leaf_diag_task();
