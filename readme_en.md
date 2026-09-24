# SVCrtOS

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Release](https://img.shields.io/badge/release-0.0.2-brightgreen.svg)](https://gitee.com/xw19981010/svcrtos_new/tags)
[![Gitee](https://img.shields.io/badge/gitee-svcrtos__new-red.svg)](https://gitee.com/xw19981010/svcrtos_new)

**[简体中文](readme.md) | English**

SVCrtOS is a secure real-time operating system for ARM Cortex-M microcontrollers. It uses SVC
(Supervisor Call) for privilege separation, the MPU for memory protection, and ships a partitioned
scheduler plus a unified device driver framework.

**It makes exactly one promise: an MCU can install, upgrade and uninstall applications and drivers
the way a phone does.**
No hard-coded addresses, no kernel rebuild, no full-chip reflash. Applications and drivers are
packaged as `.svcapp` images, written into a single on-chip image pool over the serial port, claimed
at boot, accounted for when they crash, and reclaimed from when they are removed.

## How it differs from a general-purpose RTOS

A general-purpose RTOS (FreeRTOS / RT-Thread / Zephyr) compiles applications into the firmware:
changing one application means editing code, rebuilding, reflashing and resetting the whole device.
SVCrtOS splits the firmware into three images - **kernel / driver / application** - that can each be
upgraded on their own, which opens up a capability line nobody else has:

| Aspect | General-purpose RTOS | SVCrtOS |
|---|---|---|
| Changing an application | Edit code, rebuild firmware, reflash the device | Send a `.svcapp` over serial or from the GUI; claimed automatically at boot |
| Address coupling | Hard-coded in one linker script at build time | Addresses are defined once in `config/svcrt_partition.h` and derived everywhere else; loader/app projects are **forbidden** to include the partition header |
| Driver deployment | Compiled with the kernel | Compiled as a standalone firmware for its own ROM partition, flashed and upgraded independently; apps only know the device name, never the address |
| Privilege model | All code has the same privilege (MPU usually off) | Tasks run unprivileged and trap into the kernel through SVC; the MPU isolates each task and hardware stops out-of-bounds access on the spot |
| An application stuck in a fault loop | Watchdog resets the whole device | A crash journal survives resets; three consecutive crashes disable **that slot only**, everything else keeps running |
| Uninstalling | Not a concept | Invalidate the image, erase the physical unit once it is free, `pool free` goes back from 776244 B to 786432 B |
| Replacing a version while running | Stop and restart | Two mini apps hand the control loop over; the iteration counter stays continuous, the measured gap is 27-407 ms |
| Porting existing C code | Rewrite against the RTOS API | A POSIX / Windows compatibility layer: edit the include list and it builds for the board |

> This table only compares the dynamic-installation storyline. Ecosystem, protocol stacks and
> functional certification are different axes, and this project does not claim to be better there.

## Measured numbers at a glance

Every number below comes from something the device actually returned, or from the kernel's own
export - none is an estimate. The measurement conditions and how to reproduce them are documented
per item.

| Metric | Measured value | Source |
|---|---|---|
| One PendSV round trip (instrumentation off) | 710.5 -> **430.9 cycles** after the ready set became a 256-bit two-level bitmap plus per-priority lists (-39%) | [Scheduler notes](docs/调度器说明.md) |
| PendSV share of CPU (instrumentation off) | 0.768% -> **0.448%** | [Scheduler notes](docs/调度器说明.md) |
| Switch intervals over a 4.97 s window | All 5153 switches fall on the 10 us and 2000 us bands, with no outlier | This file, "Scheduler behaviour" |
| Idle share of CPU time | `idle` **99.48%** | This file, "Scheduler behaviour" |
| MPU violation protection | `pass=78 fail=0` (an app deliberately violating its region is stopped) | [Memory protection](docs/内存保护与App越权验证.md) |
| Unprivileged FPU use | APP_DEMO section 8, seven checks pass: values written to S16-S19 read back correctly across a task switch | [Install & debug guide](docs/SVCrtOS应用安装与调试指南.md) |
| Concurrent VFS ports | Two writers plus one read-only reader on the same path; the reader only ever sees complete payloads, `pass=128 fail=0` | [VFS namespaces](docs/VFS路径命名空间.md) |
| File POSIX | `FS_DEMO` board self-test passes end to end (`open/read/write/stat/dirent` routed into the VFS) | [POSIX layer](docs/POSIX与Windows兼容层说明.md) |
| socket / select | F427 with `SOCKET_DEMO`, `pass=36 fail=0` (TCP echo, UDP, IPv6 rejection) | [socket layer](docs/socket与select兼容层.md) |
| Upgrade while running | Handover gaps of **27 / 31 / 224 / 407 ms**, `it=994 -> resumed at it=994`, controlled variable never resets | [Seamless upgrade](docs/无缝升级例程.md) |
| 32-bit tick wraparound | Injected so that `svcrt_kernel_tick` wraps; the two `APP_ALIVE` lines around it are exactly 5.000 s apart | [Scheduler notes](docs/调度器说明.md) section 2.3 |
| Two boards, one source | F427 and F401 both build from the same kernel sources (F401 `Code=46526`) | This file, "Verification status" |

## Highlights

- **SVC privilege separation**: user tasks enter kernel mode through SVC; kernel code is protected by
  hardware and cannot be touched directly from a user task
- **MPU memory protection**: per-task ROM/RAM isolation on Cortex-M MPU, blocking out-of-bounds access
- **Priority plus time-slice scheduling**: multi-priority preemption and round-robin within a priority
- **Event primitives**: lightweight event synchronisation with timeout waits
- **Semaphores and mutexes**: counting semaphores plus mutexes with priority inheritance against
  priority inversion
- **Device driver framework**: one I/O interface (`open/close/read/write/ctrl`) for built-in drivers
- **Standalone driver firmware**: a driver can be compiled as a fully independent firmware, flashed to
  its own ROM partition, decoupled from the kernel and upgradable on its own; an app reaches it by
  device name alone, without knowing its implementation or address
- **FIFO buffers**: ring buffers for kernel-to-driver data transfer
- **Stack overflow detection**: PSP out-of-bounds detection per task
- **Task stack usage analysis**: stack painting plus low-water-mark sampling to measure each task's
  peak usage and size stacks from evidence
- **Spinlocks and scheduler lock**: kernel/driver-side spinlocks (atomic CAS and interrupt-masked
  variants, SMP-ready) and a user-mode scheduler lock for lightweight critical sections
- **CPU load accounting**: continuous idle-ratio statistics
- **FPU support**: Cortex-M4F/M7 floating-point registers (S16-S31) saved and restored on demand
- **One image pool, two installation strategies**: apps and drivers share a single pool and are packed
  at 1 KB granularity. The device can run in either "fixed slots" (placements and RAM windows come
  from the configuration, which suits handing the board to a customer for further development) or
  "automatic placement" (find the largest free run in the pool, reclaim space on uninstall)
- **On-device persistent configuration region**: installation strategy, slot table and runtime
  parameters live in their own configuration sector and are written online from the host
  (`tools/svcrt_cfg.py` or the GUI) without recompiling. Strategy fields take effect after a reset -
  the write command is `cfg load`, which *receives* the record rather than re-reading it
- **Thread service**: apps and drivers create threads inside their own firmware and RAM window
  (SVC 0x1B); both the entry point and the whole stack must land in the caller's own window or the
  call is refused
- **POSIX / Windows compatibility layer**: `pthread`, `semaphore`, `mqueue`, `unistd` alongside
  `CreateThread`, `Sleep`, `strcpy_s` and friends are directly usable, so porting an existing C
  program starts with the include list
- **Dual SDK architecture**: separate application SDK and driver SDK, both able to build standalone
  partition firmware
- **File system and VFS**: littlefs on a NOR block device, with the ark_vfs path namespace above the
  `svcrt_fs` facade; file POSIX calls (`open`, `read`, `write`, `stat`, `dirent`) go straight into the VFS
- **Mini apps**: a third kind of user-mode application that lives in the file system - not installed,
  no slot taken, loaded from the volume into RAM and executed, returning both memory blocks (code and
  RAM) to the kernel on exit. Multiple instances, crash-based disabling and boot autostart included
- **Upgrade while running**: two mini apps can hand a control loop over (one state record plus one
  request record, governed by three hard rules) - the old version releases, the new one takes over,
  with no downtime and no reset of the controlled variable
- **Kernel decoupled from silicon**: an RT-Thread-style layering where the kernel has zero chip
  dependencies, so a port touches only `board/`

## Examples

Every project under `example/stm32f427/` builds with `UV4 -r`, flashes with `-f`, and runs its own
board self-test:

| Project | Kind | What it demonstrates |
|---|---|---|
| `kernel/SVCRTOS_TEST` | Kernel | The kernel itself (the one running on the board) |
| `app_sdk/APP_DEMO` | App | Eight sections of full self-tests: tasks / events / semaphores / mutexes / message queues / soft timers / heap and pthread / FPU |
| `app_sdk/POSIX_DEMO` | App | A C program written for Linux or Windows, compiled as-is |
| `app_sdk/FS_DEMO` | App | File POSIX calls routed into VFS / littlefs |
| `app_sdk/SOCKET_DEMO` | App | POSIX socket / select on the lwIP loopback netif |
| `app_sdk/MINI_DEMO` | Mini app | Loaded from the file system into RAM, both memory blocks returned on exit |
| `app_sdk/SEAMLESS_V1` / `SEAMLESS_V2` | Mini apps | Handing a control loop over while running (seamless upgrade), see [Seamless upgrade](docs/无缝升级例程.md) |
| `app_sdk/BLED_APP` + `driver_sdk/BLED_DRV` | App + standalone driver | End-to-end example where three firmwares share no addresses, see `BLUE_LED_E2E_README.md` |
| `app_sdk/APP_BAD` | App | A deliberately broken image: crash accounting and the three-strikes rule |
| `driver_sdk/DRV_DEMO` | Driver | Driver skeleton on the same install channel as an app: `--type driver` -> `.svcapp` -> `install` |
| `example/stm32f401/kernel/SVCRTOS_TEST` | Kernel | What a chip change costs (only `board/` and the board partition header) |

## Supported CPU architectures

| Architecture | FPU | MPU | Port status |
|---|---|---|---|
| Cortex-M3 | - | Yes | Implemented |
| Cortex-M4 | Yes | Yes | Implemented (recommended) |
| Cortex-M7 | Yes | Yes | Implemented (reuses the M4 port conventions) |
| RISC-V (RV32/RV64) | Per core | PMP | Abstraction layer ready, port implementation pending |
| LoongArch (LA32/LA64) | Per core | TLB / address windows | Abstraction layer ready, port implementation pending |

### The architecture abstraction layer

Between the kernel and an instruction set there is exactly one contract, and it is confined to two
headers: `kernelsrc/include/svcrt_arch.h` (architecture description and derived capabilities) and
`svcrt_hal.h` (port interface declarations).

1. **Two-level architecture description**: `SVCRT_ARCH_FAMILY_xxx` (family) plus `SVCRT_CPU_CORE_xxx`
   (core), selected by `SVCRT_ARCH_CORE` or the older `SVCRT_CPU_ARCH`. Capability defaults such as
   FPU, MPU, privilege levels and SVC-number width are derived from the core automatically.
2. **Opaque system-call context**: the kernel's `SVC_Server(void *)` touches the call number,
   arguments and return value only through `SVCRT_SVC_NUM`, `SVCRT_SVC_ARG` and `SVCRT_SVC_RET`.
   The stack frame layout belongs to the port layer - Cortex-M uses the hardware-stacked frame,
   RISC-V and LoongArch use their own trap frames.
3. **Three context-switch entry points**: `svcrt_port_stack_init()` (prepare a stack frame),
   `svcrt_port_switch_task()` (trigger a switch) and `svcrt_port_enter_idle()` (start the first context).
4. **One description of memory protection**: `svcrt_arch_mpu_t` (`region_base` / `region_attr`, two
   groups of registers) carries a task's isolation context. Cortex-M maps it to `RBAR/RASR`, RISC-V to
   `pmpaddr/pmpcfg`, LoongArch to TLB or address windows. The kernel never sees the mechanism.
5. **Stateful critical sections**: `svcrt_port_enter_critical()` / `exit_critical()` save and restore
   the interrupt state and nest correctly, covering architectures that must preserve `mstatus.MIE`
   (RISC-V) or `CRMD.IE` (LoongArch).
6. **One naming for clocks**: `svcrt_port_get_timer_counter()` / `get_timer_reload()` hide the
   difference between SysTick, `mtime` and a constant-rate timer.

Adding an architecture means implementing those interfaces under
`kernelsrc/port/<family>/<core>/` with **no change to kernel sources**. The full procedure and the
acceptance checklist are in the [port layer notes](kernelsrc/port/README.md).

## Repository layout

```
SVCRTOS/
├── kernelsrc/                      # kernel sources (zero chip dependencies)
│   ├── include/                    # kernel headers
│   │   ├── svcrt.h                 # application API
│   │   ├── svcrt_arch.h            # architecture layer (family/core/capabilities/MPU context)
│   │   ├── svcrt_config.h          # central configuration (overridable per board)
│   │   ├── svcrt_port.h            # hardware abstraction interface (declarations only)
│   │   ├── svcrt_types.h           # base types
│   │   └── ...                     # other internal kernel headers
│   ├── src/                        # kernel sources (plain C, no hardware access)
│   │   ├── svcrt_init.c            # startup and initialisation
│   │   ├── svcrt_task.c            # scheduling + SVC dispatch
│   │   ├── svcrt_event.c           # events
│   │   ├── svcrt_fifo.c            # FIFO ring buffers
│   │   ├── svcrt_dev.c             # device driver framework
│   │   └── svcrt_cfg.c             # task configuration loading
│   ├── app/                        # application template
│   ├── sdk/                        # SDK packages
│   │   ├── app_sdk/                # application SDK
│   │   ├── driver_sdk/             # driver SDK
│   │   └── posix/                  # POSIX / Windows compatibility layer (see its README)
│   ├── shell/                      # kernel console (ark-shell plus a SVCrtOS platform layer)
│   └── components/                 # optional components
│
├── board/                          # board port layer (chip dependent)
│   └── stm32f427/                  # STM32F427 example port
│       ├── svcrt_board.c           # port interfaces + interrupt entry points + on-board devices
│       ├── svcrt_board_config.h    # board configuration overrides (clock, memory addresses)
│       ├── drvuart.c/h             # UART driver (HAL based)
│       └── drvled.c/h              # LED driver (HAL based)
│
├── kernelsrc/port/                 # CPU port layer: port/<family>/<core>/
│   ├── README.md                   # port layer notes and a guide to adding an architecture
│   ├── arm/cortex-m3/              # Cortex-M3 (no FPU)
│   └── arm/cortex-m4/              # Cortex-M4
│       ├── svcrt_context.S         # PendSV context switch assembly (FPU registers included)
│       └── svcrt_port.c            # SVC context / stack frame / critical section / timer / MPU
│
├── tools/                          # host tools (gen_scatter / pack_app / svcrt_layout / gen_api_doc)
├── docs/                           # documentation, index in docs/README.md
└── example/                        # example projects
    └── stm32f427/                  # MDK projects
```

**Decoupling rules:**

- `kernelsrc/` is the pure kernel: no chip headers, no direct register access
- `board/` is chip dependent; porting to a new chip means creating `board/<chip>/`
- `svcrt_port.h` and `svcrt_hal.h` are the only coupling points between kernel and hardware, and they
  contain declarations only
- `svcrt_arch.h` describes architectures and derives capabilities; a new architecture only implements
  interfaces under `port/<family>/<core>/`, leaving the kernel untouched

## System architecture

```
┌──────────────────────────────────────────────────────────────────────┐
│                  User layer (Unprivileged)                            │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐               │
│  │  App Task1   │  │  App Task2   │  │  App TaskN   │               │
│  │  (svcrt.h)   │  │  (svcrt.h)   │  │  (svcrt.h)   │               │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘               │
│         │  SVC 0x10~0x1E  │                 │                        │
├─────────┼─────────────────┼─────────────────┼────────────────────────┤
│         └─────────────────┼─────────────────┘                        │
│           Kernel kernelsrc/ (zero chip dependencies)                  │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐               │
│  │  Scheduler   │  │   Events     │  │   Devices    │               │
│  └──────────────┘  └──────────────┘  └──────────────┘               │
│  ┌──────────────┐  ┌──────────────┐                                  │
│  │  FIFO        │  │  Config load │                                  │
│  └──────────────┘  └──────────────┘                                  │
├──────────────────────────────────────────────────────────────────────┤
│        svcrt_port.h (hardware abstraction, declarations only)         │
├──────────────────────────────────────────────────────────────────────┤
│        board/<chip>/ (board port layer)                                │
│   svcrt_board.c / svcrt_mpu.c / drivers / interrupt entry points      │
└──────────────────────────────────────────────────────────────────────┘
```

## Application API

Including `svcrt.h` makes every operating-system call available; each call traps into kernel mode
through an SVC instruction.

### Timeout semantics (one rule for every blocking call)

The waiting calls of events, semaphores, mutexes and message queues share a single `timeout_ms`
convention, the same one FreeRTOS, RT-Thread and Zephyr use:

| Value | Meaning |
|---|---|
| `timeout_ms == 0` | Do not wait: try once and return a timeout immediately if the condition is not met |
| `timeout_ms > 0` | Wait at most `timeout_ms` milliseconds |
| `timeout_ms < 0` | Wait forever, until the condition is met |

```c
svcrt_mutex_lock(mtx, -1);   /* wait forever (preferred: the intent is visible) */
svcrt_mutex_lock(mtx, 0);    /* try once, return a timeout if not available */
```

> Early versions treated 0 as "wait forever", which is the opposite of mainstream RTOS practice and
> was inconsistent between the three families of calls in the same header. The convention above is now
> uniform, and every call site in the repository spells out `-1` explicitly.

### Task management

| API | Description |
|---|---|
| `svcrt_task_wait(ms)` | Sleep for the given number of milliseconds (scheduling) |
| `svcrt_task_wait_period()` | Wait for the end of the current period |
| `svcrt_task_delay(us)` | Microsecond busy-wait |
| `svcrt_task_kill()` | Terminate the current task |

### Device operations

| API | Description |
|---|---|
| `svcrt_dev_open(name, param)` | Open a device, returning a handle |
| `svcrt_dev_close(handle)` | Close a device and release the handle |
| `svcrt_dev_read(handle, buf, len)` | Read from a device |
| `svcrt_dev_write(handle, buf, len)` | Write to a device |
| `svcrt_dev_ctrl(handle, code, value)` | Device control command |

### Events and system information

| API | Description |
|---|---|
| `svcrt_event_create(name)` | Create a named event |
| `svcrt_event_wait(handle, timeout)` | Wait for an event (with timeout) |
| `svcrt_event_set(handle)` | Signal an event |
| `svcrt_get_time_ms()` | System uptime in milliseconds |
| `svcrt_get_cpu_usage()` | CPU idle ratio |

### Semaphores and mutexes

| API | Description |
|---|---|
| `svcrt_sem_create(name, init)` | Create a counting semaphore |
| `svcrt_sem_wait(handle, timeout)` | Wait on a semaphore |
| `svcrt_sem_post(handle)` | Post a semaphore |
| `svcrt_sem_delete(handle)` | Delete a semaphore |
| `svcrt_mutex_create(name)` | Create a mutex |
| `svcrt_mutex_lock(handle, timeout)` | Lock (priority inheritance supported) |
| `svcrt_mutex_unlock(handle)` | Unlock (owner only) |
| `svcrt_mutex_delete(handle)` | Delete a mutex |

### Message queues (SVC 0x16)

| API | Description |
|---|---|
| `svcrt_mq_create(name)` | Create a queue (depth `SVCRT_MQ_DEPTH`, message up to `SVCRT_MQ_MSG_WORDS` words) |
| `svcrt_mq_send(h, buf, len_words, timeout)` | Send a message of `len_words` words (1..`SVCRT_MQ_MSG_WORDS`; copy semantics, blocks with timeout when full) |
| `svcrt_mq_recv(h, buf, len_words, timeout)` | Receive a message into a caller buffer of `len_words` words (blocks with timeout when empty) |
| `svcrt_mq_delete(h)` | Delete a queue |

**Messages are variable length**: the caller states `len_words`, the kernel copies exactly that many
words, zero-fills the rest of the slot and records the real length inside the slot. `len_words` must
not exceed the receiver buffer capacity - **the caller buffer is a hard boundary**.

Return values: `svcrt_mq_send` returns 0 on success and a negative value on timeout or error;
`svcrt_mq_recv` returns the **number of words actually received** (>0), or a negative value on timeout
or error.

### Soft timers (SVC 0x17)

| API | Description |
|---|---|
| `svcrt_timer_create(name)` | Create a timer |
| `svcrt_timer_start(h, period_ms, mode, cb, arg)` | Start it; mode selects one-shot or periodic |
| `svcrt_timer_stop(h)` | Stop it |
| `svcrt_timer_delete(h)` | Delete it |

Expiry callbacks always run in the context of the kernel's timer service task (priority
`SVCRT_TIMER_TASK_PRI`). The tick interrupt only counts down and wakes the service task, so user
callbacks never execute in an interrupt and SVC privilege separation stays intact.

### Task diagnostics and fault recovery (SVC 0x11 / 0x12 extensions)

| API | Description |
|---|---|
| `svcrt_task_status_get(task_id)` | Query task state (READY/WAIT/RUNNING/INVALID) |
| `svcrt_task_recover_req(task_id)` | Request task recovery (two-phase: rebuild the stack frame, then reschedule) |
| `svcrt_fault_record_count()` | Number of fault records |
| `svcrt_fault_record_read(index, out3)` | Read record `index` (type / task id / tick) |

HardFaults and stack overflows automatically write into a fault ring buffer (capacity
`SVCRT_FAULT_RECORD_NUM`, oldest overwritten when full). With `SVCRT_USE_FAULT_RECOVER` enabled, a
faulting task is rebuilt and resumes instead of losing its slot forever.

### Thread service (SVC 0x1B)

Apps and drivers create threads **inside their own RAM window**. A thread needs a TCB and a scheduler
slot, which only the kernel owns, so this is the only way to create one:

| API | Description |
|---|---|
| `svcrt_thread_create(entry, stack, stack_size, priority, period_ms)` | Create a thread, returning a task id (>0); `period_ms=0` is event driven, >0 is periodic |
| `svcrt_thread_self()` | The current task id (>0) |
| `svcrt_thread_exit()` | End the current thread, never returning |

Creation performs two boundary checks and refuses rather than silently accepting:

1. **The entry point must lie inside the caller's own firmware window**;
2. **The whole stack must lie inside the caller's own RAM window** (the kernel never allocates a stack
   for someone else and never lets one cross windows).

`priority` ranges over 1..254; `255` is the scheduler's "no candidate" sentinel and is explicitly
rejected.

If you would rather not call this directly, `kernelsrc/sdk/posix/` provides a POSIX / Windows
compatibility layer so `pthread_create` or `CreateThread` style code works; see
[POSIX与Windows兼容层说明.md](docs/POSIX与Windows兼容层说明.md).

### Interrupt-safe API (called directly from privileged context, no SVC)

| API | Description |
|---|---|
| `svcrt_sem_post_from_isr(h)` | Post a semaphore from an ISR (wakes a task, does not switch) |
| `svcrt_event_set_from_isr(h)` | Signal an event from an ISR |
| `svcrt_mq_send_from_isr(h, buf, len)` | Send a message from an ISR (returns -1 when full) |

## Driver development

SVCrtOS delivers drivers in three forms. **The headline feature is that a driver travels the exact
same install channel as an app**: a driver is just a `.svcapp` with `type = driver`, installed with
the same command into the same image pool, and the kernel routes it to a driver slot by the type in
the image header.

| Form | How it is built | Deployment | Where it fits |
|---|---|---|---|
| Built-in driver | Compiled with the kernel | Same firmware as the kernel | Fixed on-board peripherals |
| **Pool-installed driver** | **Built on its own + `pack_app.py --type driver`** | **Serial `install` into the unified image pool, same `.svcapp` envelope as an app** | **Drivers shipped with an app, installed/uninstalled/upgraded** |
| **Standalone driver** | **Built as its own .bin/.hex** | **Flashed into a dedicated ROM partition** | **Drivers that are hot-swapped or upgraded independently** |

```bash
# The only difference from packing an app is --type driver (type = 2 in the image)
python tools/pack_app.py --project example/stm32f427/driver_sdk/DRV_DEMO/MDK-ARM/drv_demo.uvprojx \
    --type driver --name DRV_DEMO --out build/DRV_DEMO/DRV_DEMO.svcapp
# On the board type install to open the receive window (install <slot> in fixed mode),
# then send the file as-is. Check with drv list: a type=drv slot record must appear.
```

### Standalone drivers (external driver)

A standalone driver is a firmware with **its own startup code, its own scatter file and its own link
step**, flashed into ROM/RAM partitions outside the kernel. The kernel registers the partition entry
as a task and schedules it; the driver runs unprivileged and registers its device with the kernel
through SVC `0x14`. Once registered, **any app - built-in or standalone - can open it by device name**
without knowing the implementation or the address.

```
┌─────────────────────┐   ┌─────────────────────┐   ┌─────────────────────┐
│   Kernel firmware   │   │  Standalone driver  │   │  Standalone app     │
│   (ROM partition 0) │   │   BLED (part. 1)    │   │   (ROM partition 2) │
│                     │   │                     │   │                     │
│ Scheduler/devices   │   │  DrvMain()          │   │  AppMain()          │
│         ▲           │   │   ├ register "BLED" │   │   ├ open("BLED")    │
│         │ SVC 0x14  │?──┼───┘ (svcrt_drv_     │   │   └ write(...)      │
│         │ register  │   │      register)      │   │         │           │
│         │           │   │                     │   │         │ SVC 0x10  │
│         └───────────┼───┼─────────────────────┼───┼─────────┘ device IO │
│      the kernel dispatches by function pointer; no firmware knows any address │
└─────────────────────┘   └─────────────────────┘   └─────────────────────┘
```

A driver developer includes `svcrt_driver_sdk.h`, implements the `svcrt_dev_drv_t` interface and
registers it in `DrvMain()`:

```c
#include "svcrt_driver_sdk.h"

/* 1. implement the five standard device entry points */
static svcrt_dev_drv_t bled_drv = {
    bled_drv_open,
    bled_drv_close,
    bled_drv_read,
    bled_drv_write,
    bled_drv_ctrl
};

/* 2. standalone firmware entry: registering the name is all an app needs */
void DrvMain(void)
{
    svcrt_drv_register("BLED", &bled_drv, 0);   /* SVC 0x14 registers with the kernel */

    while(1)
    {
        svcrt_task_wait(1000);   /* stay resident for background work, or just return */
    }
}
```

The application side does not care whether the driver is built in or standalone; it uses the name:

```c
int32 h = svcrt_dev_open("BLED", 0);   /* the kernel looks it up and routes to the driver */
int32 on = 1;
svcrt_dev_write(h, &on, 1);            /* light the blue LED */
```

> **Notes for standalone drivers**
> 1. The startup assembly must go through the C library entry `__main` so that `.data` is copied and
>    `.bss` is cleared. Otherwise the `bled_drv` function-pointer table holds random values and the
>    first call after registration HardFaults.
> 2. The ROM/RAM partitions of driver, application and kernel firmware must not overlap (each one's
>    scatter file states them).
> 3. Under MPU isolation a standalone driver that touches peripheral registers directly needs the
>    kernel to grant that peripheral region.
>
> A complete runnable example is `example/stm32f427/driver_sdk/BLED_DRV/` (driver) with
> `example/stm32f427/app_sdk/BLED_APP/` (application); the end-to-end write-up is
> `example/stm32f427/BLUE_LED_E2E_README.md`.

## Kernel configuration

All configuration lives in `svcrt_config.h`, and chip-dependent values can be overridden by the board
configuration file.

| Parameter | Default | Description |
|---|---|---|
| `SVCRT_CPU_ARCH` | Derived from the arch header (`SVCRT_ARCH_CORTEX_M4` on this board) | CPU architecture selection |
| `SVCRT_USE_FPU` | Derived, 1 on this board | Floating-point unit |
| `SVCRT_USE_MPU` | Derived, 1 on this board | MPU protection. On Cortex-M4 it defaults to `SVCRT_ARCH_HAS_MPU` and is never explicitly disabled anywhere in the tree; mechanism and board verification in [docs/内存保护与App越权验证.md](docs/内存保护与App越权验证.md) |
| `SVCRT_USE_PRIV` | Requires `SVCRT_USE_MPU`, 1 here | Privilege separation. With `SVCRT_USE_MPU == 1` it becomes 1 automatically: app tasks are unprivileged (`CONTROL.nPRIV = 1`), kernel tasks stay privileged |
| `SVCRT_TASK_MAX_NUM` | 48 | Total task table capacity (static TCB array). The default now lives in **section 9 of `config/svcrt_partition.h`**, next to the partition strategy - see "Task capacity layering" |
| `SVCRT_TASK_TABLE_RAM_MAX` | 8192 | RAM budget for the TCB array in bytes, **fixed at 8 KB and not derived**; a guard assertion compares `sizeof(svcrt_task_table)` and fails the build when exceeded |
| `SVCRT_TICK_PERIOD_US` | 500 | Tick period in microseconds |
| `SVCRT_EVENT_NUM` | 10 | Number of event objects |
| `SVCRT_MAX_EVENT_WAITERS` | 4 | Waiters per event |
| `SVCRT_SEM_NUM` / `SVCRT_MTX_NUM` | 8 / 8 | Semaphore / mutex objects |
| `SVCRT_MAX_SYNC_WAITERS` | 4 | Waiters per semaphore or mutex |
| `SVCRT_DEV_MAX_NUM` | 8 | Maximum number of devices |
| `SVCRT_USE_SPINLOCK` | 1 | Spinlocks (svcrt_spin.h) |
| `SVCRT_USE_SCHED_LOCK` | 1 | User-mode scheduler lock (SVC 0x11 sub-commands 7-9) |
| `SVCRT_USE_STACK_USAGE` | 1 | Task stack peak-usage statistics |
| `SVCRT_STACK_FILL_PATTERN` | 0xcdcdcdcd | Stack painting pattern for watermarks |
| `SVCRT_USE_CPU_LOAD` | 1 | CPU load accounting |
| `SVCRT_USE_STACK_CHECK` | 1 | Stack overflow detection |
| `SVCRT_SHARE_MEM_ADDR` / `SVCRT_SHARE_MEM_SIZE` | - | **Removed**: shared memory is placed by `SHARE_RAM_BASE` / `SHARE_RAM_SIZE` in `config/svcrt_partition.h` |
| `SVCRT_SYSTEM_CLOCK_HZ` | 168000000 | System clock (overridden by the board configuration) |
| `SVCRT_USE_MQ` / `SVCRT_MQ_NUM` | 1 / 8 | Message queues, on/off and count |
| `SVCRT_MQ_DEPTH` / `SVCRT_MQ_MSG_WORDS` | 8 / 4 | Queue depth and maximum message length in words |
| `SVCRT_USE_TIMER` / `SVCRT_TIMER_NUM` | 1 / 8 | Soft timers, on/off and count |
| `SVCRT_TIMER_TASK_PRI` | 200 | Timer service task priority |
| `SVCRT_TIMER_TASK_STACK_WORDS` | 96 | Timer service task stack in words |
| `SVCRT_USE_FAULT_RECOVER` | 1 | Automatic task fault recovery |
| `SVCRT_FAULT_RECORD_NUM` | 8 | Fault ring buffer capacity |
| `SVCRT_USE_STACK_CHECK` / `SVCRT_STACK_END_FLAG` | 1 / 0xed01 | Stack overflow detection and its guard word |

Every configuration macro is wrapped in `#ifndef`, and the board configuration is loaded before
`svcrt_config.h`, so **overriding at board level is the only entry point** and `kernelsrc/` never needs
editing.

### Partition and image strategy switches (`config/svcrt_partition.h`)

Addresses and partition layout are defined exactly once in `config/svcrt_partition.h`; everything else
is derived. Scatter files are generated by `tools/gen_scatter.py` and are build products - **do not
edit them by hand**. The switches that change runtime behaviour:

| Macro | Default in `config/` | Description |
|---|---|---|
| `APP_AUTO_START` / `DRIVER_AUTO_START` | 1 | **Only affects the raw-image path** (an image flashed straight from Keil has no image header and therefore no autostart flag of its own). For `.svcapp` images autostart comes from the header `flags` field and can be overridden per slot in the on-device configuration region |
| `APP_ALLOW_RAW_IMAGE` | 0 | Whether raw images (firmware flashed into the pool without a header) are claimed. The compile-time default is 0 and the F427 board sets it to 1; **the `RAW_ALLOW` bit of the on-device configuration `flags` must also be set at runtime** - both must hold |
| `APP_CRASH_RESTART_MAX` | 3 | Consecutive crash-restart limit for apps and drivers; reaching it disables the image. 0 means unlimited |
| `INSTALLER_ENABLE` | 1 | The in-kernel installer module (uses COM1); set to 0 when installing over serial is not wanted |
| `SHELL_ENABLE` | 1 | The kernel shell console (ark-shell, uses `SHELL_DEV_NAME`); when 1 the resident installer task is not registered and installation is driven by the one-shot `install` command |
| `INSTALLER_FIXED_SLOT` | -1 | Target slot for the resident installer in fixed-slot mode (configuration slot index). `-1` means unspecified, and the resident task then rejects every frame. Setting it to N stops any image running in that slot at boot, erases the whole slot, and then accepts exactly one frame - reboot to install again |
| Image header `flags` | 0x1 | Per-slot autostart bit, written at packaging time by `tools/pack_app.py --autostart / --no-autostart`; visible at runtime in the `auto` column of `app list` / `drv list` |
| `SLOT_MAX` / `SVCRT_CFG_SLOT_MAX` | 16 / 8 | Slot record limits (shared-table array / on-device configuration capacity). Both must be <= `SVCRT_SLOT_ARRAY_MAX` (16, in `svcrt_share.h`); a compile-time assertion catches violations |
| `SVCRT_DEV_SLOT_MAX` / `SVCRT_DEV_RAM_WINDOW` | 4 / 16 KB | Development slot entries and the RAM window of each. The window index is "start unit number modulo entry count", the same formula as `dev_ram_base` in `tools/gen_scatter.py` |
| `SVCRT_RECLAIM_MODE` | `SVCRT_RECLAIM_GLOBAL` | Reclaim effort on uninstall in automatic-placement mode: compact globally, or only touch sectors that contain invalidated bytes |
| `DRIVER_TASK_STACK_SIZE` / `APP_TASK_STACK_SIZE` | 1K / 4K | Driver and app task stacks, cut from the top of **their own slot's RAM block** |

> Note: loader and app projects are **forbidden** to `#include "svcrt_partition.h"`; the layout is
> obtained at runtime through SVC 0x18.

### Task capacity layering

The task table is a static TCB array whose capacity is fixed at build time. So that scenarios with
many user drivers and many user apps can be scaled up directly, capacity is layered into
**kernel / driver / application**, all in section 9 of `config/svcrt_partition.h` (the same file as
the flash/RAM layout, so one cannot be changed without noticing the other):

| Macro | Default | Description |
|---|---|---|
| `SVCRT_TASK_MAX_NUM` | 48 | Total capacity; scaling up means changing this number only |
| `SVCRT_TASK_MAX_KERNEL` | 8 | Reserve for kernel-owned and example static tasks |
| `SVCRT_TASK_PER_DRIVER` | 1 | Tasks consumed by each driver image |
| `SVCRT_TASK_PER_APP` | 1 | Tasks consumed by each app image |
| `SVCRT_TASK_NEED_MIN` | Derived | `kernel + slots x tasks per slot`, the minimum the partition strategy requires |
| `SVCRT_TASK_TABLE_RAM_MAX` | 8192 | RAM budget for the static TCB array in bytes; measured `sizeof(svcrt_task_t)` is 76 bytes (MPU off) / 140 bytes (MPU on) |

The compile-time assertion `svcrt_task_capacity_check` rejects a configuration whose capacity cannot
hold the current slot strategy, and `svcrt_task_table_ram_check` in `svcrt_cfg.c` checks the RAM
budget afterwards.

`svcrt_config.h` no longer carries defaults for those two macros; it includes
`svcrt_partition.h` instead. The board configuration still wins, because the definitions in the
partition header are wrapped in `#ifndef` and the board configuration is loaded first.

### Partitions, slots and the ABI version

Flash layout (F427, 1 MB): `BOOT(0)` -> `KERNEL(128 KB)` -> `CONFIG(128 KB, one physical sector)` ->
`IMAGE_POOL(768 KB, 6 x 128 KB units)`.

| Item | Value | Description |
|---|---|---|
| Kernel region | `KERNEL_SIZE` = 128 KB @ `0x08000000` | **Adjustable to the build result** (give the kernel more when it grows); the change is this one macro |
| Configuration region | `CONFIG_SIZE` = 128 KB @ `0x08020000` | On-device persistent configuration: installation strategy, slot table, runtime parameters, written online from the host |
| Image pool | `IMAGE_POOL_SIZE` = 768 KB @ `0x08040000`, 6 x 128 KB units | Apps and drivers **share one pool**, allocated at 1 KB granularity inside a unit |
| Slot records | `SLOT_MAX` = 16 (`SVCRT_CFG_SLOT_MAX` = 8) | How many images can be recorded, <= `SVCRT_SLOT_ARRAY_MAX` (16) |
| Development slots | `SVCRT_DEV_SLOT_MAX` = 4, window `SVCRT_DEV_RAM_WINDOW` = 16 KB | Serves only the "flash from Keil and set breakpoints" bypass; raw images never move and are not reclaimed |
| RAM pool | `SLOT_RAM_TOTAL` = 64 KB @ `0x20010000` | Buddy allocation, blocks from 1 KB to 32 KB |
| Shared-memory ABI | `SVCRT_PARTITION_VERSION` = **7** | The partition table, configuration region and development slot table all exchange this structure |
| Hardware compatibility id | `SVCRT_HW_COMPAT_ID` = **`0x42700005`** | The low 16 bits are the image compatibility generation; older `.svcapp` files are rejected by the compatibility check and must be repackaged |

**Two installation strategies, one per device**, chosen by the on-device configuration region
(compile time only supplies the fallback default):

| Strategy | Meaning |
|---|---|
| **Fixed slots** (`fixed`) | Image placement and RAM windows follow the configured slots. Uninstalling erases only that slot; nothing is compacted and no space returns to the pool. Suits "the board is handed to a customer who develops further and flashes from MDK directly" |
| **Automatic placement** (`auto`) | Installation finds the largest free run in the pool and packs at 1 KB granularity; uninstalling compacts globally or moves minimally according to `SVCRT_RECLAIM_MODE` |

> Two rules capture the boundary: **addresses are defined exactly once, in
> `config/svcrt_partition.h`**, and everything else is derived; **loader and app projects are forbidden
> to include `svcrt_partition.h`**, obtaining the layout at runtime through SVC 0x18.

The byte layout of configuration records, error codes, slot table semantics and host tooling are in
[配置区与安装策略.md](docs/配置区与安装策略.md).

## API reference

Kernel headers use Doxygen-style comments, so the reference can be generated in one command:

```bash
python tools/gen_api_doc.py          # HTML when Doxygen is present, Markdown otherwise
python tools/gen_api_doc.py --md     # force Markdown
```

- With Doxygen installed: `docs/api/html/index.html` (cross references, call graphs)
- Without Doxygen: `docs/api/SVCrtOS_API参考.md` (built-in parser, zero dependencies)

> The kernel sources mix GBK and UTF-8, so the script first writes UTF-8 copies into the system
> temporary directory and generates from those. No source file in the repository is modified;
> configuration lives in `Doxyfile` at the repository root.

## Board responsibilities: the tick entry point and the HAL millisecond base

`SysTick_Handler` belongs to the **board**; the kernel only exposes
`svcrt_kernel_tick_handler()`. Besides feeding the kernel tick, a board that uses the STM32 HAL must
also advance the HAL's millisecond base there:

```c
void SysTick_Handler(void)
{
    /* kernel tick is 500us = 2kHz, the HAL base is 1ms = 1kHz, so tops it up every 2 ticks */
    if((svcrt_kernel_get_tick() % (1000u / SVCRT_TICK_PERIOD_US)) == 0u)
    {
        HAL_IncTick();
    }

    svcrt_kernel_tick_handler();
}
```

**Why this is mandatory**: the CubeMX-generated `SysTick_Handler` (the only caller of `HAL_IncTick()`
in the whole project) is masked out by `stm32f4xx_it.c`. Without this bridge `HAL_GetTick()` stays 0
forever and every `HAL_Delay()` waits forever, because that is exactly what it waits on. The HAL is
chip-dependent code, so the bridge can only live in `board/`, and the kernel keeps zero chip
dependencies.

## Porting

SVCrtOS ports to any ARM Cortex-M MCU by adding a chip directory under `board/`.

1. **Create the board directory**: `board/<your chip>/`
2. **Create `svcrt_board_config.h`**: clock and memory addresses
3. **Implement `svcrt_board.c`**: every interface declared in `svcrt_port.h`, plus MPU operations
   (skippable on M3)
4. **Write the interrupt entry points**: `SysTick_Handler` forwards to
   `svcrt_kernel_tick_handler()` (STM32 HAL boards must also call `HAL_IncTick()`, see above), and
   `HardFault_Handler` must hand over to the kernel fault handler instead of looping forever
5. **Port the context switch assembly**: start from
   `kernelsrc/port/arm/cortex-m4/svcrt_context.S` (FPU handling differs between core revisions)
6. **Write the on-board drivers**: UART, LED and so on

**The key point: `kernelsrc/` needs no modification at all.**

Step-by-step instructions are in the [porting manual](kernelsrc/porting_manual.md).

## Out-of-kernel firmware (user apps and drivers)

Beyond compiling tasks into the kernel, SVCrtOS builds applications and drivers as **standalone
firmware** that is flashed into dedicated ROM partitions, scheduled by the kernel, and therefore
decoupled and independently upgradable.

Complete examples are `example/stm32f427/app_sdk/` (applications), `example/stm32f427/driver_sdk/`
(drivers) and the blue-LED end-to-end walkthrough in
`example/stm32f427/BLUE_LED_E2E_README.md`.

Integration points (details in part three of the [SDK user manual](kernelsrc/sdk/sdk_user_manual.md)):

1. The loader claims an installed image: it takes the entry point from the image header or the slot
   table, allocates the RAM block for the slot and registers the task (the entry carries the Thumb bit
   `| 1`)
2. Standalone firmware startup assembly must go through the C library entry `__main` so `.data` is
   copied and `.bss` cleared, otherwise the driver interface table holds random values and HardFaults
3. Kernel and firmware ROM/RAM partitions must not overlap

> **Installation and layout as it stands**: apps and drivers share one 768 KB image pool (F427) and
> are packed at 1 KB granularity. The installation strategy (fixed slots or automatic placement) and
> the slot table come from the **on-device configuration region**; compile time only supplies the
> fallback default.
> Tooling: `tools/svcrt_layout.py` produces configuration records, `tools/svcrt_cfg.py` writes them
> into the configuration region over serial, `tools/gen_scatter.py --target image --unit N` generates
> each firmware's scatter file, and `tools/pack_app.py` builds `.svcapp` images. Humans use the GUI
> `tools/svcrt_host_gui.py` (connect / console / install / layout), automation follows
> `skills/svcrtos/SKILL.md` (see section 11 of the install and debug guide).
> The "customer flashes from MDK" path goes through the **development slot table** (raw images, no
> reclaiming).
> Full semantics in [配置区与安装策略.md](docs/配置区与安装策略.md) and
> [SVCrtOS应用安装与调试指南.md](docs/SVCrtOS应用安装与调试指南.md).

## Boot sequence

```
main()
  ├── svcrt_port_board_init()      // early hardware init (board)
  ├── svcrt_cfg_load()             // load the task configuration table
  ├── svcrt_port_irq_init()        // interrupt priorities (board)
  ├── svcrt_kernel_init()          // kernel modules (events / devices / MPU / ...)
  ├── svcrt_port_start_timer()     // start SysTick (board)
  └── svcrt_start_idle()           // switch to PSP, enter privileged mode
        └── WFI loop               // wait for the first task to become ready
```

## Naming conventions

SVCrtOS follows one naming scheme, in the spirit of FreeRTOS and RT-Thread:

| Category | Pattern | Examples |
|---|---|---|
| Application API | `svcrt_<module>_<action>` | `svcrt_task_wait()`, `svcrt_dev_open()` |
| Internal kernel functions | `svcrt_<module>_<action>_internal` | `svcrt_task_wait_internal()` |
| Scheduler functions | `svcrt_sched_<action>` | `svcrt_sched_next()`, `svcrt_sched_activate()` |
| Port layer functions | `svcrt_port_<action>` | `svcrt_port_board_init()` |
| Board implementation files | `svcrt_board.c` | port interfaces plus interrupt entry points |
| Struct types | `svcrt_<module>_t` | `svcrt_task_t`, `svcrt_fifo_t`, `svcrt_dev_drv_t` |
| Enum constants | `SVCRT_<module>_<state>` | `SVCRT_TASK_READY`, `SVCRT_TASK_WAIT` |
| Macro constants | `SVCRT_<UPPER_DESCRIPTION>` | `SVCRT_FIFO_MAGIC`, `SVCRT_DEV_HANDLE_FLAG` |
| Configuration macros | `SVCRT_USE_<feature>` | `SVCRT_USE_FPU`, `SVCRT_USE_MPU` |

## Source encoding rules

- Historical sources are **GBK** so that Chinese comments stay readable in the Keil editor; modules
  added recently are **UTF-8**
- Editing a GBK file means inserting content as a **byte-level patch**; **never transcode a whole
  file**, or the Chinese comments turn into mojibake in Keil
- `*.md` documents are all UTF-8
- `tools/gen_api_doc.py` writes UTF-8 copies into the system temporary directory before running
  Doxygen, and never touches the repository

## Automated debug channel (mdkdebug MCP)

Building, flashing and on-board debugging for this project can run through an external MCP service:
[mdk_agent_mcp](https://gitee.com/xw19981010/mdk_agent_mcp.git). It wraps Keil uVision's UVSOCK/TCP
channel into a set of tools, so the loop "build -> flash -> enter debug -> run control -> read state
back" completes without anyone clicking in the Keil UI.

Capabilities available over that channel:

- Project builds: compile / full rebuild / flash, plus the combined "close old window -> build and
  flash -> reopen -> enter debug" flow
- Target inspection: memory and peripheral registers, variables and structs, CPU register banks,
  disassembly, source mapping and call stacks
- Run control: run / halt / step / reset / run to a location, software breakpoints, conditional
  breakpoints and data watchpoints
- Fault analysis: SCB fault registers and the fault site, memory map and memory search, function-level
  and sampling profilers
- Serial interaction: capture logs on the host (with incremental reads) and issue shell commands over
  the same handle
- Symbol management: switch .axf/.map symbol files at runtime, and set a global offset for symbols of
  apps that were relocated at runtime

Part of this project's board verification (kernel tick and scheduling, the install loop, uninstall and
space reclaim, image state after power loss) was carried out over that channel without any manual
clicking in Keil. Deployment, the tool list and usage examples are in that repository's README.

## Scheduler behaviour (measured trace on F427)

The structure of the scheduler (256-bit two-level bitmap ready set, PendSV pulled on demand, priority
preemption plus time slicing) is described in the [scheduler notes](docs/调度器说明.md). This section
reports **what it does on real hardware**: STM32F427VGTx @96 MHz, DAPLink (SWD, two wires, no SWO),
with kernel instrumentation events moved to the host over mdkdebug's SWD trace channel and rendered
there. 18314 raw events over 4.97 s.

> **Read the caveat first**: trace instrumentation itself costs roughly 1100-1500 cycles per PendSV
> round trip. **Only figure 7 of the third chart below (instrumentation off) may be quoted as an
> overhead figure.** The first two charts show who runs when and how regular the switching is - do not
> read their microseconds as a performance statement.

### 1. Overall: 5153 switches in 4.97 s, not one interval out of band

![Overall scheduling timeline and switch intervals](docs/img/sched-overview.png)

- **Top**: intervals of 5153 task switches over a 4.97 s window, all landing on two horizontal bands -
  10 us (a task yielding as soon as it is done) and 2000 us (one tick). Nothing falls outside the
  bands and there is no long tail.
- **Bottom**: zoomed to 21 ms, with the tick as a dashed grey line. `svcrt_shell_task` is woken exactly
  every 2 ms and runs about 10 us before returning to `idle`; at the right edge the LED and the app
  slot arrive together and three tasks complete within a single refresh.

### 2. A single switch: two PendSV entries put the new task to work

![Single switch detail](docs/img/sched-switch-detail.png)

- **Top**: a complete `idle -> svcrt_shell_task -> idle` stretch. The two PendSV entries cost 15.6 us
  and 14.0 us (1498 and 1341 cycles, **instrumentation on**), with the shell yielding in between
  (`SVCRT_TR_EV_WAIT`).
- **Bottom**: zoomed to 134 us. App task `slot5` is scheduled in and runs 72.7 us continuously, during
  which **three SVCall traps enter the kernel** - the privilege-separation path is exercised under
  real load; it finally yields through SVC and the scheduler switches back to the shell.

### 3. Distribution and A/B: the scheduler stays under 0.5% of CPU

![Statistics and optimisation A/B](docs/img/sched-stats.png)

| Item | Value (F427 @96 MHz) |
|---|---|
| Switch interval composition | 50% in the 10 us band, 50% in the 2000 us band, no samples outside them |
| CPU time share | `idle` 99.48%, `svcrt_shell_task` 0.51%, app / driver / LED tasks together < 0.01% |
| PendSV round trip (instrumentation on) | 1682 trips, median 1347 cycles, min 679 / max 1656 |
| PendSV round trip (instrumentation off) | 710.5 cycles with a linear scan of the whole table -> **430.9** with a two-level bitmap plus per-priority lists (-39%) |
| PendSV share of CPU (instrumentation off) | 0.768% -> **0.448%** |

Figure 7 of the third chart is an A/B on the same board, the same app images and the same 3 s window,
measured with DWT window differences: after the ready set moved from "linear scan of the whole table"
to "256-bit two-level bitmap plus per-priority lists", the decision phase went from 627 to 331 cycles
(-47%) and the whole PendSV round trip dropped 39%. **Trace was off for that entire set, so it carries
no instrumentation overhead and may be quoted.** How to reproduce it (capture scripts, shell commands,
measurement conditions) is in the [scheduler notes](docs/调度器说明.md).

## Verification status

Verification stands on two layers of evidence: a **full Keil UV4 rebuild** (kernel plus four example
projects, 0 errors) and **F427 hardware** (DAPLink, SWD two wires, USART1 to host COM3).

Closed on hardware:

| Item | Evidence |
|---|---|
| Kernel tick and scheduling | A heartbeat task keeps printing (`APP_ALIVE`, `cpu=0%`); periods and timeout wake-ups behave |
| The install loop | A `.svcapp` is sent over serial, claimed by the boot scan, started from its slot |
| Driver install loop | A driver package (`--type driver`, 2164 B, `type = 2` inside) goes through the **same `install` window** into the unified pool: `install: ok, slot 1`, `drv list` shows `1 drv RUNNING`, and `pool map` lists the app and the driver side by side in one pool |
| Uninstall and space reclaim | The image is invalidated; the sector is erased only once the physical unit holding the last live slot is free, and `pool free` returns from 776244 B to 786432 B |
| Image state after power loss | After a reset the kernel still distinguishes valid installed images, invalidated ones and raw images |
| Raw-image (development slot) path | An image flashed straight into the pool is claimed, its RAM window `0x20014000` binds correctly, and it runs a ten-section self-test at boot; `app stop` puts it back to `RAW` and `app start` runs it again |
| Message queues | After the variable-length semantics and return values were corrected, `mq.send` / `mq.recv` / `mq.recv_timeout` all pass on hardware |
| POSIX / Windows compatibility layer | APP_DEMO section 9 self-tests (heap, pthread create/join/return value, mutex, empty semaphore, `usleep`, `CreateThread`/`Wait`, `strcpy_s` overflow) all report `OK` |
| On-device configuration region | `tools/svcrt_cfg.py` `write` / `show` / `clear` all pass on hardware: 4x128 chunked download, read-back reporting `source: device config region`, a record with a bad CRC rejected by the device with the real reason, `clear` falling back to defaults. After `mode = fixed` and a reset, `pool` lists the fixed slots from the configuration and the shared partition table's `layout_mode` agrees with `cfg show` |
| F401 port | Both boards build from one source (F401 `Code=46526`) |
| 32-bit tick counter wraparound | Deliberate injection made `svcrt_kernel_tick` start at `0xFFFFC000` and wrap: `t=` jumped from `2147478038 ms` back to a small value, the two `APP_ALIVE` lines around it are exactly 5.000 s apart, `fault` is empty and `sched` is consistent. That run exposed and fixed an inconsistency between the insertion sort on the delay list and the expiry test at the wrap point (see [docs/调度器说明.md](docs/调度器说明.md) section 2.3) |
| Concurrent VFS ports | Two writers plus a read-only third reader hammering one path: the reader must see both **complete** payloads (`vfs.conc_saw_both`); two clean boot self-tests at `pass=128 fail=0`, `sched` consistent, `fault` empty (see [docs/VFS路径命名空间.md](docs/VFS路径命名空间.md)) |
| Fixed-slot overwrite install by the resident installer (`SHELL_ENABLE=0`) | On an `INSTALLER_FIXED_SLOT=0` variant: at boot `fixed slot 0 holds a running image (slot 0): stopping it first`, then `fixed slot 0 cleared, send the file now`; overwriting no longer produces `err -4` and the image autostarts normally; a second frame in the same power cycle is cleanly refused (`one image already installed this boot: reboot before sending another one` plus `drained 1932 B of the rejected frame`) |
| Seamless upgrade while running | Two mini apps (`SEAMLESS_V1` / `SEAMLESS_V2`) hand the control loop over: V1 alone runs at 10.7 ms per iteration, V2 at 5.9 ms after taking over; handover gaps of 27/31/224/407 ms; the iteration counter stays continuous (`it=994` -> `994`); the controlled variable `pv` never resets; `guard starves=0`; the number of `fault` lines is unchanged across the handover. Counter-examples: the old version cannot take control back, a version that times out releases nothing within 5000 ms, and a residual request is withdrawn (see [docs/无缝升级例程.md](docs/无缝升级例程.md)) |

Still not verified on hardware, or not closed:

- **Real Ethernet traffic**: the board has no usable PHY (no PHY driver, no MDIO access, no `eth_`
  interface and no RMII pin assignment under `board/stm32f427/`). The network path runs on a loopback
  netif and has not been verified against a real link (see [docs/lwIP协议栈接入.md](docs/lwIP协议栈接入.md) section 1)
- The fault-recovery **"three consecutive restarts then disable"** policy has no dedicated hardware
  verification
- **A real `install <slot>` placement check in fixed-slot mode** (the slot table takes effect and is
  listed by `pool`, but the step "the image lands exactly at the configured address" has not been run)

The documentation index is [docs/README.md](docs/README.md).

---

## Documentation map

| I want to... | Read |
|---|---|
| Get apps and drivers debugged, installed and crash-handled | [SVCrtOS应用安装与调试指南](docs/SVCrtOS应用安装与调试指南.md) (operations manual, start here) |
| Understand installation strategies, the on-device configuration region, slots and RAM windows | [配置区与安装策略](docs/配置区与安装策略.md) |
| See the index of every document | [docs/README.md](docs/README.md) |
| Understand the scheduler and how its cost was measured | [调度器说明](docs/调度器说明.md) |
| See how memory protection works and what an app violation does | [内存保护与App越权验证](docs/内存保护与App越权验证.md) |
| Move a C program written for Linux or Windows onto this kernel | [POSIX与Windows兼容层说明](docs/POSIX与Windows兼容层说明.md) |
| Write socket / select code | [socket与select兼容层](docs/socket与select兼容层.md) |
| Add a mini app that lives in the file system and runs on demand | [小程序设计](docs/小程序设计.md) |
| Build an upgrade that happens while running | [无缝升级例程](docs/无缝升级例程.md) |
| Port to another chip | [SVCrtOS移植手册](SVCrtOS移植手册.md) |
| Look up API signatures and parameters | [api/SVCrtOS_API参考.md](docs/api/SVCrtOS_API参考.md) |
| Drive the board from an automated workflow (including AI agent workflows) | [skills/svcrtos/SKILL.md](skills/svcrtos/SKILL.md) |

## Related open-source repositories

| Repository | Relationship |
|---|---|
| [svcrtos_new](https://gitee.com/xw19981010/svcrtos_new) | This repository: kernel, board ports and examples |
| [ark-shell](https://gitee.com/xw19981010/ark-shell.git) | Upstream of the kernel console (`kernelsrc/shell/` is that component plus a SVCrtOS platform layer) |
| [ark_vfs](https://gitee.com/xw19981010/ark_vfs) | Zero-dependency VFS, upstream of the file path namespace (`kernelsrc/components/ark_vfs/` is a synced copy) |
| [mdk_agent_mcp](https://gitee.com/xw19981010/mdk_agent_mcp.git) | Automated build / flash / on-board debug channel |

## Licence and version

- Licence: [MIT](LICENSE) (Copyright (c) 2026 春雫)
- Current version: **0.0.2**. Versions are recorded as git tags only; the list is at
  <https://gitee.com/xw19981010/svcrtos_new/tags>
- **0.0.2**: the dynamic-installation main line completed - mini apps (a third kind of user-mode
  application living in the file system), seamless upgrade while running, socket / select on an lwIP
  loopback netif, and file POSIX; MPU violations and unprivileged FPU use verified on hardware; the
  scheduler ready set became a 256-bit two-level bitmap (-39% per PendSV round trip)
- **0.0.1**: first release with installable applications - configurable automatic-placement and
  fixed-slot modes, the graphical host tool and the in-project operations manual
- This file is English; the Chinese original is [readme.md](readme.md)
