// AutoGauge firmware — ESP32 DevKitC, ST7796 480x320 SPI TFT + XPT2046
// touch, BLE ELM327 OBD-II adapter. Replaces the Raspberry Pi build.
#include <Arduino.h>
#include <SPI.h>
#include <NimBLEDevice.h>
#include <lvgl.h>
#include <algorithm>

#include "st7796_driver.h"
#include "xpt2046_driver.h"
#include "obd_client.h"
#include "bt_discovery.h"
#include "gauge_settings.h"
#include "gauge_widget.h"
#include "theme.h"
#include "wifi_manager.h"
#include "ota_updater.h"
#include "settings_ui.h"

// ── pin map (see wiring table) ──────────────────────────────────────────────
static constexpr int PIN_SCLK    = 18;
static constexpr int PIN_MOSI    = 23;
static constexpr int PIN_MISO    = 19;
static constexpr int PIN_LCD_CS  = 5;
static constexpr int PIN_LCD_DC  = 26;
static constexpr int PIN_LCD_RST = 25;
static constexpr int PIN_TOUCH_CS = 27;

static constexpr int DISPLAY_W = 480;
static constexpr int DISPLAY_H = 320;
static constexpr uint32_t POLL_INTERVAL_MS = 100;
static constexpr int DISCOVERY_AFTER_FAILURES = 3;

SPIClass gSpi(VSPI);
ST7796Driver gDisplay(PIN_LCD_CS, PIN_LCD_DC, PIN_LCD_RST, gSpi);
XPT2046Driver gTouch(PIN_TOUCH_CS, gSpi);
GaugeSettings gSettings;
WifiManager gWifi;
SettingsUI gSettingsUi;

// ── shared OBD state (written by the BLE task, read by the UI loop) ────────
struct SharedGaugeData {
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    GaugeData data;
    bool connected = false;
};
SharedGaugeData gShared;

// ── LVGL plumbing ────────────────────────────────────────────────────────────
// Partial-render buffers, 16 rows tall each (~15KB) rather than a full
// frame — this is a slow-changing gauge UI, not video, so the extra flush
// calls cost nothing perceptible, and it leaves more heap free for
// NimBLE's BLE stack.
static lv_color_t gDrawBuf1[DISPLAY_W * 16];
static lv_color_t gDrawBuf2[DISPLAY_W * 16];

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    // LVGL gives native-endian RGB565; the panel wants big-endian over SPI.
    uint16_t *px = (uint16_t *)px_map;
    uint32_t count = w * h;
    for (uint32_t i = 0; i < count; i++) px[i] = (px[i] >> 8) | (px[i] << 8);
    gDisplay.blit(area->x1, area->y1, area->x2, area->y2, px, count);
    lv_display_flush_ready(disp);
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    int px, py;
    if (gTouch.read(DISPLAY_W, DISPLAY_H, &px, &py)) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = px;
        data->point.y = py;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// ── gauge screen ─────────────────────────────────────────────────────────────
GaugeWidget gBoostGauge, gEthanolGauge, gCoolantGauge, gOilGauge;
lv_obj_t *gConnDot = nullptr;
lv_obj_t *gGaugeScreen = nullptr;

// ── boot splash ──────────────────────────────────────────────────────────────
// try_auto_ota() runs synchronously in setup(), before build_gauge_screen()
// exists — without this, the panel just sits there showing nothing (or
// whatever garbage was left in the display's RAM) for however long the WiFi
// connect timeout + download takes. gBootLabel gets updated at each stage so
// the user knows the board hasn't hung.
lv_obj_t *gBootLabel = nullptr;

static void build_boot_screen() {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "AutoGauge");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -20);

    gBootLabel = lv_label_create(scr);
    lv_label_set_text(gBootLabel, "Starting...");
    lv_obj_set_style_text_color(gBootLabel, theme::subtext(), 0);
    lv_obj_align(gBootLabel, LV_ALIGN_CENTER, 0, 20);
}

// Nothing else calls lv_timer_handler() while try_auto_ota() blocks in
// setup(), so each status change has to flush itself to actually show up.
static void set_boot_status(const char *text) {
    if (!gBootLabel) return;
    lv_label_set_text(gBootLabel, text);
    lv_timer_handler();
}

static void build_gauge_screen() {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);  // drop the boot splash's title/status labels
    gBootLabel = nullptr;  // that label object is now deleted — don't leave a dangling pointer
    gGaugeScreen = scr;
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 6, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *grid = lv_obj_create(scr);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, DISPLAY_W - 12, DISPLAY_H - 12);
    static int32_t col_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static int32_t row_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(grid, col_dsc, row_dsc);
    lv_obj_set_layout(grid, LV_LAYOUT_GRID);
    lv_obj_set_style_pad_column(grid, 6, 0);
    lv_obj_set_style_pad_row(grid, 6, 0);
    lv_obj_center(grid);

    auto cell = [&](int col, int row) {
        lv_obj_t *c = lv_obj_create(grid);
        lv_obj_remove_style_all(c);
        lv_obj_set_grid_cell(c, LV_GRID_ALIGN_STRETCH, col, 1, LV_GRID_ALIGN_STRETCH, row, 1);
        return c;
    };

    gBoostGauge.create(cell(0, 0),   "BOOST",   -5.0f,  25.0f, theme::boost(),   "%.1f psi");
    gEthanolGauge.create(cell(1, 0), "ETH %",   0.0f, 100.0f, theme::ethanol(), "%.0f%%");
    gCoolantGauge.create(cell(0, 1), "COOLANT", 40.0f, 130.0f, theme::coolant(), "%.0f C");
    gOilGauge.create(cell(1, 1),     "OIL",     40.0f, 150.0f, theme::oil(),     "%.0f C");

    // Small connection-state dot, top-right — green = connected, red = not.
    gConnDot = lv_obj_create(scr);
    lv_obj_remove_style_all(gConnDot);
    lv_obj_set_size(gConnDot, 10, 10);
    lv_obj_set_style_radius(gConnDot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(gConnDot, theme::danger(), 0);
    lv_obj_set_style_bg_opa(gConnDot, LV_OPA_COVER, 0);
    lv_obj_align(gConnDot, LV_ALIGN_TOP_RIGHT, -4, 4);

    // Long-press bottom-right corner -> Settings, same gesture as the Pi build.
    lv_obj_t *corner = lv_obj_create(scr);
    lv_obj_remove_style_all(corner);
    lv_obj_add_flag(corner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(corner, 80, 60);
    lv_obj_align(corner, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(corner, SettingsUI::openFromGauge, LV_EVENT_LONG_PRESSED, &gSettingsUi);
}

static void update_gauge_screen() {
    GaugeData d;
    bool connected;
    portENTER_CRITICAL(&gShared.mux);
    d = gShared.data;
    connected = gShared.connected;
    portEXIT_CRITICAL(&gShared.mux);

    gBoostGauge.setValue(d.boost_psi);
    gEthanolGauge.setValue(d.ethanol);
    gCoolantGauge.setValue(d.coolant_c);
    gOilGauge.setValue(d.oil_temp_c);
    lv_obj_set_style_bg_color(gConnDot, connected ? theme::success() : theme::danger(), 0);
}

// ── BLE OBD polling task (own core, blocks freely on BLE I/O) ──────────────
static void obd_task(void *) {
    ObdClient client;
    uint32_t retry_delay_ms = 2000;
    int consecutive_failures = 0;

    for (;;) {
        std::string address = gSettings.obdBtAddress();
        if (address.empty() || !client.connect(address)) {
            consecutive_failures++;
            portENTER_CRITICAL(&gShared.mux);
            gShared.connected = false;
            portEXIT_CRITICAL(&gShared.mux);
        } else {
            consecutive_failures = 0;
            retry_delay_ms = 2000;
            portENTER_CRITICAL(&gShared.mux);
            gShared.connected = true;
            portEXIT_CRITICAL(&gShared.mux);

            while (client.connected) {
                std::string current = gSettings.obdBtAddress();
                // Settings changed elsewhere (address edited, or the
                // settings UI's OBD Adapter picker explicitly requested a
                // reconnect after saving a new pick) — reconnect fresh
                // rather than keep polling whatever we're currently on.
                if (current != address || gSettings.consumeObdReconnectRequest()) break;
                GaugeData d = client.poll();
                portENTER_CRITICAL(&gShared.mux);
                gShared.data = d;
                gShared.connected = client.connected;
                portEXIT_CRITICAL(&gShared.mux);
                vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
            }
            client.disconnect();
        }

        if (gSettings.autoDiscoveryEnabled() && consecutive_failures >= DISCOVERY_AFTER_FAILURES) {
            std::string found = bt_discovery::discover();
            if (!found.empty() && found != address) {
                gSettings.setObdBtAddress(found);
                gSettings.setObdBtName("");
                retry_delay_ms = 2000;
            }
            consecutive_failures = 0;
        }

        // Same backoff as before, but polled in short slices so a manual
        // pick from the OBD Adapter screen takes effect right away
        // instead of waiting out up to 30s of remaining backoff.
        for (uint32_t waited = 0; waited < retry_delay_ms; waited += 100) {
            if (gSettings.consumeObdReconnectRequest()) break;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        retry_delay_ms = std::min<uint32_t>(retry_delay_ms * 2, 30000);
    }
}

// Runs once at boot, before BLE init, so the whole check happens in a clean
// WiFi-only window rather than fighting the OBD BLE link for radio time
// (display/LVGL are already up by this point, purely so the boot splash
// can show progress instead of a blank panel). Skips instantly if WiFi was
// never configured; otherwise gives it a short, bounded timeout so a
// missing hotspot (e.g. phone not tethered yet) doesn't meaningfully delay
// the gauges coming up.
static void try_auto_ota() {
    std::string ssid = gSettings.wifiSsid();
    if (ssid.empty()) return;

    set_boot_status("Checking for update...");
    Serial.println("checking for firmware update...");
    gWifi.begin();
    if (!gWifi.connect(ssid.c_str(), gSettings.wifiPassword().c_str(), /*timeout_ms=*/8000)) {
        Serial.println("update check: WiFi unreachable, continuing boot");
        set_boot_status("Update check: WiFi unreachable");
        gWifi.end();
        delay(1000);
        return;
    }
    set_boot_status("Downloading update...");
    String result = ota_updater::checkAndUpdate(gSettings);
    // Only reached when there's nothing to flash — success reboots from
    // inside checkAndUpdate().
    Serial.printf("update check: %s\n", result.c_str());
    gWifi.end();
    set_boot_status(result == "up to date" ? "Firmware up to date"
                                            : ("Update failed: " + result).c_str());
    delay(1500);
}

// Non-blocking USB-serial command console — lets WiFi/OTA/touch be tested
// and debugged without a working touchscreen (added while chasing the
// touch wiring issue, but useful for headless bring-up generally).
// Commands: "wifi <ssid> <password>", "ota", "reboot", "touch", "status".
static void serial_console_poll() {
    static String line;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c != '\n') { line += c; continue; }
        line.trim();

        if (line.startsWith("wifi ")) {
            // No second token means an open network (empty password) —
            // "wifi LamaFi" is a legitimate command, not a usage error.
            int sp = line.indexOf(' ', 5);
            String ssid = sp < 0 ? line.substring(5) : line.substring(5, sp);
            String password = sp < 0 ? "" : line.substring(sp + 1);
            if (ssid.isEmpty()) {
                Serial.println("usage: wifi <ssid> [password]");
            } else {
                gSettings.setWifiCredentials(ssid.c_str(), password.c_str());
                Serial.printf("saved WiFi credentials for \"%s\" — 'ota' to check now, or reboot\n", ssid.c_str());
            }
        } else if (line == "ota") {
            xTaskCreate([](void *) {
                std::string ssid = gSettings.wifiSsid();
                if (ssid.empty()) {
                    Serial.println("no WiFi configured — use: wifi <ssid> <password>");
                    vTaskDelete(nullptr);
                    return;
                }
                Serial.println("connecting to WiFi...");
                gWifi.begin();
                if (!gWifi.connect(ssid.c_str(), gSettings.wifiPassword().c_str())) {
                    Serial.println("could not connect to WiFi");
                    gWifi.end();
                    vTaskDelete(nullptr);
                    return;
                }
                Serial.println("downloading update...");
                String result = ota_updater::checkAndUpdate(gSettings);
                Serial.printf("ota result: %s\n", result.c_str());
                gWifi.end();
                vTaskDelete(nullptr);
            }, "ota_cmd", 16384, nullptr, 1, nullptr);
        } else if (line == "reboot") {
            ESP.restart();
        } else if (line == "touch") {
            // Single non-blocking sample — run this command repeatedly
            // while pressing the panel; no blocking loop so the UI stays live.
            int rx, ry, z1, z2;
            gTouch.readDiag(&rx, &ry, &z1, &z2);
            int pressure = z1 > 0 ? (z1 - z2 + 4095) : 0;
            Serial.printf("touch: x=%4d y=%4d  z1=%4d z2=%4d  pressure=%5d %s\n",
                          rx, ry, z1, z2, pressure,
                          pressure >= XPT2046Driver::PRESSURE_THRESHOLD ? "<-- TOUCH" : "");
        } else if (line == "status") {
            Serial.printf("free heap: %u bytes\n", ESP.getFreeHeap());
            Serial.printf("wifi ssid: \"%s\"\n", gSettings.wifiSsid().c_str());
            Serial.printf("obd address: \"%s\"\n", gSettings.obdBtAddress().c_str());
            portENTER_CRITICAL(&gShared.mux);
            bool connected = gShared.connected;
            portEXIT_CRITICAL(&gShared.mux);
            Serial.printf("obd connected: %s\n", connected ? "yes" : "no");
        } else if (line == "gpio") {
            // Read GPIO19 (MISO) with pull-up enabled to see if something is
            // actively driving it LOW, or if it's just floating.
            pinMode(PIN_MISO, INPUT_PULLUP);
            delayMicroseconds(100);
            int miso_pu = digitalRead(PIN_MISO);
            pinMode(PIN_MISO, INPUT_PULLDOWN);
            delayMicroseconds(100);
            int miso_pd = digitalRead(PIN_MISO);
            Serial.printf("GPIO19 (MISO) with pull-up: %d  with pull-down: %d\n", miso_pu, miso_pd);
            if (miso_pu == 0) Serial.println("  -> MISO is being driven LOW by something external");
            else              Serial.println("  -> MISO is floating (nothing driving it)");
            // Also pulse T_CS manually to verify GPIO27 toggles
            pinMode(PIN_TOUCH_CS, OUTPUT);
            digitalWrite(PIN_TOUCH_CS, LOW);
            delayMicroseconds(10);
            int miso_cs_low = digitalRead(PIN_MISO);
            digitalWrite(PIN_TOUCH_CS, HIGH);
            Serial.printf("GPIO19 while T_CS pulsed LOW: %d\n", miso_cs_low);
            // Restore SPI MISO pin function
            gSpi.begin(PIN_SCLK, PIN_MISO, PIN_MOSI, -1);
        } else if (!line.isEmpty()) {
            Serial.println("commands: wifi <ssid> <password> | ota | reboot | touch | gpio | status");
        }
        line = "";
    }
}

// ── setup / loop ─────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.printf("AutoGauge starting, free heap: %u bytes\n", ESP.getFreeHeap());

    gSettings.load();
    gTouch.setCalibration(gSettings.touchXMin(), gSettings.touchXMax(),
                           gSettings.touchYMin(), gSettings.touchYMax());

    gSpi.begin(PIN_SCLK, PIN_MISO, PIN_MOSI, -1);
    gDisplay.begin();
    gTouch.begin();

    lv_init();
    lv_tick_set_cb([]() -> uint32_t { return millis(); });

    lv_display_t *disp = lv_display_create(DISPLAY_W, DISPLAY_H);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, gDrawBuf1, gDrawBuf2, sizeof(gDrawBuf1), LV_DISPLAY_RENDER_MODE_PARTIAL);

    // lv_conf.h's LV_THEME_DEFAULT_DARK is only a hint for LVGL's own
    // example code — it doesn't get read automatically here, so without
    // this explicit call the display gets no theme at all and every
    // widget falls back to LVGL's bare default style (white background,
    // black text) instead of the dark theme.
    lv_theme_t *theme = lv_theme_default_init(disp, lv_color_hex(0x2196f3), lv_color_hex(0xff5252),
                                               /*dark=*/true, LV_FONT_DEFAULT);
    lv_display_set_theme(disp, theme);

    // Display/LVGL are up now so try_auto_ota() (still called before BLE
    // init, further down) has somewhere to show progress instead of the
    // panel just sitting blank for however long WiFi connect + download take.
    build_boot_screen();
    lv_timer_handler();

    try_auto_ota();

    NimBLEDevice::init("AutoGauge");

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);

    build_gauge_screen();

    gSettingsUi.init(&gSettings, &gWifi, &gTouch);
    gSettingsUi.setHomeScreen(gGaugeScreen);

    xTaskCreatePinnedToCore(obd_task, "obd_task", 12288, nullptr, 1, nullptr, 0);
}

void loop() {
    uint32_t frame_start = millis();
    lv_timer_handler();
    update_gauge_screen();
    gSettingsUi.poll();
    serial_console_poll();
    // Target ~16ms per frame without adding 16ms on top of however long
    // lv_timer_handler() took. If a flush ran long we go immediately;
    // if it was fast we sleep the remainder. Keeps touch poll rate at
    // ~60Hz rather than (60Hz minus render time).
    int32_t remaining = 16 - (int32_t)(millis() - frame_start);
    if (remaining > 1) delay(remaining);
}
