"""A small FSRS-4.5 scheduler, enough for the mock server to give realistic intervals.

This stands in for Rekalyptus's real scheduler. Grades: 1 Again, 2 Hard, 3 Good, 4 Easy.
"""
import math

W = [0.4872, 1.4003, 3.7145, 13.8206, 5.1618, 1.2298, 0.8975, 0.031, 1.6474, 0.1367,
     1.0461, 2.1072, 0.0793, 0.3246, 1.587, 0.2272, 2.8755]
DECAY, FACTOR = -0.5, 19 / 81
RETENTION = 0.9
DAY_MS = 86_400_000
RELEARN_MS = 10 * 60_000      # an Again comes back in 10 minutes


def _clamp_d(d):
    return min(10.0, max(1.0, d))


def init_difficulty(g):
    return _clamp_d(W[4] - (g - 3) * W[5])


def retrievability(elapsed_days, s):
    return (1 + FACTOR * elapsed_days / s) ** DECAY


def interval_days(s):
    return max(1, round(s / FACTOR * (RETENTION ** (1 / DECAY) - 1)))


def next_state(state, g, now_ms):
    """state: dict with s (stability, days), d (difficulty), last_ms, reps, lapses (s None for a new card).
    Returns (new_state, due_ms)."""
    s, d = state.get("s"), state.get("d")
    if s is None:
        s, d = W[g - 1], init_difficulty(g)
    else:
        elapsed = max(0.0, (now_ms - (state.get("last_ms") or now_ms)) / DAY_MS)
        r = retrievability(elapsed, s)
        d_new = d - W[6] * (g - 3)
        d = _clamp_d(W[7] * init_difficulty(4) + (1 - W[7]) * d_new)
        if g == 1:
            s = W[11] * d ** (-W[12]) * ((s + 1) ** W[13] - 1) * math.exp((1 - r) * W[14])
        else:
            mult = (W[15] if g == 2 else 1.0) * (W[16] if g == 4 else 1.0)
            s = s * (1 + math.exp(W[8]) * (11 - d) * s ** (-W[9]) * (math.exp((1 - r) * W[10]) - 1) * mult)
    s = max(0.1, s)
    new = dict(state, s=s, d=d, last_ms=now_ms, reps=state.get("reps", 0) + 1,
               lapses=state.get("lapses", 0) + (1 if g == 1 else 0))
    due = now_ms + (RELEARN_MS if g == 1 else interval_days(s) * DAY_MS)
    return new, due


def preview(state, now_ms):
    """Due time for each grade from this state, made monotonic so Hard <= Good < Easy."""
    dues = [next_state(state, g, now_ms)[1] for g in (1, 2, 3, 4)]
    dues[1] = min(dues[1], dues[2])
    dues[3] = max(dues[3], dues[2] + DAY_MS)
    return [d - now_ms for d in dues]


def label(ms):
    m = ms / 60_000
    if m < 60:
        return f"{max(1, round(m))}m"
    h = m / 60
    if h < 24:
        return f"{round(h)}h"
    d = h / 24
    if d < 14:
        return f"{round(d)}d"
    if d < 60:
        return f"{round(d / 7)}w"
    if d < 365:
        return f"{round(d / 30)}mo"
    return f"{d / 365:.1f}y"
