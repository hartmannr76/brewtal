"""Reads a board's serial log into a file (and the screen), optionally resetting it first so the
log starts at boot. Startup prints a line per stage with the milliseconds since boot, so the gap
between two lines is how long that stage took:

    [   1356][I][brewdial] internal RAM after panel + draw buffers: 142 KB free, ...
    [   4969][I][brewdial] internal RAM after screens: 142 KB free, ...   <- 3.6 s building screens

Run it with the PlatformIO (pioarduino) Python, which has pyserial and esptool:

    %USERPROFILE%\\.platformio-pioarduino\\penv\\Scripts\\python.exe tools/device_log.py COM5
    ... tools/device_log.py COM5 --seconds 600 --no-reset --out phone_test.txt

`pio device list` shows the port (an ESP32-S3 is USB 303A:1001).
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

import serial


def esptool_cmd():
    exe = shutil.which('esptool') or shutil.which('esptool.exe')
    if not exe:
        cand = os.path.join(os.path.dirname(sys.executable), 'esptool.exe')
        exe = cand if os.path.exists(cand) else None
    return [exe] if exe else [sys.executable, '-m', 'esptool']


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('port')
    ap.add_argument('--seconds', type=float, default=30)
    ap.add_argument('--out', default='device_log.txt')
    ap.add_argument('--no-reset', action='store_true', help='read what the board is doing now')
    ap.add_argument('--baud', type=int, default=115200)
    a = ap.parse_args()

    if not a.no_reset:   # esptool's reset pulses RTS; the USB port comes back right away on an S3
        r = subprocess.run(esptool_cmd() + ['--chip', 'esp32s3', '--port', a.port, '--after', 'hard_reset', 'chip_id'],
                           capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit('reset failed:\n' + r.stdout + r.stderr)

    s = None
    t0 = time.time()
    while s is None and time.time() - t0 < 10:
        try:
            s = serial.Serial(a.port, a.baud, timeout=0.2)
        except serial.SerialException:
            time.sleep(0.05)
    if s is None:
        sys.exit('could not open ' + a.port)

    end = time.time() + a.seconds
    with open(a.out, 'wb') as f:
        while time.time() < end:
            try:
                b = s.read(4096)
            except serial.SerialException:   # USB went away for a moment: reopen
                s.close()
                time.sleep(0.2)
                try:
                    s = serial.Serial(a.port, a.baud, timeout=0.2)
                except serial.SerialException:
                    pass
                continue
            if b:
                f.write(b)
                f.flush()
                sys.stdout.write(b.decode('utf-8', 'replace'))
                sys.stdout.flush()
    s.close()


if __name__ == '__main__':
    main()
