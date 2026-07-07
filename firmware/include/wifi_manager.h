// WiFi scan/connect for OTA update checks only — NOT used for the OBD
// link (that's BLE, see bt_transport.h/obd_client.h).
//
// The original ESP32 shares one 2.4GHz radio between WiFi and BT/BLE via
// time-division coexistence — leaving WiFi associated continuously would
// steal airtime from the OBD BLE connection while driving. So this stays
// powered off (WIFI_OFF) except for the few seconds a manual "Check for
// Update" actually needs it; begin()/end() bracket that window explicitly
// rather than anything auto-reconnecting in the background.
#pragma once
#include <WiFi.h>
#include <algorithm>
#include <vector>

struct WifiScanResult {
    String ssid;
    int32_t rssi;
};

class WifiManager {
public:
    void begin() {
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
    }

    // Call once done with WiFi (OTA check finished/failed) to give the
    // radio back to BT/BLE coexistence for the OBD link.
    void end() {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
    }

    std::vector<WifiScanResult> scan(uint8_t max_results = 8) {
        int n = WiFi.scanNetworks();
        std::vector<WifiScanResult> out;
        for (int i = 0; i < n; i++) {
            out.push_back({WiFi.SSID(i), WiFi.RSSI(i)});
        }
        std::sort(out.begin(), out.end(), [](const WifiScanResult &a, const WifiScanResult &b) {
            return a.rssi > b.rssi;
        });
        WiFi.scanDelete();
        if (out.size() > max_results) out.resize(max_results);
        return out;
    }

    bool connect(const String &ssid, const String &password, uint32_t timeout_ms = 15000) {
        WiFi.begin(ssid.c_str(), password.c_str());
        uint32_t deadline = millis() + timeout_ms;
        while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
            delay(200);
        }
        return WiFi.status() == WL_CONNECTED;
    }

    bool connected() const { return WiFi.status() == WL_CONNECTED; }
    String currentSSID() const { return WiFi.SSID(); }
};
