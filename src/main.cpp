// ─────────────────────────────────────────────────────────────────────────────
// Nissan LEAF / e-NV200 battery-upgrade CAN bridge — ESP32 (LilyGo T-2CANFD)
// Clean reimplementation of dalathegreat's battery-upgrade bridge.
//
// Layers:
//   board_pins.h  – verified T-2CANFD pin map
//   can_bus.*     – the only hardware-specific code (TWAI + MCP2518FD drivers)
//   bridge.*      – forwarding core + translate() hook
//   (later)       – leaf/env200 translation, Nissan CRC, web-config/OTA
// ─────────────────────────────────────────────────────────────────────────────
#include <Arduino.h>
#include "bridge.h"
#include "nissan_crc.h"
#include "leaf_5bc.h"

// One-time bench sanity check of the ported primitives (0x5BC pack/unpack + CRC-8).
static void selftest() {
  Leaf5BC s = {};
  s.LB_CAPR = 420;           // e.g. a 40 kWh pack's max GIDs
  s.LB_SOH  = 99;
  uint8_t d[8];
  leaf5bc_pack(s, d);
  const uint16_t capr = leaf5bc_unpack_capr(d);

  BridgeFrame f = {};
  f.id = 0x1DB; f.dlc = 8;
  f.data[0] = 0x12; f.data[1] = 0x34;
  calc_crc8(f);

  Serial.printf("[selftest] 5BC GID round-trip: %u (expect 420) %s | crc8=0x%02X\n",
                capr, (capr == 420) ? "OK" : "FAIL", f.data[7]);
}

void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  // Native-USB CDC: never block on writes when no host is attached — a stalled
  // print in the car (USB unplugged) must not delay CAN forwarding.
  Serial.setTxTimeoutMs(0);
#endif
  delay(300);
  selftest();
  bridge_begin();
}

void loop() {
  bridge_task();
}
