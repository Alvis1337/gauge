"""
All app screens. Each screen has draw(surf) and handle_touch(pos) -> next_screen_name.
Returns None to stay on the same screen, a string key to switch screens.
"""
import logging
import subprocess
import threading
import time
import pygame
import gauge_logging
import obd_discovery
import settings
import ui
from ui import W, H

log = logging.getLogger("screens")

# ── helpers ───────────────────────────────────────────────────────────────────

def _nmcli_scan() -> list[str]:
    try:
        out = subprocess.check_output(
            ["nmcli", "-t", "-f", "SSID,SIGNAL", "dev", "wifi", "list", "--rescan", "yes"],
            timeout=15, text=True, stderr=subprocess.DEVNULL
        )
        seen, nets = set(), []
        for line in out.splitlines():
            parts = line.split(":")
            if parts and parts[0] and parts[0] not in seen:
                seen.add(parts[0])
                try:
                    signal = int(parts[1]) if len(parts) > 1 else 0
                except ValueError:
                    signal = 0
                nets.append((parts[0], signal))
        nets.sort(key=lambda x: -x[1])
        return [s for s, _ in nets[:8]]
    except Exception:
        return []


def _nmcli_forget(ssid: str) -> None:
    # If a saved connection profile already exists for this SSID, `nmcli dev
    # wifi connect ... password ...` tries to patch it in place — and on
    # NetworkManager/nmcli here that update silently fails with
    # "802-11-wireless-security.key-mgmt: property is missing", leaving the
    # new password unapplied. It then reconnects with whatever secrets were
    # already saved (stale/wrong ones after an SSID's password changes, or
    # none at all for a profile that was never fully configured), and fails
    # with a confusing "no secrets" error instead of the real problem.
    # Deleting any existing profile first forces nmcli to build a fresh one
    # with the password actually supplied, sidestepping the bug entirely.
    subprocess.run(
        ["nmcli", "connection", "delete", ssid],
        timeout=10, capture_output=True, text=True
    )


def _nmcli_connect(ssid: str, password: str) -> bool:
    _nmcli_forget(ssid)
    cmd = ["nmcli", "dev", "wifi", "connect", ssid]
    if password:                      # open networks: omit "password" entirely
        cmd += ["password", password]
    try:
        out = subprocess.run(
            cmd, timeout=20, capture_output=True, text=True
        )
        if out.returncode != 0:
            log.warning("nmcli connect to %r failed (rc=%d): %s",
                        ssid, out.returncode, out.stderr.strip())
            return False
        log.info("nmcli connected to %r", ssid)
        return True
    except Exception:
        log.exception("nmcli connect to %r raised", ssid)
        return False


def _nmcli_connected_ssid() -> str:
    try:
        out = subprocess.check_output(
            ["nmcli", "-t", "-f", "ACTIVE,SSID", "dev", "wifi"],
            timeout=5, text=True, stderr=subprocess.DEVNULL
        )
        for line in out.splitlines():
            if line.startswith("yes:"):
                return line[4:]
    except Exception:
        pass
    return ""


# ── WiFi list screen ──────────────────────────────────────────────────────────

class WifiListScreen:
    def __init__(self):
        self._nets: list[str] = []
        self._loading = True
        self._connected = _nmcli_connected_ssid()
        self._cancelled = False
        threading.Thread(target=self._scan, daemon=True).start()

    def _scan(self):
        nets = _nmcli_scan()
        if not self._cancelled:
            self._nets = nets
            self._loading = False

    def cancel(self):
        self._cancelled = True

    def draw(self, surf: pygame.Surface):
        surf.fill(ui.BG)
        ui.text(surf, "WiFi Networks", W // 2, 10, ui._F_LG, anchor="midtop")
        ui.rect_btn(surf, "← Back", (8, 8, 70, 28), ui.PANEL, font=ui._F_SM)

        if self._loading:
            ui.text(surf, "Scanning…", W // 2, H // 2, ui._F_MD, ui.SUBTEXT, anchor="center")
            return

        if not self._nets:
            ui.text(surf, "No networks found", W // 2, H // 2, ui._F_MD, ui.SUBTEXT, anchor="center")
            return

        for i, ssid in enumerate(self._nets):
            y = 48 + i * 32
            active = ssid == self._connected
            color = ui.ACCENT if active else ui.CARD
            ui.rect_btn(surf, ("✓ " if active else "  ") + ssid,
                        (10, y, W - 20, 28), color, font=ui._F_SM, radius=4)

    def handle_touch(self, pos) -> str | None:
        if pos is None:
            return None
        if ui.hit((8, 8, 70, 28), pos):
            return "settings"
        if self._loading:
            return None
        for i, ssid in enumerate(self._nets):
            y = 48 + i * 32
            if ui.hit((10, y, W - 20, 28), pos):
                return f"wifi_connect:{ssid}"
        return None


# ── WiFi password screen ──────────────────────────────────────────────────────

class WifiPasswordScreen:
    def __init__(self, ssid: str):
        self._ssid = ssid
        self._buf = ""
        self._shift = False
        self._status = ""
        self._connecting = False

    def draw(self, surf: pygame.Surface):
        surf.fill(ui.BG)
        ui.text(surf, f"Connect to: {self._ssid}", W // 2, 8, ui._F_MD, anchor="midtop")
        # Password field
        display = ("*" * len(self._buf)) or " "
        pygame.draw.rect(surf, ui.CARD, (10, 36, W - 20, 30), border_radius=4)
        pygame.draw.rect(surf, ui.ACCENT, (10, 36, W - 20, 30), width=1, border_radius=4)
        ui.text(surf, display, 18, 42, ui._F_MD, ui.TEXT)
        if not self._buf and not self._status:
            ui.text(surf, "leave blank + OK for open networks", W // 2, 70,
                    ui._F_SM, ui.SUBTEXT, anchor="midtop")

        if self._status:
            color = ui.SUCCESS if "OK" in self._status else ui.DANGER
            ui.text(surf, self._status, W // 2, 76, ui._F_SM, color, anchor="midtop")

        if self._connecting:
            ui.text(surf, "Connecting…", W // 2, H // 2, ui._F_LG, ui.ACCENT, anchor="center")
        else:
            ui.draw_keyboard(surf, self._shift)
            ui.rect_btn(surf, "← Back", (8, 8, 60, 24), ui.PANEL, font=ui._F_SM)

    def handle_touch(self, pos) -> str | None:
        if self._connecting or pos is None:
            return None
        if ui.hit((8, 8, 60, 24), pos):
            return "wifi_list"
        k = ui.keyboard_hit(pos, self._shift)
        if k is None:
            return None
        if k == "BACKSPACE":
            self._buf = self._buf[:-1]
        elif k == "SHIFT":
            self._shift = not self._shift
        elif k == "SPACE":
            self._buf += " "
        elif k == "OK":
            self._do_connect()
        else:
            self._buf += k
            self._shift = False
        return None

    def _do_connect(self):
        self._connecting = True
        self._status = ""
        def _work():
            ok = _nmcli_connect(self._ssid, self._buf)
            self._status = "Connected OK" if ok else "Failed — check password"
            self._connecting = False
            if ok:
                settings.set("wifi_ssid", self._ssid)
        threading.Thread(target=_work, daemon=True).start()


# ── OBD config screen ─────────────────────────────────────────────────────────

class ObdConfigScreen:
    _FIELD_HOST = "host"
    _FIELD_PORT = "port"
    _SCAN_RECT  = (350, 46, 122, 34)

    def __init__(self):
        self._field = self._FIELD_HOST
        self._host_buf = settings.get("obd_host")
        self._port_buf = str(settings.get("obd_port"))
        self._saved = False
        self._scanning = False
        self._scan_status = ""

    def draw(self, surf: pygame.Surface):
        surf.fill(ui.BG)
        ui.rect_btn(surf, "← Back", (8, 8, 70, 28), ui.PANEL, font=ui._F_SM)
        ui.text(surf, "OBD Adapter", W // 2, 10, ui._F_LG, anchor="midtop")

        # Host field
        h_color = ui.ACCENT if self._field == self._FIELD_HOST else ui.BORDER
        pygame.draw.rect(surf, ui.CARD, (10, 46, W // 2 - 15, 30), border_radius=4)
        pygame.draw.rect(surf, h_color, (10, 46, W // 2 - 15, 30), width=2, border_radius=4)
        ui.text(surf, "IP", 14, 48, ui._F_SM, ui.SUBTEXT)
        ui.text(surf, self._host_buf, 14, 62, ui._F_SM, ui.TEXT)

        # Port field
        p_color = ui.ACCENT if self._field == self._FIELD_PORT else ui.BORDER
        pygame.draw.rect(surf, ui.CARD, (W // 2 + 5, 46, W // 2 - 15 - 130, 30), border_radius=4)
        pygame.draw.rect(surf, p_color, (W // 2 + 5, 46, W // 2 - 15 - 130, 30), width=2, border_radius=4)
        ui.text(surf, "Port", W // 2 + 9, 48, ui._F_SM, ui.SUBTEXT)
        ui.text(surf, self._port_buf, W // 2 + 9, 62, ui._F_SM, ui.TEXT)

        # Auto-detect
        ui.rect_btn(surf, "Scanning…" if self._scanning else "Auto-detect",
                    self._SCAN_RECT, ui.ACCENT if not self._scanning else ui.BORDER,
                    font=ui._F_SM, radius=4)
        if self._scan_status and not self._scanning:
            # Right-anchored to the screen edge, not centered on the button —
            # a result like "Found 192.168.4.1:6801" is wider than the
            # 130px-wide button and was running off the right edge of the
            # 480px screen when centered under it.
            ui.text(surf, self._scan_status, W - 8,
                    self._SCAN_RECT[1] + self._SCAN_RECT[3] + 4, ui._F_SM, ui.SUBTEXT, anchor="topright")

        if self._saved:
            ui.text(surf, "Saved!", W // 2, 84, ui._F_SM, ui.SUCCESS, anchor="midtop")

        ui.draw_numpad(surf)

    def handle_touch(self, pos) -> str | None:
        if pos is None:
            return None
        if ui.hit((8, 8, 70, 28), pos):
            return "settings"
        if ui.hit(self._SCAN_RECT, pos):
            self._start_scan()
            return None
        if ui.hit((10, 46, W // 2 - 15, 30), pos):
            self._field = self._FIELD_HOST
            return None
        if ui.hit((W // 2 + 5, 46, W // 2 - 15 - 130, 30), pos):
            self._field = self._FIELD_PORT
            return None

        k = ui.numpad_hit(pos)
        if k is None:
            return None
        buf = self._host_buf if self._field == self._FIELD_HOST else self._port_buf
        if k == "BACKSPACE":
            buf = buf[:-1]
        elif k == "OK":
            settings.set("obd_host", self._host_buf)
            settings.set("obd_port", int(self._port_buf or "0"))
            self._saved = True
            return None
        else:
            buf += k
        if self._field == self._FIELD_HOST:
            self._host_buf = buf
        else:
            self._port_buf = buf
        self._saved = False
        return None

    def _start_scan(self):
        if self._scanning:
            return
        self._scanning = True
        self._scan_status = ""

        def _work():
            found = obd_discovery.discover()
            if found:
                self._host_buf, self._port_buf = found[0], str(found[1])
                self._scan_status = f"Found {found[0]}:{found[1]}"
                self._saved = False
            else:
                self._scan_status = "No adapter found"
            self._scanning = False

        threading.Thread(target=_work, daemon=True).start()


# ── Touch calibration screen ──────────────────────────────────────────────────

_CAL_TARGETS = [(20, 20), (W - 20, 20), (W - 20, H - 20), (20, H - 20)]
_CAL_LABELS  = ["Top-left", "Top-right", "Bottom-right", "Bottom-left"]


class TouchCalScreen:
    def __init__(self, touch_ctrl):
        self._tc   = touch_ctrl
        self._step = 0
        self._raws: list[tuple[int,int]] = []

    def draw(self, surf: pygame.Surface):
        surf.fill(ui.BG)
        if self._step == 99:
            ui.text(surf, "Bad reads — try again", W // 2, H // 2 - 30,
                    ui._F_LG, ui.DANGER, anchor="center")
            ui.rect_btn(surf, "Retry", (W // 2 - 70, H // 2 + 10, 140, 36), ui.ACCENT)
            return
        if self._step >= 4:
            ui.text(surf, "Calibration saved!", W // 2, H // 2 - 20,
                    ui._F_LG, ui.SUCCESS, anchor="center")
            ui.rect_btn(surf, "Done", (W // 2 - 60, H // 2 + 10, 120, 36), ui.ACCENT)
            return

        tx, ty = _CAL_TARGETS[self._step]
        ui.text(surf, "Touch calibration", W // 2, 10, ui._F_LG, anchor="midtop")
        ui.text(surf, f"Tap: {_CAL_LABELS[self._step]}", W // 2, 40,
                ui._F_MD, ui.SUBTEXT, anchor="midtop")
        ui.text(surf, f"Step {self._step + 1} / 4", W // 2, 65,
                ui._F_SM, ui.SUBTEXT, anchor="midtop")
        # Crosshair
        pygame.draw.line(surf, ui.ACCENT, (tx - 14, ty), (tx + 14, ty), 2)
        pygame.draw.line(surf, ui.ACCENT, (tx, ty - 14), (tx, ty + 14), 2)
        pygame.draw.circle(surf, ui.ACCENT, (tx, ty), 8, 2)

    def handle_touch(self, pos) -> str | None:
        if self._step == 99:
            if pos and ui.hit((W // 2 - 70, H // 2 + 10, 140, 36), pos):
                self._step = 0
                self._raws = []
            return None
        if self._step >= 4:
            if pos and ui.hit((W // 2 - 60, H // 2 + 10, 120, 36), pos):
                return "settings"
            return None
        # A real tap (debounced, pressure-checked) arrived — record raw ADC
        if pos is not None:
            raw = self._tc.read_raw()
            self._raws.append(raw)
            self._step += 1
            if self._step == 4:
                self._apply()
        return None

    def _apply(self):
        xs = [r[0] for r in self._raws]
        ys = [r[1] for r in self._raws]
        x_min, x_max = min(xs), max(xs)
        y_min, y_max = min(ys), max(ys)
        # Reject calibration if range is too small (bad reads)
        if (x_max - x_min) < 200 or (y_max - y_min) < 200:
            self._step = 99  # signal bad cal
            return
        settings.set("touch_x_min", x_min)
        settings.set("touch_x_max", x_max)
        settings.set("touch_y_min", y_min)
        settings.set("touch_y_max", y_max)


# ── Logs screen ──────────────────────────────────────────────────────────────

_LOG_LINE_H         = 16
_LOG_LINES_PER_PAGE = 14
_LOG_MAX_LOADED     = 1000   # most recent lines read from disk, before level filtering
_LOG_REFRESH_S      = 2.0


_KNOWN_LEVELS = ("DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL")


def _parse_log_lines(raw_lines: list[str]) -> list[tuple[str, str]]:
    """Returns (filter_level, compact display text) tuples.

    log.exception() dumps a multi-line Python traceback after the actual
    formatted log line — those continuation lines don't match the
    "date time LEVEL name message" format at all, so treating everything
    as a single-line record turned a traceback into a wall of unlabeled,
    uncolored noise indistinguishable from the log lines around it.
    Continuation lines are tagged with the same filter_level as whatever
    real log line preceded them, so a whole exception dump reads (and
    filters) as one unit rather than orphaned garbage.
    """
    out = []
    prev_level = "INFO"
    for line in raw_lines:
        parts = line.rstrip("\n").split(None, 4)
        if len(parts) >= 5 and parts[2] in _KNOWN_LEVELS:
            _date, time_ms, level, _name, msg = parts
            hhmmss = time_ms.split(".")[0]
            prev_level = level
            out.append((level, f"{hhmmss} {level[0]} {msg}"[:60]))
        else:
            out.append((prev_level, ("    " + line.rstrip("\n"))[:60]))
    return out


class LogsScreen:
    def __init__(self):
        self._show_debug = False
        self._page = 0
        self._lines: list[tuple[str, str]] = []
        self._last_load = 0.0
        self._load()

    def _load(self):
        try:
            with open(gauge_logging.log_path()) as f:
                raw = f.readlines()[-_LOG_MAX_LOADED:]
        except Exception:
            raw = []
        parsed = _parse_log_lines(raw)
        self._lines = parsed if self._show_debug else [p for p in parsed if p[0] != "DEBUG"]
        self._last_load = time.monotonic()

    @property
    def _total_pages(self) -> int:
        return max(1, (len(self._lines) + _LOG_LINES_PER_PAGE - 1) // _LOG_LINES_PER_PAGE)

    def _page_lines(self) -> list[tuple[str, str]]:
        # page 0 = most recent lines (end of file); higher pages = older.
        end = len(self._lines) - self._page * _LOG_LINES_PER_PAGE
        start = max(0, end - _LOG_LINES_PER_PAGE)
        return self._lines[start:end]

    @staticmethod
    def _level_color(level: str):
        if level in ("ERROR", "CRITICAL"):
            return ui.DANGER
        if level == "WARNING":
            return (255, 170, 68)
        return ui.TEXT

    _BACK_RECT   = (8, 8, 70, 26)
    _FILTER_RECT = (W - 78, 8, 70, 26)
    _PREV_RECT   = (8, H - 30, 90, 24)
    _NEXT_RECT   = (W - 98, H - 30, 90, 24)

    def draw(self, surf: pygame.Surface):
        # Auto-refresh while looking at the latest page, so it behaves like a live tail.
        if self._page == 0 and time.monotonic() - self._last_load > _LOG_REFRESH_S:
            self._load()

        surf.fill(ui.BG)
        ui.rect_btn(surf, "← Back", self._BACK_RECT, ui.PANEL, font=ui._F_SM)
        ui.text(surf, "Logs", W // 2, 8, ui._F_LG, anchor="midtop")
        ui.rect_btn(surf, "All" if self._show_debug else "Info+",
                    self._FILTER_RECT, ui.CARD, font=ui._F_SM)

        y = 40
        lines = self._page_lines()
        if not lines:
            ui.text(surf, "No log lines", W // 2, H // 2, ui._F_MD, ui.SUBTEXT, anchor="center")
        for level, text in lines:
            ui.text(surf, text, 8, y, ui._F_SM, self._level_color(level))
            y += _LOG_LINE_H

        ui.rect_btn(surf, "‹ Newer", self._PREV_RECT, ui.CARD, font=ui._F_SM)
        ui.text(surf, f"{self._page + 1}/{self._total_pages}", W // 2, H - 26,
                ui._F_SM, ui.SUBTEXT, anchor="midtop")
        ui.rect_btn(surf, "Older ›", self._NEXT_RECT, ui.CARD, font=ui._F_SM)

    def handle_touch(self, pos) -> str | None:
        if pos is None:
            return None
        if ui.hit(self._BACK_RECT, pos):
            return "settings"
        if ui.hit(self._FILTER_RECT, pos):
            self._show_debug = not self._show_debug
            self._load()
            self._page = 0
            return None
        if ui.hit(self._PREV_RECT, pos):
            self._page = max(0, self._page - 1)
            return None
        if ui.hit(self._NEXT_RECT, pos):
            self._page = min(self._total_pages - 1, self._page + 1)
            return None
        return None


# ── Main settings menu ────────────────────────────────────────────────────────

class SettingsScreen:
    _NAV_ITEMS = [
        ("WiFi",             "wifi_list"),
        ("OBD Adapter",      "obd_config"),
        ("Touch Calibrate",  "touch_cal"),
        ("Logs",             "logs"),
    ]
    _BACK_LABEL, _BACK_KEY = "Back to Gauges", "gauges"
    _ROW_H       = 38
    _ROW_GAP     = 4
    _TOP         = 48
    _STATUS_TTL  = 3.0

    def __init__(self):
        self._status = ""
        self._status_until = 0.0

    def set_status(self, msg: str):
        self._status = msg
        self._status_until = time.monotonic() + self._STATUS_TTL

    def _nav_rect(self, i: int):
        y = self._TOP + i * (self._ROW_H + self._ROW_GAP)
        return (30, y, W - 60, self._ROW_H)

    def _action_row_y(self) -> int:
        return self._TOP + len(self._NAV_ITEMS) * (self._ROW_H + self._ROW_GAP)

    def _reconnect_rect(self):
        return (20, self._action_row_y(), W // 2 - 30, self._ROW_H)

    def _restart_rect(self):
        return (W // 2 + 10, self._action_row_y(), W // 2 - 30, self._ROW_H)

    def _back_rect(self):
        y = self._action_row_y() + self._ROW_H + self._ROW_GAP + 18  # room for status line
        return (30, y, W - 60, self._ROW_H)

    def draw(self, surf: pygame.Surface):
        surf.fill(ui.BG)
        ui.text(surf, "Settings", W // 2, 14, ui._F_XL, anchor="midtop")

        for i, (label, _) in enumerate(self._NAV_ITEMS):
            ui.rect_btn(surf, label, self._nav_rect(i), ui.CARD, font=ui._F_LG, radius=8)

        ui.rect_btn(surf, "Reconnect OBD", self._reconnect_rect(), ui.CARD, font=ui._F_MD, radius=8)
        ui.rect_btn(surf, "Restart App", self._restart_rect(), ui.DANGER, font=ui._F_MD, radius=8)

        if self._status and time.monotonic() < self._status_until:
            status_y = self._action_row_y() + self._ROW_H + 6
            ui.text(surf, self._status, W // 2, status_y, ui._F_SM, ui.ACCENT, anchor="midtop")

        ui.rect_btn(surf, self._BACK_LABEL, self._back_rect(), ui.PANEL, font=ui._F_LG, radius=8)

    def handle_touch(self, pos) -> str | None:
        for i, (_, key) in enumerate(self._NAV_ITEMS):
            if ui.hit(self._nav_rect(i), pos):
                return key
        if ui.hit(self._reconnect_rect(), pos):
            return "action:reconnect_obd"
        if ui.hit(self._restart_rect(), pos):
            return "action:restart_app"
        if ui.hit(self._back_rect(), pos):
            return self._BACK_KEY
        return None
