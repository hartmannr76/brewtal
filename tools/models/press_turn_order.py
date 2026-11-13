"""Timing model of input_task (5 ms, core 0) + loop() (frame every `frame` ms, core 1).
Physical script: press knob at t=0, release at t=120, then turn 1 click at t=120+gap.
Correct UI order is: PRESS, then +1."""
DEB, CPD = 25, 2

def simulate(gap, frame, fixed):
    turn_t = 120 + gap
    count_at = lambda t: 2 if t >= turn_t else 0            # PCNT count (hardware, lossless)
    sw_at = lambda t: 0 if 0 <= t < 120 else 1               # knob switch, active low
    press_q, applied, out = [], 0, []
    hold, hold_cnt = False, 0
    raw = stable = 1; changed = 0; down = 0; edge = 0
    for t in range(0, 400):
        if t % 5 == 0:                                       # ---- input_task sample
            s = sw_at(t)
            if s != raw:
                if raw == stable: edge = count_at(t)         # first deviation = the real edge
                raw, changed = s, t
                if fixed and stable == 0 and raw == 1: hold, hold_cnt = True, edge
                if fixed and raw == stable: hold = False      # bounce aborted the transition
            if raw != stable and t - changed >= DEB:
                stable = raw
                c = edge if fixed else count_at(t)
                if stable == 0: down = c
                else:
                    if abs(c - down) < 2 * CPD: press_q.append(c)
                    hold = False                              # (fixed: after queueing)
        if t % frame == 0:                                   # ---- loop()
            held, hc = hold, hold_cnt                        # fixed: read hold BEFORE draining
            while press_q:
                pc = press_q.pop(0)
                d = (pc - applied) // CPD; applied += d * CPD
                if d: out.append(f"{d:+d}")
                out.append("PRESS")
            lim = (hc if (fixed and held) else count_at(t))
            d = (lim - applied) // CPD; applied += d * CPD
            if d: out.append(f"{d:+d}")
    return out

for fixed in (False, True):
    print("FIXED" if fixed else "CURRENT")
    for gap in (5, 15, 40, 200):
        for frame in (16, 33, 60):
            r = simulate(gap, frame, fixed)
            ok = r == ["PRESS", "+1"]
            print(f"  turn {gap:3d} ms after release, frame {frame:2d} ms: {' '.join(r):12s} {'ok' if ok else 'WRONG ORDER'}")
