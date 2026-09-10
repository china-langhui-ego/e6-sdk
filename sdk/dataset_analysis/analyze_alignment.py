#!/usr/bin/env python3
"""
analyze_alignment.py — quantify cross-stream timestamp alignment in a dataset.

Reference timeline: RGB video mid-exposure (rgb_metainfo.csv::mid_exposure_utc_ns).
For every RGB frame it checks, at that exact instant:
  - head_pose.csv        : exact-match? (proves pose was re-stamped to RGB mid-exposure)
  - hand_tracking.csv / controller_poses.csv : exact-match?
  - accel.csv / gyro.csv : nearest-sample delta (ms)  (IMU is an independent stream -> post-align)
And cross-camera sync: each tracking/ctrl frame's mid_exposure vs nearest RGB mid_exposure.

All columns are absolute UTC ns (CLOCK_BOOTTIME + one-shot BOOTTIME->REALTIME offset),
so a delta is a true time misalignment, not a domain offset.

Usage:  python3 analyze_alignment.py <dataset_dir>
"""
import csv, sys, os, bisect, statistics as st

def read_timestamps(path):
    """Timestamp column (header-aware: timestamp_ns / timestamp) from a CSV."""
    out = []
    if not os.path.exists(path): return out
    with open(path) as f:
        r = csv.DictReader(f)
        if not r.fieldnames: return out
        col = None
        for c in ("timestamp_ns", "timestamp", "timestamp_ns ", "timestamp "):
            if c in r.fieldnames: col = c; break
        if col is None:  # fall back to first column
            col = r.fieldnames[0]
        for row in r:
            v = row.get(col, "").strip()
            if v:
                try: out.append(int(float(v)))
                except ValueError: pass
    return out

def read_metainfo_mid(path):
    if not os.path.exists(path): return []
    with open(path) as f:
        r = csv.DictReader(f)
        return [int(x["mid_exposure_utc_ns"]) for x in r if x.get("mid_exposure_utc_ns")]

def pct(vals, p):
    if not vals: return float("nan")
    vals = sorted(vals); k = (len(vals)-1)*p/100; lo=int(k); hi=min(lo+1,len(vals)-1)
    return vals[lo] + (vals[hi]-vals[lo])*(k-lo)

def nearest_idx(sorted_ts, t):
    """Index of closest timestamp to t; clamps to [0, len-1]."""
    if not sorted_ts: return -1
    i = bisect.bisect_left(sorted_ts, t)
    if i >= len(sorted_ts): i = len(sorted_ts) - 1
    if i > 0 and abs(t - sorted_ts[i-1]) < abs(t - sorted_ts[i]): i -= 1
    return i

def summ(name, deltas_ms):
    if not deltas_ms:
        print(f"  {name:22s}: (no data)"); return
    ad = [abs(d) for d in deltas_ms]
    print(f"  {name:22s}: n={len(deltas_ms):4d}  mean={st.mean(ad):7.3f}ms  "
          f"p50={pct(ad,50):7.3f}  p95={pct(ad,95):7.3f}  max={max(ad):8.3f}ms")

def main(d):
    rgb = read_metainfo_mid(os.path.join(d, "rgb_metainfo.csv"))
    if not rgb:
        print("No rgb_metainfo.csv (not a camera-on recording, or old format)."); return
    rgb_sorted = sorted(rgb); rgb_set = set(rgb)
    print(f"Dataset: {d}")
    print(f"RGB frames: {len(rgb)}  range=[{rgb_sorted[0]} .. {rgb_sorted[-1]}] "
          f"({(rgb_sorted[-1]-rgb_sorted[0])/1e9:.2f}s)")
    print()
    print("Per-RGB-frame alignment at mid_exposure_utc_ns:")

    # Pose/hand/controller: re-stamped to rgbTs if aligned -> nearest delta ~0
    print("Pose/hand/controller (re-stamped to RGB mid-exposure if aligned):")
    for name, fn in [("head_pose.csv", "head_pose.csv"),
                     ("hand_tracking.csv", "hand_tracking.csv"),
                     ("controller_poses.csv", "controller_poses.csv")]:
        ts = read_timestamps(os.path.join(d, fn))
        if not ts: continue
        tsorted = sorted(set(ts))
        deltas = [abs(m - tsorted[nearest_idx(tsorted, m)])/1e6 for m in rgb]
        summ(fn, deltas)

    # IMU: independent stream -> nearest-sample delta (post-align quality)
    print("IMU (independent stream; post-align residuals):")
    for name, fn in [("accel.csv", "accel.csv"), ("gyro.csv", "gyro.csv")]:
        ts = sorted(set(read_timestamps(os.path.join(d, fn))))
        if not ts: continue
        deltas = [abs(m - ts[nearest_idx(ts, m)])/1e6 for m in rgb]
        summ(fn, deltas)

    # Cross-camera sync: tracking/ctrl mid_exposure vs nearest RGB mid_exposure
    print()
    print("Cross-camera sync (each cam frame vs nearest RGB mid_exposure):")
    for name, fn in [("tracking", "tracking_metainfo.csv"), ("ctrl", "ctrl_metainfo.csv")]:
        mids = read_metainfo_mid(os.path.join(d, fn))
        if not mids: continue
        deltas = [(m - rgb_sorted[nearest_idx(rgb_sorted, m)])/1e6 for m in mids]
        summ(name, deltas)

    print()
    print("Reading: exact-match near 100% for head_pose/hand/controller => they are")
    print("re-stamped to RGB mid-exposure (aligned). IMU deltas are post-align residuals.")

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")
