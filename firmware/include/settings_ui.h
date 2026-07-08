// Settings screen: WiFi (scan/connect, used only for OTA checks), manual
// "Check for Update", touch calibration, and OBD adapter selection.
// Reached via a long-press on the gauge screen's bottom-right corner,
// mirroring the Pi build's UX.
//
// Background work (WiFi scan, WiFi connect, OTA fetch, OBD scan) runs on
// its own FreeRTOS task rather than blocking the UI — but LVGL itself
// isn't thread-safe, so those tasks never call lv_* directly. They write
// into a mutex-protected status struct that the main loop (the only thing
// that calls lv_timer_handler()) polls once per frame and reflects into
// labels — the same pattern main.cpp already uses for OBD/gauge data.
#pragma once
#include <lvgl.h>
#include <algorithm>
#include <vector>
#include "gauge_settings.h"
#include "wifi_manager.h"
#include "ota_updater.h"
#include "xpt2046_driver.h"
#include "bt_discovery.h"
#include "theme.h"

class SettingsUI {
public:
    void init(GaugeSettings *settings, WifiManager *wifi, XPT2046Driver *touch) {
        _settings = settings;
        _wifi = wifi;
        _touch = touch;
        _buildSettingsScreen();
        _buildWifiListScreen();
        _buildWifiPasswordScreen();
        _buildTouchCalScreen();
        _buildObdScreen();
        _refreshObdStatusLabel();
    }

    lv_obj_t *settingsScreen() { return _settingsScreen; }

    // Attach to the gauge screen's corner button:
    // lv_obj_add_event_cb(corner, SettingsUI::openFromGauge, LV_EVENT_LONG_PRESSED, &settingsUI);
    static void openFromGauge(lv_event_t *e) {
        auto *self = (SettingsUI *)lv_event_get_user_data(e);
        self->_refreshWifiStatusLabel();
        lv_scr_load(self->_settingsScreen);
    }

    // Call once per frame from loop() — reflects background task progress
    // (scan results, connect/OTA status) into the UI without any other
    // task touching LVGL directly.
    void poll() {
        // Every heap-allocating copy (String, vector<WifiScanResult>) used
        // to happen unconditionally inside this critical section, on every
        // single call — i.e. 60 times a second, forever, disabling
        // interrupts around allocator work regardless of whether anything
        // had actually changed. ESP-IDF explicitly warns against blocking
        // or allocating inside a critical section (it stalls the other
        // core / delays interrupts long enough to risk watchdog resets),
        // so only take the lock, and only copy, when a background task
        // actually posted something new.
        portENTER_CRITICAL(&_mux);
        bool scanDirty = _scanDirty;
        _scanDirty = false;
        bool btScanDirty = _btScanDirty;
        _btScanDirty = false;
        bool statusDirty = _statusDirty;
        _statusDirty = false;
        portEXIT_CRITICAL(&_mux);

        if (scanDirty) {
            std::vector<WifiScanResult> results;
            portENTER_CRITICAL(&_mux);
            results = std::move(_scanResults);
            portEXIT_CRITICAL(&_mux);
            _rebuildWifiList(results);
        }
        if (btScanDirty) {
            std::vector<BtScanResult> results;
            portENTER_CRITICAL(&_mux);
            results = std::move(_btScanResults);
            portEXIT_CRITICAL(&_mux);
            _rebuildObdList(results);
        }
        if (statusDirty) {
            String status;
            portENTER_CRITICAL(&_mux);
            status = std::move(_statusText);
            portEXIT_CRITICAL(&_mux);
            lv_label_set_text(_otaStatusLabel, status.c_str());
            lv_label_set_text(_wifiPasswordStatus, status.c_str());
        }
    }

private:
    GaugeSettings *_settings = nullptr;
    WifiManager *_wifi = nullptr;
    XPT2046Driver *_touch = nullptr;

    lv_obj_t *_settingsScreen = nullptr;
    lv_obj_t *_wifiListScreen = nullptr;
    lv_obj_t *_wifiPasswordScreen = nullptr;
    lv_obj_t *_touchCalScreen = nullptr;
    lv_obj_t *_obdScreen = nullptr;

    lv_obj_t *_wifiStatusLabel = nullptr;
    lv_obj_t *_otaStatusLabel = nullptr;
    lv_obj_t *_wifiListContainer = nullptr;
    lv_obj_t *_wifiListStatusLabel = nullptr;
    lv_obj_t *_wifiPasswordTitle = nullptr;
    lv_obj_t *_wifiPasswordTextarea = nullptr;
    lv_obj_t *_wifiPasswordStatus = nullptr;

    lv_obj_t *_touchCalStepLabel = nullptr;
    lv_obj_t *_touchCalTarget = nullptr;
    lv_obj_t *_touchCalResultLabel = nullptr;
    lv_obj_t *_touchCalActionBtn = nullptr;
    lv_obj_t *_touchCalActionLabel = nullptr;
    int _touchCalStep = 0;
    bool _touchCalBad = false;
    int _touchCalRawX[4] = {0, 0, 0, 0};
    int _touchCalRawY[4] = {0, 0, 0, 0};

    lv_obj_t *_obdStatusLabel = nullptr;
    lv_obj_t *_obdCurrentLabel = nullptr;
    lv_obj_t *_obdAutoDiscoverSwitch = nullptr;
    lv_obj_t *_obdListContainer = nullptr;
    lv_obj_t *_obdListStatusLabel = nullptr;

    portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
    std::vector<WifiScanResult> _scanResults;
    std::vector<BtScanResult> _btScanResults;
    bool _btScanDirty = false;
    bool _scanDirty = false;
    String _statusText;
    bool _statusDirty = false;
    String _pendingSsid;

    void _setStatus(String s) {
        portENTER_CRITICAL(&_mux);
        _statusText = std::move(s);
        _statusDirty = true;
        portEXIT_CRITICAL(&_mux);
    }

    void _refreshWifiStatusLabel() {
        std::string ssid = _settings->wifiSsid();
        String text = ssid.empty() ? "WiFi: not configured"
                                   : ("WiFi: " + String(ssid.c_str()));
        lv_label_set_text(_wifiStatusLabel, text.c_str());
    }

    // Updates both the settings-screen button label and the OBD Adapter
    // screen's own "current adapter" label — called synchronously (this
    // never runs off the UI thread) whenever the saved adapter changes.
    void _refreshObdStatusLabel() {
        std::string name = _settings->obdBtName();
        std::string addr = _settings->obdBtAddress();
        String text;
        if (!name.empty())      text = "OBD Adapter: " + String(name.c_str());
        else if (!addr.empty()) text = "OBD Adapter: " + String(addr.c_str());
        else                     text = "OBD Adapter: not configured";
        if (_obdStatusLabel)  lv_label_set_text(_obdStatusLabel, text.c_str());
        if (_obdCurrentLabel) lv_label_set_text(_obdCurrentLabel, text.c_str());
    }

    // ── settings screen ──────────────────────────────────────────────────
    void _buildSettingsScreen() {
        _settingsScreen = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(_settingsScreen, lv_color_hex(0x111111), 0);
        lv_obj_set_style_pad_all(_settingsScreen, 12, 0);
        lv_obj_clear_flag(_settingsScreen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(_settingsScreen, [](lv_event_t *e) {
            auto *self = (SettingsUI *)lv_event_get_user_data(e);
            self->_refreshWifiStatusLabel();
        }, LV_EVENT_SCREEN_LOADED, this);

        lv_obj_t *title = lv_label_create(_settingsScreen);
        lv_label_set_text(title, "Settings");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

        lv_obj_t *wifiBtn = lv_button_create(_settingsScreen);
        lv_obj_set_size(wifiBtn, 456, 40);
        lv_obj_align(wifiBtn, LV_ALIGN_TOP_MID, 0, 46);
        lv_obj_add_event_cb(wifiBtn, [](lv_event_t *e) {
            auto *self = (SettingsUI *)lv_event_get_user_data(e);
            self->_startWifiScan();
            lv_scr_load(self->_wifiListScreen);
        }, LV_EVENT_CLICKED, this);
        _wifiStatusLabel = lv_label_create(wifiBtn);
        lv_label_set_text(_wifiStatusLabel, "WiFi: not configured");
        lv_obj_center(_wifiStatusLabel);

        lv_obj_t *obdBtn = lv_button_create(_settingsScreen);
        lv_obj_set_size(obdBtn, 456, 40);
        lv_obj_align(obdBtn, LV_ALIGN_TOP_MID, 0, 90);
        lv_obj_add_event_cb(obdBtn, [](lv_event_t *e) {
            auto *self = (SettingsUI *)lv_event_get_user_data(e);
            self->_openObdScreen();
        }, LV_EVENT_CLICKED, this);
        _obdStatusLabel = lv_label_create(obdBtn);
        lv_label_set_text(_obdStatusLabel, "OBD Adapter: not configured");
        lv_obj_center(_obdStatusLabel);

        lv_obj_t *otaBtn = lv_button_create(_settingsScreen);
        lv_obj_set_size(otaBtn, 456, 40);
        lv_obj_align(otaBtn, LV_ALIGN_TOP_MID, 0, 134);
        lv_obj_set_style_bg_color(otaBtn, theme::ethanol(), 0);
        lv_obj_add_event_cb(otaBtn, [](lv_event_t *e) {
            auto *self = (SettingsUI *)lv_event_get_user_data(e);
            self->_startOtaCheck();
        }, LV_EVENT_CLICKED, this);
        lv_obj_t *otaBtnLabel = lv_label_create(otaBtn);
        lv_label_set_text(otaBtnLabel, "Check for Update");
        lv_obj_center(otaBtnLabel);

        lv_obj_t *touchCalBtn = lv_button_create(_settingsScreen);
        lv_obj_set_size(touchCalBtn, 456, 40);
        lv_obj_align(touchCalBtn, LV_ALIGN_TOP_MID, 0, 178);
        lv_obj_add_event_cb(touchCalBtn, [](lv_event_t *e) {
            auto *self = (SettingsUI *)lv_event_get_user_data(e);
            self->_openTouchCal();
        }, LV_EVENT_CLICKED, this);
        lv_obj_t *touchCalBtnLabel = lv_label_create(touchCalBtn);
        lv_label_set_text(touchCalBtnLabel, "Touch Calibrate");
        lv_obj_center(touchCalBtnLabel);

        _otaStatusLabel = lv_label_create(_settingsScreen);
        lv_label_set_text(_otaStatusLabel, "");
        lv_obj_set_style_text_color(_otaStatusLabel, theme::subtext(), 0);
        lv_obj_align(_otaStatusLabel, LV_ALIGN_TOP_MID, 0, 226);

        lv_obj_t *backBtn = lv_button_create(_settingsScreen);
        lv_obj_set_size(backBtn, 456, 44);
        lv_obj_align(backBtn, LV_ALIGN_BOTTOM_MID, 0, 0);
        // Back goes to whichever screen the caller registered as "home" —
        // wired up from main.cpp via setHomeScreen() instead of hardcoding
        // the gauge screen here, so this header doesn't need to know
        // main.cpp's gauge-screen globals.
        lv_obj_add_event_cb(backBtn, [](lv_event_t *e) {
            auto *self = (SettingsUI *)lv_event_get_user_data(e);
            if (self->_homeScreen) lv_scr_load(self->_homeScreen);
        }, LV_EVENT_CLICKED, this);
        lv_obj_t *backLabel = lv_label_create(backBtn);
        lv_label_set_text(backLabel, "< Back");
        lv_obj_center(backLabel);
    }

public:
    void setHomeScreen(lv_obj_t *scr) { _homeScreen = scr; }

private:
    lv_obj_t *_homeScreen = nullptr;

    // ── WiFi list screen ─────────────────────────────────────────────────
    void _buildWifiListScreen() {
        _wifiListScreen = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(_wifiListScreen, lv_color_hex(0x111111), 0);
        lv_obj_set_style_pad_all(_wifiListScreen, 8, 0);
        lv_obj_clear_flag(_wifiListScreen, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *backBtn = lv_button_create(_wifiListScreen);
        lv_obj_set_size(backBtn, 70, 30);
        lv_obj_align(backBtn, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_add_event_cb(backBtn, [](lv_event_t *e) {
            auto *self = (SettingsUI *)lv_event_get_user_data(e);
            self->_refreshWifiStatusLabel();
            lv_scr_load(self->_settingsScreen);
        }, LV_EVENT_CLICKED, this);
        lv_obj_t *backLabel = lv_label_create(backBtn);
        lv_label_set_text(backLabel, "< Back");
        lv_obj_center(backLabel);

        lv_obj_t *title = lv_label_create(_wifiListScreen);
        lv_label_set_text(title, "WiFi Networks");
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

        lv_obj_t *rescanBtn = lv_button_create(_wifiListScreen);
        lv_obj_set_size(rescanBtn, 80, 30);
        lv_obj_align(rescanBtn, LV_ALIGN_TOP_RIGHT, 0, 0);
        lv_obj_add_event_cb(rescanBtn, [](lv_event_t *e) {
            auto *self = (SettingsUI *)lv_event_get_user_data(e);
            self->_startWifiScan();
        }, LV_EVENT_CLICKED, this);
        lv_obj_t *rescanLabel = lv_label_create(rescanBtn);
        lv_label_set_text(rescanLabel, "Scan");
        lv_obj_center(rescanLabel);

        _wifiListStatusLabel = lv_label_create(_wifiListScreen);
        lv_label_set_text(_wifiListStatusLabel, "");
        lv_obj_set_style_text_color(_wifiListStatusLabel, theme::subtext(), 0);
        lv_obj_align(_wifiListStatusLabel, LV_ALIGN_TOP_MID, 0, 32);

        _wifiListContainer = lv_list_create(_wifiListScreen);
        lv_obj_set_size(_wifiListContainer, 464, 240);
        lv_obj_align(_wifiListContainer, LV_ALIGN_BOTTOM_MID, 0, 0);
    }

    void _startWifiScan() {
        lv_label_set_text(_wifiListStatusLabel, "Scanning...");
        auto *self = this;
        xTaskCreate([](void *arg) {
            auto *self = (SettingsUI *)arg;
            self->_wifi->begin();
            auto results = self->_wifi->scan();
            self->_wifi->end();  // give the radio back to BLE; connect() calls begin() again
            portENTER_CRITICAL(&self->_mux);
            self->_scanResults = std::move(results);
            self->_scanDirty = true;
            portEXIT_CRITICAL(&self->_mux);
            vTaskDelete(nullptr);
        }, "wifi_scan", 8192, self, 1, nullptr);
    }

    void _rebuildWifiList(const std::vector<WifiScanResult> &results) {
        lv_obj_clean(_wifiListContainer);
        lv_label_set_text(_wifiListStatusLabel, results.empty() ? "No networks found" : "");
        for (size_t i = 0; i < results.size(); i++) {
            String label = results[i].ssid + "  (" + String(results[i].rssi) + " dBm)";
            lv_obj_t *btn = lv_list_add_button(_wifiListContainer, nullptr, label.c_str());
            // Index into the last scan result, not a heap-allocated
            // closure — safe as long as _scanResults isn't mutated between
            // rebuild and tap, which holds since a rescan replaces the
            // whole list+button set together.
            lv_obj_set_user_data(btn, (void *)(intptr_t)i);
            lv_obj_add_event_cb(btn, [](lv_event_t *e) {
                auto *self = (SettingsUI *)lv_event_get_user_data(e);
                intptr_t idx = (intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_target(e));
                portENTER_CRITICAL(&self->_mux);
                String ssid = (size_t)idx < self->_scanResults.size() ? self->_scanResults[idx].ssid : "";
                portEXIT_CRITICAL(&self->_mux);
                if (ssid.isEmpty()) return;
                self->_pendingSsid = ssid;
                lv_label_set_text(self->_wifiPasswordTitle, ("Connect to: " + ssid).c_str());
                lv_textarea_set_text(self->_wifiPasswordTextarea, "");
                lv_label_set_text(self->_wifiPasswordStatus, "");
                lv_scr_load(self->_wifiPasswordScreen);
            }, LV_EVENT_CLICKED, this);
        }
    }

    // ── WiFi password screen ─────────────────────────────────────────────
    void _buildWifiPasswordScreen() {
        _wifiPasswordScreen = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(_wifiPasswordScreen, lv_color_hex(0x111111), 0);
        lv_obj_set_style_pad_all(_wifiPasswordScreen, 8, 0);
        lv_obj_clear_flag(_wifiPasswordScreen, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *backBtn = lv_button_create(_wifiPasswordScreen);
        lv_obj_set_size(backBtn, 70, 30);
        lv_obj_align(backBtn, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_add_event_cb(backBtn, [](lv_event_t *e) {
            auto *self = (SettingsUI *)lv_event_get_user_data(e);
            self->_refreshWifiStatusLabel();
            lv_scr_load(self->_wifiListScreen);
        }, LV_EVENT_CLICKED, this);
        lv_obj_t *backLabel = lv_label_create(backBtn);
        lv_label_set_text(backLabel, "< Back");
        lv_obj_center(backLabel);

        _wifiPasswordTitle = lv_label_create(_wifiPasswordScreen);
        lv_label_set_text(_wifiPasswordTitle, "Connect to: ");
        lv_obj_align(_wifiPasswordTitle, LV_ALIGN_TOP_MID, 0, 4);

        _wifiPasswordTextarea = lv_textarea_create(_wifiPasswordScreen);
        lv_textarea_set_one_line(_wifiPasswordTextarea, true);
        lv_textarea_set_password_mode(_wifiPasswordTextarea, true);
        lv_obj_set_size(_wifiPasswordTextarea, 300, 36);
        lv_obj_align(_wifiPasswordTextarea, LV_ALIGN_TOP_MID, -60, 34);

        lv_obj_t *connectBtn = lv_button_create(_wifiPasswordScreen);
        lv_obj_set_size(connectBtn, 110, 36);
        lv_obj_align(connectBtn, LV_ALIGN_TOP_RIGHT, 0, 34);
        lv_obj_add_event_cb(connectBtn, [](lv_event_t *e) {
            auto *self = (SettingsUI *)lv_event_get_user_data(e);
            self->_startWifiConnect();
        }, LV_EVENT_CLICKED, this);
        lv_obj_t *connectLabel = lv_label_create(connectBtn);
        lv_label_set_text(connectLabel, "Connect");
        lv_obj_center(connectLabel);

        _wifiPasswordStatus = lv_label_create(_wifiPasswordScreen);
        lv_label_set_text(_wifiPasswordStatus, "");
        lv_obj_align(_wifiPasswordStatus, LV_ALIGN_TOP_MID, 0, 76);

        lv_obj_t *kb = lv_keyboard_create(_wifiPasswordScreen);
        lv_keyboard_set_textarea(kb, _wifiPasswordTextarea);
        lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_clear_flag(kb, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    }

    void _startWifiConnect() {
        String password = lv_textarea_get_text(_wifiPasswordTextarea);
        String ssid = _pendingSsid;
        _setStatus("Connecting...");

        struct Ctx { SettingsUI *self; String ssid; String password; };
        auto *ctx = new Ctx{this, ssid, password};
        xTaskCreate([](void *arg) {
            auto *ctx = (Ctx *)arg;
            bool ok = ctx->self->_wifi->connect(ctx->ssid, ctx->password);
            if (ok) {
                ctx->self->_settings->setWifiCredentials(ctx->ssid.c_str(), ctx->password.c_str());
                ctx->self->_wifi->end();  // only needed transiently — give the radio back to BLE
                ctx->self->_setStatus("Connected! Saved.");
            } else {
                ctx->self->_wifi->end();
                ctx->self->_setStatus("Failed — check password");
            }
            delete ctx;
            vTaskDelete(nullptr);
        }, "wifi_connect", 8192, ctx, 1, nullptr);
    }

    // ── touch calibration screen ─────────────────────────────────────────
    // Tap the 4 corners in turn; whatever raw ADC reading was under the
    // finger at each tap is recorded (not the calibrated pixel position —
    // that's the whole point, since the existing calibration may be wrong).
    // The screen background itself is the clickable target: LVGL's own
    // click detection already rides on the debounced press/release from
    // xpt2046_driver.h, so "a tap happened" is exactly the same signal the
    // gauge screen's corner long-press already relies on.
    struct _CalTarget { int x, y; const char *label; };
    static _CalTarget _calTarget(int step) {
        static const _CalTarget kTargets[4] = {
            {20, 20, "Top-left"},
            {460, 20, "Top-right"},
            {460, 300, "Bottom-right"},
            {20, 300, "Bottom-left"},
        };
        return kTargets[step];
    }

    void _buildTouchCalScreen() {
        _touchCalScreen = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(_touchCalScreen, lv_color_hex(0x111111), 0);
        // No padding here (unlike the other screens) — target coordinates
        // below are absolute panel pixels, and any default content-area
        // inset would throw off where the crosshair actually lands.
        lv_obj_set_style_pad_all(_touchCalScreen, 0, 0);
        lv_obj_clear_flag(_touchCalScreen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(_touchCalScreen, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(_touchCalScreen, [](lv_event_t *e) {
            auto *self = (SettingsUI *)lv_event_get_user_data(e);
            self->_onTouchCalTap();
        }, LV_EVENT_CLICKED, this);

        lv_obj_t *title = lv_label_create(_touchCalScreen);
        lv_label_set_text(title, "Touch Calibration");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

        _touchCalStepLabel = lv_label_create(_touchCalScreen);
        lv_obj_set_style_text_color(_touchCalStepLabel, theme::subtext(), 0);
        lv_obj_align(_touchCalStepLabel, LV_ALIGN_TOP_MID, 0, 40);

        _touchCalTarget = lv_obj_create(_touchCalScreen);
        lv_obj_remove_style_all(_touchCalTarget);
        // Plain lv_obj instances default to clickable (unlike labels, which
        // clear it in their own constructor) — without this, a tap landing
        // exactly on the dot would be swallowed here instead of reaching
        // the screen background's tap handler below.
        lv_obj_clear_flag(_touchCalTarget, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(_touchCalTarget, 20, 20);
        lv_obj_set_style_radius(_touchCalTarget, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(_touchCalTarget, theme::ethanol(), 0);
        lv_obj_set_style_bg_opa(_touchCalTarget, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(_touchCalTarget, 2, 0);
        lv_obj_set_style_border_color(_touchCalTarget, lv_color_white(), 0);

        _touchCalResultLabel = lv_label_create(_touchCalScreen);
        lv_obj_set_style_text_font(_touchCalResultLabel, &lv_font_montserrat_28, 0);
        lv_obj_align(_touchCalResultLabel, LV_ALIGN_CENTER, 0, -20);
        lv_obj_add_flag(_touchCalResultLabel, LV_OBJ_FLAG_HIDDEN);

        _touchCalActionBtn = lv_button_create(_touchCalScreen);
        lv_obj_set_size(_touchCalActionBtn, 160, 44);
        lv_obj_align(_touchCalActionBtn, LV_ALIGN_CENTER, 0, 30);
        lv_obj_add_event_cb(_touchCalActionBtn, [](lv_event_t *e) {
            auto *self = (SettingsUI *)lv_event_get_user_data(e);
            self->_onTouchCalAction();
        }, LV_EVENT_CLICKED, this);
        _touchCalActionLabel = lv_label_create(_touchCalActionBtn);
        lv_obj_center(_touchCalActionLabel);
        lv_obj_add_flag(_touchCalActionBtn, LV_OBJ_FLAG_HIDDEN);
    }

    void _openTouchCal() {
        _touchCalStep = 0;
        _touchCalBad = false;
        lv_obj_add_flag(_touchCalResultLabel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_touchCalActionBtn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(_touchCalTarget, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(_touchCalStepLabel, LV_OBJ_FLAG_HIDDEN);
        _updateTouchCalStepUI();
        lv_scr_load(_touchCalScreen);
    }

    void _updateTouchCalStepUI() {
        _CalTarget t = _calTarget(_touchCalStep);
        lv_obj_align(_touchCalTarget, LV_ALIGN_TOP_LEFT, t.x - 10, t.y - 10);
        char buf[48];
        snprintf(buf, sizeof(buf), "Tap: %s  (Step %d/4)", t.label, _touchCalStep + 1);
        lv_label_set_text(_touchCalStepLabel, buf);
    }

    void _onTouchCalTap() {
        if (_touchCalStep >= 4) return;  // bad-read/done state — only the action button responds
        int rx, ry;
        _touch->lastRaw(&rx, &ry);
        _touchCalRawX[_touchCalStep] = rx;
        _touchCalRawY[_touchCalStep] = ry;
        _touchCalStep++;
        if (_touchCalStep < 4) {
            _updateTouchCalStepUI();
        } else {
            _finishTouchCal();
        }
    }

    void _finishTouchCal() {
        int xMin = _touchCalRawX[0], xMax = _touchCalRawX[0];
        int yMin = _touchCalRawY[0], yMax = _touchCalRawY[0];
        for (int i = 1; i < 4; i++) {
            xMin = std::min(xMin, _touchCalRawX[i]);
            xMax = std::max(xMax, _touchCalRawX[i]);
            yMin = std::min(yMin, _touchCalRawY[i]);
            yMax = std::max(yMax, _touchCalRawY[i]);
        }

        lv_obj_add_flag(_touchCalTarget, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_touchCalStepLabel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(_touchCalResultLabel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(_touchCalActionBtn, LV_OBJ_FLAG_HIDDEN);

        // Same "bad reads" rejection threshold as the Pi build's TouchCalScreen.
        if (xMax - xMin < 200 || yMax - yMin < 200) {
            _touchCalBad = true;
            lv_label_set_text(_touchCalResultLabel, "Bad reads -- try again");
            lv_obj_set_style_text_color(_touchCalResultLabel, theme::danger(), 0);
            lv_label_set_text(_touchCalActionLabel, "Retry");
            return;
        }

        _touchCalBad = false;
        _settings->setTouchCal(xMin, xMax, yMin, yMax);
        _touch->setCalibration(xMin, xMax, yMin, yMax);
        lv_label_set_text(_touchCalResultLabel, "Calibration saved!");
        lv_obj_set_style_text_color(_touchCalResultLabel, theme::success(), 0);
        lv_label_set_text(_touchCalActionLabel, "Done");
    }

    void _onTouchCalAction() {
        if (_touchCalBad) {
            _touchCalStep = 0;
            lv_obj_add_flag(_touchCalResultLabel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(_touchCalActionBtn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(_touchCalTarget, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(_touchCalStepLabel, LV_OBJ_FLAG_HIDDEN);
            _updateTouchCalStepUI();
        } else {
            lv_scr_load(_settingsScreen);
        }
    }

    // ── OBD adapter screen ────────────────────────────────────────────────
    // Auto-discovery (bt_discovery::discover(), driven by obd_task) stays
    // on by default and needs no UI at all when it just works. This
    // screen is for the two cases it doesn't handle: turning it off for
    // someone who'd rather pick a specific device, and picking that
    // device — a scan-and-tap list mirroring the WiFi screen, except
    // selecting an entry here is a plain settings write (no connect
    // attempt happens in the picker itself), so it runs directly in the
    // UI-thread click handler instead of needing its own background task.
    void _buildObdScreen() {
        _obdScreen = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(_obdScreen, lv_color_hex(0x111111), 0);
        lv_obj_set_style_pad_all(_obdScreen, 8, 0);
        lv_obj_clear_flag(_obdScreen, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *backBtn = lv_button_create(_obdScreen);
        lv_obj_set_size(backBtn, 70, 30);
        lv_obj_align(backBtn, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_add_event_cb(backBtn, [](lv_event_t *e) {
            auto *self = (SettingsUI *)lv_event_get_user_data(e);
            lv_scr_load(self->_settingsScreen);
        }, LV_EVENT_CLICKED, this);
        lv_obj_t *backLabel = lv_label_create(backBtn);
        lv_label_set_text(backLabel, "< Back");
        lv_obj_center(backLabel);

        lv_obj_t *title = lv_label_create(_obdScreen);
        lv_label_set_text(title, "OBD Adapter");
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

        lv_obj_t *rescanBtn = lv_button_create(_obdScreen);
        lv_obj_set_size(rescanBtn, 80, 30);
        lv_obj_align(rescanBtn, LV_ALIGN_TOP_RIGHT, 0, 0);
        lv_obj_add_event_cb(rescanBtn, [](lv_event_t *e) {
            auto *self = (SettingsUI *)lv_event_get_user_data(e);
            self->_startObdScan();
        }, LV_EVENT_CLICKED, this);
        lv_obj_t *rescanLabel = lv_label_create(rescanBtn);
        lv_label_set_text(rescanLabel, "Scan");
        lv_obj_center(rescanLabel);

        _obdCurrentLabel = lv_label_create(_obdScreen);
        lv_obj_set_style_text_color(_obdCurrentLabel, theme::subtext(), 0);
        lv_obj_align(_obdCurrentLabel, LV_ALIGN_TOP_MID, 0, 32);

        lv_obj_t *autoLabel = lv_label_create(_obdScreen);
        lv_label_set_text(autoLabel, "Auto-discovery");
        lv_obj_align(autoLabel, LV_ALIGN_TOP_LEFT, 0, 58);

        _obdAutoDiscoverSwitch = lv_switch_create(_obdScreen);
        lv_obj_align(_obdAutoDiscoverSwitch, LV_ALIGN_TOP_RIGHT, 0, 54);
        lv_obj_add_event_cb(_obdAutoDiscoverSwitch, [](lv_event_t *e) {
            auto *self = (SettingsUI *)lv_event_get_user_data(e);
            bool enabled = lv_obj_has_state(self->_obdAutoDiscoverSwitch, LV_STATE_CHECKED);
            self->_settings->setAutoDiscoveryEnabled(enabled);
        }, LV_EVENT_VALUE_CHANGED, this);

        _obdListStatusLabel = lv_label_create(_obdScreen);
        lv_label_set_text(_obdListStatusLabel, "");
        lv_obj_set_style_text_color(_obdListStatusLabel, theme::subtext(), 0);
        lv_obj_align(_obdListStatusLabel, LV_ALIGN_TOP_MID, 0, 88);

        _obdListContainer = lv_list_create(_obdScreen);
        lv_obj_set_size(_obdListContainer, 464, 186);
        lv_obj_align(_obdListContainer, LV_ALIGN_BOTTOM_MID, 0, 0);
    }

    void _openObdScreen() {
        _refreshObdStatusLabel();
        if (_settings->autoDiscoveryEnabled()) lv_obj_add_state(_obdAutoDiscoverSwitch, LV_STATE_CHECKED);
        else lv_obj_remove_state(_obdAutoDiscoverSwitch, LV_STATE_CHECKED);
        lv_scr_load(_obdScreen);
    }

    void _startObdScan() {
        lv_label_set_text(_obdListStatusLabel, "Scanning...");
        auto *self = this;
        xTaskCreate([](void *arg) {
            auto *self = (SettingsUI *)arg;
            auto results = bt_discovery::scan();
            portENTER_CRITICAL(&self->_mux);
            self->_btScanResults = std::move(results);
            self->_btScanDirty = true;
            portEXIT_CRITICAL(&self->_mux);
            vTaskDelete(nullptr);
        }, "obd_scan", 8192, self, 1, nullptr);
    }

    void _rebuildObdList(const std::vector<BtScanResult> &results) {
        lv_obj_clean(_obdListContainer);
        lv_label_set_text(_obdListStatusLabel, results.empty() ? "No devices found" : "");
        for (size_t i = 0; i < results.size(); i++) {
            String label = String(results[i].name.c_str());
            if (bt_discovery::looksLikeObdName(results[i].name)) label += "  (looks like OBD)";
            lv_obj_t *btn = lv_list_add_button(_obdListContainer, nullptr, label.c_str());
            // Same index-into-last-scan-result approach as the WiFi list —
            // safe since a rescan replaces the whole result set + list
            // together, so there's no window where the index is stale.
            lv_obj_set_user_data(btn, (void *)(intptr_t)i);
            lv_obj_add_event_cb(btn, [](lv_event_t *e) {
                auto *self = (SettingsUI *)lv_event_get_user_data(e);
                intptr_t idx = (intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_target(e));
                portENTER_CRITICAL(&self->_mux);
                bool valid = (size_t)idx < self->_btScanResults.size();
                std::string addr = valid ? self->_btScanResults[idx].address : "";
                std::string name = valid ? self->_btScanResults[idx].name : "";
                portEXIT_CRITICAL(&self->_mux);
                if (!valid) return;
                // Just a settings write + a signal for obd_task to retry
                // now — no connect attempt happens here, so this can run
                // straight in the click callback with no background task.
                self->_settings->setObdBtAddress(addr);
                self->_settings->setObdBtName(name);
                self->_settings->requestObdReconnect();
                self->_refreshObdStatusLabel();
                lv_label_set_text(self->_obdListStatusLabel, "Saved -- reconnecting...");
            }, LV_EVENT_CLICKED, this);
        }
    }

    // ── OTA ───────────────────────────────────────────────────────────────
    void _startOtaCheck() {
        _setStatus("Connecting to WiFi...");
        auto *self = this;
        xTaskCreate([](void *arg) {
            auto *self = (SettingsUI *)arg;
            String ssid = self->_settings->wifiSsid().c_str();
            String password = self->_settings->wifiPassword().c_str();
            if (ssid.isEmpty()) {
                self->_setStatus("No WiFi configured — set it up above first");
                vTaskDelete(nullptr);
                return;
            }
            self->_wifi->begin();
            if (!self->_wifi->connect(ssid, password)) {
                self->_wifi->end();
                self->_setStatus("Could not reach WiFi \"" + ssid + "\"");
                vTaskDelete(nullptr);
                return;
            }
            self->_setStatus("Downloading update...");
            String result = ota_updater::checkAndUpdate(*self->_settings);
            // Only reached when there's nothing to flash — success path
            // reboots from inside checkAndUpdate().
            self->_wifi->end();
            self->_setStatus(result == "up to date" ? "Already up to date" : "Update failed: " + result);
            vTaskDelete(nullptr);
        }, "ota_check", 16384, self, 1, nullptr);
    }
};
