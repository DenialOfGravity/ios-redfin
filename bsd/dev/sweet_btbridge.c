/*
 * redfin 0983: a byte pipe between iOS's Bluetooth stack and phone/bt/btd, so bluetoothd/BlueTool can be
 * driven over the WCN3998's HCI without an Apple transport kext. Made only with boot-arg btbridge=1:
 *
 *   /dev/cu.bluetooth, /dev/uart.bluetooth   the H4 serial port bluetoothd opens (two names, one pipe:
 *                                            the daemon tries cu first, then uart). Bytes written here go
 *                                            to /dev/btbridge; reads return bytes btd wrote.
 *   /dev/btbridge                            btd --bridge opens this: it reads the host's H4 bytes,
 *                                            forwards HCI to the controller and writes replies back.
 *   /dev/btwake                              the H4 wake line bluetoothd pokes; a dummy that accepts
 *                                            everything (reads return one 0 byte) so it does not block.
 *
 * Two 16 KiB rings: h2b (host -> bridge) and b2h (bridge -> host). Blocking reads (PCATCH), O_NONBLOCK
 * honoured, select()/poll() supported. The host side is one opener at a time; opening btbridge resets both
 * rings so each btd run starts clean. Nothing is interpreted here: btd owns H4. "SWEET BTBR:" log lines,
 * quiet once bytes flow. Default off: no nodes, nothing touched.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/vnode.h>
#include <sys/proc.h>
#include <sys/uio.h>
#include <sys/select.h>
#include <miscfs/devfs/devfs.h>
#include <kern/locks.h>
#include <pexpert/pexpert.h>

void sweet_btbridge_init(void);

#define MINOR_HOST      0       /* cu.bluetooth / uart.bluetooth */
#define MINOR_BRIDGE    1       /* btbridge */
#define MINOR_WAKE      2       /* btwake */
#define BR_RING         16384u

static LCK_GRP_DECLARE(br_grp, "sweet-btbridge");
static LCK_MTX_DECLARE(br_lock, &br_grp);

struct ring {
	uint8_t buf[BR_RING];
	uint32_t head, tail;            /* head - tail = bytes present */
	struct selinfo sel;
};

static struct {
	bool host_open, bridge_open;
	struct ring h2b, b2h;           /* h2b: host writes / bridge reads; b2h: the reverse */
	uint64_t host_rx, host_tx;
	uint32_t wake_writes, wake_reads, dropped;
} br;

static uint32_t
ring_used(const struct ring *r)
{
	return r->head - r->tail;
}

static uint32_t
ring_space(const struct ring *r)
{
	return BR_RING - (r->head - r->tail);
}

/* copy out of a ring into a uio; lock held; returns bytes moved, -errno on uiomove failure */
static int
ring_to_uio(struct ring *r, struct uio *uio)
{
	int moved = 0;
	while (ring_used(r) && uio_resid(uio) > 0) {
		uint32_t off = r->tail % BR_RING;
		uint32_t run = BR_RING - off;
		if (run > ring_used(r)) {
			run = ring_used(r);
		}
		if (run > (uint32_t)uio_resid(uio)) {
			run = (uint32_t)uio_resid(uio);
		}
		lck_mtx_unlock(&br_lock);
		int e = uiomove((caddr_t)(r->buf + off), (int)run, uio);
		lck_mtx_lock(&br_lock);
		if (e) {
			return -e;
		}
		r->tail += run;
		moved += run;
	}
	return moved;
}

/* copy a uio into a ring; lock held; bytes moved, -errno on failure */
static int
uio_to_ring(struct ring *r, struct uio *uio)
{
	int moved = 0;
	while (ring_space(r) && uio_resid(uio) > 0) {
		uint32_t off = r->head % BR_RING;
		uint32_t run = BR_RING - off;
		if (run > ring_space(r)) {
			run = ring_space(r);
		}
		if (run > (uint32_t)uio_resid(uio)) {
			run = (uint32_t)uio_resid(uio);
		}
		lck_mtx_unlock(&br_lock);
		int e = uiomove((caddr_t)(r->buf + off), (int)run, uio);
		lck_mtx_lock(&br_lock);
		if (e) {
			return -e;
		}
		r->head += run;
		moved += run;
	}
	return moved;
}

static int
br_open(dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	int m = minor(dev);
	lck_mtx_lock(&br_lock);
	if (m == MINOR_BRIDGE) {
		if (br.bridge_open) {
			lck_mtx_unlock(&br_lock);
			return EBUSY;
		}
		br.bridge_open = true;
		br.h2b.head = br.h2b.tail = br.b2h.head = br.b2h.tail = 0;   /* a fresh btd run */
		printf("SWEET BTBR: /dev/btbridge opened by btd; rings cleared\n");
	} else if (m == MINOR_HOST) {
		if (br.host_open) {
			lck_mtx_unlock(&br_lock);
			return EBUSY;
		}
		br.host_open = true;
		printf("SWEET BTBR: host serial port opened%s\n", br.bridge_open ? "" : " (no btd yet)");
	}
	lck_mtx_unlock(&br_lock);
	return 0;
}

static int
br_close(dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	int m = minor(dev);
	lck_mtx_lock(&br_lock);
	if (m == MINOR_BRIDGE) {
		br.bridge_open = false;
	} else if (m == MINOR_HOST) {
		br.host_open = false;
	}
	/* wake the other side so a blocked read returns */
	wakeup(&br.h2b);
	wakeup(&br.b2h);
	selwakeup(&br.h2b.sel);
	selwakeup(&br.b2h.sel);
	lck_mtx_unlock(&br_lock);
	return 0;
}

/* host reads b2h, bridge reads h2b; btwake returns one zero byte */
static int
br_read(dev_t dev, struct uio *uio, int ioflag)
{
	int m = minor(dev);
	if (m == MINOR_WAKE) {
		uint8_t z = 0;
		lck_mtx_lock(&br_lock);
		br.wake_reads++;
		lck_mtx_unlock(&br_lock);
		return uio_resid(uio) > 0 ? uiomove((caddr_t)&z, 1, uio) : 0;
	}
	struct ring *r = m == MINOR_HOST ? &br.b2h : &br.h2b;
	lck_mtx_lock(&br_lock);
	while (ring_used(r) == 0) {
		if (ioflag & IO_NDELAY) {
			lck_mtx_unlock(&br_lock);
			return EWOULDBLOCK;
		}
		int e = msleep(r, &br_lock, PZERO | PCATCH, "btbrrd", NULL);
		if (e) {
			lck_mtx_unlock(&br_lock);
			return e;
		}
	}
	int n = ring_to_uio(r, uio);
	if (n >= 0 && m == MINOR_HOST) {
		br.host_rx += (uint32_t)n;
	}
	/* a read freed space in this ring: a writer may be waiting */
	wakeup(r);
	selwakeup(&r->sel);
	lck_mtx_unlock(&br_lock);
	return n < 0 ? -n : 0;
}

static int
br_write(dev_t dev, struct uio *uio, int ioflag)
{
	int m = minor(dev);
	if (m == MINOR_WAKE) {
		lck_mtx_lock(&br_lock);
		if (br.wake_writes++ < 8) {
			printf("SWEET BTBR: /dev/btwake write %lld bytes\n", (long long)uio_resid(uio));
		}
		lck_mtx_unlock(&br_lock);
		uio_update(uio, uio_resid(uio));        /* accept and discard */
		return 0;
	}
	struct ring *r = m == MINOR_HOST ? &br.h2b : &br.b2h;
	lck_mtx_lock(&br_lock);
	int total = 0;
	while (uio_resid(uio) > 0) {
		while (ring_space(r) == 0) {
			if (ioflag & IO_NDELAY) {
				lck_mtx_unlock(&br_lock);
				return total ? 0 : EWOULDBLOCK;
			}
			int e = msleep(r, &br_lock, PZERO | PCATCH, "btbrwr", NULL);
			if (e) {
				lck_mtx_unlock(&br_lock);
				return total ? 0 : e;
			}
		}
		int n = uio_to_ring(r, uio);
		if (n < 0) {
			lck_mtx_unlock(&br_lock);
			return -n;
		}
		total += n;
		if (m == MINOR_HOST) {
			br.host_tx += (uint32_t)n;
		}
		/* data arrived for the reader of this ring */
		wakeup(r);
		selwakeup(&r->sel);
	}
	lck_mtx_unlock(&br_lock);
	return 0;
}

static int
br_select(dev_t dev, int which, void * wql, proc_t p)
{
	int m = minor(dev);
	if (m == MINOR_WAKE) {
		return 1;       /* always ready */
	}
	/* a side reads one ring and writes the other */
	struct ring *rd = m == MINOR_HOST ? &br.b2h : &br.h2b;
	struct ring *wr = m == MINOR_HOST ? &br.h2b : &br.b2h;
	int ready = 0;
	lck_mtx_lock(&br_lock);
	if (which == FREAD) {
		if (ring_used(rd)) {
			ready = 1;
		} else {
			selrecord(p, &rd->sel, wql);
		}
	} else if (which == FWRITE) {
		if (ring_space(wr)) {
			ready = 1;
		} else {
			selrecord(p, &wr->sel, wql);
		}
	}
	lck_mtx_unlock(&br_lock);
	return ready;
}

static int
br_ioctl(dev_t dev, u_long cmd, __unused caddr_t data, __unused int fflag, __unused proc_t p)
{
	/* a serial port gets plenty of termios/TIOC* ioctls; accept them so the daemon proceeds */
	if (minor(dev) == MINOR_WAKE || ((cmd >> 8) & 0xff) == 't') {
		return 0;
	}
	return 0;
}

static const struct cdevsw br_cdevsw = {
	.d_open       = br_open,
	.d_close      = br_close,
	.d_read       = br_read,
	.d_write      = br_write,
	.d_ioctl      = br_ioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = br_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = 0,
};

void
sweet_btbridge_init(void)
{
	uint32_t on = 0;
	PE_parse_boot_argn("btbridge", &on, sizeof(on));
	if (!on) {
		return;
	}
	int major = cdevsw_add(-1, &br_cdevsw);
	if (major < 0) {
		printf("SWEET BTBR: cdevsw_add failed (%d)\n", major);
		return;
	}
	void *a = devfs_make_node(makedev(major, MINOR_HOST), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "cu.bluetooth");
	void *b = devfs_make_node(makedev(major, MINOR_HOST), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "uart.bluetooth");
	void *c = devfs_make_node(makedev(major, MINOR_BRIDGE), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0600, "btbridge");
	void *d = devfs_make_node(makedev(major, MINOR_WAKE), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "btwake");
	printf("SWEET BTBR: /dev/cu.bluetooth %s, /dev/uart.bluetooth %s, /dev/btbridge %s, /dev/btwake %s (major %d)\n",
	    a ? "ok" : "failed", b ? "ok" : "failed", c ? "ok" : "failed", d ? "ok" : "failed", major);
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
