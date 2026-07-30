#include "can_bus.h"
#include "board_pins.h"
#include <Arduino.h>
#include <SPI.h>
#include <ACAN2517FD.h>
#include <Preferences.h>
#include "driver/twai.h"

// ── BUS_A : MCP2518FD via ACAN2517FD ─────────────────────────────────────────
static ACAN2517FD gCanA(MCP2518_CS_GPIO, SPI, MCP2518_INT_GPIO);

// ── CAN-A oscillator auto-detection ──────────────────────────────────────────
// The T-2CANFD carries either a 20 MHz or a 40 MHz crystal on the MCP2518FD
// (LilyGo's example code says 20 MHz, their schematic prints 40 MHz) and the
// board offers no way to read it back. The wrong value halves or doubles the
// bit rate and CAN-A then hears nothing at all — indistinguishable from a dead
// bus. So the value is measured once and remembered:
//
//   * NVS holds a previously locked value -> use it, straight to Normal20B.
//   * otherwise -> cycle trial configurations every 600 ms until real frames
//     arrive, then lock, switch to Normal20B and persist. canbus_begin() never
//     waits for this; boot and the web UI carry on as usual.
//
// No trial mode ever transmits a data frame or an ACTIVE ERROR FLAG, so a
// wrong-bit-rate trial cannot corrupt a live bus. ListenOnly is completely
// silent, but it can only work where some OTHER node acknowledges: on the
// two-node battery segment (bridge + LBC) the LBC would get an ACK error on
// every frame and no frame would ever complete, so no listen-only trial could
// ever succeed there. Restricted Operation — which acknowledges frames it
// decoded as valid but still sends no data frames and no error flags — is the
// only mode that can lock onto a two-node segment, and it is mixed in after
// 6 s of silence.
static Preferences prefs;

static constexpr uint32_t TRIAL_MS    = 600;   // per trial configuration
static constexpr uint32_t LISTEN_MS   = 6000;  // listen-only-trials-only window
static constexpr uint8_t  LOCK_FRAMES = 2;     // frames in ONE trial needed to lock

struct OscTrial {
  ACAN2517FDSettings::Oscillator    osc;
  ACAN2517FDSettings::OperationMode mode;
};
static const OscTrial TRIALS[4] = {
  {ACAN2517FDSettings::OSC_20MHz, ACAN2517FDSettings::ListenOnly},
  {ACAN2517FDSettings::OSC_40MHz, ACAN2517FDSettings::ListenOnly},
  {ACAN2517FDSettings::OSC_20MHz, ACAN2517FDSettings::RestrictedOperation},
  {ACAN2517FDSettings::OSC_40MHz, ACAN2517FDSettings::RestrictedOperation},
};

static ACAN2517FDSettings::Oscillator g_osc = ACAN2517FDSettings::OSC_20MHz;
static bool     g_a_begun      = false;  // begin() attempted -> end() before re-begin
static bool     g_detecting    = false;  // CAN-A is running a trial, not bridging
static uint8_t  g_trial        = 0;      // index into TRIALS
static uint8_t  g_trial_frames = 0;      // frames received in the current trial
static uint32_t g_trial_ms     = 0;
static uint32_t g_detect_ms    = 0;
static volatile bool g_osc_save_pending = false;  // CAN task sets, loop task writes

static unsigned osc_mhz(ACAN2517FDSettings::Oscillator o) {
  return (o == ACAN2517FDSettings::OSC_40MHz) ? 40 : 20;
}

// (Re)configure the MCP2518FD. end() first whenever it was begun before:
// ACAN2517FD::begin() spawns a fresh "ACAN2517Handler" task and allocates the
// filter callback array on every call and frees neither, so a bare re-begin
// would leak a task plus heap per trial; end() is the library's teardown for
// exactly that (it also detaches the INT handler). A failed begin() is a failed
// trial, not a fatal error: the controller is left parked in configuration mode
// either way — silent on the bus, receiving nothing — so the caller just waits
// out the trial. How much of the driver got set up before the failure varies
// (an early SPI read-back error skips the whole setup block; a mode-request
// timeout does not), which is why the end() above is unconditional.
// A 40 MHz trial on a 20 MHz part is expected to fail early that way: the
// library scales SPI to sysclk*2/5 = 16 MHz, over the datasheet's
// FSCK <= 0.85*(FSYSCLK/2) = 8.5 MHz (DS20006027B, Note 3), so its register
// read-back test trips. Silent and self-correcting — just not something to rely
// on, hence the frame-based test below.
static bool beginBusA(ACAN2517FDSettings::Oscillator osc,
                      ACAN2517FDSettings::OperationMode mode) {
  if (g_a_begun) gCanA.end();
  g_a_begun = true;
  ACAN2517FDSettings settings(osc, CAN_BITRATE, DataBitRateFactor::x1);
  settings.mRequestedMode = mode;
  return gCanA.begin(settings, [] { gCanA.isr(); }) == 0;
}

static bool beginBusA() {
  SPI.begin(SPI_SCLK_GPIO, SPI_MISO_GPIO, SPI_MOSI_GPIO, MCP2518_CS_GPIO);
  prefs.begin("bridge", false);
  // isKey() avoids the noisy NVS "NOT_FOUND" log on a device that has never
  // completed a detection (same reason as ap_config.cpp).
  const uint8_t saved = prefs.isKey("canaosc") ? prefs.getUChar("canaosc", 0) : 0;
  if (saved == 20 || saved == 40) {
    g_osc = (saved == 40) ? ACAN2517FDSettings::OSC_40MHz
                          : ACAN2517FDSettings::OSC_20MHz;
    Serial.printf("[can_a] using saved %u MHz oscillator\n", saved);
    return beginBusA(g_osc, ACAN2517FDSettings::Normal20B);  // classic CAN 2.0B
  }
  g_detecting    = true;
  g_trial        = 0;
  g_trial_frames = 0;
  g_trial_ms     = millis();
  g_detect_ms    = g_trial_ms;
  Serial.println("[can_a] oscillator unknown — auto-detecting 20/40 MHz "
                 "(CAN-A idle until a frame is received)");
  beginBusA(TRIALS[0].osc, TRIALS[0].mode);
  return true;  // detection in progress is not an init failure
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

void canbus_poll() {
  if (!g_detecting) return;

  // Trial frames are consumed here, never forwarded: until a trial has proved
  // itself the oscillator — and therefore the bit rate — is still a guess.
  CANFDMessage m;
  while (gCanA.receive(m)) {
    if (g_trial_frames < LOCK_FRAMES) g_trial_frames++;
  }
  // Two frames inside ONE trial. One is not enough: at the wrong bit rate the
  // controller mis-frames real traffic, and a single lucky CRC match would be
  // written to NVS and then trusted forever.
  if (g_trial_frames >= LOCK_FRAMES) {
    g_osc = TRIALS[g_trial].osc;
    beginBusA(g_osc, ACAN2517FDSettings::Normal20B);
    g_detecting        = false;
    g_osc_save_pending = true;  // NVS write happens in canbus_housekeeping()
    Serial.printf("[can_a] oscillator auto-detected: %u MHz\n", osc_mhz(g_osc));
    return;
  }

  const uint32_t now = millis();
  if (now - g_trial_ms < TRIAL_MS) return;
  g_trial_ms     = now;
  g_trial_frames = 0;
  // First 6 s: the two silent listen-only trials, which is all a multi-node
  // vehicle segment needs. After that the two Restricted Operation trials join
  // the rotation so a two-node battery segment can lock too. The rotation then
  // runs for as long as it takes — a genuinely silent bus simply never locks,
  // which costs nothing.
  const uint8_t trials = (now - g_detect_ms < LISTEN_MS) ? 2 : 4;
  g_trial = (uint8_t)((g_trial + 1) % trials);
  beginBusA(TRIALS[g_trial].osc, TRIALS[g_trial].mode);
}

void canbus_housekeeping() {
  if (!g_osc_save_pending) return;
  g_osc_save_pending = false;
  prefs.putUChar("canaosc", (uint8_t)osc_mhz(g_osc));
}

bool canbus_receive(BridgeBus bus, BridgeFrame &out) {
  if (bus == BUS_A) {
    if (g_detecting) return false;  // canbus_poll() owns CAN-A until it locks
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
    if (g_detecting) return;  // trial modes cannot transmit — drop, don't queue
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
