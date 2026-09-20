#pragma once

// Phase-2 spike for the phone-companion-display idea (see
// docs/phone-companion-design.md): QR-code pairing on top of the phase-1/3
// frame-push + touch-input server. Generates a per-run token and a QR code
// encoding the pairing URL; phone_spike_ws.cpp checks incoming WS upgrades
// against the current token. Gated by DUSK_PHONE_SPIKE, off by default.
// Displayed in-game via phone_spike_qr_texture.h (Settings, next to the
// Dual Screen HUD option) — see current_qr_bitmap() below.

#include <cstdint>
#include <string>
#include <vector>

namespace dusk::phone_spike {

// Random per-run pairing token (hex string). Regenerated each call — the
// server keeps whatever the caller last generated as "the" active token via
// set_active_token()/current_token() below.
std::string generate_token();

void set_active_token(std::string token);
const std::string& current_token();

// Best-effort first non-loopback, UP IPv4 address on this machine. Empty
// string if none found. Spike-simple: on a multi-NIC machine (VPNs, virtual
// bridges) this can pick the "wrong" interface — a real implementation would
// need to let the user choose or filter more carefully.
std::string discover_lan_ip();

// Encodes `url` as a QR code and writes it to `outPath` as a PNG (black
// modules on white, quiet zone included, reuses the same miniz PNG writer
// the companion screenshot/frame-push paths already use — no new image
// dependency beyond the QR encoder itself). Returns false on any failure
// (encode too long for the library's max version, or file write failure).
bool write_pairing_qr(const std::string& url, const std::string& outPath);

// Returns the most recently generated QR's raw RGBA8 pixels (same bitmap
// write_pairing_qr() encodes to PNG) plus its width/height (square, so one
// dimension would do, but the texture-provider caller wants both). False
// if none has been generated yet. Game thread only, like the rest of this
// spike's pairing code, but this specific getter needs no synchronization
// of its own beyond that: it's only ever written by start_pairing() and
// read by the Settings UI's texture-provider callback
// (phone_spike_qr_texture.cpp), and both run on the game thread.
bool current_qr_bitmap(std::vector<uint8_t>& outPixels, uint32_t& outWidth, uint32_t& outHeight);

// Orchestrates one pairing round: generates a fresh token and makes it the
// active one (phone_spike_ws.cpp's WS-upgrade handler checks new
// connections against it), discovers a LAN IP, builds the
// http://<ip>:<port>/?token=... pairing URL, writes its QR to `qrOutPath`,
// and logs the URL. The token is set regardless of whether LAN-IP discovery
// or the QR write succeeded — gating stays active either way; the QR is a
// convenience on top, not the security boundary. Returns false if the URL
// couldn't be built (no usable LAN IP found) or the QR write failed.
bool start_pairing(uint16_t port, const std::string& qrOutPath);

// The pairing URL start_pairing() last built (same string its QR encodes),
// for displaying as plain text next to the QR — empty if start_pairing()
// hasn't succeeded yet. Game thread only, same as current_qr_bitmap().
const std::string& current_pairing_url();

}  // namespace dusk::phone_spike
