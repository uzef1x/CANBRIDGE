#include "can_bus.h"
#include "board_pins.h"
#include <Arduino.h>
#include <SPI.h>
#include <ACAN2517FD.h>
#include "driver/twai.h"

// ── BUS_A : MCP2518FD via ACAN2517FD ─────────────────────────────────────────
static ACAN2517FD gCanA(MCP2518_CS_GPIO, SPI, MCP2518_INT_GPIO);

static bool beginBusA() {
  SPI.begin(SPI_SCLK_GPIO, SPI_MISO_GPIO, SPI_MOSI_GPIO, MCP2518_CS_GPIO);
  ACAN2517FDSettings settings(ACAN2517FDSettings::OSC_20MHz, CAN_BITRATE,
                              DataBitRateFactor::x1);
  settings.mRequestedMode = ACAN2517FDSettings::Normal20B;  // classic CAN 2.0B
  const uint32_t err = gCanA.begin(settings, [] { gCanA.isr(); });
  return err == 0;
}

// ── BUS_B : native ESP32-S3 TWAI ─────────────────────────────────────────────
static bool beginBusB() {
  twai_general_config_t g =
      TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)CANB_TX_GPIO,
                                  (gpio_num_t)CANB_RX_GPIO, TWAI_MODE_NORMAL);
  g.tx_queue_len = 32;
  g.rx_queue_len = 64;
  twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  if (twai_driver_install(&g, &t, &f) != ESP_OK) return false;
  return twai_start() == ESP_OK;
}

// ── Public API ───────────────────────────────────────────────────────────────
bool canbus_begin() {
  bool a = beginBusA();
  bool b = beginBusB();
  return a && b;
}

bool canbus_receive(BridgeBus bus, BridgeFrame &out) {
  if (bus == BUS_A) {
    CANFDMessage m;
    if (!gCanA.receive(m)) return false;
    out.id  = m.id;
    out.ext = m.ext;
    out.dlc = (m.len > 8) ? 8 : m.len;
    for (uint8_t i = 0; i < out.dlc; i++) out.data[i] = m.data[i];
    return true;
  } else {
    twai_message_t m;
    if (twai_receive(&m, 0) != ESP_OK) return false;
    out.id  = m.identifier;
    out.ext = m.extd;
    out.dlc = (m.data_length_code > 8) ? 8 : m.data_length_code;
    for (uint8_t i = 0; i < out.dlc; i++) out.data[i] = m.data[i];
    return true;
  }
}

void canbus_send(BridgeBus bus, const BridgeFrame &in) {
  if (bus == BUS_A) {
    CANFDMessage m;
    m.id   = in.id;
    m.ext  = in.ext;
    m.type = CANFDMessage::CAN_DATA;    // classic 2.0B data frame, no BRS
    m.len  = in.dlc;
    for (uint8_t i = 0; i < in.dlc; i++) m.data[i] = in.data[i];
    gCanA.tryToSend(m);
  } else {
    twai_message_t m = {};
    m.identifier       = in.id;
    m.extd             = in.ext ? 1 : 0;
    m.data_length_code = in.dlc;
    for (uint8_t i = 0; i < in.dlc; i++) m.data[i] = in.data[i];
    twai_transmit(&m, 0);               // non-blocking
  }
}
