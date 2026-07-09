// Ring buffer for OBD diagnostic messages — written from the OBD task,
// read by the settings UI's "OBD Log" screen. Lets the user inspect
// adapter responses on-device without a serial monitor.
#pragma once
#include <Arduino.h>
#include <stdarg.h>
#include <string.h>

namespace obd_log {

static constexpr size_t LINES    = 14;
static constexpr size_t LINE_LEN = 64;

struct State {
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    char   buf[LINES][LINE_LEN] = {};
    size_t head  = 0;
    bool   dirty = false;
};

inline State &_s() { static State s; return s; }

// Write a log line to Serial AND the ring buffer (from any task/core).
inline void write(const char *fmt, ...) {
    char tmp[LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    Serial.println(tmp);
    auto &s = _s();
    portENTER_CRITICAL(&s.mux);
    strncpy(s.buf[s.head % LINES], tmp, LINE_LEN - 1);
    s.buf[s.head % LINES][LINE_LEN - 1] = '\0';
    s.head++;
    s.dirty = true;
    portEXIT_CRITICAL(&s.mux);
}

inline bool isDirty() {
    auto &s = _s();
    portENTER_CRITICAL(&s.mux);
    bool d = s.dirty;
    portEXIT_CRITICAL(&s.mux);
    return d;
}

// Copy lines in chronological order into out[], clear dirty flag, return count.
inline size_t snapshot(char out[][LINE_LEN], size_t cap) {
    auto &s = _s();
    portENTER_CRITICAL(&s.mux);
    size_t total = s.head < LINES ? s.head : LINES;
    size_t start = s.head < LINES ? 0 : s.head % LINES;
    size_t n = total < cap ? total : cap;
    for (size_t i = 0; i < n; i++)
        strncpy(out[i], s.buf[(start + i) % LINES], LINE_LEN);
    s.dirty = false;
    portEXIT_CRITICAL(&s.mux);
    return n;
}

}  // namespace obd_log
