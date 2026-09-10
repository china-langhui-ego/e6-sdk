#!/usr/bin/env python3
"""
数据集时间戳对齐分析工具
分析 100 次录制中所有摄像头的时间戳对齐情况，检查 crash 等异常

用法:
    python3 analyze_all_timestamps.py                    # 分析所有数据集
    python3 analyze_all_timestamps.py --dir ../dataset   # 指定数据集目录
    python3 analyze_all_timestamps.py --pull             # 先从设备拉取数据
"""

import argparse
import csv
import json
import os
import re
import subprocess
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec
import numpy as np

# 修复中文字体显示问题
import matplotlib.font_manager as fm

def setup_chinese_font():
    chinese_fonts = [
        {'path': '/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc', 'name': 'WenQuanYi Zen Hei'},
        {'path': '/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf', 'name': 'Droid Sans Fallback'},
        {'path': '/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc', 'name': 'Noto Sans CJK JP'},
    ]
    for font_info in chinese_fonts:
        if os.path.exists(font_info['path']):
            fm.fontManager.addfont(font_info['path'])
            plt.rcParams['font.family'] = 'sans-serif'
            plt.rcParams['font.sans-serif'] = [font_info['name']] + plt.rcParams.get('font.sans-serif', [])
            plt.rcParams['axes.unicode_minus'] = False
            return
    print("警告: 未找到中文字体")

setup_chinese_font()


# ============================================================
# 数据结构
# ============================================================

@dataclass
class StreamInfo:
    name: str
    timestamps: List[int] = field(default_factory=list)  # boottime (ns or us)
    timestamp_unit: str = "unknown"  # "ns" or "us"
    frame_count: int = 0
    expected_fps: float = 0.0

    @property
    def duration_ms(self):
        if len(self.timestamps) < 2:
            return 0
        unit_factor = 1e6 if self.timestamp_unit == "ns" else 1e3
        return (self.timestamps[-1] - self.timestamps[0]) / unit_factor

    @property
    def intervals_ms(self):
        if len(self.timestamps) < 2:
            return []
        unit_factor = 1e6 if self.timestamp_unit == "ns" else 1e3
        return [(self.timestamps[i+1] - self.timestamps[i]) / unit_factor
                for i in range(len(self.timestamps) - 1)]

    @property
    def avg_fps(self):
        dur = self.duration_ms
        if dur <= 0 or self.frame_count <= 1:
            return 0
        return (self.frame_count - 1) / (dur / 1000.0)

    @property
    def has_data(self):
        return len(self.timestamps) > 0


@dataclass
class DatasetResult:
    name: str
    path: str
    streams: Dict[str, StreamInfo] = field(default_factory=dict)
    issues: List[str] = field(default_factory=list)
    files_found: List[str] = field(default_factory=list)
    files_missing: List[str] = field(default_factory=list)
    is_valid: bool = True


# ============================================================
# 时间戳提取
# ============================================================

def extract_metainfo_timestamps(dataset_dir: str, stream: str) -> List[int]:
    """从 *_metainfo.csv 读取每个样本的 UTC 时间戳 (ns)

    video 流 (rgb/tracking/ctrl) 取 mid_exposure_utc_ns 列 (索引 6);
    audio 流取 capture_utc_ns 列 (索引 2)。

    若 CSV 不存在 (旧版 mett 数据集), 返回空列表。
    """
    csv_name = f'{stream}_metainfo.csv'
    csv_path = os.path.join(dataset_dir, csv_name)
    if not os.path.exists(csv_path):
        return []

    ts_col = 'capture_utc_ns' if stream == 'audio' else 'mid_exposure_utc_ns'
    timestamps = []
    try:
        with open(csv_path, 'r') as f:
            reader = csv.DictReader(f)
            for row in reader:
                try:
                    val = row.get(ts_col, '')
                    if val:
                        timestamps.append(int(val))
                except (ValueError, TypeError):
                    pass
    except Exception:
        return []
    return timestamps


def count_metainfo_rows(dataset_dir: str, stream: str) -> int:
    """统计 *_metainfo.csv 的数据行数 (不含 header)。不存在返回 -1。"""
    csv_path = os.path.join(dataset_dir, f'{stream}_metainfo.csv')
    if not os.path.exists(csv_path):
        return -1
    try:
        with open(csv_path, 'r') as f:
            return max(0, sum(1 for _ in f) - 1)  # 减去 header
    except Exception:
        return -1


def detect_timestamp_unit(timestamps: List[int]) -> str:
    """检测时间戳单位 (ns 还是 us) - 基于帧间隔判断"""
    if not timestamps or len(timestamps) < 2:
        return "unknown"
    avg_interval = (timestamps[-1] - timestamps[0]) / (len(timestamps) - 1)
    # 如果当作 ns: 帧间隔应该是 16ms(60fps)~33ms(30fps) → 16000000~33000000
    # 如果当作 us: 帧间隔应该是 16ms(60fps)~33ms(30fps) → 16000~33000
    if 5000000 < avg_interval < 200000000:  # 合理的 ns 帧间隔范围
        return "ns"
    elif 5000 < avg_interval < 200000:  # 合理的 us 帧间隔范围
        return "us"
    elif avg_interval > 1e9:  # > 1s in ns, 可能是低频 IMU
        return "ns"
    else:
        return "ns"  # 默认 ns


def get_video_frame_count(mp4_path: str) -> int:
    """获取视频帧数 (fragmented MP4 的 nb_frames 常为 N/A, 故用 count_packets)"""
    try:
        result = subprocess.run(
            ['ffprobe', '-v', 'quiet', '-select_streams', 'v',
             '-count_packets',
             '-show_entries', 'stream=nb_read_packets',
             '-of', 'csv=p=0', mp4_path],
            capture_output=True, text=True, timeout=30)
        out = result.stdout.strip()
        if out and out.isdigit():
            return int(out)
        # 降级到 nb_frames (非 fragmented 容器)
        result = subprocess.run(
            ['ffprobe', '-v', 'quiet', '-select_streams', 'v',
             '-show_entries', 'stream=nb_frames',
             '-of', 'csv=p=0', mp4_path],
            capture_output=True, text=True, timeout=30)
        out = result.stdout.strip()
        return int(out) if out and out.isdigit() else 0
    except Exception:
        return 0


def get_audio_packet_count(m4a_path: str) -> int:
    """获取音频 packet 数"""
    try:
        result = subprocess.run(
            ['ffprobe', '-v', 'quiet', '-select_streams', 'a',
             '-count_packets',
             '-show_entries', 'stream=nb_read_packets',
             '-of', 'csv=p=0', m4a_path],
            capture_output=True, text=True, timeout=30)
        out = result.stdout.strip()
        if out and out.isdigit():
            return int(out)
        return 0
    except Exception:
        return 0


def get_video_resolution(mp4_path: str) -> Tuple[int, int]:
    """获取视频分辨率"""
    try:
        result = subprocess.run(
            ['ffprobe', '-v', 'quiet', '-select_streams', 'v',
             '-show_entries', 'stream=width,height',
             '-of', 'csv=p=0', mp4_path],
            capture_output=True, text=True, timeout=30)
        parts = result.stdout.strip().split(',')
        if len(parts) == 2:
            return int(parts[0]), int(parts[1])
    except Exception:
        pass
    return 0, 0


def extract_csv_timestamps(csv_path: str, ts_col: str = "timestamp") -> List[int]:
    """从 CSV 文件提取时间戳列"""
    timestamps = []
    try:
        with open(csv_path, 'r') as f:
            reader = csv.DictReader(f)
            for row in reader:
                try:
                    ts_str = row.get(ts_col, row.get("timestamp_ns", ""))
                    if ts_str:
                        timestamps.append(int(ts_str))
                except (ValueError, KeyError):
                    pass
    except Exception:
        pass
    return timestamps


# ============================================================
# 数据集分析
# ============================================================

def analyze_dataset(dataset_dir: str) -> DatasetResult:
    """分析单个数据集目录"""
    name = os.path.basename(dataset_dir)
    result = DatasetResult(name=name, path=dataset_dir)

    if not os.path.isdir(dataset_dir):
        result.is_valid = False
        result.issues.append("目录不存在")
        return result

    files = os.listdir(dataset_dir)
    result.files_found = files

    # 期望的文件
    expected_files = {
        'rgb.mp4': 'RGB 视频 (SBS, H.265)',
        'tracking.mp4': 'Tracking 灰度视频',
        'ctrl.mp4': 'Ctrl 灰度视频',
        'head_pose.csv': '头部姿态数据',
        'hand_tracking.csv': '手势关节数据',
        'audio.m4a': '音频录制',
    }
    # 可选文件
    optional_files = {
        'accel.csv': '加速度计数据',
        'gyro.csv': '陀螺仪数据',
        'controller_poses.csv': '手柄姿态数据',
        'imu_pose.csv': '旧版 IMU 数据',
        'camera_params_rgb.json': 'RGB 相机参数',
        'camera_params_tracking.json': 'Tracking 相机参数',
        'camera_params_ctrl.json': 'Ctrl 相机参数',
    }

    for f, desc in expected_files.items():
        if f not in files:
            result.files_missing.append(f"{f} ({desc})")

    # ---- 分析 MP4 文件 ----
    mp4_files = {
        'rgb': ('rgb.mp4', 30),
        'tracking': ('tracking.mp4', 60),
        'ctrl': ('ctrl.mp4', 60),
    }

    for stream_name, (filename, expected_fps) in mp4_files.items():
        mp4_path = os.path.join(dataset_dir, filename)
        if not os.path.exists(mp4_path):
            continue

        stream = StreamInfo(name=stream_name, expected_fps=expected_fps)

        # 检查文件大小
        file_size = os.path.getsize(mp4_path)
        if file_size < 1024:
            result.issues.append(f"{filename}: 文件过小 ({file_size} bytes)")
            result.streams[stream_name] = stream
            continue

        # 获取视频 packet 数 (fragmented MP4 用 nb_read_packets)
        stream.frame_count = get_video_frame_count(mp4_path)

        # 检查 *_metainfo.csv 是否存在
        metainfo_rows = count_metainfo_rows(dataset_dir, stream_name)
        if metainfo_rows < 0:
            # 旧版数据集 (mett 时代): 没有 metainfo.csv, 跳过本流的时间戳分析
            result.issues.append(
                f"{stream_name}_metainfo.csv: 缺失 (旧版 mett 数据集, 无 metainfo)")
            result.streams[stream_name] = stream
            continue

        # 从 metainfo.csv 提取时间戳 (mid_exposure_utc_ns, 已是 UTC ns)
        timestamps = extract_metainfo_timestamps(dataset_dir, stream_name)
        if timestamps:
            stream.timestamps = timestamps
            stream.timestamp_unit = "ns"
        else:
            result.issues.append(f"{stream_name}_metainfo.csv: 无有效时间戳")

        # 检查 metainfo 行数与视频 packet 数是否一致
        video_packets = stream.frame_count
        if metainfo_rows > 0 and video_packets > 0:
            if abs(metainfo_rows - video_packets) > max(2, video_packets * 0.01):
                result.issues.append(
                    f"{filename}: metainfo 行 ({metainfo_rows}) ≠ 视频packet ({video_packets})")

        result.streams[stream_name] = stream

    # ---- 分析 CSV 文件 ----
    csv_files = {
        'head_pose': ('head_pose.csv', ['timestamp_ns', 'timestamp'], 30),
        'hand_tracking': ('hand_tracking.csv', ['timestamp', 'timestamp_ns'], 30),
        'accel': ('accel.csv', ['timestamp_ns', 'timestamp'], 2000),
        'gyro': ('gyro.csv', ['timestamp_ns', 'timestamp'], 2000),
        'imu_pose': ('imu_pose.csv', ['timestamp_ns', 'timestamp'], 2000),
    }

    for stream_name, (filename, ts_cols, expected_fps) in csv_files.items():
        csv_path = os.path.join(dataset_dir, filename)
        if not os.path.exists(csv_path):
            continue

        stream = StreamInfo(name=stream_name, expected_fps=expected_fps)

        timestamps = []
        for col in ts_cols:
            timestamps = extract_csv_timestamps(csv_path, col)
            if timestamps:
                break
        if timestamps:
            stream.timestamps = timestamps
            stream.frame_count = len(timestamps)
            stream.timestamp_unit = detect_timestamp_unit(timestamps)

        # 检查文件是否为空或只有header
        file_size = os.path.getsize(csv_path)
        if file_size < 50:
            result.issues.append(f"{filename}: 文件过小 ({file_size} bytes)")

        result.streams[stream_name] = stream

    # ---- 分析 camera_params JSON ----
    for json_name in ['camera_params_rgb.json', 'camera_params_tracking.json', 'camera_params_ctrl.json']:
        json_path = os.path.join(dataset_dir, json_name)
        if os.path.exists(json_path):
            try:
                with open(json_path) as f:
                    params = json.load(f)
                if 'cameras' not in params:
                    result.issues.append(f"{json_name}: 缺少 cameras 字段")
            except json.JSONDecodeError:
                result.issues.append(f"{json_name}: JSON 解析失败")

    # ---- 检查音频文件 ----
    audio_path = os.path.join(dataset_dir, 'audio.m4a')
    if os.path.exists(audio_path):
        audio_size = os.path.getsize(audio_path)
        if audio_size < 1024:
            result.issues.append(f"audio.m4a: 文件过小 ({audio_size} bytes)")

        # 从 audio_metainfo.csv 提取时间戳并校验行数与 packet 数
        audio_stream = StreamInfo(name='audio', expected_fps=0)
        audio_stream.frame_count = get_audio_packet_count(audio_path)
        audio_rows = count_metainfo_rows(dataset_dir, 'audio')
        if audio_rows < 0:
            result.issues.append(
                "audio_metainfo.csv: 缺失 (旧版 mett 数据集, 无 metainfo)")
        else:
            audio_ts = extract_metainfo_timestamps(dataset_dir, 'audio')
            if audio_ts:
                audio_stream.timestamps = audio_ts
                audio_stream.timestamp_unit = "ns"
            # 校验 audio_metainfo 行数与 packet 数
            if audio_rows > 0 and audio_stream.frame_count > 0:
                if abs(audio_rows - audio_stream.frame_count) > max(2, audio_stream.frame_count * 0.01):
                    result.issues.append(
                        f"audio.m4a: metainfo 行 ({audio_rows}) ≠ 音频packet ({audio_stream.frame_count})")
        result.streams['audio'] = audio_stream

    # ---- 时间戳对齐检查 ----
    check_timestamp_alignment(result)

    return result


def check_timestamp_alignment(result: DatasetResult):
    """检查各数据流之间的时间戳对齐"""
    streams = result.streams

    # 检查帧率一致性
    for name, stream in streams.items():
        if not stream.has_data or stream.frame_count < 2:
            continue

        intervals = stream.intervals_ms
        if not intervals:
            continue

        avg_interval = np.mean(intervals)
        expected_interval = 1000.0 / stream.expected_fps if stream.expected_fps > 0 else 0

        # 帧率偏差超过 20%
        if expected_interval > 0 and abs(avg_interval - expected_interval) / expected_interval > 0.2:
            actual_fps = stream.avg_fps
            result.issues.append(
                f"{name}: 帧率异常 (期望 {stream.expected_fps}fps, 实际 {actual_fps:.1f}fps)")

        # 帧间隔异常 (超过 2 倍平均间隔)
        max_gap = max(intervals)
        if max_gap > avg_interval * 2.5:
            gap_count = sum(1 for x in intervals if x > avg_interval * 2.5)
            result.issues.append(
                f"{name}: 检测到 {gap_count} 次帧间隔过大 (最大 {max_gap:.1f}ms, 平均 {avg_interval:.1f}ms)")

        # 帧间隔标准差 (抖动过大)
        std_interval = np.std(intervals)
        if std_interval > avg_interval * 0.3:
            result.issues.append(
                f"{name}: 帧间隔抖动大 (std={std_interval:.2f}ms, avg={avg_interval:.2f}ms)")

    # 检查摄像头之间的时间戳对齐
    camera_streams = ['rgb', 'tracking', 'ctrl']
    available_cameras = [s for s in camera_streams if s in streams and streams[s].has_data]

    if len(available_cameras) >= 2:
        # 归一化时间戳到同一单位 (ms)
        for i in range(len(available_cameras)):
            for j in range(i + 1, len(available_cameras)):
                s1 = streams[available_cameras[i]]
                s2 = streams[available_cameras[j]]

                # 统一时间戳单位
                def to_ms(ts, unit):
                    return ts / 1e6 if unit == "ns" else ts / 1e3

                # 检查时间范围重叠
                s1_start = to_ms(s1.timestamps[0], s1.timestamp_unit)
                s1_end = to_ms(s1.timestamps[-1], s1.timestamp_unit)
                s2_start = to_ms(s2.timestamps[0], s2.timestamp_unit)
                s2_end = to_ms(s2.timestamps[-1], s2.timestamp_unit)

                overlap_start = max(s1_start, s2_start)
                overlap_end = min(s1_end, s2_end)
                overlap_duration = overlap_end - overlap_start

                # 检查起始时间差异
                start_diff = abs(s1_start - s2_start)
                if start_diff > 500:  # 超过 500ms
                    result.issues.append(
                        f"{available_cameras[i]} vs {available_cameras[j]}: "
                        f"起始时间差异大 ({start_diff:.0f}ms)")

                # 检查结束时间差异
                end_diff = abs(s1_end - s2_end)
                if end_diff > 500:
                    result.issues.append(
                        f"{available_cameras[i]} vs {available_cameras[j]}: "
                        f"结束时间差异大 ({end_diff:.0f}ms)")

                # 检查持续时间差异
                s1_dur = s1_end - s1_start
                s2_dur = s2_end - s2_start
                if s1_dur > 0 and s2_dur > 0:
                    dur_ratio = abs(s1_dur - s2_dur) / max(s1_dur, s2_dur)
                    if dur_ratio > 0.1:
                        result.issues.append(
                            f"{available_cameras[i]} vs {available_cameras[j]}: "
                            f"持续时间差异 {dur_ratio*100:.1f}% "
                            f"({s1_dur:.0f}ms vs {s2_dur:.0f}ms)")

    # 检查 RGB 与 CSV 数据的时间戳对齐
    if 'rgb' in streams and streams['rgb'].has_data:
        rgb = streams['rgb']
        rgb_start = rgb.timestamps[0]
        rgb_end = rgb.timestamps[-1]

        for csv_name in ['head_pose', 'hand_tracking']:
            if csv_name in streams and streams[csv_name].has_data:
                csv_stream = streams[csv_name]

                # 统一单位后比较
                def normalize(ts_list, unit):
                    if unit == "ns":
                        return [t for t in ts_list]
                    return [t * 1000 for t in ts_list]  # us -> ns

                csv_ts = normalize(csv_stream.timestamps, csv_stream.timestamp_unit)
                rgb_ts = normalize(rgb.timestamps, rgb.timestamp_unit)

                if csv_ts and rgb_ts:
                    # 检查时间范围
                    rgb_range = rgb_ts[-1] - rgb_ts[0]
                    csv_range = csv_ts[-1] - csv_ts[0]
                    if rgb_range > 0:
                        ratio = abs(csv_range - rgb_range) / rgb_range
                        if ratio > 0.15:
                            result.issues.append(
                                f"rgb vs {csv_name}: 时间范围差异 {ratio*100:.1f}%")

                    # 检查帧数比例
                    if rgb.frame_count > 0 and csv_stream.frame_count > 0:
                        frame_ratio = csv_stream.frame_count / rgb.frame_count
                        if frame_ratio < 0.8 or frame_ratio > 1.2:
                            result.issues.append(
                                f"rgb vs {csv_name}: 帧数差异大 "
                                f"(rgb={rgb.frame_count}, {csv_name}={csv_stream.frame_count})")


# ============================================================
# 跨数据集统计分析
# ============================================================

def cross_dataset_analysis(results: List[DatasetResult], output_dir: str):
    """跨数据集统计分析"""
    os.makedirs(output_dir, exist_ok=True)

    valid_results = [r for r in results if r.is_valid]

    print("\n" + "=" * 80)
    print("  跨数据集统计分析")
    print("=" * 80)

    # 1. 成功率
    total = len(results)
    valid = len(valid_results)
    has_issues = sum(1 for r in valid_results if r.issues)
    print(f"\n总数据集: {total}, 有效: {valid}, 有异常: {has_issues}")
    print(f"成功率: {valid/total*100:.1f}%" if total > 0 else "N/A")

    # 2. 帧数统计
    print("\n--- 帧数统计 ---")
    stream_names = ['rgb', 'tracking', 'ctrl', 'head_pose', 'hand_tracking']
    for sname in stream_names:
        frame_counts = []
        for r in valid_results:
            if sname in r.streams and r.streams[sname].frame_count > 0:
                frame_counts.append(r.streams[sname].frame_count)
        if frame_counts:
            arr = np.array(frame_counts)
            print(f"  {sname:15s}: "
                  f"mean={np.mean(arr):.0f}, std={np.std(arr):.0f}, "
                  f"min={np.min(arr)}, max={np.max(arr)}, "
                  f"n={len(arr)}")

    # 3. 帧率统计
    print("\n--- 帧率统计 ---")
    for sname in stream_names:
        fps_list = []
        for r in valid_results:
            if sname in r.streams and r.streams[sname].avg_fps > 0:
                fps_list.append(r.streams[sname].avg_fps)
        if fps_list:
            arr = np.array(fps_list)
            expected = valid_results[0].streams[sname].expected_fps if sname in valid_results[0].streams else 0
            print(f"  {sname:15s}: "
                  f"mean={np.mean(arr):.1f}fps, std={np.std(arr):.1f}, "
                  f"expected={expected}fps, n={len(arr)}")

    # 4. 时间戳对齐统计
    print("\n--- 时间戳对齐统计 (摄像头间起始时间差) ---")
    alignment_diffs = defaultdict(list)
    for r in valid_results:
        cameras = [(name, s) for name, s in r.streams.items()
                   if name in ['rgb', 'tracking', 'ctrl'] and s.has_data]
        for i in range(len(cameras)):
            for j in range(i + 1, len(cameras)):
                n1, s1 = cameras[i]
                n2, s2 = cameras[j]
                def to_ms(ts, unit): return ts / 1e6 if unit == "ns" else ts / 1e3
                diff = abs(to_ms(s1.timestamps[0], s1.timestamp_unit) -
                           to_ms(s2.timestamps[0], s2.timestamp_unit))
                alignment_diffs[f"{n1}-{n2}"].append(diff)

    for pair, diffs in alignment_diffs.items():
        arr = np.array(diffs)
        print(f"  {pair:20s}: mean={np.mean(arr):.1f}ms, "
              f"max={np.max(arr):.1f}ms, std={np.std(arr):.1f}ms")

    # 5. 文件完整性统计
    print("\n--- 文件完整性 ---")
    file_presence = defaultdict(lambda: [0, 0])  # [found, total]
    for r in valid_results:
        all_expected = ['rgb.mp4', 'tracking.mp4', 'ctrl.mp4',
                        'head_pose.csv', 'hand_tracking.csv', 'audio.m4a',
                        'accel.csv', 'gyro.csv']
        for f in all_expected:
            file_presence[f][1] += 1
            if f in r.files_found:
                file_presence[f][0] += 1

    for f, (found, total) in sorted(file_presence.items()):
        pct = found / total * 100 if total > 0 else 0
        status = "OK" if pct >= 95 else "WARN" if pct >= 50 else "MISS"
        print(f"  {f:30s}: {found}/{total} ({pct:.0f}%) [{status}]")

    # 6. 异常分类
    print("\n--- 异常分类 ---")
    issue_categories = defaultdict(int)
    for r in valid_results:
        for issue in r.issues:
            # 归类
            if "帧率异常" in issue:
                issue_categories["帧率异常"] += 1
            elif "帧间隔过大" in issue:
                issue_categories["帧间隔过大(丢帧)"] += 1
            elif "抖动大" in issue:
                issue_categories["帧间隔抖动大"] += 1
            elif "起始时间差异" in issue:
                issue_categories["摄像头起始时间差异"] += 1
            elif "结束时间差异" in issue:
                issue_categories["摄像头结束时间差异"] += 1
            elif "持续时间差异" in issue:
                issue_categories["持续时间差异"] += 1
            elif "时间范围差异" in issue:
                issue_categories["CSV与视频时间范围差异"] += 1
            elif "帧数差异" in issue:
                issue_categories["CSV与视频帧数差异"] += 1
            elif "metainfo" in issue and "缺失" in issue:
                issue_categories["缺少metainfo(旧版mett)"] += 1
            elif "metainfo" in issue and ("行" in issue or "packet" in issue) and "≠" in issue:
                issue_categories["metainfo行数不匹配"] += 1
            elif "文件过小" in issue:
                issue_categories["文件过小(可能crash)"] += 1
            elif "JSON" in issue:
                issue_categories["相机参数JSON异常"] += 1
            else:
                issue_categories[f"其他: {issue[:30]}"] += 1

    if issue_categories:
        for cat, count in sorted(issue_categories.items(), key=lambda x: -x[1]):
            print(f"  {cat:35s}: {count} 次")
    else:
        print("  无异常")

    # 7. Crash / 严重异常检测
    print("\n--- Crash / 严重异常检测 ---")
    crash_indicators = []
    for r in results:
        # 文件过小可能是 crash 导致编码未完成
        small_files = [iss for iss in r.issues if "文件过小" in iss]
        # 缺少关键文件
        critical_missing = [f for f in r.files_missing
                           if any(k in f for k in ['rgb.mp4', 'tracking.mp4', 'ctrl.mp4'])]
        # 帧数异常少
        low_frames = []
        for sname, stream in r.streams.items():
            if sname in ['rgb'] and 0 < stream.frame_count < 100:
                low_frames.append(f"{sname}={stream.frame_count}")
            if sname in ['tracking', 'ctrl'] and 0 < stream.frame_count < 200:
                low_frames.append(f"{sname}={stream.frame_count}")

        if small_files or critical_missing or low_frames:
            crash_indicators.append({
                'dataset': r.name,
                'small_files': small_files,
                'critical_missing': critical_missing,
                'low_frames': low_frames,
            })

    if crash_indicators:
        print(f"  检测到 {len(crash_indicators)} 个疑似 crash 的数据集:")
        for ci in crash_indicators[:10]:  # 最多显示 10 个
            print(f"    [{ci['dataset']}]")
            for sf in ci['small_files']:
                print(f"      - {sf}")
            for cm in ci['critical_missing']:
                print(f"      - 缺少: {cm}")
            for lf in ci['low_frames']:
                print(f"      - 帧数过少: {lf}")
    else:
        print("  未检测到 crash 迹象")


# ============================================================
# 绘图
# ============================================================

def generate_plots(results: List[DatasetResult], output_dir: str):
    """生成分析图表"""
    os.makedirs(output_dir, exist_ok=True)
    valid_results = [r for r in results if r.is_valid]

    if not valid_results:
        print("无有效数据集，跳过绘图")
        return

    # 1. 帧数分布图
    fig, axes = plt.subplots(2, 3, figsize=(18, 10))
    fig.suptitle('各数据流帧数分布', fontsize=14)

    stream_configs = [
        ('rgb', 'RGB (期望 ~300帧/10s@30fps)'),
        ('tracking', 'Tracking (期望 ~600帧/10s@60fps)'),
        ('ctrl', 'Ctrl (期望 ~600帧/10s@60fps)'),
        ('head_pose', 'Head Pose (期望 ~300帧)'),
        ('hand_tracking', 'Hand Tracking (期望 ~300帧)'),
        ('accel', 'IMU/Accel (期望 ~20000帧)'),
    ]

    for idx, (sname, title) in enumerate(stream_configs):
        ax = axes[idx // 3][idx % 3]
        frame_counts = [r.streams[sname].frame_count
                        for r in valid_results
                        if sname in r.streams and r.streams[sname].frame_count > 0]
        if frame_counts:
            ax.hist(frame_counts, bins=min(30, max(5, len(frame_counts) // 3)),
                    edgecolor='black', alpha=0.7, color='steelblue')
            ax.axvline(np.mean(frame_counts), color='red', linestyle='--',
                       label=f'mean={np.mean(frame_counts):.0f}')
            ax.set_title(title, fontsize=10)
            ax.set_xlabel('帧数')
            ax.set_ylabel('频次')
            ax.legend(fontsize=8)
        else:
            ax.text(0.5, 0.5, f'{sname}\n(无数据)', ha='center', va='center',
                    transform=ax.transAxes)
            ax.set_title(title, fontsize=10)

    plt.tight_layout()
    plt.savefig(os.path.join(output_dir, 'frame_count_distribution.png'), dpi=150)
    plt.close()

    # 2. 帧率分布图
    fig, axes = plt.subplots(1, 3, figsize=(15, 5))
    fig.suptitle('摄像头帧率分布', fontsize=14)

    for idx, (sname, expected_fps) in enumerate([('rgb', 30), ('tracking', 60), ('ctrl', 60)]):
        ax = axes[idx]
        fps_list = [r.streams[sname].avg_fps
                    for r in valid_results
                    if sname in r.streams and r.streams[sname].avg_fps > 0]
        if fps_list:
            ax.hist(fps_list, bins=min(30, max(5, len(fps_list) // 3)),
                    edgecolor='black', alpha=0.7, color='coral')
            ax.axvline(expected_fps, color='green', linestyle='--',
                       label=f'expected={expected_fps}fps')
            ax.axvline(np.mean(fps_list), color='blue', linestyle='--',
                       label=f'mean={np.mean(fps_list):.1f}fps')
            ax.set_title(f'{sname} 帧率分布', fontsize=10)
            ax.set_xlabel('FPS')
            ax.set_ylabel('频次')
            ax.legend(fontsize=8)
        else:
            ax.text(0.5, 0.5, f'{sname}\n(无数据)', ha='center', va='center',
                    transform=ax.transAxes)

    plt.tight_layout()
    plt.savefig(os.path.join(output_dir, 'fps_distribution.png'), dpi=150)
    plt.close()

    # 3. 时间戳对齐散点图 (如果数据足够)
    if len(valid_results) >= 2:
        fig, ax = plt.subplots(figsize=(12, 6))
        ax.set_title('各数据集持续时间对比', fontsize=14)

        names = [r.name[-8:] for r in valid_results]  # 取时间部分
        x = np.arange(len(valid_results))
        width = 0.15

        for offset, (sname, color) in enumerate([
            ('rgb', 'steelblue'), ('tracking', 'coral'),
            ('ctrl', 'green'), ('head_pose', 'purple'),
        ]):
            durations = []
            for r in valid_results:
                if sname in r.streams and r.streams[sname].has_data:
                    durations.append(r.streams[sname].duration_ms)
                else:
                    durations.append(0)

            valid_durations = [(i, d) for i, d in enumerate(durations) if d > 0]
            if valid_durations:
                indices, durs = zip(*valid_durations)
                ax.bar([x[i] + offset * width for i in indices],
                       durs, width, label=sname, color=color, alpha=0.8)

        ax.set_xlabel('数据集')
        ax.set_ylabel('持续时间 (ms)')
        ax.set_xticks(x + width * 1.5)
        ax.set_xticklabels(names, rotation=45, fontsize=7)
        ax.legend()
        ax.grid(axis='y', alpha=0.3)

        plt.tight_layout()
        plt.savefig(os.path.join(output_dir, 'duration_comparison.png'), dpi=150)
        plt.close()

    # 4. 单个数据集帧间隔热力图 (取前5个有效数据集)
    sample_results = valid_results[:5]
    if sample_results:
        fig, axes = plt.subplots(len(sample_results), 1, figsize=(15, 3 * len(sample_results)))
        if len(sample_results) == 1:
            axes = [axes]
        fig.suptitle('帧间隔时间线 (前5个数据集)', fontsize=14)

        for idx, r in enumerate(sample_results):
            ax = axes[idx]
            for sname, color in [('rgb', 'steelblue'), ('tracking', 'coral'), ('ctrl', 'green')]:
                if sname in r.streams and r.streams[sname].has_data:
                    intervals = r.streams[sname].intervals_ms
                    if intervals:
                        expected = 1000.0 / r.streams[sname].expected_fps
                        ax.plot(intervals, color=color, alpha=0.7, linewidth=0.5,
                                label=f'{sname} (avg={np.mean(intervals):.1f}ms)')
                        ax.axhline(expected, color=color, linestyle='--', alpha=0.3)

            ax.set_title(f'{r.name}', fontsize=10)
            ax.set_ylabel('帧间隔 (ms)')
            ax.legend(fontsize=7, loc='upper right')
            ax.grid(alpha=0.3)

        plt.tight_layout()
        plt.savefig(os.path.join(output_dir, 'frame_intervals_timeline.png'), dpi=150)
        plt.close()

    # 5. 异常统计图
    issue_counts = defaultdict(int)
    for r in valid_results:
        for issue in r.issues:
            if "帧间隔过大" in issue:
                issue_counts["丢帧"] += 1
            elif "帧率异常" in issue:
                issue_counts["帧率异常"] += 1
            elif "抖动" in issue:
                issue_counts["帧间隔抖动"] += 1
            elif "时间差异" in issue or "时间范围" in issue:
                issue_counts["时间戳不对齐"] += 1
            elif "metainfo" in issue:
                issue_counts["metainfo异常"] += 1
            elif "文件过小" in issue:
                issue_counts["文件异常(可能crash)"] += 1

    if issue_counts:
        fig, ax = plt.subplots(figsize=(10, 6))
        cats = list(issue_counts.keys())
        counts = list(issue_counts.values())
        colors = plt.cm.Set3(np.linspace(0, 1, len(cats)))
        ax.barh(cats, counts, color=colors, edgecolor='black')
        ax.set_xlabel('出现次数')
        ax.set_title('异常分类统计', fontsize=14)
        for i, v in enumerate(counts):
            ax.text(v + 0.5, i, str(v), va='center')
        plt.tight_layout()
        plt.savefig(os.path.join(output_dir, 'issue_summary.png'), dpi=150)
        plt.close()

    print(f"\n图表已保存到: {output_dir}/")


# ============================================================
# 主流程
# ============================================================

def pull_datasets(device_base_dir: str, local_base_dir: str):
    """从设备拉取所有数据集"""
    try:
        result = subprocess.run(
            ['adb', 'shell', 'ls', '-d', f'{device_base_dir}/20*'],
            capture_output=True, text=True, timeout=30)
        dirs = [d.strip() for d in result.stdout.strip().split('\n') if d.strip()]
    except Exception as e:
        print(f"获取设备数据集列表失败: {e}")
        return

    print(f"设备上发现 {len(dirs)} 个数据集")

    for device_dir in dirs:
        dirname = os.path.basename(device_dir)
        local_dir = os.path.join(local_base_dir, dirname)
        if os.path.exists(local_dir):
            print(f"  跳过 (已存在): {dirname}")
            continue
        print(f"  拉取: {dirname}...", end='', flush=True)
        os.makedirs(local_dir, exist_ok=True)
        subprocess.run(['adb', 'pull', f'{device_dir}/', local_dir],
                       capture_output=True, timeout=120)
        print(" 完成")

    print("数据拉取完成")


def find_datasets(base_dir: str) -> List[str]:
    """查找所有数据集目录"""
    datasets = []
    if not os.path.exists(base_dir):
        return datasets
    for entry in sorted(os.listdir(base_dir)):
        path = os.path.join(base_dir, entry)
        if os.path.isdir(path) and re.match(r'^\d{8}_\d{6}$', entry):
            datasets.append(path)
    return datasets


def main():
    parser = argparse.ArgumentParser(description='数据集时间戳对齐分析')
    parser.add_argument('--dir', default='../dataset',
                        help='数据集目录 (默认: ../dataset)')
    parser.add_argument('--pull', action='store_true',
                        help='先从设备拉取数据')
    parser.add_argument('--device-dir',
                        default='/sdcard/Android/data/com.ssnwt.helloxr/files/dataset',
                        help='设备上的数据集路径')
    parser.add_argument('--output', default=None,
                        help='分析结果输出目录')
    args = parser.parse_args()

    base_dir = os.path.abspath(args.dir)
    output_dir = args.output or os.path.join(base_dir, '..', 'dataset_analysis', 'report')

    print("=" * 80)
    print("  数据集时间戳对齐分析")
    print("=" * 80)
    print(f"数据集目录: {base_dir}")
    print(f"报告输出:   {output_dir}")

    # 从设备拉取数据
    if args.pull:
        print("\n--- 从设备拉取数据 ---")
        pull_datasets(args.device_dir, base_dir)

    # 查找数据集
    datasets = find_datasets(base_dir)
    print(f"\n发现 {len(datasets)} 个数据集")

    if not datasets:
        print("未找到数据集目录 (格式: YYYYMMDD_HHMMSS)")
        sys.exit(1)

    # 分析每个数据集
    results = []
    print("\n--- 逐个数据集分析 ---")
    for i, ds_path in enumerate(datasets):
        ds_name = os.path.basename(ds_path)
        result = analyze_dataset(ds_path)
        results.append(result)

        # 显示状态
        status = "OK" if not result.issues else f"WARN({len(result.issues)})"
        stream_summary = []
        for sname in ['rgb', 'tracking', 'ctrl', 'head_pose', 'hand_tracking']:
            if sname in result.streams:
                s = result.streams[sname]
                if s.frame_count > 0:
                    stream_summary.append(f"{sname}={s.frame_count}")

        print(f"  [{i+1:3d}/{len(datasets)}] {ds_name}  [{status}]  "
              f"{'  '.join(stream_summary)}")

        # 如果有异常，显示详情
        if result.issues:
            for issue in result.issues[:3]:  # 最多显示 3 条
                print(f"         ⚠ {issue}")
            if len(result.issues) > 3:
                print(f"         ... 还有 {len(result.issues)-3} 条异常")

    # 跨数据集统计
    cross_dataset_analysis(results, output_dir)

    # 生成图表
    print("\n--- 生成分析图表 ---")
    generate_plots(results, output_dir)

    # 保存详细报告
    report_path = os.path.join(output_dir, 'analysis_report.txt')
    os.makedirs(output_dir, exist_ok=True)
    with open(report_path, 'w') as f:
        f.write("=" * 80 + "\n")
        f.write("  数据集时间戳对齐分析报告\n")
        f.write("=" * 80 + "\n\n")

        # 汇总
        valid = [r for r in results if r.is_valid]
        f.write(f"总数据集: {len(results)}\n")
        f.write(f"有效: {len(valid)}\n")
        f.write(f"有异常: {sum(1 for r in valid if r.issues)}\n\n")

        # 每个数据集详情
        for r in results:
            f.write(f"\n{'='*60}\n")
            f.write(f"数据集: {r.name}\n")
            f.write(f"{'='*60}\n")
            f.write(f"文件: {', '.join(r.files_found)}\n")
            if r.files_missing:
                f.write(f"缺少: {', '.join(r.files_missing)}\n")
            f.write(f"\n数据流:\n")
            for sname, stream in r.streams.items():
                f.write(f"  {sname}: {stream.frame_count} 帧")
                if stream.avg_fps > 0:
                    f.write(f", {stream.avg_fps:.1f} fps")
                if stream.duration_ms > 0:
                    f.write(f", {stream.duration_ms/1000:.2f}s")
                f.write(f" (单位: {stream.timestamp_unit})\n")
            if r.issues:
                f.write(f"\n异常 ({len(r.issues)}):\n")
                for issue in r.issues:
                    f.write(f"  ⚠ {issue}\n")
            else:
                f.write("\n无异常\n")

    print(f"\n详细报告已保存: {report_path}")
    print("\n分析完成!")


if __name__ == '__main__':
    main()
