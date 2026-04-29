/*
 * SGI L1 controller USB driver -- modernized for Linux 5.15+
 *
 * Original copyright:
 *   Copyright 1999 Bob Cutler  (rwc@sgi.com)
 *   Copyright 2000 Steve Hein  (ssh@sgi.com)
 *
 * v5.0 (2026): port to Linux 5.15+
 *
 *   This is a one-shot modernization of the original 2.6 driver. The
 *   user-space ABI in sgil1.h (ioctl numbers, sgil1_cfg_t layout) is
 *   unchanged, so the prebuilt libsnxsc.linux.so / l2 tools work
 *   without recompilation.
 *
 *   Source structure:
 *     - The original sgil1.c version-dispatch shim and sgil1_26.c were
 *       combined into this single source file.
 *     - The 2.4 build path (sgil1_24.c, Makefile.24) was dropped.
 *
 *   API modernization (each item is a real, distinct breakage between
 *   2.6 and 5.15+):
 *     - <asm/semaphore.h> is gone since 2.6.26; switched to
 *       <linux/mutex.h> (DEFINE_MUTEX, mutex_init, mutex_lock,
 *       mutex_unlock).
 *     - file_operations ->ioctl was removed in 2.6.36; switched to
 *       ->unlocked_ioctl (no inode arg, returns long).
 *     - usb_buffer_alloc / usb_buffer_free were renamed in 2.6.34 to
 *       usb_alloc_coherent / usb_free_coherent.
 *     - The pdev->children[] array was removed; use cdev->portnum
 *       directly to walk the USB topology.
 *     - The dbg() / err() / info() macros from <linux/usb.h> were
 *       removed around 3.7; replaced with pr_debug / pr_err / pr_info
 *       wrappers.
 *     - URB completion callback signature lost the pt_regs arg in
 *       2.6.19; using the modern form.
 *     - signal_pending() now requires <linux/sched/signal.h> (since
 *       4.11).
 *     - The poll handler returns __poll_t with EPOLL* names (since
 *       4.16), not unsigned int with POLL*.
 *     - htons() in transfer setup replaced with cpu_to_be16().
 *     - idVendor / idProduct compared via le16_to_cpu() for __le16
 *       correctness.
 *     - Unused <linux/lp.h> include dropped.
 *     - .compat_ioctl = compat_ptr_ioctl added so 32-bit userspace
 *       (the prebuilt libsnxsc.linux.so is i386) works on 64-bit
 *       kernels.
 *     - .llseek is left unset; no_llseek was removed in 6.12.
 *     - SUBDIRS= was removed in 5.3; Makefile uses M= instead.
 *
 *   Reliability fixes that came along with the port:
 *     - sgil1_unlink_all() now uses usb_kill_urb (synchronous) rather
 *       than usb_unlink_urb (async) and sets cleanup state AFTER the
 *       kill returns. The original could have the URB completion
 *       callback fire after cleanup and overwrite SGIL1_CLOSED with
 *       SGIL1_DONE.
 *     - The write-timeout path in sgil1_write() now usb_kill_urb's
 *       the pending write so wr_state reliably reaches IDLE/DONE/
 *       ERROR rather than getting stuck at BUSY.
 *
 *   CONFIG_USB_DYNAMIC_MINORS handling:
 *     The original driver indexed sgil1_state[] by
 *     (interface->minor - SGIL1_MINOR_START), which assumed
 *     usb_register_dev allocated minors from minor_base (208) upward.
 *     Modern distro kernels enable CONFIG_USB_DYNAMIC_MINORS by
 *     default, in which case usb_register_dev allocates from 0 and
 *     the subtraction underflows -- producing an out-of-bounds
 *     memcpy in probe that FORTIFY_SOURCE catches as a BUG().
 *
 *     The fix is structural rather than a band-aid:
 *       - Drop usb_class_driver / usb_register_dev entirely.
 *       - Pre-register a fixed set of miscdevices sgil1_0..sgil1_<N-1>
 *         at module load. udev creates stable /dev/sgil1_<slot> nodes
 *         regardless of how the USB stack hands out minors. (This
 *         also matches the 2.6 initscript's behavior of mknod-ing all
 *         40 nodes at boot.)
 *       - Allocate each per-device sgil1_t on the heap; protect its
 *         lifetime with kref so disconnect-while-open is safe.
 *       - sgil1_state[] is now an array of pointers indexed by slot,
 *         which is exactly what the userspace connection-status
 *         interface (SGIL1_ST_READ, SGIL1_ST_READ_DEV_CFG) has
 *         always assumed.
 *
 *   Latency fix for L1 console:
 *     sgil1_read() resubmits the read URB immediately on a successful
 *     read instead of waiting for userspace to round-trip back through
 *     poll(). The original poll-driven resubmit left a per-cycle
 *     latency gap that's negligible on UHCI/EHCI but very noticeable
 *     on modern xHCI; it manifested as l2-console terminal
 *     sluggishness.
 *
 *   Close-time deadlock fix:
 *     sgil1_release() no longer holds rd_lock+wr_lock across
 *     usb_reset_device(). Modern usb_reset_device can synchronously
 *     invoke our disconnect callback on the same thread, which would
 *     try to take the same locks release() already holds -- a
 *     self-deadlock that wedged the process in TASK_UNINTERRUPTIBLE
 *     (immune to SIGKILL; only a reboot recovers). The fix takes a
 *     usb_get_dev reference, drops our locks, calls
 *     usb_lock_device_for_reset / usb_reset_device /
 *     usb_unlock_device, then re-acquires our locks for the final
 *     cleanup.
 *
 * Distributed under GPL version 2 or later.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>		/* signal_pending() */
#include <linux/signal.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/random.h>
#include <linux/poll.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/pagemap.h>
#include <linux/mm.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/kref.h>
#include <linux/byteorder/generic.h>
#include <linux/usb.h>

#include "sgil1.h"

/*
 * Compatibility shims for the dbg()/err()/info() macros that
 * <linux/usb.h> used to provide and which were removed long ago.
 * Use unique names so they cannot collide with anything else.
 */
#define sgil1_dbg(fmt, ...)	pr_debug("sgil1: " fmt "\n", ##__VA_ARGS__)
#define sgil1_err(fmt, ...)	pr_err  ("sgil1: " fmt "\n", ##__VA_ARGS__)
#define sgil1_info(fmt, ...)	pr_info ("sgil1: " fmt "\n", ##__VA_ARGS__)

#define SGIL1_MAX		40

#define SGIL1_ST_MINOR		249

#define SGIL1_MAX_SIZE		4096
#define SGIL1_WR_TIMEOUT	(3 * HZ)

#define SGIL1_IDLE		0
#define SGIL1_BUSY		1
#define SGIL1_DONE		2
#define SGIL1_ERROR		3
#define SGIL1_CLOSED		4

#define SGIL1_LOCK_RD		1
#define SGIL1_LOCK_WR		2
#define SGIL1_LOCK_ALL		(SGIL1_LOCK_RD|SGIL1_LOCK_WR)

typedef struct {
	struct kref		kref;
	struct usb_device	*dev;
	sgil1_cfg_t		cfg;
	int			active;
	int			slot;

	unsigned int		rd_endpoint;
	unsigned int		rd_pipe;
	char			*rd_buf;
	int			rd_size;
	int			rd_count;
	int			rd_error;
	int			rd_state;
	wait_queue_head_t	rd_wait;
	struct mutex		rd_lock;
	struct urb		*rd_urb;

	unsigned int		wr_endpoint;
	unsigned int		wr_pipe;
	char			*wr_buf;
	int			wr_size;
	int			wr_count;
	int			wr_error;
	int			wr_state;
	wait_queue_head_t	wr_wait;
	struct mutex		wr_lock;
	struct urb		*wr_urb;
} sgil1_t;

/*
 * Per-slot persistent state.
 *
 *   sgil1_misc[] and sgil1_misc_names[] are registered once at module
 *     load and torn down at module unload. They never change while the
 *     module is live -- that's what makes /dev/sgil1_<slot> a stable
 *     pathname and removes the open-vs-disconnect lifetime race from
 *     the miscdev side.
 *
 *   sgil1_state[] holds a pointer to the live sgil1_t for each slot
 *     (or NULL if no device is currently plugged into that slot). It
 *     is protected by state_lock against probe / disconnect / lookup.
 */
static struct miscdevice sgil1_misc[SGIL1_MAX];
static char              sgil1_misc_names[SGIL1_MAX][16];
static sgil1_t          *sgil1_state[SGIL1_MAX];
static DEFINE_MUTEX(state_lock);

static char *sgil1_rev = DRIVER_VERSION;


/*
 * local functions
 */
static int  sgil1_probe(struct usb_interface *interface,
			const struct usb_device_id *id);
static void sgil1_disconnect(struct usb_interface *interface);
static const struct file_operations sgil1_fops;


/****************************************************************************
 * Connection status device driver
 ****************************************************************************/

typedef struct _sgil1_st_state_t {
	__u8			active;
	__u8			new_status;
	wait_queue_head_t	status_wait;

	__u8			status[SGIL1_MAX];

	struct _sgil1_st_state_t *prev;
	struct _sgil1_st_state_t *next;
} sgil1_st_state_t;

static sgil1_st_state_t *sgil1_st_state_list;
static DEFINE_MUTEX(sgil1_st_list_lock);


static int sgil1_st_open(struct inode *inode, struct file *file)
{
	sgil1_st_state_t *st;

	st = kzalloc(sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;

	init_waitqueue_head(&st->status_wait);
	++st->active;

	mutex_lock(&sgil1_st_list_lock);
	st->next = sgil1_st_state_list;
	if (st->next)
		st->next->prev = st;
	sgil1_st_state_list = st;
	mutex_unlock(&sgil1_st_list_lock);

	file->private_data = st;
	return 0;
}

static int sgil1_st_release(struct inode *inode, struct file *file)
{
	sgil1_st_state_t *st = file->private_data;

	if (!st || !st->active)
		return -EINVAL;

	mutex_lock(&sgil1_st_list_lock);
	if (st->prev)
		st->prev->next = st->next;
	if (st->next)
		st->next->prev = st->prev;
	if (sgil1_st_state_list == st)
		sgil1_st_state_list = st->next;
	mutex_unlock(&sgil1_st_list_lock);

	kfree(st);
	return 0;
}

static ssize_t sgil1_st_read(struct file *file,
		char __user *buffer, size_t count, loff_t *ppos)
{
	sgil1_st_state_t *st = file->private_data;
	int dmax;
	int i;

	if (!st || !st->active)
		return -EINVAL;

	dmax = (count < SGIL1_MAX) ? count : SGIL1_MAX;

	mutex_lock(&state_lock);
	for (i = 0; i < dmax; i++)
		st->status[i] = sgil1_state[i] ? 1 : 0;
	mutex_unlock(&state_lock);

	if (copy_to_user(buffer, st->status, dmax))
		return -EFAULT;

	st->new_status = 0;

	return dmax;
}

static __poll_t sgil1_st_poll(struct file *file, poll_table *wait)
{
	sgil1_st_state_t *st = file->private_data;

	if (!st || !st->active)
		return 0;

	poll_wait(file, &st->status_wait, wait);

	if (st->new_status)
		return EPOLLIN | EPOLLRDNORM;

	return 0;
}

static long sgil1_st_ioctl(struct file *file,
		unsigned int cmd, unsigned long arg)
{
	sgil1_st_state_t *st = file->private_data;
	void __user *uarg = (void __user *)arg;
	sgil1_cfg_t cfg;
	sgil1_cfg_t snap;
	sgil1_t *devst;
	long rvalue;

	if (!st || !st->active)
		return -EINVAL;

	switch (cmd) {

	case SGIL1_ST_READ_REV:
		/*
		 * Original behavior: copies sizeof(sgil1_rev) bytes (the
		 * size of a pointer) from a static char* to userspace.
		 * Preserved as-is to maintain ABI compatibility with the
		 * prebuilt libsnxsc.linux.so user-space library.
		 */
		rvalue = copy_to_user(uarg, sgil1_rev, sizeof(sgil1_rev));
		break;

	case SGIL1_ST_READ_DEV_CFG:
		if (copy_from_user(&cfg, uarg, sizeof(cfg)))
			return -EFAULT;

		if (cfg.dev >= SGIL1_MAX)
			return -EINVAL;

		mutex_lock(&state_lock);
		devst = sgil1_state[cfg.dev];
		if (!devst) {
			mutex_unlock(&state_lock);
			return -EINVAL;
		}
		snap = devst->cfg;	/* take a snapshot under the lock */
		mutex_unlock(&state_lock);

		rvalue = copy_to_user(uarg, &snap, sizeof(snap));
		break;

	default:
		return -ENOIOCTLCMD;
	}

	if (rvalue)
		msleep(500);

	return rvalue;
}

static void sgil1_st_update(void)
{
	sgil1_st_state_t *st;

	mutex_lock(&sgil1_st_list_lock);
	for (st = sgil1_st_state_list; st; st = st->next) {
		st->new_status = 1;
		if (st->active)
			wake_up_interruptible(&st->status_wait);
	}
	mutex_unlock(&sgil1_st_list_lock);
}


/****************************************************************************
 * Actual (data transfer) device driver
 ****************************************************************************/


static void sgil1_lock(sgil1_t *sgil1, int which, const char *who)
{
	if (which & SGIL1_LOCK_RD)
		mutex_lock(&sgil1->rd_lock);
	if (which & SGIL1_LOCK_WR)
		mutex_lock(&sgil1->wr_lock);
}

static void sgil1_unlock(sgil1_t *sgil1, int which, const char *who)
{
	if (which & SGIL1_LOCK_RD)
		mutex_unlock(&sgil1->rd_lock);
	if (which & SGIL1_LOCK_WR)
		mutex_unlock(&sgil1->wr_lock);
}


static void sgil1_unlink_all(sgil1_t *sgil1)
{
	int state;

	/*
	 * usb_kill_urb is synchronous: by the time it returns the
	 * completion callback has run, so we set the cleanup state
	 * AFTER the kill (the callback would otherwise overwrite it).
	 */
	state = sgil1->rd_state;
	if (state == SGIL1_BUSY && sgil1->rd_urb)
		usb_kill_urb(sgil1->rd_urb);
	sgil1->rd_state = SGIL1_CLOSED;
	sgil1->rd_error = -ENODEV;
	wake_up_interruptible(&sgil1->rd_wait);

	state = sgil1->wr_state;
	if (state == SGIL1_BUSY && sgil1->wr_urb)
		usb_kill_urb(sgil1->wr_urb);
	sgil1->wr_state = SGIL1_CLOSED;
	sgil1->wr_error = -ENODEV;
	wake_up_interruptible(&sgil1->wr_wait);
}

/*
 * Free the URBs and DMA-coherent buffers attached to a sgil1_t.
 *
 * Must be called while sgil1->dev is still a valid usb_device pointer
 * (i.e. before usb_disconnect() reclaims it). Idempotent: each
 * resource pointer is NULLed after release so a subsequent call from
 * sgil1_release_struct() does nothing.
 */
static void sgil1_delete(sgil1_t *sgil1)
{
	if (sgil1->rd_buf) {
		usb_free_coherent(sgil1->dev, sgil1->rd_size,
				  sgil1->rd_buf, sgil1->rd_urb->transfer_dma);
		sgil1->rd_buf = NULL;
	}

	if (sgil1->wr_buf) {
		usb_free_coherent(sgil1->dev, sgil1->wr_size,
				  sgil1->wr_buf, sgil1->wr_urb->transfer_dma);
		sgil1->wr_buf = NULL;
	}

	if (sgil1->rd_urb) {
		usb_free_urb(sgil1->rd_urb);
		sgil1->rd_urb = NULL;
	}

	if (sgil1->wr_urb) {
		usb_free_urb(sgil1->wr_urb);
		sgil1->wr_urb = NULL;
	}
}

/* Final destructor: called when the kref count reaches zero. */
static void sgil1_release_struct(struct kref *kref)
{
	sgil1_t *sgil1 = container_of(kref, sgil1_t, kref);

	/* Idempotent; if disconnect already ran sgil1_delete this is a no-op */
	sgil1_delete(sgil1);
	kfree(sgil1);
}

static void sgil1_read_irq(struct urb *urb)
{
	sgil1_t *sgil1 = urb->context;

	sgil1->rd_count = urb->actual_length;
	sgil1->rd_error = urb->status;
	sgil1->rd_state = SGIL1_DONE;

	if (sgil1->rd_error)
		sgil1_dbg("sgil1_read_irq: read error: %d slot: %d",
			  sgil1->rd_error, sgil1->slot);

	wake_up_interruptible(&sgil1->rd_wait);
}

static void sgil1_write_irq(struct urb *urb)
{
	sgil1_t *sgil1 = urb->context;

	sgil1->wr_error = urb->status;

	if (urb->actual_length != sgil1->wr_count) {
		sgil1_dbg("short write, slot: %d, act: %d, exp: %d",
			  sgil1->slot, urb->actual_length, sgil1->wr_count);

		if (!sgil1->wr_error)
			sgil1->wr_error = -EIO;
	}

	sgil1->wr_state = sgil1->wr_error ? SGIL1_ERROR : SGIL1_DONE;
	wake_up_interruptible(&sgil1->wr_wait);
}

static int sgil1_reset_pipe(struct usb_device *dev, unsigned int pipe, int stall)
{
	int endp = usb_pipeendpoint(pipe);
	int rvalue;

	if (usb_pipein(pipe))
		endp |= USB_DIR_IN;

	rvalue = usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
				 USB_REQ_SET_FEATURE, USB_RECIP_ENDPOINT, 0,
				 endp, NULL, 0, HZ * 3);

	if (stall)
		return rvalue;

	msleep(20);

	return rvalue ? rvalue : usb_clear_halt(dev, pipe);
}

/*
 * sgil1_open: file_operations->open for /dev/sgil1_<slot>.
 *
 * The file's private_data is set to a pointer to the sgil1_misc[slot]
 * miscdevice by the misc framework before this is called. We map back
 * to the slot number (and from there the live sgil1_t, if any) by
 * pointer arithmetic on the static sgil1_misc[] array. The miscdevices
 * themselves live for the entire module lifetime, so the pointer is
 * always valid.
 */
static int sgil1_open(struct inode *inode, struct file *file)
{
	struct miscdevice *miscdev = file->private_data;
	int slot;
	sgil1_t *sgil1;
	int rval;

	slot = miscdev - sgil1_misc;
	if (slot < 0 || slot >= SGIL1_MAX) {
		/* shouldn't happen -- private_data should always point
		 * inside sgil1_misc[] for these device files */
		return -ENODEV;
	}

	mutex_lock(&state_lock);
	sgil1 = sgil1_state[slot];
	if (!sgil1) {
		mutex_unlock(&state_lock);
		return -ENODEV;
	}
	if (sgil1->active) {
		mutex_unlock(&state_lock);
		return -EBUSY;
	}
	sgil1->active = 1;
	kref_get(&sgil1->kref);		/* hold a ref for the open fd */
	mutex_unlock(&state_lock);

	/*
	 * The kref keeps sgil1 alive across a concurrent disconnect.
	 * Disconnect will null sgil1->dev under sgil1->rd_lock+wr_lock,
	 * so re-check it here before declaring the open successful.
	 */
	sgil1_lock(sgil1, SGIL1_LOCK_ALL, "open");

	if (!sgil1->dev) {
		rval = -ENODEV;
		goto err_unlock;
	}

	file->private_data = sgil1;
	sgil1->rd_state = SGIL1_IDLE;
	sgil1->rd_error = 0;
	sgil1->wr_state = SGIL1_IDLE;
	sgil1->wr_error = 0;
	init_waitqueue_head(&sgil1->rd_wait);
	init_waitqueue_head(&sgil1->wr_wait);

	sgil1_unlock(sgil1, SGIL1_LOCK_ALL, "open");

	sgil1_dbg("sgil1_open, slot: %d", slot);
	return 0;

err_unlock:
	sgil1->active = 0;
	sgil1_unlock(sgil1, SGIL1_LOCK_ALL, "open dead");
	kref_put(&sgil1->kref, sgil1_release_struct);
	return rval;
}

static int sgil1_release(struct inode *inode, struct file *file)
{
	sgil1_t *sgil1 = file->private_data;
	struct usb_device *dev_to_reset = NULL;

	if (!sgil1 || !sgil1->active)
		return -EINVAL;

	file->private_data = NULL;

	/*
	 * Stage 1: snapshot the usb_device pointer under our lock so it
	 * survives a concurrent disconnect. We must NOT hold rd_lock or
	 * wr_lock across usb_reset_device -- the reset can trigger a
	 * synchronous disconnect callback IN THIS SAME THREAD, and our
	 * disconnect callback acquires both locks. Holding them across
	 * the reset would deadlock the thread against itself, leaving the
	 * caller stuck in TASK_UNINTERRUPTIBLE (the original 2.6 driver
	 * had this same code; modern usb_reset_device is more aggressive
	 * about synchronously re-binding, which is what exposes the bug).
	 */
	sgil1_lock(sgil1, SGIL1_LOCK_ALL, "release stage 1");
	if (sgil1->dev)
		dev_to_reset = usb_get_dev(sgil1->dev);
	sgil1_unlock(sgil1, SGIL1_LOCK_ALL, "release stage 1");

	if (dev_to_reset) {
		sgil1_dbg("sgil1_release: reset device, slot: %d",
			  sgil1->slot);
		if (usb_lock_device_for_reset(dev_to_reset, NULL) >= 0) {
			int rvalue = usb_reset_device(dev_to_reset);

			usb_unlock_device(dev_to_reset);
			if (rvalue) {
				sgil1_err("SGI L1 device reset failed on close, slot: %d, error: %d",
					  sgil1->slot, rvalue);
			}
		}
		usb_put_dev(dev_to_reset);
	}

	/*
	 * Stage 2: finalize our side of the open. If the reset above
	 * triggered a disconnect, sgil1->dev is now NULL and the URBs
	 * and DMA buffers have already been freed; sgil1_unlink_all is
	 * a no-op in that case.
	 */
	sgil1_lock(sgil1, SGIL1_LOCK_ALL, "release stage 2");
	sgil1_unlink_all(sgil1);
	sgil1->active = 0;
	sgil1_unlock(sgil1, SGIL1_LOCK_ALL, "release stage 2");

	sgil1_dbg("sgil1_close, slot: %d", sgil1->slot);

	/* drop the open-side ref; if disconnect already dropped probe's
	 * ref this is the last reference and triggers the kfree */
	kref_put(&sgil1->kref, sgil1_release_struct);
	return 0;
}

static ssize_t sgil1_write(struct file *file,
		const char __user *buffer, size_t count, loff_t *ppos)
{
	sgil1_t *sgil1 = file->private_data;
	int rval = 0;
	int error;

	if (!sgil1 || !sgil1->active || (count > sgil1->wr_size) || (count < 2))
		return -EINVAL;

	sgil1_lock(sgil1, SGIL1_LOCK_WR, "write");

	if (!sgil1->dev) {
		rval = -ENODEV;
		goto exit;
	}

	if (sgil1->wr_state == SGIL1_BUSY) {
		DECLARE_WAITQUEUE(wait, current);
		int timeout = SGIL1_WR_TIMEOUT;

		set_current_state(TASK_INTERRUPTIBLE);
		add_wait_queue(&sgil1->wr_wait, &wait);

		while (timeout && (sgil1->wr_state == SGIL1_BUSY)) {
			if (signal_pending(current)) {
				remove_wait_queue(&sgil1->wr_wait, &wait);
				set_current_state(TASK_RUNNING);
				rval = -ERESTARTSYS;
				goto exit;
			}

			timeout = schedule_timeout(timeout);
			set_current_state(TASK_INTERRUPTIBLE);
		}

		remove_wait_queue(&sgil1->wr_wait, &wait);
		set_current_state(TASK_RUNNING);
	}

	switch (sgil1->wr_state) {
	case SGIL1_IDLE:
	case SGIL1_DONE:
		break;

	case SGIL1_BUSY:
		sgil1_dbg("write timeout, slot: %d", sgil1->slot);
		usb_kill_urb(sgil1->wr_urb);
		rval = -ETIMEDOUT;
		goto exit;

	case SGIL1_ERROR:
		sgil1_dbg("last write error: %d slot: %d",
			  sgil1->wr_error, sgil1->slot);
		rval = sgil1->wr_error;
		goto exit;

	default:
		rval = -ENODEV;
		goto exit;
	}

	if (!sgil1->dev) {
		rval = -ENODEV;
		goto exit;
	}

	if (copy_from_user(sgil1->wr_buf, buffer, count)) {
		rval = -EFAULT;
		goto exit;
	}

	*((__be16 *)(sgil1->wr_buf)) = cpu_to_be16((unsigned short) count);
	usb_fill_bulk_urb(sgil1->wr_urb, sgil1->dev, sgil1->wr_pipe,
			  sgil1->wr_buf, count, sgil1_write_irq, sgil1);
	sgil1->wr_count = count;
	sgil1->wr_error = 0;
	sgil1->wr_state = SGIL1_BUSY;
	error = usb_submit_urb(sgil1->wr_urb, GFP_KERNEL);

	if (error) {
		sgil1->wr_state = SGIL1_IDLE;
		sgil1_dbg("write error: %d slot: %d", error, sgil1->slot);
		rval = error;
		/* fall through to exit */
	}

exit:
	sgil1_unlock(sgil1, SGIL1_LOCK_WR, "write done");

	return rval ? rval : count;
}

static ssize_t sgil1_read(struct file *file,
		char __user *buffer, size_t count, loff_t *ppos)
{
	sgil1_t *sgil1 = file->private_data;
	int read_count;
	int rval = 0;

	if (!sgil1 || !sgil1->active)
		return -EINVAL;

	sgil1_lock(sgil1, SGIL1_LOCK_RD, "read");

	if (!sgil1->dev) {
		rval = -ENODEV;
		goto exit;
	}

	if (sgil1->rd_state != SGIL1_DONE) {
		sgil1_dbg("read not done, slot: %d state: %d",
			  sgil1->slot, sgil1->rd_state);
		rval = 0;
		goto exit;
	}

	if (sgil1->rd_error) {
		int rderr = sgil1->rd_error;

		sgil1_dbg("read error: %d slot: %d",
			  sgil1->rd_error, sgil1->slot);
		sgil1->rd_state = SGIL1_IDLE;
		sgil1->rd_error = 0;
		rval = rderr;
		goto exit;
	}

	read_count = sgil1->rd_count;
	if (read_count > 0) {
		if (sgil1->rd_count > count) {
			rval = -EINVAL;
			goto exit;
		}
		rval = read_count;

		if (copy_to_user(buffer, sgil1->rd_buf, read_count)) {
			rval = -EFAULT;
			goto exit;
		}
	}

	/*
	 * Successful read: resubmit the read URB immediately so the L1
	 * has somewhere to deliver the next chunk of data without waiting
	 * for userspace to round-trip back through poll(). The original
	 * 2.6 driver only resubmitted from sgil1_poll(), which left a
	 * per-cycle latency gap; that gap was negligible on UHCI/EHCI but
	 * is noticeable on modern xHCI and shows up as L1-console
	 * terminal sluggishness.
	 */
	usb_fill_bulk_urb(sgil1->rd_urb, sgil1->dev, sgil1->rd_pipe,
			  sgil1->rd_buf, sgil1->rd_size,
			  sgil1_read_irq, sgil1);
	sgil1->rd_state = SGIL1_BUSY;
	sgil1->rd_urb->status = 0;
	if (usb_submit_urb(sgil1->rd_urb, GFP_KERNEL)) {
		/* Couldn't resubmit (rare); fall back to poll-driven submit. */
		sgil1->rd_state = SGIL1_IDLE;
	}

exit:
	sgil1_unlock(sgil1, SGIL1_LOCK_RD, "read");

	return rval;
}


static __poll_t sgil1_poll(struct file *file, poll_table *wait)
{
	sgil1_t *sgil1 = file->private_data;
	int error;

	if (!sgil1 || !sgil1->active)
		return EPOLLHUP;

	sgil1_lock(sgil1, SGIL1_LOCK_RD, "poll");

	if (!sgil1->dev) {
		sgil1_unlock(sgil1, SGIL1_LOCK_RD, "poll nodev");
		return EPOLLHUP;
	}

	if (sgil1->rd_state == SGIL1_IDLE) {
		usb_fill_bulk_urb(sgil1->rd_urb, sgil1->dev, sgil1->rd_pipe,
				  sgil1->rd_buf, sgil1->rd_size,
				  sgil1_read_irq, sgil1);
		sgil1->rd_state = SGIL1_BUSY;

		sgil1->rd_urb->status = 0;
		error = usb_submit_urb(sgil1->rd_urb, GFP_KERNEL);
		if (error) {
			sgil1->rd_state = SGIL1_IDLE;
			sgil1->rd_error = error;
			sgil1_dbg("sgil1_poll: usb_submit_urb (read) error: %d slot: %d",
				  sgil1->rd_error, sgil1->slot);
			sgil1_unlock(sgil1, SGIL1_LOCK_RD, "poll submit fail");
			return EPOLLIN | EPOLLRDNORM;
		}
	}

	sgil1_unlock(sgil1, SGIL1_LOCK_RD, "poll submit OK");

	poll_wait(file, &sgil1->rd_wait, wait);

	if (!sgil1->dev || !sgil1->active)
		return EPOLLHUP;

	if (sgil1->rd_state == SGIL1_DONE)
		return EPOLLIN | EPOLLRDNORM;

	return 0;
}

static long sgil1_ioctl(struct file *file,
		unsigned int cmd, unsigned long arg)
{
	sgil1_t *sgil1 = file->private_data;
	void __user *uarg = (void __user *)arg;
	long rvalue;

	if (!sgil1 || !sgil1->active)
		return -EINVAL;

	sgil1_lock(sgil1, SGIL1_LOCK_ALL, "ioctl");

	if (!sgil1->dev) {
		sgil1_unlock(sgil1, SGIL1_LOCK_ALL, "ioctl nodev");
		return -ENODEV;
	}

	switch (cmd) {

	case SGIL1_RESET_PIPES:
	case SGIL1_RESET_WRITE:
		rvalue = sgil1_reset_pipe(sgil1->dev, sgil1->wr_pipe, 0);
		sgil1_dbg("SGIL1_RESET_WRITE %d.%d, %ld",
			  sgil1->dev->bus->busnum, sgil1->dev->devnum, rvalue);
		if (cmd == SGIL1_RESET_WRITE)
			break;
		/* SGIL1_RESET_PIPES falls through.... */
		fallthrough;

	case SGIL1_RESET_READ:
		rvalue = sgil1_reset_pipe(sgil1->dev, sgil1->rd_pipe, 0);
		sgil1_dbg("SGIL1_RESET_READ %d.%d, %ld",
			  sgil1->dev->bus->busnum, sgil1->dev->devnum, rvalue);
		break;

	case SGIL1_RESET_DEVICE:
		rvalue = usb_reset_device(sgil1->dev);
		if (rvalue != 0)
			sgil1->rd_state = SGIL1_ERROR;

		sgil1_dbg("SGIL1_RESET_DEVICE %d.%d, %ld",
			  sgil1->dev->bus->busnum, sgil1->dev->devnum, rvalue);
		break;

	case SGIL1_READ_CFG:
		rvalue = copy_to_user(uarg, &sgil1->cfg, sizeof(sgil1_cfg_t));
		break;

	default:
		sgil1_unlock(sgil1, SGIL1_LOCK_ALL, "ioctl bad");
		return -ENOIOCTLCMD;
	}

	sgil1_unlock(sgil1, SGIL1_LOCK_ALL, "ioctl done");

	if (rvalue)
		msleep(500);

	return rvalue;
}


/****************************************************************************
 * Support (probe/disconnect/init) routines
 ****************************************************************************/

/*
 * Per-device file_operations -- shared by every /dev/sgil1_<slot> node.
 */
static const struct file_operations sgil1_fops = {
	.owner		= THIS_MODULE,
	.read		= sgil1_read,
	.write		= sgil1_write,
	.poll		= sgil1_poll,
	.unlocked_ioctl	= sgil1_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.open		= sgil1_open,
	.release	= sgil1_release,
	/*
	 * Note: do not set .llseek = no_llseek -- the no_llseek symbol was
	 * removed in 6.12. Leaving llseek unset means any seek attempt is
	 * a no-op (the file position is irrelevant for this device).
	 */
};

static const struct usb_device_id sgil1_table[] = {
	{ USB_DEVICE(SGIL1_VENDOR_ID, SGIL1_PRODUCT_ID) },
	{ }			/* Terminating entry */
};

MODULE_DEVICE_TABLE(usb, sgil1_table);

static struct usb_driver sgil1_driver = {
	.name		= "sgil1",
	.probe		= sgil1_probe,
	.disconnect	= sgil1_disconnect,
	.id_table	= sgil1_table,
};

/*
 * Driver for the SGI L1 connection-status pseudo-device
 */

static const struct file_operations sgil1_st_fops = {
	.owner		= THIS_MODULE,
	.read		= sgil1_st_read,
	.poll		= sgil1_st_poll,
	.unlocked_ioctl	= sgil1_st_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.open		= sgil1_st_open,
	.release	= sgil1_st_release,
};

static struct miscdevice sgil1_st_dev = {
	.minor	= SGIL1_ST_MINOR,
	.name	= "sgil1_cs",
	.fops	= &sgil1_st_fops,
	.mode	= 0444,
};


static int sgil1_probe(struct usb_interface *interface,
		       const struct usb_device_id *id)
{
	struct usb_device *dev = interface_to_usbdev(interface);
	struct usb_host_interface *iface_desc;
	struct usb_endpoint_descriptor *endpoint;
	sgil1_t *sgil1;
	sgil1_cfg_t *cfg;
	struct usb_device *cdev;
	struct usb_device *pdev;
	int slot;
	int rval = -ENOMEM;

	/*
	 * Determine if this device is an SGI L1 controller
	 */
	if ((le16_to_cpu(dev->descriptor.idVendor)  != SGIL1_VENDOR_ID) ||
	    (le16_to_cpu(dev->descriptor.idProduct) != SGIL1_PRODUCT_ID) ||
	    (dev->descriptor.bNumConfigurations != 1) ||
	    (dev->actconfig->desc.bNumInterfaces != 1))
		return -ENODEV;

	iface_desc = &interface->altsetting[0];

	if (iface_desc->desc.bNumEndpoints != 2)
		return -ENODEV;

	endpoint = &iface_desc->endpoint[0].desc;
	if (!usb_endpoint_is_bulk_out(endpoint))
		return -ENODEV;

	endpoint = &iface_desc->endpoint[1].desc;
	if (!usb_endpoint_is_bulk_in(endpoint))
		return -ENODEV;

	/* Allocate the per-device state */
	sgil1 = kzalloc(sizeof(*sgil1), GFP_KERNEL);
	if (!sgil1)
		return -ENOMEM;

	kref_init(&sgil1->kref);	/* refcount = 1 (probe's ref) */
	mutex_init(&sgil1->rd_lock);
	mutex_init(&sgil1->wr_lock);
	sgil1->slot = -1;

	/*
	 * Save the USB topology configuration for this device.
	 *
	 * Modern kernels store each device's parent-port number directly
	 * in struct usb_device::portnum (1-based; 0 means "root", which
	 * matches the original code's "not found" semantics).
	 */
	cfg = &sgil1->cfg;
	cfg->bus = dev->bus->busnum;
	cfg->dev = dev->devnum;
	cdev = dev;
	pdev = dev->parent;

	while (pdev && (cfg->level < SGIL1_MAX_LEVEL)) {
		int i;

		for (i = cfg->level; i > 0; i--)
			cfg->path[i] = cfg->path[i - 1];

		++cfg->level;
		cfg->path[0] = cdev->portnum;	/* 1-based, 0 == root */
		cdev = pdev;
		pdev = pdev->parent;
	}

	/*
	 * Allocate URBs and DMA-coherent transfer buffers.
	 */
	sgil1->rd_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!sgil1->rd_urb) {
		rval = -ENOMEM;
		goto err_free;
	}

	sgil1->wr_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!sgil1->wr_urb) {
		rval = -ENOMEM;
		goto err_free;
	}

	sgil1->rd_urb->transfer_flags = URB_NO_TRANSFER_DMA_MAP;
	sgil1->wr_urb->transfer_flags = URB_NO_TRANSFER_DMA_MAP;

	sgil1->dev = dev;

	sgil1->rd_size = (SGIL1_MAX_SIZE > PAGE_SIZE) ?
				PAGE_SIZE : SGIL1_MAX_SIZE;
	sgil1->wr_size = (SGIL1_MAX_SIZE > PAGE_SIZE) ?
				PAGE_SIZE : SGIL1_MAX_SIZE;

	sgil1->rd_endpoint = iface_desc->endpoint[1].desc.bEndpointAddress &
			     USB_ENDPOINT_NUMBER_MASK;
	sgil1->wr_endpoint = iface_desc->endpoint[0].desc.bEndpointAddress &
			     USB_ENDPOINT_NUMBER_MASK;

	sgil1->rd_pipe = usb_rcvbulkpipe(sgil1->dev, sgil1->rd_endpoint);
	sgil1->wr_pipe = usb_sndbulkpipe(sgil1->dev, sgil1->wr_endpoint);

	sgil1->rd_buf = usb_alloc_coherent(sgil1->dev, sgil1->rd_size,
				GFP_KERNEL, &sgil1->rd_urb->transfer_dma);
	if (!sgil1->rd_buf) {
		rval = -ENOMEM;
		goto err_free;
	}

	sgil1->wr_buf = usb_alloc_coherent(sgil1->dev, sgil1->wr_size,
				GFP_KERNEL, &sgil1->wr_urb->transfer_dma);
	if (!sgil1->wr_buf) {
		rval = -ENOMEM;
		goto err_free;
	}

	/*
	 * Find a free slot in the global table and claim it.
	 *
	 * The slot index drives the device-file naming (/dev/sgil1_<slot>),
	 * the connection-status bitmap, and the SGIL1_ST_READ_DEV_CFG ioctl.
	 * It is unrelated to whatever USB minor the kernel may have assigned.
	 */
	mutex_lock(&state_lock);
	for (slot = 0; slot < SGIL1_MAX; slot++) {
		if (!sgil1_state[slot]) {
			sgil1_state[slot] = sgil1;
			sgil1->slot = slot;
			break;
		}
	}
	mutex_unlock(&state_lock);

	if (slot >= SGIL1_MAX) {
		sgil1_err("no free device slot (max %d)", SGIL1_MAX);
		rval = -EBUSY;
		goto err_free;
	}

	usb_set_intfdata(interface, sgil1);

	sgil1_info("SGI L1 connected, slot: %d device: %d.%d",
		   slot, dev->bus->busnum, dev->devnum);

	sgil1_st_update();

	return 0;

err_free:
	/* Drop probe's initial kref. The destructor frees URBs/buffers
	 * (sgil1->dev is still valid here) and the struct itself. */
	kref_put(&sgil1->kref, sgil1_release_struct);
	return rval;
}

static void sgil1_disconnect(struct usb_interface *interface)
{
	struct usb_device *dev = interface_to_usbdev(interface);
	sgil1_t *sgil1;

	sgil1 = usb_get_intfdata(interface);
	usb_set_intfdata(interface, NULL);

	if (!sgil1)
		return;

	/* Mark the slot free immediately so new opens see -ENODEV
	 * and the connection-status bitmap reflects the disconnect. */
	mutex_lock(&state_lock);
	if (sgil1->slot >= 0 && sgil1->slot < SGIL1_MAX &&
	    sgil1_state[sgil1->slot] == sgil1)
		sgil1_state[sgil1->slot] = NULL;
	mutex_unlock(&state_lock);

	sgil1_info("SGI L1 disconnected, slot: %d device: %d.%d",
		   sgil1->slot, dev->bus->busnum, dev->devnum);

	/*
	 * Tear down USB resources WHILE sgil1->dev is still a live
	 * usb_device pointer. After this block sgil1->dev is NULL.
	 */
	sgil1_lock(sgil1, SGIL1_LOCK_ALL, "disconnect");
	sgil1_unlink_all(sgil1);
	sgil1_delete(sgil1);
	sgil1->dev = NULL;
	sgil1_unlock(sgil1, SGIL1_LOCK_ALL, "disconnect");

	sgil1_st_update();

	/* Drop probe's initial kref. If a release() is concurrently
	 * dropping the open-side ref, whichever runs second triggers
	 * the final kfree. */
	kref_put(&sgil1->kref, sgil1_release_struct);
}


/* Helper: roll back miscdevice registrations made in sgil1_init. */
static void sgil1_unregister_miscs(int upto)
{
	int i;

	for (i = 0; i < upto; i++)
		misc_deregister(&sgil1_misc[i]);
}

static int __init sgil1_init(void)
{
	int i, rval;

	/*
	 * Register a miscdevice per slot. These exist for the entire
	 * lifetime of the module, so /dev/sgil1_0../dev/sgil1_<MAX-1>
	 * are always present (matching the historical 2.6 initscript
	 * behavior of mknod'ing all 40 nodes at boot).
	 *
	 * Reading/writing one of these when the slot has no device
	 * attached returns -ENODEV.
	 */
	for (i = 0; i < SGIL1_MAX; i++) {
		snprintf(sgil1_misc_names[i], sizeof(sgil1_misc_names[i]),
			 "sgil1_%d", i);
		sgil1_misc[i].minor = MISC_DYNAMIC_MINOR;
		sgil1_misc[i].name  = sgil1_misc_names[i];
		sgil1_misc[i].fops  = &sgil1_fops;
		sgil1_misc[i].mode  = 0666;
		rval = misc_register(&sgil1_misc[i]);
		if (rval) {
			sgil1_err("misc_register %s failed: %d",
				  sgil1_misc_names[i], rval);
			sgil1_unregister_miscs(i);
			return rval;
		}
	}

	/* Register the connection-status pseudo-device */
	rval = misc_register(&sgil1_st_dev);
	if (rval) {
		sgil1_err("misc_register sgil1_cs failed: %d", rval);
		sgil1_unregister_miscs(SGIL1_MAX);
		return rval;
	}

	/* Register the USB driver (probe/disconnect callbacks) */
	rval = usb_register(&sgil1_driver);
	if (rval) {
		sgil1_err("usb_register failed: %d", rval);
		misc_deregister(&sgil1_st_dev);
		sgil1_unregister_miscs(SGIL1_MAX);
		return rval;
	}

	sgil1_info(DRIVER_VERSION ": " DRIVER_DESC);
	return 0;
}

static void __exit sgil1_exit(void)
{
	/* Order matters: unregister the USB driver first so that any
	 * still-attached devices are disconnected (and their slot
	 * pointers cleared) before we tear down the miscdevices. */
	usb_deregister(&sgil1_driver);
	misc_deregister(&sgil1_st_dev);
	sgil1_unregister_miscs(SGIL1_MAX);
}

module_init(sgil1_init);
module_exit(sgil1_exit);

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
MODULE_ALIAS_MISCDEV(SGIL1_ST_MINOR);
