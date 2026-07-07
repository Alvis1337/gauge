// ST7796 SPI display driver for the MSP4021 4" TFT (480x320).
// This exact register sequence (gamma curves, MADCTL orientation,
// inversion-on fix) was reverse-engineered and verified working against
// the physical panel on the Raspberry Pi build (see display.py) — ported
// directly rather than relying on a generic ST7796 driver, which risks a
// subtly different init sequence on this particular panel variant.
#pragma once
#include <Arduino.h>
#include <SPI.h>

class ST7796Driver {
public:
    static constexpr int WIDTH  = 480;
    static constexpr int HEIGHT = 320;

    ST7796Driver(int pin_cs, int pin_dc, int pin_rst, SPIClass &spi)
        : _cs(pin_cs), _dc(pin_dc), _rst(pin_rst), _spi(spi) {}

    void begin() {
        pinMode(_cs, OUTPUT);
        pinMode(_dc, OUTPUT);
        pinMode(_rst, OUTPUT);
        digitalWrite(_cs, HIGH);

        _reset();
        _init();
    }

    // Blits a 16-bit RGB565 framebuffer region to the panel. Caller (the
    // LVGL flush callback) already has pixels in big-endian SPI byte order.
    void blit(int x1, int y1, int x2, int y2, const uint16_t *pixels, size_t count) {
        _setWindow(x1, y1, x2, y2);
        _cmd(0x2C);  // memory write
        _beginTransaction();
        digitalWrite(_dc, HIGH);
        digitalWrite(_cs, LOW);
        // Write-only — the panel doesn't drive MISO, and this avoids
        // clobbering the caller's pixel buffer with read-back garbage the
        // way an in-place transfer() would.
        _spi.writeBytes((const uint8_t *)pixels, count * 2);
        digitalWrite(_cs, HIGH);
        _endTransaction();
    }

private:
    int _cs, _dc, _rst;
    SPIClass &_spi;

    void _beginTransaction() { _spi.beginTransaction(SPISettings(40000000, MSBFIRST, SPI_MODE0)); }
    void _endTransaction()   { _spi.endTransaction(); }

    void _cmd(uint8_t c) {
        _beginTransaction();
        digitalWrite(_dc, LOW);
        digitalWrite(_cs, LOW);
        _spi.transfer(c);
        digitalWrite(_cs, HIGH);
        _endTransaction();
    }

    void _dat(const uint8_t *data, size_t len) {
        _beginTransaction();
        digitalWrite(_dc, HIGH);
        digitalWrite(_cs, LOW);
        for (size_t i = 0; i < len; i++) _spi.transfer(data[i]);
        digitalWrite(_cs, HIGH);
        _endTransaction();
    }

    void _dat1(uint8_t b) { _dat(&b, 1); }

    void _reg(uint8_t cmd, std::initializer_list<uint8_t> args = {}) {
        _cmd(cmd);
        if (args.size()) {
            uint8_t buf[16];
            size_t i = 0;
            for (uint8_t a : args) buf[i++] = a;
            _dat(buf, args.size());
        }
    }

    void _reset() {
        digitalWrite(_rst, HIGH); delay(50);
        digitalWrite(_rst, LOW);  delay(150);
        digitalWrite(_rst, HIGH); delay(150);
    }

    void _init() {
        _cmd(0x01); delay(120);   // Software reset
        _cmd(0x11); delay(120);   // Sleep out

        _reg(0xF0, {0xC3});       // Command set enable page 1
        _reg(0xF0, {0x96});

        _reg(0x36, {0x28});       // MADCTL: landscape, BGR
        _reg(0x3A, {0x55});       // 16-bit color (RGB565)

        _reg(0xB4, {0x01});                       // Inversion: 1-dot
        _reg(0xB6, {0x80, 0x02, 0x3B});            // Display function
        _reg(0xB7, {0xC6});                        // Entry mode

        _reg(0xC0, {0x80, 0x64});                  // Power control 1
        _reg(0xC1, {0x13});                        // Power control 2
        _reg(0xC2, {0xA7});                        // Power control 3
        _reg(0xC5, {0x09});                        // VCOM

        _reg(0xE8, {0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33});  // Display output ctrl

        _reg(0xE0, {0xF0, 0x08, 0x0C, 0x18, 0x14, 0x06, 0x2C, 0x43,    // Positive gamma
                     0x40, 0x08, 0x13, 0x11, 0x2D, 0x33});
        _reg(0xE1, {0xF0, 0x09, 0x0D, 0x1F, 0x1C, 0x07, 0x2C, 0x43,    // Negative gamma
                     0x40, 0x07, 0x10, 0x0F, 0x2D, 0x33});

        _reg(0xF0, {0x3C});       // Command set disable
        _reg(0xF0, {0x69});

        _cmd(0x21); delay(10);    // Display inversion ON — panel needs this or colors are negative
        _cmd(0x29); delay(50);    // Display on
    }

    void _setWindow(int x1, int y1, int x2, int y2) {
        uint8_t col[4] = {(uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF), (uint8_t)(x2 >> 8), (uint8_t)(x2 & 0xFF)};
        uint8_t row[4] = {(uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF), (uint8_t)(y2 >> 8), (uint8_t)(y2 & 0xFF)};
        _cmd(0x2A); _dat(col, 4);
        _cmd(0x2B); _dat(row, 4);
    }
};
