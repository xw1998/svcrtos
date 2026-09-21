#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Copy a file into the SVCrtOS device's mounted littlefs volume.

This is the host side of the kernel's `fs put <path> <len>` command, and it
exists for the same reason the device command does: a firmware image is far
larger than the shell's command line, so it cannot travel as text. The host
opens a window, the device stops reading command lines for the duration, and
the bytes go over the wire raw.

The per-chunk handshake is not decoration. The device writes each chunk into
flash before it ACKs, and a block program can turn into a sector erase; during
that window the 128-byte console FIFO is the only thing holding incoming
bytes. A sender that ran ahead would fill it and lose the tail silently, so
the device ACKs only once the bytes are on the chip and the host waits for it.

The device prints the real reason before any failure, on the same port. This
tool relays those lines verbatim instead of inventing a cause: a wrong cause
costs a full round of checking something that was never broken.

Usage
-----
    python tools/fs_put.py --port COM3 build/APP_DEMO/APP_DEMO.svcapp
    python tools/fs_put.py --port COM3 --path /pkg/app.svcapp APP_DEMO.svcapp
    python tools/fs_put.py --port COM3 --baud 115200 --chunk 256 image.svcapp

Requires pyserial:  pip install pyserial
"""
import argparse
import os
import sys
import time

ACK_BYTE = 0x06
NAK_BYTE = 0x15
CHUNK_SIZE = 256              # must not exceed the device's FS_SCRATCH_LEN (512)
ACK_TIMEOUT_S = 10.0          # a block program may have to erase a sector first
READY_TIMEOUT_S = 10.0
READY_MARK = b'one flow byte per chunk'
CR = b'\r'
LF = b'\n'


def device_lines(buf):
    """Split whatever the device printed into lines (UTF-8, tolerant).

    Lines carrying an escape sequence are the line editor echoing what was
    typed, not device output: drop them so only real messages remain.
    """
    text = bytes(buf).decode('utf-8', errors='replace')
    out = []
    for ln in text.replace('\r', '\n').split('\n'):
        ln = ln.strip()
        if ln and ('\x1b' not in ln):
            out.append(ln)
    return out


def show_device_log(lines):
    if not lines:
        print('note: the device printed nothing while we waited - its log may be '
              'disabled, or another reader is holding the port', file=sys.stderr)
        return
    for ln in lines:
        print('device: %s' % ln, file=sys.stderr)


def wait_bytes(ser, mark, timeout):
    """Read until `mark` shows up. Returns (found, lines printed on the way)."""
    buf = bytearray()
    deadline = time.time() + timeout
    while time.time() < deadline:
        b = ser.read(1)
        if not b:
            continue
        buf += b
        if mark in buf:
            return True, device_lines(buf)
    return False, device_lines(buf)


def wait_ack(ser, timeout):
    """Wait for the device's flow byte. Returns ('ack'|'nak'|'timeout', lines)."""
    buf = bytearray()
    deadline = time.time() + timeout
    while time.time() < deadline:
        b = ser.read(1)
        if not b:
            continue
        if b[0] == ACK_BYTE:
            return 'ack', device_lines(buf)
        if b[0] == NAK_BYTE:
            return 'nak', device_lines(buf)
        buf += b
    return 'timeout', device_lines(buf)


def main():
    ap = argparse.ArgumentParser(description='Copy a file into the device file system')
    ap.add_argument('file', help='local file to send')
    ap.add_argument('--port', required=True, help='serial port, e.g. COM3 or /dev/ttyUSB0')
    ap.add_argument('--baud', type=int, default=115200, help='baud rate (default 115200)')
    ap.add_argument('--path', help='destination path inside the volume (default /<basename>)')
    ap.add_argument('--chunk', type=int, default=CHUNK_SIZE,
                    help='bytes per handshake (default %d, must be <= 512)' % CHUNK_SIZE)
    args = ap.parse_args()

    try:
        import serial                                    # noqa: F401
    except ImportError:
        print('pyserial is required:  pip install pyserial', file=sys.stderr)
        return 2

    if (args.chunk <= 0) or (args.chunk > 512):
        print('--chunk must be 1..512: the device reads it into a 512-byte buffer',
              file=sys.stderr)
        return 2

    data = open(args.file, 'rb').read()
    if not data:
        print('%s is empty - the device refuses a zero-length file' % args.file,
              file=sys.stderr)
        return 2

    path = args.path if args.path else '/' + os.path.basename(args.file)

    # The path travels to the device as text on a narrow line, so it has to be
    # ASCII with no whitespace: anything else would arrive as a different name,
    # or split into extra words, and the device would then report a stat
    # failure for something we never asked for. A leading drive letter is the
    # other way this goes wrong - MSYS-style shells rewrite "/x" into a host
    # path - and it is worth naming, because the failure looks like a missing
    # file on the device.
    if len(path) > 2 and path[1] == ':' and path[0].isalpha():
        print('--path looks like a host path (%r), which is how a shell rewrites a leading "/": '
              're-run with MSYS_NO_PATHCONV=1, or pass --path without the leading slash'
              % path, file=sys.stderr)
        return 2
    if path != path.encode('ascii', 'ignore').decode('ascii') or any(c.isspace() for c in path):
        print('--path must be ASCII with no whitespace (got %r)' % path, file=sys.stderr)
        return 2

    if not path.startswith('/'):
        path = '/' + path

    ser = serial.Serial(args.port, args.baud, timeout=0.05)

    try:
        print('putting %s (%d bytes) -> %s on %s @ %d'
              % (args.file, len(data), path, args.port, args.baud))

        # CRLF, not CR: the shell ends the line on CR and the device drops the
        # leftover LF before it announces readiness, so the byte stream that
        # follows is aligned from its very first byte.
        ser.write(('fs put %s %d' % (path, len(data))).encode('ascii') + CR + LF)
        ser.flush()

        found, lines = wait_bytes(ser, READY_MARK, READY_TIMEOUT_S)
        if not found:
            print('the device never announced an fs put window - is the volume mounted, '
                  'and is this the console port?', file=sys.stderr)
            show_device_log(lines)
            return 1

        sent = 0
        while sent < len(data):
            piece = data[sent:sent + args.chunk]
            ser.write(piece)
            ser.flush()

            verdict, lines = wait_ack(ser, ACK_TIMEOUT_S)
            if verdict == 'nak':
                print('the device refused the file at offset %d' % sent, file=sys.stderr)
                show_device_log(lines)
                return 1
            if verdict == 'timeout':
                print('no flow byte after offset %d - transfer aborted' % sent, file=sys.stderr)
                show_device_log(lines)
                return 1

            sent += len(piece)
            if (sent % 4096 == 0) or (sent == len(data)):
                print('  %d/%d bytes' % (sent, len(data)), file=sys.stderr)

        # The last chunk is ACKed before the file is closed, so read on for
        # the verdict line rather than reporting success on the ACK alone.
        found, lines = wait_bytes(ser, b'fs put: ok', 10.0)
        if not found:
            show_device_log(lines)
            print('the bytes were accepted but the device never confirmed the file',
                  file=sys.stderr)
            return 1

        show_device_log(lines)

        print('ok: %s holds %d bytes' % (path, len(data)))
        return 0
    finally:
        ser.close()


if __name__ == '__main__':
    sys.exit(main())
