#!/usr/bin/env python3
"""
AutoGauge — Pi display for BMW B58 OBD2 gauges.
ST7796 4" SPI TFT. Touch via XPT2046 on SPI1.
Long-press bottom-right corner → Settings.
"""
import fcntl
import logging
import os
import signal
import sys
import threading
import time

os.environ["SDL_VIDEODRIVER"] = "offscreen"
# This app never plays sound, but pygame.init() initializes the audio
# mixer along with everything else — on this Pi that meant SDL polling a
# real ALSA device that isn't fed properly, spamming ~2 underrun errors/sec
# into the journal for the service's entire runtime (3000+ lines in 27
# minutes), drowning out anything useful in `journalctl -u autogauge`.
os.environ["SDL_AUDIODRIVER"] = "dummy"

import pygame

import gauge_logging
_LOG_PATH = gauge_logging.setup()
log = logging.getLogger("main")
log.info("=== AutoGauge starting, logging to %s ===", _LOG_PATH)

import settings
settings.load()

import config
import netdiag
import bt_discovery
from display import ST7796
from obd import GaugeData, ObdClient
from renderer import GaugeRenderer
from touch import TouchController
from screens import (
    SettingsScreen, WifiListScreen, WifiPasswordScreen,
    ObdConfigScreen, TouchCalScreen, LogsScreen,
)

_CORNER_RECT = (0, 0, 80, 60)       # physical top-left = pygame (0,0)
_LONG_MS     = 800                   # ms to hold for settings
_LOCK_PATH   = os.path.join(os.path.dirname(__file__), "autogauge.lock")


def _acquire_singleton_lock():
    # Two instances (e.g. a manual `python3 main.py` left running alongside
    # the systemd service) silently fight over the same touch/display SPI
    # devices — no exclusive-open enforcement at the OS level — which
    # produces exactly the kind of intermittent hangs and clobbered
    # settings writes that are miserable to diagnose. Refuse to start
    # instead of racing another instance.
    lock_file = open(_LOCK_PATH, "w")
    try:
        fcntl.flock(lock_file, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        log.error("another AutoGauge instance already holds %s — refusing to start", _LOCK_PATH)
        raise SystemExit(1)
    lock_file.write(str(os.getpid()))
    lock_file.flush()
    return lock_file  # keep alive for process lifetime; exit releases the flock automatically


_DISCOVERY_AFTER_FAILURES = 3   # consecutive failed *connect* attempts before trying auto-discovery
_STATUS_POLL_INTERVAL_S = 5.0   # how often the status bar's WiFi/BT checks re-shell out


def _status_thread(status_holder: list, stop_evt: threading.Event):
    """Keeps the status bar's WiFi/Bluetooth indicators fed without shelling
    out to nmcli/bluetoothctl on the render thread (30fps would make that
    a subprocess spawn per frame)."""
    while not stop_evt.is_set():
        status_holder[0] = {
            "wifi": netdiag.wifi_connected(),
            "bt": netdiag.bt_powered(),
        }
        stop_evt.wait(_STATUS_POLL_INTERVAL_S)


def _obd_thread(client: ObdClient, data_holder: list, stop_evt: threading.Event,
                address_holder: list, force_reconnect_evt: threading.Event):
    retry_delay = 2.0
    consecutive_failures = 0   # only counts failed connect() attempts, not mid-session drops
    while not stop_evt.is_set():
        address = settings.get("obd_bt_address")
        address_holder[0] = address
        force_reconnect_evt.clear()

        try:
            client.connect(address)
        except Exception:
            log.exception("OBD connect failed (address=%s)", address)
            # Enough evidence to tell "adapter off/out of range" apart
            # from "found it but the ELM327 handshake didn't negotiate"
            # from the session log alone, after the fact.
            log.warning("network diagnostics: %s", netdiag.snapshot(address))
            client.disconnect()
            consecutive_failures += 1
        else:
            retry_delay = 2.0
            consecutive_failures = 0
            try:
                while not stop_evt.is_set() and client.connected:
                    # Reconnect if settings changed
                    if settings.get("obd_bt_address") != address:
                        log.info("OBD address changed in settings, reconnecting")
                        break
                    if force_reconnect_evt.is_set():
                        log.info("manual reconnect requested, forcing fresh connection")
                        client.disconnect()
                        break
                    data_holder[0] = client.poll()
                    time.sleep(config.POLL_INTERVAL)
            except Exception:
                log.exception("OBD polling error (address=%s)", address)
                client.disconnect()

        if stop_evt.is_set():
            break

        # The configured address is genuinely unreachable (not just a blip
        # mid-session) — see if the adapter shows up somewhere else before
        # burning through the rest of the backoff schedule.
        if consecutive_failures >= _DISCOVERY_AFTER_FAILURES:
            log.info("OBD adapter at %s unreachable after %d attempts, trying auto-discovery",
                      address, consecutive_failures)
            found = bt_discovery.discover()
            if found and found != address:
                log.info("auto-discovery found adapter at %s, saving as new default", found)
                settings.set("obd_bt_address", found)
                settings.set("obd_bt_name", "")
                retry_delay = 2.0
            consecutive_failures = 0

        if force_reconnect_evt.is_set():
            retry_delay = 2.0  # a manual reconnect shouldn't wait out a stale backoff
        else:
            log.info("retrying OBD connection in %.0fs", retry_delay)

        deadline = time.monotonic() + retry_delay
        while (time.monotonic() < deadline and not stop_evt.is_set()
               and not force_reconnect_evt.is_set()):
            time.sleep(0.1)
        retry_delay = min(retry_delay * 2, 30.0)


def main() -> bool:
    """Runs the app until shutdown; returns True if a restart was requested
    from the settings UI (caller is expected to os.execv in that case)."""
    _lock_file = _acquire_singleton_lock()
    pygame.init()

    stop_evt = threading.Event()
    force_reconnect_evt = threading.Event()
    restart_requested = threading.Event()

    def _handle_term(signum, frame):
        # pygame/SDL installs its own SIGTERM/SIGINT handler that turns the
        # signal into a QUIT event instead of letting the OS kill us — since
        # nothing polled the event queue, that made `systemctl stop/restart`
        # hang until the timeout forced a SIGKILL. Registering after
        # pygame.init() overrides SDL's handler with this one.
        log.info("received signal %d, shutting down", signum)
        stop_evt.set()

    signal.signal(signal.SIGTERM, _handle_term)
    signal.signal(signal.SIGINT, _handle_term)

    screen   = pygame.Surface((config.DISPLAY_WIDTH, config.DISPLAY_HEIGHT))
    display  = ST7796()
    renderer = GaugeRenderer(screen)
    touch    = TouchController()
    client   = ObdClient()
    data_ref = [GaugeData()]
    address_ref = [""]
    status_ref = [{"wifi": False, "bt": False}]

    threading.Thread(target=_obd_thread,
                     args=(client, data_ref, stop_evt, address_ref, force_reconnect_evt),
                     daemon=True).start()
    threading.Thread(target=_status_thread,
                     args=(status_ref, stop_evt),
                     daemon=True).start()

    # Screen stack
    current      = "gauges"
    screen_obj   = None          # non-gauge screen instance
    corner_down  = None          # time when corner press started

    clock = pygame.time.Clock()

    def make_screen(key: str):
        nonlocal screen_obj
        if key == "settings":      screen_obj = SettingsScreen()
        elif key == "wifi_list":   screen_obj = WifiListScreen()
        elif key.startswith("wifi_connect:"):
            ssid = key.split(":", 1)[1]
            screen_obj = WifiPasswordScreen(ssid)
        elif key == "obd_config":  screen_obj = ObdConfigScreen()
        elif key == "touch_cal":   screen_obj = TouchCalScreen(touch)
        elif key == "logs":        screen_obj = LogsScreen()
        else:                      screen_obj = None

    def _handle_action(action: str) -> str:
        """Runs a settings-screen action button; returns a short status
        string for on-screen feedback instead of switching screens."""
        if action == "reconnect_obd":
            log.info("manual OBD reconnect requested from settings UI")
            force_reconnect_evt.set()
            return "Reconnecting..."
        if action == "restart_app":
            log.info("manual app restart requested from settings UI")
            restart_requested.set()
            stop_evt.set()
            return "Restarting..."
        log.warning("unknown settings action: %r", action)
        return ""

    _was_touched = False   # debounce state

    try:
        while not stop_evt.is_set():
            pygame.event.pump()
            raw = touch.read()                       # continuous — used for long press
            tap = raw if (raw and not _was_touched) else None  # rising edge only — used for button taps
            _was_touched = raw is not None

            if current == "gauges":
                # Long-press bottom-right corner → settings (uses continuous raw)
                if raw and pygame.Rect(_CORNER_RECT).collidepoint(raw):
                    if corner_down is None:
                        corner_down = time.monotonic()
                    elif time.monotonic() - corner_down > _LONG_MS / 1000:
                        current = "settings"
                        make_screen("settings")
                        corner_down = None
                        _was_touched = True   # suppress immediate tap in new screen
                else:
                    corner_down = None

                renderer.update(data_ref[0], client.connected,
                                status_ref[0]["wifi"], status_ref[0]["bt"])
                renderer.draw()
                # Settings hint dot in corner (below the status bar, out of its way)
                pygame.draw.circle(screen, (60, 60, 60), (10, 50), 5)

            else:
                if screen_obj:
                    next_key = screen_obj.handle_touch(tap)
                    if next_key:
                        if next_key.startswith("action:"):
                            status = _handle_action(next_key[len("action:"):])
                            if hasattr(screen_obj, "set_status"):
                                screen_obj.set_status(status)
                        else:
                            if hasattr(screen_obj, 'cancel'):
                                screen_obj.cancel()
                            if next_key == "gauges":
                                current = "gauges"
                                screen_obj = None
                            else:
                                current = next_key
                                make_screen(next_key)
                    if screen_obj:
                        screen_obj.draw(screen)

            display.show(screen)
            clock.tick(30)

    finally:
        stop_evt.set()
        client.disconnect()
        touch.close()
        display.close()
        pygame.quit()

    return restart_requested.is_set()


if __name__ == "__main__":
    _restart = False
    try:
        _restart = main()
    except Exception:
        log.exception("fatal crash, exiting")
        raise
    except KeyboardInterrupt:
        log.info("stopped by KeyboardInterrupt")
        raise
    finally:
        log.info("=== AutoGauge exiting ===")

    if _restart:
        log.info("re-executing process for restart")
        os.execv(sys.executable, [sys.executable, "-u"] + sys.argv)
