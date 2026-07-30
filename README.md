# Nissan LEAF / e-NV200 battery-upgrade CAN bridge (ESP32)

## ⚠️ Disclaimer — use entirely at your own risk

**Everything you do with this project is on you.** By downloading, building, flashing,
installing, wiring, or using anything in this repository — software, documentation,
wiring information, or images — you accept **full and sole responsibility** for the
consequences. The author and contributors accept **no responsibility and no liability
whatsoever** for any damage, loss, cost, or injury of any kind, including but not
limited to: damage to your vehicle, its battery, its electronics, or other property;
a vehicle that will not drive, charge, or start; stored fault codes; fire; personal
injury; or loss of warranty, insurance cover, or road-worthiness/type approval —
whether arising from the software, the documentation, your installation, or errors
in any of them.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
PARTICULAR PURPOSE, AND NONINFRINGEMENT. Sections 15 ("Disclaimer of Warranty") and
16 ("Limitation of Liability") of the [GPL-3.0 license](LICENSE) apply to this entire
project. If you do not agree, do not use it.

## About

A clean ESP32 reimplementation of dalathegreat's battery-upgrade CAN bridge — the
same job the Muxsan 3-port and STM32 2-port bridges do. It supports both the Nissan
LEAF and the e-NV200 in one binary, with the vehicle selected at runtime. It sits
between the car's VCM and a bigger/newer battery, translating the pack's messages so
the older car accepts it.

**This is a port of [dalathegreat](https://github.com/dalathegreat)'s GPL-3.0
battery-upgrade CAN bridges.** See [Credits & attribution](#credits--attribution).
All of the CAN translation logic comes from dala's work, with per-line source
references in the code comments; none of the underlying reverse-engineering
originates here.

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
     (L = │  │ (G =                                    (L = │  │ (G =
     blue)│  │green)*                                  blue)│  │green)*
          │  │                                              │  │
        pin 2  pin 3                                     pin 2  pin 3
   ┌──────┴──┴─────────────── LilyGo T-2CANFD ──────────────┴──┴──────┐
   │      the two black 4-pin 3.81 mm terminal blocks (P1 / P2)       │
   │   ┌────────────────────┐              ┌───────────────────────┐  │
   │   │ P1: CAN-B (TWAI)   │   ESP32-S3   │ P2: CAN-A (MCP2518FD) │  │
   │   │ isolated           │              │ isolated              │  │
   │   └────────────────────┘              └───────────────────────┘  │
   │                                                                  │
   └───────────────────────────────┬──────────────────────────────────┘
                                    │
       2-pin 5.08 mm terminal:      │
                 VIN 12–24 V (+) ───┤   ← car switched 12 V accessory
                 GND (−)         ───┘     (USB-C 5 V = bench/flashing only)

   * Nissan code L = blue, G = green. LEAF harness colors at the VCM per the 2015
     US-market LEAF service manual (SM15EA0ZE0U0). Colors are not uniform on every
     branch of the harness; confirm the pair electrically before cutting.
```

Photos of the actual board (T-2CANFD V1.0):

**Underside: every CAN screw is named next to its solder pad.** Wire by these
printed labels, not by position. One block is port A
(`DGNDA · CANHA · CANLA · SGNDA`), the other port B
(`DGNDB · CANHB · CANLB · SGNDB`); which one faces vehicle vs battery doesn't
matter. The firmware auto-detects.

![T-2CANFD underside with per-pin CAN labels](docs/hardware/t2canfd_bottom_can_labels.jpg)

**Top: the two black 4-pin screw blocks (top edge) are the CAN ports.** At the
opposite end: the `12–24V` power screw terminal with `GND` marked beside it.

![T-2CANFD top side](docs/hardware/t2canfd_top.jpg)

The MCP2518FD oscillator (20 or 40 MHz, board-dependent) is **auto-detected** on
first run and remembered. Nothing to configure. Detection needs live CAN traffic:
on a silent bus (car or battery unpowered), CAN-A stays quiet until frames appear.
Nothing is wrong: it locks on first traffic.

## Web dashboard

The bridge runs a WiFi soft-AP + live telemetry dashboard, no phone app or toolchain
needed:

- **Connect to WiFi**: SSID `CANBRIDGE`. Each bridge generates its own random
  password on first boot and prints it on the serial console — the web installer
  shows that line right after flashing, e.g. `[webui] AP 'CANBRIDGE' password: …`.
  You can set your own password later from the dashboard's settings.
- **Open** <http://10.0.0.1:8080> in a phone or laptop browser. The explicit `:8080` port stops browsers from silently upgrading the address to HTTPS (which the bridge cannot serve); plain <http://10.0.0.1> also works, except in browsers that force an HTTPS upgrade for all sites. The AP answers the OS connectivity checks, so phones treat it as a normal network and route to the bridge over WiFi.

Features:
- Status bar: active profile (LEAF / e-NV200 / Monitor), car state
  (Idle/Driving/Charging/Asleep), which bus is the battery, WebSocket connection
  state, uptime — plus a "Detected" field showing the wire-confirmed car
  generation and battery size (battery only in Monitor mode).
- Battery tiles: SOC, GIDs/kWh, SOH, pack voltage/current/power, temperature,
  charge/discharge power limits, relay/failsafe status, DTC.
- Drive tiles: speed (approx), gear, ECO, torque, inverter voltage.
- Rolling ~15 s sparkline charts for power, SOC, and temperature.
- **Battery cells** section: polls the LBC the same way an OBD diagnostic dongle does
  (0x79B/0x7BB diagnostic groups), one group per ~3 s, covering all 96 cell voltages,
  balancing-shunt status, pack temperatures, Hx, insulation resistance, and the
  battery's part number/serial/BMS ID. Auto-pauses for ~100 s whenever a real
  OBD diagnostic tool is seen polling the bus, to avoid two ISO-TP conversations
  colliding on the same request ID.
- Frame monitor: live per-ID rate/count/payload tables for both CAN buses.
- **Custom CAN transmit** panel (battery / vehicle / raw bus A / raw bus B), behind an
  "Arm transmit" toggle. Transmitting also requires the bridge to positively confirm
  from live CAN traffic that the car is parked, so the panel stays locked on a bench
  with no car attached.

  > ⚠️ **Transmitting on a live vehicle bus can trigger faults or unsafe behavior.**
  > Only use the custom-TX panel when you understand the frame you're sending. This
  > talks directly to the EV-CAN in a car.

The dashboard is a single embedded page (no filesystem/SD card involved) served
straight from flash; the telemetry tap (`src/telemetry.*`) only reads frames on the
bridge's existing forwarding path and never modifies them. All dashboard-originated CAN
transmits are queued and sent from the dedicated CAN-pump task via `webui_drain_tx()`,
the only code that touches the CAN drivers. They are never sent from the WiFi/web task,
since the CAN drivers are not thread-safe.

## Selecting the profile

Three profiles exist. Out of the box the bridge runs **Monitor** until you pick
another profile:

- **Monitor** (default) — for an unmodified LEAF or e-NV200: every frame is
  forwarded exactly as received, nothing is modified or spoofed. The dashboard
  is live, and the cell readout works by sending the same diagnostic requests
  an OBD diagnostic dongle would.
- **LEAF** — the battery-upgrade translation. Many combinations exist, so the
  firmware auto-detects the car's generation (ZE0 / AZE0) and the installed
  battery (24 / 30 / 40 / 62 kWh) from the CAN traffic.
- **e-NV200** — the van's battery-upgrade translation: 24 → 40 kWh only (the
  van only ever shipped with those two packs). Nothing is auto-detected; the
  translation is fixed for that swap.

To change the profile: use the web dashboard's Profile dropdown, or send
`leaf`, `env200` or `monitor` over the serial monitor (115200). Both paths
refuse the change unless the car is parked (or the bus is silent, as on a
bench). The choice is stored in NVS, applied at the next reboot, and never
swapped mid-run.

## Flash from a web browser

Open **<https://uzef1x.github.io/CANBRIDGE/>** in Chrome or Edge, plug the board in
over USB-C and click Install — the page always flashes the latest firmware. The
page can then show the board's serial output, which is where you read the WiFi
password it generated. Nothing else needs configuring: the bridge starts in
Monitor, and the profile is set later from its dashboard.

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
src/leaf_translation.{h,cpp}    full LEAF battery-upgrade translation + read-only detection (Monitor)
src/env200_translation.{h,cpp}  e-NV200 (24→40 kWh) translation
src/telemetry.{h,cpp}     read-only decoded-state tap for the web dashboard
src/leaf_diag.{h,cpp}     OBD-style battery diagnostic polling (0x79B/0x7BB) for the dashboard
src/webui.{h,cpp}         WiFi soft-AP + async web server/WebSocket + CAN-TX queue
src/webui_page.h          embedded dashboard HTML/CSS/JS (PROGMEM)
src/ap_config.{h,cpp}     WiFi AP password persistence (NVS)
src/fw_version.h          hand-set firmware version constant (dashboard + boot log)
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

## License & credits

This project is a derivative work of dalathegreat's **GPL-3.0** battery-upgrade bridges,
so it is released under the **GNU General Public License v3.0** — see [LICENSE](LICENSE).
If you use, modify, or distribute it, you must preserve the attribution below and keep it
GPL-3.0. This is required by dala's license and is the right thing to do — the credit for
this work belongs with them.

### Credits & attribution

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
- **Battery-Emulator** — https://github.com/dalathegreat/Battery-Emulator (**GPL-3.0**). The
  dashboard's OBD-style battery diagnostic polling (`src/leaf_diag.*` — 0x79B/0x7BB group
  requests, cell-voltage/shunt/temperature/identity parsing, the `Temp_fromRAW_to_F()`
  conversion) is ported from its `Software/src/battery/NISSAN-LEAF-BATTERY.cpp`.
- dala's bridges were developed together with **Muxsan**'s 3-port CAN-bridge hardware.

Also referenced / thanks to:
- **NismoBoy34 / Esp32LeafInverterBridge** — the prior ESP32 attempt that prompted this project. Its battery-upgrade translation was never implemented, so the logic here is re-ported directly from dala rather than taken from it.
- **LilyGo T-2CANFD** board pinout — from [Xinyuan-LilyGO/T-2Can](https://github.com/Xinyuan-LilyGO/T-2Can).
- **ACAN2517FD** MCP2518FD driver library by Pierre Molinaro.
- **Longan_CANFD** — https://github.com/Longan-Labs/Longan_CANFD — MCP2518FD library used during development to cross-check controller configuration (via LilyGo's example code).

The code comments carry per-handler source line references (e.g. `// L523-736`) so every
value can be traced back to dala's original file and line.
