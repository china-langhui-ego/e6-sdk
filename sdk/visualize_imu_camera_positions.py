#!/usr/bin/env python3
"""
IMU—相机位置关系可视化 — 把 camera_params_{rgb,ctrl,tracking}.json 的外参
在 IMU(Body) 坐标系下渲染成交互式 3D HTML，聚焦"位置关系"：
  - IMU 位于原点（外参均为 Camera→Body，Body 原点即 IMU）
  - 每个相机中心 → IMU 原点的连线，标注距离 (mm)
  - 每组双目 L<->R 基线连线，标注基线长度 (mm)
  - 相机局部坐标轴（短轴）辅助表达朝向；不画 frustum（需要视锥请用
    visualize_extrinsics.py）

外参约定（同 camera_analysis.py / visualize_extrinsics.py）：
  - Camera→Body/IMU (Tbc): X_body = R @ X_cam + t
  - rotation = 四元数 [x, y, z, w]
  - position = 相机原点在 IMU/Body 系中的位置（相机中心）
数据集 JSON 为 format v2（带顶层 "version" 字段），向下兼容 v1（无 version）。

用法：
  python3 visualize_imu_camera_positions.py <dataset_dir> [--out out.html] [--opencv]
"""
import argparse
import json
import os
import sys

import numpy as np
import plotly.graph_objects as go

try:
    from camera_analysis import quat_to_rotmat  # 与权威实现数值一致
except Exception:  # 回退：内联同名实现（与 camera_analysis.py:50-57 一致）
    def quat_to_rotmat(q_xyzw):
        x, y, z, w = q_xyzw
        return np.array([
            [1 - 2*(y*y + z*z), 2*(x*y - w*z),     2*(x*z + w*y)],
            [2*(x*y + w*z),     1 - 2*(x*x + z*z), 2*(y*z - w*x)],
            [2*(x*z - w*y),     2*(y*z + w*x),     1 - 2*(x*x + y*y)]
        ])


GROUPS = ['rgb', 'ctrl', 'tracking']
GROUP_COLORS = {'rgb': '#1f77b4', 'ctrl': '#2ca02c', 'tracking': '#ff7f0e'}

# OpenXR Body → OpenCV Body 转换（对当前格式 v2 / 新 libcamera 外参）：
#   F = diag(1,-1,-1)  # Body 翻转：OpenXR Body (X右/Y上/Z后) → OpenCV Body (X右/Y下/Z前)
#   R_cv = F @ R_svr   # 旋转：仅 Body 翻转，无相机侧换基
#   t_cv = F @ t_svr   # 平移：即 [x,-y,-z]
#
# 为什么不右乘 Readme 3.3 的 H.T（SVR 相机系→optical 换基）：Readme 的配方及其
# "R_cv ≈ I" 自测是按老外参约定（6 月数据集）写的。当前 libcamera 输出的相机侧
# 旋转已是"存储图像方向"语义，再乘 H.T 会重复滚转 90°（R_cv ≈ Hᵀ，错误）。
# 实证（20260731_132954 数据集解码帧）：
#   - RGB：R_svr ≈ diag(1,-1,-1)，F·R ≈ I，且 rgb.mp4 左半帧场景正立
#     （图像右 = 场景右 = body +X）→ 相机侧已是 optical 语义 ✓
#   - tracking：R 的 camX(图像右) ≈ 场景下方向，且 tracking.mp4 画面内容横躺
#     （场景上 = 图像左）→ 竖装 90° 已包含在 R 内 ✓
F_BODY_FLIP = np.diag([1.0, -1.0, -1.0])


def svr_body_to_opencv(cameras):
    """将全部相机外参由 OpenXR Body 系转换到 OpenCV Body 系（原地修改）：
    R_cv = F·R, t_cv = F·t（当前 libcamera 外参相机侧已是存储图像/optical 语义，
    无需 Readme 3.3 中针对老约定的 H.T 换基）。"""
    for cam in cameras.values():
        cam['position'] = F_BODY_FLIP @ cam['position']
        cam['R'] = F_BODY_FLIP @ cam['R']


def load_extrinsics(base_dir):
    """读取 3 组 camera_params_*.json → {name: cam_dict}（位置 + R，IMU 系）。"""
    cameras = {}
    for group in GROUPS:
        path = os.path.join(base_dir, f'camera_params_{group}.json')
        if not os.path.exists(path):
            print(f"  [警告] 缺失 {path}，跳过 {group} 组", file=sys.stderr)
            continue
        with open(path) as f:
            data = json.load(f)
        ver = data.get('version', 1)
        for cam in data['cameras']:
            extr = cam['extrinsics']
            pos = np.array(extr['position'], dtype=np.float64)
            if pos.shape != (3,):
                raise ValueError(f"{group}-{cam['eye']} position 维度错误: {pos}")
            q = np.array(extr['rotation'], dtype=np.float64)
            q = q / np.linalg.norm(q)
            name = f"{group}-{cam['eye']}"
            cameras[name] = {
                'name': name, 'group': group, 'eye': cam['eye'],
                'position': pos, 'R': quat_to_rotmat(q), 'version': ver,
            }
    return cameras


def print_summary(cameras, frame_name='IMU'):
    """控制台打印：各相机相对 IMU 的位置/距离 + 各组基线。"""
    print(f"\n=== 相机相对 IMU 的位置关系 ({frame_name} 系) ===")
    print(f"{'相机':<16}{'位置 (mm)':<36}{'距 IMU (mm)':<12}")
    for name, cam in cameras.items():
        pmm = cam['position'] * 1000.0
        dist = np.linalg.norm(cam['position']) * 1000.0
        print(f"{name:<16}({pmm[0]:+8.2f}, {pmm[1]:+8.2f}, {pmm[2]:+8.2f})  {dist:8.2f}")
    print("\n=== 各组 L<->R 基线 ===")
    for group in GROUPS:
        l = cameras.get(f'{group}-left'); r = cameras.get(f'{group}-right')
        if l and r:
            bl = np.linalg.norm(l['position'] - r['position']) * 1000.0
            print(f"  {group:<9} baseline = {bl:7.2f} mm")
        else:
            print(f"  {group:<9} (缺失，跳过)")


def _line(p0, p1, color, dash='solid', width=2, legendgroup=None,
          showlegend=False, name=None):
    return go.Scatter3d(
        x=[p0[0], p1[0]], y=[p0[1], p1[1]], z=[p0[2], p1[2]],
        mode='lines', line=dict(color=color, width=width, dash=dash),
        name=name, legendgroup=legendgroup, showlegend=showlegend,
        hoverinfo='skip',
    )


def build_figure(cameras, frame_name='IMU'):
    """构建 3D 场景：IMU 原点 + 相机中心 + IMU→相机距离线 + 双目基线。

    frame_name: 'IMU'（SVR Body 系）或 'OpenCV'（H 变换后），仅影响标题与轴标签。
    """
    fig = go.Figure()
    annotations = []  # layout.scene.annotations（3D 文本标注）
    origin = np.zeros(3)

    # IMU 原点：标记 + 三轴
    fig.add_trace(go.Scatter3d(
        x=[0], y=[0], z=[0], mode='markers+text',
        marker=dict(color='black', size=7, symbol='diamond'),
        text=['IMU (Body origin)'], textposition='bottom center',
        textfont=dict(size=11, color='black'),
        name='IMU', showlegend=True,
        hovertemplate='IMU origin (0,0,0)<extra></extra>',
    ))
    L = 0.03
    for ax_name, color, vec in [('X', '#e41a1c', [L, 0, 0]),
                                ('Y', '#4daf4a', [0, L, 0]),
                                ('Z', '#377eb8', [0, 0, L])]:
        fig.add_trace(_line(origin, np.array(vec, float), color,
                            width=8, legendgroup='IMU',
                            showlegend=(ax_name == 'X'), name='IMU axes'))

    # 每个相机：中心点 + 局部轴 + IMU→相机连线（带距离标注）
    legend_seen = set()
    for name, cam in cameras.items():
        color = GROUP_COLORS.get(cam['group'], '#888888')
        lg = cam['group']
        showlegend = lg not in legend_seen
        legend_seen.add(lg)
        label = f"{lg} ({'L' if cam['eye'] == 'left' else 'R'})"
        pos = cam['position']
        dist_mm = np.linalg.norm(pos) * 1000.0

        # 相机中心
        fig.add_trace(go.Scatter3d(
            x=[pos[0]], y=[pos[1]], z=[pos[2]],
            mode='markers+text', marker=dict(color=color, size=6),
            text=[name], textposition='top center',
            textfont=dict(size=9, color=color),
            name=label, legendgroup=lg, showlegend=showlegend,
            hovertemplate=(f"{name}<br>pos=({pos[0]:.4f},{pos[1]:.4f},{pos[2]:.4f}) m"
                           f"<br>dist to IMU={dist_mm:.1f} mm<extra></extra>"),
        ))

        # 相机局部坐标轴 (X=红, Y=绿, Z=蓝)，短轴仅示意朝向
        ax_len = 0.015
        for ax_idx, ax_color in enumerate(['#ff4444', '#44ff44', '#4444ff']):
            tip = pos + ax_len * cam['R'][:, ax_idx]
            fig.add_trace(_line(pos, tip, ax_color, width=3, legendgroup=lg))

        # IMU → 相机连线 + 中点距离标注
        fig.add_trace(_line(origin, pos, color, dash='dot', width=2,
                            legendgroup=lg,
                            showlegend=(showlegend), name=f'{lg} dist to IMU'))
        mid = pos / 2.0
        annotations.append(dict(
            x=mid[0], y=mid[1], z=mid[2],
            text=f'{dist_mm:.1f}', showarrow=False,
            font=dict(size=9, color=color),
        ))

    # 每组双目基线 + 标注
    for group in GROUPS:
        l = cameras.get(f'{group}-left'); r = cameras.get(f'{group}-right')
        if not (l and r):
            continue
        color = GROUP_COLORS.get(group, '#888888')
        bl_mm = np.linalg.norm(l['position'] - r['position']) * 1000.0
        fig.add_trace(_line(l['position'], r['position'], color,
                            dash='solid', width=6,
                            legendgroup=f'{group}-baseline', showlegend=True,
                            name=f'{group} baseline'))
        mid = (l['position'] + r['position']) / 2.0
        annotations.append(dict(
            x=mid[0], y=mid[1], z=mid[2] + 0.004,
            text=f'<b>{bl_mm:.1f} mm</b>', showarrow=False,
            font=dict(size=10, color=color),
        ))

    fig.update_layout(
        title=f'IMU—相机位置关系（{frame_name} 系，单位 m；标注单位 mm）',
        scene=dict(xaxis_title=f'{frame_name} X (m)', yaxis_title=f'{frame_name} Y (m)',
                   zaxis_title=f'{frame_name} Z (m)', aspectmode='data',
                   annotations=annotations),
        legend=dict(itemsizing='constant'),
        margin=dict(l=0, r=0, t=40, b=0),
    )
    return fig


def main():
    parser = argparse.ArgumentParser(description='IMU—相机位置关系 3D 可视化')
    parser.add_argument('dataset_dir',
                        help='数据集目录（含 camera_params_*.json）')
    parser.add_argument('--out', default=None, help='输出 HTML 路径')
    parser.add_argument('--opencv', action='store_true',
                        help='转换到 OpenCV Body 系 (X右/Y下/Z前) 再可视化：'
                             't_cv = [x,-y,-z]，R_cv = F·R（仅 Body 翻转；'
                             '当前 libcamera 外参相机侧已是存储图像/optical 语义）')
    args = parser.parse_args()

    base_dir = args.dataset_dir
    if not os.path.isdir(base_dir):
        sys.exit(f"错误：数据集目录不存在: {base_dir}")

    print(f"加载外参: {base_dir}")
    cameras = load_extrinsics(base_dir)
    if not cameras:
        sys.exit("错误：未加载到任何相机参数")
    print(f"  共 {len(cameras)} 个相机: {', '.join(cameras.keys())}")

    frame_name = 'IMU'
    if args.opencv:
        svr_body_to_opencv(cameras)
        frame_name = 'OpenCV'
        print("  [转换] OpenXR Body → OpenCV Body：t_cv = [x,-y,-z]，R_cv = F·R "
              "（仅 Body 翻转，无相机侧换基）")

    print_summary(cameras, frame_name)

    fig = build_figure(cameras, frame_name)
    out_path = args.out or os.path.join(base_dir, 'imu_camera_positions.html')
    fig.write_html(out_path, include_plotlyjs='cdn')
    print(f"\n已保存: {out_path}")


if __name__ == '__main__':
    main()
