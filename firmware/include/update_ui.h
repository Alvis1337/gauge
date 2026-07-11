// Update Mode UI — shown when the user picks "Update / Upload Logs" at boot.
// Runs a blocking event loop (no OBD task, no NimBLE) so WiFi has the
// whole radio and the full heap. Two operations: OTA firmware check and
// OBD log upload to a configurable webhook URL.
#pragma once
#include <lvgl.h>
#include <WiFi.h>
#include "gauge_settings.h"
#include "wifi_manager.h"
#include "ota_updater.h"
#include "log_uploader.h"
#include "theme.h"

class UpdateUI {
public:
    // Blocks until the user taps "Restart in Gauge Mode".
    // Pass autoUpload=true (set when entering via "Upload to Discord" button)
    // to trigger the log upload automatically once WiFi connects.
    void run(GaugeSettings *settings, WifiManager *wifi, bool autoUpload = false) {
        _settings    = settings;
        _wifi        = wifi;
        _done        = false;
        _autoUpload  = autoUpload;
        _buildMainScreen();
        _buildWebhookScreen();
        lv_scr_load(_mainScreen);
        lv_timer_handler();
        _autoConnectWifi();

        while (!_done) {
            lv_timer_handler();
            _poll();
            delay(16);
        }
        _wifi->end();
    }

private:
    GaugeSettings *_settings    = nullptr;
    WifiManager   *_wifi        = nullptr;
    bool           _done        = false;
    bool           _autoUpload  = false;

    portMUX_TYPE _mux             = portMUX_INITIALIZER_UNLOCKED;
    String       _statusText;
    bool         _statusDirty    = false;
    bool         _otaInProgress  = false;
    bool         _uploadInProgress = false;

    lv_obj_t *_mainScreen    = nullptr;
    lv_obj_t *_wifiLabel     = nullptr;
    lv_obj_t *_webhookLabel  = nullptr;
    lv_obj_t *_statusLabel   = nullptr;
    lv_obj_t *_otaBtn        = nullptr;
    lv_obj_t *_uploadBtn     = nullptr;

    lv_obj_t *_webhookScreen   = nullptr;
    lv_obj_t *_webhookTextarea = nullptr;

    void _setStatus(String s) {
        portENTER_CRITICAL(&_mux);
        _statusText  = std::move(s);
        _statusDirty = true;
        portEXIT_CRITICAL(&_mux);
    }

    void _poll() {
        // Read the dirty flag AND move the status string in a single critical
        // section. Two separate locks would allow a new _setStatus() call to
        // land between them — leaving _statusDirty=true but _statusText empty
        // (already moved), so the next poll() would blank the label.
        // String::move is pointer-transfer only (no allocation), so it's safe
        // inside portENTER_CRITICAL.
        String status;
        portENTER_CRITICAL(&_mux);
        bool dirty = _statusDirty;
        if (dirty) { status = std::move(_statusText); _statusDirty = false; }
        portEXIT_CRITICAL(&_mux);
        if (!dirty) return;
        lv_label_set_text(_statusLabel, status.c_str());
        _refreshWifiLabel();
        _refreshWebhookLabel();
    }

    void _refreshWifiLabel() {
        if (!_wifiLabel) return;
        std::string ssid = _settings->wifiSsid();
        if (WiFi.status() == WL_CONNECTED)
            lv_label_set_text(_wifiLabel, ("WiFi: " + String(ssid.c_str())).c_str());
        else if (ssid.empty())
            lv_label_set_text(_wifiLabel, "WiFi: not configured (use Settings)");
        else
            lv_label_set_text(_wifiLabel, ("WiFi: " + String(ssid.c_str()) + "...").c_str());
    }

    void _refreshWebhookLabel() {
        if (!_webhookLabel) return;
        std::string url = _settings->logWebhookUrl();
        lv_label_set_text(_webhookLabel,
            url.empty() ? "Webhook: not set" : ("Webhook: " + String(url.c_str())).c_str());
    }

    void _buildMainScreen() {
        _mainScreen = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(_mainScreen, lv_color_hex(0x111111), 0);
        lv_obj_set_style_pad_all(_mainScreen, 12, 0);
        lv_obj_clear_flag(_mainScreen, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *title = lv_label_create(_mainScreen);
        lv_label_set_text(title, "Update Mode");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

        _wifiLabel = lv_label_create(_mainScreen);
        lv_obj_set_style_text_color(_wifiLabel, theme::subtext(), 0);
        lv_obj_align(_wifiLabel, LV_ALIGN_TOP_MID, 0, 40);
        _refreshWifiLabel();

        _webhookLabel = lv_label_create(_mainScreen);
        lv_obj_set_style_text_color(_webhookLabel, theme::subtext(), 0);
        lv_obj_set_style_text_font(_webhookLabel, &lv_font_montserrat_14, 0);
        lv_obj_align(_webhookLabel, LV_ALIGN_TOP_MID, 0, 62);
        _refreshWebhookLabel();

        _otaBtn = lv_button_create(_mainScreen);
        lv_obj_set_size(_otaBtn, 456, 44);
        lv_obj_align(_otaBtn, LV_ALIGN_TOP_MID, 0, 90);
        lv_obj_set_style_bg_color(_otaBtn, theme::ethanol(), 0);
        lv_obj_add_event_cb(_otaBtn, [](lv_event_t *e) {
            ((UpdateUI *)lv_event_get_user_data(e))->_startOta();
        }, LV_EVENT_CLICKED, this);
        lv_obj_t *otaLabel = lv_label_create(_otaBtn);
        lv_label_set_text(otaLabel, "Check for OTA Update");
        lv_obj_center(otaLabel);

        _uploadBtn = lv_button_create(_mainScreen);
        lv_obj_set_size(_uploadBtn, 456, 44);
        lv_obj_align(_uploadBtn, LV_ALIGN_TOP_MID, 0, 140);
        lv_obj_set_style_bg_color(_uploadBtn, lv_color_hex(0x2a5caa), 0);
        lv_obj_add_event_cb(_uploadBtn, [](lv_event_t *e) {
            ((UpdateUI *)lv_event_get_user_data(e))->_startUpload();
        }, LV_EVENT_CLICKED, this);
        lv_obj_t *uploadLabel = lv_label_create(_uploadBtn);
        lv_label_set_text(uploadLabel, "Upload OBD Log");
        lv_obj_center(uploadLabel);

        lv_obj_t *webhookBtn = lv_button_create(_mainScreen);
        lv_obj_set_size(webhookBtn, 456, 40);
        lv_obj_align(webhookBtn, LV_ALIGN_TOP_MID, 0, 190);
        lv_obj_set_style_bg_color(webhookBtn, lv_color_hex(0x2a2a2a), 0);
        lv_obj_add_event_cb(webhookBtn, [](lv_event_t *e) {
            auto *self = (UpdateUI *)lv_event_get_user_data(e);
            lv_textarea_set_text(self->_webhookTextarea,
                                 self->_settings->logWebhookUrl().c_str());
            lv_scr_load(self->_webhookScreen);
        }, LV_EVENT_CLICKED, this);
        lv_obj_t *webhookBtnLabel = lv_label_create(webhookBtn);
        lv_label_set_text(webhookBtnLabel, "Set Webhook URL");
        lv_obj_center(webhookBtnLabel);

        _statusLabel = lv_label_create(_mainScreen);
        lv_label_set_text(_statusLabel, "");
        lv_obj_set_style_text_color(_statusLabel, theme::subtext(), 0);
        lv_obj_align(_statusLabel, LV_ALIGN_TOP_MID, 0, 238);

        lv_obj_t *restartBtn = lv_button_create(_mainScreen);
        lv_obj_set_size(restartBtn, 456, 44);
        lv_obj_align(restartBtn, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(restartBtn, lv_color_hex(0x333333), 0);
        lv_obj_add_event_cb(restartBtn, [](lv_event_t *e) {
            ((UpdateUI *)lv_event_get_user_data(e))->_done = true;
        }, LV_EVENT_CLICKED, this);
        lv_obj_t *restartLabel = lv_label_create(restartBtn);
        lv_label_set_text(restartLabel, "< Restart in Gauge Mode");
        lv_obj_center(restartLabel);
    }

    void _buildWebhookScreen() {
        _webhookScreen = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(_webhookScreen, lv_color_hex(0x111111), 0);
        lv_obj_set_style_pad_all(_webhookScreen, 8, 0);
        lv_obj_clear_flag(_webhookScreen, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *backBtn = lv_button_create(_webhookScreen);
        lv_obj_set_size(backBtn, 70, 30);
        lv_obj_align(backBtn, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_add_event_cb(backBtn, [](lv_event_t *e) {
            lv_scr_load(((UpdateUI *)lv_event_get_user_data(e))->_mainScreen);
        }, LV_EVENT_CLICKED, this);
        lv_obj_t *backLabel = lv_label_create(backBtn);
        lv_label_set_text(backLabel, "< Back");
        lv_obj_center(backLabel);

        lv_obj_t *title = lv_label_create(_webhookScreen);
        lv_label_set_text(title, "Webhook URL");
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

        lv_obj_t *hint = lv_label_create(_webhookScreen);
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(hint, theme::subtext(), 0);
        lv_label_set_text(hint, "Discord webhook or any HTTP POST endpoint");
        lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 32);

        _webhookTextarea = lv_textarea_create(_webhookScreen);
        lv_textarea_set_one_line(_webhookTextarea, true);
        lv_obj_clear_flag(_webhookTextarea, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(_webhookTextarea, 370, 36);
        lv_obj_align(_webhookTextarea, LV_ALIGN_TOP_LEFT, 0, 54);

        lv_obj_t *saveBtn = lv_button_create(_webhookScreen);
        lv_obj_set_size(saveBtn, 80, 36);
        lv_obj_align(saveBtn, LV_ALIGN_TOP_RIGHT, 0, 54);
        lv_obj_add_event_cb(saveBtn, [](lv_event_t *e) {
            auto *self = (UpdateUI *)lv_event_get_user_data(e);
            std::string url = lv_textarea_get_text(self->_webhookTextarea);
            self->_settings->setLogWebhookUrl(url);
            self->_setStatus(url.empty() ? "Webhook URL cleared" : "Webhook URL saved");
            lv_scr_load(self->_mainScreen);
        }, LV_EVENT_CLICKED, this);
        lv_obj_t *saveLabel = lv_label_create(saveBtn);
        lv_label_set_text(saveLabel, "Save");
        lv_obj_center(saveLabel);

        lv_obj_t *kb = lv_keyboard_create(_webhookScreen);
        lv_keyboard_set_textarea(kb, _webhookTextarea);
        lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_clear_flag(kb, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    }

    void _autoConnectWifi() {
        std::string ssid = _settings->wifiSsid();
        if (ssid.empty()) return;
        _setStatus("Connecting to WiFi...");
        struct Ctx { UpdateUI *self; };
        auto *ctx = new Ctx{this};
        xTaskCreatePinnedToCore([](void *arg) {
            auto *ctx = (Ctx *)arg;
            obd_log::write("[upd] WiFi connecting...");
            ctx->self->_wifi->begin();
            bool ok = ctx->self->_wifi->connect(
                ctx->self->_settings->wifiSsid().c_str(),
                ctx->self->_settings->wifiPassword().c_str());
            if (!ok) {
                obd_log::write("[upd] WiFi failed");
                ctx->self->_setStatus("WiFi connection failed");
            } else if (ctx->self->_autoUpload) {
                obd_log::write("[upd] WiFi ok, uploading...");
                ctx->self->_setStatus("WiFi connected — uploading log...");
                std::string url = ctx->self->_settings->logWebhookUrl();
                String result = log_uploader::upload(url.c_str());
                if (result.isEmpty()) {
                    obd_log::write("[upd] upload ok");
                    ctx->self->_setStatus("Log uploaded!");
                } else {
                    obd_log::write("[upd] upload err: %s", result.c_str());
                    ctx->self->_setStatus(result);
                }
            } else {
                obd_log::write("[upd] WiFi ok");
                ctx->self->_setStatus("WiFi connected");
            }
            delete ctx;
            vTaskDelete(nullptr);
        }, "upd_wifi", 24576, ctx, 1, nullptr, 1);
    }

    void _startOta() {
        if (WiFi.status() != WL_CONNECTED) {
            _setStatus("Not connected to WiFi");
            return;
        }
        portENTER_CRITICAL(&_mux);
        bool already = _otaInProgress || _uploadInProgress;
        if (!already) _otaInProgress = true;
        portEXIT_CRITICAL(&_mux);
        if (already) return;

        _setStatus("Checking for update...");
        struct Ctx { UpdateUI *self; };
        auto *ctx = new Ctx{this};
        xTaskCreatePinnedToCore([](void *arg) {
            auto *ctx = (Ctx *)arg;
            obd_log::write("[upd] OTA check starting");
            String result = ota_updater::checkAndUpdate(*ctx->self->_settings);
            // Only reached if no update (update reboots the device).
            bool upToDate = (result == "up to date");
            obd_log::write("[upd] OTA: %s", result.c_str());
            portENTER_CRITICAL(&ctx->self->_mux);
            ctx->self->_otaInProgress = false;
            portEXIT_CRITICAL(&ctx->self->_mux);
            ctx->self->_setStatus(upToDate ? "Firmware up to date" : "OTA: " + result);
            delete ctx;
            vTaskDelete(nullptr);
        }, "upd_ota", 16384, ctx, 1, nullptr, 1);
    }

    void _startUpload() {
        if (WiFi.status() != WL_CONNECTED) {
            _setStatus("Not connected to WiFi");
            return;
        }
        portENTER_CRITICAL(&_mux);
        bool already = _uploadInProgress || _otaInProgress;
        if (!already) _uploadInProgress = true;
        portEXIT_CRITICAL(&_mux);
        if (already) return;

        _setStatus("Uploading OBD log...");
        struct Ctx { UpdateUI *self; };
        auto *ctx = new Ctx{this};
        xTaskCreatePinnedToCore([](void *arg) {
            auto *ctx = (Ctx *)arg;
            obd_log::write("[upd] manual upload...");
            std::string url = ctx->self->_settings->logWebhookUrl();
            String result = log_uploader::upload(url.c_str());
            portENTER_CRITICAL(&ctx->self->_mux);
            ctx->self->_uploadInProgress = false;
            portEXIT_CRITICAL(&ctx->self->_mux);
            if (result.isEmpty()) {
                obd_log::write("[upd] upload ok");
                ctx->self->_setStatus("Log uploaded!");
            } else {
                obd_log::write("[upd] upload err: %s", result.c_str());
                ctx->self->_setStatus(result);
            }
            delete ctx;
            vTaskDelete(nullptr);
        }, "upd_log", 24576, ctx, 1, nullptr, 1);
    }
};
