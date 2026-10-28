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
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>
#include <kern/locks.h>
#include <pexpert/pexpert.h>

void sweet_btbridge_init(void);
static void br_ctl_drain(void);
static void br_ctl_init(void);

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
		if (m == MINOR_BRIDGE) {
			/* 1015: btd's bytes for a kernel-control client (bluetoothd) go to its socket */
			lck_mtx_unlock(&br_lock);
			br_ctl_drain();
			lck_mtx_lock(&br_lock);
		}
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
	if (!(on & 2)) {
		br_ctl_init();
	}
}

/*
 * ---- redfin 1015: the kernel control com.apple.uart.bluetooth --------------------------------------------
 *
 * iOS 16's bluetoothd and BlueTool do not read a tty: they open a PF_SYSTEM kernel control named
 * com.apple.uart[.sk].bluetooth (AppleOnboardSerial's, absent here), set termios through
 * getsockopt/setsockopt(SYSPROTO_CONTROL, TIOCGETA 0x40487413 / TIOCSETA 0x80487414) and then stream H4
 * bytes (ios-asteroids docs/BT_IOS_TRANSPORT.md, from disassembly; their Poco F1 WCN3990 works this way).
 * This control is the same pipe as /dev/cu.bluetooth: what the client sends goes to h2b (btd reads it on
 * /dev/btbridge), what btd writes to b2h is queued on the client's socket. Not privileged (bluetoothd runs as
 * mobile), one client at a time (a new one replaces the old). bluetoothd's default is the Skywalk name
 * (.sk.), which needs a channel we do not have: its launchd job gets
 * BT_HCI_TRANSPORT=H4BC:POSIX:com.apple.uart.bluetooth,3000000 (tools-claude/bt-launchd.py).
 * termios is kept and given back, nothing reaches the UART (btd owns its speed). btbridge=3: no control.
 */
static kern_ctl_ref br_ctl_ref;
static uint32_t br_ctl_unit;            /* 0: no client */
static bool br_ctl_draining;
static uint8_t br_termios[72];          /* struct termios as userland passes it (iOS: 72 bytes) */
static uint32_t br_ctl_opts_said, br_ctl_sent, br_ctl_queued;
static uint32_t br_ctl_hex_said, br_ctl_err_said;  /* diag: client bytes in hex, enqueue errors */

static errno_t
br_ctl_connect(__unused kern_ctl_ref ref, struct sockaddr_ctl *sac, void **unitinfo)
{
	char who[32] = "?";
	proc_selfname(who, sizeof(who));
	lck_mtx_lock(&br_lock);
	uint32_t old = br_ctl_unit;
	br_ctl_unit = sac->sc_unit;
	br.b2h.head = br.b2h.tail = 0;          /* a new client starts clean (as a fresh open would) */
	lck_mtx_unlock(&br_lock);
	*unitinfo = NULL;
	printf("SWEET BTBR: kernel control connected by %s (unit %u%s)%s\n", who, sac->sc_unit, old ? ", replaces the last" : "",
	    br.bridge_open ? "" : ", no btd yet");
	return 0;
}

static errno_t
br_ctl_disconnect(__unused kern_ctl_ref ref, u_int32_t unit, __unused void *unitinfo)
{
	lck_mtx_lock(&br_lock);
	if (br_ctl_unit == unit) {
		br_ctl_unit = 0;
	}
	lck_mtx_unlock(&br_lock);
	printf("SWEET BTBR: kernel control unit %u gone (%u bytes in, %u out; unread by btd %u, unsent to client %u, btd %s)\n",
	    unit, br_ctl_sent, br_ctl_queued, br.h2b.head - br.h2b.tail, br.b2h.head - br.b2h.tail,
	    br.bridge_open ? "open" : "not open");
	return 0;
}

/* client -> btd: into h2b, waiting up to 2 s for room */
static errno_t
br_ctl_send(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, mbuf_t m, __unused int flags)
{
	errno_t error = 0;
	lck_mtx_lock(&br_lock);
	for (mbuf_t n = m; n != NULL && error == 0; n = mbuf_next(n)) {
		const uint8_t *p = mbuf_data(n);
		size_t len = mbuf_len(n);
		if (len > 0 && br_ctl_hex_said < 24) {      /* diag: what the client sends */
			br_ctl_hex_said++;
			printf("SWEET BTBR: client sends %zu bytes (btd %s, h2b unread %u):", len, br.bridge_open ? "open" : "not open",
			    br.h2b.head - br.h2b.tail);
			for (size_t i = 0; i < len && i < 32; i++) {
				printf(" %02x", p[i]);
			}
			printf("\n");
		}
		while (len > 0) {
			if (ring_space(&br.h2b) == 0) {
				struct timespec ts = { 2, 0 };
				if (msleep(&br.h2b, &br_lock, PZERO, "btbrctl", &ts) == EWOULDBLOCK) {
					br.dropped += (uint32_t)len;
					error = ENOBUFS;
					break;
				}
				continue;
			}
			uint32_t at = br.h2b.head % BR_RING, run = BR_RING - at;
			if (run > ring_space(&br.h2b)) {
				run = ring_space(&br.h2b);
			}
			if (run > len) {
				run = (uint32_t)len;
			}
			memcpy(br.h2b.buf + at, p, run);
			br.h2b.head += run;
			p += run;
			len -= run;
			br_ctl_sent += run;
			wakeup(&br.h2b);
			selwakeup(&br.h2b.sel);
		}
	}
	lck_mtx_unlock(&br_lock);
	mbuf_freem(m);
	return error;
}

/* btd -> client: what fits in the socket now; the rest when the client reads (br_ctl_rcvd) */
static void
br_ctl_drain(void)
{
	uint8_t chunk[1024];
	lck_mtx_lock(&br_lock);
	if (br_ctl_draining || br_ctl_unit == 0) {
		lck_mtx_unlock(&br_lock);
		return;
	}
	br_ctl_draining = true;
	while (br_ctl_unit != 0 && ring_used(&br.b2h) != 0) {
		uint32_t unit = br_ctl_unit, at = br.b2h.tail % BR_RING, run = BR_RING - at;
		if (run > ring_used(&br.b2h)) {
			run = ring_used(&br.b2h);
		}
		if (run > sizeof(chunk)) {
			run = sizeof(chunk);
		}
		memcpy(chunk, br.b2h.buf + at, run);
		lck_mtx_unlock(&br_lock);
		errno_t e = ctl_enqueuedata(br_ctl_ref, unit, chunk, run, 0);
		lck_mtx_lock(&br_lock);
		if (e != 0) {
			if (br_ctl_err_said++ < 8) {
				printf("SWEET BTBR: ctl_enqueuedata unit %u, %u bytes -> %d\n", unit, run, e);
			}
			break;                  /* the socket is full: br_ctl_rcvd drains again */
		}
		if (br_ctl_queued == 0) {
			printf("SWEET BTBR: first %u bytes from btd to the client\n", run);
		}
		br.b2h.tail += run;
		br_ctl_queued += run;
		wakeup(&br.b2h);                /* room for btd's writes */
		selwakeup(&br.b2h.sel);
	}
	br_ctl_draining = false;
	lck_mtx_unlock(&br_lock);
}

static void
br_ctl_rcvd(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, __unused int flags)
{
	br_ctl_drain();
}

static errno_t
br_ctl_setopt(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, int opt, void *data, size_t len)
{
	if ((uint32_t)opt == 0x80487414u && data != NULL && len <= sizeof(br_termios)) {      /* TIOCSETA */
		memcpy(br_termios, data, len);
	}
	if (br_ctl_opts_said++ < 24) {
		/* struct termios (arm64): c_iflag, c_oflag, c_cflag, c_lflag (8 bytes each), c_cc[20], c_ispeed, c_ospeed */
		uint64_t cflag = 0, ospeed = 0;
		if (len >= 72 && data != NULL) {
			memcpy(&cflag, (uint8_t *)data + 16, 8);
			memcpy(&ospeed, (uint8_t *)data + 64, 8);
		}
		printf("SWEET BTBR: control setopt %#x (%zu bytes)%s c_cflag %#llx speed %llu\n", (uint32_t)opt, len,
		    (uint32_t)opt == 0x80487414u ? " termios:" : "", cflag, ospeed);
	}
	return 0;
}

static errno_t
br_ctl_getopt(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, int opt, void *data, size_t *len)
{
	if (br_ctl_opts_said++ < 24) {
		printf("SWEET BTBR: control getopt %#x (%zu bytes)\n", (uint32_t)opt, len ? *len : 0);
	}
	if ((uint32_t)opt == 0x40487413u) {                    /* TIOCGETA */
		if (data != NULL && len != NULL) {
			size_t n = *len < sizeof(br_termios) ? *len : sizeof(br_termios);
			memcpy(data, br_termios, n);
			*len = n;
		}
		return 0;
	}
	if (data != NULL && len != NULL) {
		bzero(data, *len);
	}
	return 0;
}

static void
br_ctl_init(void)
{
	struct kern_ctl_reg reg;
	bzero(&reg, sizeof(reg));
	strlcpy(reg.ctl_name, "com.apple.uart.bluetooth", sizeof(reg.ctl_name));
	reg.ctl_flags = CTL_FLAG_REG_SOCK_STREAM | CTL_FLAG_REG_EXTENDED;
	reg.ctl_sendsize = 64 * 1024;
	reg.ctl_recvsize = 64 * 1024;
	reg.ctl_connect = br_ctl_connect;
	reg.ctl_disconnect = br_ctl_disconnect;
	reg.ctl_send = br_ctl_send;
	reg.ctl_setopt = br_ctl_setopt;
	reg.ctl_getopt = br_ctl_getopt;
	reg.ctl_rcvd = br_ctl_rcvd;
	/* termios defaults: 3 Mbaud raw (8N1, CREAD, CRTS_IFLOW|CCTS_OFLOW = 0x38b00) until the client sets its own */
	uint64_t cflag = 0x38b00, speed = 3000000;
	memcpy(br_termios + 16, &cflag, 8);
	memcpy(br_termios + 56, &speed, 8);
	memcpy(br_termios + 64, &speed, 8);
	errno_t e = ctl_register(&reg, &br_ctl_ref);
	printf("SWEET BTBR: kernel control com.apple.uart.bluetooth -> %d (id %u)\n", e, reg.ctl_id);
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
