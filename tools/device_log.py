"""Reads a board's serial log into a file (and the screen), optionally resetting it first so the
log starts at boot. Startup prints a line per stage with the milliseconds since boot, so the gap
between two lines is how long that stage took:

    [   1356][I][brewdial] internal RAM after panel + draw buffers: 142 KB free, ...
    [   1633][I][brewdial] internal RAM after screens: 141 KB free, ...   <- 0.28 s building screens

Run it with the PlatformIO (pioarduino) Python, which has pyserial and esptool:

    %USERPROFILE%\\.platformio-pioarduino\\penv\\Scripts\\python.exe tools/device_log.py COM5
    ... tools/device_log.py COM5 --seconds 600 --no-reset --out phone_test.txt

`pio device list` shows the port (an ESP32-S3 is USB 303A:1001). tools/device_bench.py uses the
functions below.
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


def reset_board(port):
    """Hard reset through the S3's USB-Serial/JTAG (esptool pulses RTS); the port stays up."""
    r = subprocess.run(esptool_cmd() + ['--chip', 'esp32s3', '--port', port, '--after', 'hard_reset', 'chip_id'],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError('reset failed:\n' + r.stdout + r.stderr)


def open_port(port, baud=115200, wait_s=10):
    t0 = time.time()
    while time.time() - t0 < wait_s:
        try:
            return serial.Serial(port, baud, timeout=0.2)
        except serial.SerialException:
            time.sleep(0.05)
    raise RuntimeError('could not open ' + port)


def capture(port, seconds, out_file, echo=True, stop_at=None, baud=115200):
    """Reads for `seconds` (or until a line containing `stop_at` arrives). Returns the text."""
    s = open_port(port, baud)
    end = time.time() + seconds
    buf = b''
    with open(out_file, 'wb') as f:
        while time.time() < end:
            try:
                b = s.read(4096)
            except serial.SerialException:   # USB went away for a moment: reopen
                s.close()
                time.sleep(0.2)
                try:
                    s = serial.Serial(port, baud, timeout=0.2)
                except serial.SerialException:
                    pass
                continue
            if not b:
                continue
            f.write(b)
            f.flush()
            buf += b
            if echo:
                sys.stdout.write(b.decode('utf-8', 'replace'))
                sys.stdout.flush()
            if stop_at and stop_at.encode() in buf:
                break
    s.close()
    return buf.decode('utf-8', 'replace')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('port')
    ap.add_argument('--seconds', type=float, default=30)
    ap.add_argument('--out', default='device_log.txt')
    ap.add_argument('--no-reset', action='store_true', help='read what the board is doing now')
    ap.add_argument('--baud', type=int, default=115200)
    a = ap.parse_args()
    if not a.no_reset:
        try:
            reset_board(a.port)
        except RuntimeError as e:
            sys.exit(str(e))
    try:
        capture(a.port, a.seconds, a.out, baud=a.baud)
    except RuntimeError as e:
        sys.exit(str(e))


if __name__ == '__main__':
    main()
