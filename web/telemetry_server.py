"""接收 STM32 的 USB 或局域网遥测，保存日志并提供只读页面。"""

import argparse
from collections import deque
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import threading
import time


def validate_telemetry(data):
    """检查固件上传字段，拒绝无效温度和不一致的传感器状态。"""
    fields = {"device_id", "uptime_ms", "temperature_centi", "sensor_status",
              "pump_enabled", "valve_enabled"}
    if not isinstance(data, dict) or set(data) not in (fields, fields | {"measurement"}):
        raise ValueError("字段不完整或包含未知字段")
    if not isinstance(data["device_id"], str) or not 1 <= len(data["device_id"]) <= 64:
        raise ValueError("设备标识无效")
    if type(data["uptime_ms"]) is not int or not 0 <= data["uptime_ms"] <= 0xFFFFFFFF:
        raise ValueError("运行时间无效")
    if data["sensor_status"] not in ("ok", "not_found", "crc_error"):
        raise ValueError("传感器状态无效")
    temperature = data["temperature_centi"]
    if data["sensor_status"] == "ok":
        if type(temperature) is not int or not -5500 <= temperature <= 12500:
            raise ValueError("温度无效")
    elif temperature is not None:
        raise ValueError("传感器故障时温度必须为空")
    if any(type(data[key]) is not bool for key in ("pump_enabled", "valve_enabled")):
        raise ValueError("控制状态必须为布尔值")
    if "measurement" in data:
        measurement = data["measurement"]
        keys = {"stage", "error", "upper_wet", "lower_wet", "upper_triggered",
                "lower_triggered", "elapsed_ms", "result_ms"}
        if not isinstance(measurement, dict) or set(measurement) != keys:
            raise ValueError("测量字段不完整")
        if measurement["stage"] not in ("idle", "suction", "extra_suction", "draining",
                                        "timing", "complete", "aborted", "fault"):
            raise ValueError("测量阶段无效")
        if measurement["error"] not in ("none", "upper_wet", "invalid_order", "edge_overflow",
                                        "suction_timeout", "upper_timeout", "lower_timeout", "unstable_input"):
            raise ValueError("测量故障无效")
        if any(type(measurement[key]) is not bool for key in
               ("upper_wet", "lower_wet", "upper_triggered", "lower_triggered")):
            raise ValueError("液位与触发状态必须为布尔值")
        if type(measurement["elapsed_ms"]) is not int or not 0 <= measurement["elapsed_ms"] <= 0xFFFFFFFF:
            raise ValueError("计时值无效")
        result = measurement["result_ms"]
        if result is not None and (type(result) is not int or not 0 <= result <= 0xFFFFFFFF or
                                   not measurement["lower_triggered"]):
            raise ValueError("测量结果无效")
        if measurement["lower_triggered"] and not measurement["upper_triggered"]:
            raise ValueError("计时点触发顺序无效")
        if measurement["stage"] == "complete" and result is None:
            raise ValueError("完成状态缺少结果")
    return dict(data)


class TelemetryServer(ThreadingHTTPServer):
    """保存最新状态和最近一小时曲线，完整记录追加到本地 JSONL。"""

    daemon_threads = True

    def __init__(self, address, log_path, source="network"):
        super().__init__(address, TelemetryHandler)
        self.log_path = Path(log_path)
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        self.latest = None
        self.source = source
        self.received_monotonic = None
        self.lock = threading.Lock()
        self.history = deque(maxlen=7200)
        # 启动只读取日志尾部，避免长期日志越积越大后阻塞启动。
        if self.log_path.exists():
            with self.log_path.open("rb") as stream:
                start = max(0, stream.seek(0, 2) - 8 * 1024 * 1024)
                stream.seek(start)
                if start:
                    stream.readline()
                for line in stream:
                    try:
                        record = json.loads(line)
                        validate_telemetry({k: v for k, v in record.items() if k != "received_at"})
                        self._append_history(record)
                    except (ValueError, TypeError, KeyError, AttributeError):
                        continue

    def _append_history(self, record):
        """保留空温度采样作为曲线断点，不用设备运行时长充当日期。"""
        received = datetime.fromisoformat(record["received_at"])
        if received.tzinfo is None:
            raise ValueError("接收时间缺少时区")
        timestamp = received.timestamp()
        if timestamp >= time.time() - 3600:
            point = {key: record[key] for key in
                     ("device_id", "received_at", "uptime_ms", "temperature_centi", "sensor_status")}
            self.history.append((timestamp, point))

    def record(self, data):
        record = {**data, "received_at": datetime.now(timezone.utc).isoformat()}
        with self.lock:
            with self.log_path.open("a+b") as stream:
                if stream.tell():
                    stream.seek(-1, 2)
                    if stream.read(1) != b"\n":
                        stream.write(b"\n")
                stream.write((json.dumps(record, ensure_ascii=False) + "\n").encode("utf-8"))
            self._append_history(record)
            self.latest = record
            self.received_monotonic = time.monotonic()

    def snapshot(self):
        with self.lock:
            age = None if self.latest is None else time.monotonic() - self.received_monotonic
            return {"data": self.latest, "age_seconds": age,
                    "stale": age is None or age > 15, "source": self.source}

    def history_snapshot(self):
        """返回实际接收的最近一小时数据；不会为重复网页刷新新增采样。"""
        with self.lock:
            cutoff = time.time() - 3600
            while self.history and self.history[0][0] < cutoff:
                self.history.popleft()
            return {"window_seconds": 3600,
                    "points": [point for timestamp, point in self.history if timestamp >= cutoff]}


class TelemetryHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def setup(self):
        super().setup()
        self.connection.settimeout(5)

    def reply(self, status, body=b"", content_type="application/json; charset=utf-8"):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        if body:
            self.wfile.write(body)
        self.close_connection = True

    def do_GET(self):
        if self.path == "/":
            page = Path(__file__).with_name("dashboard.html").read_bytes()
            self.reply(200, page, "text/html; charset=utf-8")
        elif self.path == "/api/latest":
            self.reply(200, json.dumps(self.server.snapshot(), ensure_ascii=False).encode("utf-8"))
        elif self.path == "/api/history":
            self.reply(200, json.dumps(self.server.history_snapshot(), ensure_ascii=False).encode("utf-8"))
        else:
            self.reply(404)

    def do_POST(self):
        if self.path != "/api/telemetry":
            self.reply(404)
            return
        try:
            if self.headers.get("Transfer-Encoding") is not None:
                raise ValueError("不支持分块请求")
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= 1024:
                raise ValueError("请求长度无效")
            body = self.rfile.read(length)
            if len(body) != length:
                raise ValueError("请求未完成")
            data = validate_telemetry(json.loads(body))
        except (ValueError, TypeError, TimeoutError):
            self.reply(400, b'{"error":"invalid telemetry"}')
            return
        try:
            self.server.record(data)
        except OSError:
            self.reply(500, b'{"error":"log write failed"}')
            return
        self.reply(204)


def receive_serial_line(server, line):
    """只保存完整遥测帧，忽略普通调试日志、残帧及非法数据。"""
    if not line.startswith(b"Telemetry: ") or len(line) > 1024:
        return False
    try:
        data = validate_telemetry(json.loads(line[11:]))
    except (ValueError, TypeError):
        return False
    server.record(data)
    return True


def receive_serial(server, serial_module, port, stop):
    """串口断开后自动重连，网页通过原有过期标记显示中断。"""
    while not stop.is_set():
        try:
            with serial_module.Serial(port, 115200, timeout=.5) as connection:
                print(f"USB 接收已连接：{port}", flush=True)
                pending = bytearray()
                discard = False
                while not stop.is_set():
                    pending.extend(connection.read(min(connection.in_waiting or 1, 1024)))
                    while b"\n" in pending:
                        line, _, rest = pending.partition(b"\n")
                        pending = bytearray(rest)
                        if not discard:
                            try:
                                receive_serial_line(server, line.rstrip(b"\r"))
                            except OSError:
                                print("USB 数据日志写入失败，本条未保存。", flush=True)
                        discard = False
                    if len(pending) > 1024:
                        pending.clear()
                        discard = True
        except (serial_module.SerialException, OSError):
            print(f"USB 串口 {port} 暂不可用，等待重新连接。", flush=True)
            stop.wait(3)


def main():
    parser = argparse.ArgumentParser(description="STM32 温度与液位接收服务")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--log", type=Path, default=Path(__file__).parent.parent / "logs" / "telemetry.jsonl")
    parser.add_argument("--serial", help="经 USB 接收，例如 COM5；不依赖 Wi-Fi")
    args = parser.parse_args()
    if args.serial:
        try:
            import serial
        except ImportError:
            parser.error("USB 接收需要 pyserial，请在独立 Python 环境安装后启动")
    with TelemetryServer((args.host, args.port), args.log,
                         "usb" if args.serial else "network") as server:
        stop = threading.Event()
        receiver = None
        if args.serial:
            receiver = threading.Thread(target=receive_serial,
                                        args=(server, serial, args.serial, stop), daemon=True)
            receiver.start()
        print(f"接收服务已启动：http://127.0.0.1:{args.port}/", flush=True)
        print("手机请访问 http://电脑局域网IP:端口/；按 Ctrl+C 停止。", flush=True)
        print(f"日志：{args.log.resolve()}", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
        finally:
            stop.set()
            if receiver:
                receiver.join(timeout=2)


if __name__ == "__main__":
    main()
