// ─────────────────────────────────────────────────────────────────────────────
// Home-WiFi station mode: an OPTIONAL, ADDITIONAL join to the user's own
// network, layered on top of (never instead of) the CANBRIDGE soft-AP that
// webui.cpp always brings up. WiFi.mode(WIFI_AP_STA) runs the AP and this
// station side concurrently — the soft-AP, its DNS captive portal, and both
// HTTP servers are untouched by anything in this file.
//
// NVS persistence, namespace "bridge" key prefix "wifi" — same Preferences
// namespace/style as ap_config.cpp. Empty/absent SSID = station mode disabled
// (the device stays AP-only, same as before this feature existed).
//
// SAFETY: like ap_password_store(), the underlying NVS write can stall on
// flash and must only ever run on the Arduino loop task, never the AsyncTCP
// task. Unlike the AP password, a new SSID/password takes effect immediately
// (no reboot needed) — wifi_client_store() restarts the join in place.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

// Load persisted credentials and, if an SSID is configured, switch WiFi to
// AP+STA and start joining. Call once from webui_begin(), AFTER the soft-AP
// (WiFi.softAP/softAPConfig) has already been set up — WiFi.mode(WIFI_AP_STA)
// here must never be the first WiFi call, or the AP config could be lost.
void wifi_client_begin();

// Persist a new SSID/password pair and (re)start the join immediately — no
// reboot required. Empty ssid disables station mode and forgets the stored
// credentials (AP mode only, same as before this feature existed). Validates
// ssid 0..32 chars and password empty or 8..63 chars; no-op (returns false)
// otherwise. Main-loop task ONLY — the underlying NVS write can stall.
bool wifi_client_store(const char *ssid, const char *password);

// Poll join progress once per second and print status TRANSITIONS to Serial
// ("joining -> no_ap (reason 201)" etc.), so a failed join is diagnosable from
// the serial log. Call every pass from webui_housekeeping() (Arduino loop
// task); internally rate-limited.
void wifi_client_tick();

// Coarse join status token for the JSON snapshot — fixed strings only, never
// free text (the snapshot embeds it unescaped): "off" (disabled), "up"
// (joined), "no_ap" (network not found: wrong name, 5 GHz-only, or out of
// range), "auth" (credentials rejected — usually a wrong password), "joining"
// (still trying / no verdict yet).
const char *wifi_client_status();

// True once the station interface has actually joined the configured network
// and obtained an IP.
bool wifi_client_up();

// Station IP as a string ("" when not connected). Pointer to a static buffer,
// valid for the process lifetime.
const char *wifi_client_ip();

// The currently configured SSID ("" when station mode is disabled). Pointer
// to a static buffer, valid for the process lifetime.
const char *wifi_client_ssid();

// Kick off an async nearby-network scan (WiFi.scanNetworks(true)). Returns
// immediately; completion is picked up by wifi_client_tick(). Main-loop task
// ONLY — matches the rest of this file's WiFi-call discipline, and lets the
// caller (webui.cpp) apply its usual AsyncTCP-vs-loop-task handoff for the
// {"cmd":"wifiscan"} request that triggers this.
void wifi_client_scan_start();

// True exactly once, on the first wifi_client_tick() call after a scan
// finishes — the caller is expected to send the ready JSON (see
// wifi_client_scan_json()) right then and only then; the flag does not stay
// set waiting for a slow consumer. False the rest of the time, including
// while a scan is still running or none was ever started.
bool wifi_client_scan_ready();

// The scan result, pre-built as a complete WS text message
// ({"type":"wifiscan","nets":[...]}) by wifi_client_tick() when the scan
// completed. Pointer to a static buffer, valid for the process lifetime;
// only meaningful right after wifi_client_scan_ready() returned true.
const char *wifi_client_scan_json();
