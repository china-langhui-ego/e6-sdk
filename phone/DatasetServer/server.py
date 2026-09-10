#!/usr/bin/env python3
"""
DatasetServer — XR 数据集分片上传服务

接收 VR 头显通过 HTTP multipart 上传的录制分片。
每个分片是一个完整的子目录（mp4 + csv + json），上传后保存到 received_chunks/。

启动方式:
    python server.py [--port PORT] [--dir SAVE_DIR]

默认端口: 9000
默认存储: ./dataset/
"""

import argparse
import json
import os
import socket
import sys
import time
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler

# ============================================================
# Configuration
# ============================================================

DEFAULT_PORT = 9000
DEFAULT_SAVE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dataset")
# 单次 socket 读取的空闲超时秒数。客户端中途断开（如 VR 关机掉电，不发 TCP FIN）
# 或传输长时间停滞时，读取会抛出 socket.timeout，使请求线程退出，避免被死连接
# 永久阻塞。这是"空闲超时"而非"总时长"——只要持续有数据到达就会不断重置，
# 不影响大文件的慢速上传。
DEFAULT_RECEIVE_TIMEOUT = 60

# ============================================================
# Utility: list local IPs
# ============================================================

def get_local_ips():
    """Return a list of (interface_name, ip) tuples."""
    ips = []

    # Method 1: gethostname → getaddrinfo (works on Windows, some Linux configs)
    try:
        hostname = socket.gethostname()
        for family in (socket.AF_INET,):
            try:
                addrs = socket.getaddrinfo(hostname, None, family)
                for addr in addrs:
                    ip = addr[4][0]
                    if not ip.startswith("127."):
                        ips.append(("", ip))
            except socket.gaierror:
                continue
    except Exception:
        pass

    # Method 2: UDP connect trick — ask the kernel which IP it would use
    # to reach an external address. This is the reliable fallback for Linux
    # where /etc/hosts maps hostname → 127.0.1.1 by default.
    if not ips:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.settimeout(0)
            # 10.254.254.254 is a TEST-NET address (RFC 5737) — unreachable,
            # but UDP connect() doesn't send packets; it just binds the socket
            # to the local IP that would be used for that route.
            s.connect(("10.254.254.254", 1))
            ip = s.getsockname()[0]
            s.close()
            if ip and not ip.startswith("127."):
                ips.append(("", ip))
        except Exception:
            pass

    # Deduplicate by IP
    seen = set()
    unique = []
    for name, ip in ips:
        if ip not in seen:
            seen.add(ip)
            unique.append((name, ip))
    return unique


# ============================================================
# Multipart parser
# ============================================================

class MultipartParser:
    """Minimal multipart/form-data parser for chunk uploads."""

    def __init__(self, boundary: str):
        self.boundary = boundary.encode("utf-8")
        self.delim = b"--" + self.boundary
        self.end_delim = b"--" + self.boundary + b"--"

    def parse(self, data: bytes) -> dict:
        """Parse multipart body. Returns {"chunk_name": str, "files": [(filename, bytes), ...]}."""
        result = {"chunk_name": "", "files": []}

        # Split by boundary delimiter, skip first (preamble) and last (epilogue)
        parts = data.split(self.delim + b"\r\n")
        for part in parts:
            if not part or part.startswith(b"--"):
                continue
            # Strip trailing \r\n--boundary suffix if present
            idx = part.find(b"\r\n" + self.delim)
            if idx > 0:
                part = part[:idx]

            # Separate headers and body
            header_end = part.find(b"\r\n\r\n")
            if header_end < 0:
                continue
            headers_section = part[:header_end].decode("utf-8", errors="replace")
            body = part[header_end + 4:]

            # Remove trailing \r\n
            if body.endswith(b"\r\n"):
                body = body[:-2]

            # Parse Content-Disposition
            name = None
            filename = None
            for line in headers_section.split("\r\n"):
                line_lower = line.lower()
                if line_lower.startswith("content-disposition:"):
                    # Extract name= and filename=
                    for segment in line.split(";"):
                        segment = segment.strip()
                        if segment.startswith("name="):
                            name = segment[5:].strip().strip('"')
                        elif segment.startswith("filename="):
                            filename = segment[9:].strip().strip('"')

            if name == "chunk_name":
                result["chunk_name"] = body.decode("utf-8", errors="replace").strip()
            elif name == "files" and filename:
                result["files"].append((filename, body))

        return result


# ============================================================
# HTTP Request Handler
# ============================================================

class DatasetHandler(BaseHTTPRequestHandler):
    server_start_time = time.time()
    received_chunks_count = 0
    # StreamRequestHandler.setup() 会据此对连接 socket 调用 settimeout()。
    # 被 main() 用 --timeout 参数覆盖。
    timeout = DEFAULT_RECEIVE_TIMEOUT

    def log_message(self, format, *args):
        """Suppress default access log — we print custom logs instead."""
        pass

    # --- CORS helpers ---
    def _set_cors(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")

    def _send_json(self, status: int, data: dict):
        self.send_response(status)
        self._set_cors()
        self.send_header("Content-Type", "application/json; charset=utf-8")
        body = json.dumps(data, ensure_ascii=False, indent=2)
        self.send_header("Content-Length", str(len(body.encode("utf-8"))))
        self.end_headers()
        self.wfile.write(body.encode("utf-8"))

    def _send_text(self, status: int, text: str):
        self.send_response(status)
        self._set_cors()
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(text.encode("utf-8"))))
        self.end_headers()
        self.wfile.write(text.encode("utf-8"))

    # --- Routing ---
    def do_OPTIONS(self):
        self.send_response(204)
        self._set_cors()
        self.end_headers()

    def do_GET(self):
        if self.path == "/" or self.path == "":
            self._serve_welcome()
        elif self.path == "/api/health":
            self._serve_health()
        else:
            self._send_json(404, {"error": "not found"})

    def do_POST(self):
        if self.path == "/api/upload":
            self._handle_upload()
        else:
            self._send_json(404, {"error": "not found"})

    # --- Handlers ---
    def _serve_welcome(self):
        ips = get_local_ips()
        lines = [
            "=" * 50,
            "  DatasetServer - XR 数据集上传服务",
            "=" * 50,
            "",
            "  分片上传地址:",
        ]
        if not ips:
            lines.append(f"    http://localhost:{self.server.port}")
        else:
            for name, ip in ips:
                label = f" ({name})" if name else ""
                lines.append(f"    http://{ip}:{self.server.port}{label}")
        lines += [
            "",
            "  API 接口:",
            "    POST /api/upload   上传分片 (multipart/form-data)",
            "    GET  /api/health   健康检查",
            "    GET  /             API 文档 (此页)",
            "",
            "  存储目录: ./dataset/",
            "",
            "  --- 上传 API 说明 ---",
            "",
            "  POST /api/upload",
            "  Content-Type: multipart/form-data",
            "",
            "  字段:",
            "    chunk_name  (string)          分片目录名，如 SN_20260630_143000",
            "    files       (file, 可多个)     分片内的文件",
            "",
            "  示例 (curl):",
            "    curl -F \"chunk_name=SN_20260630_143000\" \\",
            "         -F \"files=@rgb.mp4\" \\",
            "         -F \"files=@tracking.mp4\" \\",
            "         http://HOST:PORT/api/upload",
            "",
            "  响应:",
            '    {"status":"ok","chunk_name":"...","file_count":14,"total_bytes":12345}',
            "",
            "=" * 50,
        ]
        text = "\n".join(lines)
        self._send_text(200, text)

    def _serve_health(self):
        uptime = int(time.time() - self.server_start_time)
        self._send_json(200, {
            "status": "ok",
            "received_chunks": DatasetHandler.received_chunks_count,
            "uptime_seconds": uptime,
        })

    def _read_chunked_body(self):
        """Read HTTP chunked transfer-encoded body from self.rfile.
        Returns the decoded body as bytes.
        Raises ValueError on malformed chunked data.
        """
        body = bytearray()
        while True:
            line = self.rfile.readline()
            if not line:
                break
            # Chunk size is hex, optionally followed by ';' and chunk extensions
            chunk_size_str = line.split(b';')[0].strip()
            try:
                chunk_size = int(chunk_size_str, 16)
            except ValueError:
                raise ValueError(f"Invalid chunk size: {chunk_size_str!r}")
            body.extend(self.rfile.read(chunk_size))
            # Read trailing CRLF after chunk data
            self.rfile.readline()
            if chunk_size == 0:
                break
        return bytes(body)

    def _read_body_with_progress(self, content_length, upload_start):
        """Read fixed-length body with progress display. Returns bytes."""
        body = bytearray()
        remaining = content_length
        report_interval = max(content_length // 20, 1024 * 1024)  # every 5% or at least 1MB
        next_report = report_interval
        while remaining > 0:
            chunk_size = min(65536, remaining)
            chunk = self.rfile.read(chunk_size)
            if not chunk:
                break
            body.extend(chunk)
            remaining -= len(chunk)
            if len(body) >= next_report:
                pct = len(body) * 100 // content_length
                mb_done = len(body) / (1024 * 1024)
                mb_total = content_length / (1024 * 1024)
                elapsed = time.time() - upload_start
                speed = (len(body) / (1024 * 1024)) / elapsed if elapsed > 0 else 0
                eta = (content_length - len(body)) / (speed * 1024 * 1024) if speed > 0 else 0
                print(f"\r  [{pct:3d}%] {mb_done:.0f}/{mb_total:.0f} MB | {speed:.1f} MB/s | ETA {eta:.0f}s", end="", flush=True)
                next_report = ((len(body) // report_interval) + 1) * report_interval
        return bytes(body)

    def _handle_upload(self):
        content_type = self.headers.get("Content-Type", "")
        if "multipart/form-data" not in content_type:
            self._send_json(400, {"error": "Content-Type must be multipart/form-data"})
            return

        # Extract boundary
        boundary = None
        for part in content_type.split(";"):
            part = part.strip()
            if part.lower().startswith("boundary="):
                boundary = part[9:].strip().strip('"')
        if not boundary:
            self._send_json(400, {"error": "missing boundary in Content-Type"})
            return

        # Read body — support both chunked and fixed-length transfer encodings
        upload_start = time.time()
        transfer_encoding = self.headers.get("Transfer-Encoding", "").lower()
        content_length = int(self.headers.get("Content-Length", 0))

        try:
            if "chunked" in transfer_encoding:
                # Chunked transfer encoding (no Content-Length)
                print(f"\r  接收中 (chunked)...", end="", flush=True)
                body = self._read_chunked_body()
                elapsed = time.time() - upload_start
                body_mb = len(body) / (1024 * 1024)
                speed = body_mb / elapsed if elapsed > 0 else 0
                print(f"\r  [done] {body_mb:.1f} MB | {speed:.1f} MB/s")
            elif content_length > 0:
                # Standard fixed-length (backward compatible with old client)
                if content_length > 5 * 1024 * 1024 * 1024:
                    self._send_json(413, {"error": "payload too large (>5GB)"})
                    return
                body = self._read_body_with_progress(content_length, upload_start)
            else:
                self._send_json(400, {"error": "missing Content-Length or Transfer-Encoding: chunked"})
                return
        except OSError as e:
            # 客户端中途断开（如 VR 关机掉电，不发 FIN）或传输长时间停滞：socket 读取
            # 抛出 socket.timeout / ConnectionError（均为 OSError 子类）。此时 body
            # 尚未读完，不会进入后续落盘逻辑（无残留文件）。放弃本次上传，关闭连接，
            # 让请求线程退出——配合下面的 ThreadingHTTPServer，单个死连接不再拖垮整个服务。
            print()  # 换行，结束未完成的进度行
            ts = time.strftime("%H:%M:%S")
            print(f"[{ts}] [中断] 上传中断：客户端断开或传输超时 ({type(e).__name__})，已丢弃本次分片")
            self.close_connection = True
            return

        # Parse multipart
        parser = MultipartParser(boundary)
        try:
            result = parser.parse(body)
        except Exception as e:
            self._send_json(400, {"error": f"multipart parse error: {e}"})
            return

        chunk_name = result.get("chunk_name", "")
        files = result.get("files", [])

        if not chunk_name:
            self._send_json(400, {"error": "missing chunk_name field"})
            return
        if not files:
            self._send_json(400, {"error": "no files uploaded"})
            return

        # Security: sanitize chunk_name (prevent path traversal)
        chunk_name = os.path.basename(chunk_name)
        if not chunk_name:
            self._send_json(400, {"error": "invalid chunk_name"})
            return

        # Save files
        chunk_dir = os.path.join(self.server.save_dir, chunk_name)
        os.makedirs(chunk_dir, exist_ok=True)

        total_bytes = 0
        saved_count = 0
        ts = time.strftime("%H:%M:%S")
        print(f"[{ts}] {chunk_name}")
        for filename, file_data in files:
            # filename 可含相对子目录（如 "cam0/video.mp4"），保留层级落盘。
            # 安全处理：统一分隔符、剥离开头分隔符（禁止绝对路径）、剔除 "."/".." 与空段，
            # 从根上阻断路径穿越（../ 或绝对路径写入到 chunk_dir 之外）。
            rel = filename.replace("\\", "/").lstrip("/")
            parts = [p for p in rel.split("/") if p not in ("", ".", "..")]
            if not parts:
                continue
            safe_rel = os.path.join(*parts)
            filepath = os.path.join(chunk_dir, safe_rel)
            # 再次确认解析后的绝对路径仍在 chunk_dir 之内（防御性双重检查）
            if not os.path.abspath(filepath).startswith(
                    os.path.abspath(chunk_dir) + os.sep):
                continue
            os.makedirs(os.path.dirname(filepath), exist_ok=True)
            with open(filepath, "wb") as f:
                f.write(file_data)
            size_kb = len(file_data) / 1024
            if size_kb >= 1024:
                size_str = f"{size_kb / 1024:.1f} MB"
            else:
                size_str = f"{size_kb:.1f} KB"
            total_bytes += len(file_data)
            saved_count += 1
            print(f"[{ts}]   + {safe_rel}  ({size_str})")

        total_mb = total_bytes / (1024 * 1024)
        elapsed = time.time() - upload_start
        DatasetHandler.received_chunks_count += 1
        print(f"[{ts}] OK {chunk_name} 完成 — {saved_count} 文件, {total_mb:.1f} MB, 耗时 {elapsed:.1f}s")

        try:
            self._send_json(200, {
                "status": "ok",
                "chunk_name": chunk_name,
                "file_count": saved_count,
                "total_bytes": total_bytes,
            })
        except OSError:
            # 分片已成功落盘，但客户端在收到响应前断开（如上传完即关机）。
            # 数据已保存，无需特殊处理；仅记录并关闭连接。
            ts = time.strftime("%H:%M:%S")
            print(f"[{ts}] [警告] 响应发送失败：客户端已断开（分片数据已成功保存）")
            self.close_connection = True


# ============================================================
# Main
# ============================================================

def main():
    parser = argparse.ArgumentParser(description="XR Dataset Upload Server")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT,
                        help=f"HTTP listen port (default: {DEFAULT_PORT})")
    parser.add_argument("--dir", type=str, default=DEFAULT_SAVE_DIR,
                        help=f"Save directory (default: {DEFAULT_SAVE_DIR})")
    parser.add_argument("--timeout", type=int, default=DEFAULT_RECEIVE_TIMEOUT,
                        help=f"Socket receive idle timeout in seconds; aborts uploads "
                             f"whose client dies mid-transfer (default: {DEFAULT_RECEIVE_TIMEOUT})")
    args = parser.parse_args()

    # Ensure save directory exists
    os.makedirs(args.dir, exist_ok=True)

    # Apply per-connection read timeout (StreamRequestHandler.setup reads this)
    DatasetHandler.timeout = args.timeout

    # ThreadingHTTPServer: each upload runs in its own (daemon) thread, so one
    # dead/slow client cannot block health checks or other uploads.
    server = ThreadingHTTPServer(("0.0.0.0", args.port), DatasetHandler)
    server.port = args.port
    server.save_dir = args.dir

    # Print startup info
    print()
    print("=" * 50)
    print("  DatasetServer - XR 数据集上传服务")
    print("=" * 50)
    print()
    print("  分片上传地址:")
    ips = get_local_ips()
    if ips:
        for name, ip in ips:
            label = f" ({name})" if name else ""
            print(f"    http://{ip}:{args.port}{label}")
    else:
        print(f"    http://localhost:{args.port}")
    print()
    print(f"  接口文档: http://127.0.0.1:{args.port}/")
    print(f"  健康检查: GET  /api/health")
    print(f"  存储目录: ./dataset/")
    print()
    print("=" * 50)
    print()

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nServer stopped.")


if __name__ == "__main__":
    main()
