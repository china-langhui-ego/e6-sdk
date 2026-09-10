# 串口时间同步 Server (Windows / Linux)

通过 CDC-ACM 串口与 VR 头显进行 NTP 时间同步。**免驱动**——
Windows 自带 `usbser.sys` 自动识别 CDC-ACM 设备为 COM 口，
Linux 内核 `cdc_acm` 模块枚举为 `/dev/ttyACM*`，均无需 Zadig/WinUSB/管理员权限。

## 使用

### Windows / macOS

NTP 时间同步服务已集成进 EgoVision PC 端，无需单独启动脚本：在 App 内打开「时间同步」开关（默认关闭，打开后下次启动自动恢复），连接 USB 后即自动响应 VR 的时间请求。

### Linux

```sh
chmod +x linux_run_time_sync.sh
./linux_run_time_sync.sh   # 启动（Ctrl+C 退出）
```

脚本自动选择 Python 运行环境，优先级从高到低：

1. **`tools/linux-venv/`** — 已存在则直接用
2. **系统 Python 3.10+** — 有 pyserial 直接跑；没有则自动创建 `tools/linux-venv/`
3. **自动下载独立 Python 3.12** — 系统没有 3.10+ 时，下载到 `tools/python-linux/`，再创建 venv

首次运行还会自动配置 udev 规则和 dialout 组（仅首次需要 sudo），后续运行无需权限。

## 文件

| 文件 | 作用 |
|------|------|
| `scripts/time_sync_server.py` | NTP 同步主程序 |
| `scripts/find_port.py` | 按 VID-PID 18D1:903B 自动识别串口 |
| `scripts/serial_common.py` | NTP 协议常量 + NTP 时间戳时钟（直读系统时间） |
| `linux_run_time_sync.sh` | Linux 启动（自动选 Python + 配置环境） |
| `tools/linux-venv/` | Linux venv（自动生成，不入库） |
| `tools/python-linux/` | Linux 独立 Python 3.12（系统无 3.10+ 时自动下载） |

## 故障排查

| 现象 | 原因 | 解决 |
|------|------|------|
| `No VR serial port found` | Ego VR 应用未运行（应用启动时自行开启 acm，本服务不做 setprop 以免误伤非 Ego 设备） | 启动/重启 VR 端 Ego 应用，重连 USB |
| `Open COMx failed` (Windows) | 端口被占用 | 关闭其他串口工具 |
| `adb devices` 空 (Linux) | udev 规则缺 `4ee2` PID | 重跑 `./linux_run_time_sync.sh` |
| `Permission denied: /dev/ttyACM0` (Linux) | 不在 dialout 组 | 脚本已自动添加，重新登录或 `newgrp dialout` |
