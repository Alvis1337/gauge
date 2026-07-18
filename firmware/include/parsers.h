// Parse OBD-II response payloads. Callers pass the hex data bytes
// *after* the response echo as a null-terminated char* + length — see
// obd_client.h's _extractPayload() for how that's isolated. NAN = no value.
// All functions take (const char *h, size_t len) — no heap allocation.
#pragma once
#include <cmath>
#include <cstdlib>

namespace parsers {

inline long hexByte(const char *h, size_t len, size_t offset) {
    if (len < offset + 2) return -1;
    char buf[3] = {h[offset], h[offset + 1], '\0'};
    return strtol(buf, nullptr, 16);
}

inline float parseKpa(const char *h, size_t len) {
    long a = hexByte(h, len, 0);
    return a < 0 ? NAN : (float)a;
}

inline float parseCoolant(const char *h, size_t len) {
    long a = hexByte(h, len, 0);
    return a < 0 ? NAN : (float)(a - 40);
}

inline float parseEthanol(const char *h, size_t len) {
    long a = hexByte(h, len, 0);
    return a < 0 ? NAN : (float)a;
}

inline float parseFuelLevel(const char *h, size_t len) {
    long a = hexByte(h, len, 0);
    return a < 0 ? NAN : (float)a * 100.0f / 255.0f;
}

inline float parseRpm(const char *h, size_t len) {
    long a = hexByte(h, len, 0);
    long b = hexByte(h, len, 2);
    if (a < 0 || b < 0) return NAN;
    return (float)((a * 256) + b) / 4.0f;
}

}  // namespace parsers
