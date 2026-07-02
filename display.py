"""
ST7796 SPI display driver for MSP4021 4" TFT (480x320).
Display on SPI0 CE0; DC=GPIO24, RESET=GPIO25.
Uses gpiod 2.x for GPIO control.
"""
import time
import gpiod
from gpiod.line import Direction, Value
import spidev
import numpy as np
import pygame

WIDTH  = 480
HEIGHT = 320

DC_PIN    = 24
RESET_PIN = 25
GPIO_CHIP = "/dev/gpiochip0"

_CHUNK = 4096   # max bytes per SPI transfer; see show() for why


class ST7796:
    def __init__(self):
        self._gpio = gpiod.request_lines(
            GPIO_CHIP,
            consumer="autogauge",
            config={
                DC_PIN:    gpiod.LineSettings(direction=Direction.OUTPUT, output_value=Value.INACTIVE),
                RESET_PIN: gpiod.LineSettings(direction=Direction.OUTPUT, output_value=Value.ACTIVE),
            }
        )

        self._spi = spidev.SpiDev()
        self._spi.open(0, 0)
        self._spi.max_speed_hz = 40_000_000
        self._spi.mode = 0

        self._reset()
        self._init()

    # ── low-level ────────────────────────────────────────────────

    def _dc(self, high: bool):
        self._gpio.set_value(DC_PIN, Value.ACTIVE if high else Value.INACTIVE)

    def _reset(self):
        self._gpio.set_value(RESET_PIN, Value.ACTIVE);  time.sleep(0.05)
        self._gpio.set_value(RESET_PIN, Value.INACTIVE); time.sleep(0.15)
        self._gpio.set_value(RESET_PIN, Value.ACTIVE);  time.sleep(0.15)

    def _cmd(self, cmd: int):
        self._dc(False)
        self._spi.writebytes2([cmd])

    def _dat(self, data):
        self._dc(True)
        if isinstance(data, int):
            self._spi.writebytes2([data])
        else:
            self._spi.writebytes2(data)

    def _reg(self, cmd: int, *args):
        self._cmd(cmd)
        if args:
            self._dat(list(args))

    # ── init sequence ────────────────────────────────────────────

    def _init(self):
        self._cmd(0x01);             time.sleep(0.12)  # Software reset
        self._cmd(0x11);             time.sleep(0.12)  # Sleep out

        self._reg(0xF0, 0xC3)                          # Command set enable page 1
        self._reg(0xF0, 0x96)

        self._reg(0x36, 0x28)                          # MADCTL: landscape, BGR
        self._reg(0x3A, 0x55)                          # 16-bit color (RGB565)

        self._reg(0xB4, 0x01)                          # Inversion: 1-dot
        self._reg(0xB6, 0x80, 0x02, 0x3B)             # Display function
        self._reg(0xB7, 0xC6)                          # Entry mode

        self._reg(0xC0, 0x80, 0x64)                   # Power control 1
        self._reg(0xC1, 0x13)                          # Power control 2
        self._reg(0xC2, 0xA7)                          # Power control 3
        self._reg(0xC5, 0x09)                          # VCOM

        self._reg(0xE8, 0x40, 0x8A, 0x00, 0x00,
                        0x29, 0x19, 0xA5, 0x33)        # Display output ctrl

        self._reg(0xE0, 0xF0, 0x08, 0x0C, 0x18,       # Positive gamma
                        0x14, 0x06, 0x2C, 0x43,
                        0x40, 0x08, 0x13, 0x11,
                        0x2D, 0x33)

        self._reg(0xE1, 0xF0, 0x09, 0x0D, 0x1F,       # Negative gamma
                        0x1C, 0x07, 0x2C, 0x43,
                        0x40, 0x07, 0x10, 0x0F,
                        0x2D, 0x33)

        self._reg(0xF0, 0x3C)                          # Command set disable
        self._reg(0xF0, 0x69)

        self._cmd(0x21);             time.sleep(0.01)  # Display inversion ON (panel needs this or colors are negative)
        self._cmd(0x29);             time.sleep(0.05)  # Display on

    # ── public ───────────────────────────────────────────────────

    def show(self, surface: pygame.Surface):
        self._reg(0x2A, 0x00, 0x00, (WIDTH  - 1) >> 8, (WIDTH  - 1) & 0xFF)
        self._reg(0x2B, 0x00, 0x00, (HEIGHT - 1) >> 8, (HEIGHT - 1) & 0xFF)
        self._cmd(0x2C)

        rgb = pygame.surfarray.pixels3d(surface)        # W×H×3 uint8
        rgb = np.transpose(rgb, (1, 0, 2))              # H×W×3
        rgb = rgb[::-1, ::-1, :]                        # rotate 180° to match physical orientation
        r = rgb[:, :, 0].astype(np.uint16)
        g = rgb[:, :, 1].astype(np.uint16)
        b = rgb[:, :, 2].astype(np.uint16)
        px = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        px = ((px >> 8) | (px << 8)) & 0xFFFF          # big-endian for SPI
        self._dc(True)
        buf = px.astype(np.uint16).tobytes()
        # One giant writebytes2() call (~307KB) can occasionally lose its DMA
        # completion interrupt on the bcm2835 SPI driver and hang forever in
        # spi_transfer_one_message with no kernel-side error at all. Chunking
        # into small fixed-size transfers avoids triggering that.
        for i in range(0, len(buf), _CHUNK):
            self._spi.writebytes2(buf[i:i + _CHUNK])

    def close(self):
        self._spi.close()
        self._gpio.release()
