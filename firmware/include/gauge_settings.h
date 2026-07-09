// Persistent settings in ESP32 NVS flash — firmware equivalent of
// settings.py's JSON file. Namespace + key-value like the Python
// _DEFAULTS dict, just backed by flash instead of a file.
#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <string>

class GaugeSettings {
public:
    void load() { _prefs.begin("autogauge", /*readOnly=*/false); }

    std::string obdBtAddress() { return _prefs.getString("obd_bt_addr", "").c_str(); }
    void setObdBtAddress(const std::string &v) { _prefs.putString("obd_bt_addr", v.c_str()); }

    std::string obdBtName() { return _prefs.getString("obd_bt_name", "").c_str(); }
    void setObdBtName(const std::string &v) { _prefs.putString("obd_bt_name", v.c_str()); }

    std::string wifiSsid() { return _prefs.getString("wifi_ssid", "").c_str(); }
    std::string wifiPassword() { return _prefs.getString("wifi_pass", "").c_str(); }
    void setWifiCredentials(const std::string &ssid, const std::string &password) {
        _prefs.putString("wifi_ssid", ssid.c_str());
        _prefs.putString("wifi_pass", password.c_str());
    }

    // The GitHub release asset's ETag we last successfully flashed — lets
    // the OTA updater skip the download+flash+reboot cycle when nothing
    // actually changed, instead of doing it on every single boot just
    // because WiFi happened to be in range.
    std::string lastOtaEtag() { return _prefs.getString("ota_etag", "").c_str(); }
    void setLastOtaEtag(const std::string &v) { _prefs.putString("ota_etag", v.c_str()); }

    // Touch calibration — defaults measured from the same physical panel
    // as the Pi build (see touch.py's _DEFAULT_* constants).
    int touchXMin() { return _prefs.getInt("touch_x_min", 350); }
    int touchXMax() { return _prefs.getInt("touch_x_max", 3800); }
    int touchYMin() { return _prefs.getInt("touch_y_min", 350); }
    int touchYMax() { return _prefs.getInt("touch_y_max", 3900); }
    void setTouchCal(int x_min, int x_max, int y_min, int y_max) {
        _prefs.putInt("touch_x_min", x_min);
        _prefs.putInt("touch_x_max", x_max);
        _prefs.putInt("touch_y_min", y_min);
        _prefs.putInt("touch_y_max", y_max);
    }

    // Whether obd_task should fall back to bt_discovery::discover() after
    // repeated connect failures. Defaults on; the settings UI's manual
    // "scan and pick" OBD Adapter screen is the way to turn it off for
    // someone who'd rather point at a specific device than let the
    // name-hint heuristic guess.
    bool autoDiscoveryEnabled() { return _prefs.getBool("obd_auto_disc", true); }
    void setAutoDiscoveryEnabled(bool v) { _prefs.putBool("obd_auto_disc", v); }

    // Webhook URL for log upload (Discord or any HTTP POST endpoint).
    std::string logWebhookUrl() { return _prefs.getString("log_webhook", "").c_str(); }
    void setLogWebhookUrl(const std::string &v) { _prefs.putString("log_webhook", v.c_str()); }

    // In-memory only (never persisted) — lets the settings UI tell
    // obd_task to retry right now after the address changes, instead of
    // waiting out whatever's left of the current backoff delay (up to
    // 30s). Firmware equivalent of the Pi build's force_reconnect_evt.
    void requestObdReconnect() {
        portENTER_CRITICAL(&_reconnectMux);
        _reconnectRequested = true;
        portEXIT_CRITICAL(&_reconnectMux);
    }
    bool consumeObdReconnectRequest() {
        portENTER_CRITICAL(&_reconnectMux);
        bool r = _reconnectRequested;
        _reconnectRequested = false;
        portEXIT_CRITICAL(&_reconnectMux);
        return r;
    }

private:
    Preferences _prefs;
    portMUX_TYPE _reconnectMux = portMUX_INITIALIZER_UNLOCKED;
    bool _reconnectRequested = false;
};
