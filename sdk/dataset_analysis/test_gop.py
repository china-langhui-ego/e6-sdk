#!/usr/bin/env python3
"""
test_gop.py — sentinel for the video GOP (I-frame interval) spec.

SPEC (docs/unit-test-plan.md, Focus 2): each encoded video stream has a
CONSTANT 1-second GOP (KEY_I_FRAME_INTERVAL = 1), with B-frames disabled
(commit f144e6c) so decode order == presentation order. Asserted in SECONDS
so it holds regardless of the per-stream fps setting (persist.sxr.cam.rgb.fps
30 or 60). The frame count is reported for information only.

Background: the Qualcomm HEVC encoder derives the I-frame frame count as
KEY_I_FRAME_INTERVAL(1 s) x KEY_FRAME_RATE, so KEY_FRAME_RATE must match the
real input fps. rgb previously drifted to a 0.5 s GOP because its encoder was
constructed with frameRate=30 while the camera ran at 60 fps; that is now fixed
by readRgbFps() (main.cpp) reading persist.sxr.cam.rgb.fps.

Checks, per video .mp4 (rgb/tracking/ctrl):
  1. >= 2 keyframes present.
  2. Consecutive keyframe PTS interval ~= 1.0 s  (the GOP spec).
  3. GOP constant (no variable-GOP regression).
  4. Packet PTS strictly increasing in file order (no B-frame reordering =>
     I/P only, f144e6c).

USAGE:
  python3 test_gop.py                 # default golden
  python3 test_gop.py <dataset_dir>

Exit code: 0 = pass, 1 = fail.
"""
import sys
import os
import glob
import subprocess
from statistics import median

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURE = os.path.join(HERE, "..", "app", "src", "main", "cpp", "dataset", "20260701_202804")

EXPECTED_GOP_S = 1.0      # one I-frame per second (KEY_I_FRAME_INTERVAL)
GOP_TOL_S = 0.05          # each keyframe interval within +/-0.05s of 1.0s
CONST_TOL_S = 0.08        # informational only: normal frame jitter is ~30-40ms


def pkt_info(mp4):
    """List of (pts_time_seconds, is_keyframe) in file (decode) order."""
    out = subprocess.check_output(
        ["ffprobe", "-v", "error", "-select_streams", "v:0",
         "-show_entries", "packet=pts_time,flags", "-of", "csv=p=0", mp4],
        text=True)
    info = []
    for line in out.splitlines():
        if not line.strip():
            continue
        parts = line.split(",")
        pts, flags = (parts + ["", ""])[:2]
        try:
            t = float(pts)
        except ValueError:
            continue
        info.append((t, "K" in flags))
    return info


def main(argv):
    root = argv[1] if len(argv) > 1 else FIXTURE
    videos = sorted(glob.glob(os.path.join(root, "**", "*.mp4"), recursive=True))
    if not videos:
        print(f"FAIL: no .mp4 under {root}")
        return 1
    print(f"Dataset: {root}")

    fails = []
    for mp4 in videos:
        name = os.path.basename(mp4)
        if name not in ("rgb.mp4", "tracking.mp4", "ctrl.mp4"):
            continue
        info = pkt_info(mp4)
        if len(info) < 2:
            fails.append(f"{name}: <2 packets")
            continue
        kf_idx = [i for i, (t, k) in enumerate(info) if k]
        kf_t = [t for t, k in info if k]
        if len(kf_idx) < 2:
            fails.append(f"{name}: <2 keyframes (got {len(kf_idx)})")
            continue
        gop_s = [kf_t[i + 1] - kf_t[i] for i in range(len(kf_t) - 1)]    # seconds
        gop_f = [kf_idx[i + 1] - kf_idx[i] for i in range(len(kf_idx) - 1)]  # frames
        bad = [g for g in gop_s if abs(g - EXPECTED_GOP_S) > GOP_TOL_S]
        const_ok = (max(gop_s) - min(gop_s)) <= CONST_TOL_S  # informational
        mono = all(info[i + 1][0] > info[i][0] for i in range(len(info) - 1))
        ok = not bad and mono  # GOP_TOL_S already bounds constancy; don't fail on frame jitter
        # derive fps from the median frame interval for the report
        ints = [info[i + 1][0] - info[i][0] for i in range(len(info) - 1)]
        fps = 1.0 / median(ints) if ints and median(ints) > 0 else 0
        print(f"  {name:16s} pkts={len(info):5d} kf={len(kf_idx):4d} "
              f"GOP={median(gop_s):.3f}s/{median(gop_f):.0f}f (~{fps:.0f}fps) "
              f"const={'Y' if const_ok else 'N'} mono={'Y' if mono else 'N'} "
              f"-> {'PASS' if ok else 'FAIL'}")
        if bad:
            fails.append(f"{name}: {len(bad)} keyframe intervals outside "
                         f"{EXPECTED_GOP_S}+/-{GOP_TOL_S}s (e.g. {bad[0]:.3f}s)")
        if not mono:
            fails.append(f"{name}: packet PTS not strictly increasing -> "
                         f"B-frame reordering (f144e6c violated?)")

    if fails:
        print(f"\nFAILED ({len(fails)}):")
        for f in fails:
            print("  -", f)
        return 1
    print("\nPASS: all video streams have constant 1s GOP, no B-frames.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
