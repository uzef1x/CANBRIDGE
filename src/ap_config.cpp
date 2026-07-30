#include "ap_config.h"
#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h>
#include <string.h>

static Preferences prefs;
static char g_ap_pw[64] = "";

// Unambiguous alphabet for a generated password: digits/letters that are easy
// to tell apart when hand-copied off a serial log (excludes 0/O, 1/l/I).
static const char kGenAlphabet[] = "23456789ABCDEFGHJKMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz";
#define GEN_PW_LEN 12

// Fill `out` (>= GEN_PW_LEN+1 bytes) with a random password from the hardware
// RNG (esp_random(); true entropy source on ESP32, not a seeded PRNG).
static void generate_ap_password(char *out) {
  const size_t alphabet_len = sizeof(kGenAlphabet) - 1;  // exclude the '\0'
  for (int i = 0; i < GEN_PW_LEN; i++) {
    out[i] = kGenAlphabet[esp_random() % alphabet_len];
  }
  out[GEN_PW_LEN] = '\0';
}

void ap_config_begin() {
  prefs.begin("bridge", false);
  // isKey() avoids the noisy NVS "NOT_FOUND" error log on a fresh device that
  // has never had a custom password set.
  String stored = prefs.isKey("appw") ? prefs.getString("appw", "") : String("");
  if (stored.length() >= 8 && stored.length() <= 63) {
    strncpy(g_ap_pw, stored.c_str(), sizeof(g_ap_pw) - 1);
    g_ap_pw[sizeof(g_ap_pw) - 1] = '\0';
  } else {
    // First boot (or corrupted/short stored value): generate a random
    // per-device default and persist it so it is stable across reboots.
    char generated[GEN_PW_LEN + 1];
    generate_ap_password(generated);
    prefs.putString("appw", generated);
    strncpy(g_ap_pw, generated, sizeof(g_ap_pw) - 1);
    g_ap_pw[sizeof(g_ap_pw) - 1] = '\0';
  }
  // Printed every boot — this line is how a first-time user learns the
  // password (the web installer's instructions point them at the serial log).
  Serial.printf("[webui] AP 'CANBRIDGE' password: %s\n", g_ap_pw);
}

const char *ap_password() { return g_ap_pw; }

bool ap_password_store(const char *pw) {
  size_t len = pw ? strlen(pw) : 0;
  if (len < 8 || len > 63) return false;
  prefs.putString("appw", pw);
  strncpy(g_ap_pw, pw, sizeof(g_ap_pw) - 1);
  g_ap_pw[sizeof(g_ap_pw) - 1] = '\0';
  Serial.println("[ap_config] AP password updated — reboot to apply");
  return true;
}
