/*
 * flame: the WCN3990 radio for the kernel's Wi-Fi station. bsd/dev/flame_sta.c (ios-topaz 0900's topaz_sta.c)
 * puts en0 and wifid's Apple80211 requests (bsd/dev/redwood_*.c) on this file's flame_wlan_* C API, the one
 * TopazModem.cpp offers there. The radio itself runs in userland (phone/wifi: wland), which opens /dev/wlan
 * once it is up; sys/flame_wlan.h is the protocol, one message per read() or write().
 *
 * The station's requests (scan, join, leave) are queued for wland and each waits for its answer; frames en0
 * sends are queued too (dropped when the queue is full). What wland writes back wakes the waiter, or goes to
 * en0 (received frames) and the link hook. When wland closes the device the link is down and waiters fail.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/proc.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <miscfs/devfs/devfs.h>
#include <kern/locks.h>
#include <pexpert/pexpert.h>
#include <libkern/libkern.h>
#include <sys/flame_wlan.h>

void flame_wlan_init(void);
void flame_sta_init(void);              /* bsd/dev/flame_sta.c */
void redwood_net_init(void);            /* bsd/dev/redwood_net.c: its boot-args */

/* as bsd/dev/flame_sta.c declares it (topaz_wlan_bss) */
struct flame_wlan_bss {
	uint8_t bssid[6];
	uint8_t ssid[32], ssid_length;
	uint16_t frequency;
	int16_t rssi;
	uint16_t beacon_interval, capability;
	uint32_t age_ms;
	uint16_t elements_length;
	uint8_t elements[768];
};

void (*flame_wlan_ready_hook)(void);
void (*flame_wlan_rx_hook)(const uint8_t *frame, size_t length);
void (*flame_wlan_link_hook)(bool up);

static LCK_GRP_DECLARE(fw_grp, "flame-wlan");
static LCK_MTX_DECLARE(fw_lock, &fw_grp);

#define FW_QLEN         64
#define FW_PENDING      INT32_MIN
static struct {
	uint16_t type, len;
	uint8_t data[FW_MSG_MAX - sizeof(struct fw_hdr)];
} fw_q[FW_QLEN];
static unsigned fw_q_head, fw_q_count;
static uint64_t fw_tx_dropped;

static bool fw_opened, fw_ready, fw_link_up;
static uint8_t fw_addr[6];
static struct fw_link fw_link;          /* the network of the last FW_LINK up */
#define FW_BSS_MAX      64
static struct flame_wlan_bss fw_bss[FW_BSS_MAX];
static unsigned fw_nbss;
static int32_t fw_scan_status = FW_PENDING, fw_join_status = FW_PENDING, fw_left_status = FW_PENDING;

/* with fw_lock */
static bool
fw_queue_locked(uint16_t type, const void *data, size_t len)
{
	if (fw_q_count >= FW_QLEN || len > sizeof(fw_q[0].data)) {
		return false;
	}
	unsigned i = (fw_q_head + fw_q_count) % FW_QLEN;
	fw_q[i].type = type;
	fw_q[i].len = (uint16_t)len;
	if (len) {
		memcpy(fw_q[i].data, data, len);
	}
	fw_q_count++;
	wakeup(&fw_q_count);
	return true;
}

/* A request of the station: queued for wland, then its answer (0 or an errno) within `secs`. */
static int
fw_request(uint16_t type, const void *data, size_t len, int32_t *status, int secs)
{
	lck_mtx_lock(&fw_lock);
	if (!fw_opened) {
		lck_mtx_unlock(&fw_lock);
		return ENXIO;
	}
	*status = FW_PENDING;
	if (!fw_queue_locked(type, data, len)) {
		lck_mtx_unlock(&fw_lock);
		return ENOBUFS;
	}
	struct timespec ts = { secs, 0 };
	while (*status == FW_PENDING && fw_opened) {
		if (msleep(status, &fw_lock, PZERO, "flamewlan", &ts) == EWOULDBLOCK) {
			break;
		}
	}
	int rc = *status != FW_PENDING ? *status : fw_opened ? ETIMEDOUT : ENXIO;
	lck_mtx_unlock(&fw_lock);
	return rc;
}

/* ---- the C API of bsd/dev/flame_sta.c ---- */

bool flame_wlan_ready(void);
int flame_wlan_scan(void);
bool flame_wlan_heard(unsigned index, struct flame_wlan_bss *bss);
void flame_wlan_address(uint8_t address[6]);
int flame_wlan_join(const uint8_t *ssid, unsigned ssid_len, const uint8_t *bssid, const uint8_t *pass, unsigned pass_len);
int flame_wlan_join_pmk(const uint8_t *ssid, unsigned ssid_len, const uint8_t *bssid, const uint8_t pmk[32]);
int flame_wlan_leave(void);
bool flame_wlan_joined(void);
bool flame_wlan_transmit(const uint8_t *frame, size_t length);

bool
flame_wlan_ready(void)
{
	return fw_ready;
}

int
flame_wlan_scan(void)
{
	return fw_request(FW_SCAN, NULL, 0, &fw_scan_status, 20);
}

bool
flame_wlan_heard(unsigned index, struct flame_wlan_bss *bss)
{
	lck_mtx_lock(&fw_lock);
	bool have = index < fw_nbss;
	if (have) {
		memcpy(bss, &fw_bss[index], sizeof(*bss));
	}
	lck_mtx_unlock(&fw_lock);
	return have;
}

void
flame_wlan_address(uint8_t address[6])
{
	memcpy(address, fw_addr, 6);
}

static int
fw_join(const uint8_t *ssid, unsigned ssid_len, const uint8_t *bssid, uint8_t kind, const uint8_t *key, unsigned key_len)
{
	struct fw_join j;
	bzero(&j, sizeof(j));
	if (ssid_len == 0 || ssid_len > sizeof(j.ssid) || key_len > sizeof(j.key)) {
		return EINVAL;
	}
	memcpy(j.ssid, ssid, ssid_len);
	j.ssid_len = (uint8_t)ssid_len;
	if (bssid != NULL) {
		memcpy(j.bssid, bssid, 6);
	} else {
		j.any_bssid = 1;
	}
	j.key_kind = kind;
	j.key_len = (uint8_t)key_len;
	if (key_len) {
		memcpy(j.key, key, key_len);
	}
	return fw_request(FW_JOIN, &j, sizeof(j), &fw_join_status, 40);
}

int
flame_wlan_join(const uint8_t *ssid, unsigned ssid_len, const uint8_t *bssid, const uint8_t *pass, unsigned pass_len)
{
	return fw_join(ssid, ssid_len, bssid, pass != NULL && pass_len ? FW_KEY_PASSPHRASE : FW_KEY_NONE, pass,
	           pass != NULL ? pass_len : 0);
}

int
flame_wlan_join_pmk(const uint8_t *ssid, unsigned ssid_len, const uint8_t *bssid, const uint8_t pmk[32])
{
	return fw_join(ssid, ssid_len, bssid, FW_KEY_PMK, pmk, 32);
}

int
flame_wlan_leave(void)
{
	return fw_request(FW_LEAVE, NULL, 0, &fw_left_status, 10);
}

bool
flame_wlan_joined(void)
{
	return fw_link_up;
}

/* the network joined, as the last scan heard it (or by name alone): for a join wland made by itself */
bool flame_wlan_link_bss(struct flame_wlan_bss *bss);
bool
flame_wlan_link_bss(struct flame_wlan_bss *bss)
{
	lck_mtx_lock(&fw_lock);
	bool have = fw_link_up && fw_link.ssid_len > 0;
	if (have) {
		bzero(bss, sizeof(*bss));
		unsigned i;
		for (i = 0; i < fw_nbss && memcmp(fw_bss[i].bssid, fw_link.bssid, 6) != 0; i++) {
		}
		if (i < fw_nbss) {
			memcpy(bss, &fw_bss[i], sizeof(*bss));
		} else {
			memcpy(bss->bssid, fw_link.bssid, 6);
			memcpy(bss->ssid, fw_link.ssid, sizeof(bss->ssid));
			bss->ssid_length = fw_link.ssid_len > 32 ? 32 : fw_link.ssid_len;
		}
	}
	lck_mtx_unlock(&fw_lock);
	return have;
}

bool
flame_wlan_transmit(const uint8_t *frame, size_t length)
{
	lck_mtx_lock(&fw_lock);
	bool ok = fw_opened && fw_link_up && fw_queue_locked(FW_TX, frame, length);
	if (!ok) {
		fw_tx_dropped++;
	}
	lck_mtx_unlock(&fw_lock);
	return ok;
}

/* ---- /dev/wlan ---- */

static int
fw_open(__unused dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	lck_mtx_lock(&fw_lock);
	int rc = fw_opened ? EBUSY : 0;
	if (rc == 0) {
		fw_opened = true;
		fw_q_head = fw_q_count = 0;
	}
	lck_mtx_unlock(&fw_lock);
	return rc;
}

static int
fw_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	lck_mtx_lock(&fw_lock);
	bool was_up = fw_link_up;
	fw_opened = false;
	fw_link_up = false;
	fw_q_head = fw_q_count = 0;
	wakeup(&fw_scan_status);
	wakeup(&fw_join_status);
	wakeup(&fw_left_status);
	wakeup(&fw_q_count);
	lck_mtx_unlock(&fw_lock);
	printf("flame wlan: the radio's side closed /dev/wlan (%llu frames dropped)\n", fw_tx_dropped);
	if (was_up && flame_wlan_link_hook != NULL) {
		flame_wlan_link_hook(false);
	}
	return 0;
}

static int
fw_read(__unused dev_t dev, struct uio *uio, int ioflag)
{
	static uint8_t buf[FW_MSG_MAX];         /* one reader: wland */
	lck_mtx_lock(&fw_lock);
	while (fw_q_count == 0) {
		if (ioflag & IO_NDELAY) {
			lck_mtx_unlock(&fw_lock);
			return EWOULDBLOCK;
		}
		int e = msleep(&fw_q_count, &fw_lock, PZERO | PCATCH, "fwread", NULL);
		if (e) {
			lck_mtx_unlock(&fw_lock);
			return e;
		}
	}
	struct fw_hdr h = { fw_q[fw_q_head].type, fw_q[fw_q_head].len };
	if (uio_resid(uio) < (user_ssize_t)(sizeof(h) + h.len)) {
		lck_mtx_unlock(&fw_lock);
		return EMSGSIZE;
	}
	memcpy(buf, &h, sizeof(h));
	memcpy(buf + sizeof(h), fw_q[fw_q_head].data, h.len);
	fw_q_head = (fw_q_head + 1) % FW_QLEN;
	fw_q_count--;
	lck_mtx_unlock(&fw_lock);
	return uiomove((caddr_t)buf, (int)(sizeof(h) + h.len), uio);
}

static void
fw_wake_status(int32_t *status, const uint8_t *p, size_t len)
{
	int32_t v = EIO;
	if (len >= sizeof(v)) {
		memcpy(&v, p, sizeof(v));
	}
	lck_mtx_lock(&fw_lock);
	*status = v;
	wakeup(status);
	lck_mtx_unlock(&fw_lock);
}

static int
fw_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	static uint8_t buf[FW_MSG_MAX];         /* one writer: wland */
	user_ssize_t n = uio_resid(uio);
	struct fw_hdr h;
	if (n < (user_ssize_t)sizeof(h) || n > (user_ssize_t)sizeof(buf)) {
		return EINVAL;
	}
	int e = uiomove((caddr_t)buf, (int)n, uio);
	if (e) {
		return e;
	}
	memcpy(&h, buf, sizeof(h));
	const uint8_t *p = buf + sizeof(h);
	if (h.len != n - (user_ssize_t)sizeof(h)) {
		return EINVAL;
	}
	switch (h.type) {
	case FW_HELLO: {
		if (h.len < sizeof(struct fw_hello)) {
			return EINVAL;
		}
		bool first = !fw_ready;
		memcpy(fw_addr, ((const struct fw_hello *)p)->address, 6);
		fw_ready = true;
		printf("flame wlan: the radio is up (%02x:%02x:%02x:%02x:%02x:%02x)%s\n", fw_addr[0], fw_addr[1], fw_addr[2],
		    fw_addr[3], fw_addr[4], fw_addr[5], first ? ": en0 for wifid" : "");
		if (first && flame_wlan_ready_hook != NULL) {
			flame_wlan_ready_hook();
		}
		break;
	}
	case FW_BSS_BEGIN:
		lck_mtx_lock(&fw_lock);
		fw_nbss = 0;
		lck_mtx_unlock(&fw_lock);
		break;
	case FW_BSS: {
		const struct fw_bss *b = (const struct fw_bss *)p;
		if (h.len < sizeof(*b)) {
			return EINVAL;
		}
		lck_mtx_lock(&fw_lock);
		if (fw_nbss < FW_BSS_MAX) {
			struct flame_wlan_bss *t = &fw_bss[fw_nbss++];
			memcpy(t->bssid, b->bssid, 6);
			memcpy(t->ssid, b->ssid, sizeof(t->ssid));
			t->ssid_length = b->ssid_len > 32 ? 32 : b->ssid_len;
			t->frequency = b->frequency;
			t->rssi = b->rssi;
			t->beacon_interval = b->beacon_interval;
			t->capability = b->capability;
			t->age_ms = b->age_ms;
			t->elements_length = b->elements_len > sizeof(t->elements) ? sizeof(t->elements) : b->elements_len;
			memcpy(t->elements, b->elements, t->elements_length);
		}
		lck_mtx_unlock(&fw_lock);
		break;
	}
	case FW_SCAN_DONE:
		fw_wake_status(&fw_scan_status, p, h.len);
		break;
	case FW_JOIN_DONE:
		fw_wake_status(&fw_join_status, p, h.len);
		break;
	case FW_LEFT:
		fw_wake_status(&fw_left_status, p, h.len);
		break;
	case FW_LINK: {
		bool up = h.len >= 1 && p[0] != 0;
		lck_mtx_lock(&fw_lock);
		bool changed = up != fw_link_up;
		fw_link_up = up;
		if (up && h.len >= sizeof(fw_link)) {
			memcpy(&fw_link, p, sizeof(fw_link));
		}
		lck_mtx_unlock(&fw_lock);
		if (changed && flame_wlan_link_hook != NULL) {
			flame_wlan_link_hook(up);
		}
		break;
	}
	case FW_RX:
		if (h.len >= 14 && h.len <= FW_FRAME_MAX && flame_wlan_rx_hook != NULL) {
			flame_wlan_rx_hook(p, h.len);
		}
		break;
	default:
		return EINVAL;
	}
	return 0;
}

static const struct cdevsw fw_cdevsw = {
	.d_open       = fw_open,
	.d_close      = fw_close,
	.d_read       = fw_read,
	.d_write      = fw_write,
	.d_ioctl      = eno_ioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = eno_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = 0,
};

/* Boot-arg sweetwifid=0: no /dev/wlan, no en0 (wland keeps its own utun). */
void
flame_wlan_init(void)
{
	uint32_t on = 1;
	PE_parse_boot_argn("sweetwifid", &on, sizeof(on));
	if (!on) {
		return;
	}
	int major = cdevsw_add(-1, &fw_cdevsw);
	if (major < 0) {
		printf("flame wlan: cdevsw_add failed (%d)\n", major);
		return;
	}
	devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0600, "wlan");
	redwood_net_init();
	flame_sta_init();
	printf("flame wlan: /dev/wlan ready (major %d)\n", major);
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
