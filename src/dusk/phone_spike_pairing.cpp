#include "dusk/phone_spike_pairing.h"

#include "dusk/dualscreen.h"

#if DUSK_PHONE_SPIKE

#include "dusk/logging.h"

#include <qrcodegen.h>

#include "miniz.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>

#include <cstdio>
#include <mutex>
#include <vector>

namespace dusk::phone_spike {
namespace {
std::mutex g_tokenMutex;
std::string g_activeToken;

// Game-thread-only (see current_qr_bitmap()'s doc comment) — no mutex,
// unlike the token above, which set_active_token()/current_token() do
// guard because those ARE called across threads elsewhere.
std::vector<uint8_t> g_qrPixels;
uint32_t g_qrWidth = 0;
uint32_t g_qrHeight = 0;
std::string g_pairingUrl;
}  // namespace

std::string generate_token() {
    // /dev/urandom directly rather than std::random_device: this only needs
    // to be hard to guess within a short pairing window on the local LAN,
    // not cryptographically bulletproof, and this sidesteps relying on a
    // given std lib's random_device quality guarantees (which vary).
    uint8_t raw[8];
    FILE* f = fopen("/dev/urandom", "rb");
    if (f == nullptr || fread(raw, 1, sizeof(raw), f) != sizeof(raw)) {
        // Degrade rather than fail pairing entirely: a spike-only fallback,
        // clearly worse than urandom but never leaves the token empty
        // (an empty token would compare-equal against a missing query
        // param and defeat the whole check).
        DuskLog.warn("phone spike: /dev/urandom unavailable, using a weak fallback token");
        for (uint8_t& b : raw) {
            b = static_cast<uint8_t>(rand() & 0xFF);
        }
    }
    if (f != nullptr) {
        fclose(f);
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string token;
    token.reserve(sizeof(raw) * 2);
    for (uint8_t b : raw) {
        token.push_back(kHex[b >> 4]);
        token.push_back(kHex[b & 0x0F]);
    }
    return token;
}

void set_active_token(std::string token) {
    std::lock_guard lock{g_tokenMutex};
    g_activeToken = std::move(token);
}

const std::string& current_token() {
    return g_activeToken;
}

namespace {

// Lower is better. Real LAN addresses win outright; VPN interfaces (name
// match) and CGNAT addresses (Tailscale's actual range, 100.64.0.0/10 —
// caught by IP even if some other VPN doesn't match the name list below)
// are a last resort, not a hard exclusion, so pairing still works over a
// VPN-only link rather than returning nothing. Confirmed against a real
// gap: this machine has both a LAN and a Tailscale interface, and
// getifaddrs()'s enumeration order isn't LAN-first — the original
// first-match version picked Tailscale.
int address_priority(std::string_view ifName, uint32_t hostOrderAddr) {
    static constexpr std::string_view kVpnPrefixes[] = {
        "tailscale", "wg", "utun", "tun", "ppp", "zt", "docker", "veth", "br-", "virbr",
    };
    for (const auto prefix : kVpnPrefixes) {
        if (ifName.compare(0, prefix.size(), prefix) == 0) {
            return 2;
        }
    }
    const bool cgnat = (hostOrderAddr & 0xFFC00000) == 0x64400000;  // 100.64.0.0/10
    if (cgnat) {
        return 2;
    }
    const bool rfc1918 = (hostOrderAddr & 0xFF000000) == 0x0A000000 ||        // 10.0.0.0/8
                         (hostOrderAddr & 0xFFF00000) == 0xAC100000 ||        // 172.16.0.0/12
                         (hostOrderAddr & 0xFFFF0000) == 0xC0A80000;          // 192.168.0.0/16
    return rfc1918 ? 0 : 1;
}

}  // namespace

std::string discover_lan_ip() {
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0) {
        return {};
    }
    std::string best;
    int bestPriority = 3;
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if ((ifa->ifa_flags & IFF_LOOPBACK) != 0 || (ifa->ifa_flags & IFF_UP) == 0) {
            continue;
        }
        const auto* sin = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
        const int priority =
            address_priority(ifa->ifa_name != nullptr ? ifa->ifa_name : "", ntohl(sin->sin_addr.s_addr));
        if (priority >= bestPriority) {
            continue;
        }
        char buf[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)) != nullptr) {
            best = buf;
            bestPriority = priority;
            if (bestPriority == 0) {
                break;  // can't do better than a real LAN address
            }
        }
    }
    freeifaddrs(ifaddr);
    return best;
}

bool write_pairing_qr(const std::string& url, const std::string& outPath) {
    uint8_t qrcode[qrcodegen_BUFFER_LEN_MAX];
    uint8_t tempBuffer[qrcodegen_BUFFER_LEN_MAX];
    const bool encoded = qrcodegen_encodeText(url.c_str(), tempBuffer, qrcode, qrcodegen_Ecc_MEDIUM,
        qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true);
    if (!encoded) {
        DuskLog.warn("phone spike: QR encode failed (URL too long?): {}", url);
        return false;
    }

    const int modules = qrcodegen_getSize(qrcode);
    constexpr int kModulePx = 8;
    constexpr int kQuietModules = 4;  // QR spec's minimum recommended quiet zone
    const int imgModules = modules + kQuietModules * 2;
    const int imgPx = imgModules * kModulePx;

    std::vector<uint8_t> pixels(static_cast<size_t>(imgPx) * imgPx * 4, 0xFF);  // opaque white
    for (int y = 0; y < modules; ++y) {
        for (int x = 0; x < modules; ++x) {
            if (!qrcodegen_getModule(qrcode, x, y)) {
                continue;
            }
            const int px0 = (x + kQuietModules) * kModulePx;
            const int py0 = (y + kQuietModules) * kModulePx;
            for (int dy = 0; dy < kModulePx; ++dy) {
                for (int dx = 0; dx < kModulePx; ++dx) {
                    const size_t idx =
                        (static_cast<size_t>(py0 + dy) * imgPx + (px0 + dx)) * 4;
                    pixels[idx + 0] = 0;
                    pixels[idx + 1] = 0;
                    pixels[idx + 2] = 0;
                    pixels[idx + 3] = 0xFF;
                }
            }
        }
    }

    // Cache before the PNG round-trip so the in-game texture provider has
    // it even if the file write below fails for some reason (e.g. a
    // read-only data dir) — the PNG is a convenience, not the source of
    // truth.
    g_qrPixels = pixels;
    g_qrWidth = static_cast<uint32_t>(imgPx);
    g_qrHeight = static_cast<uint32_t>(imgPx);

    size_t pngSize = 0;
    void* png =
        tdefl_write_image_to_png_file_in_memory_ex(pixels.data(), imgPx, imgPx, 4, &pngSize, 6, MZ_FALSE);
    if (png == nullptr) {
        DuskLog.warn("phone spike: QR PNG encode failed");
        return false;
    }
    FILE* file = fopen(outPath.c_str(), "wb");
    bool ok = false;
    if (file != nullptr) {
        ok = fwrite(png, 1, pngSize, file) == pngSize;
        fclose(file);
    }
    mz_free(png);
    if (!ok) {
        DuskLog.warn("phone spike: could not write QR PNG to {}", outPath);
    }
    return ok;
}

bool current_qr_bitmap(std::vector<uint8_t>& outPixels, uint32_t& outWidth, uint32_t& outHeight) {
    if (g_qrPixels.empty()) {
        return false;
    }
    outPixels = g_qrPixels;
    outWidth = g_qrWidth;
    outHeight = g_qrHeight;
    return true;
}

bool start_pairing(uint16_t port, const std::string& qrOutPath) {
    set_active_token(generate_token());

    const std::string ip = discover_lan_ip();
    if (ip.empty()) {
        DuskLog.warn("phone spike: no LAN IPv4 found, cannot build a pairing URL "
                     "(token gating is still active — connect manually if you know the address)");
        return false;
    }

    g_pairingUrl = "http://" + ip + ":" + std::to_string(port) + "/?token=" + current_token();
    DuskLog.info("phone spike: pairing URL {}", g_pairingUrl);
    if (!write_pairing_qr(g_pairingUrl, qrOutPath)) {
        return false;
    }
    DuskLog.info("phone spike: pairing QR written to {}", qrOutPath);
    return true;
}

const std::string& current_pairing_url() {
    return g_pairingUrl;
}

}  // namespace dusk::phone_spike

#endif  // DUSK_PHONE_SPIKE
