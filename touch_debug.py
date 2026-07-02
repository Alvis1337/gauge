#!/usr/bin/env python3
import spidev, time

spi = spidev.SpiDev()
spi.open(1, 0)
spi.max_speed_hz = 50_000
spi.mode = 0

def read(cmd):
    rx = spi.xfer2([cmd, 0, 0])
    return ((rx[1] << 8) | rx[2]) >> 3

def pressure():
    z1 = read(0xB0)
    z2 = read(0xC0)
    return z1 - z2 + 4095 if z1 > 0 else 0

def wait_for_real_touch():
    """Require 5 consecutive readings above threshold before accepting."""
    count = 0
    while count < 5:
        if pressure() > 500:
            count += 1
        else:
            count = 0
        time.sleep(0.02)

def wait_for_release():
    count = 0
    while count < 5:
        if pressure() < 200:
            count += 1
        else:
            count = 0
        time.sleep(0.02)

corners = ["TOP-LEFT", "TOP-RIGHT", "BOTTOM-RIGHT", "BOTTOM-LEFT"]
results = []

for corner in corners:
    print(f"\nTap and HOLD the physical {corner} corner...", flush=True)
    wait_for_real_touch()
    # Average 5 readings for stability
    xs, ys = [], []
    for _ in range(5):
        xs.append(read(0xD0))
        ys.append(read(0x90))
        time.sleep(0.02)
    x = sum(xs) // len(xs)
    y = sum(ys) // len(ys)
    print(f"  raw X={x}  Y={y}", flush=True)
    results.append((corner, x, y))
    wait_for_release()
    time.sleep(0.3)

print("\n=== Results ===", flush=True)
for corner, x, y in results:
    print(f"  {corner}: X={x}, Y={y}", flush=True)
spi.close()
