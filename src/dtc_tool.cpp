// ─────────────────────────────────────────────────────────────────────────────
// See dtc_tool.h for the concurrency contract and protocol provenance.
//
// VERIFIED PROTOCOL (captured live on this car), all ISO-TP over classic CAN,
// 8-byte frames, unused request bytes 0x00:
//   READ:  req `03 19 02 2B 00 00 00 00` -> SF `03 59 02 <mask>` (no DTCs) or
//          FF/CF chain assembling to `59 02 <mask> <4-byte DTC records...>`,
//          each accepted frame answered with FC `30 00 00 00 00 00 00 00`.
//   CLEAR: req `02 10 C0 00 00 00 00 00` -> expect `02 50 C0`, then
//          req `04 14 FF FF FF 00 00 00` -> expect `01 54`.
// ─────────────────────────────────────────────────────────────────────────────
#include "dtc_tool.h"
#include "telemetry.h"
#include "can_bus.h"
#include <Arduino.h>
#include <string.h>

// ── ECU table — ONLY the ECUs wire-confirmed reachable from the bridge's
// position on EV-CAN (in the battery line). The VCM and the body/chassis ECUs
// (ABS, BCM, meter, EPS, airbag, …) answer diagnostics on the OBD/Car-CAN side
// via the VCM gateway and never appear on this bus — a live capture proved the
// VCM (EV/HEV) clear is invisible here — so they are deliberately omitted: this
// tool only lists what it can actually reach and clear.
// The battery is identified by req id 0x79B (its bus is g_telemetry.battery_bus);
// every other ECU here sits on the vehicle bus (the other of the two).
// clear_verified: this ECU returns the positive clear-accepted response (0x54)
// to the 10 C0 + 14 FF FF FF sequence. ALL SIX confirmed on this car 2026-09-28
// via an acceptance test (each returned 54). The battery is additionally proven
// to actually remove stored codes (its 4 real DTCs went away); the other five
// were acknowledged with no codes present to remove.
static const struct { const char *name; uint16_t req; uint16_t resp; bool clear_verified; } ECUS[] = {
  {"HV Battery",  0x79B, 0x7BB, true},
  {"Inverter/MC", 0x784, 0x78C, true},
  {"Charger/PDM", 0x792, 0x793, true},
  {"HVAC",        0x744, 0x764, true},
  {"Shift",       0x79D, 0x7BD, true},
  {"TCU",         0x746, 0x783, true},
};
#define DTC_ECU_COUNT (sizeof(ECUS) / sizeof(ECUS[0]))

DtcEcuResult g_dtc_results[DTC_ECU_COUNT] = {};
uint32_t g_dtc_generation = 0;
volatile int g_dtc_scanning = 0;

// Read cross-task alongside g_dtc_results (same display-only, no-lock contract
// — see dtc_tool.h). Bumped to "" on every new job so a stale error doesn't
// linger from a previous run.
static char g_dtc_error[48] = "";

int  dtc_ecu_count()            { return (int)DTC_ECU_COUNT; }
const char *dtc_ecu_name(int i) { return ECUS[i].name; }
uint16_t dtc_ecu_req(int i)     { return ECUS[i].req; }
bool dtc_ecu_clear_verified(int i) { return ECUS[i].clear_verified; }
const char *dtc_error()         { return g_dtc_error; }

static BridgeBus ecu_bus(int i) {
  int32_t bb = g_telemetry.battery_bus;
  if (ECUS[i].req == 0x79B) return (BridgeBus)bb;         // HV Battery
  return (BridgeBus)((bb == BUS_A) ? BUS_B : BUS_A);       // every other ECU: the vehicle bus
}

// ── AsyncTCP WS task -> CAN task handoff (flags only, no CAN touched here) ──
static volatile bool g_pending_scan      = false;
static volatile int  g_pending_clear_ecu = -1;  // -1 none, -2 clear-all, >=0 ecu index

bool dtc_tool_active() { return g_dtc_scanning != 0; }
void dtc_request_scan()          { g_pending_scan = true; }
void dtc_request_clear(int ecu)  { g_pending_clear_ecu = ecu; }
void dtc_request_clear_all()     { g_pending_clear_ecu = -2; }

// ── Job state (CAN-pump task only — dtc_tool_capture()/dtc_tool_task() both
// run there, so no lock is needed between them) ─────────────────────────────
enum JobMode   { JOB_NONE=0, JOB_SCAN, JOB_CLEAR };
enum ClearStep { CLEAR_SESSION=0, CLEAR_CLEARDTC, CLEAR_REREAD };

#define DTC_STEP_TIMEOUT_MS 250u

static int      g_mode       = JOB_NONE;
static int      g_cur_ecu    = -1;
static int      g_clear_step = CLEAR_SESSION;
static bool     g_clear_all  = false;  // this CLEAR job is chaining through every FAULTS ecu
static bool     g_awaiting   = false;  // a request is in flight, waiting on a response
static uint32_t g_deadline   = 0;

// ISO-TP reassembly for the response currently being waited on.
static uint8_t  g_asm_buf[64];
static uint16_t g_asm_expected = 0;
static uint16_t g_asm_got      = 0;
static bool     g_asm_active   = false;  // true between FF and the completing CF
static bool     g_frame_ready  = false;  // g_asm_buf[0..g_asm_got) is a complete assembled payload

static const uint8_t REQ_READ_DTC[8]   = {0x03,0x19,0x02,0x2B,0x00,0x00,0x00,0x00};
static const uint8_t REQ_SESSION_C0[8] = {0x02,0x10,0xC0,0x00,0x00,0x00,0x00,0x00};
static const uint8_t REQ_CLEAR_DTC[8]  = {0x04,0x14,0xFF,0xFF,0xFF,0x00,0x00,0x00};
static const uint8_t FLOW_CONTROL[8]   = {0x30,0x00,0x00,0x00,0x00,0x00,0x00,0x00};

static void set_msg(char *dst, size_t dstlen, const char *msg) {
  strncpy(dst, msg, dstlen - 1);
  dst[dstlen - 1] = '\0';
}

static void send_to(int ecu, const uint8_t *bytes) {
  BridgeFrame f = {};
  f.id  = ECUS[ecu].req;
  f.ext = false;
  f.dlc = 8;
  memcpy(f.data, bytes, 8);
  canbus_send(ecu_bus(ecu), f);
}

static void arm_wait() {
  g_asm_expected = 0; g_asm_got = 0; g_asm_active = false; g_frame_ready = false;
  g_awaiting = true;
  g_deadline = millis() + DTC_STEP_TIMEOUT_MS;
}

static void finish_job() {
  g_mode     = JOB_NONE;
  g_awaiting = false;
  g_cur_ecu  = -1;
  g_dtc_scanning = 0;
  g_dtc_generation++;
}

static void scan_advance() {
  int next = g_cur_ecu + 1;
  if (next >= (int)DTC_ECU_COUNT) { finish_job(); return; }
  g_cur_ecu = next;
  g_dtc_results[next].status = DTC_ECU_SCANNING;
  send_to(next, REQ_READ_DTC);
  arm_wait();
  g_dtc_generation++;
}

static void clear_start_reread() {
  g_clear_step = CLEAR_REREAD;
  send_to(g_cur_ecu, REQ_READ_DTC);
  arm_wait();
  g_dtc_generation++;
}

static void clear_advance_or_finish() {
  if (!g_clear_all) { finish_job(); return; }
  // Chain forward only (never loop back over ECUs already handled this run).
  int next = -1;
  for (int i = g_cur_ecu + 1; i < (int)DTC_ECU_COUNT; i++)
    if (g_dtc_results[i].status == DTC_ECU_FAULTS) { next = i; break; }
  if (next < 0) { finish_job(); return; }
  g_cur_ecu = next;
  g_clear_step = CLEAR_SESSION;
  g_dtc_results[next].status = DTC_ECU_CLEARING;
  g_dtc_results[next].clear_msg[0] = '\0';
  send_to(next, REQ_SESSION_C0);
  arm_wait();
  g_dtc_generation++;
}

// Parse an assembled `59 02 <mask> <4-byte records...>` payload into
// g_dtc_results[ecu], clamped to DTC_MAX_PER_ECU. Anything else (negative
// response, garbled reassembly) is treated as no usable answer.
static void parse_dtc_payload_and_finalize(int ecu) {
  DtcEcuResult &r = g_dtc_results[ecu];
  if (g_asm_got < 3 || g_asm_buf[0] != 0x59 || g_asm_buf[1] != 0x02) {
    r.status = DTC_ECU_NORESP;
    r.n = 0;
    return;
  }
  int n = (g_asm_got - 3) / 4;
  if (n > DTC_MAX_PER_ECU) n = DTC_MAX_PER_ECU;
  for (int i = 0; i < n; i++) {
    const uint8_t *p = &g_asm_buf[3 + i * 4];
    r.codes[i].b0 = p[0]; r.codes[i].b1 = p[1]; r.codes[i].b2 = p[2]; r.codes[i].status = p[3];
  }
  r.n = (uint8_t)n;
  r.status = (n > 0) ? DTC_ECU_FAULTS : DTC_ECU_OK;
}

static void handle_timeout() {
  g_awaiting = false;
  if (g_mode == JOB_SCAN) {
    g_dtc_results[g_cur_ecu].status = DTC_ECU_NORESP;
    g_dtc_results[g_cur_ecu].n = 0;
    g_dtc_generation++;
    scan_advance();
    return;
  }
  // JOB_CLEAR
  DtcEcuResult &r = g_dtc_results[g_cur_ecu];
  if (g_clear_step == CLEAR_SESSION) {
    set_msg(r.clear_msg, sizeof(r.clear_msg), "clear failed: no 50C0");
    clear_start_reread();
    return;
  }
  if (g_clear_step == CLEAR_CLEARDTC) {
    set_msg(r.clear_msg, sizeof(r.clear_msg), "clear failed: no 54");
    clear_start_reread();
    return;
  }
  // CLEAR_REREAD timed out
  r.status = DTC_ECU_NORESP;
  r.n = 0;
  g_dtc_generation++;
  clear_advance_or_finish();
}

static void handle_response() {
  g_awaiting = false;
  if (g_mode == JOB_SCAN) {
    parse_dtc_payload_and_finalize(g_cur_ecu);
    g_dtc_generation++;
    scan_advance();
    return;
  }
  // JOB_CLEAR
  DtcEcuResult &r = g_dtc_results[g_cur_ecu];
  if (g_clear_step == CLEAR_SESSION) {
    bool ok = (g_asm_got >= 2 && g_asm_buf[0] == 0x50 && g_asm_buf[1] == 0xC0);
    if (ok) {
      g_clear_step = CLEAR_CLEARDTC;
      send_to(g_cur_ecu, REQ_CLEAR_DTC);
      arm_wait();
    } else {
      set_msg(r.clear_msg, sizeof(r.clear_msg), "clear failed: no 50C0");
      clear_start_reread();
    }
    return;
  }
  if (g_clear_step == CLEAR_CLEARDTC) {
    bool ok = (g_asm_got >= 1 && g_asm_buf[0] == 0x54);
    set_msg(r.clear_msg, sizeof(r.clear_msg), ok ? "cleared" : "clear failed: no 54");
    clear_start_reread();
    return;
  }
  // CLEAR_REREAD
  parse_dtc_payload_and_finalize(g_cur_ecu);
  g_dtc_generation++;
  clear_advance_or_finish();
}

void dtc_tool_task() {
  const uint32_t now = millis();

  // ── Pick up a new job when idle ──────────────────────────────────────────
  if (g_mode == JOB_NONE) {
    if (g_pending_scan) {
      g_pending_scan = false;
      g_dtc_error[0] = '\0';
      if (!can_tx_safe()) {
        for (int i = 0; i < (int)DTC_ECU_COUNT; i++) g_dtc_results[i] = {};
        set_msg(g_dtc_error, sizeof(g_dtc_error), "locked: car must be parked");
        g_dtc_generation++;
        return;
      }
      if (g_telemetry.battery_bus < 0) {
        for (int i = 0; i < (int)DTC_ECU_COUNT; i++) g_dtc_results[i] = {};
        set_msg(g_dtc_error, sizeof(g_dtc_error), "battery bus not yet identified");
        g_dtc_generation++;
        return;
      }
      for (int i = 0; i < (int)DTC_ECU_COUNT; i++) g_dtc_results[i] = {};  // status DTC_ECU_UNKNOWN == 0
      g_mode      = JOB_SCAN;
      g_cur_ecu   = 0;
      g_clear_all = false;
      g_dtc_scanning = 1;
      g_dtc_results[0].status = DTC_ECU_SCANNING;
      send_to(0, REQ_READ_DTC);
      arm_wait();
      g_dtc_generation++;
      return;
    }
    if (g_pending_clear_ecu != -1) {
      int req = g_pending_clear_ecu;
      g_pending_clear_ecu = -1;
      g_dtc_error[0] = '\0';
      if (!can_tx_safe()) { set_msg(g_dtc_error, sizeof(g_dtc_error), "locked: car must be parked"); g_dtc_generation++; return; }
      if (g_telemetry.battery_bus < 0) { set_msg(g_dtc_error, sizeof(g_dtc_error), "battery bus not yet identified"); g_dtc_generation++; return; }

      int start_ecu;
      if (req == -2) {  // clear-all: first ECU currently in FAULTS
        start_ecu = -1;
        for (int i = 0; i < (int)DTC_ECU_COUNT; i++) if (g_dtc_results[i].status == DTC_ECU_FAULTS) { start_ecu = i; break; }
        if (start_ecu < 0) return;  // nothing to clear
        g_clear_all = true;
      } else {
        if (req < 0 || req >= (int)DTC_ECU_COUNT) return;
        start_ecu = req;
        g_clear_all = false;
      }
      g_mode       = JOB_CLEAR;
      g_cur_ecu    = start_ecu;
      g_clear_step = CLEAR_SESSION;
      g_dtc_scanning = 1;
      g_dtc_results[start_ecu].status = DTC_ECU_CLEARING;
      g_dtc_results[start_ecu].clear_msg[0] = '\0';
      send_to(start_ecu, REQ_SESSION_C0);
      arm_wait();
      g_dtc_generation++;
      return;
    }
    return;  // nothing to do
  }

  // ── Active job: re-check the interlock every tick, at execution time, not
  // just when the job was enqueued from the WS task — the car can start
  // moving mid-scan/clear. ────────────────────────────────────────────────
  if (!can_tx_safe()) {
    if (g_cur_ecu >= 0) g_dtc_results[g_cur_ecu].status = DTC_ECU_UNKNOWN;
    set_msg(g_dtc_error, sizeof(g_dtc_error), "locked: car must be parked (aborted)");
    g_mode     = JOB_NONE;
    g_awaiting = false;
    g_cur_ecu  = -1;
    g_dtc_scanning = 0;
    g_dtc_generation++;
    return;
  }

  if (!g_awaiting) return;  // shouldn't happen (every job start arms a wait), but never spin

  if (!g_frame_ready) {
    if ((int32_t)(now - g_deadline) < 0) return;  // still within the timeout
    handle_timeout();
    return;
  }

  handle_response();
}

void dtc_tool_capture(BridgeBus from, const BridgeFrame &f) {
  if (g_mode == JOB_NONE || !g_awaiting || g_frame_ready) return;  // no job waiting on a reply right now
  if (g_cur_ecu < 0 || f.id != ECUS[g_cur_ecu].resp) return;
  if (ecu_bus(g_cur_ecu) != from) return;

  const uint8_t *d = f.data;
  const uint8_t pci = d[0] >> 4;

  if (pci == 0x0) {  // single frame — d[0] IS the literal length (0..7), not a nibble-coded PCI
    uint8_t len = d[0] & 0x0F;
    if (len > 7) len = 7;
    for (int i = 0; i < len; i++) g_asm_buf[i] = d[1 + i];
    g_asm_got      = len;
    g_asm_expected = len;
    g_asm_active   = false;
    g_frame_ready  = true;
    return;
  }
  if (pci == 0x1) {  // first frame of a multi-frame chain
    uint16_t total = (uint16_t)(((d[0] & 0x0F) << 8) | d[1]);
    if (total > sizeof(g_asm_buf)) total = sizeof(g_asm_buf);  // clamp — defensive, never trust the wire blindly
    g_asm_expected = total;
    uint16_t first_chunk = (total < 6) ? total : 6;
    for (int i = 0; i < first_chunk; i++) g_asm_buf[i] = d[2 + i];
    g_asm_got     = first_chunk;
    g_asm_active  = true;
    g_frame_ready = false;
    // Block-size-1 flow control: unconditionally request the rest right away
    // (same pattern as leaf_diag_capture()'s FC). Gated by can_tx_safe() —
    // if it just went false, drop this: the step will simply time out.
    if (can_tx_safe()) {
      BridgeFrame fc = {};
      fc.id  = ECUS[g_cur_ecu].req;
      fc.ext = false;
      fc.dlc = 8;
      memcpy(fc.data, FLOW_CONTROL, 8);
      canbus_send(from, fc);
    }
    return;
  }
  if (pci == 0x2) {  // consecutive frame
    if (!g_asm_active) return;
    for (int i = 0; i < 7 && g_asm_got < g_asm_expected; i++) g_asm_buf[g_asm_got++] = d[1 + i];
    if (g_asm_got >= g_asm_expected) { g_asm_active = false; g_frame_ready = true; }
    return;
  }
  // other PCI types (flow control echoed back, etc.) — not expected here, ignore
}
