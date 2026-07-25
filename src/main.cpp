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
#include "webui.h"
#include "vehicle_config.h"
#include "fw_version.h"

// Task watchdog (2 s, panic-reboot) is configured, the core-0 IDLE subscription
// dropped, and the CAN task subscribed — all in bridge_start_can_task() /
// can_task() (bridge.cpp). The dedicated CAN task is the ONLY watched task: a
// hung CAN task (e.g. a stuck SPI transaction) would silently drop the battery
// off the EV-CAN mid-drive, so it must reboot; the Arduino loop task and the
// web/WiFi core are intentionally NOT watched, so a web-side hang leaves a
// healthy bridge forwarding instead of rebooting it. 2 s is generous (dala's
// AVR uses 15 ms) — tighten at bench bring-up once real timing is measured.

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
  Serial.printf("[boot] CANBRIDGE firmware %s\n", FW_VERSION);
  selftest();
  bridge_begin();
  // Start CAN forwarding on its own watchdog-monitored task FIRST — from here
  // frames are pumped continuously regardless of what this task does next.
  bridge_start_can_task();
  // Now bring up WiFi/web. This blocks the loop task for ~1 s, but the CAN task
  // (higher priority, core 1) keeps forwarding throughout — so there is no
  // CAN-forwarding outage during AP bring-up, on boot or after a reboot.
  webui_begin();
}

void loop() {
  // The Arduino loop task now handles ONLY non-CAN, stall-tolerant work: serial
  // vehicle selection (rare NVS write) and web housekeeping (pending NVS
  // settings writes + deferred reboot). All CAN forwarding runs on the dedicated
  // task created in setup(); nothing here can block it. This task is not
  // watchdog-monitored on purpose (a hung web side must not reboot a healthy
  // bridge).
  vehicle_serial_task();
  webui_housekeeping();
  delay(5);  // yield to the CAN task, AsyncTCP, and IDLE (feeds the IDLE WDT)
}
