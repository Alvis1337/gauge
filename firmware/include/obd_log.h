// Ring buffer for OBD diagnostic messages — written from the OBD task,
// read by the settings UI's "OBD Log" screen and the Discord uploader.
// Stored in RTC memory so the log survives ESP.restart() — meaning a
// Gauge Mode session's data is still readable after rebooting into Update
// Mode to upload it.
#pragma once
#include <Arduino.h>
#include <stdarg.h>
#include <string.h>

namespace obd_log {

static constexpr size_t LINES    = 20;
static constexpr size_t LINE_LEN = 64;

// RTC_DATA_ATTR persists across ESP.restart() (software reset) so logs
// written in Gauge Mode survive the reboot into Update Mode for upload.
// Does NOT persist across power-off or hard reset — that's fine, we only
// need logs from the current ignition cycle.
RTC_DATA_ATTR static char   _buf[LINES][LINE_LEN];
RTC_DATA_ATTR static size_t _head;

static portMUX_TYPE _mux   = portMUX_INITIALIZER_UNLOCKED;
static bool         _dirty = false;

inline void clear() {
    portENTER_CRITICAL(&_mux);
    memset(_buf, 0, sizeof(_buf));
    _head  = 0;
    _dirty = false;
    portEXIT_CRITICAL(&_mux);
}

inline void write(const char *fmt, ...) {
    char tmp[LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    Serial.println(tmp);
    portENTER_CRITICAL(&_mux);
    strncpy(_buf[_head % LINES], tmp, LINE_LEN - 1);
    _buf[_head % LINES][LINE_LEN - 1] = '\0';
    _head++;
    _dirty = true;
    portEXIT_CRITICAL(&_mux);
}

inline bool isDirty() {
    portENTER_CRITICAL(&_mux);
    bool d = _dirty;
    portEXIT_CRITICAL(&_mux);
    return d;
}

inline size_t snapshot(char out[][LINE_LEN], size_t cap) {
    portENTER_CRITICAL(&_mux);
    size_t total = _head < LINES ? _head : LINES;
    size_t start = _head < LINES ? 0 : _head % LINES;
    size_t n = total < cap ? total : cap;
    for (size_t i = 0; i < n; i++)
        strncpy(out[i], _buf[(start + i) % LINES], LINE_LEN);
    _dirty = false;
    portEXIT_CRITICAL(&_mux);
    return n;
}

}  // namespace obd_log
