# AutoGauge — Claude Code context

## What this is

ESP32-based OBD-II gauge for a 2021 BMW X5 B58 3.0T. Replaces a Raspberry Pi build.
Hardware: ESP32 DevKitC, ST7796 480×320 SPI TFT, XPT2046 SPI resistive touch, BLE ELM327
OBD adapter (Vgate iCar / iOS-Vlink).

All firmware lives in `firmware/`. PlatformIO project. Headers-only implementation pattern —
most logic lives in `firmware/include/*.h`, with `firmware/src/main.cpp` as the thin entry point.

## Hardware pin map

| Signal     | GPIO |
|------------|------|
| SCLK       | 18   |
| MOSI       | 23   |
| MISO       | 19   |
| LCD CS     | 5    |
| LCD DC     | 26   |
| LCD RST    | 25   |
| Touch CS   | 27   |
| NeoPixel bar data    | 4  |
| NeoPixel status data | 16 |

Display: MSP4021 4" panel, 480×320. Init sequence sets MADCTL argument `0x28`
(cmd `0x36`; MV=1 landscape + RGB=1 BGR filter order) then issues INVON command `0x21`
(display inversion ON — this panel renders colors negative without it). Combined effect:
LVGL's (R,G,B) arrives as (255-B, 255-G, 255-R) at the panel; `flush_cb` in main.cpp
pre-compensates (swaps R5↔B5 and inverts all channels + byte-swaps for big-endian SPI)
so every `lv_color_hex()` value in the app just works.

SPI bus: both display and touch share VSPI. Display at 40MHz MODE0; touch at 1MHz MODE3.

NeoPixel outputs: a 60-pixel RGBW (SK6812, GRBW order) shift-light bar on GPIO4, and a
separate single-pixel RGBW status LED on GPIO16 — two physically distinct strips, own
GPIO each, driven via the ESP32's RMT peripheral (hardware-clocked, not bit-banged) so
neither stalls a core. See `firmware/include/neopixel_output.h`.

## Architecture

### Dual-core split

| Core | What runs there |
|------|----------------|
| 0    | `obd_task` (BLE I/O, blocks freely) + NimBLE host task |
| 1    | Arduino `loop()` → LVGL `lv_timer_handler()` + `update_gauge_screen()` + `SettingsUI::poll()` + `serial_console_poll()` |

**Rule: all `lv_*` calls must be on core 1.** Background tasks (wifi_scan, wifi_connect,
obd_scan, upd_wifi, upd_ota, upd_log) never call LVGL directly. They write into mutex-
protected shared state; `poll()` / `loop()` reflects that into labels once per frame.

### Cross-core synchronization

Use `portMUX_TYPE` spinlocks with `portENTER_CRITICAL` / `portEXIT_CRITICAL`.

**How it actually works on ESP32 dual-core (important):**
`portENTER_CRITICAL` on the *calling* core disables that core's interrupts and acquires
the spinlock. If the other core is already holding the same mux, the calling core spins
with ITS OWN interrupts disabled until the lock is released — meaning both cores can end
up with interrupts disabled simultaneously during contention. Short critical sections
aren't just a convention; they're essential to avoid triggering the interrupt watchdog.

**LVGL has no internal FreeRTOS task** (`LV_USE_OS = LV_OS_NONE` in `lv_conf.h`).
`lv_freertos.c` compiles to dead code. LVGL is driven entirely by `lv_timer_handler()`
in `loop()`. All `lv_*` calls happen in the Arduino main task on core 1.

**Background task core assignment:** `obd_task` is the only task pinned to a core
(core 0 via `xTaskCreatePinnedToCore`). All other `xTaskCreate` tasks — wifi_scan,
wifi_connect, obd_scan, upd_wifi, upd_ota, upd_log — are unpinned; the FreeRTOS
scheduler places them on whichever core it chooses. They are currently safe on any core
because none call `lv_*`. Any future task that needs LVGL must be pinned to core 1 or
use the dirty-flag pattern.

**What is safe inside `portENTER_CRITICAL`:**
- Plain assignments (bool, int, float)
- `String::move` / `std::move` on a String (pointer swap, no allocation)
- `std::vector::move` (pointer swap, no allocation)
- `strncpy` into a stack `char` buffer

**What is NOT safe inside `portENTER_CRITICAL`:**
- `malloc` / `new` / heap allocation of any kind
- `std::string` construction from a char* (allocates)
- `std::vector` copy constructor (allocates)
- `String` copy (allocates)
- Any blocking call, delay, I/O

ESP-IDF explicitly warns: blocking or allocating inside a critical section stalls the
other core and risks watchdog resets.

### Dirty-flag pattern for background → UI updates

Background task writes data + sets a dirty flag under the same lock. `poll()` snapshots
both atomically (single `portENTER_CRITICAL`). Two separate critical sections create a
window where a new write lands between them, leaving the dirty flag set but the data
already moved — next frame would blank the label. Always snapshot flag + data together.

### In-progress flags

Every operation that spawns a background task has an `_xxxInProgress` bool. Checked and
set atomically under the spinlock before spawning. Cleared under the spinlock in the task
before it posts its result status. Rules:
- Scan ops check `_wifiScanInProgress || _wifiConnectInProgress` (scan blocked while
  connect is running — both use the same WiFi driver with no internal locking).
- OTA and Upload are mutually exclusive (`_otaInProgress || _uploadInProgress`).

## Key files

| File | Role |
|------|------|
| `firmware/src/main.cpp` | Entry point, gauge screen, `obd_task`, `loop()`, serial console |
| `firmware/include/obd_client.h` | ELM327 AT-command client; `GaugeData` struct |
| `firmware/include/ble_transport.h` | NimBLE GATT transport; FreeRTOS queue for rxCb→recvUntil |
| `firmware/include/bt_discovery.h` | BLE scan + ELM327 verify; `_scanMutex()` serializes all BLE scan ops |
| `firmware/include/parsers.h` | Pure functions: hex OBD response → float |
| `firmware/include/gauge_settings.h` | NVS-backed settings via `Preferences`; `consumeObdReconnectRequest()` has its own `_reconnectMux` |
| `firmware/include/settings_ui.h` | Settings screen (WiFi, OBD adapter, touch cal, OBD log) |
| `firmware/include/update_ui.h` | Update Mode UI (OTA check, log upload, webhook config) |
| `firmware/include/gauge_widget.h` | Single gauge cell (arc + label) |
| `firmware/include/neopixel_output.h` | Shift-light bar (GPIO4) + status LED (GPIO16); called from `loop()` every frame |
| `firmware/include/wifi_manager.h` | Thin wrapper around Arduino WiFi — **no internal locking** |
| `firmware/include/theme.h` | Color constants |
| `firmware/include/obd_log.h` | Ring-buffer log (written from any task; `snapshot()` + `isDirty()` for UI) |

## Four gauges (current layout)

| Cell | Label  | PID              | Range       | Format   |
|------|--------|------------------|-------------|----------|
| 0,0  | BOOST  | 010B - 0133      | -5 to 25    | %.1f psi |
| 1,0  | ETH %  | Mode 22 0x44DE   | 0 to 100    | %.0f%%   |
| 0,1  | OIL    | Mode 01 0x5C     | 40 to 150   | %.0f C   |
| 1,1  | FUEL % | Mode 01 0x2F     | 0 to 100    | %.0f%%   |

Ethanol is BMW MHD flex fuel kit specific (Mode 22, B58 PID). Skipped after 3 consecutive
NaN responses (`SKIP_AFTER_FAILURES`) to avoid burning polling time on an unplugged kit.
Oil temp uses Mode 01 0x5C (standard, not BMW Mode 22). Boost is MAP minus baro; if baro
PID 0133 times out, falls back to 101.325 kPa (marked in log with "baro~").

## BLE adapter specifics

Vgate iCar / iOS-Vlink quirks (baked into `ble_transport.h` and `obd_client.h`):
- Uses a random BLE address — `connect()` tries PUBLIC then RANDOM.
- Proprietary GATT service `e7810a71...` / characteristic `bef8d6c9...` (write = notify = same UUID).
- Also subscribes to indicate (CCCD 0x0003, not just 0x0001): initial `>` prompt arrives as
  notify but command responses arrive as indicate.
- 500ms settle delay after GATT subscription before first AT command.
- Skip ATZ/ATWS — resets the BLE stack on this adapter; go straight to ATE0.

## XPT2046 touch quirks

This specific panel has two non-obvious axis properties baked into `xpt2046_driver.h`:
- **Axes are swapped**: the X ADC command (`0xD0`) tracks the physical *vertical* axis;
  the Y ADC command (`0x90`) tracks the physical *horizontal* axis. Opposite of what
  you'd expect.
- **Both axes are physically inverted**: `read()` mirrors both before returning pixel
  coordinates (`display_w - 1 - x`, `display_h - 1 - y`).
- No T_IRQ pin wired — pressure is polled every LVGL frame via `_pressure()`.
- First ADC sample after switching channels is discarded (mux settling); position reads
  are median-filtered over 3 samples (`POSITION_SAMPLES = 3`).

If touch coordinates seem wrong after hardware changes, check these axis mappings first.

Known fallback order for GATT service discovery:
1. Vgate proprietary (e7810a71 / bef8d6c9)
2. Nordic UART Service (6e400001 / 6e400002 / 6e400003)
3. HM-10 / AT-09 style (ffe0 / ffe1)
4. FFF0 vendor split (fff0 / fff2 write / fff1 notify)
5. Generic fallback: scan all services for any writable + notifiable pair

## Settings persistence

`GaugeSettings` wraps ESP32 `Preferences` (NVS). All reads/writes go through NVS which
has its own internal mutex — **no additional locking needed** for cross-core settings
access. The one exception is `consumeObdReconnectRequest()` which is in-memory only and
has its own `_reconnectMux`.

## WiFi / BLE coexistence

The original ESP32 shares one radio between WiFi and BLE via time-division coexistence.
WiFi is powered off (`WIFI_OFF`) except during explicit update/scan windows.
`WifiManager::begin()` / `end()` bracket each window. Leaving WiFi associated in the
background would steal airtime from the OBD BLE link.

## Update Mode vs Gauge Mode

Mode is chosen at boot via a 5-second splash screen (or `gRtcBootToUpdate` RTC flag that
survives `ESP.restart()`). They are mutually exclusive — WiFi and BLE are never both
active at the same time. Update Mode runs `UpdateUI::run()` which blocks; on exit it calls
`ESP.restart()`.

`rebootToUpdateMode()` (defined in main.cpp, declared extern in settings_ui.h) sets both
`gRtcBootToUpdate` and `gRtcAutoUpload` before restarting — tells the next boot to skip
the countdown and auto-upload the OBD log to Discord.

## `bt_discovery::_scanMutex()`

Singleton FreeRTOS mutex. Held across the entire `discover()` call (scan + verify) so
`obd_task`'s auto-discovery and the settings UI's manual scan can't both drive the NimBLE
scan state machine at once. `scan()` (UI path) also holds it for the duration of its scan.

## Serial debug console

Available over USB. Commands: `wifi <ssid> [password]`, `ota`, `reboot`, `touch`, `gpio`,
`webhook <url>`, `status`. Non-blocking (`serial_console_poll()` called from `loop()`).
Useful for headless bring-up without a working touchscreen.

## Coding rules for this project

- No comments explaining WHAT code does — names do that. Comments only for non-obvious
  WHY (hidden constraint, workaround, invariant that would surprise a reader).
- No heap allocation inside `portENTER_CRITICAL`. Use stack buffers + `strncpy`, or `std::move`.
- Background tasks never call `lv_*`. Write to shared state + set dirty flag; let `poll()` update UI.
- `_wifi->end()` deferred via `connectRunning` flag in `poll()` — never call it while
  a connect task is in flight.
- All in-progress flags: check + set in single critical section, clear in task under lock
  before posting result status.
- LVGL v9.5.0 API (not v8). Object creation: `lv_button_create`, `lv_obj_create`, etc.

## Toolchain constraints

Target: `xtensa-esp32-elf-gcc` via PlatformIO / Arduino-ESP32 framework.
NimBLE: `h2zero/NimBLE-Arduino@^1.4.2`. LVGL: 9.5.0 (vendored in `lib/lvgl` — pulled
from lib_deps registry would try to compile an ARM Helium .S file the Xtensa assembler
can't handle). Partition scheme: `min_spiffs.csv` (two OTA app slots ~1.9MB each;
current image ~975KB).

- **Exceptions are disabled** (`-fno-exceptions`). Never use `try`/`catch` or design
  anything that depends on stack unwinding. Signal errors via return values or NaN.
- **No smart pointers** in hot paths. `std::unique_ptr` / `std::shared_ptr` exist but
  add heap overhead in a ~300KB free-heap environment where NimBLE holds ~40KB and
  LVGL compositing needs large contiguous chunks. Raw pointers with clear ownership
  are the right call here.
- **C++ standard: `gnu++11` only.** PlatformIO Arduino-ESP32 defaults to `-std=gnu++11`.
  Neither C++14 nor C++17 features are available. Do NOT use: `std::clamp` (C++17),
  `std::string_view` (C++17), structured bindings `auto &[a,b]` (C++17), `if constexpr`
  (C++17), `std::optional` (C++17), `std::make_unique` (C++14), generic lambdas (C++14),
  or multi-statement `constexpr` functions (C++14). Safe: lambdas with explicit types,
  range-for, `std::move`, `std::sort`, `std::min`/`max`, `static constexpr`,
  single-return `constexpr`.
- **No RTTI** (`-fno-rtti`). `dynamic_cast` and `typeid` are unavailable.
- **Stack is scarce.** Tasks are created with explicit stack sizes (8–24KB). Don't put
  large arrays or deeply-nested frames on the stack inside tasks. `static` locals inside
  functions are fine (go in BSS/data, not stack).
- **Heap fragmentation matters.** Prefer fixed-size buffers, `std::move`, and reuse over
  repeated allocate-free cycles. Vectors that grow once and stay are fine; vectors that
  repeatedly grow and shrink fragment the heap.

## Task stack sizes

| Task name      | Stack  | Notes |
|----------------|--------|-------|
| obd_task       | 12288  | Pinned core 0; ELM327 string handling + BLE I/O |
| wifi_scan      | 8192   | WiFi scan + std::vector build |
| wifi_connect   | 8192   | WiFi connect loop |
| obd_scan       | 8192   | NimBLE `bt_discovery::scan()` — NimBLE yields during 6s scan via ulTaskNotifyTake |
| upd_wifi       | 24576  | WiFi connect + optional log upload |
| upd_ota        | 16384  | HTTPClient OTA download |
| upd_log        | 24576  | HTTPClient log POST |
| ota_reboot     | 2048   | vTaskDelay + ESP.restart() only |

## Race condition checklist (run mentally before any new background task)

When adding a new operation that spawns a `xTaskCreate`:
1. Is there an `_xxxInProgress` flag? Check + set atomically under `portENTER_CRITICAL`.
2. Does it use WiFi? Block if `_wifiScanInProgress || _wifiConnectInProgress` (they share
   a driver with no internal locking).
3. Does it use the BLE radio? Acquire `bt_discovery::_scanMutex()` first.
4. Does the task write shared data for the UI? Use dirty-flag pattern: write data + set
   flag in ONE critical section.
5. Does the task allocate inside a critical section? It shouldn't. Move allocation out.
6. Does `poll()` need to defer a cleanup call (like `_wifi->end()`) until the task is
   done? Read the in-progress flag in the same critical section as the dirty flags.
7. Clear the in-progress flag under lock BEFORE calling `_setStatus()` — so a second
   tap is unblocked the instant the operation finishes, not after the label update.