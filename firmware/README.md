# AutoGauge firmware (ESP32)

Replaces the Raspberry Pi build entirely — one ESP32 DevKitC (30-pin,
WROOM-32) drives the ST7796 480x320 SPI TFT + XPT2046 touch panel and
polls the ELM327 BLE OBD-II adapter directly over its built-in BT radio.

## Wiring

Both the display and touch panel share one SPI bus (ESP32 hardware VSPI)
with separate chip-selects.

| Signal | ESP32 pin | Notes |
|---|---|---|
| SCLK (shared) | GPIO18 | VSPI hardware clock |
| MOSI (shared) | GPIO23 | VSPI hardware MOSI |
| MISO (shared) | GPIO19 | only touch uses this — display is write-only |
| LCD_CS | GPIO5 | |
| LCD_DC | GPIO26 | data/command select |
| LCD_RST | GPIO25 | |
| TOUCH_CS | GPIO27 | |
| LCD backlight | 3V3 | always-on, same as the Pi build |
| Panel GND / VCC | GND / 3V3 | |

No touch IRQ pin is wired — pressure is polled every frame (see
`xpt2046_driver.h`), matching the Pi build's proven approach. GPIO0/2/12/15
(boot-strapping) and GPIO6-11 (internal flash) are deliberately unused.

## Build / flash

```
cd firmware
pio run              # build
pio run -t upload    # flash over USB
pio device monitor    # serial log (115200 baud)
```

First build downloads the ESP32 toolchain + LVGL + NimBLE-Arduino
(several hundred MB) — subsequent builds are fast.

## First boot

`obd_bt_address` starts empty (NVS default) — the gauge screen shows `--`
everywhere and a red connection dot until an adapter is configured. Wiring
up a settings UI (BLE scan/pick list, touch calibration) to set this from
the device itself is tracked as follow-up work; until then, the auto-
discovery fallback (same logic as `bt_discovery.py`) kicks in automatically
after 3 failed connection attempts and will find + save a real ELM327
adapter's address on its own if one is advertising nearby.

## Source layout

- `src/main.cpp` — setup/loop, LVGL wiring, BLE polling task
- `include/st7796_driver.h` — display driver (ported from `display.py`)
- `include/xpt2046_driver.h` — touch driver (ported from `touch.py`)
- `include/ble_transport.h` — BLE GATT transport (ported from `bt_transport.py`)
- `include/obd_client.h` — ELM327 protocol (ported from `obd.py`)
- `include/bt_discovery.h` — BLE adapter scan/discover (ported from `bt_discovery.py`)
- `include/parsers.h` — PID payload decoding (ported from `parsers.py`)
- `include/gauge_settings.h` — NVS-backed settings (replaces `settings.json`)
- `include/gauge_widget.h`, `include/theme.h` — dark-theme LVGL gauge UI
