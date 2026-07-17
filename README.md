# Nissan LEAF / e-NV200 battery-upgrade CAN bridge (ESP32)

A clean ESP32 reimplementation of dalathegreat's battery-upgrade CAN bridge — the
job the Muxsan 3-port and STM32 2-port bridges do — for **both** the Nissan LEAF and
the e-NV200, in one binary, selectable at runtime. It sits between the car's VCM and
a bigger/newer battery, translating the pack's messages so the older car accepts it.

**All translation logic is ported from dalathegreat's proven sources**, with source
line references in the code comments. Nothing is invented. It is **not** derived from
NismoBoy34's ESP32 repo (whose battery-upgrade translation was never implemented).

## ⚠️ Safety
This controls a real vehicle. It has been **compile-verified** and the translation was
**independently cross-checked against dala's source**, but it has **not been tested on
hardware or in a car**. First bring-up must be on a bench (CAN analyzer / replayed
logs), then in-car with the ability to revert to a plain pass-through.

## Hardware — LilyGo T-2CANFD (ESP32-S3)
- **CAN-B** = native ESP32-S3 TWAI (TX=GPIO7, RX=GPIO6)
- **CAN-A** = MCP2518FD over SPI (CS=10, SCLK=12, MOSI=11, MISO=13, INT=8), 20 MHz osc
- Both Nissan segments: **classic CAN 2.0B @ 500 kbit/s**
- Wire one bus to the vehicle (VCM) side, the other to the battery (LBC) side. The
  battery side is auto-detected (from 0x1DB on LEAF / 0x55B on e-NV200); direction is
  otherwise symmetric.

Pins verified against `Xinyuan-LilyGO/T-2Can → libraries/private_library/pin_config.h`.

## Selecting the vehicle
Choice is stored in NVS and read once at boot (never swapped mid-run). Over the serial
monitor (115200) send `leaf` or `env200`, then reboot. Default is LEAF.

## Code layout
```
include/board_pins.h      verified T-2CANFD pin map
src/can_frame.h           neutral BridgeFrame + bus identity
src/can_bus.{h,cpp}       the ONLY hardware-specific layer (TWAI + MCP2518FD)
src/bridge.{h,cpp}        forwarding core + translate() dispatch + block via return false
src/vehicle_config.{h,cpp} runtime LEAF/e-NV200 selection (NVS-persisted)
src/nissan_crc.{h,cpp}    CRC-8 (0x85) + calc_sum2 / calc_checksum4
src/leaf_5bc.h            0x5BC model + pack/unpack
src/leaf_5c0.h            0x5C0 temperature model + pack
src/leaf_translation.{h,cpp}    full LEAF battery-upgrade translation
src/env200_translation.{h,cpp}  e-NV200 (24→40 kWh) translation
src/main.cpp              setup() / loop() + boot self-test
```
Design: all battery-upgrade logic lives behind `translate()`; the driver and
forwarding code never change as features are added. Generated frames are injected onto
the auto-detected battery bus; the block list drops frames via `return false`.

## Architecture faithfulness (vs dala 3-port)
- dala forwards a received frame to the *other* bus unless blocked → same here.
- dala injects generated frames to `battery_can_bus` → here via `canbus_send(battery_bus,…)`.
- dala's 3rd port is `#define DISABLE_CAN3` by default → a 2-bus bridge matches the
  default configuration.
- dala's software TX FIFO works around an MCP25625 erratum; the MCP2518FD/TWAI drivers
  have their own TX queues and no such erratum.

## Build / flash (PlatformIO)
```
pio run -e lilygo-t2canfd              # compile
pio run -e lilygo-t2canfd -t upload    # flash over USB-C
pio device monitor -b 115200           # boot log, self-test, A->B / B->A counters
```
Plain Arduino C++ — also builds in the Arduino IDE (ESP32-S3 board + ACAN2517FD lib).
