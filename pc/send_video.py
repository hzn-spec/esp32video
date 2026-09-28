#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把视频 / 摄像头画面转成 1bpp 推到 ESP32-S3 的 SSD1306 上播放。

为什么要电脑来干活：
    ESP32 解不了真视频，而且屏幕只有 128x64 单色。所以缩放、抖动这些重活
    全部在这里做完，ESP32 只收成品帧（1024 字节）写进 I2C。

用法示例：
    python send_video.py --host 10.144.179.174 --source test
    python send_video.py --host 10.144.179.174 --source movie.mp4 --loop
    python send_video.py --host 10.144.179.174 --source 0            # 摄像头
    python send_video.py --host 10.144.179.174 --source "text:hello" # 只发一行字
"""

import argparse
import socket
import struct
import sys
import time

import cv2
import numpy as np
from PIL import Image, ImageOps

# 有些机器的终端代码页编码不了中文，会让 print 直接抛异常；
# 这里只放宽错误处理，不强行改编码，免得反而变乱码。
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(errors="replace")
    except Exception:                                   # pragma: no cover
        pass

W, H = 128, 64
MAGIC = b"\xA5\x5A"
MSG_FRAME, MSG_TEXT = 1, 2
FRAME_BYTES = W * H // 8          # 1024

# Pillow 9.1 之后用 Image.Dither 枚举，之前是模块级常量
try:
    DITHER_FS = Image.Dither.FLOYDSTEINBERG
    DITHER_NONE = Image.Dither.NONE
except AttributeError:                                  # pragma: no cover
    DITHER_FS = Image.FLOYDSTEINBERG
    DITHER_NONE = Image.NONE


def pack_frame(gray, dither=True, invert=False, autocontrast=True):
    """灰度 ndarray (64,128) uint8 -> SSD1306 页格式 bytes(1024)

    页格式：每字节 8 个纵向像素，bit0 在最上方、bit7 在最下方。
    """
    img = Image.fromarray(gray, mode="L")
    if autocontrast:
        # 小小一块屏幕，先把对比度拉满好看得多，尤其是暗场景
        img = ImageOps.autocontrast(img, cutoff=1)
    if invert:
        img = ImageOps.invert(img)
    img = img.convert("1", dither=(DITHER_FS if dither else DITHER_NONE))

    # 坑：PIL 的 '1' 模式图转成 numpy 得到的是 bool 数组（True/False），
    # 不是 0/255。所以这里必须用 != 0，不能写 (> 127)——
    # 后者会把所有 True 变成 1 再拿去比 127，结果全是 0，
    # 表现就是「帧率正常、链路正常、屏幕全黑」，而且完全不报错。
    bits = (np.asarray(img) != 0).astype(np.uint8)
    bits = bits.reshape(H // 8, 8, W)                   # (页, 页内行, 列)

    out = np.zeros((H // 8, W), dtype=np.uint8)
    for r in range(8):
        out |= bits[:, r, :] << r
    return out.tobytes()


def self_check():
    """打包一个棋盘格，确认位序/字节序真的能出亮点。

    这类错误不会抛异常，只会让屏幕一片黑，先自己验一遍省得白折腾。
    """
    checker = (np.indices((H, W)).sum(axis=0) % 2 * 255).astype(np.uint8)
    payload = pack_frame(checker, dither=False, autocontrast=False)
    if len(payload) != FRAME_BYTES:
        sys.exit(f"自检失败：帧长 {len(payload)}，应该是 {FRAME_BYTES}")
    ones = sum(bin(b).count("1") for b in payload)
    if ones == 0:
        sys.exit("自检失败：打包出来的帧一个亮点都没有（多半是位打包写错了）")
    return ones


def send_msg(sock, mtype, payload):
    sock.sendall(MAGIC + bytes([mtype]) + struct.pack("<H", len(payload)) + payload)


def test_frame(i):
    """不依赖任何素材的测试画面：斜条纹滚动 + 边框 + 沿 x 扫的竖条。

    边框用来确认画面没有偏移，斜纹看两个轴的方向，扫动的竖条看流畅度。
    """
    x = np.arange(W)[None, :]
    y = np.arange(H)[:, None]

    g = np.where(((x + 2 * y + i * 3) // 8) % 2 == 0, 0, 255).astype(np.uint8)
    g[0, :] = 255
    g[H - 1, :] = 255
    g[:, 0] = 255
    g[:, W - 1] = 255
    g[2:H - 2, (i * 2) % W] = 255
    return g


def open_source(source):
    """返回 (类型, 对象)：('test', None) 或 ('cap', VideoCapture)"""
    if source == "test":
        return "test", None

    if source.isdigit():
        cap = cv2.VideoCapture(int(source))
    else:
        cap = cv2.VideoCapture(source)

    if not cap.isOpened():
        sys.exit(f"打不开输入源：{source}")

    if source.isdigit():
        cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
        cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
    return "cap", cap


def main():
    ap = argparse.ArgumentParser(description="把视频推到 ESP32-S3 的 SSD1306 上")
    ap.add_argument("--host", required=True, help="OLED 上显示的 IP")
    ap.add_argument("--port", type=int, default=8888)
    ap.add_argument("--source", default="test",
                    help="test / 视频文件路径 / 摄像头编号 / text:消息")
    ap.add_argument("--fps", type=float, default=25.0,
                    help="目标帧率，默认 25（I2C 400kHz 上限约 40）")
    ap.add_argument("--loop", action="store_true", help="视频文件循环播放")
    ap.add_argument("--no-dither", action="store_true", help="关掉抖动，纯二值化")
    ap.add_argument("--no-autocontrast", action="store_true", help="关掉自动对比度")
    ap.add_argument("--invert", action="store_true", help="黑白反转")
    args = ap.parse_args()

    ones = self_check()
    print(f"自检通过：一帧 {FRAME_BYTES} 字节，棋盘格 {ones} 个亮点")

    try:
        sock = socket.create_connection((args.host, args.port), timeout=5)
    except OSError as e:
        sys.exit(f"连不上 {args.host}:{args.port} -> {e}\n"
                 f"确认 OLED 上显示的 IP 和这个一致，并且电脑连的是同一个 Wi-Fi。")
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    print(f"已连接 {args.host}:{args.port}")

    # 只发一行字，用来快速确认链路通不通
    if args.source.startswith("text:"):
        send_msg(sock, MSG_TEXT, args.source[5:].encode("ascii", "replace"))
        time.sleep(0.2)
        sock.close()
        print("已发送文字")
        return

    kind, cap = open_source(args.source)
    is_file = kind == "cap" and not args.source.isdigit()

    period = 1.0 / args.fps
    next_t = time.perf_counter()
    stat_t = next_t
    sent = 0
    i = 0

    try:
        while True:
            if kind == "test":
                payload = pack_frame(test_frame(i), dither=False, autocontrast=False)
            else:
                ok, frame = cap.read()
                if not ok:
                    if is_file and args.loop:
                        cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
                        continue
                    print("\n输入结束")
                    break
                # 直接拉伸到 128x64；单色小屏上比例失真比黑边更容易看清
                gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
                gray = cv2.resize(gray, (W, H), interpolation=cv2.INTER_AREA)
                payload = pack_frame(gray,
                                     dither=not args.no_dither,
                                     invert=args.invert,
                                     autocontrast=not args.no_autocontrast)

            send_msg(sock, MSG_FRAME, payload)
            i += 1
            sent += 1

            now = time.perf_counter()
            if now - stat_t >= 2.0:
                print(f"\r已发送 {sent} 帧，实际 {sent / (now - stat_t):5.1f} fps   ",
                      end="", flush=True)
                sent = 0
                stat_t = now

            next_t += period
            sleep = next_t - time.perf_counter()
            if sleep > 0:
                time.sleep(sleep)
            else:
                next_t = time.perf_counter()   # 跟不上就重新对齐，别让欠账越积越多

    except (BrokenPipeError, ConnectionResetError):
        print("\nESP32 断开了连接")
    except KeyboardInterrupt:
        print("\n已停止")
    finally:
        if cap is not None:
            cap.release()
        sock.close()


if __name__ == "__main__":
    main()
