#!/usr/bin/env python3
"""
Standalone ELM327 emulator server for testing obd.py's ObdClient without a
real vehicle or WiFi OBD adapter. Extends ELM327-emulator's built-in 'car'
scenario (which already covers RPM/MAP/baro/coolant/throttle — everything
obd.py needs from Mode 01) with the two BMW-specific Mode 22 UDS DIDs the
B58 dashboard actually reads (oil temp DID 0x4402, ethanol content DID
0x4010), since those aren't part of any standard scenario.

Requires the ELM327-emulator package — install with:
  python3 -m venv .venv-test && .venv-test/bin/pip install ELM327-emulator

Run with: .venv-test/bin/python3 tests/emulator_server.py -n <port>
Any extra CLI args are forwarded to `python3 -m elm` (see `elm -h`).
"""
import sys

from elm import obd_message
from elm.obd_message import ECU_ADDR_E, ECU_R_ADDR_E, HD, SZ, DT

# raw=0xB8 (184): 184 * 191.25 / 255 - 48 = 90.0 degC  (parsers.parse_oil_temp_4402)
# raw=0x1E (30):  30.0 percent                          (parsers.parse_ethanol)
_BMW_UDS = {
    'OIL_TEMP_4402': {
        'Request': '^224402' + obd_message.ELM_FOOTER,
        'Descr': 'BMW oil temperature (UDS DID 0x4402)',
        'Header': ECU_ADDR_E,
        'Response': HD(ECU_R_ADDR_E) + SZ('04') + DT('62 44 02 B8'),
    },
    'ETHANOL_4010': {
        'Request': '^224010' + obd_message.ELM_FOOTER,
        'Descr': 'BMW ethanol content (UDS DID 0x4010)',
        'Header': ECU_ADDR_E,
        'Response': HD(ECU_R_ADDR_E) + SZ('04') + DT('62 40 10 1E'),
    },
}

obd_message.ObdMessage['bmw_b58'] = {**obd_message.ObdMessage['car'], **_BMW_UDS}

if __name__ == "__main__":
    from elm.interpreter import main
    argv = sys.argv[1:]
    if '-s' not in argv and '--scenario' not in argv:
        argv = ['-s', 'bmw_b58'] + argv
    sys.argv = ['elm'] + argv
    main()
