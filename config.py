import os
import subprocess

OBD_BT_ADDRESS = ""  # BLE MAC address of the ELM327 adapter; set via Settings > OBD Adapter > Scan
OBD_TIMEOUT = 3.0
POLL_INTERVAL = 0.1  # seconds between full PID sweeps


def _detect_version() -> str:
    # There's no build/release pipeline for the Pi app — deploys are just
    # `git pull` + service restart — so the running commit's short SHA is
    # the only thing that reliably identifies "what code is this?".
    try:
        out = subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"],
            cwd=os.path.dirname(os.path.abspath(__file__)),
            timeout=3, capture_output=True, text=True, check=False,
        )
        if out.returncode == 0:
            return out.stdout.strip()
    except Exception:
        pass
    return "unknown"


VERSION = _detect_version()

# fb1 = SPI TFT after driver loads; use fb0 for HDMI testing
FB_DEVICE = "/dev/fb1"

DISPLAY_WIDTH  = 480
DISPLAY_HEIGHT = 320

# (label, min, max, color_rgb)
GAUGE_SPECS = [
    ("BOOST",   -5.0,   25.0, (255,  68,  68)),
    ("RPM",      0.0, 8000.0, ( 68, 170, 255)),
    ("COOLANT", 40.0,  130.0, ( 68, 255, 136)),
    ("OIL",     40.0,  150.0, (255, 170,  68)),
]
