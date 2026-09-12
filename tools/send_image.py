#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Send a SVCrtOS image (.svcapp / .bin) to the on-device installer over a serial
port, using the ACK-paced protocol implemented by svcrt_loader_stream_payload().

Why not just copy the file to the port
--------------------------------------
The installer programs each 512-byte chunk into flash as it arrives. Flash
programming and the sector erase both run from flash, so during those windows
the device cannot service its UART receive interrupt and any byte that arrives
is dropped (ORE). Streaming the file without pausing therefore loses bytes and
the install fails.

The device sends one ACK byte (0x06) after the header has been erased/committed
and after each chunk has been programmed. This script waits for that ACK before
sending the next unit, so the device is never busy in flash while bytes are on
the wire.

Usage
-----
    python tools/send_image.py --port COM3 image.svcapp
    python tools/send_image.py --port COM3 --baud 115200 --chunk 512 image.svcapp
    python tools/send_image.py --port COM3 --no-ack image.svcapp    # old, lossy

Requires pyserial:  pip install pyserial
"""
import argparse
import sys
import time

ACK_BYTE = 0x06
HEADER_SIZE = 256                 # must match SVCRT_APP_HEADER_SIZE
CHUNK_SIZE = 512                  # must match SVCRT_LOADER_CHUNK_SIZE
ACK_TIMEOUT_S = 5.0               # an erase of a 128K sector can take ~2 s


def wait_ack(ser, timeout):
    """Wait for one ACK byte, ignoring anything else the device may print."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        b = ser.read(1)
        if not b:
            continue
        if b[0] == ACK_BYTE:
            return True
    return False


def main():
    ap = argparse.ArgumentParser(description='Send an image to the SVCrtOS installer')
    ap.add_argument('image', help='image file (.svcapp for install path, .bin for the raw path)')
    ap.add_argument('--port', required=True, help='serial port, e.g. COM3 or /dev/ttyUSB0')
    ap.add_argument('--baud', type=int, default=115200, help='baud rate (default 115200)')
    ap.add_argument('--chunk', type=int, default=CHUNK_SIZE, help='payload chunk size in bytes')
    ap.add_argument('--no-ack', action='store_true',
                    help='stream without waiting for ACKs (old behaviour, loses bytes on large images)')
    args = ap.parse_args()

    try:
        import serial                                    # noqa: F401
    except ImportError:
        print('pyserial is required:  pip install pyserial', file=sys.stderr)
        return 2

    data = open(args.image, 'rb').read()
    if len(data) < HEADER_SIZE:
        print('image is smaller than the 256-byte header', file=sys.stderr)
        return 2

    ser = serial.Serial(args.port, args.baud, timeout=0.05)

    try:
        print('sending %s (%d bytes) on %s @ %d' % (args.image, len(data), args.port, args.baud))

        # Unit 1: the 256-byte header. The device erases the target sectors
        # while we wait, so the ACK may take a couple of seconds.
        ser.write(data[:HEADER_SIZE])
        ser.flush()
        if not args.no_ack:
            if not wait_ack(ser, ACK_TIMEOUT_S):
                print('no ACK for the header - is the installer running on this port?',
                      file=sys.stderr)
                return 1

        sent = HEADER_SIZE
        while sent < len(data):
            piece = data[sent:sent + args.chunk]
            ser.write(piece)
            ser.flush()
            if not args.no_ack:
                if not wait_ack(ser, ACK_TIMEOUT_S):
                    print('no ACK after offset %d - transfer aborted' % sent, file=sys.stderr)
                    return 1
            sent += len(piece)
            if args.no_ack:
                # keep the device's UART from overrunning: pace roughly by wire time
                time.sleep(len(piece) * 10.0 / args.baud)

        print('done, %d bytes sent' % sent)
        if args.no_ack:
            print('note: --no-ack keeps the legacy lossy behaviour; the device may reject the image')
        return 0
    finally:
        ser.close()


if __name__ == '__main__':
    sys.exit(main())
