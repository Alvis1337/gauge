OBD_HOST = "192.168.0.10"
OBD_PORT = 35000
OBD_TIMEOUT = 3.0
POLL_INTERVAL = 0.1  # seconds between full PID sweeps

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
