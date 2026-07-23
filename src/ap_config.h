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

// Load the persisted password (or the default) into a static buffer. Call
// once, before webui_begin() — i.e. from setup()/bridge_begin(), not from
// inside webui_begin() itself, since Part D defers webui_begin() to after
// the first loop() pass.
void ap_config_begin();

// Returns the stored AP password, or "canbridge123" if unset/invalid (<8
// chars). Pointer to a static buffer, valid for the process lifetime.
const char *ap_password();

// Persist a new AP password. Validates length 8..63; no-op (returns false)
// otherwise. Main-loop task ONLY — the underlying NVS write can stall.
bool ap_password_store(const char *pw);
