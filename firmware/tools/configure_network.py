"""生成编译配置或经 USB 更新设备网络参数，不在输出中显示密码。"""

import argparse
import getpass
import importlib
import ipaddress
import json
from pathlib import Path
import socket
import secrets
import time


ROOT = Path(__file__).resolve().parent.parent


def detect_ip():
    """通过默认路由查询本机地址，不发送应用数据。"""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.connect(("192.0.2.1", 80))
        return sock.getsockname()[0]


def c_string(value):
    """按 UTF-8 字节生成 C 字符串，兼容中文 SSID。"""
    return '"' + ''.join(f"\\{byte:03o}" for byte in value.encode("utf-8")) + '"'


def validate_config(config):
    """统一检查编译配置和运行时配置的字段与字节长度。"""
    if not isinstance(config, dict):
        raise ValueError("配置必须是 JSON 对象")
    ssid = config["wifi_ssid"]
    password = config["wifi_password"]
    if not isinstance(ssid, str) or not 1 <= len(ssid.encode("utf-8")) <= 32:
        raise ValueError("SSID 必须为 1～32 个 UTF-8 字节")
    if not isinstance(password, str) or len(password.encode("utf-8")) > 64:
        raise ValueError("密码不得超过 64 个 UTF-8 字节")
    if any(char in ssid + password for char in '\r\n\0'):
        raise ValueError("SSID 和密码不能包含换行或空字节")
    address = str(ipaddress.IPv4Address(config["server_ip"]))
    port = config["server_port"]
    if type(port) is not int or not 1 <= port <= 65535:
        raise ValueError("端口必须为 1～65535")
    return {"wifi_ssid": ssid, "wifi_password": password,
            "server_ip": address, "server_port": port}


def generate(config, destination):
    """保留原有头文件生成入口，构建时不需要串口依赖。"""
    config = validate_config(config)
    ssid, password = config["wifi_ssid"], config["wifi_password"]
    address, port = config["server_ip"], config["server_port"]
    text = ("/* 本地生成的网络配置，请勿提交 Git。 */\n#pragma once\n"
            f"#define NETWORK_WIFI_SSID {c_string(ssid)}\n"
            f"#define NETWORK_WIFI_PASSWORD {c_string(password)}\n"
            f"#define NETWORK_SERVER_IP {c_string(address)}\n"
            f"#define NETWORK_SERVER_PORT {port}U\n")
    destination.write_text(text, encoding="utf-8")


class ConfigurationError(Exception):
    """仅包含可公开的配网错误，不携带配置帧或底层异常。"""


def encode_frame(config, request_id):
    """编码有界配置帧；空密码使用横线占位。"""
    config = validate_config(config)
    if len(request_id) != 8 or any(char not in "0123456789abcdef" for char in request_id):
        raise ValueError("请求标识无效")
    ssid = config["wifi_ssid"].encode("utf-8").hex()
    password = config["wifi_password"].encode("utf-8").hex() or "-"
    return (f"NETCFG1 SET {request_id} {ssid} {password} "
            f"{config['server_ip']} {config['server_port']}\n").encode("ascii")


def apply_to_serial(serial_port, config, request_id, report=print):
    """只认可本次请求的保存应答和生效后的上传确认。"""
    serial_port.write(b"\n" + encode_frame(config, request_id))
    serial_port.flush()
    deadline = time.monotonic() + 60
    saved = False
    pending = bytearray()
    while time.monotonic() < deadline:
        pending.extend(serial_port.readline())
        while b"\n" in pending:
            raw, _, remaining = pending.partition(b"\n")
            pending = bytearray(remaining)
            line = raw.decode("ascii", errors="replace").strip()
            if line == f"NETCFG1 SAVED {request_id}":
                if not saved:
                    report("配置保存成功，正在连接新网络；这尚不代表上传成功。")
                    saved = True
                    deadline = time.monotonic() + 60
            elif saved and line == f"NETCFG1 UPLOADED {request_id}":
                report("收到 HTTP 204 上传确认：新配置已生效，接收服务已保存数据。")
                return True
            elif line.startswith(f"NETCFG1 ERROR {request_id} "):
                reason = line.rsplit(" ", 1)[-1]
                message = {"FLASH": "设备配置写入失败，上一份配置仍保留。",
                           "INVALID": "设备拒绝了非法配置。", "BUSY": "设备正在处理另一份配置。"}
                raise ConfigurationError(message.get(reason, "设备拒绝了配置，请重新下发。"))
        if len(pending) > 1024:
            pending.clear()
    if saved:
        report("配置已保存，但未收到 HTTP 204；请检查 Wi-Fi 参数、电脑地址和接收服务。")
        return False
    raise ConfigurationError("未收到配置保存确认；请检查串口、固件版本及设备运行状态。")


def apply_configuration(config, port):
    """按需加载 pyserial，使用现有 ST-LINK 虚拟串口。"""
    config = validate_config(config)
    try:
        serial = importlib.import_module("serial")
    except ImportError:
        raise ConfigurationError("USB 配网需要 pyserial，请运行：python -m pip install pyserial") from None
    try:
        with serial.Serial(port, 115200, timeout=0.5, write_timeout=5,
                           rtscts=False, dsrdtr=False) as serial_port:
            serial_port.reset_input_buffer()
            request_id = secrets.token_hex(4)
            prepare_serial(serial_port, request_id)
            return apply_to_serial(serial_port, config, request_id)
    except (serial.SerialException, OSError):
        raise ConfigurationError("串口打开或通信失败，请检查 COM 端口并关闭占用它的串口助手。") from None


def prepare_serial(serial_port, request_id):
    """先握手确认接收就绪；残帧被丢弃时允许再同步一次。"""
    for _ in range(2):
        serial_port.write(f"\nNETCFG1 HELLO {request_id}\n".encode("ascii"))
        serial_port.flush()
        deadline = time.monotonic() + 3
        pending = bytearray()
        while time.monotonic() < deadline:
            pending.extend(serial_port.readline())
            while b"\n" in pending:
                raw, _, remaining = pending.partition(b"\n")
                pending = bytearray(remaining)
                if raw.strip() == f"NETCFG1 READY {request_id}".encode("ascii"):
                    return
            if len(pending) > 1024:
                pending.clear()
    raise ConfigurationError("设备未响应 USB 配置握手；请确认新固件已烧录、设备已启动及串口正确。")


def run_wizard(path, port):
    """交互配置并明确确认保存和下发；密码始终隐藏输入。"""
    try:
        address = detect_ip()
    except OSError:
        address = ""
    ssid = input("2.4GHz Wi-Fi 名称：")
    password = getpass.getpass("Wi-Fi 密码（开放网络直接回车）：")
    address = input(f"电脑接收地址 [{address}]：").strip() or address
    port_text = input("接收服务端口 [8080]：").strip() or "8080"
    config = validate_config({"wifi_ssid": ssid, "wifi_password": password,
                              "server_ip": address, "server_port": int(port_text)})
    print(f"将通过 {port} 更新设备，接收地址 http://{address}:{config['server_port']}/；密码不会显示。")
    if input("保存到本地配置并下发？[y/N]：").strip().lower() != "y":
        print("已取消，未保存或下发。")
        return False
    path.write_text(json.dumps(config, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return apply_configuration(config, port)


def main():
    parser = argparse.ArgumentParser(description="生成 STM32 本地网络配置")
    actions = parser.add_mutually_exclusive_group()
    actions.add_argument("--init", action="store_true", help="首次生成本地 JSON，自动填写电脑 IP")
    actions.add_argument("--apply", action="store_true", help="通过 USB 下发现有本地 JSON，无需重新烧录")
    actions.add_argument("--wizard", action="store_true", help="交互填写、保存并通过 USB 下发配置")
    parser.add_argument("--port", default="COM5", help="ST-LINK 虚拟串口，默认 COM5")
    args = parser.parse_args()
    path = ROOT / "config" / "network.local.json"
    if args.init:
        if path.exists():
            raise SystemExit("本地配置已存在，未覆盖。")
        config = {"wifi_ssid": "", "wifi_password": "", "server_ip": detect_ip(), "server_port": 8080}
        path.write_text(json.dumps(config, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(f"已生成 {path}，请填写 Wi-Fi 名称及密码，再运行本脚本。")
        return
    try:
        if args.wizard:
            if not run_wizard(path, args.port):
                raise SystemExit(1)
            return
        config = json.loads(path.read_text(encoding="utf-8-sig"))
        if args.apply:
            if not apply_configuration(config, args.port):
                raise SystemExit(1)
            return
        generate(config, ROOT / "Core" / "Inc" / "network_config.local.h")
    except ConfigurationError as error:
        raise SystemExit(str(error)) from None
    except (KeyboardInterrupt, EOFError):
        raise SystemExit("配置操作已取消。") from None
    except (OSError, ValueError, KeyError, TypeError):
        raise SystemExit("配置读取失败，请检查本地 JSON 的字段、地址与长度；密码不会打印。")
    print(f"固件配置已生成，接收地址 http://{config['server_ip']}:{config['server_port']}/")


if __name__ == "__main__":
    main()
