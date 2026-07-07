// Persistent settings in ESP32 NVS flash — firmware equivalent of
// settings.py's JSON file. Namespace + key-value like the Python
// _DEFAULTS dict, just backed by flash instead of a file.
#pragma once
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

private:
    Preferences _prefs;
};
