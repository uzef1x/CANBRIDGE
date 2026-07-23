#include "ap_config.h"
#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

static Preferences prefs;
static char g_ap_pw[64] = "canbridge123";

void ap_config_begin() {
  prefs.begin("bridge", false);
  // isKey() avoids the noisy NVS "NOT_FOUND" error log on a fresh device that
  // has never had a custom password set (the default is used).
  String stored = prefs.isKey("appw") ? prefs.getString("appw", "") : String("");
  if (stored.length() >= 8 && stored.length() <= 63) {
    strncpy(g_ap_pw, stored.c_str(), sizeof(g_ap_pw) - 1);
    g_ap_pw[sizeof(g_ap_pw) - 1] = '\0';
  } else {
    strncpy(g_ap_pw, "canbridge123", sizeof(g_ap_pw) - 1);
    g_ap_pw[sizeof(g_ap_pw) - 1] = '\0';
  }
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
