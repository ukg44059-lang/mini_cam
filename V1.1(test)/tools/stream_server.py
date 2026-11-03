#!/usr/bin/env python3
"""
接收 app_stream.c 从串口连续吐出来的图像，转成 MJPEG，在浏览器里实时显示。

用法：
    python tools/stream_server.py COM17
    python tools/stream_server.py COM17 --baud 2000000 --http-port 8080
    python tools/stream_server.py /dev/ttyACM0            # Linux
    python tools/stream_server.py --source cc1310 --stm32-port COM22 --cc1310-port COM20

网页可选择接收来源、串口和波特率。选择433 MHz接收时，电脑只打开接收端
CC1310串口，控制命令和发送端状态沿433 MHz无线链路往返。

然后浏览器打开   http://localhost:8080   （脚本会自动帮你开）

依赖：
    pip install pyserial pillow numpy

工作方式：
    串口读取线程  ->  解析帧  ->  RGB565 转 RGB888  ->  编码成 JPEG  ->  放进 latest
    HTTP 线程     ->  /stream 用 multipart/x-mixed-replace 把 JPEG 一张张推给浏览器

    浏览器端就是一个普通的 <img src="/stream">，不需要 WebSocket，也不需要插件。

串口帧格式（和固件 app_stream.c 的 send_frame 对应）：
    magic            4 字节   "OV3S"
    width            2 字节   小端
    height           2 字节   小端
    format           1 字节   0=JPEG, 1=灰度, 2=RGB565
    seq              1 字节   帧序号低 8 位，用来发现丢帧
    length           4 字节   小端，payload 字节数
    payload          length 字节，RGB565 大端（高字节先出）
    checksum         4 字节   payload 所有字节之和，小端
"""

from test_console import experiments, PAGE as TEST_PAGE
import argparse
import re
import io
import json
import os
import sys
import threading
import time
import webbrowser
import struct
from collections import deque
from urllib.parse import parse_qs, urlsplit
from serial.tools import list_ports
from datetime import datetime
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    import serial
except ImportError:
    sys.exit("缺少 pyserial，先执行:  pip install pyserial")
try:
    import numpy as np
except ImportError:
    sys.exit("缺少 numpy，先执行:  pip install numpy")
try:
    from PIL import Image
except ImportError:
    sys.exit("缺少 pillow，先执行:  pip install pillow")

MAGIC = b"OV3S"          # S = stream。app_camera 用的是 "OV36" + 10 字节头，
                         # 两套头长度不同，magic 分开才不会互相误认
HEADER_REST = 10         # magic 之后：w(2) h(2) fmt(1) seq(1) len(4)
FMT_JPEG, FMT_GRAY, FMT_RGB565 = 0, 1, 2
MAX_W, MAX_H = 2048, 2048
# 板子的缓冲池是 614400 字节，加上余量取 1.5MB 当上限。
# 这个上限的作用是挡掉「假帧头」：随机字节解出来的 w/h 往往要求几 MB 数据，
# 没有上限的话解析器会傻等，一等就是十几秒，整个流就卡死了。
MAX_FRAME_BYTES = 1_500_000


def path_qs(path):
    """把 '/set?a=1&b=2' 里的查询串解析成 dict"""
    return {k: v[-1] for k, v in parse_qs(urlsplit(path).query).items()}


class EventLog:
    def __init__(self):
        self.lock = threading.Lock()
        self.rows = deque(maxlen=300)
        self.seq = 0
        self.recent = {}

    def add(self, message, level="INFO"):
        message = str(message)[:700]
        with self.lock:
            now = time.monotonic()
            if now - self.recent.get(message, -100) < 10:
                return
            self.recent[message] = now
            if len(self.recent) > 600:
                self.recent = {k: v for k, v in self.recent.items() if now-v < 60}
            self.seq += 1
            self.rows.append(dict(id=self.seq, time=datetime.now().strftime("%H:%M:%S"),
                                  level=level, message=message))

    def since(self, cursor):
        with self.lock:
            return {"cursor": self.seq, "entries": [r for r in self.rows if r["id"] > cursor]}


events = EventLog()


# ------------------------------------------------------------------ 共享状态 --
class Latest:
    """最新一帧 + 统计。串口线程写，HTTP 线程读。"""

    def __init__(self):
        self.cond = threading.Condition()
        self.jpeg = None
        self.rgb = None           # 解出来的像素数组（目前只在 publish 里用）
        self.gen = 0              # 帧代号，HTTP 线程靠它判断有没有新帧

        self.width = 0
        self.height = 0
        self.frames = 0           # 收到的完整帧数
        self.dropped = 0          # 靠 seq 断层发现的丢帧
        self.bad_crc = 0          # 校验和不对
        self.resyncs = 0          # 找不到 magic 重新同步的次数
        self.bytes_rx = 0
        self.t0 = time.time()
        self.t_last = 0.0
        self.fps = 0.0
        # 吞吐用和 fps 一样的 1 秒滚动窗口算。
        # 别用 bytes_rx/uptime 那种「开机以来的平均」：服务器往往比固件先开着，
        # 前面那段空转会把平均值一直拉低，看上去像丢速度，其实链路是满的。
        self.kbps = 0.0
        self.serial_ok = False
        self.serial_err = ""

        # ---- JPEG 压缩情况 ----
        self.jpeg_bytes = 0        # 当前这一帧压完多大
        self.raw_bytes = 0         # 压之前多大（w*h*2）
        self.jpeg_avg = 0.0        # 最近若干帧的平均，单帧会随画面内容跳
        self.quality = 85
        self.bpp = 2
        self.hw_jpeg = False   # 当前这帧是不是传感器硬件压的

        # ---- 下发给板子的设置（板子那边是真生效的，不是只改显示）----
        self.want_res = None       # 0=QQVGA 1=QVGA，None=没改过
        self.want_fps = 0          # 0=不限
        self.want_jpeg = False     # 是否已下发硬件 JPEG 模式
        self.jpeg_qs = 12          # 传感器的量化步长，越小越清晰
        self.performance = {}
        self.firmware_mode = "unknown"
        self.control_tail = bytearray()

        # 写命令与切换/关闭控制口互斥；pyserial 支持同时读写。
        self.tx_lock = threading.Lock()
        self.ser = None
        self.receiver = {"source": "stm32", "port": "COM30", "baud": 1000000,
                         "control_port": "COM30", "control_baud": 1000000}
        self.control_transport = "serial"
        self.control_last = 0
        self.receiver_version = 0
        self.source_defaults = {}
        self.radio_packets = self.radio_incomplete = 0
        self.sender_port = "COM34"
        self.sender = {"connected": False, "port": "COM34", "last": ""}


    def publish(self, jpeg, rgb, w, h, bpp=2):
        """bpp=0 表示这是传感器直出的 JPEG，jpeg 参数就是原始码流"""
        experiments.frame(w, h, bpp)
        with self.cond:
            self.jpeg = jpeg
            self.rgb = rgb
            self.width, self.height = w, h
            self.jpeg_bytes = len(jpeg)
            # JPEG 模式下没有「原始帧」这个东西了（板子根本没发原始像素），
            # 但拿 w*h*2 当基准算压缩比仍然有意义：
            # 它回答的是「同样这张图，不压缩要占多少链路带宽」
            self.raw_bytes = w * h * (2 if bpp == 0 else bpp)
            self.bpp = bpp
            self.hw_jpeg = (bpp == 0)
            # 指数滑动平均：单帧大小会随画面内容大幅跳动（对着白墙和对着人差好几倍），
            # 平均值才看得出压缩率的实际水平
            self.jpeg_avg = (len(jpeg) if self.jpeg_avg == 0
                             else self.jpeg_avg * 0.9 + len(jpeg) * 0.1)
            self.gen += 1
            self.t_last = time.time()
            self.cond.notify_all()

    def send_cmd(self, text):
        """给板子下发一行命令。失败返回错误字符串，成功返回 None"""
        try:
            with self.tx_lock:
                ser = self.ser
                if ser is None:
                    return "控制串口没打开"
                raw = text.encode("ascii")
                if self.receiver["source"] == "rf433":
                    if len(raw) > 160:
                        return "无线控制命令超过160字节"
                    body = b"WRC1" + len(raw).to_bytes(2, "little") + raw
                    check = 2166136261
                    for byte in body:
                        check = ((check ^ byte) * 16777619) & 0xffffffff
                    ser.write(body + check.to_bytes(4, "little"))
                else:
                    ser.write(raw)
                ser.flush()
            events.add("相机控制：" + text.strip())
            return None
        except Exception as e:
            return str(e)

    def snapshot_stats(self):
        with self.cond:
            up = time.time() - self.t0
            return {
                "receiver": dict(self.receiver),
                "control_transport": self.control_transport,
                "control_age": round(time.time()-self.control_last, 1) if self.control_last else None,
                "sender": dict(self.sender),
                "performance": dict(self.performance),
                "firmware_mode": self.firmware_mode,
                "radio_packets": self.radio_packets,
                "radio_incomplete": self.radio_incomplete,
                "frame_age": round(time.time()-self.t_last, 1) if self.t_last else None,
                "width": self.width,
                "height": self.height,
                "frames": self.frames,
                "dropped": self.dropped,
                "bad_crc": self.bad_crc,
                "resyncs": self.resyncs,
                "fps": round(self.fps, 2),
                "kbps": round(self.kbps, 1),
                "jpeg_bytes": self.jpeg_bytes,
                "jpeg_avg": int(self.jpeg_avg),
                "raw_bytes": self.raw_bytes,
                "ratio": round(self.raw_bytes / self.jpeg_bytes, 1) if self.jpeg_bytes else 0,
                "quality": self.quality,
                "bpp": self.bpp,
                "hw_jpeg": self.hw_jpeg,
                "jpeg_qs": self.jpeg_qs,
                "want_res": self.want_res,
                "want_fps": self.want_fps,
                "uptime": int(up),
                "serial_ok": self.serial_ok,
                "serial_err": self.serial_err,
                "has_frame": self.jpeg is not None,
            }


latest = Latest()

def feed_performance(raw):
    experiments.telemetry(raw)
    """Extract bounded ASCII telemetry among optional binary mirrored frames."""
    with latest.cond:
        data = bytes(latest.control_tail) + raw
        if b'[TEST] event=' in data:
            latest.firmware_mode = "test"
        elif b'[PERF] bytes=' in data and latest.firmware_mode != "test":
            latest.firmware_mode = "stream"
        pattern = rb'\[PERF\] bytes=(\d+) capture_ms=(\d+) send_ms=(\d+) pipeline=(\d) mirror=(\d) spi=(\d)\r?\n'
        for match in re.finditer(pattern, data):
            values = map(int, match.groups())
            latest.performance = dict(zip(('bytes','capture_ms','send_ms','pipeline','mirror','spi'), values))
        latest.control_tail = bytearray(data[-200:])


# -------------------------------------------------------------- 串口读取 ----
def decode_frame(payload, w, h, bpp):
    """
    解成给 PIL 用的 numpy 数组。

    bpp=2  RGB565 大端  -> (h, w, 3) uint8，PIL 认成 RGB
    bpp=1  灰度 Y       -> (h, w)    uint8，PIL 认成 L

    大于 VGA 的档位板子会自动切灰度：RGB565 一像素 2 字节，
    SVGA 800x600 要 960000 字节，比 STM32U575 的总 SRAM 还大，装不下整帧。
    """
    if bpp == 1:
        return np.frombuffer(payload, dtype=np.uint8).reshape(h, w)

    v = np.frombuffer(payload, dtype=">u2").reshape(h, w).astype(np.uint16)
    r = (v >> 11) & 0x1F
    g = (v >> 5) & 0x3F
    b = v & 0x1F
    # 5bit/6bit 拉满到 8bit：乘 255 再除满量程，比单纯左移更准
    r = ((r * 255 + 15) // 31).astype(np.uint8)
    g = ((g * 255 + 31) // 63).astype(np.uint8)
    b = ((b * 255 + 15) // 31).astype(np.uint8)
    return np.dstack([r, g, b])


class Reader:
    """
    带重新同步的帧解析器。

    固件启动时会先打印一段文字，而且串口随时可能在半路接上，
    所以不能假设一上来就对齐在帧头，必须能自己找 magic。
    """

    def __init__(self, ser, jpeg_quality):
        self.ser = ser
        self.version = latest.receiver_version
        self.buf = bytearray()
        self.jpeg_quality = jpeg_quality
        self.last_seq = None
        self.fps_t0 = time.time()
        self.fps_n = 0
        self.byte_mark = latest.bytes_rx
        self.log_buffer = bytearray()

    def _fill(self, need):
        """读到 buf 至少有 need 字节，返回是否成功"""
        while len(self.buf) < need:
            if self.version != latest.receiver_version:
                raise ConnectionChanged()
            chunk = self.ser.read(min(65536, max(1, getattr(self.ser, 'in_waiting', 0))))
            if not chunk:
                return False           # 超时
            self.buf += chunk
            latest.bytes_rx += len(chunk)
        return True

    def _find_magic(self):
        """把 buf 前面的垃圾丢掉，直到 buf 以 magic 开头"""
        while True:
            i = self.buf.find(MAGIC)
            if i == 0:
                return True
            if i > 0:
                # 前面那段是启动文字或者上一帧的残渣
                text = bytes(self.buf[:i])
                self._echo(text)
                del self.buf[:i]
                return True
            # 没找到。保留末尾 3 字节，magic 可能被切开了
            if len(self.buf) > 3:
                self._echo(bytes(self.buf[:-3]))
                del self.buf[:-3]
            if not self._fill(len(self.buf) + 1):
                return False

    def _echo(self, raw):
        """
        把非图像数据当固件日志打出来，方便看初始化过程和报错。

        注意两个坑，都踩过：
        1. 串口是按块读的，一个多字节汉字可能被切成两半，decode 出来是 U+FFFD，
           而 Windows 控制台默认 GBK 编不了 U+FFFD，print 会直接抛异常；
           异常再把整个读取循环打死 —— 表现就是「网页一直卡在等第一帧」。
        2. 所以既要滤掉 U+FFFD，又要把 print 整个包进 try：
           日志再有用也不能让它干掉图像流。
        """
        if not raw:
            return
        feed_performance(raw)
        self.log_buffer += raw
        while b"\n" in self.log_buffer:
            line, _, rest = self.log_buffer.partition(b"\n")
            self.log_buffer = bytearray(rest)
            self._echo_line(line)
        if len(self.log_buffer) > 512:
            self.log_buffer.clear()

    @staticmethod
    def _echo_line(raw):

        # ★ 关键：重新同步时这里拿到的是大段【二进制像素数据】，不是日志文本。
        #
        # 早期版本无脑往控制台打印，结果换一次分辨率就狂刷几百 KB 乱码，
        # 而打印比串口还慢 -> 读取线程被拖住 -> 串口缓冲溢出 -> 更找不到帧头
        # -> echo 更多垃圾，直接进死循环，表现是「换大分辨率就再也收不到帧」。
        #
        # 所以两道闸：超过 512 字节的一律不打（真日志没那么长），
        # 以及必须能严格按 UTF-8 解出来（二进制基本不可能通过）。
        if len(raw) > 512:
            return
        try:
            t = raw.decode("utf-8")          # 严格解码，不用 errors=replace
        except UnicodeDecodeError:
            return                           # 二进制，或者汉字被切成两半，丢掉
        # 除了换行/制表，不该有控制字符；有就说明是数据不是文本
        if any((ord(c) < 32) and (c not in "\r\n\t") for c in t):
            return
        t = t.strip("\r\n\x00")
        if not t.strip():
            return
        try:
            for line in t.splitlines():
                if line.strip():
                    print("[板子] " + line.rstrip())
                    events.add("接收板：" + line.rstrip())
        except Exception:
            pass

    def read_frame(self):
        """读一帧，成功返回 (rgb, w, h)，超时/出错返回 None"""
        if not self._find_magic():
            return None
        if not self._fill(len(MAGIC) + HEADER_REST):
            return None

        hdr = self.buf[len(MAGIC):len(MAGIC) + HEADER_REST]
        w = hdr[0] | (hdr[1] << 8)
        h = hdr[2] | (hdr[3] << 8)
        fmt = hdr[4]
        seq = hdr[5]
        # 长度是板子显式给的，不再靠 w*h*bpp 反推 —— JPEG 是变长的，推不出来
        n = hdr[6] | (hdr[7] << 8) | (hdr[8] << 16) | (hdr[9] << 24)

        # 头部合法性检查要【尽量严】。
        # 4 字节的 magic 在几十万字节的像素数据里是会撞上的，尤其是画面平坦、
        # 字节值集中在 0x30~0x60 那一带的时候（'O''V''3''6' 正好落在这个区间）。
        # 一旦锁到假头，w/h 是随机值，可能要求等好几 MB 数据，
        # 等够了校验必然失败 —— 分辨率越大越容易踩到。
        #
        # 三道过滤：宽高必须是 4 的倍数（板子支持的档位全都是）、
        # bpp 只能是 1 或 2、整帧不能超过板子物理上可能发出的最大值。
        if not (0 < w <= MAX_W and 0 < h <= MAX_H
                and fmt in (FMT_JPEG, FMT_GRAY, FMT_RGB565)
                and (w % 4) == 0 and (h % 4) == 0
                and 0 < n <= MAX_FRAME_BYTES
                # 原始像素格式下长度必须刚好等于 w*h*bpp；JPEG 是变长的不能这么查
                and (fmt == FMT_JPEG or n == w * h * (1 if fmt == FMT_GRAY else 2))):
            del self.buf[:1]
            latest.resyncs += 1
            return None

        total = len(MAGIC) + HEADER_REST + n + 4
        if not self._fill(total):
            return None

        off = len(MAGIC) + HEADER_REST
        payload = bytes(self.buf[off:off + n])
        csum = int.from_bytes(self.buf[off + n:off + n + 4], "little")

        if (sum(payload) & 0xFFFFFFFF) != csum:
            # ★ 只跳过 1 个字节，【绝对不能】把整段 total 丢掉 ★
            #
            # 校验失败有两种可能：真帧被打断了，或者刚才锁的是假帧头。
            # 是假帧头的话，真正的帧头就在这段数据【里面】，
            # 整段丢掉会把真帧头一起丢了，然后下一轮继续锁到假头……
            # 之前就是这么进的死循环：换到大分辨率后 bad_crc 一路涨、
            # 再也收不到一帧。跳 1 个字节重新扫，代价只是多扫几十万字节，
            # bytes.find 是 C 实现的，这点开销可以忽略。
            del self.buf[:1]
            latest.bad_crc += 1
            latest.resyncs += 1
            return None

        del self.buf[:total]

        # 丢帧检测：seq 是 8 位循环的
        if self.last_seq is not None:
            gap = (seq - self.last_seq) & 0xFF
            if gap > 1:
                latest.dropped += gap - 1
        self.last_seq = seq

        latest.frames += 1
        self.fps_n += 1
        now = time.time()
        dt = now - self.fps_t0
        if dt >= 1.0:
            latest.fps = self.fps_n / dt
            latest.kbps = (latest.bytes_rx - self.byte_mark) / 1024.0 / dt
            self.byte_mark = latest.bytes_rx
            self.fps_n = 0
            self.fps_t0 = now

        return payload, w, h, fmt


class RadioReader(Reader):
    """Reassemble RFI1 UART records; only expose complete OV3S frames to Reader.

    A missing RF packet or an aborted frame never leaves the OV3S parser waiting
    for bytes from the next image. Duplicate ACK retries do not duplicate pixels.
    """
    def __init__(self, ser, jpeg_quality):
        super().__init__(ser, jpeg_quality)
        self.wire = bytearray()
        self.assembly = bytearray()
        self.key = None
        self.expected = None

    def _echo(self, raw):
        # Only checked WTS1 records may confirm a remote command. Bytes skipped
        # during UART resynchronization can include a damaged status payload.
        self.log_buffer += raw
        while b'\n' in self.log_buffer:
            line, _, rest = self.log_buffer.partition(b'\n')
            self.log_buffer = bytearray(rest)
            if line.startswith(b'[RF433 RX]') or line.startswith(b'[RF433]'):
                self._echo_line(line)
        if len(self.log_buffer) > 512:
            self.log_buffer.clear()

    def _wire_fill(self, need):
        while len(self.wire) < need:
            if self.version != latest.receiver_version:
                raise ConnectionChanged()
            chunk = self.ser.read(min(65536, max(1, getattr(self.ser, 'in_waiting', 0))))
            if not chunk:
                return False
            self.wire += chunk
            latest.bytes_rx += len(chunk)
        return True

    def _record(self):
        while True:
            markers = [(self.wire.find(b"RFI1"), b"RFI1"), (self.wire.find(b"WTS1"), b"WTS1")]
            markers = [(i, marker) for i, marker in markers if i >= 0]
            i, marker = min(markers, default=(-1, None), key=lambda item: item[0])
            if i >= 0:
                self._echo(bytes(self.wire[:i]))
                del self.wire[:i]
                break
            if len(self.wire) > 3:
                self._echo(bytes(self.wire[:-3]))
                del self.wire[:-3]
            if not self._wire_fill(len(self.wire)+1):
                return None
        if not self._wire_fill(6):
            return None
        n = int.from_bytes(self.wire[4:6], "little")
        if marker == b"WTS1":
            if not 1 <= n <= 224:
                del self.wire[:1]
                latest.resyncs += 1
                return None
        elif not 20 <= n <= 244:
            del self.wire[:1]
            latest.resyncs += 1
            return None
        if not self._wire_fill(n+10):
            return None
        packet = bytes(self.wire[6:6+n])
        check = int.from_bytes(self.wire[6+n:10+n], "little")
        valid_check = (sum(packet) & 0xffffffff == check) if marker == b"RFI1" else False
        if marker == b"WTS1":
            value = 2166136261
            for byte in self.wire[:6+n]:
                value = ((value ^ byte) * 16777619) & 0xffffffff
            valid_check = value == check
        if not valid_check:
            del self.wire[:1]
            latest.bad_crc += 1
            return None
        del self.wire[:n+10]
        if marker == b"WTS1":
            latest.control_transport = "433 MHz 双向无线"
            latest.control_last = time.time()
            latest.sender.update(connected=True, port="433 MHz", last=packet.decode("ascii", "replace").strip())
            feed_performance(packet)
            if b'[RF433 TX]' in packet:
                events.add('发送端无线日志：'+packet.decode('ascii', 'replace').strip())
            return None
        magic, ver, kind, session, frame, offset, size, flags, reserved = struct.unpack("<2sBBIIIHBB", packet[:20])
        if (magic != b"RI" or ver != 1 or kind != 1 or reserved or flags & ~3
                or not 1 <= size <= 224 or n != size+20
                or offset+size > MAX_FRAME_BYTES+18):
            latest.resyncs += 1
            return None
        latest.radio_packets += 1
        key = session, frame
        data = packet[20:]
        if key == self.key and offset < len(self.assembly):
            if self.assembly[offset:offset+size] == data:
                return None
        if flags & 1:
            if offset or size < 14 or data[:4] != MAGIC:
                latest.resyncs += 1
                return None
            if self.assembly:
                latest.radio_incomplete += 1
                events.add("无线图像不完整，已丢弃并接收下一帧", "WARN")
            self.assembly = bytearray()
            self.key = key
            self.expected = int.from_bytes(data[10:14], "little")+18
            if not 18 < self.expected <= MAX_FRAME_BYTES+18:
                self.key = None
                return None
        if self.key != key or offset != len(self.assembly):
            if self.assembly:
                latest.radio_incomplete += 1
            self.key = None
            self.assembly.clear()
            return None
        self.assembly += data
        if len(self.assembly) > self.expected:
            self.key = None
            self.assembly.clear()
            latest.resyncs += 1
        elif flags & 2:
            if len(self.assembly) == self.expected:
                result = bytes(self.assembly)
                self.key = None
                self.assembly.clear()
                return result
            latest.radio_incomplete += 1
            self.key = None
            self.assembly.clear()
        return None

    def _fill(self, need):
        while len(self.buf) < need:
            frame = self._record()
            if frame is not None:
                self.buf += frame
            else:
                return False
        return True


class ConnectionChanged(Exception):
    pass


def drain_control(ser, stop):
    """Drain the mirrored STM32 stream while CC1310 is selected.

    Otherwise the USB UART/Windows queue overflows and leaves a damaged old
    frame waiting when the user switches back to direct reception.
    """
    try:
        while not stop.is_set():
            ser.read(max(4096, ser.in_waiting))
    except serial.SerialException:
        pass  # The main worker owns reconnect and closes the handle.


def wireless_query_worker():
    """Periodic query through the same receiver port; never opens a sender UART."""
    while True:
        if latest.receiver['source'] == 'rf433' and latest.ser is not None:
            error = latest.send_cmd('H0\n')
            if error:
                events.add('无线状态查询：'+error, 'WARN')
        time.sleep(5)


def serial_worker(port, baud, jpeg_quality):
    last_error = None
    while True:
        version = latest.receiver_version
        cfg = dict(latest.receiver)
        handles = []
        drain_stop = threading.Event()
        drainer = None
        try:
            # The RF control reader may still be finishing its 300 ms read
            # after a source change. Let it release the port before reopening.
            if cfg["source"] != "rf433":
                for _ in range(10):
                    if latest.ser is None or version != latest.receiver_version:
                        break
                    time.sleep(0.05)
            if version != latest.receiver_version:
                raise ConnectionChanged()
            with latest.tx_lock:
                ser = serial.Serial(cfg["port"], cfg["baud"], timeout=0.3, write_timeout=1)
                handles.append(ser)
                control = ser
                if cfg["source"] != "rf433" and cfg["control_port"] != cfg["port"]:
                    control = serial.Serial(cfg["control_port"], cfg["control_baud"],
                                            timeout=0.3, write_timeout=1)
                    handles.append(control)
                    drainer = threading.Thread(target=drain_control,
                                               args=(control, drain_stop), daemon=True)
                    drainer.start()
                if control is not None:
                    latest.ser = control
                    if cfg["source"] == "stm32":
                        control.write(b"V1\n")
                        control.flush()
                ser.reset_input_buffer()
                with latest.cond:
                    latest.jpeg = latest.rgb = None
                    latest.width = latest.height = latest.frames = latest.dropped = 0
                    latest.bad_crc = latest.resyncs = latest.bytes_rx = 0
                    latest.fps = latest.kbps = latest.jpeg_avg = 0
                    latest.jpeg_bytes = latest.raw_bytes = 0
                    latest.t_last = 0
                    latest.radio_packets = latest.radio_incomplete = 0
                    latest.gen += 1
                    latest.t0 = time.time()
                    latest.serial_ok, latest.serial_err = True, ""
                    latest.cond.notify_all()
            print("[接收] %s %s @ %d; 控制 %s" %
                  (cfg["source"], cfg["port"], cfg["baud"], cfg["control_port"]))
            events.add("图像串口已连接：%s %s @ %d" % (cfg["source"], cfg["port"], cfg["baud"]))
            if cfg["source"] == "rf433":
                latest.control_transport = "433 MHz 双向无线"
                error = latest.send_cmd("H0\n")
                if error:
                    raise serial.SerialException(error)
            last_error = None
            rd = (RadioReader if cfg["source"] == "rf433" else Reader)(ser, jpeg_quality)
            rd.version = version
            next_log = 0
            while version == latest.receiver_version:
                got = rd.read_frame()
                if got is None:
                    continue
                if version != latest.receiver_version:
                    break
                payload, w, h, fmt = got
                if fmt == FMT_JPEG:
                    latest.publish(payload, None, w, h, 0)
                else:
                    arr = decode_frame(payload, w, h, 1 if fmt == FMT_GRAY else 2)
                    b = io.BytesIO()
                    Image.fromarray(arr).save(b, "JPEG", quality=latest.quality)
                    latest.publish(b.getvalue(), arr, w, h, 1 if fmt == FMT_GRAY else 2)
                if time.monotonic() >= next_log:
                    events.add("收到完整图像：%s，%d×%d，%d 字节，累计 %d 帧" %
                               (cfg["source"], w, h, len(payload), latest.frames))
                    next_log = time.monotonic()+10
        except ConnectionChanged:
            pass
        except Exception as e:
            missing = cfg["port"].upper() not in {p.device.upper() for p in list_ports.comports()}
            latest.serial_err = ("接收板尚未接入，请接入后刷新串口并选择接收板应用串口"
                                 if cfg["source"] == "rf433" and missing else str(e))
            print("[串口] " + str(e))
            message = "图像串口未连接：%s；%s" % (cfg["port"], latest.serial_err)
            if message != last_error:
                events.add(message, "WARN")
                last_error = message
        finally:
            drain_stop.set()
            if drainer is not None:
                drainer.join(timeout=1)
            with latest.tx_lock:
                if latest.ser in handles:
                    latest.ser = None
                latest.serial_ok = False
                for handle in handles:
                    handle.close()
        # A switch interrupts retry delay promptly.
        for _ in range(20):
            if version != latest.receiver_version:
                break
            time.sleep(0.1)


# ------------------------------------------------------------------ 网页 ----
PAGE = """<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<title>OV3660 实时画面</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  body { margin:0; background:#14161a; color:#e6e8eb;
         font-family: ui-sans-serif, system-ui, "Microsoft YaHei", sans-serif; }
  .wrap { max-width: 1060px; margin: 0 auto; padding: 24px 20px 48px; }
  h1 { font-size: 18px; font-weight: 600; margin: 0 0 4px; }
  .sub { color:#8b929c; font-size: 13px; margin-bottom: 20px; }
  .stage { background:#000; border:1px solid #2a2e35; border-radius:10px;
           overflow:hidden; display:flex; align-items:center; justify-content:center;
           min-height: 320px; position: relative; }
  #cam { width:100%; height:auto; display:block; image-rendering: pixelated; }
  #wait { position:absolute; color:#6b7280; font-size:14px; }

  .panel { background:#1b1e24; border:1px solid #2a2e35; border-radius:9px;
           padding:14px 16px; margin:14px 0; }
  .row { display:flex; align-items:center; gap:12px; flex-wrap:wrap; margin:9px 0; }
  .row > .lbl { color:#8b929c; font-size:12px; width:104px; flex:none; }
  .hint { color:#5f6670; font-size:11px; }

  button { background:#2a2e35; color:#e6e8eb; border:1px solid #3a3f47;
           border-radius:7px; padding:6px 13px; font-size:13px; cursor:pointer; }
  button:hover { background:#343941; }
  button.on { background:#2f5d7c; border-color:#3d7aa3; color:#fff; }
  select { background:#2a2e35; color:#e6e8eb; border:1px solid #3a3f47; border-radius:6px; padding:6px; max-width:300px; }
  input[type=range] { width:190px; accent-color:#3d7aa3; }
  .val { font-variant-numeric: tabular-nums; font-size:13px; min-width:34px;
         display:inline-block; }

  .grid { display:grid; grid-template-columns: repeat(auto-fit, minmax(118px,1fr)); gap:10px; }
  .card { background:#1b1e24; border:1px solid #2a2e35; border-radius:8px; padding:10px 12px; }
  .k { color:#8b929c; font-size:11px; letter-spacing:.03em; }
  .v { font-size:18px; font-weight:600; margin-top:3px; font-variant-numeric: tabular-nums; }
  .v small { font-size:11px; font-weight:400; color:#8b929c; }
  .warn .v { color:#f0a04b; }
  .bad .v  { color:#e5645b; }
  .ok  .v  { color:#5ec27a; }
  .note { color:#6b7280; font-size:12px; margin-top:18px; line-height:1.75; }
  code { background:#22262d; padding:1px 5px; border-radius:4px; font-size:12px; }
  b.hl { color:#c9ced6; }
  #systemLog { background:#fff; color:#111; color-scheme:light; border:1px solid #ccc;
               border-radius:6px; height:180px; overflow:auto; padding:10px;
               font:12px/1.65 Consolas,"Microsoft YaHei",monospace; white-space:pre-wrap;
               overflow-wrap:anywhere; margin:0; }
</style></head><body><div class="wrap">
  <h1>OV3660 实时画面</h1>
  <div class="sub" id="route">选择图像接收链路</div>
  <div class="panel">
    <div class="row"><span class="lbl">图像来源</span>
      <select id="source" aria-label="图像来源" onchange="sourceChanged()"><option value="stm32">STM32 直接接收</option><option value="cc1310">CC1310 SPI 转发接收</option><option value="rf433">433 MHz 无线接收</option></select>
      <select id="port" aria-label="接收串口"></select>
      <select id="baud" aria-label="接收波特率"><option>115200</option><option>460800</option><option>921600</option><option>1000000</option><option>2000000</option></select>
      <button onclick="refreshPorts()">刷新串口</button><button onclick="connectSource()">连接 / 切换</button>
    </div>
    <div class="hint" id="connectionStatus">正在连接…</div>
  </div>

  <div class="stage">
    <span id="wait">等待第一帧…</span>
    <img id="cam" style="display:none">
  </div>

  <div class="panel">
    <div class="row"><span class="lbl">系统状态日志</span><button onclick="clearLogs()">清空显示</button><span class="hint" id="radioStatus"></span></div>
    <pre id="systemLog" role="log" aria-label="系统状态日志" aria-live="polite">等待系统状态…</pre>
  </div>

  <div class="panel">
    <div class="row">
      <span class="lbl">分辨率</span>
      <button id="r0" onclick="setRes(0)">160×120</button>
      <button id="r1" onclick="setRes(1)">320×240</button>
      <button id="r2" onclick="setRes(2)">400×300</button>
      <button id="r3" onclick="setRes(3)">480×360</button>
      <button id="r4" onclick="setRes(4)">640×480</button>
      <button id="r5" onclick="setRes(5)">800×600</button>
      <button id="r6" onclick="setRes(6)">1024×768</button>
      <button id="r7" onclick="setRes(7)">1600×1200</button>
      <button id="r8" onclick="setRes(8)">2048×1536</button>
    </div>
    <div class="row">
      <span class="lbl">帧率上限</span>
      <button onclick="setFps(0)"  id="f0">不限</button>
      <button onclick="setFps(1)"  id="f1">1</button>
      <button onclick="setFps(2)"  id="f2">2</button>
      <button onclick="setFps(5)"  id="f5">5</button>
      <button onclick="setFps(10)" id="f10">10</button>
      <button onclick="setFps(15)" id="f15">15</button>
      <span class="hint">板子照抓不误，只是到点才发 —— 限帧不会增加画面延迟</span>
    </div>
    <div class="row">
      <span class="lbl">压缩方式</span>
      <button id="mraw"  onclick="setMode('raw')">原始像素 (板子发 RGB565)</button>
      <button id="mjpeg" onclick="setMode('jpeg')">硬件 JPEG (OV3660 自己压)</button>
      <span class="hint">切到硬件 JPEG 后所有分辨率都能用，包括 2048×1536</span>
    </div>
    <div class="row" id="qsrow">
      <span class="lbl">传感器压缩</span>
      <input type="range" id="qs" min="8" max="48" step="1" value="12"
             oninput="qsLive(this.value)" onchange="setQs(this.value)">
      <span class="val" id="qsv">12</span>
      <span class="hint">OV3660 的量化步长，<b>越小越清晰、码流越大</b>（和下面那个方向相反）</span>
    </div>
    <div class="row" id="pcqrow">
      <span class="lbl">JPEG 质量</span>
      <input type="range" id="q" min="10" max="95" step="5" value="85"
             oninput="qLive(this.value)" onchange="setQ(this.value)">
      <span class="val" id="qv">85</span>
      <span class="hint">只在本机重新编码，不影响板子发出来的原始数据</span>
    </div>
    <div class="row">
      <span class="lbl">图传性能</span>
      <button onclick="setPerf('pipeline',1)">并行采集</button>
      <button onclick="setPerf('pipeline',0)">顺序采集</button>
      <button onclick="setPerf('mirror',0)">关闭图像副本</button>
      <button onclick="setPerf('mirror',1)">保留图像副本</button>
      <span class="hint" id="perfStatus">等待板端耗时统计</span>
    </div>
    <div class="row">
      <span class="lbl">VGA 预设</span>
      <button onclick="setVgaProfile(12)">画质优先</button>
      <button onclick="setVgaProfile(20)">平衡</button>
      <button onclick="setVgaProfile(28)">流畅</button>
      <span class="hint">切换到 640×480 硬件 JPEG；实际帧率随画面内容变化</span>
    </div>
    <div class="row">
      <span class="lbl"></span>
      <button id="snap" onclick="doSnap()">保存当前帧 (JPEG)</button>
      <button id="toggle" onclick="doToggle()">暂停</button>
      <span id="msg" class="hint"></span>
    </div>
  </div>

  <div class="grid">
    <div class="card"><div class="k">分辨率</div><div class="v" id="res">-</div></div>
    <div class="card" id="c_fps"><div class="k">帧率 FPS</div><div class="v" id="fps">-</div></div>
    <div class="card"><div class="k">链路吞吐</div><div class="v" id="kbps">- <small>KB/s</small></div></div>
    <div class="card"><div class="k">原始帧大小</div><div class="v" id="raw">-</div></div>
    <div class="card"><div class="k">JPEG 当前帧</div><div class="v" id="jpg">-</div></div>
    <div class="card"><div class="k">JPEG 平均</div><div class="v" id="jpgavg">-</div></div>
    <div class="card" id="c_ratio"><div class="k">压缩比</div><div class="v" id="ratio">-</div></div>
    <div class="card"><div class="k">已收帧数</div><div class="v" id="frames">-</div></div>
    <div class="card" id="c_drop"><div class="k">丢帧</div><div class="v" id="dropped">-</div></div>
    <div class="card" id="c_crc"><div class="k">校验错</div><div class="v" id="bad">-</div></div>
    <div class="card"><div class="k">运行时长</div><div class="v" id="up">-</div></div>
  </div>

  <div class="note">433 MHz 来源请选择接收板 COM20，921600 波特率；发送板 COM34 用于状态日志。相机设置通过 STM32 COM30 下发。传感器压缩参数越大，图像越小、画质越低。关闭图像副本可减少串口镜像耗时，切回 STM32 来源时会自动恢复。</div>
</div>

<script>
const img = document.getElementById('cam'), wait = document.getElementById('wait');
let live = true;
async function setPerf(key,value) {
  const result = await (await fetch('/set?' + key + '=' + value)).json();
  if (!result.ok) document.getElementById('msg').textContent = result.err;
}
async function setVgaProfile(qs) {
  const state = await (await fetch('/stats')).json();
  const mirror = state.receiver.source === 'stm32' ? 1 : 0;
  const result = await (await fetch('/set?mode=jpeg&res=4&fps=0&pipeline=1&spi=2&profile=1&mirror=' + mirror + '&qs=' + qs)).json();
  if (!result.ok) document.getElementById('msg').textContent = result.err;
}
img.onload = () => { wait.style.display='none'; img.style.display='block'; };
img.src = '/stream';

function doToggle() {
  const b = document.getElementById('toggle');
  live = !live;
  b.textContent = live ? '暂停' : '继续';
  img.src = live ? ('/stream?t=' + Date.now()) : '';
  if (!live) img.style.display = 'block';
}
async function doSnap() {
  const m = document.getElementById('msg');
  m.textContent = '保存中…';
  try {
    const r = await (await fetch('/snapshot')).json();
    if (r.ok) {
      // 把文件大小直接显示出来，省得再去资源管理器看属性
      const kb = (r.bytes / 1024).toFixed(1);
      const ratio = (r.raw / r.bytes).toFixed(1);
      m.textContent = r.file + '   ' + r.bytes + ' 字节 (' + kb + ' KB)，'
                    + '质量 ' + r.quality + '，压缩比 ' + ratio + ':1';
    } else { m.textContent = '失败：' + r.err; }
  } catch (e) { m.textContent = '失败：' + e; }
  setTimeout(() => m.textContent = '', 12000);
}
async function post(qs) {
  const m = document.getElementById('msg');
  try { const r = await (await fetch('/set?' + qs)).json();
        if (!r.ok) { m.textContent = '失败：' + (r.err || '未知');
                     setTimeout(() => m.textContent = '', 4000); } }
  catch (e) { m.textContent = '失败：' + e; }
}
const setRes  = v => post('res=' + v);
const setFps  = v => post('fps=' + v);
const setQ    = v => post('quality=' + v);
const setMode = v => post('mode=' + v);
const setQs   = v => post('qs=' + v);
const qLive   = v => document.getElementById('qv').textContent = v;
const qsLive  = v => document.getElementById('qsv').textContent = v;

const kb = n => n >= 1024 ? (n/1024).toFixed(1) + ' <small>KB</small>'
                          : n + ' <small>B</small>';
const set  = (id, v) => document.getElementById(id).innerHTML = v;
const cls  = (id, c) => { const e = document.getElementById(id);
                          e.className = 'card' + (c ? ' ' + c : ''); };
const mark = (id, on) => { const e = document.getElementById(id);
                           if (e) e.className = on ? 'on' : ''; };

let connectionDefaults = {};
let connectionConfig = null;
async function refreshPorts() {
  const r = await (await fetch('/ports')).json();
  connectionDefaults = r.defaults;
  connectionConfig = r.receiver;
  const sel = document.getElementById('port');
  const old = sel.value;
  sel.replaceChildren();
  for (const p of r.ports) sel.add(new Option(p.device + ' — ' + p.description, p.device));
  if (!old) {
    document.getElementById('source').value = r.receiver.source;
    sourceChanged();
    if (![...sel.options].some(o => o.value === r.receiver.port)) sel.add(new Option(r.receiver.port + '（未接入）', r.receiver.port));
    sel.value = r.receiver.port;
    document.getElementById('baud').value = String(r.receiver.baud);
  } else {
    if (![...sel.options].some(o => o.value === old)) sel.add(new Option(old + '（未接入）', old));
    sel.value = old;
  }
}
function sourceChanged() {
  const d = connectionDefaults[document.getElementById('source').value];
  if (!d) return;
  const sel = document.getElementById('port');
  if (![...sel.options].some(o => o.value === d.port)) sel.add(new Option(d.port + '（未接入）', d.port));
  sel.value = d.port;
  document.getElementById('baud').value = String(d.baud);
}
async function connectSource() {
  const qs = new URLSearchParams({source:document.getElementById('source').value,
    port:document.getElementById('port').value, baud:document.getElementById('baud').value});
  const r = await (await fetch('/connection?' + qs)).json();
  document.getElementById('connectionStatus').textContent = r.ok ? '切换中，等待完整新帧…' : r.err;
}
refreshPorts();
let logCursor = 0, logLines = [];
function clearLogs() { logLines = []; document.getElementById('systemLog').textContent = ''; }
async function pollLogs() {
  try {
    const r = await (await fetch('/logs?after=' + logCursor)).json();
    if (r.cursor < logCursor) { logLines = []; logCursor = 0; return; }
    logCursor = r.cursor;
    if (r.entries.length) {
      const box = document.getElementById('systemLog');
      const bottom = box.scrollHeight-box.scrollTop-box.clientHeight < 35;
      logLines.push(...r.entries.map(e => '[' + e.time + '] ' + e.level + ' ' + e.message));
      logLines = logLines.slice(-300);
      box.textContent = logLines.join('\\n');
      if (bottom) box.scrollTop = box.scrollHeight;
    }
  } catch (e) {}
}
pollLogs(); setInterval(pollLogs, 1000);
setInterval(async () => {
  try {
    const s = await (await fetch('/stats')).json();
    const c = s.receiver;
    const p = s.performance;
    if (p && p.capture_ms !== undefined) {
      document.getElementById('perfStatus').textContent =
        '采集 ' + p.capture_ms + ' ms；发送 ' + p.send_ms + ' ms；' +
        (p.pipeline ? '并行' : '顺序') + '；副本' + (p.mirror ? '开' : '关') +
        '；SPI ' + [0.625,1.25,2.5][p.spi] + ' MHz';
    }
    document.getElementById('route').textContent = c.source === 'rf433'
      ? 'OV3660 → STM32 → SPI2 → CC1310 发送 → 433.92 MHz → CC1310 接收 → ' + c.port + ' → 浏览器'
      : c.source === 'cc1310'
      ? 'OV3660 → STM32 → SPI2 → CC1310 → ' + c.port + ' → 浏览器'
      : 'OV3660 → STM32 → ' + c.port + ' → 浏览器';
    document.getElementById('connectionStatus').textContent =
      (s.serial_ok ? '已连接 ' : '未连接 ') + c.port + ' @ ' + c.baud +
      (c.source === 'rf433' ? '；控制与发送端状态走433 MHz无线链路' :
        '；控制串口 ' + c.control_port) + (s.serial_err ? '；' + s.serial_err : '') +
      (s.has_frame ? '' : '；等待完整新帧…');
    document.getElementById('radioStatus').textContent = c.source === 'rf433'
      ? ('发送端无线状态 ' + (s.sender.connected ? '已收到' : '等待') +
         (s.control_age === null ? '' : '，' + s.control_age + '秒前') +
         '；无线分包 ' + s.radio_packets + '；不完整图像 ' + s.radio_incomplete +
         (s.sender.last.includes('NO_ACK') ? '；尚未收到接收板确认' : '')) : '';
    if (s.frame_age > 3) { s.fps = 0; s.kbps = 0; }
    if (!s.has_frame) { img.style.display = 'none'; document.getElementById('wait').style.display = ''; }
    else if (live) { img.style.display = 'block'; document.getElementById('wait').style.display = 'none'; }
    set('res', s.width ? s.width + ' × ' + s.height +
                        (s.hw_jpeg ? ' <small>硬件JPEG</small>'
                                   : (s.bpp === 1 ? ' <small>灰度</small>' : '')) : '-');
    set('fps', s.fps.toFixed(1));
    set('kbps', s.kbps.toFixed(0) + ' <small>KB/s</small>');
    set('raw', kb(s.raw_bytes));
    set('jpg', kb(s.jpeg_bytes));
    set('jpgavg', kb(s.jpeg_avg));
    set('ratio', s.ratio ? s.ratio.toFixed(1) + ' <small>: 1</small>' : '-');
    set('frames', s.frames);
    set('dropped', s.dropped);
    set('bad', s.bad_crc);
    set('up', Math.floor(s.uptime/60) + 'm ' + (s.uptime%60) + 's');

    cls('c_fps', s.fps >= 3 ? 'ok' : (s.fps > 0 ? 'warn' : 'bad'));
    cls('c_drop', s.dropped ? 'warn' : '');
    cls('c_crc',  s.bad_crc > 10 ? 'bad' : (s.bad_crc ? 'warn' : ''));
    cls('c_ratio', 'ok');

    // 按板子【实际报上来的宽高】高亮，而不是按点了哪个按钮 ——
    // 这样命令没生效的话一眼就能看出来
    const W = [160, 320, 400, 480, 640, 800, 1024, 1600, 2048];
    W.forEach((w, i) => mark('r' + i, s.width === w));
    mark('mjpeg', s.hw_jpeg);
    mark('mraw', !s.hw_jpeg);
    // 两个压缩参数只有对应模式下才有意义，另一个置灰
    document.getElementById('qsrow').style.opacity  = s.hw_jpeg ? '1' : '0.35';
    document.getElementById('pcqrow').style.opacity = s.hw_jpeg ? '0.35' : '1';
    const qs = document.getElementById('qs');
    if (document.activeElement !== qs) { qs.value = s.jpeg_qs; qsLive(s.jpeg_qs); }
    [0,1,2,5,10,15].forEach(v => mark('f' + v, s.want_fps === v));

    const q = document.getElementById('q');
    if (document.activeElement !== q) { q.value = s.quality; qLive(s.quality); }
  } catch (e) {}
}, 1000);
</script></body></html>
"""


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass                      # 别把每个请求都刷到终端上

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/":
            self._send(200, "text/html; charset=utf-8", TEST_PAGE.encode("utf-8"))
        elif path == "/viewer":
            self._send(200, "text/html; charset=utf-8", PAGE.replace("<body>", '<body><div style="padding:16px;background:#dff4ee"><a href="/">进入 V1.1(test) 四档电流测试台</a></div>').encode("utf-8"))
        elif path == "/test/state":
            self._send(200, "application/json", json.dumps(experiments.state()).encode())
        elif path == "/test/guide":
            guide = Path(__file__).resolve().parents[1] / '测试操作说明.md'
            self._send(200, "text/plain; charset=utf-8", guide.read_bytes())
        elif path == "/test/export":
            with experiments.lock:
                body = dict(schema="four_mode_v1", active=experiments.active, boots=experiments.boots, readings=experiments.readings)
                payload = json.dumps(body, ensure_ascii=False, indent=2).encode('utf-8')
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Disposition", 'attachment; filename="test_results.json"')
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
        elif path == "/stats":
            self._send(200, "application/json",
                       json.dumps(latest.snapshot_stats()).encode())
        elif path == "/logs":
            try:
                cursor = max(0, int(path_qs(self.path).get("after", "0")))
            except ValueError:
                cursor = 0
            self._send(200, "application/json", json.dumps(events.since(cursor)).encode())
        elif path == "/ports":
            self._send(200, "application/json", json.dumps({
                "ports": [{"device": p.device, "description": p.description}
                          for p in list_ports.comports()],
                "defaults": latest.source_defaults, "receiver": latest.receiver}).encode())
        elif path == "/connection":
            self._connection(path_qs(self.path))
        elif path == "/set":
            self._set(path_qs(self.path))
        elif path == "/snapshot":
            self._snapshot()
        elif path == "/stream":
            self._stream()
        else:
            self._send(404, "text/plain", b"not found")

    def do_POST(self):
        if self.path != '/test/action':
            return self._send(404, 'text/plain', b'not found')
        try:
            size = int(self.headers.get('Content-Length', '0'))
            if not 0 < size <= 16000000:
                raise ValueError('请求大小无效')
            q = json.loads(self.rfile.read(size))
            if not isinstance(q, dict):
                raise ValueError('请求必须为JSON对象')
            result = experiments.action(q, latest)
            code = 200
        except (ValueError, KeyError, TypeError) as e:
            result, code = dict(ok=False, err=str(e)), 400
        self._send(code, 'application/json; charset=utf-8', json.dumps(result).encode())

    def _send(self, code, ctype, body):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _connection(self, q):
        try:
            source, port, baud = q["source"], q["port"], int(q["baud"])
            if source not in ("stm32", "cc1310", "rf433") or not port or baud not in (115200, 460800, 921600, 1000000, 2000000):
                raise ValueError("串口参数无效")
            cfg = dict(latest.receiver, source=source, port=port, baud=baud)
            if source == "stm32":
                cfg.update(control_port=port, control_baud=baud)
            elif source == "rf433":
                cfg.update(control_port=port, control_baud=baud)
            else:
                cfg.update(control_port=latest.source_defaults["stm32"]["port"],
                           control_baud=latest.source_defaults["stm32"]["baud"])
            with latest.cond:
                latest.receiver = cfg
                latest.source_defaults[source] = {"port": port, "baud": baud}
                latest.receiver_version += 1
                latest.jpeg = None
                latest.width = latest.height = latest.frames = latest.dropped = 0
                latest.bad_crc = latest.resyncs = latest.bytes_rx = 0
                latest.fps = latest.kbps = latest.jpeg_avg = 0
                latest.jpeg_bytes = latest.raw_bytes = latest.t_last = 0
                latest.radio_packets = latest.radio_incomplete = 0
                latest.sender["last"] = ""
                latest.serial_ok = False
                latest.serial_err = ""
                latest.gen += 1
                latest.cond.notify_all()
            out = {"ok": True}
            events.add("切换图像来源：%s %s @ %d" % (source, port, baud))
        except (KeyError, ValueError) as e:
            out = {"ok": False, "err": str(e)}
        self._send(200, "application/json", json.dumps(out).encode())

    def _set(self, q):
        """
        改设置。三个参数各走各的路：

          quality  纯本机的事，只影响 JPEG 编码，板子不知情，立即生效
          res      要下发到板子（板子重配传感器 + 重开 DCMI），
                   生效与否看后续帧头里的宽高
          fps      同样下发到板子，板子按最小间隔决定发不发
        """
        out = {"ok": True}

        # Optional performance controls in the STM32 streaming firmware.
        for key, letter, allowed in (("mirror", "V", (0, 1)),
                                     ("pipeline", "P", (0, 1)),
                                     ("spi", "S", (0, 1, 2)),
                                     ("profile", "T", (0, 1))):
            if key in q:
                try:
                    value = int(q[key])
                    if value not in allowed:
                        raise ValueError(key + " 参数无效")
                    error = latest.send_cmd("%s%d\n" % (letter, value))
                    if error:
                        raise ValueError(error)
                    out[key] = value
                except ValueError as e:
                    out.update(ok=False, err=str(e))

        if "quality" in q:
            try:
                v = max(1, min(95, int(q["quality"])))
                latest.quality = v
                out["quality"] = v
            except ValueError:
                out["ok"] = False
                out["err"] = "quality 不是数字"

        if "res" in q:
            # 序号和固件里 ov3660_framesize_t 的顺序一一对应。
            # 只放开 MCU 的 RAM 装得下的那几档：
            #   0 160x120  1 320x240  2 400x300  3 480x360  4 640x480(RGB565 上限)
            #   5 800x600 灰度（板子自动切成 1 字节/像素）
            # 6 以上(XGA/UXGA/QXGA)传感器支持但整帧存不下，板子会拒绝
            # 0~5 原始像素模式能用；6~8(XGA/UXGA/QXGA) 只有硬件 JPEG 模式下才装得下，
            # 这里一律放行，由板子按当前格式决定收不收 —— 装不下它会拒绝并保持原档位
            v = q["res"].upper()
            idx = {"QQVGA": 0, "QVGA": 1, "VGA": 4, "SVGA": 5,
                   "XGA": 6, "UXGA": 7, "QXGA": 8}.get(v)
            if idx is None and v.isdigit():
                idx = int(v) if 0 <= int(v) <= 8 else None
            if idx is None:
                out["ok"] = False
                out["err"] = "res 只支持 0~8"
            else:
                err = latest.send_cmd("R%d\n" % idx)
                if err:
                    out["ok"] = False
                    out["err"] = err
                else:
                    latest.want_res = idx
                    out["res"] = idx

        if "mode" in q:
            # jpeg = 传感器硬件压缩(M0)，raw = 原始像素(M1)
            v = q["mode"].lower()
            if v not in ("jpeg", "raw"):
                out["ok"] = False
                out["err"] = "mode 只支持 jpeg / raw"
            else:
                err = latest.send_cmd("M%d\n" % (0 if v == "jpeg" else 1))
                if err:
                    out["ok"] = False
                    out["err"] = err
                else:
                    latest.want_jpeg = (v == "jpeg")
                    out["mode"] = v

        if "qs" in q:
            # 传感器的量化步长，8~63，越小越清晰码流越大
            try:
                # 上限 48：实测 qs=63 时传感器只吐 68 字节的空壳 JPEG
                v = max(8, min(48, int(q["qs"])))
                err = latest.send_cmd("Q%d\n" % v)
                if err:
                    out["ok"] = False
                    out["err"] = err
                else:
                    latest.jpeg_qs = v
                    out["qs"] = v
            except ValueError:
                out["ok"] = False
                out["err"] = "qs 不是数字"

        if "fps" in q:
            try:
                v = max(0, min(60, int(q["fps"])))
                err = latest.send_cmd("F%d\n" % v)
                if err:
                    out["ok"] = False
                    out["err"] = err
                else:
                    latest.want_fps = v
                    out["fps"] = v
            except ValueError:
                out["ok"] = False
                out["err"] = "fps 不是数字"

        self._send(200, "application/json", json.dumps(out).encode())

    def _snapshot(self):
        """
        存当前这一帧。

        存的是【正在推给浏览器的那串 JPEG 字节本身】，不重新编码 ——
        这样磁盘上的文件大小和网页上「JPEG 当前帧」那个数字是同一个值，
        改质量滑块后存下来能直接对比出文件大小的变化。
        （重新编码的话，编码时刻的画面已经变了，大小就对不上了。）
        """
        with latest.cond:
            jpeg = latest.jpeg
            w, h, bpp, q = latest.width, latest.height, latest.bpp, latest.quality
        if jpeg is None:
            self._send(200, "application/json",
                       json.dumps({"ok": False, "err": "还没收到帧"}).encode())
            return

        with latest.cond:
            hw = latest.hw_jpeg
            qs = latest.jpeg_qs
        # 文件名把「谁压的、什么参数」写清楚，免得几十张混在一起分不出来
        name = ("snap_%s_%dx%d_%s.jpg" % (
            datetime.now().strftime("%Y%m%d_%H%M%S"), w, h,
            ("hwjpeg_qs%d" % qs) if hw else ("pc_q%d" % q)))
        path = os.path.join(SNAP_DIR, name)
        try:
            with open(path, "wb") as f:
                f.write(jpeg)
            size = len(jpeg)
            # 硬件 JPEG 时 bpp=0，拿 w*h*2 当「不压缩要多少字节」的基准
            raw = w * h * (2 if bpp == 0 else bpp)
            print("[保存] %s  %d 字节 (%.1f KB)  等效原始 %d 字节  压缩比 %.1f:1"
                  % (path, size, size / 1024.0, raw, raw / size if size else 0))
            self._send(200, "application/json", json.dumps({
                "ok": True, "file": path, "bytes": size,
                "raw": raw, "quality": q, "hw": hw,
            }).encode())
        except Exception as e:
            self._send(200, "application/json",
                       json.dumps({"ok": False, "err": str(e)}).encode())

    def _stream(self):
        self.send_response(200)
        self.send_header("Content-Type",
                         "multipart/x-mixed-replace; boundary=frame")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        seen = -1
        try:
            while True:
                with latest.cond:
                    if latest.gen == seen:
                        latest.cond.wait(timeout=5.0)
                    if latest.gen == seen or latest.jpeg is None:
                        continue
                    seen = latest.gen
                    jpg = latest.jpeg
                self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\n"
                                 b"Content-Length: " + str(len(jpg)).encode() +
                                 b"\r\n\r\n" + jpg + b"\r\n")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass                  # 浏览器关了标签页，正常退出


SNAP_DIR = "."


def main():
    global SNAP_DIR
    ap = argparse.ArgumentParser(description="OV3660 串口图传接收 + 网页显示")
    ap.add_argument("port", nargs="?", default=None, help="串口号，比如 COM17 或 /dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=1000000,
                    help="STM32 控制/镜像波特率，优化版 dual 固件默认 1000000")
    ap.add_argument("--source", choices=["stm32", "cc1310", "rf433"], default="rf433")
    ap.add_argument("--stm32-port", default="COM30")
    ap.add_argument("--cc1310-port", default="COM34")
    ap.add_argument("--cc1310-baud", type=int, default=115200)
    ap.add_argument("--rf-port", default="COM20", help="接收板的应用串口，接板后可在网页改选")
    ap.add_argument("--rf-baud", type=int, default=921600)
    ap.add_argument("--tx-status-port", default="COM34", help="发送板状态日志串口")
    ap.add_argument("--http-port", type=int, default=8080, help="网页端口")
    ap.add_argument("--quality", type=int, default=85, help="JPEG 质量 1-95")
    ap.add_argument("--snap-dir", default="captures", help="保存快照的目录")
    ap.add_argument("--no-browser", action="store_true", help="不自动打开浏览器")
    a = ap.parse_args()

    # Windows 控制台默认 GBK，打印固件的中文日志会抛 UnicodeEncodeError，
    # 统一改成 utf-8 + replace，从根上断掉「一条日志打死整个进程」这条路
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass

    latest.quality = max(1, min(95, a.quality))
    SNAP_DIR = a.snap_dir
    os.makedirs(SNAP_DIR, exist_ok=True)

    stm_port = a.port if a.port and a.source == "stm32" else a.stm32_port
    cc_port = a.port if a.port and a.source == "cc1310" else a.cc1310_port
    rf_port = a.port if a.port and a.source == "rf433" else a.rf_port
    latest.source_defaults = {"stm32": {"port": stm_port, "baud": a.baud},
                              "cc1310": {"port": cc_port, "baud": a.cc1310_baud},
                              "rf433": {"port": rf_port, "baud": a.rf_baud}}
    latest.receiver = dict(latest.source_defaults[a.source], source=a.source,
                           control_port=(rf_port if a.source == "rf433" else stm_port),
                           control_baud=(a.rf_baud if a.source == "rf433" else a.baud))
    latest.sender_port = a.tx_status_port
    latest.sender["port"] = a.tx_status_port
    events.add("服务启动：图像来源 %s；接收 %s" % (a.source, latest.receiver["port"]))
    threading.Thread(target=serial_worker,
                     args=(stm_port, a.baud, a.quality), daemon=True).start()

    threading.Thread(target=wireless_query_worker, daemon=True).start()

    url = "http://localhost:%d" % a.http_port
    srv = ThreadingHTTPServer(("127.0.0.1", a.http_port), Handler)
    srv.daemon_threads = True
    print("[网页] " + url + "    (Ctrl+C 退出)")
    if not a.no_browser:
        threading.Timer(0.8, lambda: webbrowser.open(url)).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n退出")


if __name__ == "__main__":
    main()
