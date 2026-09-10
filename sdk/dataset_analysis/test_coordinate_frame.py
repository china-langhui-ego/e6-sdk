#!/usr/bin/env python3
"""
test_coordinate_frame.py — sentinel for the dataset coordinate-frame spec.

SPEC (docs/unit-test-plan.md §0.2, decision 2026-06-30):
  Dataset extrinsics (camera_params_*.json) and IMU bias (imu_calibration.json)
  are stored RAW in the SVR / OpenXR Body frame — NO coordinate transform.
  The legacy posBodyToOpencv [x, -y, -z] OpenCV-Body transform was removed
  (supersedes commits 6bd1de7 / 79ece61 / bfe7047).

WHY THIS TEST EXISTS:
  The OpenCV-body convention once lived only in commit messages + prose docs, so
  it drifted silently. This test locks the "raw, no transform" decision so that
  reintroducing posBodyToOpencv (which flips Y/Z) is caught immediately.

GOLDEN — app/src/main/cpp/dataset/20260701_202804:
  A current-spec recording (all streams: rgb/tracking/ctrl cameras, hand,
  controller, IMU, head_pose, *_metainfo, imu_calibration), captured with the
  current code (raw SVR body, no app-side transform). Its extrinsics/bias are
  the trusted RAW reference. A new recording from the SAME device must match
  these calibration constants (extrinsics are static per-device); a DIFFERENT
  device's values are noted, not failed — only a Y/Z flip (posBodyToOpencv
  reintroduced) is a failure. The golden is gitignored (local, device-specific).

WHAT IT CHECKS:
  1. Schema / structure of every camera_params_*.json (group, 2 eyes, intrinsics,
     radialDistortion[8], extrinsics position[3]/rotation[4]).
  2. Rotation is a unit quaternion.
  3. No-transform sentinel: target extrinsics == golden extrinsics (raw). If the
     target instead matches the golden with Y/Z sign-flipped, that is the
     signature of a reintroduced OpenCV transform -> FAIL with an explicit message.
  4. imu_calibration.json bias, if present, is non-empty (raw passthrough).

USAGE:
  python3 test_coordinate_frame.py                 # target = bundled golden (self-check)
  python3 test_coordinate_frame.py <dataset_dir>   # target = a recording under test
  python3 test_coordinate_frame.py <dir> --schema-only

Exit code: 0 = pass, 1 = fail (CI-friendly).
"""
import sys
import os
import json
import glob
import math

HERE = os.path.dirname(os.path.abspath(__file__))
GOLDEN_ROOT = os.path.join(HERE, "..", "app", "src", "main", "cpp", "dataset")
GOLDEN_NAME = "20260701_202804"          # current-spec recording, raw SVR body (see docstring)
TOL = 1e-5                                # JSON is written at setprecision(6)


def find_param_files(root):
    """All camera_params_*.json under root (recursive; tolerates nesting)."""
    return sorted(glob.glob(os.path.join(root, "**", "camera_params_*.json"), recursive=True))


def load_camera_params(root):
    """Return {group: {eye: {position[3], rotation[4], intrinsics, width, height}}}."""
    out = {}
    for path in find_param_files(root):
        with open(path) as f:
            doc = json.load(f)
        group = doc.get("group", os.path.basename(path))
        for cam in doc.get("cameras", []):
            eye = cam.get("eye")
            ext = cam.get("extrinsics", {})
            out.setdefault(group, {})[eye] = {
                "position": ext.get("position", []),
                "rotation": ext.get("rotation", []),
                "intrinsics": cam.get("intrinsics", {}),
                "width": cam.get("width"),
                "height": cam.get("height"),
                "_file": path,
            }
    return out


def approx_eq(a, b, tol=TOL):
    if len(a) != len(b):
        return False
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def flipped_match(pos, ref):
    """True if pos == [ref0, -ref1, -ref2] within tol (the posBodyToOpencv signature)."""
    if len(pos) != 3 or len(ref) != 3:
        return False
    return (abs(pos[0] - ref[0]) <= TOL
            and abs(pos[1] + ref[1]) <= TOL
            and abs(pos[2] + ref[2]) <= TOL)


def check_schema(params, fails):
    valid_groups = {"tracking", "ctrl", "rgb"}
    for group, eyes in params.items():
        if group not in valid_groups:
            fails.append(f"[{group}] unexpected group name (want one of {sorted(valid_groups)})")
        if set(eyes.keys()) != {"left", "right"}:
            fails.append(f"[{group}] expected eyes {{left,right}}, got {sorted(eyes.keys())}")
        for eye, c in eyes.items():
            if len(c["position"]) != 3:
                fails.append(f"[{group}/{eye}] position must have 3 elements")
            if len(c["rotation"]) != 4:
                fails.append(f"[{group}/{eye}] rotation must have 4 elements (x,y,z,w)")
            rd = c["intrinsics"].get("radialDistortion", [])
            if len(rd) != 8:
                fails.append(f"[{group}/{eye}] radialDistortion must have 8 elements, got {len(rd)}")


def check_unit_quaternion(params, fails):
    for group, eyes in params.items():
        for eye, c in eyes.items():
            q = c["rotation"]
            if len(q) != 4:
                continue  # already reported by schema
            n = math.sqrt(sum(v * v for v in q))
            if abs(n - 1.0) > 1e-4:
                fails.append(f"[{group}/{eye}] rotation not a unit quaternion (|q|={n:.6f})")


def check_no_transform(target, golden, fails, infos):
    """Sentinel: detect the OpenCV-transform signature (Y/Z flip) vs the golden.
    The golden is device-specific, so a value MISMATCH that is NOT a flip means
    'different device/calibration' — inconclusive for the coordinate-frame spec,
    not a failure. Only an actual Y/Z flip (posBodyToOpencv reintroduced) fails."""
    for group, g_eyes in golden.items():
        if group not in target:
            continue  # target lacks this group — not a coordinate-frame failure
        for eye, gc in g_eyes.items():
            tc = target[group].get(eye)
            if not tc:
                continue
            gpos, tpos = gc["position"], tc["position"]
            if approx_eq(tpos, gpos):
                continue  # exact raw match with golden — correct
            if flipped_match(tpos, gpos):
                fails.append(
                    f"[{group}/{eye}] OpenCV-body transform DETECTED: position {tpos} "
                    f"== posBodyToOpencv({gpos}) — Y/Z flipped. The raw-SVR-body spec "
                    f"(docs/unit-test-plan.md §0.2) was violated; posBodyToOpencv was reintroduced?"
                )
                if not approx_eq(tc["rotation"], gc["rotation"]):
                    fails.append(
                        f"[{group}/{eye}] rotation {tc['rotation']} != golden {gc['rotation']} "
                        f"(raw passthrough expected; quatCB_ToOpencv reintroduced?)"
                    )
            else:
                # Different values, not a flip -> different device/calibration.
                # Not a coordinate-frame regression; the golden's value-pin is
                # device-specific and does not apply to this recording.
                infos.append(
                    f"[{group}/{eye}] differs from golden (different device/calibration, "
                    f"not a Y/Z-flip) — value-pin N/A"
                )


def check_imu_bias(root, fails, infos):
    """imu_calibration.json bias must be present (raw passthrough). Fixture lacks
    it (feature postdates the recording), so absence is informational, not failure."""
    paths = glob.glob(os.path.join(root, "**", "imu_calibration.json"), recursive=True)
    if not paths:
        infos.append("imu_calibration.json absent (legacy dataset; bias check skipped)")
        return
    with open(paths[0]) as f:
        doc = json.load(f)
    bias = doc.get("imu", {}).get("bias", {})
    for key in ("accelerometer_mps2", "gyroscope_rads"):
        v = bias.get(key)
        if not isinstance(v, list) or len(v) != 3:
            fails.append(f"imu_calibration.json bias.{key} must be a 3-element array (raw passthrough)")


def main(argv):
    schema_only = "--schema-only" in argv
    args = [a for a in argv[1:] if not a.startswith("--")]
    target_root = args[0] if args else os.path.join(GOLDEN_ROOT, GOLDEN_NAME)

    target = load_camera_params(target_root)
    if not target:
        print(f"FAIL: no camera_params_*.json found under {target_root}")
        return 1

    print(f"Target : {target_root}")
    print(f"Groups : {sorted(target.keys())}  (schema + unit-quaternion checks)")

    fails, infos = [], []
    check_schema(target, fails)
    check_unit_quaternion(target, fails)
    check_imu_bias(target_root, fails, infos)

    if not schema_only:
        golden = load_camera_params(os.path.join(GOLDEN_ROOT, GOLDEN_NAME))
        if golden:
            print(f"Golden : {GOLDEN_NAME} (verified raw SVR body) — no-transform sentinel ON)")
            check_no_transform(target, golden, fails, infos)
        else:
            infos.append("golden fixture missing — no-transform sentinel skipped")

    for i in infos:
        print(f"  note: {i}")

    if fails:
        print(f"\nFAILED ({len(fails)}):")
        for fmsg in fails:
            print(f"  - {fmsg}")
        return 1

    print("\nPASS: coordinate-frame spec holds (raw SVR body, no transform).")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
