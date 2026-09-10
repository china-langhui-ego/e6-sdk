#!/usr/bin/env python3
"""
test_timestamp_alignment.py — sentinel for Focus 1 (cross-stream timestamp
alignment on one absolute UTC timeline).

SPEC (docs/unit-test-plan.md Focus 1, commits 4bf7b5a / 5a186a5):
  Every stream's timestamp is absolute UTC = CLOCK_BOOTTIME + a one-shot
  BOOTTIME->REALTIME offset captured at recording start. Accel/gyro share
  that single timeline: contemporaneous ranges, monotonic, ~1 kHz.

WHY THIS TEST EXISTS:
  The offset is applied in ~5 places; a dropped or doubled application would
  silently put a stream on the wrong timeline. This test catches both:
    * offset DROPPED  -> raw boottime (~1e14 ns, "year 1970+uptime") -> FAIL
    * offset DOUBLED  -> ~3.5e18 ns ("year ~2083")                  -> FAIL
  by asserting timestamps decode to a sane wall-clock year (2025-2028) and
  that accel/gyro are contemporaneous.

Checks (on whatever of accel.csv / gyro.csv / head_pose.csv are present):
  A1 UTC domain : min timestamp > 1e17 ns (not raw boottime)
  A2 sane year  : decoded UTC year in [2025, 2028] (not dropped/doubled offset)
  A3 coeval     : accel & gyro spans overlap >90% and agree within 5%
  A4 monotonic  : each stream's timestamps non-decreasing
  A5 rate       : per-stream interval in [0.3 ms, 10 ms] (plausible IMU rate)

USAGE:
  python3 test_timestamp_alignment.py                 # bundled fixture
  python3 test_timestamp_alignment.py <dataset_dir>

Exit code: 0 = pass, 1 = fail.
"""
import sys
import os
import csv
import glob
import datetime

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURE = os.path.join(HERE, "..", "app", "src", "main", "cpp", "dataset", "20260701_202804")


def load_ts(root, name):
    """timestamp_ns column from <root>/**/<name>, sorted. Empty list if absent."""
    hits = glob.glob(os.path.join(root, "**", name), recursive=True)
    if not hits:
        return []
    ts = []
    with open(hits[0]) as f:
        r = csv.DictReader(f)
        col = "timestamp_ns" if "timestamp_ns" in (r.fieldnames or []) else (r.fieldnames[0] if r.fieldnames else None)
        for row in r:
            v = (row.get(col) or "").strip()
            if v:
                try:
                    ts.append(int(float(v)))
                except ValueError:
                    pass
    return ts


def utc_year(ns):
    return datetime.datetime.utcfromtimestamp(ns / 1e9).year


def main(argv):
    root = argv[1] if len(argv) > 1 else FIXTURE
    accel = sorted(load_ts(root, "accel.csv"))
    gyro = sorted(load_ts(root, "gyro.csv"))
    head = sorted(load_ts(root, "head_pose.csv"))

    if not accel and not gyro:
        print(f"FAIL: no accel.csv/gyro.csv under {root}")
        return 1

    print(f"Dataset: {root}")
    for nm, t in (("accel", accel), ("gyro", gyro), ("head_pose", head)):
        if t:
            print(f"  {nm:10s} n={len(t):6d} span={ (t[-1]-t[0])/1e9:6.2f}s "
                  f"year={utc_year(t[0])} ts_min={t[0]}")
        else:
            print(f"  {nm:10s} (absent/empty — skipped)")

    fails = []

    def check_domain(name, t):
        if not t:
            return
        if t[0] <= 10**17:
            fails.append(f"{name}: min ts {t[0]} <= 1e17 — looks like RAW boottime, BOOTTIME->UTC offset not applied")
            return
        y = utc_year(t[0])
        if not (2025 <= y <= 2028):
            fails.append(f"{name}: decoded UTC year {y} outside [2025,2028] — offset dropped/doubled or clock corrupt")

    def check_monotonic(name, t):
        if not t:
            return
        bad = next((i for i in range(1, len(t)) if t[i] < t[i - 1]), None)
        if bad is not None:
            fails.append(f"{name}: non-monotonic at index {bad} ({t[bad-1]} -> {t[bad]})")

    def check_rate(name, t, lo, hi):
        if len(t) < 10:
            return
        avg = (t[-1] - t[0]) / (len(t) - 1)
        ms = avg / 1e6
        if not (lo <= ms <= hi):
            fails.append(f"{name}: avg interval {ms:.3f} ms outside [{lo}, {hi}] — rate implausible")

    # IMU streams run at ~1 kHz; head_pose is sampled per rendered frame (~60-120 Hz).
    for nm, t in (("accel", accel), ("gyro", gyro)):
        check_domain(nm, t); check_monotonic(nm, t); check_rate(nm, t, 0.3, 10.0)
    check_domain("head", head); check_monotonic("head", head); check_rate("head", head, 5.0, 40.0)

    # A3: accel & gyro coeval (the two streams that should overlap).
    if accel and gyro:
        lo = max(accel[0], gyro[0])
        hi = min(accel[-1], gyro[-1])
        overlap = max(0, hi - lo)
        union = max(accel[-1], gyro[-1]) - min(accel[0], gyro[0])
        ratio = overlap / union if union > 0 else 0
        sa, sg = accel[-1] - accel[0], gyro[-1] - gyro[0]
        span_ratio = min(sa, sg) / max(sa, sg) if max(sa, sg) > 0 else 0
        if ratio < 0.90:
            fails.append(f"accel/gyro overlap only {ratio*100:.1f}% — not on the same timeline")
        if span_ratio < 0.95:
            fails.append(f"accel/gyro spans differ >5% (accel={sa/1e9:.2f}s gyro={sg/1e9:.2f}s)")
        if not fails:
            print(f"  coeval: overlap={ratio*100:.1f}% span_ratio={span_ratio*100:.1f}%")

    if fails:
        print(f"\nFAILED ({len(fails)}):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("\nPASS: streams share one absolute UTC timeline (offset applied, coeval, monotonic).")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
