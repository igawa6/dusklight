#pragma once

// Phase-2 spike for the phone-companion-display idea (see
// docs/phone-companion-design.md): QR-code pairing on top of the phase-1/3
// frame-push + touch-input server. Generates a per-run token and a QR code
// encoding the pairing URL; phone_spike_ws.cpp checks incoming WS upgrades
// against the current token. Gated by DUSK_PHONE_SPIKE, off by default —
// throwaway scaffolding, not the shipped feature's pairing UX (no in-game
// display of the QR yet, just a PNG written to disk).

#include <cstdint>
#include <string>

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

// Orchestrates one pairing round: generates a fresh token and makes it the
// active one (phone_spike_ws.cpp's WS-upgrade handler checks new
// connections against it), discovers a LAN IP, builds the
// http://<ip>:<port>/?token=... pairing URL, writes its QR to `qrOutPath`,
// and logs the URL. The token is set regardless of whether LAN-IP discovery
// or the QR write succeeded — gating stays active either way; the QR is a
// convenience on top, not the security boundary. Returns false if the URL
// couldn't be built (no usable LAN IP found) or the QR write failed.
bool start_pairing(uint16_t port, const std::string& qrOutPath);

}  // namespace dusk::phone_spike
