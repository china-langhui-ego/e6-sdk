"""按 VID-PID 自动查找 VR 串口（CDC-ACM）对应的 COM 端口。

VR 在 sys.usb.config="mtp,acm,adb" 下枚举为 VID_18D1 PID_903B 复合设备，
ACM interface 被 Windows usbser.sys 识别为 COM 口。
"""
from serial.tools.list_ports import comports

VR_VID = 0x18D1
VR_PID = 0x903B


def find_vr_serial_port():
    """返回匹配 VR 的 COM 端口名（如 "COM3"），未找到返回 None。"""
    matches = [p for p in comports() if p.vid == VR_VID and p.pid == VR_PID]
    if not matches:
        return None
    if len(matches) == 1:
        return matches[0].device
    # 多端口（理论不应发生，acm 单 COM）：优先选描述/interface 含 acm/serial 的
    for p in matches:
        desc = f"{p.description or ''} {getattr(p, 'interface', '') or ''}".lower()
        if "acm" in desc or "serial" in desc:
            return p.device
    return matches[0].device
