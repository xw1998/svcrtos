#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Send a SVCrtOS image (.svcapp) to the on-device installer over a serial port,
using the ACK-paced protocol implemented by svcrt_loader_stream_image().

Why not just copy the file to the port
--------------------------------------
The installer programs each 512-byte chunk into flash as it arrives. Flash
programming and the sector erase both run from flash, so during those windows
the device cannot service its UART receive interrupt and any byte that arrives
is dropped (ORE). Streaming the file without pausing therefore loses bytes and
the install fails.

The device sends one ACK byte (0x06) once it has taken everything it needs
before the payload, and after each payload chunk has been programmed. A NAK
byte (0x15) means "this frame is over, stop sending".

Frame layout (must match kernelsrc/src/svcrt_loader.c)
------------------------------------------------------
    +----------------+---------------------+-------------------------+
    | header 256 B   | relocation table    | payload                 |
    |                | reloc_count * 4 B   | image_size B            |
    +----------------+---------------------+-------------------------+
      \__________________ sent as one burst __/  \__ ACK-paced 512 B chunks __/

The header and the table are pushed back to back because the device reads the
table before it ACKs; it cannot ACK earlier, since the table is only
addressable once it has been written to flash.

Usage
-----
    python tools/send_image.py --port COM3 build/APP_DEMO/APP_DEMO.svcapp
    python tools/send_image.py --port COM3 --baud 115200 --chunk 512 image.svcapp
    python tools/send_image.py --port COM3 --no-ack image.svcapp    # old, lossy

Requires pyserial:  pip install pyserial
"""
import argparse
import struct
import sys
import time

ACK_BYTE = 0x06
NAK_BYTE = 0x15
HEADER_SIZE = 256                 # must match SVCRT_APP_HEADER_SIZE
CHUNK_SIZE = 512                  # must match SVCRT_LOADER_CHUNK_SIZE
ACK_TIMEOUT_S = 5.0               # an erase of a 128K sector can take ~2 s
HEADER_SETTLE_S = 0.05            # let the device finish flashing the header

# header field offsets, see kernelsrc/include/svcrt_app_image.h
OFF_IMAGE_SIZE = 16
OFF_RELOC_COUNT = 116
OFF_PAYLOAD_OFFSET = 124


def wait_ack(ser, timeout):
    """Wait for the device's flow-control byte.

    Returns 'ack', 'nak' or 'timeout'. Anything else the device prints
    (log lines, the shell echo) is ignored.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        b = ser.read(1)
        if not b:
            continue
        if b[0] == ACK_BYTE:
            return 'ack'
        if b[0] == NAK_BYTE:
            return 'nak'
    return 'timeout'


def main():
    ap = argparse.ArgumentParser(description='Send an image to the SVCrtOS installer')
    ap.add_argument('image', help='image file (.svcapp)')
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

    image_size = struct.unpack_from('<I', data, OFF_IMAGE_SIZE)[0]
    reloc_count = struct.unpack_from('<I', data, OFF_RELOC_COUNT)[0]
    payload_offset = struct.unpack_from('<I', data, OFF_PAYLOAD_OFFSET)[0]

    reloc_len = reloc_count * 4
    total = payload_offset + image_size
    if total != len(data):
        print('image length mismatch: header says %d bytes, file has %d'
              % (total, len(data)), file=sys.stderr)
        return 2

    ser = serial.Serial(args.port, args.baud, timeout=0.05)

    try:
        print('sending %s (%d bytes: header %d + reloc %d + payload %d) on %s @ %d'
              % (args.image, len(data), HEADER_SIZE, reloc_len, image_size,
                 args.port, args.baud))

        # Unit 0: header + relocation table, sent as one burst. The device
        # cannot ACK the header alone -- it needs the table addressable first.
        ser.write(data[:HEADER_SIZE])
        ser.flush()
        time.sleep(HEADER_SETTLE_S)
        ser.write(data[HEADER_SIZE:HEADER_SIZE + reloc_len])
        ser.flush()

        if not args.no_ack:
            r = wait_ack(ser, ACK_TIMEOUT_S)
            if r == 'nak':
                print('device rejected the frame (relocation table invalid)', file=sys.stderr)
                return 1
            if r == 'timeout':
                print('no ACK for the header - is the installer running on this port?',
                      file=sys.stderr)
                return 1

        sent = payload_offset
        while sent < payload_offset + image_size:
            piece = data[sent:sent + args.chunk]
            ser.write(piece)
            ser.flush()
            if not args.no_ack:
                r = wait_ack(ser, ACK_TIMEOUT_S)
                if r == 'nak':
                    print('device rejected the frame after offset %d' % sent, file=sys.stderr)
                    return 1
                if r == 'timeout':
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
