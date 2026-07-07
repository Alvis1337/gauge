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

STATUS_BAR_H     = 18
STATUS_BAR_COLOR = (10, 10, 10)
STATUS_BAR_LINE  = (50, 50, 50)

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
        self._wifi_connected = False
        self._bt_powered = False

    def update(self, data: GaugeData, connected: bool = False,
               wifi_connected: bool = False, bt_powered: bool = False):
        self._data = data
        self._connected = connected
        self._wifi_connected = wifi_connected
        self._bt_powered = bt_powered

    def _obd_color(self):
        if not self._connected:
            return CONN_BAD
        if (time.monotonic() - self._data.ts) > STALE_AFTER_S:
            return CONN_STALE
        return CONN_OK

    def _draw_status_bar(self, w):
        pygame.draw.rect(self._surface, STATUS_BAR_COLOR, (0, 0, w, STATUS_BAR_H))
        pygame.draw.line(self._surface, STATUS_BAR_LINE,
                          (0, STATUS_BAR_H - 1), (w, STATUS_BAR_H - 1))

        ver_surf = self._font_status.render(f"v{config.VERSION}", True, LABEL_COLOR)
        self._surface.blit(ver_surf, ver_surf.get_rect(midleft=(8, STATUS_BAR_H // 2)))

        items = [
            ("OBD",  self._obd_color()),
            ("BT",   CONN_OK if self._bt_powered else CONN_BAD),
            ("WiFi", CONN_OK if self._wifi_connected else CONN_BAD),
        ]
        x = w - 10
        for label, color in items:
            s = self._font_status.render(label, True, LABEL_COLOR)
            r = s.get_rect(midright=(x, STATUS_BAR_H // 2))
            self._surface.blit(s, r)
            x = r.left - 10
            pygame.draw.circle(self._surface, color, (x, STATUS_BAR_H // 2), 4)
            x -= 12

    def draw(self):
        w, h = self._surface.get_size()
        self._surface.fill(BG_COLOR)
        self._draw_status_bar(w)

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

        top = STATUS_BAR_H
        cell_w = w // 2
        cell_h = (h - top) // 2
        for i, (label, value, text, vmin, vmax, color) in enumerate(gauges):
            cx = cell_w * (i % 2) + cell_w // 2
            cy = top + cell_h * (i // 2) + cell_h // 2
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
