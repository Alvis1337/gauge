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
GaugeWidget gBoostGauge, gRpmGauge, gCoolantGauge, gOilGauge;
lv_obj_t *gConnDot = nullptr;

static void build_gauge_screen() {
    lv_obj_t *scr = lv_scr_act();
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

    gBoostGauge.create(cell(0, 0), "BOOST",   -5.0f,   25.0f, theme::boost(),   "%.1f psi");
    gRpmGauge.create(cell(1, 0),   "RPM",      0.0f, 8000.0f, theme::rpm(),     "%.0f");
    gCoolantGauge.create(cell(0, 1), "COOLANT", 40.0f, 130.0f, theme::coolant(), "%.0f C");
    gOilGauge.create(cell(1, 1),   "OIL",      40.0f, 150.0f, theme::oil(),     "%.0f C");

    // Small connection-state dot, top-right — green = connected, red = not.
    gConnDot = lv_obj_create(scr);
    lv_obj_remove_style_all(gConnDot);
    lv_obj_set_size(gConnDot, 10, 10);
    lv_obj_set_style_radius(gConnDot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(gConnDot, theme::danger(), 0);
    lv_obj_set_style_bg_opa(gConnDot, LV_OPA_COVER, 0);
    lv_obj_align(gConnDot, LV_ALIGN_TOP_RIGHT, -4, 4);
}

static void update_gauge_screen() {
    GaugeData d;
    bool connected;
    portENTER_CRITICAL(&gShared.mux);
    d = gShared.data;
    connected = gShared.connected;
    portEXIT_CRITICAL(&gShared.mux);

    gBoostGauge.setValue(d.boost_psi);
    gRpmGauge.setValue(d.rpm);
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
                if (current != address) break;  // settings changed elsewhere, reconnect fresh
                GaugeData d = client.poll();
                portENTER_CRITICAL(&gShared.mux);
                gShared.data = d;
                gShared.connected = client.connected;
                portEXIT_CRITICAL(&gShared.mux);
                vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
            }
            client.disconnect();
        }

        if (consecutive_failures >= DISCOVERY_AFTER_FAILURES) {
            std::string found = bt_discovery::discover();
            if (!found.empty() && found != address) {
                gSettings.setObdBtAddress(found);
                gSettings.setObdBtName("");
                retry_delay_ms = 2000;
            }
            consecutive_failures = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
        retry_delay_ms = std::min<uint32_t>(retry_delay_ms * 2, 30000);
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

    NimBLEDevice::init("AutoGauge");

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

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);

    build_gauge_screen();

    xTaskCreatePinnedToCore(obd_task, "obd_task", 8192, nullptr, 1, nullptr, 0);
}

void loop() {
    lv_timer_handler();
    update_gauge_screen();
    delay(16);  // ~60fps UI refresh; BLE polling runs independently on core 0
}
