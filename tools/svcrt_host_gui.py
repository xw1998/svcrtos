#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
SVCrtOS 主机上位机（图形界面）
=============================

把「需要 PC 配合」的几件事集中到一个窗口里：

    连接      选择串口 / 波特率，打开、关闭、刷新可用串口列表
    控制台    直接发 shell 命令，带常用命令快捷键，设备输出实时显示
    安装镜像  把 .svcapp 通过 ACK 节流协议装进设备（可选指定固定槽位）
    布局配置  生成 / 校验 / 写入 512 字节布局配置记录，读回与擦除
    帮助      协议约定与推荐操作顺序

为什么要一个界面
----------------
设备侧的协议是有状态的：安装窗口、配置接收窗口都要求主机在正确的时刻
写入字节（见 docs/配置区与安装策略.md、docs/SVCrtOS应用安装与调试指南.md）。
界面把这些时刻变成按钮和进度条，但不隐藏它们：日志区原样显示设备打印的
原因行，本程序不替设备猜原因——协议里的流控字节（0x06 / 0x15）本身不带
原因码，原因只在设备随后打印的那一行里。

两条必须守住的约定（界面已内建，手工敲命令时同样适用）：

    1. shell 以 CR 断行。发 `cfg load` 时若用 CRLF，残留的 LF 会占掉记录的
       第 1 个字节，设备收满 512 字节后会报 bad magic。
    2. 安装 / 配置写入期间，不允许第二个读者打开同一个串口。

依赖：pyserial（pip install pyserial）；tkinter 随 Python 提供。

用法
----
    py -3 tools/svcrt_host_gui.py
    python3 tools/svcrt_host_gui.py --selftest     # 无窗口自检（CI / 冒烟）
"""
import argparse
import collections
import contextlib
import json
import os
import struct
import subprocess
import sys
import threading
import time

# ---------------------------------------------------------------- 常量

APP_TITLE = "SVCrtOS 上位机"

DEFAULT_BAUD = 115200
BAUD_CHOICES = ("9600", "19200", "57600", "115200", "230400", "460800", "921600")

ACK_BYTE = 0x06           # 继续
NAK_BYTE = 0x15           # 停止（不带原因码，原因在设备打印行里）

# .svcapp 镜像头，见 kernelsrc/include/svcrt_app_image.h
IMG_HEADER_SIZE = 256
IMG_MAGIC = 0x53564341    # "SVCA"
IMG_TYPE_APP = 1
IMG_TYPE_DRIVER = 2
IMG_FLAG_AUTOSTART = 1 << 0
IMG_OFF_TYPE = 4
IMG_OFF_HW_COMPAT = 8
IMG_OFF_VERSION = 12
IMG_OFF_IMAGE_SIZE = 16
IMG_OFF_ENTRY_OFFSET = 20
IMG_OFF_NOMINAL_BASE = 24
IMG_OFF_CRC32 = 28
IMG_OFF_FLAGS = 96
IMG_OFF_STATE = 100
IMG_OFF_IMAGE_ID = 104
IMG_OFF_RAM_SIZE = 108
IMG_OFF_RELOC_COUNT = 116
IMG_OFF_RELOC_KIND = 120
IMG_OFF_PAYLOAD_OFFSET = 124
IMG_OFF_NOMINAL_RAM_BASE = 128

INSTALL_CHUNK = 512       # must match SVCRT_LOADER_CHUNK_SIZE
INSTALL_ACK_TIMEOUT = 5.0   # 128K 扇区擦除可到 ~2 s
INSTALL_HEADER_SETTLE = 0.05

# 配置记录，见 kernelsrc/include/svcrt_cfg.h 与 tools/svcrt_layout.py
CFG_RECORD_SIZE = 512
CFG_CHUNK_SIZE = 128
CFG_MAGIC = 0x47464353    # "SCFG"
CFG_READY_TIMEOUT = 5.0
CFG_CHUNK_TIMEOUT = 5.0
CFG_LAST_TIMEOUT = 15.0
CFG_QUIET_S = 0.6

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(HERE)
LAYOUT_TOOL = os.path.join(HERE, "svcrt_layout.py")
SEND_TOOL = os.path.join(HERE, "send_image.py")

# ---------------------------------------------------------------- 小工具


def device_lines(raw):
    """把设备原始字节切成可显示的日志行（UTF-8 容错）。"""
    text = bytes(raw).decode("utf-8", errors="replace")
    out = []
    for ln in text.replace("\r", "\n").split("\n"):
        ln = ln.strip()
        if ln:
            out.append(ln)
    return out


def run_tool(script, args, timeout=120):
    """跑同目录下的工具脚本，剥掉宿主 PYTHONHOME/PYTHONPATH 泄漏。"""
    env = dict(os.environ)
    env.pop("PYTHONHOME", None)
    env.pop("PYTHONPATH", None)
    cmd = [sys.executable, script] + list(args)
    p = subprocess.run(cmd, cwd=PROJECT_ROOT, capture_output=True,
                       text=True, timeout=timeout, env=env)
    return p.returncode, p.stdout or "", p.stderr or ""


def available_ports():
    try:
        from serial.tools import list_ports
    except Exception:
        return []
    out = []
    for p in list_ports.comports():
        desc = p.description or ""
        out.append((p.device, desc))
    return out


def human_size(n):
    for unit in ("B", "KB", "MB"):
        if n < 1024 or unit == "MB":
            return "%d %s" % (n, unit) if unit == "B" else "%.1f %s" % (n, unit)
        n /= 1024.0
    return "%d B" % n


def parse_u32(text, field):
    """接受 0x... 或十进制；失败就报错，不猜。"""
    text = str(text).strip()
    try:
        return int(text, 0)
    except ValueError:
        raise ValueError("%s: '%s' 不是合法整数（可写 0x08040000 或 131072）" % (field, text))


def parse_image_header(path):
    """解析 .svcapp 头；结构不对就抛错，返回 dict。"""
    data = open(path, "rb").read()
    if len(data) < IMG_HEADER_SIZE:
        raise ValueError("镜像小于 256 字节的固定头，不是 .svcapp")
    h = {}
    h["file_size"] = len(data)
    h["magic"] = struct.unpack_from("<I", data, 0)[0]
    if h["magic"] != IMG_MAGIC:
        raise ValueError("镜像魔数 0x%08X != 0x%08X（SVCA），不是 .svcapp"
                         % (h["magic"], IMG_MAGIC))
    h["type"] = struct.unpack_from("<I", data, IMG_OFF_TYPE)[0]
    h["hw_compat"] = struct.unpack_from("<I", data, IMG_OFF_HW_COMPAT)[0]
    h["version"] = struct.unpack_from("<I", data, IMG_OFF_VERSION)[0]
    h["image_size"] = struct.unpack_from("<I", data, IMG_OFF_IMAGE_SIZE)[0]
    h["entry_offset"] = struct.unpack_from("<I", data, IMG_OFF_ENTRY_OFFSET)[0]
    h["nominal_base"] = struct.unpack_from("<I", data, IMG_OFF_NOMINAL_BASE)[0]
    h["crc32"] = struct.unpack_from("<I", data, IMG_OFF_CRC32)[0]
    h["flags"] = struct.unpack_from("<I", data, IMG_OFF_FLAGS)[0]
    h["state"] = struct.unpack_from("<I", data, IMG_OFF_STATE)[0]
    h["image_id"] = struct.unpack_from("<I", data, IMG_OFF_IMAGE_ID)[0]
    h["ram_size"] = struct.unpack_from("<I", data, IMG_OFF_RAM_SIZE)[0]
    h["reloc_count"] = struct.unpack_from("<I", data, IMG_OFF_RELOC_COUNT)[0]
    h["reloc_kind"] = struct.unpack_from("<I", data, IMG_OFF_RELOC_KIND)[0]
    h["payload_offset"] = struct.unpack_from("<I", data, IMG_OFF_PAYLOAD_OFFSET)[0]
    h["nominal_ram_base"] = struct.unpack_from("<I", data, IMG_OFF_NOMINAL_RAM_BASE)[0]

    if h["type"] not in (IMG_TYPE_APP, IMG_TYPE_DRIVER):
        raise ValueError("镜像 type=%u 既不是 app(1) 也不是 driver(2)" % h["type"])
    expect_payload = IMG_HEADER_SIZE + h["reloc_count"] * 4
    if h["payload_offset"] != expect_payload:
        raise ValueError("payload_offset=%u 与 reloc_count=%u 推出的 %u 不一致"
                         % (h["payload_offset"], h["reloc_count"], expect_payload))
    total = h["payload_offset"] + h["image_size"]
    if total != h["file_size"]:
        raise ValueError("镜像长度对不上：头里算出 %u 字节，文件实际 %u 字节"
                         % (total, h["file_size"]))
    if h["state"] != 0:
        raise ValueError("镜像 state=%u（1=未提交）；未提交的镜像不该被安装"
                         % h["state"])
    h["autostart"] = bool(h["flags"] & IMG_FLAG_AUTOSTART)
    return h


# ---------------------------------------------------------------- 串口链路


class Link(object):
    """串口 + 单读者线程。

    只有一个线程真正读串口：普通状态下由读者线程读，收到分行后塞进队列给
    界面显示；当某个协议操作需要独占时（安装、写配置），把读者暂停，由操作
    自己读，用完恢复。这样在任何时刻都只有一个读者，不会互相偷字节。
    """

    def __init__(self):
        self.ser = None
        self._alive = True
        self._paused = threading.Event()
        self._lock = threading.RLock()
        self._lines = collections.deque(maxlen=4000)
        self._lines_lock = threading.Lock()
        self._thread = threading.Thread(target=self._reader, name="svcrt-rx",
                                        daemon=True)
        self._thread.start()

    # ---- 开关 ----

    @property
    def is_open(self):
        return self.ser is not None and getattr(self.ser, "is_open", False)

    def port_name(self):
        return getattr(self.ser, "port", None)

    def open(self, port, baud):
        import serial
        self.close()
        ser = serial.Serial(port, baud, timeout=0.05)
        self.ser = ser
        self.drain_lines()
        return ser

    def close(self):
        ser, self.ser = self.ser, None
        if ser is not None:
            try:
                ser.close()
            except Exception:
                pass

    def shutdown(self):
        self._alive = False
        self.close()

    # ---- 读者线程 ----

    def _reader(self):
        pending = bytearray()
        while self._alive:
            ser = self.ser
            if ser is None or not getattr(ser, "is_open", False) or self._paused.is_set():
                if pending:
                    self._push(bytes(pending))
                    pending = bytearray()
                time.sleep(0.03)
                continue
            try:
                data = ser.read(256)
            except Exception:
                time.sleep(0.05)
                continue
            if not data:
                continue
            pending += data
            while True:
                idx = -1
                for i, ch in enumerate(pending):
                    if ch in (0x0D, 0x0A):
                        idx = i
                        break
                if idx < 0:
                    break
                line = bytes(pending[:idx])
                del pending[:idx + 1]
                text = line.decode("utf-8", errors="replace").strip()
                if text:
                    self._push(text)
        if pending:
            self._push(bytes(pending))

    def _push(self, raw):
        """raw 可以是 bytes（原始流）或已经解码好的一行 str。"""
        if isinstance(raw, str):
            lines = [raw.strip()] if raw.strip() else []
        else:
            lines = device_lines(raw)
        for ln in lines:
            with self._lines_lock:
                self._lines.append(ln)

    def drain_lines(self):
        with self._lines_lock:
            self._lines.clear()

    def poll_lines(self, limit=400):
        out = []
        with self._lines_lock:
            while self._lines and len(out) < limit:
                out.append(self._lines.popleft())
        return out

    # ---- 独占 ----

    @contextlib.contextmanager
    def exclusive(self, settle_s=0.08):
        """暂停读者线程并把残留输入丢掉，交给调用者独占读写。"""
        if not self.is_open:
            raise RuntimeError("串口未打开")
        with self._lock:
            self._paused.set()
            time.sleep(settle_s)      # 等在途的那次 read 落地
            self.drain_lines()
            try:
                # 也丢掉驱动缓冲里的残留：上一帧失败时设备可能多回了字节，
                # 留着会被这一帧的 wait_flow 当成它的流控字节。
                self.ser.reset_input_buffer()
            except Exception:
                pass
            try:
                yield self
            finally:
                self._paused.clear()

    # ---- 独占期间可用的读写原语 ----

    def write(self, data):
        self.ser.write(data)
        self.ser.flush()

    def read_byte(self, deadline):
        while time.time() < deadline:
            b = self.ser.read(1)
            if b:
                return b
        return b""

    def collect(self, timeout, stops=(), quiet_s=None, on_line=None):
        """读到命中 stops 之一、或静默超过 quiet_s、或超时。

        返回 (hit, raw)；hit 表示命中了 stops 里的某个标记。
        """
        buf = bytearray()
        deadline = time.time() + timeout
        last = time.time()
        while time.time() < deadline:
            b = self.ser.read(1)
            if b:
                buf += b
                last = time.time()
                if on_line:
                    on_line(buf)
                hit = False
                for s in stops:
                    if s in buf:
                        hit = True
                        break
                if hit:
                    return True, bytes(buf)
                continue
            if quiet_s is not None and (time.time() - last) >= quiet_s:
                return False, bytes(buf)
        return False, bytes(buf)

    def wait_flow(self, timeout):
        """等一个流控字节，返回 (verdict, raw)。verdict ∈ ack/nak/timeout。"""
        buf = bytearray()
        deadline = time.time() + timeout
        while time.time() < deadline:
            b = self.ser.read(1)
            if not b:
                continue
            if b[0] == ACK_BYTE:
                return "ack", bytes(buf)
            if b[0] == NAK_BYTE:
                return "nak", bytes(buf)
            buf += b
        return "timeout", bytes(buf)


# ---------------------------------------------------------------- 界面

try:
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox
except ImportError as _e:                                  # pragma: no cover
    tk = None
    _TK_IMPORT_ERROR = _e

    class _Missing(object):
        pass
else:
    _TK_IMPORT_ERROR = None


class LogPane(ttk.Frame):
    """只读日志面板：写入即滚动到底，支持清屏与复制全部。"""

    def __init__(self, master, height=14, title=None):
        ttk.Frame.__init__(self, master)
        self._text = tk.Text(self, height=height, wrap="none",
                             background="#101418", foreground="#d8dee9",
                             insertbackground="#d8dee9", font=("Consolas", 9))
        ysb = ttk.Scrollbar(self, orient="vertical", command=self._text.yview)
        xsb = ttk.Scrollbar(self, orient="horizontal", command=self._text.xview)
        self._text.configure(yscrollcommand=ysb.set, xscrollcommand=xsb.set)
        self._text.grid(row=0, column=0, sticky="nsew")
        ysb.grid(row=0, column=1, sticky="ns")
        xsb.grid(row=1, column=0, sticky="ew")
        self.rowconfigure(0, weight=1)
        self.columnconfigure(0, weight=1)
        self._text.configure(state="disabled")

        bar = ttk.Frame(self)
        bar.grid(row=2, column=0, columnspan=2, sticky="ew", pady=(2, 0))
        if title:
            ttk.Label(bar, text=title).pack(side="left")
        ttk.Button(bar, text="清屏", width=8, command=self.clear).pack(side="right")
        ttk.Button(bar, text="复制全部", width=10,
                   command=self.copy_all).pack(side="right", padx=4)
        self._autoscroll = tk.BooleanVar(value=True)
        ttk.Checkbutton(bar, text="自动滚动", variable=self._autoscroll).pack(side="right")

    def clear(self):
        self._text.configure(state="normal")
        self._text.delete("1.0", "end")
        self._text.configure(state="disabled")

    def copy_all(self):
        try:
            self.clipboard_clear()
            self.clipboard_append(self._text.get("1.0", "end").strip())
        except Exception:
            pass

    def write(self, text, tag=None):
        if not text:
            return
        at_end = self._text.yview()[1] >= 0.999
        self._text.configure(state="normal")
        self._text.insert("end", text + "\n", tag or ())
        # 上限保护：超过 5000 行就丢掉最前面的
        if int(self._text.index("end-1c").split(".")[0]) > 5000:
            self._text.delete("1.0", "1000.0")
        self._text.configure(state="disabled")
        if self._autoscroll.get() or at_end:
            self._text.see("end")

    def write_lines(self, lines):
        for ln in lines:
            self.write(ln)


class SlotDialog(tk.Toplevel):
    """固定槽位一行记录的编辑对话框。"""

    TYPES = ("app", "driver")

    def __init__(self, master, slot=None, on_ok=None):
        tk.Toplevel.__init__(self, master)
        self.title("槽位")
        self.resizable(False, False)
        self.transient(master)
        self.grab_set()
        self.result = None
        self._on_ok = on_ok

        slot = slot or {"type": "app", "base": "0x08040000", "size": "0x20000",
                        "ram_base": "0x20010000", "ram_size": "0x1000",
                        "autostart": 1}

        self.var_type = tk.StringVar(value=slot.get("type", "app"))
        self.var_base = tk.StringVar(value=str(slot.get("base", "0x08040000")))
        self.var_size = tk.StringVar(value=str(slot.get("size", "0x20000")))
        self.var_ram_base = tk.StringVar(value=str(slot.get("ram_base", "")))
        self.var_ram_size = tk.StringVar(value=str(slot.get("ram_size", "")))
        self.var_autostart = tk.BooleanVar(value=bool(slot.get("autostart", 0)))

        rows = [
            ("类型", ttk.Combobox(self, textvariable=self.var_type, width=18,
                                 values=self.TYPES, state="readonly")),
            ("Flash 基址", ttk.Entry(self, textvariable=self.var_base, width=20)),
            ("Flash 长度", ttk.Entry(self, textvariable=self.var_size, width=20)),
            ("RAM 基址", ttk.Entry(self, textvariable=self.var_ram_base, width=20)),
            ("RAM 长度", ttk.Entry(self, textvariable=self.var_ram_size, width=20)),
        ]
        for i, (label, widget) in enumerate(rows):
            ttk.Label(self, text=label).grid(row=i, column=0, sticky="w",
                                             padx=(10, 6), pady=3)
            widget.grid(row=i, column=1, sticky="ew", padx=(0, 10), pady=3)
        ttk.Checkbutton(self, text="上电自启 (autostart)",
                        variable=self.var_autostart).grid(row=len(rows), column=1,
                                                          sticky="w", pady=3)
        ttk.Label(self, text="地址可取 0x 十六进制；长度必须是 2 的幂，\n"
                             "RAM 基址/长度留空表示由内核按 ram_size 分配。",
                  foreground="#666").grid(row=len(rows) + 1, column=0, columnspan=2,
                                          sticky="w", padx=10, pady=(2, 6))

        bar = ttk.Frame(self)
        bar.grid(row=len(rows) + 2, column=0, columnspan=2, sticky="e", pady=(0, 10))
        ttk.Button(bar, text="取消", width=10, command=self.destroy).pack(side="right",
                                                                        padx=(4, 10))
        ttk.Button(bar, text="确定", width=10, command=self._ok).pack(side="right")
        self.columnconfigure(1, weight=1)

    def _ok(self):
        try:
            base = parse_u32(self.var_base.get(), "Flash 基址")
            size = parse_u32(self.var_size.get(), "Flash 长度")
            if size <= 0 or (size & (size - 1)) != 0:
                raise ValueError("Flash 长度必须是 2 的幂（如 0x20000）")
            ram_base_txt = self.var_ram_base.get().strip()
            ram_size_txt = self.var_ram_size.get().strip()
            ram_base = parse_u32(ram_base_txt, "RAM 基址") if ram_base_txt else 0
            ram_size = parse_u32(ram_size_txt, "RAM 长度") if ram_size_txt else 0
            if ram_size and (ram_size & (ram_size - 1)) != 0:
                raise ValueError("RAM 长度必须是 2 的幂（如 0x1000）")
        except ValueError as e:
            messagebox.showerror("输入有误", str(e), parent=self)
            return
        self.result = {
            "type": self.var_type.get(),
            "base": "0x%08X" % base,
            "size": "0x%X" % size,
            "ram_base": ("0x%08X" % ram_base) if ram_base_txt else "",
            "ram_size": ("0x%X" % ram_size) if ram_size_txt else "",
            "autostart": 1 if self.var_autostart.get() else 0,
        }
        if callable(self._on_ok):
            self._on_ok(self.result)
        self.destroy()


# ---------------------------------------------------------------- 主窗口


class HostGui(object):
    def __init__(self, root):
        self.root = root
        self.link = Link()
        self.busy = threading.Event()
        self.var_port = tk.StringVar()
        self.var_baud = tk.StringVar(value=str(DEFAULT_BAUD))
        self.var_status = tk.StringVar(value="未连接")
        # 工作线程不能直接动 tk：日志与状态先入队，由主线程的 _poll_ui 落地。
        self._ui_logs = collections.deque()
        self._ui_calls = collections.deque()
        self._status_text = "未连接"
        self._status_shown = None
        self._progress = (0, 0)
        self._progress_shown = None
        self.var_slot_target = tk.StringVar(value="0")

        root.title("%s — %s" % (APP_TITLE, PROJECT_ROOT))
        root.minsize(980, 660)
        try:
            ttk.Style().theme_use("vista" if os.name == "nt" else "clam")
        except Exception:
            pass

        self.nb = ttk.Notebook(root)
        self.nb.pack(fill="both", expand=True, padx=8, pady=(8, 4))
        self._build_conn_tab()
        self._build_console_tab()
        self._build_install_tab()
        self._build_layout_tab()
        self._build_help_tab()

        bar = ttk.Frame(root)
        bar.pack(fill="x", side="bottom")
        ttk.Label(bar, textvariable=self.var_status, anchor="w").pack(
            side="left", fill="x", expand=True, padx=8, pady=4)

        self._refresh_ports()
        self.root.after(80, self._poll_ui)
        self.root.after(120, self._poll_console)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

    # ------------------------------------------------------------ 基础设施

    def log(self, pane, text):
        self._ui_logs.append((pane, text))

    def post(self, fn):
        """把一段必须在主线程执行的动作排进队列。"""
        self._ui_calls.append(fn)

    def log_lines(self, pane, lines):
        for ln in lines:
            self.log(pane, ln)

    def set_status(self, text):
        self._status_text = text

    def _poll_ui(self):
        """主线程：把工作线程排进来的日志 / 回调 / 状态落地到控件上。"""
        while self._ui_logs:
            pane, text = self._ui_logs.popleft()
            try:
                pane.write(text)
            except Exception:
                pass
        while self._ui_calls:
            fn = self._ui_calls.popleft()
            try:
                fn()
            except Exception:
                pass
        if self._status_shown != self._status_text:
            self._status_shown = self._status_text
            self.var_status.set(self._status_text)
        if self._progress_shown != self._progress:
            self._progress_shown = self._progress
            done, total = self._progress
            pct = 0 if total <= 0 else int(done * 1000 / total)
            if hasattr(self, "prog"):
                self.prog.configure(value=pct)
                self.lbl_prog.configure(text="%d %%" % (pct // 10))
        self.root.after(80, self._poll_ui)

    def run_async(self, fn, label):
        """在后台线程跑一个协议操作；同一时刻只允许一个。"""
        if self.busy.is_set():
            messagebox.showwarning("正在忙", "上一个操作还没结束，请等它完成或关掉串口。")
            return
        if not self.link.is_open:
            messagebox.showwarning("未连接", "请先在「连接」页打开串口。")
            return

        def worker():
            self.busy.set()
            self.set_status("%s …" % label)
            rc = 1
            try:
                rc = fn()
            except Exception as e:
                self.set_status("%s：失败（%s）" % (label, e))
                rc = 1
            else:
                self.set_status("%s：%s" % (label, "完成" if rc == 0 else "失败"))
            finally:
                self.busy.clear()
            return rc

        threading.Thread(target=worker, name="svcrt-op", daemon=True).start()

    def _poll_console(self):
        lines = self.link.poll_lines()
        if lines:
            self.log_lines(self.pane_console, ["[dev] " + ln for ln in lines])
        self.root.after(120, self._poll_console)

    def _on_close(self):
        if self.busy.is_set():
            if not messagebox.askyesno("仍在操作", "有操作正在执行，确定退出吗？"):
                return
        try:
            self.link.shutdown()
        finally:
            self.root.destroy()

    # ------------------------------------------------------------ 连接页

    def _build_conn_tab(self):
        tab = ttk.Frame(self.nb)
        self.nb.add(tab, text="连接")

        box = ttk.LabelFrame(tab, text="串口")
        box.pack(fill="x", padx=10, pady=10)
        ttk.Label(box, text="端口").grid(row=0, column=0, padx=(10, 6), pady=6, sticky="w")
        self.cmb_port = ttk.Combobox(box, textvariable=self.var_port, width=22)
        self.cmb_port.grid(row=0, column=1, sticky="w", pady=6)
        ttk.Button(box, text="刷新", width=8, command=self._refresh_ports).grid(
            row=0, column=2, padx=6)
        ttk.Label(box, text="波特率").grid(row=0, column=3, padx=(18, 6), sticky="w")
        ttk.Combobox(box, textvariable=self.var_baud, width=10,
                     values=BAUD_CHOICES).grid(row=0, column=4, sticky="w")
        self.btn_open = ttk.Button(box, text="打开串口", width=12, command=self._toggle_port)
        self.btn_open.grid(row=0, column=5, padx=12)
        ttk.Label(box, text="默认 115200；设备侧控制台固定 8N1。",
                  foreground="#666").grid(row=1, column=0, columnspan=6, sticky="w",
                                          padx=10, pady=(0, 8))
        box.columnconfigure(6, weight=1)

        quick = ttk.LabelFrame(tab, text="设备信息（一键发送，输出在「控制台」页）")
        quick.pack(fill="x", padx=10, pady=(0, 10))
        for i, (text, cmd) in enumerate([
                ("内核信息 (info)", "info"),
                ("应用列表 (app)", "app"),
                ("驱动列表 (drv)", "drv"),
                ("任务 (task)", "task"),
                ("故障 (fault)", "fault"),
                ("存储池 (pool)", "pool"),
                ("配置 (cfg show)", "cfg show"),
                ("看门狗/日志 (log)", "log")]):
            ttk.Button(quick, text=text, width=18,
                       command=lambda c=cmd: self.send_command(c)).grid(
                row=i // 4, column=i % 4, padx=6, pady=6, sticky="w")

        note = ("提示：`fault` 与 `log` 是排查安装/启动失败的第一站；\n"
                "`pool` 会列出镜像池与固定槽位（固定槽模式下才出现槽位行）。\n"
                "安装或写配置前建议先 `reboot`，让设备从干净状态开始。")
        ttk.Label(tab, text=note, justify="left", foreground="#444").pack(
            anchor="w", padx=14, pady=(4, 10))

        self.pane_conn = LogPane(tab, height=6, title="连接日志")
        self.pane_conn.pack(fill="both", expand=True, padx=10, pady=(0, 10))

    def _refresh_ports(self):
        ports = available_ports()
        values = ["%s — %s" % (dev, desc) if desc else dev for dev, desc in ports]
        self.cmb_port.configure(values=values)
        if values and not self.var_port.get():
            self.var_port.set(values[0])
        if not values:
            self.log(self.pane_conn, "没有发现串口；检查 DAPLink / USB-TTL 是否插好。")
        else:
            self.log(self.pane_conn, "发现 %d 个串口：%s" % (len(ports), ", ".join(values)))

    def _port_device(self):
        raw = self.var_port.get().strip()
        if not raw:
            return ""
        return raw.split(" ")[0].split(" —")[0].strip()

    def _toggle_port(self):
        if self.link.is_open:
            self.link.close()
            self.btn_open.configure(text="打开串口")
            self.set_status("已关闭串口")
            self.log(self.pane_conn, "串口已关闭")
            return
        dev = self._port_device()
        if not dev:
            messagebox.showwarning("未选端口", "请先选择或刷新串口列表。")
            return
        try:
            baud = int(self.var_baud.get())
        except ValueError:
            baud = DEFAULT_BAUD
        try:
            self.link.open(dev, baud)
        except Exception as e:
            messagebox.showerror("打开失败", "%s\n\n常见原因：端口被 Keil / 其他串口工具占用。" % e)
            return
        self.btn_open.configure(text="关闭串口")
        self.set_status("已连接 %s @ %d" % (dev, baud))
        self.log(self.pane_conn, "已打开 %s @ %d" % (dev, baud))

    # ------------------------------------------------------------ 控制台页

    def _build_console_tab(self):
        tab = ttk.Frame(self.nb)
        self.nb.add(tab, text="控制台")

        quick = ttk.LabelFrame(tab, text="常用命令")
        quick.pack(fill="x", padx=10, pady=10)
        cmds = [("info", "info"), ("app", "app"), ("drv", "drv"), ("task", "task"),
                ("fault", "fault"), ("pool", "pool"), ("trace", "trace"),
                ("cfg show", "cfg show")]
        for i, (text, cmd) in enumerate(cmds):
            ttk.Button(quick, text=text, width=12,
                       command=lambda c=cmd: self.send_command(c)).grid(
                row=0, column=i, padx=4, pady=6, sticky="w")

        bar = ttk.Frame(quick)
        bar.grid(row=1, column=0, columnspan=8, sticky="w", padx=4, pady=(0, 6))
        ttk.Label(bar, text="槽位").pack(side="left")
        ttk.Spinbox(bar, from_=0, to=15, width=4,
                    textvariable=self.var_slot_target).pack(side="left", padx=4)
        for text, fmt in (("启动", "app start %s"), ("停止", "app stop %s"),
                          ("卸载", "app uninstall %s"),
                          ("启动驱动", "drv start %s"), ("停止驱动", "drv stop %s"),
                          ("卸载驱动", "drv uninstall %s")):
            ttk.Button(bar, text=text, width=10,
                       command=lambda f=fmt: self.send_command(
                           f % self.var_slot_target.get())).pack(side="left", padx=3)
        ttk.Button(bar, text="重启设备", width=10,
                   command=lambda: self.send_command("reboot", quiet=1.2,
                                                     timeout=4.0)).pack(
            side="left", padx=(14, 3))

        self.pane_console = LogPane(tab, height=20, title="设备输出")
        self.pane_console.pack(fill="both", expand=True, padx=10, pady=(0, 6))

        entry_bar = ttk.Frame(tab)
        entry_bar.pack(fill="x", padx=10, pady=(0, 10))
        ttk.Label(entry_bar, text="命令").pack(side="left")
        self.var_cmd = tk.StringVar()
        ent = ttk.Entry(entry_bar, textvariable=self.var_cmd)
        ent.pack(side="left", fill="x", expand=True, padx=6)
        ent.bind("<Return>", lambda _e: self._send_entry())
        ttk.Button(entry_bar, text="发送", width=10, command=self._send_entry).pack(
            side="left")
        ttk.Label(entry_bar, text="（命令行以 CR 结尾发送，与串口终端一致）",
                  foreground="#666").pack(side="left", padx=8)

    def _send_entry(self):
        text = self.var_cmd.get().strip()
        if not text:
            return
        self.var_cmd.set("")
        # 手工输入的命令可能长（cfg load 进入接收态后会等 512 B），
        # 单独给一个更宽的等待窗口，避免看起来像卡住。
        quiet, timeout = (2.0, 8.0) if text.split()[0] in ("install", "cfg") else (0.7, 6.0)
        self.send_command(text, quiet=quiet, timeout=timeout)

    def send_command(self, text, quiet=0.7, timeout=6.0, pane=None):
        """发一条 shell 命令；设备输出原样写到指定日志面板（默认控制台页）。"""
        if not self.link.is_open:
            messagebox.showwarning("未连接", "请先在「连接」页打开串口。")
            return
        pane = pane if pane is not None else self.pane_console

        def op():
            with self.link.exclusive():
                self.link.write((text + "\r").encode("ascii", errors="replace"))
                _hit, raw = self.link.collect(timeout, quiet_s=quiet)
                lines = device_lines(raw)
            self.log(pane, "> " + text)
            if lines:
                self.log_lines(pane, lines)
            else:
                self.log(pane, "(没有输出：命令可能仍在等待输入，或设备正忙)")
            return 0

        self.run_async(op, "命令 %s" % text)

    # ------------------------------------------------------------ 安装页

    def _build_install_tab(self):
        tab = ttk.Frame(self.nb)
        self.nb.add(tab, text="安装镜像")

        box = ttk.LabelFrame(tab, text="镜像文件 (.svcapp)")
        box.pack(fill="x", padx=10, pady=10)
        self.var_image = tk.StringVar()
        ttk.Entry(box, textvariable=self.var_image).grid(
            row=0, column=0, columnspan=3, sticky="ew", padx=(10, 6), pady=8)
        ttk.Button(box, text="浏览…", width=10,
                   command=self._browse_image).grid(row=0, column=3, padx=4)
        ttk.Button(box, text="查看信息", width=10,
                   command=self._show_image_info).grid(row=0, column=4, padx=(4, 10))
        box.columnconfigure(0, weight=1)

        self.lbl_image_info = ttk.Label(box, text="尚未选择镜像", justify="left",
                                        foreground="#333")
        self.lbl_image_info.grid(row=1, column=0, columnspan=5, sticky="w",
                                 padx=10, pady=(0, 8))

        opt = ttk.LabelFrame(tab, text="安装方式与后续动作")
        opt.pack(fill="x", padx=10)
        self.var_fixed = tk.BooleanVar(value=False)
        ttk.Radiobutton(opt, text="自动位置（内核按最大密度找落点）",
                        variable=self.var_fixed, value=False).grid(
            row=0, column=0, sticky="w", padx=10, pady=6)
        ttk.Radiobutton(opt, text="指定固定槽位",
                        variable=self.var_fixed, value=True).grid(
            row=0, column=1, sticky="w", padx=10)
        ttk.Spinbox(opt, from_=0, to=15, width=4,
                    textvariable=self.var_slot_target).grid(row=0, column=2, sticky="w")
        ttk.Label(opt, text="（仅固定槽位模式有效；auto 模式下带槽位号会被拒）",
                  foreground="#666").grid(row=0, column=3, sticky="w", padx=8)

        self.var_pre_reboot = tk.BooleanVar(value=True)
        self.var_post_start = tk.BooleanVar(value=True)
        self.var_post_reboot = tk.BooleanVar(value=False)
        ttk.Checkbutton(opt, text="安装前先重启设备（推荐）",
                        variable=self.var_pre_reboot).grid(row=1, column=0,
                                                           sticky="w", padx=10, pady=(0, 8))
        ttk.Checkbutton(opt, text="安装成功后启动",
                        variable=self.var_post_start).grid(row=1, column=1, sticky="w")
        ttk.Checkbutton(opt, text="安装成功后重启",
                        variable=self.var_post_reboot).grid(row=1, column=3, sticky="w")

        act = ttk.Frame(tab)
        act.pack(fill="x", padx=10, pady=10)
        self.btn_install = ttk.Button(act, text="开始安装", width=14,
                                      command=self._install)
        self.btn_install.pack(side="left")
        self.btn_abort = ttk.Button(act, text="中止", width=8, state="disabled",
                                    command=self._abort_install)
        self.btn_abort.pack(side="left", padx=8)
        self.prog = ttk.Progressbar(act, mode="determinate", maximum=1000)
        self.prog.pack(side="left", fill="x", expand=True, padx=10)
        self.lbl_prog = ttk.Label(act, text="0 %", width=8)
        self.lbl_prog.pack(side="left")

        self.pane_install = LogPane(tab, height=16, title="安装日志")
        self.pane_install.pack(fill="both", expand=True, padx=10, pady=(0, 10))
        self.install_abort = threading.Event()

    def _browse_image(self):
        path = filedialog.askopenfilename(
            title="选择 .svcapp 镜像",
            filetypes=[("SVCrtOS 镜像", "*.svcapp"), ("所有文件", "*.*")])
        if path:
            self.var_image.set(path)
            self._show_image_info()

    def _show_image_info(self):
        path = self.var_image.get().strip()
        if not path:
            return
        try:
            h = parse_image_header(path)
        except Exception as e:
            self.lbl_image_info.configure(text="无法解析：%s" % e, foreground="#a00")
            return
        kind = "应用 (app)" if h["type"] == IMG_TYPE_APP else "驱动 (driver)"
        self.lbl_image_info.configure(
            text=("类型 %s   版本 0x%08X   hw_compat 0x%08X   %s\n"
                  "负载 %s   重定位表 %d 项（偏移按半字，kind=%u）   RAM %s\n"
                  "入口偏移 0x%X   标称基址 0x%08X   自启 %s   image_id 0x%08X"
                  % (kind, h["version"], h["hw_compat"], human_size(h["file_size"]),
                     human_size(h["image_size"]), h["reloc_count"], h["reloc_kind"],
                     human_size(h["ram_size"]), h["entry_offset"], h["nominal_base"],
                     "是" if h["autostart"] else "否", h["image_id"])),
            foreground="#333")

    def _set_progress(self, done, total):
        self._progress = (done, total)

    def _abort_install(self):
        self.install_abort.set()
        self.log(self.pane_install, "已请求中止：当前分块发完后不会再发。")

    def _install(self):
        path = self.var_image.get().strip()
        if not path or not os.path.isfile(path):
            messagebox.showwarning("没有镜像", "请先选择存在的 .svcapp 文件。")
            return
        if not self.link.is_open:
            messagebox.showwarning("未连接", "请先在「连接」页打开串口。")
            return
        try:
            h = parse_image_header(path)
        except Exception as e:
            messagebox.showerror("镜像不可用", "%s" % e)
            return
        data = open(path, "rb").read()

        fixed = bool(self.var_fixed.get())
        try:
            slot = int(self.var_slot_target.get())
        except ValueError:
            slot = 0
        pre_reboot = self.var_pre_reboot.get()
        post_start = self.var_post_start.get()
        post_reboot = self.var_post_reboot.get()

        self.install_abort.clear()
        self.prog.configure(value=0)
        self.lbl_prog.configure(text="0 %")

        reloc_len = h["reloc_count"] * 4
        payload_off = h["payload_offset"]
        image_size = h["image_size"]
        log = self.pane_install

        def op():
            self.post(lambda: self.btn_abort.configure(state="normal"))
            try:
                with self.link.exclusive():
                    self.log(log, "镜像 %s" % path)
                    self.log(log, "  %d 字节 = 头 256 + 重定位表 %d + 负载 %d"
                                  % (len(data), reloc_len, image_size))

                    if pre_reboot:
                        self.log(log, "先重启设备…")
                        self.link.write(b"reboot\r")
                        _ok, raw = self.link.collect(3.0, quiet_s=1.2)
                        self.log_lines(log, device_lines(raw))
                        time.sleep(0.3)

                    cmd = ("install %d" % slot) if fixed else "install"
                    self.log(log, "> " + cmd)
                    self.link.write((cmd + "\r").encode("ascii"))
                    # 停止条件只是「别再无谓地等下去」，能不能开装要看设备有没有
                    # 明确说 'send the file now'：设备拒收命令时回的是 usage/err 行，
                    # 那时若照发镜像，字节会落到 shell 里变成一串乱码命令。
                    hit, raw = self.link.collect(
                        4.0, stops=(b"send the file now", b"usage:", b"no such slot",
                                    b"install: failed"))
                    self.log_lines(log, device_lines(raw))
                    if b"send the file now" not in raw:
                        self.log(log, "设备没有进入安装窗口（没有 'send the file now'）："
                                      "检查是否已在固定槽模式下、槽位号是否在槽位表范围内。")
                        return 1

                    # 头 + 重定位表作为一次突发：设备要先把表写到 Flash 才能 ACK。
                    self.link.write(data[:IMG_HEADER_SIZE])
                    time.sleep(INSTALL_HEADER_SETTLE)
                    self.link.write(data[IMG_HEADER_SIZE:IMG_HEADER_SIZE + reloc_len])
                    verdict, raw = self.link.wait_flow(INSTALL_ACK_TIMEOUT)
                    self.log_lines(log, device_lines(raw))
                    if verdict != "ack":
                        self.log(log, "头/重定位表被拒绝（%s）——原因见设备打印的那一行。"
                                      % ("设备 NAK" if verdict == "nak" else "超时"))
                        return 1

                    sent = payload_off
                    while sent < payload_off + image_size:
                        if self.install_abort.is_set():
                            self.log(log, "已中止：停在偏移 %d" % sent)
                            return 1
                        piece = data[sent:sent + INSTALL_CHUNK]
                        self.link.write(piece)
                        verdict, raw = self.link.wait_flow(INSTALL_ACK_TIMEOUT)
                        if raw:
                            self.log_lines(log, device_lines(raw))
                        if verdict != "ack":
                            self.log(log, "偏移 %d 处分块被拒绝（%s）"
                                          % (sent, "设备 NAK" if verdict == "nak" else "超时"))
                            return 1
                        sent += len(piece)
                        self._set_progress(sent - payload_off, image_size)

                    _hit, raw = self.link.collect(2.0, quiet_s=0.6)
                    lines = device_lines(raw)
                    self.log_lines(log, lines)
                    self._set_progress(image_size, image_size)

                    installed = None
                    for ln in lines:
                        if "install: ok" in ln:
                            tail = ln.split("slot")[-1].strip()
                            if tail.isdigit():
                                installed = int(tail)
                    if installed is None and fixed:
                        installed = slot
                    if installed is None:
                        self.log(log, "传输完成，但没解析到 'install: ok, slot N'；"
                                      "用 `app` / `drv` / `pool` 确认结果。")
                        return 0
                    self.log(log, "安装完成：槽位 %d" % installed)

                    if post_start:
                        start_cmd = ("app start %d" if h["type"] == IMG_TYPE_APP
                                     else "drv start %d") % installed
                        self.log(log, "> " + start_cmd)
                        self.link.write((start_cmd + "\r").encode("ascii"))
                        _ok, raw = self.link.collect(3.0, quiet_s=0.8)
                        self.log_lines(log, device_lines(raw))
                    if post_reboot:
                        self.link.write(b"reboot\r")
                        _ok, raw = self.link.collect(3.0, quiet_s=1.2)
                        self.log_lines(log, device_lines(raw))
                    return 0
            finally:
                self.post(lambda: self.btn_abort.configure(state="disabled"))

        self.run_async(op, "安装镜像")

    # ------------------------------------------------------------ 布局配置页

    def _build_layout_tab(self):
        tab = ttk.Frame(self.nb)
        self.nb.add(tab, text="布局配置")

        top = ttk.Frame(tab)
        top.pack(fill="x", padx=10, pady=10)

        pol = ttk.LabelFrame(top, text="策略（写入后需重启才生效）")
        pol.pack(side="left", fill="both", expand=True)
        self.var_mode = tk.StringVar(value="auto")
        self.var_reclaim = tk.StringVar(value="global")
        self.var_flags = tk.StringVar(value="0")
        self.var_log_level = tk.StringVar(value="0")
        self.var_restart_max = tk.StringVar(value="0")
        self.var_boot_delay = tk.StringVar(value="0")

        ttk.Label(pol, text="安装落点").grid(row=0, column=0, sticky="w", padx=(10, 6), pady=5)
        ttk.Radiobutton(pol, text="auto（内核自适应）", variable=self.var_mode,
                        value="auto", command=self._mode_changed).grid(
            row=0, column=1, sticky="w")
        ttk.Radiobutton(pol, text="fixed（按槽位表）", variable=self.var_mode,
                        value="fixed", command=self._mode_changed).grid(
            row=0, column=2, sticky="w", padx=(10, 0))

        ttk.Label(pol, text="卸载回收力度").grid(row=1, column=0, sticky="w",
                                                padx=(10, 6), pady=5)
        ttk.Combobox(pol, textvariable=self.var_reclaim, width=12,
                     values=("global", "minimal", "none"), state="readonly").grid(
            row=1, column=1, sticky="w")
        ttk.Label(pol, text="global=能压就压  minimal=只回收同单元  none=不回收",
                  foreground="#666").grid(row=1, column=2, columnspan=3, sticky="w",
                                          padx=8)

        ttk.Label(pol, text="配置标志 flags").grid(row=2, column=0, sticky="w",
                                                   padx=(10, 6), pady=5)
        ttk.Entry(pol, textvariable=self.var_flags, width=12).grid(row=2, column=1,
                                                                   sticky="w")
        ttk.Label(pol, text="日志级别").grid(row=2, column=2, sticky="w", padx=(10, 6))
        ttk.Entry(pol, textvariable=self.var_log_level, width=8).grid(row=2, column=3,
                                                                      sticky="w")
        ttk.Label(pol, text="故障重启上限").grid(row=3, column=0, sticky="w",
                                                 padx=(10, 6), pady=5)
        ttk.Entry(pol, textvariable=self.var_restart_max, width=12).grid(row=3, column=1,
                                                                         sticky="w")
        ttk.Label(pol, text="开机保持(ms)").grid(row=3, column=2, sticky="w",
                                                 padx=(10, 6))
        ttk.Entry(pol, textvariable=self.var_boot_delay, width=8).grid(row=3, column=3,
                                                                       sticky="w")
        ttk.Label(pol, text="0 表示“不覆盖”——留 0 就沿用编译期默认。",
                  foreground="#666").grid(row=4, column=0, columnspan=4, sticky="w",
                                          padx=10, pady=(0, 8))

        files = ttk.LabelFrame(top, text="文件位置")
        files.pack(side="left", fill="both", expand=True, padx=(10, 0))
        self.var_json = tk.StringVar(
            value=os.path.join(PROJECT_ROOT, "build", "layout.json"))
        self.var_bin = tk.StringVar(
            value=os.path.join(PROJECT_ROOT, "build", "layout_record.bin"))
        self.var_sctdir = tk.StringVar(value="")
        for i, (label, var, is_dir) in enumerate([
                ("JSON 描述", self.var_json, False),
                ("记录 .bin", self.var_bin, False),
                ("槽位 .sct 目录", self.var_sctdir, True)]):
            ttk.Label(files, text=label).grid(row=i, column=0, sticky="w",
                                              padx=(10, 6), pady=5)
            ttk.Entry(files, textvariable=var, width=34).grid(row=i, column=1,
                                                              sticky="ew", pady=5)
            ttk.Button(files, text="…", width=3,
                       command=lambda v=var, d=is_dir: self._pick_path(v, d)).grid(
                row=i, column=2, padx=(4, 10))
        files.columnconfigure(1, weight=1)

        slots = ttk.LabelFrame(tab, text="固定槽位表（仅 fixed 模式使用）")
        slots.pack(fill="both", expand=True, padx=10)
        cols = ("idx", "type", "base", "size", "ram_base", "ram_size", "autostart")
        heads = ("#", "类型", "Flash 基址", "Flash 长度", "RAM 基址", "RAM 长度", "自启")
        self.tree = ttk.Treeview(slots, columns=cols, show="headings", height=6)
        for c, h in zip(cols, heads):
            self.tree.heading(c, text=h)
            self.tree.column(c, width=110 if c != "idx" else 40, anchor="w")
        self.tree.pack(side="left", fill="both", expand=True, padx=(10, 0), pady=8)

        side = ttk.Frame(slots)
        side.pack(side="left", fill="y", padx=10, pady=8)
        for text, cmd in (("添加", self._slot_add), ("编辑", self._slot_edit),
                          ("删除", self._slot_del), ("上移", lambda: self._slot_move(-1)),
                          ("下移", lambda: self._slot_move(1))):
            ttk.Button(side, text=text, width=8, command=cmd).pack(pady=2)

        acts = ttk.Frame(tab)
        acts.pack(fill="x", padx=10, pady=10)
        for text, cmd in (
                ("载入模板", self._layout_template),
                ("导入 JSON", self._layout_import),
                ("导出 JSON", self._layout_export),
                ("校验", self._layout_check),
                ("生成记录", self._layout_build),
                ("写入设备", self._layout_write),
                ("读回设备", self._layout_show),
                ("擦除配置", self._layout_clear),
                ("打印宏定义", self._layout_emit_header)):
            ttk.Button(acts, text=text, width=11, command=cmd).pack(side="left", padx=3)

        self.pane_layout = LogPane(tab, height=10, title="配置日志")
        self.pane_layout.pack(fill="both", expand=True, padx=10, pady=(0, 10))
        self.slots = []
        self._mode_changed()
        self._layout_template(quiet=True)

    def _pick_path(self, var, is_dir):
        path = (filedialog.askdirectory(title="选择目录") if is_dir
                else filedialog.asksaveasfilename(title="选择文件"))
        if path:
            var.set(path)

    def _mode_changed(self):
        fixed = self.var_mode.get() == "fixed"
        state = "normal" if fixed else "disabled"
        for child in self.tree.master.winfo_children():
            if isinstance(child, ttk.Frame):
                for b in child.winfo_children():
                    try:
                        b.configure(state=state)
                    except Exception:
                        pass
        self.tree.configure(selectmode="extended" if fixed else "none")

    # ---- 槽位表 ----

    def _slots_to_tree(self):
        self.tree.delete(*self.tree.get_children())
        for i, s in enumerate(self.slots):
            self.tree.insert("", "end", iid=str(i),
                             values=(i, s.get("type", ""), s.get("base", ""),
                                     s.get("size", ""), s.get("ram_base", "") or "-",
                                     s.get("ram_size", "") or "-",
                                     "是" if s.get("autostart") else "否"))

    def _slot_selected(self):
        sel = self.tree.selection()
        return int(sel[0]) if sel else None

    def _slot_add(self):
        dlg = SlotDialog(self.root, None, on_ok=self._slot_added)
        dlg.wait_window()

    def _slot_added(self, rec):
        self.slots.append(rec)
        self._slots_to_tree()

    def _slot_edit(self):
        i = self._slot_selected()
        if i is None:
            return
        dlg = SlotDialog(self.root, self.slots[i],
                         on_ok=lambda rec: (self.slots.__setitem__(i, rec),
                                            self._slots_to_tree()))
        dlg.wait_window()

    def _slot_del(self):
        sel = sorted((int(x) for x in self.tree.selection()), reverse=True)
        for i in sel:
            del self.slots[i]
        self._slots_to_tree()

    def _slot_move(self, delta):
        i = self._slot_selected()
        if i is None:
            return
        j = i + delta
        if not (0 <= j < len(self.slots)):
            return
        self.slots[i], self.slots[j] = self.slots[j], self.slots[i]
        self._slots_to_tree()
        self.tree.selection_set(str(j))

    # ---- 描述集合 ----

    def _layout_dict(self):
        return {
            "mode": self.var_mode.get(),
            "reclaim_mode": self.var_reclaim.get(),
            "flags": parse_u32(self.var_flags.get() or "0", "flags"),
            "knobs": {
                "log_level": parse_u32(self.var_log_level.get() or "0", "日志级别"),
                "fault_restart_max": parse_u32(self.var_restart_max.get() or "0",
                                               "故障重启上限"),
                "boot_delay_ms": parse_u32(self.var_boot_delay.get() or "0", "开机保持"),
            },
            "slots": [dict(s) for s in self.slots],
        }

    def _layout_apply(self, d):
        self.var_mode.set(d.get("mode", "auto"))
        self.var_reclaim.set(d.get("reclaim_mode", "global"))
        self.var_flags.set(str(d.get("flags", 0)))
        k = d.get("knobs", {}) or {}
        self.var_log_level.set(str(k.get("log_level", 0)))
        self.var_restart_max.set(str(k.get("fault_restart_max", 0)))
        self.var_boot_delay.set(str(k.get("boot_delay_ms", 0)))
        self.slots = [dict(s) for s in (d.get("slots") or [])]
        self._slots_to_tree()
        self._mode_changed()

    def _write_json_file(self):
        d = self._layout_dict()
        path = self.var_json.get().strip()
        if not path:
            path = os.path.join(PROJECT_ROOT, "build", "layout.json")
        d = dict(d, slots=[dict(s) for s in self.slots])
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(d, f, indent=2, ensure_ascii=False)
        return path

    # ---- 工具调用 ----

    def _layout_template(self, quiet=False):
        mode = self.var_mode.get()
        rc, out, err = run_tool(LAYOUT_TOOL, ["template", "--mode", mode])
        if rc != 0:
            self.log(self.pane_layout, "模板生成失败：%s" % (err.strip() or out.strip()))
            return
        try:
            d = json.loads(out)
        except Exception as e:
            self.log(self.pane_layout, "模板不是合法 JSON：%s" % e)
            return
        # 保留用户已经编好的槽位表（切模式时不至于把表清空）
        keep = self.slots if (mode == "fixed" and self.slots) else d.get("slots", [])
        d["slots"] = keep
        self._layout_apply(d)
        if not quiet:
            self.log(self.pane_layout, "已载入 %s 模式模板（%d 个槽位）"
                                       % (mode, len(self.slots)))

    def _layout_import(self):
        path = self.var_json.get().strip()
        if not path or not os.path.isfile(path):
            path = filedialog.askopenfilename(title="选择布局 JSON",
                                              filetypes=[("JSON", "*.json")])
        if not path:
            return
        try:
            d = json.load(open(path, "r", encoding="utf-8"))
        except Exception as e:
            self.log(self.pane_layout, "读取失败：%s" % e)
            return
        self.var_json.set(path)
        self._layout_apply(d)
        self.log(self.pane_layout, "已从 %s 导入" % path)

    def _layout_export(self):
        try:
            path = self._write_json_file()
        except Exception as e:
            self.log(self.pane_layout, "写出失败：%s" % e)
            return
        self.log(self.pane_layout, "已写出 %s" % path)

    def _layout_check(self):
        try:
            path = self._write_json_file()
        except Exception as e:
            self.log(self.pane_layout, "写出失败：%s" % e)
            return
        self.log(self.pane_layout, "校验 %s" % path)

        def worker():
            rc, out, err = run_tool(LAYOUT_TOOL, ["check", "--config", path])
            for ln in (out or "").splitlines():
                self.log(self.pane_layout, ln)
            for ln in (err or "").splitlines():
                self.log(self.pane_layout, "!! " + ln)
            self.log(self.pane_layout, "校验%s" % ("通过" if rc == 0 else "失败"))
        threading.Thread(target=worker, daemon=True).start()

    def _layout_emit_header(self):
        try:
            path = self._write_json_file()
        except Exception as e:
            self.log(self.pane_layout, "写出失败：%s" % e)
            return
        rc, out, err = run_tool(LAYOUT_TOOL, ["build", "--config", path, "--emit-header"])
        for ln in (out or "").splitlines():
            self.log(self.pane_layout, ln)
        if err.strip():
            self.log(self.pane_layout, "!! " + err.strip())
        if rc != 0:
            self.log(self.pane_layout, "打印失败（退出码 %d）" % rc)

    def _layout_build(self, quiet=False):
        """生成 512 字节记录；返回记录路径，失败抛异常。"""
        path = self._write_json_file()
        binpath = self.var_bin.get().strip() or os.path.join(PROJECT_ROOT, "build",
                                                             "layout_record.bin")
        os.makedirs(os.path.dirname(binpath) or ".", exist_ok=True)
        args = ["build", "--config", path, "--bin", binpath]
        sctdir = self.var_sctdir.get().strip()
        if sctdir:
            os.makedirs(sctdir, exist_ok=True)
            args += ["--sct-dir", sctdir]
        rc, out, err = run_tool(LAYOUT_TOOL, args)
        for ln in (out or "").splitlines():
            if not quiet:
                self.log(self.pane_layout, ln)
        if err.strip():
            self.log(self.pane_layout, "!! " + err.strip())
        if rc != 0:
            raise RuntimeError("svcrt_layout.py build 失败（退出码 %d）" % rc)
        rec = open(binpath, "rb").read()
        if len(rec) != CFG_RECORD_SIZE:
            raise RuntimeError("记录长度 %d != %d" % (len(rec), CFG_RECORD_SIZE))
        magic, = struct.unpack_from("<I", rec, 0)
        if magic != CFG_MAGIC:
            raise RuntimeError("记录魔数 0x%08X != 0x%08X" % (magic, CFG_MAGIC))
        self.var_bin.set(binpath)
        if not quiet:
            self.log(self.pane_layout, "已生成 %s（%d 字节）" % (binpath, len(rec)))
        return binpath

    # ---- 设备侧：写入 / 读回 / 擦除 ----

    def run_async_ex(self, fn, label, on_done=None):
        """run_async 的带回调版本：fn 结束时在 UI 线程上回调 on_done(rc)。"""
        if self.busy.is_set():
            messagebox.showwarning("正在忙", "上一个操作还没结束。")
            return
        if not self.link.is_open:
            messagebox.showwarning("未连接", "请先在「连接」页打开串口。")
            return

        def worker():
            self.busy.set()
            self.set_status("%s …" % label)
            rc = 1
            try:
                rc = fn()
            except Exception as e:
                self.set_status("%s：失败（%s）" % (label, e))
                rc = 1
            else:
                self.set_status("%s：%s" % (label, "完成" if rc == 0 else "失败"))
            finally:
                self.busy.clear()
            if on_done is not None:
                self.post(lambda: on_done(rc))

        threading.Thread(target=worker, name="svcrt-op", daemon=True).start()

    def _layout_write(self):
        try:
            binpath = self._layout_build(quiet=True)
        except Exception as e:
            messagebox.showerror("生成记录失败", "%s" % e)
            return
        rec = open(binpath, "rb").read()
        log = self.pane_layout

        def op():
            with self.link.exclusive():
                self.log(log, "写入 %s（%d 字节，4 x %d 分块）"
                              % (binpath, len(rec), CFG_CHUNK_SIZE))
                # CR 结尾：CRLF 残留的 LF 会顶偏整条记录。
                self.link.write(b"cfg load\r")
                _hit, raw = self.link.collect(CFG_READY_TIMEOUT,
                                              stops=(b"0x15 = stop",),
                                              quiet_s=CFG_QUIET_S)
                self.log_lines(log, device_lines(raw))
                if b"ready" not in raw:
                    self.log(log, "设备没有进入接收状态（没看到 'ready' 行）")
                    return 1
                chunks = [rec[i:i + CFG_CHUNK_SIZE]
                          for i in range(0, CFG_RECORD_SIZE, CFG_CHUNK_SIZE)]
                for i, chunk in enumerate(chunks):
                    last = (i + 1) == len(chunks)
                    self.link.write(chunk)
                    verdict, raw = self.link.wait_flow(
                        CFG_LAST_TIMEOUT if last else CFG_CHUNK_TIMEOUT)
                    self.log_lines(log, device_lines(raw))
                    if verdict != "ack":
                        self.log(log, "第 %d/%d 块被拒绝（%s）——原因见上面设备行；"
                                      "bad magic 通常意味着流被顶偏了一字节。"
                                      % (i + 1, len(chunks),
                                         "设备 NAK" if verdict == "nak" else "超时"))
                        return 1
                self.log(log, "记录已被设备接受并存储")
                return 0

        def done(rc):
            if rc != 0:
                return
            if messagebox.askyesno("写入成功", "记录已写入。\n\n现在重启设备让策略生效吗？\n"
                                              "（重启后自动读回一次）"):
                self._layout_reboot_and_show()
            else:
                self.log(self.pane_layout, "未重启：策略在下次重启后生效。")
                self._layout_show()

        self.run_async_ex(op, "写入布局配置", done)

    def _layout_reboot_and_show(self):
        def op():
            with self.link.exclusive():
                self.log(self.pane_layout, "> reboot")
                self.link.write(b"reboot\r")
                _ok, raw = self.link.collect(4.0, quiet_s=1.2)
                self.log_lines(self.pane_layout, device_lines(raw))
                time.sleep(0.3)
                self.log(self.pane_layout, "> cfg show")
                self.link.write(b"cfg show\r")
                _ok, raw = self.link.collect(4.0, quiet_s=CFG_QUIET_S)
                self.log_lines(self.pane_layout, device_lines(raw))
            return 0

        self.run_async(op, "重启并读回")

    def _layout_show(self):
        self.send_command("cfg show", pane=self.pane_layout, quiet=CFG_QUIET_S)

    def _layout_clear(self):
        if not messagebox.askyesno("擦除配置",
                                   "擦除设备上的配置区记录，回到编译期默认布局？\n"
                                   "（不可撤销；之后需要重启设备）"):
            return
        self.send_command("cfg clear", pane=self.pane_layout, quiet=0.8)

    # ------------------------------------------------------------ 帮助页

    def _build_help_tab(self):
        tab = ttk.Frame(self.nb)
        self.nb.add(tab, text="帮助")
        text = tk.Text(tab, wrap="word", height=20, font=("Microsoft YaHei UI", 9))
        ysb = ttk.Scrollbar(tab, orient="vertical", command=text.yview)
        text.configure(yscrollcommand=ysb.set)
        text.pack(side="left", fill="both", expand=True, padx=(10, 0), pady=10)
        ysb.pack(side="left", fill="y", pady=10)

        text.insert("end", HELP_TEXT)
        text.configure(state="disabled")


HELP_TEXT = """SVCrtOS 上位机 —— 使用说明
================================

一、推荐操作顺序
    1. 连接页选串口（DAPLink 的虚拟串口或 USB-TTL），115200，打开；
    2. 控制台页 `info` 确认内核在跑、`pool` 看镜像池现状；
    3. 要改安装策略：布局配置页 → 选 auto / fixed → 编辑槽位表 → 校验 →
       生成记录 → 写入设备（重启后生效）；
    4. 要装应用/驱动：安装镜像页选 .svcapp → 查看信息核对 hw_compat →
       决定自动落点还是指定槽位 → 开始安装；
    5. 出错先看控制台页的 `fault` 与 `log`，再看安装日志里设备打印的原因行。

二、三条协议约定（界面已内建，手工敲命令也要守）
    1. shell 以 CR 断行。发 `cfg load` 用 CRLF 时，残留的 LF 会占掉记录第 1 个
       字节，设备收满 512 字节后报 bad magic——这是"配置写不进去"的头号原因。
    2. `cfg load` 的语义是"进入接收状态等 512 字节"，不是"重新读一遍"；手工敲
       会等到超时并回 timed out waiting for the record。
    3. 安装窗口（`install [slot]`）与配置接收窗口期间，串口只能有一个读者。
       本程序用"暂停读线程"来保证；用别的串口工具同时开着会丢字节。

三、安装协议要点
    * 一次发送：256 字节镜像头 + 重定位表（reloc_count x 4）作为突发；
    * 之后按 512 字节分块，每块等设备一个流控字节：0x06=继续、0x15=停止；
    * 流控字节不带原因码，原因一定在设备随后的打印行里（err=SVCRT_LOADER_ERR_x）；
    * 固定槽模式下 `install <slot>`，auto 模式下直接 `install`（带槽位号会被拒）。

四、配置记录要点
    * 512 字节记录，分 4 块 x 128 字节上传，逐块等流控字节；
    * 记录由 tools/svcrt_layout.py 从 JSON 生成，magic = "SCFG"；
    * 写入生效需重启；擦除用 `cfg clear`，之后回编译期默认布局。

五、命令行等价物（本界面就是它们的图形外壳）
    tools/send_image.py --port COM3 image.svcapp          安装
    tools/svcrt_cfg.py show|write|clear --port COM3       配置区读写
    tools/svcrt_layout.py template|check|build            布局生成与校验
    tools/pack_app.py --project ... --out APP.svcapp      打包镜像

六、相关文档
    docs/SVCrtOS应用安装与调试指南.md     （含第 11 节：交给 AI 代理调试）
    docs/配置区与安装策略.md              （配置区 ABI 与 CLI 细节）
    docs/内核Shell控制台使用说明.md        （shell 命令表）
"""


def selftest():
    """无窗口自检：不碰串口，只验证解析、工具调用与界面能建起来。"""
    problems = []

    if tk is None:
        problems.append("tkinter 不可用：%s" % _TK_IMPORT_ERROR)
    if not os.path.isfile(LAYOUT_TOOL):
        problems.append("找不到 %s" % LAYOUT_TOOL)

    # 1) 镜像头解析：正例 + 负例
    import tempfile
    hdr = bytearray(IMG_HEADER_SIZE)
    struct.pack_into("<I", hdr, 0, IMG_MAGIC)
    struct.pack_into("<I", hdr, IMG_OFF_TYPE, IMG_TYPE_APP)
    struct.pack_into("<I", hdr, IMG_OFF_HW_COMPAT, 0x42700005)
    struct.pack_into("<I", hdr, IMG_OFF_IMAGE_SIZE, 32)
    struct.pack_into("<I", hdr, IMG_OFF_RELOC_COUNT, 0)
    struct.pack_into("<I", hdr, IMG_OFF_PAYLOAD_OFFSET, IMG_HEADER_SIZE)
    tmpd = tempfile.mkdtemp(prefix="svcrt_gui_selftest_")
    good = os.path.join(tmpd, "good.svcapp")
    open(good, "wb").write(bytes(hdr) + b"\x00" * 32)
    try:
        h = parse_image_header(good)
        if h["image_size"] != 32 or h["type"] != IMG_TYPE_APP:
            problems.append("镜像头解析结果不对：%r" % h)
    except Exception as e:
        problems.append("正例镜像解析失败：%s" % e)

    bad = os.path.join(tmpd, "bad.svcapp")
    open(bad, "wb").write(b"\x00" * 400)
    try:
        parse_image_header(bad)
        problems.append("负例镜像（魔数错）没有被拒绝")
    except ValueError:
        pass
    except Exception as e:
        problems.append("负例镜像抛了意外异常：%s" % e)

    # 2) 布局工具链：模板 -> 生成记录
    rec_path = os.path.join(tmpd, "rec.bin")
    rc, out, err = run_tool(LAYOUT_TOOL, ["template", "--mode", "fixed"])
    if rc != 0:
        problems.append("template 失败：%s" % (err.strip() or out.strip()))
    else:
        try:
            json.loads(out)
        except Exception as e:
            problems.append("template 输出不是 JSON：%s" % e)
        cfg = os.path.join(tmpd, "layout.json")
        open(cfg, "w", encoding="utf-8").write(out)
        rc, out2, err2 = run_tool(LAYOUT_TOOL, ["check", "--config", cfg])
        if rc != 0:
            problems.append("check 失败：%s" % (err2.strip() or out2.strip()))
        rc, out3, err3 = run_tool(LAYOUT_TOOL, ["build", "--config", cfg,
                                                "--bin", rec_path])
        if rc != 0:
            problems.append("build 失败：%s" % (err3.strip() or out3.strip()))
        elif os.path.getsize(rec_path) != CFG_RECORD_SIZE:
            problems.append("记录长度 %d != %d" % (os.path.getsize(rec_path),
                                                  CFG_RECORD_SIZE))
        else:
            magic, = struct.unpack_from("<I", open(rec_path, "rb").read(), 0)
            if magic != CFG_MAGIC:
                problems.append("记录魔数 0x%08X != 0x%08X" % (magic, CFG_MAGIC))

    # 3) 界面能否建起来（建完立刻销毁，不进入事件循环）
    if tk is not None:
        try:
            root = tk.Tk()
            root.withdraw()
            gui = HostGui(root)
            root.update_idletasks()
            gui.link.shutdown()
            root.destroy()
        except Exception as e:
            problems.append("界面构建失败：%r" % e)

    try:
        import shutil
        shutil.rmtree(tmpd, ignore_errors=True)
    except Exception:
        pass

    if problems:
        print("自检发现问题：")
        for p in problems:
            print("  - %s" % p)
        return 1
    print("自检通过：镜像头解析、布局工具链、界面构建 均正常")
    return 0


def main():
    ap = argparse.ArgumentParser(description="SVCrtOS 上位机（图形界面）")
    ap.add_argument("--selftest", action="store_true",
                    help="不开窗口做自检：解析、布局工具链、界面构建")
    ap.add_argument("--port", help="启动时自动打开该串口，如 COM3")
    ap.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    if tk is None:
        print("tkinter 不可用：%s" % _TK_IMPORT_ERROR, file=sys.stderr)
        print("Windows 官方 Python 自带 tkinter；若是精简版，请重装并勾选 tcl/tk。",
              file=sys.stderr)
        return 2

    try:
        import serial                                    # noqa: F401
    except ImportError:
        print("需要 pyserial：pip install pyserial", file=sys.stderr)
        return 2

    root = tk.Tk()
    gui = HostGui(root)
    if args.port:
        gui.var_port.set(args.port)
        gui.var_baud.set(str(args.baud))
        gui._toggle_port()
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
