#!/usr/bin/env python3
"""
接收 app_camera.c 从串口吐出来的一帧 RGB565 图像，存成 PNG。

用法：
    python tools/recv_frame.py COM17            # Windows
    python tools/recv_frame.py /dev/ttyACM0     # Linux

依赖：
    pip install pyserial pillow
    （没装 pillow 也能跑，会存成 .ppm，用画图/GIMP 都能打开）

流程：
    1. 先跑这个脚本，它会一直等着
    2. 然后按开发板上的 B1 蓝色按钮
    3. 图像收完自动存成 frame_<时间戳>.png

串口帧格式（和固件里的 dump_frame() 对应）：
    magic            4 字节   "OV36"
    width            2 字节   小端
    height           2 字节   小端
    bytes_per_pixel  1 字节
    reserved         1 字节
    payload          w*h*bpp 字节，RGB565 大端（高字节先出）
    checksum         4 字节   payload 所有字节之和，小端
"""

import sys
import time
import struct

try:
    import serial
except ImportError:
    sys.exit("缺少 pyserial，先执行:  pip install pyserial")

BAUD = 921600
MAGIC = b"OV36"


def rgb565_to_rgb888(buf, w, h):
    """RGB565 大端（高字节先出）-> 每像素 3 字节 RGB888"""
    out = bytearray(w * h * 3)
    for i in range(w * h):
        lo = buf[i * 2]
        hi = buf[i * 2 + 1]
        # OV 传感器 RGB565 是【高字节先出】，所以是大端，不是小端
        px = (lo << 8) | hi
        r5 = (px >> 11) & 0x1F
        g6 = (px >> 5) & 0x3F
        b5 = px & 0x1F
        # 5/6 bit 扩展到 8 bit：把高位复制到低位，比单纯左移更接近原亮度
        out[i * 3 + 0] = (r5 << 3) | (r5 >> 2)
        out[i * 3 + 1] = (g6 << 2) | (g6 >> 4)
        out[i * 3 + 2] = (b5 << 3) | (b5 >> 2)
    return bytes(out)


def save(rgb, w, h, stem):
    try:
        from PIL import Image
        img = Image.frombytes("RGB", (w, h), rgb)
        name = stem + ".png"
        img.save(name)
        return name
    except ImportError:
        name = stem + ".ppm"
        with open(name, "wb") as f:
            f.write(b"P6\n%d %d\n255\n" % (w, h))
            f.write(rgb)
        return name


def read_exact(ser, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = ser.read(n - len(buf))
        if not chunk:
            raise TimeoutError("读超时，已收到 %d/%d 字节" % (len(buf), n))
        buf += chunk
    return bytes(buf)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    port = sys.argv[1]
    trigger = "--trigger" in sys.argv[2:]

    ser = serial.Serial(port, BAUD, timeout=5)
    if trigger:
        # app_camera.c uses the 'd' command to capture and immediately dump one frame.
        ser.write(b"d")
        ser.flush()
    print("已打开 %s @ %d" % (port, BAUD))
    print("等待图像…… 现在去按开发板上的 B1 蓝色按钮")
    print("（板子的普通日志会照常打印出来）\n")

    window = bytearray()
    while True:
        b = ser.read(1)
        if not b:
            continue

        window += b
        if len(window) > 4:
            del window[:-4]

        if bytes(window) != MAGIC:
            # 不是帧头就当普通日志打出来
            try:
                sys.stdout.write(b.decode("utf-8", "replace"))
                sys.stdout.flush()
            except Exception:
                pass
            continue

        # --- 收到帧头 ---
        hdr = read_exact(ser, 6)
        w, h, bpp, _ = struct.unpack("<HHBB", hdr)
        size = w * h * bpp
        print("\n\n帧头: %dx%d, 每像素 %d 字节, 共 %d 字节" % (w, h, bpp, size))

        if not (0 < w <= 4096 and 0 < h <= 4096 and bpp in (1, 2)):
            print("帧头不合法，跳过")
            window.clear()
            continue

        t0 = time.time()
        payload = read_exact(ser, size)
        ck = struct.unpack("<I", read_exact(ser, 4))[0]
        dt = time.time() - t0

        calc = sum(payload) & 0xFFFFFFFF
        ok = (calc == ck)
        print("接收耗时 %.2fs (%.0f kB/s)，校验和 %s (收到 0x%08X / 算出 0x%08X)"
              % (dt, size / dt / 1024, "正确" if ok else "错误！", ck, calc))
        if not ok:
            print("校验和不对说明串口丢字节了，图像可能是花的。")

        if bpp == 2:
            rgb = rgb565_to_rgb888(payload, w, h)
        else:
            rgb = bytes(v for p in payload for v in (p, p, p))

        stem = "frame_%s" % time.strftime("%Y%m%d_%H%M%S")
        name = save(rgb, w, h, stem)
        print("已保存: %s\n" % name)

        window.clear()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n退出")
