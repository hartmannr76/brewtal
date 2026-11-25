"""On-device UI benchmark: flashes the bench build, reads its results and keeps a history.

    %USERPROFILE%\\.platformio-pioarduino\\penv\\Scripts\\python.exe tools/device_bench.py COM5
    ... --no-flash            the board already runs the bench build: just reset and read
    ... --check               exit 1 when a key number got >15% worse than the last run
    ... --note "arc fix"      saved with the run
    ... --no-save             don't add the run to the history
    ... --fit                 also fit the host cost model (sim/harness/bench_model.h) to this run

The bench build (env crowpanel21_bench: the demo + lib/app bench.h) waits for boot to settle,
then runs a fixed script through the same input path as the knob and logs one line per phase:
idle, 16 knob steps up, 16 down, the menu opened and closed, and the whole screen redrawn every
frame. Boot time and internal RAM come from the startup log. Every run is appended to
bench/results.csv (date, commit, env, phase, metric, value) so the numbers can be followed over
time; commit that file with the change it measures.

The host runs the same script without a device (sim/harness bench_check, a ctest, and the web
harness's Performance card), estimating frame times from the rendered area with the model in
sim/harness/bench_model.h. --fit prints new constants for it from this run.
"""
import argparse
import csv
import datetime
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import device_log  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS = os.path.join(ROOT, 'bench', 'results.csv')
FIELDS = ['run_at', 'commit', 'dirty', 'env', 'note', 'phase', 'metric', 'value']

# metric -> +1 higher is better, -1 lower is better. Only these can fail --check; the rest are shown.
KEY = {'fps': +1, 'avg_ms': -1, 'display_ready_ms': -1, 'screens_ms': -1, 'internal_free_kb': +1}
INFO = {'max_ms': -1, 'px_per_frame': -1, 'rendered_px_per_frame': -1, 'frames': +1, 'largest_block_kb': +1}
TOLERANCE = 0.15   # identical runs agree within ~3% (measured 2026-11-25)


def find_pio():
    for c in (shutil.which('pio'),
              os.path.expandvars(r'%USERPROFILE%\.platformio\penv\Scripts\pio.exe'),
              os.path.expandvars(r'%USERPROFILE%\.platformio-pioarduino\penv\Scripts\pio.exe'),
              os.path.expanduser('~/.platformio/penv/bin/pio')):
        if c and os.path.exists(c):
            return c
    sys.exit('pio not found')


def git(*args):
    try:
        return subprocess.run(['git'] + list(args), cwd=ROOT, capture_output=True, text=True).stdout.strip()
    except OSError:
        return ''


def parse(log):
    """-> {(phase, metric): value}"""
    out = {}
    stage = {}
    for line in log.splitlines():
        m = re.match(r'\[\s*(\d+)\]\[I\]\[brewdial\] (.*)', line)
        if m:
            ms, msg = int(m.group(1)), m.group(2)
            s = re.match(r'internal RAM after (.*?): (\d+) KB free, largest block (\d+) KB', msg)
            if s:
                stage[s.group(1)] = ms
            r = re.match(r'display ready \(.*internal free (\d+) KB, largest block (\d+) KB\)', msg)
            if r:
                out[('boot', 'display_ready_ms')] = ms
                out[('boot', 'internal_free_kb')] = int(r.group(1))
                out[('boot', 'largest_block_kb')] = int(r.group(2))
        b = re.search(r'BENCH phase=(\S+) (.*)', line)
        if b:
            for kv in b.group(2).split():
                k, _, v = kv.partition('=')
                if k != 'ms':
                    out[(b.group(1), k)] = float(v)
    if 'screens' in stage and 'panel + draw buffers' in stage:
        out[('boot', 'screens_ms')] = stage['screens'] - stage['panel + draw buffers']
    return out


def load_previous(env):
    if not os.path.exists(RESULTS):
        return None, {}
    with open(RESULTS, newline='', encoding='utf-8') as f:
        rows = [r for r in csv.DictReader(f) if r['env'] == env]
    if not rows:
        return None, {}
    last = rows[-1]['run_at']
    prev = {(r['phase'], r['metric']): float(r['value']) for r in rows if r['run_at'] == last}
    head = next(r for r in rows if r['run_at'] == last)
    return head, prev


def fit(now):
    """Frame time = base + per-pixel cost, fitted to (rendered px, avg ms) per phase, minimising
    relative error so small frames count as much as big ones."""
    pts = [(now[(p, 'rendered_px_per_frame')], now[(p, 'avg_ms')]) for (p, m) in now
           if m == 'avg_ms' and (p, 'rendered_px_per_frame') in now and now[(p, 'avg_ms')] > 0]
    if len(pts) < 2:
        print('fit: needs rendered_px_per_frame and avg_ms for two or more phases')
        return
    sw = swx = swxx = swy = swxy = 0.0
    for x, y in pts:
        w = 1.0 / (y * y)
        sw, swx, swxx, swy, swxy = sw + w, swx + w * x, swxx + w * x * x, swy + w * y, swxy + w * x * y
    b = (sw * swxy - swx * swy) / (sw * swxx - swx * swx)
    a = (swy - b * swx) / sw
    print('\ncost model fit (paste into sim/harness/bench_model.h):')
    print(f'constexpr double kBenchBaseUs = {a * 1000:.1f};')
    print(f'constexpr double kBenchUsPerPx = {b * 1000:.4f};')
    for x, y in pts:
        e = a + b * x
        print(f'  {x:>9.0f} px: measured {y:6.1f} ms, model {e:6.1f} ms ({(e - y) / y * 100:+.0f}%)')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('port')
    ap.add_argument('--env', default='crowpanel21_bench')
    ap.add_argument('--no-flash', action='store_true')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--no-save', action='store_true')
    ap.add_argument('--note', default='')
    ap.add_argument('--fit', action='store_true')
    ap.add_argument('--timeout', type=float, default=120)
    a = ap.parse_args()

    if not a.no_flash:
        print(f'flashing {a.env} ...', flush=True)
        env = dict(os.environ, PYTHONIOENCODING='utf-8', PYTHONUTF8='1')
        r = subprocess.run([find_pio(), 'run', '-e', a.env, '-t', 'upload', '--upload-port', a.port], cwd=ROOT,
                           capture_output=True, text=True, encoding='utf-8', errors='replace', env=env)
        if r.returncode != 0:
            sys.exit('flash failed:\n' + r.stdout[-3000:] + r.stderr[-2000:])
    device_log.reset_board(a.port)
    raw = os.path.join(tempfile.gettempdir(), f'brew_bench_{a.env}.log')
    print('running the benchmark (about a minute) ...', flush=True)
    log = device_log.capture(a.port, a.timeout, raw, echo=False, stop_at='BENCH done')
    if 'BENCH done' not in log:
        sys.exit(f'no "BENCH done" within {a.timeout:.0f} s (is {a.env} a bench build?). Log: {raw}')
    now = parse(log)

    head, prev = load_previous(a.env)
    print(f'\n{a.env}, log: {raw}')
    if head:
        print(f'compared with {head["run_at"]} ({head["commit"]}{"+" if head["dirty"] == "1" else ""}'
              f'{", " + head["note"] if head["note"] else ""})')
    print(f'{"phase":<12} {"metric":<18} {"before":>10} {"now":>10} {"change":>8}')
    worse = []
    for (phase, metric), v in now.items():
        old = prev.get((phase, metric))
        sense = KEY.get(metric, INFO.get(metric, 0))
        change, flag = '', ''
        if old not in (None, 0):
            pct = (v - old) / abs(old)
            change = f'{pct * 100:+.0f}%'
            if sense and -sense * pct > TOLERANCE:
                flag = '  WORSE' if metric in KEY else '  (worse)'
                if metric in KEY:
                    worse.append(f'{phase}.{metric}')
        fmt = lambda x: '' if x is None else (f'{x:.1f}' if isinstance(x, float) and x != int(x) else f'{x:.0f}')
        print(f'{phase:<12} {metric:<18} {fmt(old):>10} {fmt(v):>10} {change:>8}{flag}')

    if not a.no_save:
        os.makedirs(os.path.dirname(RESULTS), exist_ok=True)
        new_file = not os.path.exists(RESULTS)
        run_at = datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')
        commit, dirty = git('rev-parse', '--short', 'HEAD'), '1' if git('status', '--porcelain', '--untracked-files=no') else '0'
        with open(RESULTS, 'a', newline='', encoding='utf-8') as f:
            w = csv.DictWriter(f, FIELDS)
            if new_file:
                w.writeheader()
            for (phase, metric), v in now.items():
                w.writerow({'run_at': run_at, 'commit': commit, 'dirty': dirty, 'env': a.env, 'note': a.note,
                            'phase': phase, 'metric': metric, 'value': v})
        print(f'\nsaved to {os.path.relpath(RESULTS, ROOT)}')
    if a.fit:
        fit(now)
    if worse:
        print(f'more than {TOLERANCE * 100:.0f}% worse than the last run: {", ".join(worse)}')
        if a.check:
            sys.exit(1)


if __name__ == '__main__':
    main()
