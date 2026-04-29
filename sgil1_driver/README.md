# SGI L1 USB driver — modernized port (v5.0)

This is a port of the `sgil1` driver to modern Linux.  
AI (Claude Opus) was used to do this port.

Read the repo's README for installation scenarios.

Driver is in `./src`

## How to install:

./setup-driver.sh

Build deps:

build-essential, linux-headers


Or to manually build:  
`make && sudo make install`

---



### AI written overview below:

Port of the original `sgil1` Linux 2.6 USB driver — used by SGI L3
controllers to talk to SGI 3000-series L1 system controllers — to
**Linux 5.15+**.

The user/kernel ABI exposed by `sgil1.h` (the `sgil1_cfg_t` struct and
the `SGIL1_*` ioctl numbers) is unchanged, so the prebuilt
`libsnxsc.linux.so` and the `l2*` userspace tools that ship in the
original L3 software bundle keep working without recompilation.

---

## Confirmed kernel versions

This is the part to be careful about, so to be precise:

- **Linux 6.12 / Debian 13.** Actually built, loaded, and exercised
  end-to-end (probe, `l2` interactive console, quit, replug). This is
  the only kernel I can vouch for from direct testing.
- **Linux 5.15 LTS through 6.11.** *Should* compile and load with no
  source changes — every kernel API the driver uses is present and
  stable across this entire range — but I haven't physically tested
  any of them. If a build breaks, it'll be a clear missing-symbol
  error from the linker, not silent misbehavior; the API surface area
  this driver touches (URB submit/kill, miscdevice, kref, mutex,
  unlocked_ioctl) is some of the most stable in the kernel.
- **Linux 6.13 and later.** Unknown. The kernel is a moving target.
  See "Forward compatibility" below for the specific things most
  likely to bite eventually.

The lower bound of 5.15 is a deliberate cutoff: `compat_ptr_ioctl`
landed in 5.5, `__poll_t` in 4.16, the `<linux/sched/signal.h>` split
in 4.11, and so on. 5.15 has all of them. If you genuinely need to
run on 5.4 or earlier, this would need version-conditional code; tell
me which kernel and we'll sort it out.

---

## What changed from the original 2.6 driver

The 2.4 build path has been deleted (`sgil1_24.c`, `Makefile.24`, the
`KVER==2.4` branch, the version-dispatching `sgil1.c` shim, etc.).
The two remaining 2.6 source files were combined into a single modern
source file.

### API modernization

Each item below is a real, distinct breakage between Linux 2.6 and
modern kernels:

| # | Old (2.6) | New (5.15+) | Reason |
|---|-----------|-------------|--------|
| 1 | `<asm/semaphore.h>` | `<linux/mutex.h>` | header gone since 2.6.26 |
| 2 | `DECLARE_MUTEX`, `init_MUTEX`, `down`/`up`, `struct semaphore` | `DEFINE_MUTEX`, `mutex_init`, `mutex_lock`/`mutex_unlock`, `struct mutex` | semaphore API for binary locks removed |
| 3 | `.ioctl` (with `inode` arg, returning `int`) | `.unlocked_ioctl` (no `inode` arg, returns `long`) | removed in 2.6.36 (Sep 2010) |
| 4 | `usb_buffer_alloc` / `usb_buffer_free` | `usb_alloc_coherent` / `usb_free_coherent` | renamed in 2.6.34 |
| 5 | `pdev->children[port] == cdev` walk | `cdev->portnum` | the `children[]` array was removed |
| 6 | `dbg(...)`, `err(...)`, `info(...)` from `<linux/usb.h>` | `pr_debug`/`pr_err`/`pr_info` (wrapped) | macros removed ~3.7 |
| 7 | `void cb(struct urb *, struct pt_regs *)` | `void cb(struct urb *)` | URB callback signature changed in 2.6.19 |
| 8 | implicit `signal_pending` | `#include <linux/sched/signal.h>` | required since 4.11 |
| 9 | `make ... SUBDIRS=$(PWD)` | `make ... M=$(PWD)` | `SUBDIRS=` removed in 5.3 |
| 10 | `htons(...)` | `cpu_to_be16(...)` | endian-correct kernel idiom |
| 11 | `unsigned int` poll return, `POLL*` | `__poll_t`, `EPOLL*` | typed since 4.16 |
| 12 | `<linux/lp.h>` included | removed | was never used |
| 13 | bare `idVendor`/`idProduct` compares | wrapped in `le16_to_cpu()` | `__le16` correctness |
| 14 | (no `.compat_ioctl`) | `.compat_ioctl = compat_ptr_ioctl` | 32-bit userspace on 64-bit kernels |
| 15 | `usb_unlink_urb` (async) | `usb_kill_urb` (sync) | fixes a state-clobber race in cleanup paths |
| 16 | `.llseek = no_llseek` | omitted | `no_llseek` symbol removed in 6.12 |

### Reliability fixes that came along with the port

- `sgil1_unlink_all()` sets the cleanup state **after**
  `usb_kill_urb()` returns. The original used `usb_unlink_urb()`
  (async) and set state first, but the URB completion callback could
  still run afterward and overwrite the cleanup state with
  `SGIL1_DONE`. With the synchronous kill the callback has already
  run by the time we set `SGIL1_CLOSED`.
- The write-timeout path in `sgil1_write()` now `usb_kill_urb`s the
  pending write so `wr_state` reliably reaches `IDLE`/`DONE`/`ERROR`
  rather than getting stuck at `BUSY`.
- The `set_current_state(TASK_INTERRUPTIBLE)` in the manual wait loop
  is now re-asserted after each `schedule_timeout()` (the original
  could re-check the loop condition while in `TASK_RUNNING`, which is
  a long-standing rough-edge of the open-coded wait loop).

### CONFIG_USB_DYNAMIC_MINORS handling

The original driver indexed `sgil1_state[]` by
`(interface->minor - SGIL1_MINOR_START)`. That worked in 2.6 when
`usb_register_dev` always allocated minors from `minor_base` (208)
upward. On modern distro kernels with `CONFIG_USB_DYNAMIC_MINORS=y`
(the upstream default), `usb_register_dev` ignores `minor_base` and
allocates from 0, so the subtraction underflows and the `memcpy` in
probe writes through a wildly out-of-bounds pointer.
`FORTIFY_SOURCE` catches that as a "buffer of size 0" `BUG()`.

The fix is structural rather than a band-aid:

- Drop the `usb_class_driver` / `usb_register_dev` path entirely.
- Pre-register a fixed set of miscdevices `sgil1_0`..`sgil1_39` at
  module load. udev creates stable `/dev/sgil1_<slot>` nodes
  regardless of how the USB stack hands out minors. (This also
  matches the historical 2.6 initscript behavior of `mknod`-ing all
  40 nodes at boot.)
- Allocate each per-device `sgil1_t` on the heap; protect its
  lifetime with `kref` so disconnect-while-open is safe.
- `sgil1_state[]` is now an array of pointers, indexed by slot
  0..39 — which is exactly what the userspace connection-status
  interface (`SGIL1_ST_READ` and `SGIL1_ST_READ_DEV_CFG`) has always
  assumed.

### Latency fix for the L1 console

`sgil1_read()` resubmits the read URB immediately on a successful
read instead of leaving the IN endpoint idle until the next
`sgil1_poll()` call. The original poll-driven resubmit added a
per-cycle latency gap that's negligible on UHCI/EHCI but very
noticeable on modern xHCI; it manifested as L1-console terminal
sluggishness when running `l2`.

### Close-time deadlock fix

`sgil1_release()` does not hold `rd_lock`+`wr_lock` across
`usb_reset_device()`. Modern `usb_reset_device` can synchronously
invoke our disconnect callback on the same thread, which would try
to take the same locks `release()` already holds — a self-deadlock
that wedged the calling process in `TASK_UNINTERRUPTIBLE` (immune
to `SIGKILL`; only a reboot recovers).

The fix takes a `usb_get_dev` reference, drops our locks, calls
`usb_lock_device_for_reset` + `usb_reset_device` +
`usb_unlock_device`, then re-acquires our locks for the final
cleanup.

---

## Forward compatibility (5.15 → 6.12 → beyond)

For a single specific kernel within the **5.15 LTS through 6.12**
range, you should not need any per-version changes — the source as
written compiles and runs the same on any of them. The one breakage
in that window was `no_llseek` (removed in 6.12, commit
`868941b14441`); the port works around it by simply not setting
`.llseek`.

For **6.13+ and beyond**, no claim either way. The areas most likely
to bite eventually:

- The URB submission and callback API. Last big rework was 2.6.19;
  it could change again.
- `unlocked_ioctl` / `compat_ioctl` if someone modernizes the ioctl
  interface generally.
- Continuing tightening of `FORTIFY_SOURCE`. The dynamic-minors
  trap was caused by such tightening; another iteration could
  expose something I haven't thought of.
- Whatever happens to the USB minor-number scheme, miscdevice minor
  allocation, or the misc class.

I deliberately did *not* sprinkle preemptive `LINUX_VERSION_CODE`
guards into the source — they make the code unreadable and add up to
nothing on any single kernel. The right time to add a version check
is when a real kernel actually breaks the build, and at that point
the `#if` should be wrapped tightly around the specific change with a
comment explaining what broke.

---

## Build and install

Tested verbatim on Debian 13 with kernel 6.12.

```sh
# prerequisites
sudo apt install build-essential linux-headers-$(uname -r)

# build
make

# install module + udev rules; loads automatically on plug-in,
# or you can modprobe right away
sudo make install
sudo modprobe sgil1
```

After plugging in an SGI L1 controller you'll see in dmesg:

```
sgil1: SGI L1 connected, slot: 0 device: 2.5
```

and find `/dev/sgil1_0` (the connected device), `/dev/sgil1_1`
through `/dev/sgil1_39` (empty slots), and `/dev/sgil1_cs` (the
connection-status pseudo-device) in `/dev/`. The connected device's
slot number is what the connection-status bitmap reports and what
the userspace tools open.

To rebuild after editing the source:

```sh
sudo rmmod sgil1
make clean && make
sudo make install        # or just: sudo cp sgil1.ko ...
sudo modprobe sgil1
```

If `rmmod` reports "module is in use", unplug the L1 first and close
any process that has `/dev/sgil1_cs` or `/dev/sgil1_<N>` open. After
a kernel `BUG()` only a reboot is reliable — `D`-state processes
ignore `SIGKILL`.

---

## Files in this directory

- **`sgil1.c`** — the modernized driver (replaces the original
  `sgil1.c`, `sgil1_24.c`, and `sgil1_26.c`).
- **`sgil1.h`** — unchanged user/kernel ABI; the `DRIVER_VERSION`
  string is the only thing that changes between revisions of this
  port.
- **`Makefile`** — out-of-tree module build using `M=`.
- **`99-sgil1.rules`** — udev rules for `/dev/sgil1_*` permissions
  (0666 for the per-device nodes, 0444 for `sgil1_cs`, matching the
  historical `sgil1.initscript`).
- **`README.md`** — this file.

The 2.4 makefile (`Makefile.24`), the version-dispatch `sgil1.c`,
the RPM makefile (`Makefile.rpm`), the RedHat config
(`linux/rhconfig.h`), and the initscript (`sgil1.initscript`) from
the original tarball were deliberately not carried over. On a
systemd/udev system none of them is needed; the udev rules file
takes the initscript's role.

---

## Things deliberately *not* changed

- **`SGIL1_ST_MINOR = 249`** for the connection-status misc device.
  Hard-coded value the original initscript baked into `mknod`. Modern
  udev creates the node by class name regardless, so the minor
  doesn't matter to userspace, but pinning it preserves historical
  behavior. If you ever see `misc_register failed, error -16` (EBUSY)
  at module load, switch this to `MISC_DYNAMIC_MINOR`.
- **The `SGIL1_ST_READ_REV` ioctl behavior.** It copies
  `sizeof(sgil1_rev)` bytes (the size of a `char *`, so 8 on 64-bit)
  from a string pointer to userspace — a latent bug in the original.
  But the prebuilt `libsnxsc.linux.so` consumes it as-is, so
  "fixing" it could break userspace. Left untouched, with a comment.
- **The reset-on-close behavior** (`usb_reset_device` from
  `sgil1_release`). The 2.6 driver did this and the userspace tools
  expect the L1 to be in a fresh state when a new `l2` invocation
  opens the device. The deadlock fix described above kept the reset
  intact. If you find post-quit reset slowness annoying you can drop
  the `usb_reset_device` call from `sgil1_release` — the trade-off
  is that one user's leftover endpoint state would be visible to the
  next.
