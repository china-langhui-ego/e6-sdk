"""串口时间同步 Server (Windows, pyserial)

通过 CDC-ACM 串口与 VR 头显进行 NTP 时间同步。免驱动。
自动检测 VR USB 插入/拔出，自动启停时间同步。acm 串口由 VR 端 Ego 应用
自行开启，本脚本不做任何 setprop（避免误伤不支持 acm 的非 Ego 设备）。

用法:
    python time_sync_server.py          # 持续运行，自动管理
    python time_sync_server.py --once   # 单次同步后退出
    python time_sync_server.py --port COM3  # 跳过 adb，指定端口直连
"""
import sys, os, signal, struct, time as _time, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import serial

from serial_common import (
    ts_print, ts_printerr,
    _calibrate_clock, ntp_now, now_mono_ns,
    PKT_MAGIC, PKT_VERSION, PKT_REQ, PKT_REQ_TEST, PKT_RESULT, PKT_STOP,
    REQ_SIZE, RESP_SIZE, RESULT_SIZE, _get_be32, _get_be64, _pack_ntp_response, _pack_stop,
)
from find_port import find_vr_serial_port

BAUDRATE = 2000000
READ_TIMEOUT_S = 1.0  # PC→VR heart beat 间隔(HEARTBEAT 由 idle 驱动)
VR_VID, VR_PID = 0x18D1, 0x903B
ADB_TIMEOUT = 15
ACM_WAIT_TIMEOUT = 25

_vr_serial = None  # 运行时检测

_shutdown_requested = False


# ====== 串口 I/O ======

def _reset_serial(ser):
    try:
        ser.reset_input_buffer()
    except Exception:
        pass


def _read_fully(ser, size, timeout_s):
    buf = bytearray()
    deadline = now_mono_ns() + int(timeout_s * 1_000_000_000)
    while len(buf) < size:
        if _shutdown_requested:
            return None
        try:
            chunk = ser.read(size - len(buf))
        except serial.SerialException:
            raise  # 重新抛出让调用方判断设备断开
        if chunk:
            buf.extend(chunk)
        if now_mono_ns() >= deadline:
            break
    return bytes(buf) if len(buf) == size else None


def _open_serial(port):
    ser = serial.Serial(port, BAUDRATE,
        bytesize=serial.EIGHTBITS, parity=serial.PARITY_NONE,
        stopbits=serial.STOPBITS_ONE, timeout=0.1)
    ser.reset_input_buffer()
    ser.reset_output_buffer()
    # drain 残留数据：VR 可能在 COM 口打开前就已发送请求堆积在缓冲
    drained = 0
    deadline = now_mono_ns() + 500_000_000  # 500ms
    while now_mono_ns() < deadline:
        try:
            if ser.read(256):
                drained += 1
            else:
                break
        except Exception:
            break
    if drained:
        ts_print(f"Drained {drained} stale chunks from buffer")
    ser.reset_input_buffer()
    return ser


def _send_stop(ser):
    try:
        ser.write(_pack_stop())
        ser.flush()
        ts_print("STOP packet sent to VR")
    except Exception:
        pass


# ====== NTP 同步循环 ======

# ====== 批量统计（自适应断批） ======
BATCH_GAP_MS = 50  # 相邻请求间隔 > 此值断批（VR measure 内交换通常 <50ms）


_idx = [0]  # 全局批次序号


def _flush_batch(batch_start_ns, batch_count, min_gap_ms, max_gap_ms,
                 batch_end_ns=0, extra=""):
    """输出批次统计。batch_end_ns=0 表示用当前时间；否则用指定结束时间。
       extra 非空时追加到行尾（用于合并 RESULT 数据）。"""
    if batch_count == 0:
        return
    _idx[0] += 1
    end_ns = batch_end_ns if batch_end_ns else now_mono_ns()
    elapsed_ms = max(1, (end_ns - batch_start_ns) // 1_000_000)
    line = (f"#{_idx[0]:4d} | {batch_count:2d} reqs | {elapsed_ms:2d}ms batch | "
            f"gap {min_gap_ms:.1f}-{max_gap_ms:4.1f}ms")
    if extra:
        line += extra
    ts_print(line)


# ====== NTP 同步循环 ======

def _send_heartbeat(ser):
    """PC→VR 心跳：发 HEARTBEAT(0xEE)，失败静默（串口下游异常 PC 侧无法控制）。"""
    try:
        ser.write(b'\xEE')
        ser.flush()
    except Exception:
        pass


def _time_sync_loop(ser, once=False):
    ts_print("Device connected. Waiting for time requests...")
    # 立即发 HEARTBEAT → VR 端 nativeWaitForReady 收到后确认 PC 可达
    _send_heartbeat(ser)
    idle_count = 0
    serial_errors = 0  # 连续串口异常计数

    # 自适应断批统计
    batch_count = 0
    batch_start_ns = 0
    min_gap_ms = 0.0
    max_gap_ms = 0.0
    last_req_ns = 0

    while not _shutdown_requested:
        try:
            req = _read_fully(ser, REQ_SIZE, READ_TIMEOUT_S)
        except serial.SerialException as e:
            serial_errors += 1
            ts_printerr(f"Serial read error (#{serial_errors}): {e}")
            if serial_errors >= 3:
                ts_printerr("Serial lost (3 consecutive errors), closing port")
                return False  # 设备断开，返回主循环等待重连
            _time.sleep(0.5)
            continue

        serial_errors = 0  # 成功读取，重置错误计数

        if req is None:
            if _shutdown_requested:
                _send_stop(ser)
                break
            # 空闲超时 → 收尾当前批次。跳过启动时的单次残留请求。
            if batch_count > 1:
                _flush_batch(batch_start_ns, batch_count, min_gap_ms,
                             max_gap_ms, last_req_ns)
            batch_count = 0
            # PC→VR 心跳：每次空闲都发 HEARTBEAT(0xEE)，间隔约 READ_TIMEOUT_S(1s)。
            # _send_heartbeat 在 idle 分支内无条件执行，与 idle_count 取值无关，故每次 idle 都会发送。
            _send_heartbeat(ser)
            idle_count += 1
            if idle_count == 6:
                ts_print("Waiting for request...")
            continue

        idle_count = 0

        # VR 结果上报（RESULT 包）→ flush 当前批次并合并 RESULT 数据到行尾
        if req[2] == PKT_RESULT and len(req) >= RESULT_SIZE:
            # setClock 方案下 VR 上报 k 恒 0、correction 恒等于 offset，日志不再解析/展示
            round_num, off_us, rtt_us = struct.unpack('>iiI', req[4:16])
            if round_num == 1:
                _idx[0] = 0  # 新一轮时间同步采集：批次计数清零，从 1 重新开始
            extra = (f" | RTT={rtt_us/1000:.3f}ms | offset={off_us/1000:+10.3f}ms")
            _flush_batch(batch_start_ns, batch_count, min_gap_ms,
                         max_gap_ms, last_req_ns, extra=extra)
            batch_count = 0
            min_gap_ms = 0.0
            max_gap_ms = 0.0
            continue

        if (req[0] != PKT_MAGIC or req[1] != PKT_VERSION
                or req[2] not in (PKT_REQ, PKT_REQ_TEST)):
            continue

        seq = _get_be32(req, 4)
        origin_ts = _get_be64(req, 8)

        recv_ts = ntp_now()
        # 打点对称化：xmit_ts 贴近实际发送（先组包占位，write 前最后时刻打点
        # 回填 @24），把"打点→字节提交"的 pack+调度延迟压到最小——该延迟的
        # 一半直接进入 offset 成为测量系统误差 eps（实测 eps≈-0.13ms 由它主导）。
        # 与 VR 端 t1 贴近 writeFully 对称。
        resp = bytearray(_pack_ntp_response(seq, origin_ts, recv_ts, 0))
        struct.pack_into(">Q", resp, 24, ntp_now())

        try:
            ser.write(resp)
            ser.flush()
        except Exception as e:
            _flush_batch(batch_start_ns, batch_count, min_gap_ms,
                         max_gap_ms, last_req_ns)
            ts_printerr(f"write failed: {e}, device may be disconnected")
            return False

        # 自适应断批：与上一个请求间隔 > BATCH_GAP_MS → 新批次
        now_ns = now_mono_ns()
        gap_ms = (now_ns - last_req_ns) / 1_000_000.0 if last_req_ns > 0 else 0.0
        if last_req_ns > 0 and gap_ms > BATCH_GAP_MS:
            _flush_batch(batch_start_ns, batch_count, min_gap_ms,
                         max_gap_ms, last_req_ns)
            batch_count = 0
            min_gap_ms = 0.0
            max_gap_ms = 0.0

        if batch_count == 0:
            batch_start_ns = now_ns

        # 记录批次内帧间隔统计
        if batch_count > 0:
            if min_gap_ms == 0.0:
                min_gap_ms = gap_ms
            else:
                min_gap_ms = min(min_gap_ms, gap_ms)
            max_gap_ms = max(max_gap_ms, gap_ms)

        batch_count += 1
        last_req_ns = now_ns

        if once:
            ts_print("Single sync completed.")
            break

    return not _shutdown_requested


# ====== VR 设备检测与串口模式管理 ======

def _detect_vr_serial():
    """通过 adb devices -l 自动识别 VR 设备（model 含 sxr），返回序列号或 None。"""
    try:
        r = subprocess.run(["adb", "devices", "-l"],
                           capture_output=True, timeout=10)
        for line in r.stdout.decode(errors="replace").splitlines():
            if "device" not in line:
                continue
            if "model:" not in line:
                continue
            parts = line.split()
            model = ""
            for p in parts:
                if p.startswith("model:"):
                    model = p.split(":", 1)[1]
                    break
            if model and "sxr" in model.lower():
                return parts[0]  # 第一列是序列号
    except Exception:
        pass
    return None


def _adb(args, timeout=ADB_TIMEOUT):
    """执行 adb 命令（自动检测 VR 设备序列号），返回 (returncode, stdout_str)。"""
    global _vr_serial
    if _vr_serial is None:
        _vr_serial = _detect_vr_serial()
    if _vr_serial is None:
        return -1, "VR device not found"
    try:
        r = subprocess.run(["adb", "-s", _vr_serial] + args,
                           capture_output=True, timeout=timeout)
        return r.returncode, r.stdout.decode(errors="replace").strip()
    except Exception as e:
        return -1, str(e)


def _is_vr_online():
    global _vr_serial
    _vr_serial = _detect_vr_serial()
    if _vr_serial is None:
        return False
    rc, _ = _adb(["shell", "echo", "ok"])
    return rc == 0


def _wait_for_vr_com_port():
    """等待 VR COM 口出现。返回端口名或 None。

    acm 由 VR 端 Ego 应用自行开启（BleService 启动/看门狗 ensureAcmUsbConfig），
    本脚本不做 setprop sys.usb.config：adb 检测按 model 含 sxr 匹配，可能命中
    不支持 acm 的非 Ego 设备，代设会触发其 USB 重新枚举、adb 永久掉线。
    """
    port = find_vr_serial_port()
    if port:
        ts_print(f"VR already in ACM mode: {port}")
        return port

    ts_print(f"Waiting for COM port (VID {VR_VID:04X} PID {VR_PID:04X})...")
    deadline = _time.time() + ACM_WAIT_TIMEOUT
    while _time.time() < deadline:
        if _shutdown_requested:
            return None
        port = find_vr_serial_port()
        if port:
            ts_print(f"COM port found: {port}")
            return port
        _time.sleep(0.5)
    ts_printerr("Timeout: COM port did not appear. Check USB connection and "
                "that the Ego VR app is running (it enables the ACM port).")
    return None


def _wait_for_vr_usb():
    """阻塞等待 VR USB 插入并与 ADB 通信。每 3s 检测一次。"""
    global _vr_serial
    _vr_serial = None  # 重置，重新检测
    while not _shutdown_requested:
        if _is_vr_online():
            return True
        _time.sleep(3)
    return False


# ====== 信号处理 ======

_console_ctrl_pressed = 0

def _signal_handler(signum, frame):
    global _shutdown_requested, _console_ctrl_pressed
    _console_ctrl_pressed += 1
    if _console_ctrl_pressed == 1:
        _shutdown_requested = True
        ts_print("Shutting down, press Ctrl+C again to force quit...")
    else:
        ts_print("Force quitting.")
        signal.signal(signal.SIGINT, signal.SIG_DFL)
        os.kill(os.getpid(), signal.SIGINT)


# ====== 主入口 ======

def main():
    global _shutdown_requested
    import argparse
    parser = argparse.ArgumentParser(description="Serial Time Sync Server")
    parser.add_argument("--once", action="store_true", help="单次同步后退出")
    parser.add_argument("--port", default=None, help="指定串口（Windows: COMx，Linux: /dev/ttyACMx）")
    args = parser.parse_args()

    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(line_buffering=True)
    if hasattr(sys.stderr, "reconfigure"):
        sys.stderr.reconfigure(line_buffering=True)

    _calibrate_clock()
    signal.signal(signal.SIGINT, _signal_handler)
    signal.signal(signal.SIGTERM, _signal_handler)

    # 控制台窗口事件（仅 Windows，捕获关闭按钮；Linux 靠 SIGTERM/SIGINT 已足够）
    if sys.platform == 'win32':
        try:
            import ctypes
            from serial_common import _kernel32
            CTRL_CLOSE_EVENT = 2
            _HANDLER_ROUTINE = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_ulong)
            _kernel32.SetConsoleCtrlHandler.argtypes = [ctypes.c_void_p, ctypes.c_bool]
            _kernel32.SetConsoleCtrlHandler.restype = ctypes.c_bool
            @_HANDLER_ROUTINE
            def _console_ctrl_handler(dwCtrlType):
                global _shutdown_requested
                if dwCtrlType == CTRL_CLOSE_EVENT:
                    _shutdown_requested = True
                    return True
                return False
            _kernel32.SetConsoleCtrlHandler(_console_ctrl_handler, True)
        except Exception:
            pass  # 非关键，SIGINT 已覆盖 Ctrl+C

    ts_print("=== Serial Time Sync Server (pyserial) ===")

    if args.port:
        # 直连模式：跳过 adb，直接用指定端口
        ts_print(f"Opening {args.port} @ {BAUDRATE} baud...")
        try:
            ser = _open_serial(args.port)
        except Exception as e:
            ts_printerr(f"Open {args.port} failed: {e}")
            return
        disconnected = _time_sync_loop(ser, once=args.once)
        try:
            if _shutdown_requested:
                _send_stop(ser)
            ser.close()
        except Exception:
            pass
        ts_print("Server stopped.")
        return

    # 自动模式：adb 检测 VR + 等待 COM 口（acm 由 VR 端应用开启）+ 同步
    while not _shutdown_requested:
        # 1) 检测 VR 是否存在（通过 ADB）
        if not _is_vr_online():
            ts_print("VR not detected. Waiting for USB connection...")
            _wait_for_vr_usb()
            if _shutdown_requested:
                break
            ts_print("VR detected.")

        # 2) 等待 COM 口（PC 端不做 setprop，防误伤不支持 acm 的非 Ego 设备）
        port = _wait_for_vr_com_port()
        if port is None:
            ts_printerr("No COM port. Retrying...")
            _time.sleep(3)
            continue

        # 3) 打开 COM 口并启动时间同步
        ts_print(f"Opening {port} @ {BAUDRATE} baud...")
        try:
            ser = _open_serial(port)
        except Exception as e:
            ts_printerr(f"Open {port} failed: {e}")
            _time.sleep(2)
            continue

        disconnected = _time_sync_loop(ser, once=args.once)
        try:
            if _shutdown_requested:
                _send_stop(ser)
            ser.close()
        except Exception:
            pass

        if _shutdown_requested:
            ts_print("Server stopped.")
            break

        if disconnected:
            ts_print("Device disconnected. Waiting for reconnect...")
            _calibrate_clock()

        if args.once:
            break

        _time.sleep(2)


if __name__ == "__main__":
    main()
