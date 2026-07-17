// ─────────────────────────────────────────────────────────────────────────────
// Nissan CAN frame checksums (ported verbatim from dalathegreat helper_functions.c).
// Any handler that MODIFIES or GENERATES a checksummed frame must recompute it,
// or the receiver flags "frozen / invalid data".
// All operate on the 8-byte payload: compute over data[0..6], store in data[7].
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include "can_frame.h"

// CRC-8, polynomial 0x85 (0x1DB, 0x55B, 0x59E, ...).
void calc_crc8(BridgeFrame &f);

// Nibble-sum + 2, low nibble of data[7] (0x1F2 on ZE0).
void calc_sum2(BridgeFrame &f);

// Nibble-sum - 4, low nibble of data[7] (generated ZE0 PDM frames 0x390/0x393).
void calc_checksum4(BridgeFrame &f);
