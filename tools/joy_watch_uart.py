#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""JoyLink 串口帧查看器 —— 把 JoyStick1 的 18 字节二进制帧解析成可读数据。

帧格式（与 APP/JoyLink/JoyLink.hpp 一致）：
    [0..1]  0x55 0xAA
    [2..13] x1 y1 x2 y2 rp1 rp2  (小端，x/y 为 int16 ±1000，rp 为 uint16 0~4095)
    [14]    keys    (bit0=SW4 bit1=SW5 bit2=SW1 bit3=SW2 bit4=SW3)
    [15]    initOk
    [16]    seq     (滚动序号，用来统计丢帧)
    [17]    crc8    (对 [0..16]，多项式 0x07 初值 0x00)

用法：
    pip install pyserial
    python tools/joy_watch_uart.py COM4
    python tools/joy_watch_uart.py COM4 --tcp 8000     # 同时给 VOFA 发 FireWater 文本流
    python tools/joy_watch_uart.py COM4 --hex          # 同时打印原始十六进制帧

VOFA 画曲线：加 --tcp 8000 后，VOFA 的"数据接口"选 TCP 连 127.0.0.1:8000，
"数据引擎"选 FireWater，即可把 x1/y1/x2/y2/rp1/rp2 画成曲线。
"""

import argparse
import socket
import struct
import sys
import threading
import time

try:
    import serial
except ImportError:
    sys.exit("缺少 pyserial，请先执行: pip install pyserial")

FRAME_LEN = 18
HEAD0, HEAD1 = 0x55, 0xAA

KEY_NAMES = ["SW4", "SW5", "SW1", "SW2", "SW3"]  # keys 低位到高位


def crc8(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def parse_frame(buf: bytes):
    """18 字节整帧 -> 字典；帧头/CRC 不过返回 None"""
    if buf[0] != HEAD0 or buf[1] != HEAD1:
        return None
    if crc8(buf[:17]) != buf[17]:
        return None
    x1, y1, x2, y2 = struct.unpack_from("<hhhh", buf, 2)
    rp1, rp2 = struct.unpack_from("<HH", buf, 10)
    keys, init_ok, seq = buf[14], buf[15], buf[16]
    return dict(x1=x1, y1=y1, x2=x2, y2=y2, rp1=rp1, rp2=rp2,
                keys=keys, initOk=init_ok, seq=seq)


def keys_text(keys: int) -> str:
    pressed = [name for i, name in enumerate(KEY_NAMES) if keys >> i & 1]
    return "+".join(pressed) if pressed else "-"


class TcpBroadcaster(threading.Thread):
    """把解析后的数值按 FireWater 文本格式转发给 VOFA（TCP 客户端连进来）"""

    def __init__(self, port: int):
        super().__init__(daemon=True)
        self.lines: list[str] = []
        self.lock = threading.Lock()
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", port))
        self.srv.listen(1)
        self.conn = None
        print(f"[tcp] 等待 VOFA 连接 127.0.0.1:{port} ...")

    def push(self, line: str):
        with self.lock:
            self.lines.append(line)

    def run(self):
        self.conn, addr = self.srv.accept()
        print(f"[tcp] VOFA 已连接: {addr}")
        while True:
            with self.lock:
                batch, self.lines = self.lines, []
            for line in batch:
                try:
                    self.conn.sendall(line.encode())
                except OSError:
                    print("[tcp] VOFA 已断开")
                    return
            time.sleep(0.01)


def main():
    ap = argparse.ArgumentParser(description="JoyLink 串口帧查看器")
    ap.add_argument("port", help="串口号，例如 COM4 或 /dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=921600, help="波特率（默认 921600）")
    ap.add_argument("--tcp", type=int, metavar="PORT", help="开启 FireWater 文本转发端口（给 VOFA 用）")
    ap.add_argument("--hex", action="store_true", help="每帧打印原始十六进制")
    args = ap.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=0.1)

    tcp = None
    if args.tcp:
        tcp = TcpBroadcaster(args.tcp)
        tcp.start()

    stream = bytearray()   # 字节流滑动窗口
    ok = crc_err = lost = 0
    last_seq = None
    last_show = 0.0

    while True:
        stream += ser.read(256)

        # 滑动同步：找帧头，凑够 18 字节就试解析
        while True:
            i = stream.find(bytes((HEAD0, HEAD1)))
            if i < 0:
                stream.clear()               # 连帧头都没有，全丢
                break
            if i > 0:
                del stream[:i]               # 丢掉帧头前的垃圾
            if len(stream) < FRAME_LEN:
                break
            frame = bytes(stream[:FRAME_LEN])
            d = parse_frame(frame)
            if d is None:
                crc_err += 1
                del stream[:2]               # 帧头可能错位，退一字节继续找
                continue
            del stream[:FRAME_LEN]
            ok += 1

            # seq 丢帧统计（256 回绕）
            if last_seq is not None:
                gap = (d["seq"] - last_seq - 1) & 0xFF
                lost += gap
            last_seq = d["seq"]

            if args.hex:
                print(frame.hex(" "))

            if tcp:
                tcp.push(f"{d['x1']},{d['y1']},{d['x2']},{d['y2']},{d['rp1']},{d['rp2']}\n")

            # 20Hz 刷新一行，别刷屏
            now = time.time()
            if now - last_show >= 0.05:
                last_show = now
                sys.stdout.write(
                    f"\rx1={d['x1']:>6} y1={d['y1']:>6}  x2={d['x2']:>6} y2={d['y2']:>6}  "
                    f"rp1={d['rp1']:>4} rp2={d['rp2']:>4}  keys={keys_text(d['keys']):<13} "
                    f"initOk={d['initOk']}  seq={d['seq']:>3}  "
                    f"| 帧 ok={ok} crc错={crc_err} 丢={lost}   "
                )
                sys.stdout.flush()


if __name__ == "__main__":
    main()
