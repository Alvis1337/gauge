// ELM327 protocol client over BLE — the firmware equivalent of obd.py.
// Same init handshake, response-echo parsing, and Mode 01/22 PID set as
// the Python version; only the transport underneath changed.
//
// Hot path (_query / _sendRaw / _extractPayload / _responseEcho) is
// entirely stack-allocated — no std::string in the polling loop.
// ~30 heap alloc/free cycles per 100 ms were eliminated; on a 300 KB heap
// shared with NimBLE and LVGL that matters for long-term fragmentation.
#pragma once
#include <Arduino.h>
#include <cctype>
#include <cstring>
#include <string>
#include "ble_transport.h"
#include "obd_log.h"
#include "parsers.h"

struct GaugeData {
    float boost_psi  = NAN;
    float fuel_pct   = NAN;
    float oil_temp_c = NAN;
    float ethanol    = NAN;  // MHD flex fuel kit — Mode 22 PID 0x44DE
};

class ObdClient {
public:
    static constexpr uint32_t DEFAULT_TIMEOUT_MS  = 1500;
    static constexpr uint32_t MODE22_TIMEOUT_MS   = 2000;
    static constexpr int      SKIP_AFTER_FAILURES = 3;

    bool connected = false;

    bool connect(const std::string &address) {
        _ethanolFails = 0;
        obd_log::write("[try] %s", address.c_str());
        if (!_transport.connect(address, DEFAULT_TIMEOUT_MS)) {
            obd_log::write("[fail] BLE connect");
            return false;
        }
        obd_log::write("[ble] connected, init ELM...");
        if (!_initElm()) {
            obd_log::write("[fail] ELM init");
            _transport.close();
            return false;
        }
        connected = true;
        obd_log::write("[conn] OK %s", address.c_str());
        return true;
    }

    void disconnect() {
        connected = false;
        obd_log::write("[disc]");
        _transport.close();
    }

    GaugeData poll() {
        GaugeData data;
        float map_kpa  = _query("010B", parsers::parseKpa);
        float baro_kpa = _query("0133", parsers::parseKpa);
        bool  baro_fallback = isnan(baro_kpa);
        if (baro_fallback) baro_kpa = 101.325f;
        if (!isnan(map_kpa))
            data.boost_psi = (map_kpa - baro_kpa) * 0.145038f;
        data.fuel_pct   = _query("012F", parsers::parseFuelLevel);
        data.oil_temp_c = _query("015C", parsers::parseCoolant);

        if (_ethanolFails < SKIP_AFTER_FAILURES) {
            data.ethanol = _query("2244DE", parsers::parseEthanol, MODE22_TIMEOUT_MS);
            if (isnan(data.ethanol)) ++_ethanolFails; else _ethanolFails = 0;
        }

        char bstr[8], estr[8], fstr[8], ostr[8];
        if (isnan(data.boost_psi))  snprintf(bstr, sizeof(bstr), "--");
        else                        snprintf(bstr, sizeof(bstr), "%.1f", data.boost_psi);
        if (isnan(data.ethanol))    snprintf(estr, sizeof(estr), "--");
        else                        snprintf(estr, sizeof(estr), "%.0f%%", data.ethanol);
        if (isnan(data.fuel_pct))   snprintf(fstr, sizeof(fstr), "--");
        else                        snprintf(fstr, sizeof(fstr), "%.0f%%", data.fuel_pct);
        if (isnan(data.oil_temp_c)) snprintf(ostr, sizeof(ostr), "--");
        else                        snprintf(ostr, sizeof(ostr), "%.0fC", data.oil_temp_c);
        obd_log::write("[poll] B=%s E=%s F=%s O=%s%s",
                       bstr, estr, fstr, ostr, baro_fallback ? " baro~" : "");
        return data;
    }

private:
    BleTransport _transport;
    int _ethanolFails = 0;

    bool _initElm() {
        struct InitCmd { const char *cmd; uint32_t timeout_ms; uint32_t delay_ms; };
        static const InitCmd cmds[] = {
            // Skip ATZ/ATWS: Vgate iCar / iOS-Vlink resets its BLE stack on
            // any reset command. ATE0 first is enough.
            {"ATE0",  DEFAULT_TIMEOUT_MS, 0},
            {"ATL0",  DEFAULT_TIMEOUT_MS, 0},
            {"ATS0",  DEFAULT_TIMEOUT_MS, 0},
            {"ATSP0", DEFAULT_TIMEOUT_MS, 0},
            {"ATH1",  DEFAULT_TIMEOUT_MS, 0},
        };
        for (auto &c : cmds) {
            char resp[256];
            if (!_sendRaw(c.cmd, c.timeout_ms, resp, sizeof(resp))) {
                obd_log::write("[fail] AT cmd: %s", c.cmd);
                return false;
            }
            obd_log::write("[at] %s -> ok", c.cmd);
            if (c.delay_ms) delay(c.delay_ms);
        }
        return true;
    }

    // No heap allocation. Sends cmd+\r, receives until '>', writes into out[].
    bool _sendRaw(const char *cmd, uint32_t timeout_ms, char *out, size_t out_len) {
        char payload[16];
        size_t cmd_len = strlen(cmd);
        if (cmd_len + 1 >= sizeof(payload)) { out[0] = '\0'; return false; }
        memcpy(payload, cmd, cmd_len);
        payload[cmd_len] = '\r';

        if (!_transport.send((const uint8_t *)payload, cmd_len + 1)) {
            connected = false;
            out[0] = '\0';
            return false;
        }
        uint8_t buf[256];
        size_t n = _transport.recvUntil('>', buf, sizeof(buf), timeout_ms);
        if (n == 0) {
            obd_log::write("[obd] %s -> timeout", cmd);
            if (!_transport.connected()) connected = false;
            out[0] = '\0';
            return false;
        }
        size_t copy = n < out_len - 1 ? n : out_len - 1;
        memcpy(out, buf, copy);
        out[copy] = '\0';

        char preview[49] = {};
        for (size_t i = 0, j = 0; i < n && j < 48; i++)
            preview[j++] = (buf[i] >= 0x20 && buf[i] < 0x7f) ? (char)buf[i] : '.';
        obd_log::write("[obd] %s -> '%s'", cmd, preview);
        return true;
    }

    // Builds the expected response echo for a given command into out[].
    // "010B" -> "410B", "2244DE" -> "6244DE".
    static void _responseEcho(const char *cmd, char *out, size_t out_len) {
        char mode_str[3] = {cmd[0], cmd[1], '\0'};
        int mode = (int)strtol(mode_str, nullptr, 16);
        size_t n = (size_t)snprintf(out, out_len, "%02X", mode + 0x40);
        for (size_t i = 2; cmd[i] && n < out_len - 1; i++)
            out[n++] = (char)toupper((unsigned char)cmd[i]);
        out[n] = '\0';
    }

    // Strips non-hex chars from raw, finds the response echo, writes payload
    // bytes after it into out[]. Returns false if echo not found.
    static bool _extractPayload(const char *raw, const char *cmd,
                                char *out, size_t out_len) {
        char stripped[256];
        size_t si = 0;
        for (const char *p = raw; *p && si < sizeof(stripped) - 1; p++) {
            unsigned char c = (unsigned char)*p;
            if (isxdigit(c)) stripped[si++] = (char)toupper(c);
        }
        stripped[si] = '\0';

        char echo[16];
        _responseEcho(cmd, echo, sizeof(echo));
        const char *pos = strstr(stripped, echo);
        if (!pos) { out[0] = '\0'; return false; }
        pos += strlen(echo);

        size_t len = strlen(pos);
        size_t copy = len < out_len - 1 ? len : out_len - 1;
        memcpy(out, pos, copy);
        out[copy] = '\0';
        return copy > 0;
    }

    float _query(const char *cmd, float (*parser)(const char *, size_t),
                 uint32_t timeout_ms = DEFAULT_TIMEOUT_MS) {
        char raw[256];
        if (!_sendRaw(cmd, timeout_ms, raw, sizeof(raw))) return NAN;
        char payload[128];
        if (!_extractPayload(raw, cmd, payload, sizeof(payload))) return NAN;
        return parser(payload, strlen(payload));
    }
};
