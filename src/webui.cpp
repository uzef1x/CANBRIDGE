// ─────────────────────────────────────────────────────────────────────────────
// WiFi dashboard implementation. See webui.h for the concurrency contract.
//
// WebSocket message handling (onMessage) runs in the AsyncTCP task — it must
// never touch the CAN drivers directly. Parsed TX requests go onto g_tx_queue;
// webui_drain_tx() (called from the dedicated CAN-pump task) is the only place
// canbus_send() is called from here. webui_broadcast() also runs on the CAN
// task; webui_housekeeping() (NVS writes, reboot) runs on the Arduino loop task.
// ─────────────────────────────────────────────────────────────────────────────
#include "webui.h"
#include "telemetry.h"
#include "leaf_diag.h"
#include "can_bus.h"
#include "vehicle_config.h"
#include "leaf_translation.h"
#include "ap_config.h"
#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string.h>
#include <DNSServer.h>
#include "webui_page.h"

static AsyncWebServer server(80);
// Mirror on 8080: browsers auto-upgrade port-80 URLs to HTTPS (which we can't
// serve) but never rewrite URLs with an explicit port — http://10.0.0.1:8080
// is the reliable address on strict-HTTPS phones.
static AsyncWebServer server8080(8080);
static AsyncWebSocket ws("/ws");
static DNSServer g_dns;

// ── Web -> bridge TX queue ───────────────────────────────────────────────────
struct TxRequest {
  BridgeBus bus;
  BridgeFrame frame;
};
static QueueHandle_t g_tx_queue = nullptr;
static volatile uint32_t g_reboot_request_ms = 0;  // web-requested reboot, executed in main loop
static volatile bool g_web_started = false;        // true once webui_begin() has run (read by CAN task)

// ── Async -> main-loop pending-request handoff ──────────────────────────────
// NVS writes (vehicle_store / ap_password_store) must never happen on the
// AsyncTCP task (flash-write stalls would hang CAN forwarding). The async WS
// handler only stashes the request here; webui_housekeeping() (Arduino loop
// task) drains it and performs the actual write.
static volatile int g_pending_profile = -1;  // -1 = none, else Vehicle value

// Single-producer (AsyncTCP task) / single-consumer (main loop) handoff for
// the AP password string. The async side copies the string into the buffer
// FIRST, then sets pending_appw_ready LAST (release order); the main loop
// only reads the buffer once it observes pending_appw_ready == true, then
// clears the flag. One flag, one buffer, one writer, one reader — no tearing.
static char g_pending_appw[64];
static volatile bool g_pending_appw_ready = false;

// ── Small helpers ────────────────────────────────────────────────────────────
static void ws_reply(AsyncWebSocketClient *client, const char *msg) {
  if (client && client->status() == WS_CONNECTED) client->text(msg);
}

// Resolve a "battery"/"vehicle"/"A"/"B" bus token to a real BridgeBus.
// Returns false (with *err set) if not resolvable.
static bool resolve_bus(const char *tok, BridgeBus &out, const char **err) {
  if (!strcmp(tok, "A")) { out = BUS_A; return true; }
  if (!strcmp(tok, "B")) { out = BUS_B; return true; }
  int32_t bb = g_telemetry.battery_bus;
  if (!strcmp(tok, "battery")) {
    if (bb < 0) { *err = "battery bus not yet identified"; return false; }
    out = (BridgeBus)bb;
    return true;
  }
  if (!strcmp(tok, "vehicle")) {
    if (bb < 0) { *err = "battery bus not yet identified"; return false; }
    out = (bb == BUS_A) ? BUS_B : BUS_A;
    return true;
  }
  *err = "unknown bus";
  return false;
}

static void ws_err(AsyncWebSocketClient *client, const char *msg) {
  char out[128];
  snprintf(out, sizeof(out), "{\"type\":\"err\",\"msg\":\"%s\"}", msg);
  ws_reply(client, out);
}

static void ws_ack(AsyncWebSocketClient *client, const char *msg) {
  char out[128];
  snprintf(out, sizeof(out), "{\"type\":\"ack\",\"msg\":\"%s\"}", msg);
  ws_reply(client, out);
}

// Handle {"cmd":"tx",...} — queue a custom TX frame. Gated by car_write_safe().
static void handle_cmd_tx(AsyncWebSocketClient *client, JsonDocument &doc) {
  if (!car_write_safe()) { ws_err(client, "locked: car must be parked"); return; }

  const char *bus_tok = doc["bus"] | "";
  const char *id_str  = doc["id"] | "";
  int dlc = doc["dlc"] | 0;
  JsonArrayConst data_arr = doc["data"];

  BridgeBus bus;
  const char *err = nullptr;
  if (!resolve_bus(bus_tok, bus, &err)) { ws_err(client, err); return; }

  BridgeFrame f = {};
  f.id  = (uint32_t)strtoul(id_str, nullptr, 0);
  f.ext = f.id > 0x7FF;
  f.dlc = (dlc < 0) ? 0 : (dlc > 8 ? 8 : (uint8_t)dlc);
  uint8_t i = 0;
  for (JsonVariantConst v : data_arr) {
    if (i >= 8) break;
    f.data[i++] = (uint8_t)(v.as<int>() & 0xFF);
  }
  if (i > f.dlc) f.dlc = i;

  if (!g_tx_queue) { ws_err(client, "tx queue unavailable"); return; }

  TxRequest req = { bus, f };
  if (xQueueSend(g_tx_queue, &req, 0) != pdTRUE) { ws_err(client, "tx queue full"); return; }

  char out[128];
  snprintf(out, sizeof(out), "{\"type\":\"ack\",\"msg\":\"queued 0x%03lX on %s (%s)\"}",
           (unsigned long)f.id, bus_tok, (bus == BUS_A) ? "A" : "B");
  ws_reply(client, out);
}

// Handle {"cmd":"reboot"} — defer to the main loop task via g_reboot_request_ms.
static void handle_cmd_reboot(AsyncWebSocketClient *client) {
  if (!car_write_safe()) { ws_err(client, "locked: car must be parked"); return; }
  g_reboot_request_ms = millis() + 300;  // give the WS reply time to flush; actual ESP.restart() runs from webui_housekeeping() on the loop task
  ws_ack(client, "rebooting");
}

// Handle {"cmd":"setprofile","value":"leaf"|"env200"} — stash for the main loop.
static void handle_cmd_setprofile(AsyncWebSocketClient *client, JsonDocument &doc) {
  if (!car_write_safe()) { ws_err(client, "locked: car must be parked"); return; }
  const char *value = doc["value"] | "";
  Vehicle v;
  if      (!strcmp(value, "leaf"))   v = VEHICLE_LEAF;
  else if (!strcmp(value, "env200")) v = VEHICLE_ENV200;
  else { ws_err(client, "value must be leaf|env200"); return; }
  // Reject while a prior profile change is still pending (not yet drained by the
  // loop task) so a second request can't be lost in the drain window.
  if (g_pending_profile >= 0) { ws_err(client, "busy, retry in a moment"); return; }
  g_pending_profile = (int)v;  // drained by webui_housekeeping() on the loop task
  ws_ack(client, "stored; reboot to apply");
}

// Handle {"cmd":"setappw","value":"<8..63 chars>"} — stash for the main loop.
// NEVER echoes the password back.
static void handle_cmd_setappw(AsyncWebSocketClient *client, JsonDocument &doc) {
  const char *value = doc["value"] | "";
  size_t len = strlen(value);
  if (len < 8 || len > 63) { ws_err(client, "password must be 8-63 chars"); return; }
  if (!car_write_safe()) { ws_err(client, "locked: car must be parked"); return; }
  // Reject a second setappw while the main loop hasn't consumed the first, so
  // we never strncpy over the buffer the consumer is mid-read of (avoids a torn
  // password persisted to NVS). The user simply retries.
  if (g_pending_appw_ready) { ws_err(client, "busy, retry in a moment"); return; }
  // Single-producer handoff: copy into the buffer, THEN set ready last.
  strncpy(g_pending_appw, value, sizeof(g_pending_appw) - 1);
  g_pending_appw[sizeof(g_pending_appw) - 1] = '\0';
  g_pending_appw_ready = true;
  ws_ack(client, "stored; reboot to apply");
}

// Parse an incoming WS text frame and dispatch to the right cmd handler.
// Runs in the AsyncTCP task — no handler here may touch canbus_send(), NVS,
// or ESP.restart() directly; each defers to the main loop via pending state.
static void handle_ws_command(AsyncWebSocketClient *client, const uint8_t *data, size_t len) {
  JsonDocument doc;
  DeserializationError jerr = deserializeJson(doc, data, len);
  if (jerr) { ws_err(client, "bad json"); return; }

  const char *cmd = doc["cmd"] | "";
  if      (!strcmp(cmd, "tx"))         handle_cmd_tx(client, doc);
  else if (!strcmp(cmd, "reboot"))     handle_cmd_reboot(client);
  else if (!strcmp(cmd, "setprofile")) handle_cmd_setprofile(client, doc);
  else if (!strcmp(cmd, "setappw"))    handle_cmd_setappw(client, doc);
  else ws_err(client, "unknown cmd");
}

static void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                       AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_DATA) {
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
      handle_ws_command(client, data, len);
    }
  }
}

void webui_begin() {
  g_tx_queue = xQueueCreate(16, sizeof(TxRequest));

  WiFi.mode(WIFI_AP);
  WiFi.softAP("CANBRIDGE", ap_password(), 1);
  // 10.0.0.x instead of the ESP32 default 192.168.4.x to avoid colliding with
  // common home-router subnets on the client's other interfaces.
  WiFi.softAPConfig(IPAddress(10, 0, 0, 1), IPAddress(10, 0, 0, 1), IPAddress(255, 255, 255, 0));
  WiFi.setSleep(false);  // modem power-save on soft-AP causes hung/slow responses on some clients
  Serial.printf("[webui] AP 'CANBRIDGE' up, IP: %s\n", WiFi.softAPIP().toString().c_str());

  // DNS: answer every query with our own IP so any typed hostname lands on the
  // dashboard. The OS connectivity probes are answered with SUCCESS below (not
  // hijacked into a portal): a captive-portal state would keep the phone's
  // default route on mobile data, making http://10.0.0.1 unreachable from a
  // real browser — the portal popup webview is for authentication flows only.
  g_dns.start(53, "*", WiFi.softAPIP());

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  // Bench-debug request log (runs in the AsyncTCP task; low volume, USB only).
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    Serial.printf("[http] GET / from %s\n", req->client()->remoteIP().toString().c_str());
    req->send(200, "text/html", PAGE_HTML);
  });

  // Answer OS connectivity probes with the responses they expect from the real
  // internet, so the network validates as "online": the phone keeps its WiFi
  // route usable and a normal browser can reach 10.0.0.1. (The phone will show
  // "no internet" only if it re-probes something we don't cover.)
  server.on("/generate_204", HTTP_GET, [](AsyncWebServerRequest *req) {                     // Android
    Serial.println("[http] android probe -> 204");
    req->send(204);
  });
  server.on("/gen_204", HTTP_GET, [](AsyncWebServerRequest *req) { req->send(204); });      // Android alt
  auto apple_ok = [](AsyncWebServerRequest *req) {                                          // iOS/macOS
    Serial.println("[http] apple probe -> Success");
    req->send(200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
  };
  server.on("/hotspot-detect.html", HTTP_GET, apple_ok);
  server.on("/library/test/success.html", HTTP_GET, apple_ok);
  server.on("/ncsi.txt", HTTP_GET, [](AsyncWebServerRequest *req) {                         // Windows
    req->send(200, "text/plain", "Microsoft NCSI");
  });
  server.on("/connecttest.txt", HTTP_GET, [](AsyncWebServerRequest *req) {                  // Windows 10+
    req->send(200, "text/plain", "Microsoft Connect Test");
  });

  // Profile selection, AP password, and reboot are all handled over the
  // WebSocket now (see handle_ws_command) — no GET endpoints for them.

  // Catch-all for anything else: redirect to the dashboard.
  server.onNotFound([](AsyncWebServerRequest *req) {
    Serial.printf("[http] %s %s from %s -> redirect\n", req->methodToString(),
                  req->url().c_str(), req->client()->remoteIP().toString().c_str());
    req->redirect("http://10.0.0.1/");
  });

  // Same dashboard + WebSocket on :8080 (see note at the server8080 definition).
  server8080.addHandler(&ws);
  server8080.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    Serial.printf("[http] GET :8080/ from %s\n", req->client()->remoteIP().toString().c_str());
    req->send(200, "text/html", PAGE_HTML);
  });
  server8080.onNotFound([](AsyncWebServerRequest *req) {
    req->redirect("http://10.0.0.1:8080/");
  });

  server.begin();
  server8080.begin();
  Serial.println("[webui] HTTP + WS server started on :80 and :8080 (connectivity probes answered OK)");
  g_web_started = true;  // webui_broadcast() may now touch DNS/WS
}

// ── Telemetry broadcast ──────────────────────────────────────────────────────
static uint32_t g_last_broadcast_ms = 0;

static void build_snapshot(char *buf, size_t buflen) {
  const Telemetry &t = g_telemetry;

  // Auto-detection readout: ONLY facts confirmed from observed traffic — never
  // the translation layer's boot assumptions. 24/30 kWh packs are identified by
  // the ABSENCE of the 40/62 marker frames, so that counts as confirmed only
  // after the battery has been live for a while.
  char detected[48] = "-";
  if (vehicle_active() == VEHICLE_LEAF) {
    bool veh_ok = false, batt_ok = false;
    const char *veh  = leaf_detected_vehicle(&veh_ok);
    const char *batt = leaf_detected_battery(&batt_ok);
    const uint32_t first = g_telemetry.first_battery_frame_ms;
    const bool batt_by_absence = !batt_ok && first && (millis() - first > 5000);
    if (batt_by_absence) batt = "24/30 kWh";
    if (veh_ok && (batt_ok || batt_by_absence))
      snprintf(detected, sizeof(detected), "%s \xc2\xb7 %s", veh, batt);
    else if (veh_ok)
      snprintf(detected, sizeof(detected), "%s", veh);
    else if (batt_ok || batt_by_absence)
      snprintf(detected, sizeof(detected), "%s", batt);
  }  // e-NV200 profile: fixed 24->40 kWh translation, nothing is auto-detected
  int n = snprintf(buf, buflen,
    "{\"vehicle\":\"%s\",\"uptime_ms\":%lu,\"free_heap\":%lu,"
    "\"profile_stored\":\"%s\",\"detected\":\"%s\",\"write_safe\":%d,"
    "\"can_rx_age_ms\":%ld,"
    "\"battery_bus\":%ld,\"last_battery_frame_ms\":%lu,"
    "\"pack_voltage_v\":%.1f,\"pack_current_a\":%.1f,\"pack_power_kw\":%.2f,"
    "\"usable_soc_pct\":%ld,\"soc_tenth_pct\":%ld,\"gids\":%ld,\"soh_pct\":%ld,"
    "\"full_cap_qc_wh\":%ld,\"remain_cap_qc_wh\":%ld,"
    "\"temp_max_c\":%ld,\"temp_avg_c\":%ld,\"temp_min_c\":%ld,\"battery_dtc\":%ld,"
    "\"max_discharge_kw\":%.2f,\"max_charge_kw\":%.2f,"
    "\"lb_failsafe_status\":%ld,\"lb_relay_cut_request\":%ld,\"lb_main_relay_on\":%ld,"
    "\"torque_nm\":%.1f,\"inverter_voltage_v\":%.0f,\"gear\":%ld,\"eco_on\":%ld,"
    "\"speed_kmh\":%.1f,\"charge_power_kw\":%.2f,\"target_soc_80\":%ld,\"vcm_awake\":%ld,"
    "\"car_state\":%ld,\"frames\":[",
    vehicle_name(vehicle_active()), (unsigned long)millis(), (unsigned long)ESP.getFreeHeap(),
    vehicle_name(vehicle_stored()), detected, car_write_safe() ? 1 : 0,
    (long)(t.last_rx_ms ? (int32_t)(millis() - t.last_rx_ms) : -1),
    (long)t.battery_bus, (unsigned long)t.last_battery_frame_ms,
    t.pack_voltage_v, t.pack_current_a, t.pack_power_kw,
    (long)t.usable_soc_pct, (long)t.soc_tenth_pct, (long)t.gids, (long)t.soh_pct,
    (long)t.full_cap_qc_wh, (long)t.remain_cap_qc_wh,
    (long)t.temp_max_c, (long)t.temp_avg_c, (long)t.temp_min_c, (long)t.battery_dtc,
    t.max_discharge_kw, t.max_charge_kw,
    (long)t.lb_failsafe_status, (long)t.lb_relay_cut_request, (long)t.lb_main_relay_on,
    t.torque_nm, t.inverter_voltage_v, (long)t.gear, (long)t.eco_on,
    t.speed_kmh, t.charge_power_kw, (long)t.target_soc_80, (long)t.vcm_awake,
    (long)t.car_state);

  bool first = true;
  const uint32_t now = millis();
  // 200-byte margin per entry: worst case a frame-monitor row is ~110 bytes,
  // so this guard never lets a write straddle the end of the buffer.
  for (int bus = 0; bus < 2 && n < (int)buflen - 200; bus++) {
    for (int i = 0; i < FRAME_MON_SLOTS && n < (int)buflen - 200; i++) {
      const FrameMonEntry &e = g_frame_mon[bus][i];
      if (!e.used) continue;
      float hz = (e.interval_ms > 0) ? (1000.0f / (float)e.interval_ms) : 0.0f;
      uint32_t age = now - e.last_seen_ms;
      n += snprintf(buf + n, buflen - n,
        "%s{\"bus\":\"%s\",\"id\":\"0x%03lX\",\"count\":%lu,\"hz\":%.1f,\"dlc\":%lu,"
        "\"data\":\"%02X%02X%02X%02X%02X%02X%02X%02X\",\"age_ms\":%lu}",
        first ? "" : ",", (bus == 0) ? "A" : "B", (unsigned long)e.id,
        (unsigned long)e.count, hz, (unsigned long)e.dlc,
        (uint8_t)(e.data_lo), (uint8_t)(e.data_lo >> 8), (uint8_t)(e.data_lo >> 16), (uint8_t)(e.data_lo >> 24),
        (uint8_t)(e.data_hi), (uint8_t)(e.data_hi >> 8), (uint8_t)(e.data_hi >> 16), (uint8_t)(e.data_hi >> 24),
        (unsigned long)age);
      first = false;
    }
  }
  n += snprintf(buf + n, (n < (int)buflen) ? buflen - n : 0, "]}");
}

// ── Battery-cells broadcast ──────────────────────────────────────────────────
// Independent of the 250 ms snapshot cadence above: sent only when a poll cycle
// actually produced a fresh group-2 (cells) or group-6 (shunts) result, which
// happens at most every few seconds (DIAG_POLL_INTERVAL_MS per group, 4 groups
// in rotation). Built in its own buffer so the 250 ms snapshot path never grows.
//
// "shunts" packing: 24 lowercase hex chars, 4 bits each = 96 bits, one per
// cell. Char at string index i encodes cells [4*i, 4*i+3]: bit b (0=LSB) of
// that hex digit is cell (4*i + b)'s shunt-active flag. Equivalently, cell n's
// bit lives at hex digit (n/4), bit (n%4) of that digit's value — this is
// simply g_leaf_diag.shunts[] (32 cells/word) re-sliced into nibbles.
static void build_cells_message(char *buf, size_t buflen) {
  const LeafDiag &ld = g_leaf_diag;
  int n = snprintf(buf, buflen, "{\"type\":\"cells\",\"gen\":%lu,\"mv\":[", (unsigned long)ld.cells_generation);
  for (int i = 0; i < 96 && n < (int)buflen; i++) {
    n += snprintf(buf + n, buflen - n, "%s%u", i ? "," : "", ld.cells_mv[i]);
  }
  n += snprintf(buf + n, (n < (int)buflen) ? buflen - n : 0, "],\"shunts\":\"");
  for (int i = 0; i < 24 && n < (int)buflen; i++) {
    int word = i / 8, nibble_pos = i % 8;
    uint8_t nib = (ld.shunts[word] >> (nibble_pos * 4)) & 0xF;
    n += snprintf(buf + n, buflen - n, "%x", nib);
  }
  n += snprintf(buf + n, (n < (int)buflen) ? buflen - n : 0, "\",\"temps\":[");
  for (int i = 0; i < 4 && n < (int)buflen; i++) {
    n += snprintf(buf + n, buflen - n, "%s%s", i ? "," : "",
                  (ld.temp_c[i] <= -1000) ? "null" : "");
    if (ld.temp_c[i] > -1000) n += snprintf(buf + n, buflen - n, "%ld", (long)ld.temp_c[i]);
  }
  const uint32_t now = millis();
  const uint32_t age_s = (ld.last_poll_ok_ms == 0) ? 0 : (now - ld.last_poll_ok_ms) / 1000;
  const int paused = ((int32_t)(now - ld.paused_until_ms) < 0) ? 1 : 0;
  n += snprintf(buf + n, (n < (int)buflen) ? buflen - n : 0,
    "],\"hx\":%.2f,\"insulation\":%lu,\"part\":\"%s\",\"serial\":\"%s\",\"bmsid\":\"%s\","
    "\"poll_age_s\":%lu,\"paused\":%d}",
    (double)ld.hx_pct, (unsigned long)ld.insulation_raw, ld.part_number, ld.serial, ld.bms_id,
    (unsigned long)age_s, paused);
}

static uint32_t g_last_client_report_ms = 0;

// Drain pending settings requests stashed by the AsyncTCP task. Main loop
// task ONLY — this is where NVS writes actually happen (Part B #3).
static void drain_pending_settings() {
  if (g_pending_profile >= 0) {
    vehicle_store((Vehicle)g_pending_profile);
    g_pending_profile = -1;
  }
  if (g_pending_appw_ready) {
    ap_password_store(g_pending_appw);
    g_pending_appw_ready = false;
  }
}

// Telemetry push — runs on the CAN-pump task (same task that WRITES telemetry/
// leaf_diag, so the multi-word arrays read by build_snapshot()/build_cells_
// message() can't tear). Reads only; no NVS, no reboot, no CAN TX. No-op until
// webui_begin() has run.
void webui_broadcast() {
  if (!g_web_started) return;

  g_dns.processNextRequest();  // cheap UDP poll; must run every pass, not at 250 ms

  const uint32_t now = millis();

  if (now - g_last_client_report_ms >= 5000) {
    g_last_client_report_ms = now;
    Serial.printf("[webui] wifi clients: %d, ws clients: %u\n",
                  WiFi.softAPgetStationNum(), (unsigned)ws.count());
  }

  if (ws.count() != 0) {
    static uint32_t last_sent_cells_gen = 0, last_sent_shunts_gen = 0;
    if (g_leaf_diag.cells_generation != last_sent_cells_gen ||
        g_leaf_diag.shunts_generation != last_sent_shunts_gen) {
      last_sent_cells_gen  = g_leaf_diag.cells_generation;
      last_sent_shunts_gen = g_leaf_diag.shunts_generation;
      static char cells_buf[1536];
      build_cells_message(cells_buf, sizeof(cells_buf));
      ws.textAll(cells_buf);
    }
  }

  if (now - g_last_broadcast_ms < 250) return;
  g_last_broadcast_ms = now;
  if (ws.count() == 0) return;  // skip building JSON when nobody's listening

  // Worst case: 2 buses x 64 slots x ~110 bytes/row + preamble ~= 15 KB.
  static char snapshot[16384];
  build_snapshot(snapshot, sizeof(snapshot));
  ws.textAll(snapshot);
  ws.cleanupClients();
}

// Housekeeping — runs on the Arduino loop task, NOT the CAN task. This is where
// the two flash-stalling / reset operations live, so they can never block CAN
// forwarding: NVS writes for pending profile/AP-password changes, and the
// deferred reboot. No-op until webui_begin() has run.
void webui_housekeeping() {
  if (!g_web_started) return;

  drain_pending_settings();

  if (g_reboot_request_ms && (int32_t)(millis() - g_reboot_request_ms) >= 0) {
    Serial.println("[webui] reboot requested from dashboard");
    ESP.restart();
  }
}

void webui_drain_tx() {
  if (!g_tx_queue) return;
  TxRequest req;
  while (xQueueReceive(g_tx_queue, &req, 0) == pdTRUE) {
    // Re-check the interlock HERE (main loop), not just at enqueue time on the
    // async task: the car could have started moving between queueing and now.
    // Belt-and-suspenders against the cross-task TOCTOU — drop stale writes.
    if (!car_write_safe()) continue;
    canbus_send(req.bus, req.frame);  // main loop task only — driver not thread-safe
  }
}
