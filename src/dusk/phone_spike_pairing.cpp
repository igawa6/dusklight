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

std::string discover_lan_ip() {
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0) {
        return {};
    }
    std::string result;
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if ((ifa->ifa_flags & IFF_LOOPBACK) != 0 || (ifa->ifa_flags & IFF_UP) == 0) {
            continue;
        }
        char buf[INET_ADDRSTRLEN];
        const auto* sin = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
        if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)) != nullptr) {
            result = buf;
            break;  // first non-loopback UP IPv4 — good enough for a spike
        }
    }
    freeifaddrs(ifaddr);
    return result;
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

bool start_pairing(uint16_t port, const std::string& qrOutPath) {
    set_active_token(generate_token());

    const std::string ip = discover_lan_ip();
    if (ip.empty()) {
        DuskLog.warn("phone spike: no LAN IPv4 found, cannot build a pairing URL "
                     "(token gating is still active — connect manually if you know the address)");
        return false;
    }

    const std::string url = "http://" + ip + ":" + std::to_string(port) + "/?token=" + current_token();
    DuskLog.info("phone spike: pairing URL {}", url);
    if (!write_pairing_qr(url, qrOutPath)) {
        return false;
    }
    DuskLog.info("phone spike: pairing QR written to {}", qrOutPath);
    return true;
}

}  // namespace dusk::phone_spike

#endif  // DUSK_PHONE_SPIKE
