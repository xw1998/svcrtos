# littlefs (vendored)

Upstream: <https://github.com/littlefs-project/littlefs>, tag **v2.9.3**
(`LFS_VERSION 0x00020009`), licence: BSD-3-Clause, see `LICENSE.md`.

Files here are **verbatim upstream** — do not patch them. The SVCrtOS build
configuration lives one directory up in `../lfs_svcrt_config.h`, which the
project passes as `LFS_CONFIG` so `lfs_util.h` can include it.

## Why littlefs

| Requirement | How littlefs meets it |
|---|---|
| Power-loss safety | Every write is copy-on-write with a commit; a torn write leaves the previous state readable |
| Works on raw NOR | Needs only read / prog / erase, no FTL, no spare-area handling |
| Small RAM | All buffers are caller-provided; `LFS_NO_MALLOC` keeps the kernel heap-free |
| Wear | `block_cycles` relocates blocks so a hot metadata block cannot wear out one sector |

Nothing in the kernel links against the filesystem unless it is compiled in;
`svcrt_fs.c` is the only file that talks to littlefs.

## Upgrading

1. Download the new tag from upstream.
2. Copy `lfs.c`, `lfs.h`, `lfs_util.c`, `lfs_util.h`, `LICENSE.md` over these.
3. Re-check `../lfs_svcrt_config.h` against the new `lfs_util.h` (new optional
   `LFS_NO_*` switches may have appeared) and rebuild.
4. Run the on-board test (`fs test`) — a disk format change would show up as a
   mount failure on an existing volume, which is exactly what
   `LFS_DISK_VERSION` guards.
