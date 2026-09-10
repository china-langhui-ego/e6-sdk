"""串口时间同步 - 公共模块（传输无关）

NTP 协议常量 + 混合时钟工具 + 日志。从 usb-server-windows/common.py 提取，
不含任何 WinUSB/AOA 代码。串口方案通过 pyserial 访问 COM 口。

本模块分阶段构建：
- T1：NTP 协议常量 + 包打包/解析工具
- T2：NTP 时间戳时钟（_calibrate_clock / ntp_now / now_mono_ns，直读系统时间）+ 日志（ts_print）
"""
import os
import struct
import time as _time

_IS_WINDOWS = os.name == 'nt'

if _IS_WINDOWS:
    import ctypes
    from ctypes import wintypes

# ====== NTP 协议常量 ======
NTP_EPOCH_OFFSET = 2208988800
PKT_MAGIC   = 0xA0
PKT_VERSION = 0x01
PKT_REQ     = 0x01  # NTP 请求（持续时间同步）
PKT_REQ_TEST = 0x04  # NTP 请求（偏移测试，单次）
PKT_SYNC_END = 0x05  # VR→PC：采集会话结束（用户停止同步）
PKT_RESP    = 0x02
PKT_STOP    = 0xFF
PKT_RESULT  = 0x03  # VR→PC 单向结果上报包(不触发NTP响应)
RESULT_SIZE = 24    # 与 C++ 端 RESULT_SIZE 一致
REQ_SIZE    = 24
RESP_SIZE   = 40


# ====== NTP 包工具 ======
def _get_be32(buf: bytes, offset: int) -> int:
    return struct.unpack(">I", buf[offset:offset + 4])[0]

def _get_be64(buf: bytes, offset: int) -> int:
    return struct.unpack(">Q", buf[offset:offset + 8])[0]

def _pack_ntp_response(seq: int, origin_ts: int, recv_ts: int, xmit_ts: int) -> bytes:
    resp = bytearray(RESP_SIZE)
    resp[0] = PKT_MAGIC
    resp[1] = PKT_VERSION
    resp[2] = PKT_RESP
    resp[3] = 0
    struct.pack_into(">I", resp, 4, seq)
    struct.pack_into(">Q", resp, 8, origin_ts)
    struct.pack_into(">Q", resp, 16, recv_ts)
    struct.pack_into(">Q", resp, 24, xmit_ts)
    return bytes(resp)

def _pack_stop() -> bytes:
    """STOP 包：通知 VR 优雅结束同步"""
    pkt = bytearray(RESP_SIZE)
    pkt[0] = PKT_MAGIC
    pkt[1] = PKT_VERSION
    pkt[2] = PKT_STOP
    return bytes(pkt)


# ====== Win32 时钟 DLL（仅 Windows，kernel32，无 WinUSB） ======
if _IS_WINDOWS:
    _kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)


# ====== 日志 ======
def _local_time_str():
    now = _time.time()
    ms = int((now % 1) * 1000)
    tm = _time.localtime(now)
    return f"[{tm.tm_year}-{tm.tm_mon:02d}-{tm.tm_mday:02d} {tm.tm_hour:02d}:{tm.tm_min:02d}:{tm.tm_sec:02d}.{ms:03d}]"

def ts_print(*args, **kwargs):
    print(f"{_local_time_str()}", *args, **kwargs)

def ts_printerr(*args, **kwargs):
    import sys
    print(f"{_local_time_str()}", *args, file=sys.stderr, **kwargs)


# ====== NTP 时钟 ======
_calibrated = False

if _IS_WINDOWS:
    # Windows：ntp_now 直读系统时间（GetSystemTimePreciseAsFileTime，~100ns 精度）。
    # setClock 方案下与 Dart 端 ntp_clock_windows.dart 一致——用户手动改系统时间后
    # 立即反映，不再用"校准基准 + 单调推演"混合时钟（不感知系统时间修改）。
    # now_mono_ns 仍用 QPC 提供单调高精度（超时/批次统计需要）。
    _qpc_freq = 0

    def _calibrate_clock():
        global _qpc_freq, _calibrated
        freq = wintypes.LARGE_INTEGER()
        _kernel32.QueryPerformanceFrequency(ctypes.byref(freq))
        _qpc_freq = freq.value
        if _qpc_freq == 0:
            ts_printerr("QueryPerformanceFrequency returned 0, using fallback 10MHz")
            _qpc_freq = 10_000_000
        _calibrated = True

    def ntp_now():
        if not _calibrated: _calibrate_clock()
        ft = wintypes.FILETIME()
        _kernel32.GetSystemTimePreciseAsFileTime(ctypes.byref(ft))
        uli = (ft.dwHighDateTime << 32) | ft.dwLowDateTime
        unix_100ns = uli - 116444736000000000
        sec = unix_100ns // 10000000
        nsec = (unix_100ns % 10000000) * 100
        ntp_sec = sec + NTP_EPOCH_OFFSET
        ntp_frac = (nsec << 32) // 1_000_000_000
        return (ntp_sec << 32) | ntp_frac

    def now_mono_ns():
        if not _calibrated: _calibrate_clock()
        counter = wintypes.LARGE_INTEGER()
        _kernel32.QueryPerformanceCounter(ctypes.byref(counter))
        s = counter.value // _qpc_freq; r = counter.value % _qpc_freq
        return s * 1_000_000_000 + int(r * 1_000_000_000 / _qpc_freq)

else:
    # Linux：直接用 clock_gettime（CLOCK_REALTIME / CLOCK_MONOTONIC），纳秒精度
    def _calibrate_clock():
        global _calibrated
        _calibrated = True  # Linux 无需校准，clock_gettime 自带绝对时基

    def ntp_now():
        ns = _time.clock_gettime_ns(_time.CLOCK_REALTIME)
        sec = ns // 1_000_000_000
        nsec = ns % 1_000_000_000
        ntp_sec = sec + NTP_EPOCH_OFFSET
        ntp_frac = (nsec << 32) // 1_000_000_000
        return (ntp_sec << 32) | ntp_frac

    def now_mono_ns():
        return _time.clock_gettime_ns(_time.CLOCK_MONOTONIC)
