#include "bridge.h"
#include "can_bus.h"
#include "vehicle_config.h"
#include "leaf_translation.h"
#include "env200_translation.h"
#include <Arduino.h>

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
    if (translate(from, f)) {
      canbus_send(to, f);
      counter++;
    }
  }
}

void bridge_begin() {
  Serial.println("[bridge] Nissan LEAF / e-NV200 battery-upgrade bridge");
  vehicle_begin();
  if (canbus_begin()) Serial.println("[bridge] CAN-A + CAN-B @ 500k classic: OK");
  else                Serial.println("[bridge] CAN init FAILED (check board/wiring)");
}

void bridge_task() {
  vehicle_serial_task();
  if (vehicle_active() == VEHICLE_LEAF) leaf_tick();

  pump(BUS_A, BUS_B, g_a_to_b);
  pump(BUS_B, BUS_A, g_b_to_a);

  const uint32_t now = millis();
  if (now - g_last_report_ms >= 2000) {
    g_last_report_ms = now;
    Serial.printf("[bridge] A->B: %lu   B->A: %lu\n",
                  (unsigned long)g_a_to_b, (unsigned long)g_b_to_a);
  }
}
