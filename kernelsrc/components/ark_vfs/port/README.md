# ark_vfs on SVCrtOS

This is the worked example of a port. It is short on purpose: if porting
ark_vfs to your RTOS needed more than this, the library would have failed at
the one thing it promises.

Everything here compiles against two worlds at once:

| Side | What it uses |
|---|---|
| ark_vfs | `ark_vfs_backend_t`, `ark_vfs_hooks_t`, `ark_vfs_fsdrv_t`, `ark_vfs_mount` |
| SVCrtOS | `svcrt_blk_*`, `svcrt_dev_*`, `svcrt_sched_lock_internal`, `svcrt_kernel_get_tick`, `svcrt_log_emit` |

Nothing in `src/` knows which RTOS it is running on, and nothing in the
kernel knows what a mount table is.

## Files

| File | Role |
|---|---|
| `ark_vfs_port_svcrtos.h` | the port's public surface: one struct, three functions |
| `ark_vfs_port_svcrtos.c` | block backend, hooks, `ark_vfs_port_init()` |
| `ark_vfs_devfs.c` | the SVCrtOS device registry as a filesystem: `/dev/<name>` |
| `tests/mock/*.h` | minimal stand-ins for six kernel headers, so the port builds on a PC |
| `tests/mock/mock_svcrtos.c` | the same contracts implemented in memory |
| `tests/test_port_svcrtos.c` | 70 checks over offsets, erase alignment, error mapping, `/dev`, hooks |
| `tests/run_port_tests.sh` | build + run the above (`-Wall -Wextra -Werror`) |

Source files are shared between the board build and the host test. There is
no `#ifdef HOST` anywhere: the include path decides which `svcrt_blk.h` is
seen.

## Porting in three steps

### 1. A backend: bytes, not files

`ark_vfs_backend_t` is five function pointers and two numbers. On SVCrtOS
that is `ark_vfs_port_blk_init()`:

```c
ark_vfs_port_blk_t vol;
ark_vfs_port_blk_init(&vol, "nor0", 0u, 4u * 1024u * 1024u, "nor");
/* &vol.be is now a valid ark_vfs_backend_t - the struct starts with it */
```

Two things it refuses to do, both on purpose:

- **it does not clamp.** A window that does not fit the device, or that is
  not a whole number of erase units, is `ARK_E_INVAL` at bind time. A
  quietly shortened volume corrupts data hours later, somewhere else.
- **it does not invent an erase.** A device with `erase_unit == 0` gets
  `ARK_E_NOSYS`, not a silent success.

Offsets are translated in exactly one place - backend `off` is relative to
the window, `off + base` is what the chip sees - so a partition costs
nothing extra:

```c
ark_vfs_port_blk_init(&vol, "nor0", 0x100000u, 0x100000u, "sector7");  /* 1 MiB window */
```

Erase ranges are widened *outwards* to unit boundaries, never inwards: a
short erase leaves stale bytes that look like a successful write later.

### 2. Hooks: a lock and a clock

```c
ark_vfs_hooks_t hooks;
ark_vfs_port_hooks(&hooks);
```

- the **lock** is `svcrt_sched_lock_internal()`, which stops task switches
  and nests. A spinlock would be the wrong tool here: an ark_vfs call can
  end in a flash erase, and a spinlock held across an erase stalls every
  interrupt on the chip. Set `ARK_VFS_PORT_SVCRTOS_LOCK=0` for a build where
  one task owns the VFS and the lock should not exist at all.
- the **clock** is `svcrt_kernel_get_tick()` converted to milliseconds. The
  division only pulls in a helper if your tick period actually divides
  1000 (500 us does: tick / 2).
- the **log** hook formats through `vsnprintf` and emits with
  `svcrt_log_emit`.
- Passing `NULL` for every hook is legal and gives a bare-metal build: no
  lock, no time, no output.

### 3. Filesystems: plug in, do not modify

`ark_vfs_devfs.c` is the example: ~380 lines turn the kernel's device
registry into `/dev/uart0`, `/dev/nor0`, and so on.

```c
svcrt_dev_register("uart0", &uart_drv, 0u);
ark_vfs_port_init();                       /* hooks + mount devfs at /dev */
tty = ark_vfs_open("/dev/uart0", ARK_O_RDWR);
```

The design choices worth copying:

- **a device node table is an ordinary filesystem.** Every "special case for
  paths that are really devices" is a branch the core would carry forever.
- **names come from the registry**, so `/dev` cannot drift from reality: a
  device that has been unregistered disappears from the listing.
- **it is flat.** `/dev/uart0`, never `/dev/serial/0` - the registry has no
  hierarchy, and inventing one would be a lie about the hardware.
- **`open` distinguishes two failures.** A name not in the registry is
  `ARK_E_NOENT`; a name in the registry whose driver refuses to open is
  `ARK_E_IO`. They are different stories and the caller is told which.

A real flash filesystem (littlefs, FAT, your own) plugs in the same way: copy
`ark_vfs_devfs.c`, replace the callbacks with calls into that filesystem, and
keep returning the negative `ARK_E_*` codes.

## Verifying without a board

```sh
sh ports/svcrtos/tests/run_port_tests.sh
```

```text
port tests: 70 checks, 0 failures
```

The fake chip is deliberately strict: writing a byte that is not `0xFF`
fails, an unaligned erase fails. A port that quietly rounds or clamps passes
a lenient fake and fails on the board - moving that failure earlier is the
whole point of the test.

## The feature gate

Both port sources are wrapped in `#if (SVCRT_USE_VFS != 1) ... #else ... #endif`,
where the switch comes from the kernel's `svcrt_features.h`. This is on purpose:
the port is the SVCrtOS side of ark_vfs, and a kernel built without a VFS has no
business compiling it - nor could it, since the block backend needs
`SVCRT_USE_BLK`. When the switch is off both units compile to nothing but a
harmless `typedef`, so they can stay in the project file without costing a byte
(verified with `nm`: zero text symbols).

The host test pins the switch **on** in `tests/mock/svcrt_features.h`; the mock
honours a `-D` override, so the off state can be checked too:

```sh
gcc -c -Wall -Wextra -Werror -std=c99 -DSVCRT_USE_VFS=0 \
    -Iinclude -Isrc -Iports/svcrtos -Iports/svcrtos/tests/mock \
    ports/svcrtos/ark_vfs_port_svcrtos.c -o /dev/null
```

## What is not covered here

- **littlefs over ark_vfs.** The kernel already has `svcrt_fs`, and a second
  littlefs binding would be two implementations of one thing. When a
  filesystem driver is written here, it belongs next to this file.
- **Concurrency.** The lock is exercised for nesting balance, not for real
  contention: a single-threaded host test cannot prove a two-task race.
  That needs the board.
