import math
import time
import pygame
import config
from obd import GaugeData

BG_COLOR    = (17, 17, 17)
ARC_BG      = (42, 42, 42)
ARC_WIDTH   = 12
LABEL_COLOR = (170, 170, 170)
VALUE_COLOR = (255, 255, 255)

CONN_OK     = (68,  220, 100)   # connected, fresh data
CONN_STALE  = (255, 170,  68)   # connected but no fresh poll recently
CONN_BAD    = (255,  68,  68)   # not connected
STALE_AFTER_S = 3.0

ARC_START_DEG = 135      # degrees (pygame: 0=right, ccw positive — we convert below)
ARC_SWEEP_DEG = 270


def _draw_arc(surface: pygame.Surface, color, rect: pygame.Rect,
              start_deg: float, sweep_deg: float, width: int):
    if sweep_deg <= 0:
        return
    # pygame draws arcs CCW from right. We want CW from bottom-left (135° clock).
    # Convert: pygame angle = -(clock_angle - 90) in radians
    steps = max(2, int(abs(sweep_deg)))
    points = []
    cx = rect.centerx
    cy = rect.centery
    rx = rect.width  / 2
    ry = rect.height / 2
    for i in range(steps + 1):
        t = start_deg + sweep_deg * i / steps
        rad = math.radians(t)
        x = cx + rx * math.cos(rad)
        y = cy + ry * math.sin(rad)
        points.append((x, y))
    if len(points) >= 2:
        pygame.draw.lines(surface, color, False, points, width)


class GaugeRenderer:
    def __init__(self, surface: pygame.Surface):
        self._surface = surface
        self._font_label = pygame.font.SysFont("monospace", 18, bold=False)
        self._font_value = pygame.font.SysFont("monospace", 30, bold=True)
        self._font_status = pygame.font.SysFont("monospace", 13, bold=True)
        self._data = GaugeData()
        self._connected = False

    def update(self, data: GaugeData, connected: bool = False):
        self._data = data
        self._connected = connected

    def _draw_status(self, w):
        if not self._connected:
            color, label = CONN_BAD, "OBD"
        elif (time.monotonic() - self._data.ts) > STALE_AFTER_S:
            color, label = CONN_STALE, "OBD"
        else:
            color, label = CONN_OK, "OBD"
        cx, cy = w - 12, 12
        pygame.draw.circle(self._surface, color, (cx, cy), 5)
        s = self._font_status.render(label, True, LABEL_COLOR)
        r = s.get_rect(midright=(cx - 10, cy))
        self._surface.blit(s, r)

    def draw(self):
        w, h = self._surface.get_size()
        self._surface.fill(BG_COLOR)
        self._draw_status(w)

        gauges = [
            ("BOOST",   self._data.boost_psi,  self._data.boost_display(),
             config.GAUGE_SPECS[0][1], config.GAUGE_SPECS[0][2], config.GAUGE_SPECS[0][3]),
            ("RPM",     self._data.rpm,         self._data.rpm_display(),
             config.GAUGE_SPECS[1][1], config.GAUGE_SPECS[1][2], config.GAUGE_SPECS[1][3]),
            ("COOLANT", self._data.coolant_c,   self._data.coolant_display(),
             config.GAUGE_SPECS[2][1], config.GAUGE_SPECS[2][2], config.GAUGE_SPECS[2][3]),
            ("OIL",     self._data.oil_temp_c,  self._data.oil_display(),
             config.GAUGE_SPECS[3][1], config.GAUGE_SPECS[3][2], config.GAUGE_SPECS[3][3]),
        ]

        cell_w = w // 2
        cell_h = h // 2
        for i, (label, value, text, vmin, vmax, color) in enumerate(gauges):
            cx = cell_w * (i % 2) + cell_w // 2
            cy = cell_h * (i // 2) + cell_h // 2
            radius = int(min(cell_w, cell_h) * 0.38)
            self._draw_gauge(cx, cy, radius, label, value, text, vmin, vmax, color)

    def _draw_gauge(self, cx, cy, radius, label, value, text, vmin, vmax, color):
        rect = pygame.Rect(cx - radius, cy - radius, radius * 2, radius * 2)

        # Background arc
        _draw_arc(self._surface, ARC_BG, rect, ARC_START_DEG, ARC_SWEEP_DEG, ARC_WIDTH)

        # Foreground arc (value)
        if value is not None:
            fraction = max(0.0, min(1.0, (value - vmin) / (vmax - vmin)))
            _draw_arc(self._surface, color, rect, ARC_START_DEG,
                      ARC_SWEEP_DEG * fraction, ARC_WIDTH)

        # Label text (above center)
        lbl_surf = self._font_label.render(label, True, LABEL_COLOR)
        lbl_rect = lbl_surf.get_rect(center=(cx, cy - radius // 4))
        self._surface.blit(lbl_surf, lbl_rect)

        # Value text (below center)
        val_surf = self._font_value.render(text, True, VALUE_COLOR)
        val_rect = val_surf.get_rect(center=(cx, cy + radius // 3))
        self._surface.blit(val_surf, val_rect)
