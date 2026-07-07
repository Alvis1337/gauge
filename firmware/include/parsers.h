// Parse OBD-II response payloads. Callers pass in just the hex data bytes
// *after* the response echo (e.g. "1AF8" for a "410C" RPM response) — see
// obd_client.h's extractPayload() for how that's isolated from the raw
// adapter text (CAN header / PCI length byte / echo / spacing all
// stripped there). NAN means "no value", mirroring Python's None.
#pragma once
#include <cmath>
#include <cstdlib>
#include <string>

namespace parsers {

inline long hexByte(const std::string &h, size_t offset) {
    if (h.size() < offset + 2) return -1;
    return strtol(h.substr(offset, 2).c_str(), nullptr, 16);
}

inline float parseRpm(const std::string &h) {
    if (h.size() < 4) return NAN;
    long a = hexByte(h, 0), b = hexByte(h, 2);
    if (a < 0 || b < 0) return NAN;
    return (a * 256 + b) / 4.0f;
}

inline float parseKpa(const std::string &h) {
    long a = hexByte(h, 0);
    return a < 0 ? NAN : (float)a;
}

inline float parseCoolant(const std::string &h) {
    long a = hexByte(h, 0);
    return a < 0 ? NAN : (float)(a - 40);
}

inline float parseThrottle(const std::string &h) {
    long a = hexByte(h, 0);
    return a < 0 ? NAN : a * 100.0f / 255.0f;
}

// mode 22 DID 0x4402: raw * 191.25 / 255 - 48 = degC
// Source: github.com/TomWis97/bmw-obd2-display
inline float parseOilTemp4402(const std::string &h) {
    long a = hexByte(h, 0);
    return a < 0 ? NAN : a * 191.25f / 255.0f - 48.0f;
}

inline float parseEthanol(const std::string &h) {
    long a = hexByte(h, 0);
    return a < 0 ? NAN : (float)a;
}

}  // namespace parsers
