// ─────────────────────────────────────────────────────────────────────────────
// LilyGo T-2CANFD (ESP32-S3) board pin map.
// Values taken verbatim from the vendor repo:
//   Xinyuan-LilyGO/T-2Can  →  libraries/private_library/pin_config.h
// CAN-B = native ESP32-S3 TWAI.  CAN-A = MCP2518FD (CAN-FD controller) over SPI.
// Both Nissan buses run CLASSIC CAN 2.0B @ 500 kbit/s (FD controller in 20B mode).
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

// CAN-B : native TWAI controller (internal to the ESP32-S3)
#define CANB_TX_GPIO   7
#define CANB_RX_GPIO   6

// Shared SPI bus for the MCP2518FD (CAN-A)
#define SPI_SCLK_GPIO  12
#define SPI_MOSI_GPIO  11
#define SPI_MISO_GPIO  13

// CAN-A : MCP2518FD
#define MCP2518_CS_GPIO   10
#define MCP2518_INT_GPIO  8   // primary interrupt line (INT_0=9, INT_1=3 spare)

// MCP2518FD oscillator: LilyGo's example implies 20 MHz (Longan
// mcp2518fd::begin() default clockset = MCP2518FD_20MHz) but their schematic
// prints a 40 MHz crystal, and the boards are not consistent. Not a constant:
// can_bus.cpp detects it at first run and remembers it in NVS.

// Both Nissan CAN segments
#define CAN_BITRATE     (500UL * 1000UL)

#define ESP_BOOT_GPIO   0
