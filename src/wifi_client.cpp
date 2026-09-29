#include "wifi_client.h"
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <string.h>

static Preferences prefs;
static char g_ssid[33] = "";  // 32 chars max + '\0', empty = station disabled
static char g_pass[64] = "";  // empty = open network
static char g_ip[16]   = "";  // dotted-quad, filled lazily by wifi_client_ip()

// Nearby-network scan state. Both fields are written only from
// wifi_client_scan_start()/wifi_client_tick(), which per this file's contract
// only ever run on the Arduino loop task — no cross-task synchronization
// needed here, unlike g_last_reason above (which the WiFi event task writes).
static bool g_scan_pending = false;   // scanNetworks(true) issued, awaiting scanComplete()
static bool g_scan_ready   = false;   // true for exactly one wifi_client_tick() call after completion
static char g_scan_json[2560];        // pre-built {"type":"wifiscan",...} WS message

// Last STA disconnect reason (esp_wifi_types.h wifi_err_reason_t), written by
// the WiFi event task, read by wifi_client_status()/tick() on the loop task.
// Aligned 32-bit scalar single-store — same accepted cross-task pattern as
// telemetry.h's scalars. 0 = no disconnect seen since the last join attempt.
static volatile int32_t g_last_reason = 0;

// (Re)start the station join from the current g_ssid/g_pass, or drop back to
// AP-only mode if station is disabled. Never touches the soft-AP: switching
// between WIFI_AP and WIFI_AP_STA only adds/removes the station side, and the
// AP is only ever configured once, earlier, in webui_begin().
static void start_join() {
  if (g_ssid[0] == '\0') {
    WiFi.disconnect(true /* wifioff */, false /* don't erase saved creds — we own persistence */);
    WiFi.mode(WIFI_AP);  // AP-only: no station means nothing to (re)join
    g_ip[0] = '\0';
    return;
  }
  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(true);  // let the Arduino core rejoin on its own after a drop
  g_last_reason = 0;            // fresh attempt: forget the previous verdict
  if (g_pass[0] != '\0') WiFi.begin(g_ssid, g_pass);
  else                   WiFi.begin(g_ssid);  // open network
  // CRITICAL: enabling the station re-enables ESP32 modem power-save by default,
  // which dozes the radio between DTIM beacons -> high latency + heavy packet
  // loss to the bridge (measured ~1.2s RTT / 80% loss on a phone hotspot, which
  // buffers for sleeping clients far worse than a router). webui_begin()'s
  // setSleep(false) was applied while AP-only and does NOT survive the switch to
  // AP_STA, so re-assert it here, after begin(), for a responsive station link.
  WiFi.setSleep(false);
}

void wifi_client_begin() {
  // Capture WHY a join attempt fails — WiFi.status() alone can't distinguish
  // "network not found" from "wrong password". Runs on the WiFi event task;
  // writes only the one volatile scalar above (see its comment).
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    g_last_reason = (int32_t)info.wifi_sta_disconnected.reason;
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  prefs.begin("bridge", false);
  // isKey() avoids the noisy NVS "NOT_FOUND" error log on a fresh device that
  // has never had home-WiFi credentials set.
  String stored_ssid = prefs.isKey("wssid") ? prefs.getString("wssid", "") : String("");
  String stored_pass = prefs.isKey("wpass") ? prefs.getString("wpass", "") : String("");
  strncpy(g_ssid, stored_ssid.c_str(), sizeof(g_ssid) - 1);
  g_ssid[sizeof(g_ssid) - 1] = '\0';
  strncpy(g_pass, stored_pass.c_str(), sizeof(g_pass) - 1);
  g_pass[sizeof(g_pass) - 1] = '\0';

  if (g_ssid[0] != '\0') {
    Serial.printf("[wifi_client] joining home WiFi '%s'...\n", g_ssid);
    start_join();
  }
  // else: station stays off, WiFi.mode() left as WIFI_AP (set by webui_begin() above us)
}

bool wifi_client_store(const char *ssid, const char *password) {
  size_t slen = ssid ? strlen(ssid) : 0;
  size_t plen = password ? strlen(password) : 0;
  if (slen > 32) return false;
  if (plen != 0 && (plen < 8 || plen > 63)) return false;

  prefs.putString("wssid", ssid ? ssid : "");
  prefs.putString("wpass", password ? password : "");

  strncpy(g_ssid, ssid ? ssid : "", sizeof(g_ssid) - 1);
  g_ssid[sizeof(g_ssid) - 1] = '\0';
  strncpy(g_pass, password ? password : "", sizeof(g_pass) - 1);
  g_pass[sizeof(g_pass) - 1] = '\0';

  Serial.println(g_ssid[0] ? "[wifi_client] home WiFi credentials updated, joining now"
                            : "[wifi_client] home WiFi disabled");
  start_join();
  return true;
}

bool wifi_client_up() {
  if (g_ssid[0] == '\0') return false;
  return WiFi.status() == WL_CONNECTED;
}

const char *wifi_client_status() {
  if (g_ssid[0] == '\0') return "off";
  if (WiFi.status() == WL_CONNECTED) return "up";
  // wifi_err_reason_t: 201 = NO_AP_FOUND. Credential rejections show up as
  // AUTH_EXPIRE(2), 4WAY_HANDSHAKE_TIMEOUT(15), AUTH_FAIL(202) or
  // HANDSHAKE_TIMEOUT(204) depending on AP/timing (names from
  // esp_wifi_types.h). Anything else: no verdict yet, keep "joining".
  switch (g_last_reason) {
    case 201:                              return "no_ap";
    case 2: case 15: case 202: case 204:   return "auth";
    default:                               return "joining";
  }
}

// Append `s`, JSON-escaped, to buf[0..n) (capacity buflen). SSIDs come off
// the air from whatever's broadcasting nearby — attacker-controlled input —
// so unlike wifi_client_store()'s reject-on-bad-char validation (which is
// fine for a value we're about to persist to NVS and use verbatim as a
// station-join identifier), here we can't just refuse to show a network that
// exists. This is a read-only display field broadcast over the dashboard WS,
// so instead of rejecting we escape: quotes, backslashes, and any control
// byte become safe JSON. Bounds-checked per byte, not just via snprintf's
// return value, since a crafted SSID could otherwise be used to probe for a
// buffer-overflow bug in this path specifically.
static int append_json_escaped(char *buf, int n, size_t buflen, const char *s) {
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if (n >= (int)buflen - 8) break;  // room for the longest single escape (\u00XX)
    if (*p == '"' || *p == '\\') {
      buf[n++] = '\\';
      buf[n++] = (char)*p;
    } else if (*p < 0x20) {
      n += snprintf(buf + n, buflen - n, "\\u%04x", *p);
    } else {
      buf[n++] = (char)*p;
    }
  }
  return n;
}

// Build the {"type":"wifiscan","nets":[...]} message into g_scan_json from
// the `count` results WiFi.scanNetworks() just produced, then free the core's
// scan-result table. Called once, right after scanComplete() reports done —
// never re-entered mid-build, so the dedupe/sort table below can be a plain
// local (small: capped at MAX_CANDIDATES entries, well under loop-task stack).
static void build_scan_json(int16_t count) {
  // ssid[] is a fixed buffer, not a String/const char* into one — WiFi.SSID(i)
  // returns a temporary String, and holding onto its c_str() past the
  // statement that created it would be a dangling pointer.
  struct Candidate { char ssid[33]; int32_t rssi; bool secure; };
  static const int MAX_CANDIDATES = 40;
  Candidate cand[MAX_CANDIDATES];
  int n_cand = 0;

  // Dedupe by SSID text, keeping the strongest RSSI sighting of each. O(n^2)
  // over at most `count` (core-capped, typically well under 100) results —
  // this runs once per scan, not per frame, so that's fine.
  for (int16_t i = 0; i < count && n_cand < MAX_CANDIDATES; i++) {
    String ssid_s = WiFi.SSID(i);
    if (ssid_s.length() == 0) continue;  // hidden network: skip, manual entry covers it
    int32_t rssi = WiFi.RSSI(i);
    bool secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    int existing = -1;
    for (int j = 0; j < n_cand; j++) {
      if (strcmp(cand[j].ssid, ssid_s.c_str()) == 0) { existing = j; break; }
    }
    if (existing >= 0) {
      if (rssi > cand[existing].rssi) { cand[existing].rssi = rssi; cand[existing].secure = secure; }
    } else {
      strncpy(cand[n_cand].ssid, ssid_s.c_str(), sizeof(cand[n_cand].ssid) - 1);
      cand[n_cand].ssid[sizeof(cand[n_cand].ssid) - 1] = '\0';
      cand[n_cand].rssi = rssi;
      cand[n_cand].secure = secure;
      n_cand++;
    }
  }

  // Strongest-first, simple insertion sort (n_cand <= MAX_CANDIDATES, small).
  for (int i = 1; i < n_cand; i++) {
    Candidate key = cand[i];
    int j = i - 1;
    while (j >= 0 && cand[j].rssi < key.rssi) { cand[j + 1] = cand[j]; j--; }
    cand[j + 1] = key;
  }

  int shown = (n_cand < 20) ? n_cand : 20;  // cap at 20 entries
  int n = snprintf(g_scan_json, sizeof(g_scan_json), "{\"type\":\"wifiscan\",\"nets\":[");
  // 300-byte reserve = one worst-case entry (32-char SSID fully \u-escaped
  // ≈ 225 bytes) plus the closing "]}" — guarantees we only ever emit whole
  // entries, so the message can be truncated in COUNT but never be malformed.
  for (int i = 0; i < shown && n < (int)sizeof(g_scan_json) - 300; i++) {
    n += snprintf(g_scan_json + n, sizeof(g_scan_json) - n, "%s{\"ssid\":\"",
                  i ? "," : "");
    n = append_json_escaped(g_scan_json, n, sizeof(g_scan_json), cand[i].ssid);
    n += snprintf(g_scan_json + n, sizeof(g_scan_json) - n, "\",\"rssi\":%ld,\"sec\":%d}",
                  (long)cand[i].rssi, cand[i].secure ? 1 : 0);
  }
  n += snprintf(g_scan_json + n, (n < (int)sizeof(g_scan_json)) ? sizeof(g_scan_json) - n : 0, "]}");

  WiFi.scanDelete();  // free the core's internal result table now that we've copied out of it
}

void wifi_client_scan_start() {
  if (g_scan_pending) return;  // a scan is already running; let it finish rather than restarting it
  // scanNetworks(true) calls WiFi.enableSTA(true) internally, which ORs
  // WIFI_MODE_STA onto whatever mode is already set (AP-only or AP_STA) — it
  // never clears the AP bit, so the soft-AP is unaffected whether station
  // mode is currently disabled or already joined to home WiFi. No explicit
  // WiFi.mode() call needed here; verified against WiFiGenericClass::
  // enableSTA()/mode() in the Arduino core before relying on it. This also
  // does NOT start a station join — scanning and joining are independent,
  // and no join is configured unless the user has already saved credentials.
  WiFi.scanNetworks(true /* async */);
  g_scan_pending = true;
  g_scan_ready = false;
}

bool wifi_client_scan_ready() {
  bool r = g_scan_ready;
  g_scan_ready = false;  // one-shot: the caller is expected to send it now
  return r;
}

const char *wifi_client_scan_json() { return g_scan_json; }

void wifi_client_tick() {
  static uint32_t last_ms = 0;
  static const char *last_status = "";
  uint32_t now = millis();
  if (now - last_ms < 1000) return;
  last_ms = now;
  const char *s = wifi_client_status();
  if (s != last_status) {  // pointer compare is enough: fixed string literals
    if (strcmp(s, "up") == 0)
      Serial.printf("[wifi_client] joined '%s', IP %s\n", g_ssid, wifi_client_ip());
    else
      Serial.printf("[wifi_client] status: %s -> %s (last disconnect reason %ld)\n",
                    last_status, s, (long)g_last_reason);
    last_status = s;
  }

  if (g_scan_pending) {
    int16_t result = WiFi.scanComplete();  // >=0 done (count), WIFI_SCAN_RUNNING(-1) still going, WIFI_SCAN_FAILED(-2) gave up
    if (result != WIFI_SCAN_RUNNING) {
      build_scan_json((result >= 0) ? result : 0);
      g_scan_pending = false;
      g_scan_ready = true;
    }
  }
}

const char *wifi_client_ip() {
  if (wifi_client_up()) {
    snprintf(g_ip, sizeof(g_ip), "%s", WiFi.localIP().toString().c_str());
  } else {
    g_ip[0] = '\0';
  }
  return g_ip;
}

const char *wifi_client_ssid() { return g_ssid; }
