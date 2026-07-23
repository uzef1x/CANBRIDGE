#include "bridge.h"
#include "can_bus.h"
#include "vehicle_config.h"
#include "leaf_translation.h"
#include "env200_translation.h"
#include "telemetry.h"
#include "leaf_diag.h"
#include "webui.h"
#include "ap_config.h"
#include <Arduino.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ── Translation hook ─────────────────────────────────────────────────────────
// Called for every frame received on `from`, before it is forwarded to the other
// bus. Return true to forward (optionally after modifying `f`), false to drop.
static bool translate(BridgeBus from, BridgeFrame &f) {
  switch (vehicle_active()) {
    case VEHICLE_LEAF:   return leaf_translate(from, f);
    case VEHICLE_ENV200: return env200_translate(from, f);
  }
  return true;
}

// ── Heartbeat counters ───────────────────────────────────────────────────────
static uint32_t g_a_to_b = 0;
static uint32_t g_b_to_a = 0;
static uint32_t g_last_report_ms = 0;

static void pump(BridgeBus from, BridgeBus to, uint32_t &counter) {
  BridgeFrame f;
  while (canbus_receive(from, f)) {
    telemetry_capture(from, f);  // read-only tap, before translate()
    leaf_diag_capture(from, f);  // LeafSpy-style diag tap; may TX flow control directly
    if (translate(from, f)) {
      canbus_send(to, f);
      counter++;
    }
  }
}

void bridge_begin() {
  Serial.println("[bridge] Nissan LEAF / e-NV200 battery-upgrade bridge");
  telemetry_begin();
  leaf_diag_begin();
  vehicle_begin();
  ap_config_begin();  // load persisted AP password before webui_begin() uses it
  if (canbus_begin()) Serial.println("[bridge] CAN-A + CAN-B @ 500k classic: OK");
  else                Serial.println("[bridge] CAN init FAILED (check board/wiring)");
}

// ── Dedicated CAN-pump task ──────────────────────────────────────────────────
// This task is the ONLY code that calls canbus_send()/canbus_receive() and the
// ONLY writer of telemetry / leaf_diag / frame-monitor state. It is pinned to
// core 1 at a priority above the Arduino loop task and the async web stack, so
// nothing on the web side (the ~1 s WiFi bring-up, NVS flash writes, JSON
// serialization) can stall CAN forwarding — the whole reason this task exists.
//
// It also runs webui_broadcast() (DNS poll + WebSocket telemetry push): that
// code READS telemetry/leaf_diag arrays, so it is deliberately kept on THIS
// task (same task as the writer) to avoid cross-task tearing of the multi-word
// arrays (cells_mv[], shunts[], frame-monitor rows). NVS writes and reboot are
// NOT done here (they belong to the loop task, where a flash-write stall is
// harmless) — see webui_housekeeping() called from loop().
//
// The one thing this task must not do is starve the core: it yields one tick
// (~1 ms at the 1000 Hz FreeRTOS tick) each pass. CAN frames arrive no faster
// than every 10 ms and the controllers' RX FIFOs (TWAI 64-deep) buffer several
// ms of traffic, so a 1 ms poll never overflows and adds at most ~1 ms of
// forwarding latency.
static constexpr uint32_t WDT_TIMEOUT_S = 2;
static TaskHandle_t g_can_task = nullptr;

static void can_task(void *) {
  esp_task_wdt_add(NULL);  // watch THIS task (a stuck SPI/TWAI TX -> reboot)
  for (;;) {
    if (vehicle_active() == VEHICLE_LEAF) leaf_tick();

    pump(BUS_A, BUS_B, g_a_to_b);
    pump(BUS_B, BUS_A, g_b_to_a);

    webui_drain_tx();    // web-originated CAN TX (re-checks car_write_safe inside)
    leaf_diag_task();    // LeafSpy-style diag poll state machine
    webui_broadcast();   // DNS poll + WS telemetry push (reads telemetry; no-op until web up)

    const uint32_t now = millis();
    if (now - g_last_report_ms >= 2000) {
      g_last_report_ms = now;
      Serial.printf("[bridge] A->B: %lu   B->A: %lu\n",
                    (unsigned long)g_a_to_b, (unsigned long)g_b_to_a);
    }

    esp_task_wdt_reset();
    vTaskDelay(1);  // yield ~1 ms: bounds latency, feeds IDLE, never starves web
  }
}

void bridge_start_can_task() {
  esp_task_wdt_init(WDT_TIMEOUT_S, true);  // reconfigure the global TWDT (idempotent)

  // The IDF/Arduino boot already subscribes the core-0 IDLE task to the TWDT.
  // Leaving it subscribed means a busy/wedged WiFi/AsyncTCP core (all on core 0)
  // would panic-reboot the bridge — dropping the battery off the EV-CAN — even
  // when CAN forwarding on core 1 is perfectly healthy. That is the opposite of
  // what we want: a web-side problem must NOT reboot a working bridge. So drop
  // the core-0 IDLE subscription; the CAN task (added below, via
  // esp_task_wdt_add in can_task) becomes the ONLY watched task. (Residual: the
  // CAN task's own webui_broadcast() briefly takes lwip/WebSocket locks, so a
  // true lwip deadlock could still stall the CAN task and trip ITS 2 s watchdog
  // — a bounded, controlled reboot, not a silent hang.)
  disableCore0WDT();

  // Core 1 (APP_CPU, same core as the Arduino loop); WiFi/lwip live on core 0.
  // Priority 11 is ONE above the AsyncTCP task (CONFIG_ASYNC_TCP_PRIORITY = 10,
  // no core affinity) so CAN genuinely wins latency races on core 1, while the
  // per-pass vTaskDelay(1) still guarantees the loop task (prio 1) and IDLE get
  // CPU. 12 KB stack covers the JSON snprintf path (its big buffers are static).
  xTaskCreatePinnedToCore(can_task, "can_pump", 12288, nullptr, 11, &g_can_task, 1);
}
