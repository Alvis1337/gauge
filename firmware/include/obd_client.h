// ELM327 protocol client over BLE — the firmware equivalent of obd.py.
// Same init handshake, response-echo parsing, and Mode 01/22 PID set as
// the Python version; only the transport underneath changed.
#pragma once
#include <Arduino.h>
#include <algorithm>
#include <cctype>
#include <string>
#include "ble_transport.h"
#include "parsers.h"

struct GaugeData {
    float boost_psi  = NAN;
    float rpm        = NAN;
    float coolant_c  = NAN;
    float oil_temp_c = NAN;
    float throttle   = NAN;
    float ethanol    = NAN;
};

class ObdClient {
public:
    static constexpr uint32_t DEFAULT_TIMEOUT_MS  = 1500;
    static constexpr uint32_t ATZ_TIMEOUT_MS      = 5000;
    // Mode 22 PIDs are manufacturer-specific and many cars don't support
    // them. A short timeout limits how long an unsupported PID stalls the
    // poll cycle; after SKIP_AFTER_FAILURES consecutive NAN returns the
    // PID is dropped entirely until the next connect().
    static constexpr uint32_t MODE22_TIMEOUT_MS   = 800;
    static constexpr int      SKIP_AFTER_FAILURES = 3;

    bool connected = false;

    bool connect(const std::string &address) {
        _oilTempFails = 0;
        _ethanolFails = 0;
        if (!_transport.connect(address, DEFAULT_TIMEOUT_MS)) return false;
        if (!_initElm()) {
            _transport.close();
            return false;
        }
        connected = true;
        return true;
    }

    void disconnect() {
        connected = false;
        _transport.close();
    }

    GaugeData poll() {
        GaugeData data;
        float map_kpa  = _query("010B", parsers::parseKpa);
        float baro_kpa = _query("0133", parsers::parseKpa);
        if (!isnan(map_kpa) && !isnan(baro_kpa)) {
            data.boost_psi = (map_kpa - baro_kpa) * 0.145038f;
        }
        data.coolant_c = _query("0105", parsers::parseCoolant);
        data.throttle  = _query("0111", parsers::parseThrottle);

        // Mode 22 (manufacturer-specific): shorter timeout, skip after
        // SKIP_AFTER_FAILURES consecutive NANs so an unsupported PID
        // doesn't stall the entire poll cycle every lap.
        if (_oilTempFails < SKIP_AFTER_FAILURES) {
            data.oil_temp_c = _query("224402", parsers::parseOilTemp4402, MODE22_TIMEOUT_MS);
            if (isnan(data.oil_temp_c)) ++_oilTempFails; else _oilTempFails = 0;
        }
        if (_ethanolFails < SKIP_AFTER_FAILURES) {
            data.ethanol = _query("224010", parsers::parseEthanol, MODE22_TIMEOUT_MS);
            if (isnan(data.ethanol)) ++_ethanolFails; else _ethanolFails = 0;
        }
        return data;
    }

private:
    BleTransport _transport;
    int _oilTempFails = 0;
    int _ethanolFails = 0;

    bool _initElm() {
        struct InitCmd { const char *cmd; uint32_t timeout_ms; uint32_t delay_ms; };
        static const InitCmd cmds[] = {
            {"ATZ", ATZ_TIMEOUT_MS, 2000}, {"ATE0", DEFAULT_TIMEOUT_MS, 0},
            {"ATL0", DEFAULT_TIMEOUT_MS, 0}, {"ATS0", DEFAULT_TIMEOUT_MS, 0},
            {"ATSP0", DEFAULT_TIMEOUT_MS, 0}, {"ATH1", DEFAULT_TIMEOUT_MS, 0},
        };
        for (auto &c : cmds) {
            std::string resp;
            if (!_sendRaw(c.cmd, c.timeout_ms, resp)) return false;
            if (c.delay_ms) delay(c.delay_ms);
        }
        return true;
    }

    float _query(const char *cmd, float (*parser)(const std::string &),
                 uint32_t timeout_ms = DEFAULT_TIMEOUT_MS) {
        std::string raw;
        if (!_sendRaw(cmd, timeout_ms, raw)) return NAN;
        std::string payload = _extractPayload(raw, cmd);
        if (payload.empty()) return NAN;
        return parser(payload);
    }

    bool _sendRaw(const char *cmd, uint32_t timeout_ms, std::string &out) {
        std::string payload = std::string(cmd) + "\r";
        if (!_transport.send((const uint8_t *)payload.data(), payload.size())) {
            connected = false;
            return false;
        }
        uint8_t buf[256];
        size_t n = _transport.recvUntil('>', buf, sizeof(buf), timeout_ms);
        if (n == 0) {
            // A dropped BLE link (adapter out of range, powered off) shows
            // up here as a recv timeout, not a send failure — writeValue()
            // can succeed against a connection NimBLE hasn't fully torn
            // down yet. Without this check, `connected` would stay true
            // forever once that happens: poll() keeps returning all-NaN
            // data every cycle and obd_task's `while (client.connected)`
            // loop never exits to retry/rediscover the adapter.
            if (!_transport.connected()) connected = false;
            return false;
        }
        out.assign((const char *)buf, n);
        return true;
    }

    static std::string _responseEcho(const std::string &cmd) {
        int mode = strtol(cmd.substr(0, 2).c_str(), nullptr, 16);
        char buf[3];
        snprintf(buf, sizeof(buf), "%02X", mode + 0x40);
        std::string rest = cmd.substr(2);
        std::transform(rest.begin(), rest.end(), rest.begin(), ::toupper);
        return std::string(buf) + rest;
    }

    // Finds the response echo anywhere in the hex stream and returns the
    // data bytes after it — robust to whatever the adapter prepends (CAN
    // header, PCI/length byte, spacing) since it searches for the actual
    // marker instead of assuming a fixed strip length.
    static std::string _extractPayload(const std::string &raw, const std::string &cmd) {
        std::string s;
        for (char c : raw) if (isxdigit((unsigned char)c)) s += toupper(c);
        std::string echo = _responseEcho(cmd);
        size_t idx = s.find(echo);
        if (idx == std::string::npos) return "";
        return s.substr(idx + echo.size());
    }
};
