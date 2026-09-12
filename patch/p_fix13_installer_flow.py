# -*- coding: utf-8 -*-
"""
Close the "serial install loses bytes" gap (round 8).

The problem
-----------
The installer streams the image straight into flash: every 512-byte chunk is
programmed as it arrives, and the sectors are erased right after the header.
On STM32F4 a sector erase takes 0.5~2 s and a 512-byte program a few ms. Both
run from flash, so while they execute the CPU cannot fetch instructions and the
UART interrupt cannot run. The USART receive path holds one byte, so every byte
that arrives in that window raises ORE and is lost. A sender that just dumps the
file into the port therefore loses bytes on any image bigger than a few KB, and
the install fails and has to be redone.

The fix
-------
Pace the transfer with a one-byte handshake, so the host never transmits while
the device is busy in flash:

    device: receive 256-byte header
            erase the target sectors          (host is silent: it waits for ACK)
            program the header
            -> ACK
    loop:   host sends <=512 bytes
            device programs them
            -> ACK

Ordering matters and is self-synchronising: the ACK is queued *after* the flash
programming of a chunk, and the host only sends the next chunk after it sees the
ACK, so the device is never in a flash-blocked window while bytes are in flight.
The device->host direction carries only ACK bytes, so it cannot collide with the
image stream.

A plain "dump the file to the port" sender still works (it just ignores the ACK
bytes) but keeps the old lossy behaviour - that is inherent to not honouring
flow control, and is documented.

Host side: tools/send_image.py implements the ACK-paced transfer.
"""
import os
import shutil

ROOT = r'D:\工作\git_project\svcrtos_new'
BK = r'C:\Users\14905\OneDrive\Documents\lingxi-claw\20260911-13-47-19-445\backup_fix13'

REL = 'kernelsrc/src/svcrt_loader.c'

ACK_HELPERS = '''/* ============================================================
 * Streaming install flow control
 * @details While a flash sector is erased (0.5~2 s on STM32F4) or a chunk is
 *          programmed, the CPU cannot fetch from flash, so the UART interrupt
 *          does not run and any byte arriving in that window is lost (the
 *          USART receive path holds a single byte). A sender that streams the
 *          whole file without pausing therefore loses bytes on any image of
 *          more than a few KB.
 *
 *          Protocol: the device sends one ACK (0x06) after the header has been
 *          erased + committed, and after each payload chunk has been
 *          programmed. The host sends the next unit only after it sees that
 *          ACK, so it never transmits while the device is blocked in flash.
 *          See tools/send_image.py for the host implementation.
 *
 *          A sender that ignores the ACKs (a plain file dump to the port)
 *          still works, it just keeps the old lossy behaviour.
 * ============================================================ */
#define SVCRT_LOADER_ACK_BYTE    (0x06u)

static void svcrt_loader_ack(int32 dev)
{
    uint8 ack = SVCRT_LOADER_ACK_BYTE;

    /* Queue the ACK after the flash work of this unit is finished. The TX path
     * is interrupt driven, so the byte may leave the device slightly later -
     * that is fine and cannot dead-lock: the host is waiting, and the device
     * only re-enters flash once the next chunk has arrived. */
    (void)svcrt_dev_write_internal(dev, &ack, 1);
}

/* 从设备读取指定长度（重试有限次，避免非阻塞读返回 0 时空转） */'''

NEW_STREAM = '''/* Stream the payload into an already erased region (header + chunked payload).
 * Returns 0 or SVCRT_LOADER_ERR_x.
 *
 * One ACK is sent back to the host per committed unit, and the host sends the
 * next unit only after seeing it. That keeps the wire quiet while the device is
 * blocked in flash, which is what removes the ORE byte loss described above. */
static int32 svcrt_loader_stream_payload(int32 dev, uint32 base, const svcrt_app_header_t *p_hdr)
{
    uint32 written = 0u;

    if(svcrt_port_flash_write(base, (const uint8 *)p_hdr, SVCRT_APP_HEADER_SIZE) != 0)
    {
        return SVCRT_LOADER_ERR_FLASH;
    }

    /* 擦除已经结束、头也写进 Flash：通知发送端可以开始发负载 */
    svcrt_loader_ack(dev);

    while(written < p_hdr->image_size)
    {
        uint32 want = p_hdr->image_size - written;

        if(want > SVCRT_LOADER_CHUNK_SIZE)
        {
            want = SVCRT_LOADER_CHUNK_SIZE;
        }

        if(svcrt_loader_read_dev(dev, svcrt_loader_chunk, want) != (int32)want)
        {
            return SVCRT_LOADER_ERR_SIZE;
        }

        if(svcrt_port_flash_write(base + SVCRT_APP_HEADER_SIZE + written,
                                  svcrt_loader_chunk, want) != 0)
        {
            return SVCRT_LOADER_ERR_FLASH;
        }

        written += want;

        /* 本块已落盘：放行下一块 */
        svcrt_loader_ack(dev);
    }

    return 0;
}'''

SEND_IMAGE = '''#!/usr/bin/env python3
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
'''


def main():
    os.makedirs(BK, exist_ok=True)

    p = os.path.join(ROOT, REL.replace('/', os.sep))
    shutil.copy2(p, os.path.join(BK, 'svcrt_loader.c'))
    text = open(p, 'rb').read().decode('utf-8')

    old_helper = '/* 从设备读取指定长度（重试有限次，避免非阻塞读返回 0 时空转） */'
    assert text.count(old_helper) == 1, 'helper anchor not unique (%d)' % text.count(old_helper)
    text = text.replace(old_helper, ACK_HELPERS)

    i = text.index('/* 把负载流式写入已擦除的分区（头 + 分块负载），返回 0 或 SVCRT_LOADER_ERR_x */')
    j = text.index('int32 svcrt_loader_load_dev_hdr(int32 dev, const svcrt_app_header_t *p_hdr, uint32 image_len)')
    assert 0 < i < j
    text = text[:i] + NEW_STREAM + '\n\n' + text[j:]

    open(p, 'wb').write(text.encode('utf-8'))
    print('  [ok] %s' % REL)

    t = os.path.join(ROOT, 'tools', 'send_image.py')
    open(t, 'wb').write(SEND_IMAGE.encode('utf-8'))
    print('  [ok] tools/send_image.py (new)')
    print('install flow-control patch done')


if __name__ == '__main__':
    main()
