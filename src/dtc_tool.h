// ─────────────────────────────────────────────────────────────────────────────
// On-demand UDS DTC (diagnostic trouble code) scan/clear tool, driven from the
// web dashboard. Reads (service 0x19 ReadDTCInformation, subfn 0x02, mask
// 0x2B) and clears (service 0x10 DiagnosticSessionControl session 0xC0, then
// service 0x14 ClearDiagnosticInformation groupOfDTC 0xFFFFFF) DTCs on every
// ECU in the table in dtc_tool.cpp, over classic-CAN ISO-TP. Byte sequences
// are wire-captured ground truth from this car, not off a spec sheet — see
// dtc_tool.cpp.
//
// Task split mirrors leaf_diag.h exactly: dtc_tool_capture() is tapped from
// pump() (right after leaf_diag_capture()), dtc_tool_task() is ticked from
// can_task() (right after leaf_diag_task()) — both on the dedicated CAN-pump
// task, so no lock is needed between them. dtc_tool_task() advances at most
// one action per call (send one request, or process one already-assembled
// response) — it never blocks waiting on a reply, so it can't trip the CAN
// task's 2 s watchdog.
//
// Concurrency: g_dtc_results[]/g_dtc_generation/g_dtc_scanning are WRITTEN
// only by the CAN-pump task and READ by the web (AsyncTCP/loop) task without a
// lock — same accepted display-only cross-task pattern as leaf_diag (see the
// banner in leaf_diag.h / telemetry.h). g_dtc_generation is a plain uint32_t
// the web task polls to know when to re-push; DtcEcuResult itself is struct-
// assigned (not single-store), so a torn read is possible in principle — same
// trade-off as cells_mv[]/shunts[], display-only, never safety-relevant.
//
// dtc_request_scan()/dtc_request_clear()/dtc_request_clear_all() are called
// from the AsyncTCP WS task (webui.cpp's handle_cmd_dtc* handlers) — they ONLY
// set volatile pending flags, never touch CAN, NVS, or canbus_send(). The
// actual TX happens in dtc_tool_task() on the CAN task, which re-checks
// can_tx_safe() at execution time (not just at WS-enqueue time) before any
// transmit — the car may start moving between the click and the tick.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <stdint.h>
#include "can_frame.h"

#define DTC_MAX_PER_ECU 16

struct DtcCode { uint8_t b0, b1, b2, status; };

enum DtcEcuStatus { DTC_ECU_UNKNOWN=0, DTC_ECU_SCANNING, DTC_ECU_OK, DTC_ECU_FAULTS, DTC_ECU_NORESP, DTC_ECU_CLEARING };

struct DtcEcuResult {
  uint8_t status;        // DtcEcuStatus
  uint8_t n;             // number of DtcCode entries valid in codes[]
  DtcCode codes[DTC_MAX_PER_ECU];
  char clear_msg[24];    // "", "cleared", "clear failed: no 50C0", etc.
};

extern DtcEcuResult g_dtc_results[];   // one per ECU (see dtc_ecu_count())
extern uint32_t g_dtc_generation;      // bumped on any results change; web task watches this
extern volatile int g_dtc_scanning;    // 1 while a scan/clear job is in progress, else 0

int  dtc_ecu_count();
const char *dtc_ecu_name(int i);
uint16_t dtc_ecu_req(int i);
bool dtc_ecu_clear_verified(int i);  // true = clear sequence wire-verified on this car; false = standard-UDS, unproven
const char *dtc_error();  // "" when no error; set on refusal/abort, cleared at each new job start

// control — called from the AsyncTCP WS task (webui.cpp): only set volatile
// pending flags, never touch CAN.
void dtc_request_scan();          // full scan of all ECUs
void dtc_request_clear(int ecu);  // clear one ECU (then it re-reads that ECU)
void dtc_request_clear_all();     // clear every ECU currently in FAULTS state
bool dtc_tool_active();           // true while a job is ACTIVELY running (g_dtc_scanning)
bool dtc_tool_busy();             // true while a job is pending OR active — leaf_diag stands down on this

// CAN-pump task:
void dtc_tool_capture(BridgeBus from, const BridgeFrame &f);  // feed every RX frame to the active job
void dtc_tool_task();                                         // tick the state machine
