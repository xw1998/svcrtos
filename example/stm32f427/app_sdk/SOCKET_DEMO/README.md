# SOCKET_DEMO

A plain POSIX sockets program (`Src/socket_demo.c`) built as an SVCrtOS App
**without edits**. It is the B-side counterpart of `POSIX_DEMO` (files) and of
`FS_DEMO` (VFS): the same source compiles on Linux with `cc socket_demo.c`,
and here the names `socket/bind/listen/accept/connect/send/recv/select/...`
come from `kernelsrc/sdk/posix`.

## What it checks

| Group | Items |
|-------|-------|
| names | `htons/ntohs` round trip, `inet_addr`, `inet_ntoa` |
| tcp echo | socket, `SO_REUSEADDR`, bind, listen, getsockname, connect, accept, getpeername, send/recv both ways, `select` readable, `select` timeout, `shutdown(SHUT_WR)` -> peer EOF, `SO_ERROR` |
| udp | socket, bind, `sendto`, `recvfrom` (payload + source port) |
| refusals | `socket(AF_INET6)` reported as an error, not silently accepted |

Every line prints `ok` / `FAIL` and the run ends with `result : pass=N fail=M`,
so a console capture is the evidence.

## Where the packets go

This board has no Ethernet PHY. The kernel brings up lwIP's loopback interface
(`127.0.0.1/8`), so connections and data run through the real TCP state
machine; only the wire is missing. The kernel side is the socket service of
SVC `0x1E` (`kernelsrc/src/svcrt_net.c`); the App side is the sockets block in
`kernelsrc/sdk/posix/svcrt_posix.c`.

## Build

Open `MDK-ARM/socket_demo.uvprojx` and build. The `BeforeMake` hook runs
`tools/gen_app_sct.py` and generates `build/socket_demo.sct`; do not edit the
`.sct` by hand.

The dev window (dev slot 3, 8 KB RAM, 2 KB heap) is what the debug download
uses. The App does not include any partition header - the layout is fetched at
run time over SVC `0x18`.

## Notes

- `SO_RCVTIMEO` / `SO_SNDTIMEO` are held and honoured by the App side (the
  kernel socket calls themselves never block); 0/0 means "wait forever".
- IPv4 only: `AF_INET`. `AF_INET6` is refused.
- With lwIP's `MEMP_NUM_NETCONN = 4`, at most 4 sockets exist at once, and the
  loopback echo check holds 3 of them (listener + accepted + client).
