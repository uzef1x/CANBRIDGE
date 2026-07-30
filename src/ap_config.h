// ─────────────────────────────────────────────────────────────────────────────
// Soft-AP password persistence, NVS namespace "bridge" key "appw" (shared
// namespace with vehicle_config.cpp's own Preferences handle — fine on ESP32).
//
// SAFETY: like the vehicle profile, a new password only takes effect on the
// next reboot (WiFi.softAP() is only called once, from webui_begin()). The
// NVS write itself must only ever be called from the main loop task (see
// webui.cpp's pending-request handoff) — never from an AsyncTCP callback.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

// Load the persisted password into a static buffer, generating and storing a
// random per-device default on first boot (no user password has ever been
// set). Call once, before webui_begin() — bridge_begin()/setup() already do.
// Prints the in-effect password to the boot log; main.cpp's loop() reprints it
// at ~5 s uptime, because the boot print lands before USB-CDC re-enumerates
// after a reset and would otherwise never be seen. That line is how a
// first-time user learns a generated password.
void ap_config_begin();

// Returns the stored AP password: a user-set password if one was ever
// stored via ap_password_store(), otherwise the random per-device default
// generated on first boot. Pointer to a static buffer, valid for the
// process lifetime.
const char *ap_password();

// Persist a new AP password. Validates length 8..63; no-op (returns false)
// otherwise. Main-loop task ONLY — the underlying NVS write can stall.
bool ap_password_store(const char *pw);
