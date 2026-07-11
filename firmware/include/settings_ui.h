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
#include "obd_log.h"
#include "theme.h"

// Defined in main.cpp — sets RTC flag then reboots into Update Mode so the
// OBD log written this session is still readable for Discord upload.
void rebootToUpdateMode();

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
        _buildObdLogScreen();
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
        // Move _statusText atomically with the dirty-flag clear (same lock).
        // Two separate critical sections let a new _setStatus() land between
        // them: _statusDirty ends up true again but _statusText is already
        // empty, so the next poll() call would blank the label.
        String status;
        portENTER_CRITICAL(&_mux);
        bool scanDirty = _scanDirty;
        _scanDirty = false;
        bool btScanDirty = _btScanDirty;
        _btScanDirty = false;
        bool statusDirty = _statusDirty;
        if (statusDirty) { status = std::move(_statusText); _statusDirty = false; }
        bool connectRunning = _wifiConnectInProgress;
        portEXIT_CRITICAL(&_mux);

        if (scanDirty) {
            // No lock needed here: the scan task that set _scanDirty=true also
            // cleared _wifiScanInProgress in the same critical section, then
            // called vTaskDelete(). Both poll() and LVGL click handlers run on
            // core 1 sequentially, so no new scan can start while poll() runs —
            // _scanResults is stable for the duration of this call.
            _rebuildWifiList(_scanResults);
            // Free the WiFi driver heap (~30KB) now that the list is built, but
            // only if a connect task isn't also using the WiFi driver right now.
            if (!connectRunning) _wifi->end();
        }
        if (btScanDirty) {
            // Same reasoning as above — _btScanResults is stable on core 1.
            _rebuildObdList(_btScanResults);
        }
        if (statusDirty) {
            lv_label_set_text(_otaStatusLabel, status.c_str());
            lv_label_set_text(_wifiPasswordStatus, status.c_str());
        }
        if (obd_log::isDirty() && _obdLogScreen && lv_scr_act() == _obdLogScreen) {
            _refreshObdLog();
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
    lv_obj_t *_obdLogScreen = nullptr;
    lv_obj_t *_obdLogLabel = nullptr;

    lv_obj_t *_wifiStatusLabel = nullptr;
    lv_obj_t *_otaStatusLabel = nullptr;
    lv_obj_t *_wifiListContainer = nullptr;
    lv_obj_t *_wifiListStatusLabel = nullptr;
    lv_obj_t *_wifiPageLabel = nullptr;
    lv_obj_t *_wifiPrevBtn = nullptr;
    lv_obj_t *_wifiNextBtn = nullptr;
    size_t _wifiPage = 0;
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
    lv_obj_t *_obdPageLabel = nullptr;
    lv_obj_t *_obdPrevBtn = nullptr;
    lv_obj_t *_obdNextBtn = nullptr;
    size_t _obdPage = 0;

    portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
    std::vector<WifiScanResult> _scanResults;
    std::vector<BtScanResult> _btScanResults;
    bool _btScanDirty = false;
    bool _scanDirty = false;
    bool _wifiScanInProgress = false;
    bool _btScanInProgress = false;
    bool _wifiConnectInProgress = false;
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
            lv_obj_clean(self->_wifiListContainer);
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
            // Free WiFi now if the scan task has already finished.
            // If the scan is still running, leave the driver alone —
            // poll() will call end() once the task posts scanDirty.
            portENTER_CRITICAL(&self->_mux);
            bool busy = self->_wifiScanInProgress || self->_wifiConnectInProgress;
            portEXIT_CRITICAL(&self->_mux);
            if (!busy) self->_wifi->end();
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

        // Plain non-scrollable container. lv_list_create() forces LVGL into
        // layer-compositing mode (ARGB8888 stripe allocations) for the
        // clipped scroll area — 13 KB of contiguous heap per render pass.
        // That exhausts the heap when WiFi is also allocated (~35 KB).
        // A plain obj with absolute-positioned children avoids all of that.
        _wifiListContainer = lv_obj_create(_wifiListScreen);
        lv_obj_set_size(_wifiListContainer, 464, 216);
        lv_obj_align(_wifiListContainer, LV_ALIGN_TOP_MID, 0, 54);
        lv_obj_clear_flag(_wifiListContainer, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(_wifiListContainer, lv_color_hex(0x111111), 0);
        lv_obj_set_style_border_width(_wifiListContainer, 0, 0);
        lv_obj_set_style_pad_all(_wifiListContainer, 0, 0);

        _wifiPrevBtn = lv_button_create(_wifiListScreen);
        lv_obj_set_size(_wifiPrevBtn, 80, 28);
        lv_obj_align(_wifiPrevBtn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_add_state(_wifiPrevBtn, LV_STATE_DISABLED);
        lv_obj_add_event_cb(_wifiPrevBtn, [](lv_event_t *e) {
            auto *self = (SettingsUI *)lv_event_get_user_data(e);
            if (self->_wifiPage > 0) { self->_wifiPage--; self->_rebuildWifiList(self->_scanResults); }
        }, LV_EVENT_CLICKED, this);
        lv_obj_t *prevLabel = lv_label_create(_wifiPrevBtn);
        lv_label_set_text(prevLabel, "< Prev");
        lv_obj_center(prevLabel);

        _wifiPageLabel = lv_label_create(_wifiListScreen);
        lv_label_set_text(_wifiPageLabel, "");
        lv_obj_set_style_text_color(_wifiPageLabel, theme::subtext(), 0);
        lv_obj_align(_wifiPageLabel, LV_ALIGN_BOTTOM_MID, 0, -6);

        _wifiNextBtn = lv_button_create(_wifiListScreen);
        lv_obj_set_size(_wifiNextBtn, 80, 28);
        lv_obj_align(_wifiNextBtn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        lv_obj_add_state(_wifiNextBtn, LV_STATE_DISABLED);
        lv_obj_add_event_cb(_wifiNextBtn, [](lv_event_t *e) {
            auto *self = (SettingsUI *)lv_event_get_user_data(e);
            self->_wifiPage++;
            self->_rebuildWifiList(self->_scanResults);
        }, LV_EVENT_CLICKED, this);
        lv_obj_t *nextLabel = lv_label_create(_wifiNextBtn);
        lv_label_set_text(nextLabel, "Next >");
        lv_obj_center(nextLabel);
    }

    void _startWifiScan() {
        // Guard against double-tap spawning two concurrent scan tasks —
        // WiFi.scanNetworks() is not safe to call from two tasks at once.
        portENTER_CRITICAL(&_mux);
        bool already = _wifiScanInProgress || _wifiConnectInProgress;
        if (!already) _wifiScanInProgress = true;
        portEXIT_CRITICAL(&_mux);
        if (already) {
            lv_label_set_text(_wifiListStatusLabel, "Scanning...");
            return;
        }

        _wifiPage = 0;
        lv_label_set_text(_wifiListStatusLabel, "Scanning...");
        auto *self = this;
        xTaskCreatePinnedToCore([](void *arg) {
            auto *self = (SettingsUI *)arg;
            self->_wifi->begin();
            auto results = self->_wifi->scan();
            // end() is intentionally NOT called here — WiFi.mode(WIFI_OFF)
            // immediately after scanNetworks() destabilises the driver and
            // breaks the next scan. The radio is returned to BLE when the
            // user either connects to a network (connect task calls end())
            // or navigates back without picking one (settings screen load
            // calls end() via LV_EVENT_SCREEN_LOADED).
            // Move the OLD results out first so their String destructors run
            // AFTER the lock — free() inside portENTER_CRITICAL risks the
            // interrupt watchdog on a rescan that replaces existing entries.
            std::vector<WifiScanResult> old;
            portENTER_CRITICAL(&self->_mux);
            old = std::move(self->_scanResults);   // old was empty: no destr
            self->_scanResults = std::move(results); // dest now empty: safe
            self->_scanDirty = true;
            self->_wifiScanInProgress = false;
            portEXIT_CRITICAL(&self->_mux);
            // old destructs here — String::free() runs outside the spinlock
            vTaskDelete(nullptr);
        }, "wifi_scan", 8192, self, 1, nullptr, 1);
    }

    static const size_t WIFI_PAGE_SIZE = 5;

    void _rebuildWifiList(const std::vector<WifiScanResult> &results) {
        lv_obj_clean(_wifiListContainer);

        size_t total = results.size();
        size_t totalPages = total == 0 ? 1 : (total + WIFI_PAGE_SIZE - 1) / WIFI_PAGE_SIZE;
        if (_wifiPage >= totalPages) _wifiPage = 0;

        size_t pageStart = _wifiPage * WIFI_PAGE_SIZE;
        size_t pageEnd   = pageStart + WIFI_PAGE_SIZE;
        if (pageEnd > total) pageEnd = total;

        lv_label_set_text(_wifiListStatusLabel, total == 0 ? "No networks found" : "");

        for (size_t i = pageStart; i < pageEnd; i++) {
            String lbl = results[i].ssid + "  (" + String(results[i].rssi) + " dBm)";
            lv_obj_t *btn = lv_button_create(_wifiListContainer);
            lv_obj_set_size(btn, 464, 40);
            lv_obj_set_pos(btn, 0, (int)((i - pageStart) * 44));
            lv_obj_set_user_data(btn, (void *)(intptr_t)i);
            lv_obj_t *btnLabel = lv_label_create(btn);
            lv_label_set_text(btnLabel, lbl.c_str());
            lv_obj_align(btnLabel, LV_ALIGN_LEFT_MID, 8, 0);
            lv_obj_add_event_cb(btn, [](lv_event_t *e) {
                auto *self = (SettingsUI *)lv_event_get_user_data(e);
                intptr_t idx = (intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_current_target(e));
                char ssid_buf[64] = {};
                portENTER_CRITICAL(&self->_mux);
                bool valid = (size_t)idx < self->_scanResults.size() &&
                             !self->_scanResults[idx].ssid.isEmpty();
                if (valid)
                    strncpy(ssid_buf, self->_scanResults[idx].ssid.c_str(), sizeof(ssid_buf) - 1);
                portEXIT_CRITICAL(&self->_mux);
                if (!valid) return;
                String ssid(ssid_buf);
                self->_pendingSsid = ssid;
                lv_label_set_text(self->_wifiPasswordTitle, ("Connect to: " + ssid).c_str());
                lv_textarea_set_text(self->_wifiPasswordTextarea, "");
                lv_label_set_text(self->_wifiPasswordStatus, "");
                lv_scr_load(self->_wifiPasswordScreen);
            }, LV_EVENT_CLICKED, this);
        }

        char pageBuf[24];
        snprintf(pageBuf, sizeof(pageBuf), "Page %u / %u",
                 (unsigned)(_wifiPage + 1), (unsigned)totalPages);
        lv_label_set_text(_wifiPageLabel, pageBuf);

        if (_wifiPage == 0)
            lv_obj_add_state(_wifiPrevBtn, LV_STATE_DISABLED);
        else
            lv_obj_remove_state(_wifiPrevBtn, LV_STATE_DISABLED);

        if (_wifiPage + 1 >= totalPages)
            lv_obj_add_state(_wifiNextBtn, LV_STATE_DISABLED);
        else
            lv_obj_remove_state(_wifiNextBtn, LV_STATE_DISABLED);
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
        lv_obj_clear_flag(_wifiPasswordTextarea, LV_OBJ_FLAG_SCROLLABLE);
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
        portENTER_CRITICAL(&_mux);
        bool already = _wifiConnectInProgress;
        if (!already) _wifiConnectInProgress = true;
        portEXIT_CRITICAL(&_mux);
        if (already) return;

        String password = lv_textarea_get_text(_wifiPasswordTextarea);
        String ssid = _pendingSsid;
        _setStatus("Connecting...");

        struct Ctx { SettingsUI *self; String ssid; String password; };
        auto *ctx = new Ctx{this, ssid, password};
        xTaskCreatePinnedToCore([](void *arg) {
            auto *ctx = (Ctx *)arg;
            ctx->self->_wifi->begin();  // re-init WiFi (freed after scan to save heap)
            bool ok = ctx->self->_wifi->connect(ctx->ssid, ctx->password);
            // Clear flag before end() so poll()'s pending _wifi->end() (if a
            // scan completed while we were connecting) can finally run — and
            // so a second tap is unblocked the moment we're done.
            portENTER_CRITICAL(&ctx->self->_mux);
            ctx->self->_wifiConnectInProgress = false;
            portEXIT_CRITICAL(&ctx->self->_mux);
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
        }, "wifi_connect", 8192, ctx, 1, nullptr, 1);
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

        lv_obj_t *logBtn = lv_button_create(_obdScreen);
        lv_obj_set_size(logBtn, 464, 30);
        lv_obj_align(logBtn, LV_ALIGN_TOP_MID, 0, 108);
        lv_obj_set_style_bg_color(logBtn, lv_color_hex(0x2a2a2a), 0);
        lv_obj_add_event_cb(logBtn, [](lv_event_t *e) {
            auto *self = (SettingsUI *)lv_event_get_user_data(e);
            self->_refreshObdLog();
            lv_scr_load(self->_obdLogScreen);
        }, LV_EVENT_CLICKED, this);
        lv_obj_t *logBtnLabel = lv_label_create(logBtn);
        lv_label_set_text(logBtnLabel, "View OBD Log");
        lv_obj_center(logBtnLabel);

        _obdListContainer = lv_obj_create(_obdScreen);
        lv_obj_set_size(_obdListContainer, 464, 128);
        lv_obj_align(_obdListContainer, LV_ALIGN_TOP_MID, 0, 142);
        lv_obj_clear_flag(_obdListContainer, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(_obdListContainer, lv_color_hex(0x111111), 0);
        lv_obj_set_style_border_width(_obdListContainer, 0, 0);
        lv_obj_set_style_pad_all(_obdListContainer, 0, 0);

        _obdPrevBtn = lv_button_create(_obdScreen);
        lv_obj_set_size(_obdPrevBtn, 80, 28);
        lv_obj_align(_obdPrevBtn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_add_state(_obdPrevBtn, LV_STATE_DISABLED);
        lv_obj_add_event_cb(_obdPrevBtn, [](lv_event_t *e) {
            auto *self = (SettingsUI *)lv_event_get_user_data(e);
            if (self->_obdPage > 0) { self->_obdPage--; self->_rebuildObdList(self->_btScanResults); }
        }, LV_EVENT_CLICKED, this);
        lv_obj_t *obdPrevLabel = lv_label_create(_obdPrevBtn);
        lv_label_set_text(obdPrevLabel, "< Prev");
        lv_obj_center(obdPrevLabel);

        _obdPageLabel = lv_label_create(_obdScreen);
        lv_label_set_text(_obdPageLabel, "");
        lv_obj_set_style_text_color(_obdPageLabel, theme::subtext(), 0);
        lv_obj_align(_obdPageLabel, LV_ALIGN_BOTTOM_MID, 0, -6);

        _obdNextBtn = lv_button_create(_obdScreen);
        lv_obj_set_size(_obdNextBtn, 80, 28);
        lv_obj_align(_obdNextBtn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        lv_obj_add_state(_obdNextBtn, LV_STATE_DISABLED);
        lv_obj_add_event_cb(_obdNextBtn, [](lv_event_t *e) {
            auto *self = (SettingsUI *)lv_event_get_user_data(e);
            self->_obdPage++;
            self->_rebuildObdList(self->_btScanResults);
        }, LV_EVENT_CLICKED, this);
        lv_obj_t *obdNextLabel = lv_label_create(_obdNextBtn);
        lv_label_set_text(obdNextLabel, "Next >");
        lv_obj_center(obdNextLabel);
    }

    void _openObdScreen() {
        _refreshObdStatusLabel();
        if (_settings->autoDiscoveryEnabled()) lv_obj_add_state(_obdAutoDiscoverSwitch, LV_STATE_CHECKED);
        else lv_obj_remove_state(_obdAutoDiscoverSwitch, LV_STATE_CHECKED);
        lv_scr_load(_obdScreen);
    }

    void _startObdScan() {
        portENTER_CRITICAL(&_mux);
        bool already = _btScanInProgress;
        if (!already) _btScanInProgress = true;
        portEXIT_CRITICAL(&_mux);
        if (already) return;

        _obdPage = 0;
        lv_label_set_text(_obdListStatusLabel, "Scanning...");
        auto *self = this;
        xTaskCreatePinnedToCore([](void *arg) {
            auto *self = (SettingsUI *)arg;
            auto results = bt_discovery::scan();
            std::vector<BtScanResult> oldBt;
            portENTER_CRITICAL(&self->_mux);
            oldBt = std::move(self->_btScanResults);
            self->_btScanResults = std::move(results);
            self->_btScanDirty = true;
            self->_btScanInProgress = false;
            portEXIT_CRITICAL(&self->_mux);
            vTaskDelete(nullptr);
        }, "obd_scan", 8192, self, 1, nullptr, 1);
    }

    static const size_t OBD_PAGE_SIZE = 3;

    void _rebuildObdList(const std::vector<BtScanResult> &results) {
        lv_obj_clean(_obdListContainer);

        size_t total = results.size();
        size_t totalPages = total == 0 ? 1 : (total + OBD_PAGE_SIZE - 1) / OBD_PAGE_SIZE;
        if (_obdPage >= totalPages) _obdPage = 0;

        size_t pageStart = _obdPage * OBD_PAGE_SIZE;
        size_t pageEnd   = pageStart + OBD_PAGE_SIZE;
        if (pageEnd > total) pageEnd = total;

        lv_label_set_text(_obdListStatusLabel, total == 0 ? "No devices found" : "");

        for (size_t i = pageStart; i < pageEnd; i++) {
            String lbl = String(results[i].name.c_str());
            if (bt_discovery::looksLikeObdName(results[i].name)) lbl += "  (OBD)";
            lv_obj_t *btn = lv_button_create(_obdListContainer);
            lv_obj_set_size(btn, 464, 40);
            lv_obj_set_pos(btn, 0, (int)((i - pageStart) * 44));
            lv_obj_set_user_data(btn, (void *)(intptr_t)i);
            lv_obj_t *btnLabel = lv_label_create(btn);
            lv_label_set_text(btnLabel, lbl.c_str());
            lv_obj_align(btnLabel, LV_ALIGN_LEFT_MID, 8, 0);
            lv_obj_add_event_cb(btn, [](lv_event_t *e) {
                auto *self = (SettingsUI *)lv_event_get_user_data(e);
                intptr_t idx = (intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_target(e));
                char addr_buf[32] = {};
                char name_buf[64] = {};
                portENTER_CRITICAL(&self->_mux);
                bool valid = (size_t)idx < self->_btScanResults.size();
                if (valid) {
                    strncpy(addr_buf, self->_btScanResults[idx].address.c_str(), sizeof(addr_buf) - 1);
                    strncpy(name_buf, self->_btScanResults[idx].name.c_str(), sizeof(name_buf) - 1);
                }
                portEXIT_CRITICAL(&self->_mux);
                if (!valid) return;
                self->_settings->setObdBtAddress(addr_buf);
                self->_settings->setObdBtName(name_buf);
                self->_settings->requestObdReconnect();
                self->_refreshObdStatusLabel();
                lv_label_set_text(self->_obdListStatusLabel, "Saved -- reconnecting...");
            }, LV_EVENT_CLICKED, this);
        }

        char pageBuf[24];
        snprintf(pageBuf, sizeof(pageBuf), "Page %u / %u",
                 (unsigned)(_obdPage + 1), (unsigned)totalPages);
        lv_label_set_text(_obdPageLabel, pageBuf);

        if (_obdPage == 0)
            lv_obj_add_state(_obdPrevBtn, LV_STATE_DISABLED);
        else
            lv_obj_remove_state(_obdPrevBtn, LV_STATE_DISABLED);

        if (_obdPage + 1 >= totalPages)
            lv_obj_add_state(_obdNextBtn, LV_STATE_DISABLED);
        else
            lv_obj_remove_state(_obdNextBtn, LV_STATE_DISABLED);
    }

    // ── OBD log screen ───────────────────────────────────────────────────
    void _buildObdLogScreen() {
        _obdLogScreen = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(_obdLogScreen, lv_color_hex(0x111111), 0);
        lv_obj_set_style_pad_all(_obdLogScreen, 8, 0);
        lv_obj_clear_flag(_obdLogScreen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(_obdLogScreen, [](lv_event_t *e) {
            auto *self = (SettingsUI *)lv_event_get_user_data(e);
            self->_refreshObdLog();
        }, LV_EVENT_SCREEN_LOADED, this);

        lv_obj_t *backBtn = lv_button_create(_obdLogScreen);
        lv_obj_set_size(backBtn, 70, 30);
        lv_obj_align(backBtn, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_add_event_cb(backBtn, [](lv_event_t *e) {
            auto *self = (SettingsUI *)lv_event_get_user_data(e);
            lv_scr_load(self->_obdScreen);
        }, LV_EVENT_CLICKED, this);
        lv_obj_t *backLabel = lv_label_create(backBtn);
        lv_label_set_text(backLabel, "< Back");
        lv_obj_center(backLabel);

        lv_obj_t *title = lv_label_create(_obdLogScreen);
        lv_label_set_text(title, "OBD Log");
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

        lv_obj_t *uploadBtn = lv_button_create(_obdLogScreen);
        lv_obj_set_size(uploadBtn, 130, 30);
        lv_obj_align(uploadBtn, LV_ALIGN_TOP_RIGHT, 0, 0);
        lv_obj_set_style_bg_color(uploadBtn, lv_color_hex(0x2a5caa), 0);
        lv_obj_add_event_cb(uploadBtn, [](lv_event_t *) {
            rebootToUpdateMode();
        }, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *uploadBtnLabel = lv_label_create(uploadBtn);
        lv_label_set_text(uploadBtnLabel, "Upload to Discord");
        lv_obj_center(uploadBtnLabel);

        lv_obj_t *logBox = lv_obj_create(_obdLogScreen);
        lv_obj_set_size(logBox, 464, 258);
        lv_obj_align(logBox, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(logBox, lv_color_hex(0x1a1a1a), 0);
        lv_obj_set_style_pad_all(logBox, 6, 0);
        lv_obj_clear_flag(logBox, LV_OBJ_FLAG_SCROLLABLE);

        _obdLogLabel = lv_label_create(logBox);
        lv_obj_set_style_text_font(_obdLogLabel, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(_obdLogLabel, lv_color_hex(0xc0c0c0), 0);
        lv_obj_set_width(_obdLogLabel, lv_pct(100));
        lv_label_set_long_mode(_obdLogLabel, LV_LABEL_LONG_WRAP);
        lv_label_set_text(_obdLogLabel, "No OBD activity yet");
    }

    void _refreshObdLog() {
        if (!_obdLogLabel) return;
        static char lines[obd_log::LINES][obd_log::LINE_LEN];
        size_t n = obd_log::snapshot(lines, obd_log::LINES);
        if (n == 0) { lv_label_set_text(_obdLogLabel, "No OBD activity yet"); return; }
        // Show only the last 15 lines — fits the fixed-height box at
        // font_montserrat_14 without any scrolling.
        static const size_t OBD_LOG_DISPLAY = 15;
        size_t start = n > OBD_LOG_DISPLAY ? n - OBD_LOG_DISPLAY : 0;
        String text;
        text.reserve((n - start) * obd_log::LINE_LEN);
        for (size_t i = start; i < n; i++) {
            text += lines[i];
            if (i + 1 < n) text += '\n';
        }
        lv_label_set_text(_obdLogLabel, text.c_str());
    }

    // ── OTA ───────────────────────────────────────────────────────────────
    // Running OTA in-place while NimBLE is active fails: mbedTLS needs
    // ~32KB heap for TLS buffers but NimBLE is already holding ~40KB.
    // The boot-time try_auto_ota() runs before NimBLEDevice::init() and
    // has plenty of heap — so "Check for Update" just reboots into it.
    // The boot splash shows progress/result exactly as it does on a
    // normal power-on update check.
    void _startOtaCheck() {
        std::string ssid = _settings->wifiSsid();
        if (ssid.empty()) {
            _setStatus("No WiFi configured — set it up first");
            return;
        }
        _setStatus("Rebooting to check for update...");
        xTaskCreatePinnedToCore([](void *) {
            vTaskDelay(pdMS_TO_TICKS(1500));
            ESP.restart();
            vTaskDelete(nullptr);
        }, "ota_reboot", 2048, nullptr, 1, nullptr, 1);
    }
};
