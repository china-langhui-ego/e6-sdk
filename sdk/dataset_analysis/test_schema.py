#!/usr/bin/env python3
"""
test_schema.py — Focus 4 (doc-code consistency): dataset file schemas match
the documented contract.

SPEC: docs/unit-test-plan.md Focus 4 + CLAUDE.md "Dataset Output Structure".
The CSV headers and camera_params_*.json shape are the contract between the
native recorders and every downstream consumer. If a saver changes a header
or JSON key shape, this fails (and flags that CLAUDE.md / docs are now stale).

The golden headers below mirror CLAUDE.md; keep them in sync if the doc changes.

Checks:
  * CORE CSVs (accel/gyro/head_pose) MUST be present with the exact header.
  * At least one camera_params_*.json MUST be present with the documented shape.
  * ERA-DEPENDENT files (hand_tracking / controller_poses / *_metainfo.csv /
    imu_calibration.json) are validated IF present; absence is only noted,
    because old recordings predate those features (e.g. the bundled 20260613
    fixture predates the dual-track hand+controller CSVs from da7a2f8).

USAGE:
  python3 test_schema.py                 # bundled fixture
  python3 test_schema.py <dataset_dir>

Exit code: 0 = pass, 1 = fail.
"""
import sys
import os
import csv
import json
import glob

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURE = os.path.join(HERE, "..", "app", "src", "main", "cpp", "dataset", "20260701_202804")

# --- Golden headers (mirror CLAUDE.md "Dataset Output Structure") ---
CSV_HEADER = {
    "accel.csv":     "timestamp_ns,x,y,z",
    "gyro.csv":      "timestamp_ns,x,y,z",
    "head_pose.csv": "timestamp_ns,pos_x,pos_y,pos_z,quat_x,quat_y,quat_z,quat_w",
    "controller_poses.csv":
        "frame_number,timestamp_ns,left_active,left_px,left_py,left_pz,left_qx,left_qy,left_qz,left_qw,"
        "right_active,right_px,right_py,right_pz,right_qx,right_qy,right_qz,right_qw",
}
# hand_tracking.csv is 4 fixed cols + 26*2 generated joint blocks; lock the prefix.
HAND_PREFIX = "frame_number,timestamp,left_active,right_active"
VIDEO_METAINFO_HEADER = "frame_index,frame_id,pts_us,exposure_start_utc_ns,exposure_duration_ns,gain,mid_exposure_utc_ns"
AUDIO_METAINFO_HEADER = "packet_index,pts_us,capture_utc_ns"

CORE_CSV = ["accel.csv", "gyro.csv", "head_pose.csv"]


def find(root, name):
    hits = glob.glob(os.path.join(root, "**", name), recursive=True)
    return hits[0] if hits else None


def read_header(path):
    with open(path) as f:
        return f.readline().strip()


def main(argv):
    root = argv[1] if len(argv) > 1 else FIXTURE
    if not os.path.isdir(root):
        print(f"FAIL: dataset dir not found: {root}")
        return 1
    print(f"Dataset: {root}")

    fails = []
    notes = []

    # --- CORE CSV headers (required in every era) ---
    for name in CORE_CSV:
        p = find(root, name)
        if not p:
            fails.append(f"{name}: MISSING (core CSV must always be present)")
            continue
        got = read_header(p)
        exp = CSV_HEADER[name]
        if got != exp:
            fails.append(f"{name}: header mismatch\n     got: {got}\n     want: {exp}")
        else:
            print(f"  PASS {name}: header OK")

    # --- camera_params_*.json (required: at least one) ---
    cparams = glob.glob(os.path.join(root, "**", "camera_params_*.json"), recursive=True)
    if not cparams:
        fails.append("camera_params_*.json: MISSING (at least one camera-params file required)")
    for p in cparams:
        group = os.path.basename(p)
        try:
            doc = json.load(open(p))
        except Exception as e:
            fails.append(f"{group}: invalid JSON ({e})")
            continue
        cams = doc.get("cameras")
        if not isinstance(cams, list) or len(cams) != 2:
            fails.append(f"{group}: 'cameras' must be a 2-element array")
            continue
        ok = True
        for c in cams:
            if c.get("eye") not in ("left", "right"):
                fails.append(f"{group}: eye must be left/right"); ok = False
            intr = c.get("intrinsics", {})
            for k in ("focalX", "focalY", "centerX", "centerY"):
                if k not in intr:
                    fails.append(f"{group}/{c.get('eye')}: intrinsics missing {k}"); ok = False
            rd = intr.get("radialDistortion")
            if not (isinstance(rd, list) and len(rd) == 8):
                fails.append(f"{group}/{c.get('eye')}: radialDistortion must be len 8"); ok = False
            ext = c.get("extrinsics", {})
            if not (isinstance(ext.get("position"), list) and len(ext["position"]) == 3):
                fails.append(f"{group}/{c.get('eye')}: extrinsics.position must be len 3"); ok = False
            if not (isinstance(ext.get("rotation"), list) and len(ext["rotation"]) == 4):
                fails.append(f"{group}/{c.get('eye')}: extrinsics.rotation must be len 4"); ok = False
        if ok:
            print(f"  PASS {group}: schema OK (2 cams, intrinsics, extrinsics)")

    # --- ERA-DEPENDENT files: validate header if present, else note ---
    def check_opt_csv(name, expected):
        p = find(root, name)
        if not p:
            notes.append(f"{name}: absent (era-dependent; old recording)")
            return
        got = read_header(p)
        if got != expected:
            fails.append(f"{name}: header mismatch\n     got: {got}\n     want: {expected}")
        else:
            print(f"  PASS {name}: header OK")

    # hand_tracking: lock the fixed prefix (rest is a generated per-joint loop).
    hp = find(root, "hand_tracking.csv")
    if not hp:
        notes.append("hand_tracking.csv: absent (predates dual-track da7a2f8 on this fixture)")
    else:
        got = read_header(p) if (p := hp) else ""
        if not got.startswith(HAND_PREFIX):
            fails.append(f"hand_tracking.csv: header prefix mismatch\n     got: {got[:60]}\n     want prefix: {HAND_PREFIX}")
        else:
            print(f"  PASS hand_tracking.csv: prefix OK")

    check_opt_csv("controller_poses.csv", CSV_HEADER["controller_poses.csv"])

    # metainfo CSVs: rgb/tracking/ctrl share the video header; audio has its own.
    for stem in ("rgb", "tracking", "ctrl"):
        check_opt_csv(f"{stem}_metainfo.csv", VIDEO_METAINFO_HEADER)
    check_opt_csv("audio_metainfo.csv", AUDIO_METAINFO_HEADER)

    ic = find(root, "imu_calibration.json")
    if not ic:
        notes.append("imu_calibration.json: absent (era-dependent)")
    else:
        try:
            d = json.load(open(ic))
            if "imu" not in d or "bias" not in d.get("imu", {}):
                fails.append("imu_calibration.json: missing imu.bias")
            else:
                print("  PASS imu_calibration.json: has imu.bias")
        except Exception as e:
            fails.append(f"imu_calibration.json: invalid JSON ({e})")

    for n in notes:
        print(f"  note: {n}")

    if fails:
        print(f"\nFAILED ({len(fails)}):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("\nPASS: present dataset files match the documented schema (CLAUDE.md).")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
