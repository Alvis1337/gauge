"""
Direct framebuffer writer for fbtft SPI TFT displays.
Converts a pygame RGB surface to RGB565 and mmaps it to /dev/fb1.
"""
import mmap
import os
import numpy as np
import pygame


class FramebufferOutput:
    def __init__(self, device: str, width: int, height: int):
        self._w = width
        self._h = height
        self._fd = os.open(device, os.O_RDWR)
        self._mm = mmap.mmap(self._fd, width * height * 2)

    def write(self, surface: pygame.Surface):
        # Get RGB array (H x W x 3, uint8)
        rgb = pygame.surfarray.pixels3d(surface)   # W x H x 3
        rgb = np.transpose(rgb, (1, 0, 2))         # H x W x 3

        r = rgb[:, :, 0].astype(np.uint16)
        g = rgb[:, :, 1].astype(np.uint16)
        b = rgb[:, :, 2].astype(np.uint16)

        # Pack to RGB565 little-endian
        px = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        self._mm.seek(0)
        self._mm.write(px.astype(np.uint16).tobytes())

    def close(self):
        self._mm.close()
        os.close(self._fd)
