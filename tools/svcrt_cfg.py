#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Talk to the on-device layout configuration region (the CONFIG region) over the
console UART.

Three operations, matching the kernel shell command `cfg` (see
kernelsrc/shell/svcrt_shell.c):

    show    read back the layout that is in effect right now
    write   upload one 512-byte configuration record
    clear   erase the CONFIG region and fall back to the compile-time default

Why a binary upload and not a hex line
--------------------------------------
The shell line buffer (ARK_SHELL_LINE_SIZE, see ark_shell_config.h) is 128
bytes, so a 512-byte record cannot be typed as a command argument. The record
is therefore streamed as 4 chunks of 128 bytes. The device answers one
flow-control byte after each chunk: ACK (0x06) means "send the next chunk",
NAK (0x15) means "stop". The last chunk is answered only after the device has
validated and written the record, so the final flow byte is the verdict.

The flow byte carries no reason code. When the device rejects a record it
prints the reason on the same port just before the NAK -- this tool relays
those lines verbatim and never invents a cause of its own.

Usage
-----
    python tools/svcrt_cfg.py show  --port COM3
    python tools/svcrt_cfg.py clear --port COM3
    python tools/svcrt_cfg.py write --port COM3 --bin build/layout.bin
    python tools/svcrt_cfg.py write --port COM3 --config cfg.json --reboot

`--bin` expects the 512-byte record written by `tools/svcrt_layout.py build
--bin`. `--config` / `--json` instead runs that tool first, so an operator can
go from a JSON description straight to a programmed device.

Requires pyserial:  pip install pyserial
"""
import argparse
import os
import struct
import subprocess
import sys
import time

RECORD_SIZE = 512          # must match SVCRT_CFG_RECORD_SIZE
CHUNK_SIZE = 128           # must match SVCRT_CFG_CHUNK_SIZE
MAGIC = 0x47464353         # SVCRT_CFG_MAGIC, "SCFG" little-endian

ACK_BYTE = 0x06
NAK_BYTE = 0x15

READY_TIMEOUT_S = 5.0      # how long to wait for the "ready, send ..." line
CHUNK_TIMEOUT_S = 5.0      # per-chunk flow byte, chunks before the last one
LAST_TIMEOUT_S = 15.0      # the last chunk also covers flash programming (and
                           # a possible 128K sector erase, ~2 s on STM32F4)
QUIET_S = 0.6              # silence that ends a plain shell command's output

HERE = os.path.dirname(os.path.abspath(__file__))
LAYOUT_TOOL = os.path.join(HERE, "svcrt_layout.py")


def device_lines(buf):
    text = bytes(buf).decode("utf-8", errors="replace")
    return [ln.strip() for ln in text.replace("\r", "\n").split("\n") if ln.strip()]


def drain(ser, quiet_s=QUIET_S, limit_s=30.0):
    """Read until the line has been quiet for quiet_s; return the lines."""
    buf = bytearray()
    deadline = time.time() + limit_s
    last = time.time()
    while time.time() < deadline:
        b = ser.read(1)
        if b:
            buf += b
            last = time.time()
            continue
        if (time.time() - last) >= quiet_s:
            break
    return device_lines(buf)


def wait_flow(ser, timeout):
    """Wait for one flow byte. Returns (verdict, lines)."""
    buf = bytearray()
    deadline = time.time() + timeout
    while time.time() < deadline:
        b = ser.read(1)
        if not b:
            continue
        if b[0] == ACK_BYTE:
            return "ack", device_lines(buf)
        if b[0] == NAK_BYTE:
            return "nak", device_lines(buf)
        buf += b
    return "timeout", device_lines(buf)


def report(lines):
    for ln in lines:
        print("device: %s" % ln)


def load_record(args):
    """Return the 512 bytes to send, building them first when asked to."""
    if args.bin:
        rec = open(args.bin, "rb").read()
        if len(rec) != RECORD_SIZE:
            raise SystemExit("record is %d bytes, expected %d: %s"
                             % (len(rec), RECORD_SIZE, args.bin))
    else:
        tmp = args.binout or os.path.join(os.getcwd(), "layout_record.bin")
        cmd = [sys.executable, LAYOUT_TOOL, "build"]
        cmd += ["--json", args.json] if args.json else ["--config", args.config]
        cmd += ["--bin", tmp]
        if args.sct_dir:
            cmd += ["--sct-dir", args.sct_dir]
        r = subprocess.run(cmd)
        if r.returncode != 0:
            raise SystemExit("svcrt_layout.py build failed (%d)" % r.returncode)
        rec = open(tmp, "rb").read()
        print("built %s (%d bytes)" % (tmp, len(rec)))

    magic, = struct.unpack_from("<I", rec, 0)
    if magic != MAGIC:
        raise SystemExit("record magic is 0x%08X, expected 0x%08X (not a layout "
                         "record): refusing to send it" % (magic, MAGIC))
    return rec


def cmd_show(ser, args):
    ser.write(b"cfg show\r\n")
    ser.flush()
    report(drain(ser))
    return 0


def cmd_clear(ser, args):
    ser.write(b"cfg clear\r\n")
    ser.flush()
    report(drain(ser))
    if args.reboot:
        return do_reboot(ser)
    return 0


def do_reboot(ser):
    ser.write(b"reboot\r\n")
    ser.flush()
    report(drain(ser, quiet_s=1.5, limit_s=5.0))
    return 0


def cmd_write(ser, args):
    rec = load_record(args)

    # CR only: the shell ends a command line on CR. A CRLF terminator leaves
    # the LF in the console receive FIFO, and that stray byte would shift the
    # whole record by one -- the device would collect 512 bytes and reject
    # them with 'bad magic'.
    ser.write(b"cfg load\r")
    ser.flush()

    # Wait for the whole handshake banner, not just the word 'ready': the
    # device is still printing after 'ready' appears, and the record must not
    # start before it has entered the receive loop. The banner ends with the
    # flow-byte legend; a short silence is the fallback in case that line is
    # lost on the wire.
    buf = bytearray()
    deadline = time.time() + READY_TIMEOUT_S
    ready = False
    last_rx = time.time()
    while time.time() < deadline:
        b = ser.read(1)
        if not b:
            if ready and (time.time() - last_rx) > QUIET_S:
                break
            continue
        buf += b
        last_rx = time.time()
        if b"ready" in buf:
            ready = True
        if b"0x15 = stop" in buf:
            break
    if not ready:
        report(device_lines(buf))
        print("no 'ready' line from the device within %.1fs" % READY_TIMEOUT_S,
              file=sys.stderr)
        return 2
    report(device_lines(buf))

    chunks = [rec[i:i + CHUNK_SIZE] for i in range(0, RECORD_SIZE, CHUNK_SIZE)]
    for i, chunk in enumerate(chunks):
        last = (i + 1) == len(chunks)
        ser.write(chunk)
        ser.flush()

        verdict, lines = wait_flow(ser, LAST_TIMEOUT_S if last else CHUNK_TIMEOUT_S)
        report(lines)

        if verdict == "nak":
            print("device rejected the record (chunk %d/%d); see the device "
                  "line above for the reason" % (i + 1, len(chunks)),
                  file=sys.stderr)
            return 1
        if verdict == "timeout":
            print("no flow byte after chunk %d/%d within %.1fs"
                  % (i + 1, len(chunks), LAST_TIMEOUT_S if last else CHUNK_TIMEOUT_S),
                  file=sys.stderr)
            return 2

    print("record accepted and stored (%d bytes)" % RECORD_SIZE)

    if args.reboot:
        return do_reboot(ser)

    # Read back what the device now believes, so the operator sees the result
    # rather than a promise.
    return cmd_show(ser, args)


def main():
    ap = argparse.ArgumentParser(description="SVCrtOS device layout configuration over serial")
    ap.add_argument("action", choices=("show", "write", "clear"))
    ap.add_argument("--port", required=True, help="serial port, e.g. COM3 or /dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200, help="baud rate (default 115200)")
    ap.add_argument("--bin", help="write: a ready 512-byte record to send")
    ap.add_argument("--config", help="write: build the record from this JSON file (path)")
    ap.add_argument("--json", help="write: build the record from this JSON text")
    ap.add_argument("--sct-dir", help="write: also emit one .sct per slot here")
    ap.add_argument("--binout", help="write: where the built record is kept")
    ap.add_argument("--reboot", action="store_true",
                    help="reboot the device afterwards (needed before installing images)")
    args = ap.parse_args()

    if args.action == "write" and not (args.bin or args.config or args.json):
        ap.error("write needs one of --bin / --config / --json")

    try:
        import serial
    except ImportError:
        print("pyserial is required:  pip install pyserial", file=sys.stderr)
        return 2

    ser = serial.Serial(args.port, args.baud, timeout=0.05)

    # Whatever the console printed before we got here is not ours: drop it.
    drain(ser, quiet_s=0.3, limit_s=1.0)

    try:
        if args.action == "show":
            return cmd_show(ser, args)
        if args.action == "clear":
            return cmd_clear(ser, args)
        return cmd_write(ser, args)
    finally:
        ser.close()


if __name__ == "__main__":
    sys.exit(main())
