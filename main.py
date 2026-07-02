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
import threading
import time

os.environ["SDL_VIDEODRIVER"] = "offscreen"

import pygame

import gauge_logging
_LOG_PATH = gauge_logging.setup()
log = logging.getLogger("main")
log.info("=== AutoGauge starting, logging to %s ===", _LOG_PATH)

import settings
settings.load()

import config
from display import ST7796
from obd import GaugeData, ObdClient
from renderer import GaugeRenderer
from touch import TouchController
from screens import (
    SettingsScreen, WifiListScreen, WifiPasswordScreen,
    ObdConfigScreen, TouchCalScreen,
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


def _obd_thread(client: ObdClient, data_holder: list, stop_evt: threading.Event,
                host_holder: list):
    retry_delay = 2.0
    while not stop_evt.is_set():
        host = settings.get("obd_host")
        port = settings.get("obd_port")
        host_holder[0] = host
        try:
            client.connect(host, port)
            retry_delay = 2.0
            while not stop_evt.is_set() and client.connected:
                # Reconnect if settings changed
                if settings.get("obd_host") != host or settings.get("obd_port") != port:
                    log.info("OBD host/port changed in settings, reconnecting")
                    break
                data_holder[0] = client.poll()
                time.sleep(config.POLL_INTERVAL)
        except Exception:
            log.exception("OBD error (host=%s port=%s)", host, port)
            client.disconnect()
        if not stop_evt.is_set():
            log.info("retrying OBD connection in %.0fs", retry_delay)
            time.sleep(retry_delay)
            retry_delay = min(retry_delay * 2, 30.0)


def main():
    _lock_file = _acquire_singleton_lock()
    pygame.init()

    stop_evt = threading.Event()

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
    host_ref = [""]

    threading.Thread(target=_obd_thread,
                     args=(client, data_ref, stop_evt, host_ref),
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
        else:                      screen_obj = None

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

                renderer.update(data_ref[0], client.connected)
                renderer.draw()
                # Settings hint dot in corner
                pygame.draw.circle(screen, (60, 60, 60), (10, 6), 5)

            else:
                if screen_obj:
                    next_key = screen_obj.handle_touch(tap)
                    if next_key:
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


if __name__ == "__main__":
    try:
        main()
    except Exception:
        log.exception("fatal crash, exiting")
        raise
    except KeyboardInterrupt:
        log.info("stopped by KeyboardInterrupt")
        raise
    finally:
        log.info("=== AutoGauge exiting ===")
