// ─────────────────────────────────────────────────────────────────────────────
// WiFi dashboard: soft-AP + async web server/WebSocket for live telemetry,
// custom CAN transmit, and the settings menu (profile, AP password, reboot).
// All CAN TX and all state changes (NVS writes, ESP.restart()) requested from
// the web side are deferred off the AsyncTCP task: CAN TX is drained by
// webui_drain_tx() on the CAN-pump task; NVS writes + reboot by
// webui_housekeeping() on the Arduino loop task. canbus_send(), NVS writes, and
// ESP.restart() are never called from a web/AsyncTCP callback.
// Custom TX, reboot, and settings changes are additionally gated by
// car_write_safe() (telemetry.h): refused while the car is confirmed moving.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

// Start the soft-AP + web server. Called once from setup(), AFTER
// bridge_start_can_task() so CAN forwarding is already running on its own task
// and this ~1 s blocking bring-up cannot stall it. Not idempotent — call once.
void webui_begin();

// Telemetry push: DNS poll + rate-limited (250 ms) WebSocket broadcast. Runs on
// the Arduino loop task (NOT the CAN task) — that task is the only one watched
// by the panic watchdog, and this code takes lwip/WebSocket locks that could
// stall >2 s, which would otherwise panic-reboot the bridge mid-drive. Reads
// (never writes) telemetry/leaf_diag state; the multi-word arrays it reads
// (frame-monitor rows, cells_mv[], shunts[]) are written by the CAN task
// without a lock, so a rare torn read is possible now that reader and writer
// are on different tasks — display-only data, never consulted by
// car_write_safe()/can_tx_safe() (see telemetry.h), so this is an accepted
// trade-off. Never writes NVS / reboots / sends CAN.
void webui_broadcast();

// Loop-task housekeeping: apply pending NVS settings writes and any deferred
// reboot. Runs on the Arduino loop task (NOT the CAN task) so a flash-write
// stall can't block CAN forwarding.
void webui_housekeeping();

// Drain queued web-originated CAN TX requests onto the real buses. Runs on the
// CAN-pump task (the only caller of canbus_send). Re-checks car_write_safe().
void webui_drain_tx();
