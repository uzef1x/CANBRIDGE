# LoRa telemetry add-on — research & design notes

Captured 2026-07-23. This folder holds everything gathered while scoping a LoRa
telemetry uplink for the CANBRIDGE (LilyGo T-2CANFD) board. **Nothing here is
implemented** — it's a durable record so we can pick up later if we decide to
build it.

## Files in this folder
- `README.md` — this file (decisions, pinout, part list, firmware sketch).
- `T-2Can-Fd_V1.0_schematic.pdf` — official LilyGo schematic (from
  `Xinyuan-LilyGO/T-2Can`, `project/T-2Can-Fd_V1.0.pdf`). Source of the header pinout.
- `t2canfd_expansion_header_pinout.png` — cropped render of the 26-pin header symbol.

---

## Scope decision (locked)
- **Telemetry only over LoRa.** One-way status beacon → server. NOT firmware
  updates, NOT a CAN bus tap (LoRa bandwidth is a few hundred bytes/min; the
  Nissan bus is 500 kbit/s — physically impossible to stream).
- Firmware updates, if ever wanted, go over **WiFi STA + ESP32 OTA** when the car
  is home (partitions already have app0/app1 OTA slots). LoRa would at most be a
  "go fetch update X" trigger channel — not the payload. See "Rejected/deferred".

## Why LoRa needs receiving infrastructure (don't forget this)
LoRa is not cellular. The board can't reach a server on its own. Options:
- **LoRaWAN**: gateway + network server (TTN or self-hosted ChirpStack) → MQTT/HTTP
  into your server. Coverage varies by region. Gives AES-128 for free.
- **Private P2P**: a Raspberry Pi + LoRa hat at home receives and relays. Simple,
  free, but only works when the car is within range of *your* receiver. No
  encryption/auth by default — you'd add AES-GCM + rolling counter yourself.

## Security constraint (hard requirement if any downlink is ever added)
Telemetry *uplink* can be plaintext (worst case: neighbours learn your SoC).
Any **downlink that causes an action** (OTA trigger, CAN write) MUST be
authenticated AND still gate on `car_write_safe()` (parked/Park/stationary
interlock in `src/telemetry.h`). A radio must never bypass the interlock.
This intersects the open safety review — see memory `safety-review-2026-07-23`.

---

## Hardware verification — VIABLE

### T-2CANFD 26-pin expansion header (unpopulated 2.54 mm, 2×13)
Read directly off `T-2Can-Fd_V1.0_schematic.pdf`. ⚠️ = do not use for the radio.

| Pin | Signal      | | Pin | Signal |
|-----|-------------|-|-----|--------|
| 1   | **3V3**     | | 2   | GND    |
| 3   | **5V**      | | 4   | GND    |
| 5   | IO35 ⚠️PSRAM| | 6   | IO39   |
| 7   | IO38        | | 8   | IO42   |
| 9   | IO37 ⚠️PSRAM| | 10  | IO41   |
| 11  | IO36 ⚠️PSRAM| | 12  | IO40   |
| 13  | IO16        | | 14  | IO4    |
| 15  | IO15        | | 16  | IO5    |
| 17  | IO45 ⚠️strap| | 18  | IO48   |
| 19  | IO47        | | 20  | IO21   |
| 21  | IO14        | | 22  | IO17   |
| 23  | IO18        | | 24  | GND    |
| 25  | IO46 ⚠️strap| | 26  | IO3 ⚠️strap |

21 GPIOs broken out + 3V3 + 5V + 4×GND.

### Do-not-use pins and why
- **IO35 / IO36 / IO37** — consumed by the board's octal (OPI) PSRAM. The build is
  `board_build.arduino.memory_type = qio_opi`; octal PSRAM reserves GPIO33–37.
- **IO45 / IO46** — boot strapping pins (VDD_SPI voltage / boot mode).
- **IO3** — strapping / JTAG select. Also wired as MCP2518FD `INT_1` spare.

### Key finding: existing SPI bus is NOT on the header
Cross-checked against `include/board_pins.h` / vendor `pin_config.h`:
SCLK 12, MOSI 11, MISO 13, CS 10, INT 8/9, CANB TX 7 / RX 6 are **internal only**.
→ You **cannot** tap the MCP2518FD SPI bus from the header. This is actually good:
give the LoRa radio its **own dedicated SPI** on free header pins (the ESP32-S3
GPIO matrix routes any SPI peripheral to any GPIO). No bus-sharing mutex, no
contention with the safety-critical CAN pump task.

### GPIOs already used on the board (for reference)
| Function            | GPIO |
|---------------------|------|
| CANB TX / RX (TWAI) | 7 / 6 |
| SPI SCLK/MOSI/MISO  | 12 / 11 / 13 |
| MCP2518FD CS        | 10 |
| MCP2518FD INT / INT_0 / INT_1 | 8 / 9 / 3 |
| BOOT                | 0 |
| USB CDC (native)    | 19 / 20 |

---

## Part list to order
- **Ebyte E22-900M22S** (SX1262, 868/915 MHz, 22 dBm) — **IPEX/u.FL variant**
  (NOT the stamp-hole one). ~€8–10.
- **u.FL → SMA pigtail**.
- **868 MHz SMA antenna** (mount near glass / externally — steel body attenuates).
- Optional: 2×13 2.54 mm pin header strip to populate the board header.

Total ~€15. Architecture: existing ESP32-S3 stays the brain; E22 is just the radio.

## Rejected / deferred alternatives
- **Full-image OTA over LoRa** — REJECTED. 850 KB app image vs 1% duty cycle =
  >1 day airtime; bad failure mode. (Delta-OTA ~tens of KB would be minutes, only
  if we ever commit to detools/janpatch tooling.)
- **WiFi STA + MQTT + OTA** — the better path if the car parks in WiFi range;
  needs zero new hardware (WiFi + AsyncWebServer already present). LoRa only wins
  when the car parks out of WiFi range and we want data while it's away.
- **Cellular (SIM7600/A7670)** — the proper "both telemetry + OTA, anywhere"
  answer, but costs a SIM. Deferred.
- **Whole second LilyGo LoRa board (T3-S3) as UART relay** — fallback if bare-module
  wiring is unwanted; more money + second MCU. LilyGo sells NO bare SPI LoRa
  add-on that fits the T-2CANFD (all their LoRa products are complete MCU boards).

---

## Proposed pin assignment (all caveat-free, adjacent on header)
| E22 pin   | T-2CANFD GPIO | Header pin |
|-----------|---------------|------------|
| VCC 3.3V  | 3V3           | 1  |
| GND       | GND           | 2  |
| SCK       | IO15          | 15 |
| MOSI      | IO16          | 13 |
| MISO      | IO17          | 22 |
| NSS / CS  | IO18          | 23 |
| BUSY      | IO21          | 20 |
| DIO1      | IO4           | 14 |
| NRST      | IO5           | 16 |

Still free afterward: IO38, IO39, IO40, IO41, IO42, IO47, IO48.
Power: E22 draws ~120 mA peak on TX — fine off the 3V3 rail.

## Firmware sketch (RadioLib, not yet written)
- Library: **RadioLib** (Jgromes) — add to `lib_deps` in `platformio.ini`.
- Dedicated `SPIClass` on FSPI/HSPI bound to the pins above.
- `Module(NSS=18, DIO1=4, NRST=5, BUSY=21)` → `SX1262 radio(&module)`.
- Region: EU868, respect 1% duty cycle. Pack the payload from `g_telemetry`
  (`src/telemetry.h`): SoC, pack V/A, GIDs, SoH, min/avg/max temp, car_state,
  relay/failsafe flags ≈ 20 bytes. Send every 30–60 s.
- Run the sender on its OWN FreeRTOS task / timer — NEVER from the CAN pump task
  (protects the 10 ms 0x1DB forwarding deadline). Read only scalar telemetry
  fields (atomic 32-bit reads, per the concurrency note in `telemetry.h`).
- LeafSpy-style full 96-cell dump: delta-encode → ~150 bytes, a few times/hour.

## Open items before building
1. Confirm the E22 IPEX variant part number with the actual supplier at order time.
2. Decide receiver architecture: LoRaWAN (TTN/ChirpStack) vs private P2P Pi.
3. If any downlink is ever added: design the auth (AES) + interlock gating first.
