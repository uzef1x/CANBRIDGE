# Nissan LEAF / e-NV200 battery-upgrade CAN bridge (ESP32)

A clean ESP32 reimplementation of dalathegreat's battery-upgrade CAN bridge — the
job the Muxsan 3-port and STM32 2-port bridges do — for **both** the Nissan LEAF and
the e-NV200, in one binary, selectable at runtime. It sits between the car's VCM and
a bigger/newer battery, translating the pack's messages so the older car accepts it.

**This is a port of [dalathegreat](https://github.com/dalathegreat)'s GPL-3.0
battery-upgrade CAN bridges** — see [Credits & attribution](#credits--attribution).
All of the CAN translation logic comes from dala's work, with per-line source
references in the code comments; none of the underlying reverse-engineering
originates here.

## Credits & attribution

This project **ports / adapts the work of [dalathegreat](https://github.com/dalathegreat)**
(and contributors) to the ESP32. The hard part — reverse-engineering the Nissan battery
CAN protocol (message IDs, byte layouts, bit masks, checksums, GID/capacity spoofing,
DTC-clearing frames, generation/battery auto-detection) — is **theirs**, not ours. This
repo only re-implements that logic on new hardware.

Source material (© their respective authors):
- **Nissan LEAF Battery Upgrade** — https://github.com/dalathegreat/Nissan-LEAF-Battery-Upgrade (**GPL-3.0**). The LEAF translation here is ported from its `Software/CANBRIDGE-3port/leaf-can-bridge-3-port` firmware.
- **Nissan e-NV200 Battery Upgrade** — https://github.com/dalathegreat/Nissan-env200-Battery-Upgrade (**GPL-3.0**). The e-NV200 translation is ported from its `leaf-can-bridge-3-port-env200` firmware.
- **leaf_can_bus_messages** — https://github.com/dalathegreat/leaf_can_bus_messages (**GPL-3.0**) — CAN signal (DBC) definitions used as reference.
- **EV-CANlogs** — https://github.com/dalathegreat/EV-CANlogs — real captured CAN logs used for reference/validation.
- dala's bridges were developed together with **Muxsan**'s 3-port CAN-bridge hardware.

Also referenced / thanks to:
- **NismoBoy34 / Esp32LeafInverterBridge** — the prior ESP32 attempt that prompted this project. Its battery-upgrade translation was never implemented, so the logic here is re-ported directly from dala rather than taken from it.
- **LilyGo T-2CANFD** board pinout — from [Xinyuan-LilyGO/T-2Can](https://github.com/Xinyuan-LilyGO/T-2Can).
- **ACAN2517FD** MCP2518FD driver library by Pierre Molinaro.

The code comments carry per-handler source line references (e.g. `// L523-736`) so every
value can be traced back to dala's original file and line.

## ⚠️ Safety
This controls a real vehicle. It has been **compile-verified** and the translation was
**independently cross-checked against dala's source**, but it has **not been tested on
hardware or in a car**. First bring-up must be on a bench (CAN analyzer / replayed
logs), then in-car with the ability to revert to a plain pass-through.

## Hardware — LilyGo T-2CANFD (ESP32-S3)

The board has **two galvanically-isolated CAN ports**. The bridge is installed **inline
in the Nissan EV-CAN bus, between the vehicle and the battery** (dala: *"mounted between
the battery and vehicle EV-CAN system"*). You cut the EV-CAN bus and connect one segment
to each port. Both segments are classic **CAN 2.0B @ 500 kbit/s**.

### Wiring diagram

```text
        Nissan EV-CAN bus (classic CAN 2.0B, 500 kbit/s)
        ── cut the bus and insert the bridge inline ──

   VEHICLE segment                                  BATTERY segment
   (VCM, cluster, charger)                          (new pack: LBC / BMS)
          │  │                                              │  │
      CANH│  │CANL                                      CANH│  │CANL
          │  │                                              │  │
   ┌──────┴──┴──────────────  LilyGo T-2CANFD  ─────────────┴──┴──────┐
   │                                                                  │
   │   ┌────────────────────┐              ┌───────────────────────┐  │
   │   │ CAN-B port (TWAI)  │   ESP32-S3   │ CAN-A port (MCP2518FD)│  │
   │   │ isolated           │              │ isolated, 20 MHz      │  │
   │   └────────────────────┘              └───────────────────────┘  │
   │                                                                  │
   └───────────────────────────────┬──────────────────────────────────┘
                                    │
                 VIN 12–24 V (+) ───┤   ← car switched 12 V accessory
                 GND (−)         ───┘     (USB-C 5 V = bench/flashing only)
```

- Connect **each EV-CAN segment to one port**: CANH→CANH, CANL→CANL.
- It does **not** matter which port faces the vehicle vs the battery — the firmware
  **auto-detects the battery side** (from 0x1DB on LEAF / 0x55B on e-NV200) and is
  otherwise a symmetric bidirectional bridge.
- Read the **CAN-A / CAN-B and CANH / CANL / VIN / GND positions off the board's
  silkscreen** — don't assume terminal order.
- Power from the car's **switched 12 V** on `VIN` (board accepts 12–24 V). Use USB-C 5 V
  only on the bench.

### On-board pinout (reference — already routed on the PCB; you do NOT wire these)

| Function | ESP32-S3 GPIO |
|---|---|
| CAN-B (TWAI) TX / RX | GPIO7 / GPIO6 |
| CAN-A (MCP2518FD) chip-select | GPIO10 |
| CAN-A SPI SCK / MOSI / MISO | GPIO12 / GPIO11 / GPIO13 |
| CAN-A interrupt | GPIO8 |
| MCP2518FD oscillator | 20 MHz |

Verified against `Xinyuan-LilyGO/T-2Can → libraries/private_library/pin_config.h`.

> ⚠️ **The vehicle-side tap is car-specific and safety-critical.** *Where* to cut/tap the
> EV-CAN — which harness connector and which wires — depends on the model and is
> documented in dalathegreat's install guides and videos, **not here** (this README does
> not invent Nissan pinouts). See the
> [e-NV200](https://github.com/dalathegreat/Nissan-env200-Battery-Upgrade) and
> [LEAF](https://github.com/dalathegreat/Nissan-LEAF-Battery-Upgrade) install docs.
> Mis-wiring the EV-CAN can immobilise the car.

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

## License

This project is a derivative work of dalathegreat's **GPL-3.0** battery-upgrade bridges,
so it is released under the **GNU General Public License v3.0** — see [LICENSE](LICENSE).
If you use, modify, or distribute it, you must preserve the attribution above and keep it
GPL-3.0. This is required by dala's license and is the right thing to do — the credit for
this work belongs with them.
