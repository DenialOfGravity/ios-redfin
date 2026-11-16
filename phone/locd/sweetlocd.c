/*
 * sweetlocd: the redfin port's GNSS daemon (Claude 2026-11-15). Owns everything GNSS so wland stays a modem/Wi-Fi daemon:
 * the QMI LOC client to the modem's GNSS engine (service 0x10, instance 2) and the PDS bridge for Apple locationd
 * (loc.c + pdsemu.c, moved here from ws-gnss-c's wland).
 *
 * Link: /dev/sweet-mqrtr-aux (xnu 1124, needs kern.sweet_mqrtr_aux & 1; sweetbb & 1): the kernel's second QRTR client
 * device, own ports 0x4700-0x470f (LOC 0x4700, PDS relay 0x4701), own read queue; the modem's servers come as NEW_SERVER at
 * open. It has no link control, so sweetlocd starting, stopping or crashing never touches wland's /dev/sweet-mqrtr or the
 * GLINK link that carries calls/SMS; at close the kernel sends QRTR DEL_CLIENT for our ports (the modem drops our LOC
 * client and its session even after a crash). If the device cannot be opened (old kernel, sysctl 0, EBUSY) it is tried
 * again every 10 s, one log line per new error.
 *
 * Switch /var/local/loc.on (words probe=1 / session=N / pds=1 / unlock=mi / unlock=none; loc.c header). Without it
 * nothing goes to the modem, except the engine lock repair: when /var/local/locd.lock-orig exists (an earlier run unlocked
 * and did not restore) the daemon also runs with loc.on absent, restores that value (GET, SET, GET; at most 5 runs) and
 * exits. Otherwise it exits 0 when loc.on is absent at start, or once it has been gone 15 s and the session is stopped
 * (launchd job org.iosonpixel.sweetlocd: KeepAlive PathState loc.on or locd.lock-orig). SIGTERM/SIGINT/SIGHUP: STOP +
 * REG_EVENTS 0 for a running session, BYE to the relay, the engine lock restore if we changed it, wait for the answers
 * (at most 3 s, 15 s while engine lock work is due), close, exit 0. ws-gnss-g: after loc.on goes and a confirmed restore,
 * the daemon stays up (sending nothing) until the modem's EFS write-back is seen or loc.c's 900 s limit ends the wait,
 * so locd.lock-orig is removed by the run that restored it (before: exit after 60 s, "engine lock work unfinished").
 *
 *   sweetlocd            daemon; log appended to /var/local/locd.log (SWEETLOCD_LOG=- : stdout)
 *   sweetlocd status     /var/local/loc.status (the daemon rewrites it every 2 s while something changes)
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/stat.h>
#include "englock.h"
#include "link.h"
#include "loc.h"

#define AUX_DEV		"/dev/sweet-mqrtr-aux"
#define LOCD_LOG	"/var/local/locd.log"
#define GONE_EXIT_MS	15000
#define SHUTDOWN_MS	3000
#define EFS_EXIT_MS	960000	/* ws-gnss-g: loc.c EFS_WAIT_MS (900 s) + 60 s */
#define LOCK_SHUTDOWN_MS 15000	/* engine lock restore: SET + GET (+ a second try); launchd ExitTimeOut is 25 */

static int fd = -1;
static volatile sig_atomic_t stop_sig;
static unsigned long long n_rx, n_tx, n_txfail;

uint64_t link_ms(void)
{
	static struct timespec t0;
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	if (!t0.tv_sec && !t0.tv_nsec)
		t0 = t;
	return (uint64_t)(t.tv_sec - t0.tv_sec) * 1000u + (uint64_t)((t.tv_nsec - t0.tv_nsec) / 1000000);
}

int qrtr_send(uint32_t type, uint32_t src_port, uint32_t dst_node, uint32_t dst_port, const void *payload, uint32_t len)
{
	static uint8_t b[sizeof(struct qrtr_hdr) + 8192 + 4];
	struct qrtr_hdr h = { 1, type, QRTR_NODE_APPS, src_port, 0, len, dst_node, dst_port };
	uint32_t padded = (len + 3) & ~3u;
	if (fd < 0 || padded > 8192) {
		n_txfail++;
		return -1;
	}
	memcpy(b, &h, sizeof(h));
	if (len)
		memcpy(b + sizeof(h), payload, len);
	memset(b + sizeof(h) + len, 0, padded - len);
	ssize_t w = write(fd, b, sizeof(h) + padded);
	if (w != (ssize_t)(sizeof(h) + padded)) {
		n_txfail++;
		if (n_txfail <= 10)
			printf("[%6llu] sweetlocd: write to %s: %s\n", (unsigned long long)link_ms(), AUX_DEV,
			       w < 0 ? strerror(errno) : "short");
		return -1;
	}
	n_tx++;
	return 0;
}

static void on_packet(const struct qrtr_hdr *h, const uint8_t *p, uint32_t n)
{
	uint32_t w[5] = { 0, 0, 0, 0, 0 };
	switch (h->type) {
	case QRTR_TYPE_DATA:
		if (h->confirm_rx) {	/* the sender waits for this (Linux qrtr_port_post) */
			uint32_t rt[5] = { QRTR_TYPE_RESUME_TX, QRTR_NODE_APPS, h->dst_port, 0, 0 };
			qrtr_send(QRTR_TYPE_RESUME_TX, h->dst_port, h->src_node, h->src_port, rt, sizeof(rt));
		}
		if (h->dst_port == LOC_PORT)
			loc_handle(h, p, n);
		else if (h->dst_port == PDSR_WPORT && h->src_node == QRTR_NODE_APPS)
			loc_pdsr_handle(h, p, n);
		break;
	case QRTR_TYPE_NEW_SERVER:
	case QRTR_TYPE_DEL_SERVER:
		memcpy(w, p, n < sizeof(w) ? n : sizeof(w));
		if (h->type == QRTR_TYPE_NEW_SERVER)
			loc_server(w[1], w[2], w[3], w[4]);
		else
			loc_server_gone(w[3], w[4]);
		break;
	case QRTR_TYPE_BYE:
		loc_server_gone(h->src_node, 0xffffffffu);
		break;
	default:
		break;
	}
}

static void try_open(void)
{
	static uint64_t next;
	static int last_err = -1;
	uint64_t now = link_ms();
	if (fd >= 0 || now < next)
		return;
	next = now + 10000;
	fd = open(AUX_DEV, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		if (errno != last_err)
			printf("[%6llu] sweetlocd: %s: %s%s (again every 10 s)\n", (unsigned long long)now, AUX_DEV, strerror(errno),
			       errno == ENXIO ? " (kern.sweet_mqrtr_aux & 1 not set)" : errno == ENOENT ? " (kernel without xnu 1124 or sweetbb & 1)" :
			       errno == EBUSY ? " (another sweetlocd?)" : "");
		last_err = errno;
		return;
	}
	last_err = -1;
	printf("[%6llu] sweetlocd: %s open (LOC port %#x, PDS relay port %#x)\n", (unsigned long long)now, AUX_DEV, LOC_PORT, PDSR_WPORT);
}

static void read_all(void)
{
	static uint8_t pkt[sizeof(struct qrtr_hdr) + 65536 + 64];
	for (int k = 0; k < 256 && fd >= 0; k++) {
		ssize_t r = read(fd, pkt, sizeof(pkt));
		if (r <= 0) {
			if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
				static unsigned told;
				if (told++ < 5)
					printf("[%6llu] sweetlocd: %s read: %s\n", (unsigned long long)link_ms(), AUX_DEV, strerror(errno));
			}
			return;
		}
		if ((size_t)r < sizeof(struct qrtr_hdr))
			continue;
		struct qrtr_hdr h;
		memcpy(&h, pkt, sizeof(h));
		uint32_t n = (uint32_t)((size_t)r - sizeof(h));
		if (h.size < n)
			n = h.size;
		n_rx++;
		on_packet(&h, pkt + sizeof(h), n);
	}
}

static void on_sig(int s)
{
	stop_sig = s;
}

static void log_open(void)
{
	const char *e = getenv("SWEETLOCD_LOG");
	if (e && !strcmp(e, "-"))
		return;
	struct stat st;
	if (!stat(LOCD_LOG, &st) && st.st_size > (4 << 20))
		rename(LOCD_LOG, LOCD_LOG ".old");	/* keeps the newest 4-8 MB */
	if (!freopen(LOCD_LOG, "a", stdout))
		return;
	dup2(fileno(stdout), 2);
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "status"))
		return loc_print();
	if (argc > 1) {
		fprintf(stderr, "usage: sweetlocd            (daemon; switch %s, log %s)\n"
				"       sweetlocd status     (%s)\n", LOC_ON, LOCD_LOG, LOC_STATUS);
		return 2;
	}
	log_open();
	setvbuf(stdout, NULL, _IOLBF, 0);
	time_t wall = time(NULL);
	char ts[64];
	strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&wall));
	link_ms();
	int repair_only = 0;
	if (access(LOC_ON, F_OK)) {
		if (access(LOCK_ORIG, F_OK)) {
			printf("[%6llu] sweetlocd: start %s pid %d: %s absent, nothing to do, exit 0\n", 0ull, ts, (int)getpid(), LOC_ON);
			return 0;
		}
		repair_only = 1;
		printf("[%6llu] sweetlocd: start %s pid %d: %s absent but %s present: engine lock repair only\n", 0ull, ts,
		       (int)getpid(), LOC_ON, LOCK_ORIG);
	} else {
		printf("[%6llu] sweetlocd: start %s pid %d (%s present)\n", 0ull, ts, (int)getpid(), LOC_ON);
	}
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_sig;	/* no SA_RESTART: select returns at once */
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
	uint64_t gone_since = 0, down_at = 0;
	int efs_told = 0;
	const char *why = NULL;
	for (;;) {
		try_open();
		struct timeval tv = { 0, 100000 };
		fd_set rs;
		FD_ZERO(&rs);
		if (fd >= 0)
			FD_SET(fd, &rs);
		int r = select(fd >= 0 ? fd + 1 : 0, &rs, NULL, NULL, &tv);
		if (r > 0 && fd >= 0 && FD_ISSET(fd, &rs))
			read_all();
		loc_poll();
		uint64_t now = link_ms();
		if (!why && stop_sig) {
			why = stop_sig == SIGTERM ? "SIGTERM" : stop_sig == SIGINT ? "SIGINT" : "SIGHUP";
		}
		if (!why) {
			if (loc_on_present())
				gone_since = 0;
			else if (!gone_since)
				gone_since = now;
			else if (now - gone_since >= GONE_EXIT_MS && !loc_lock_busy())
				why = repair_only ? "repair run over" : LOC_ON " gone";
			else if (now - gone_since >= 4 * GONE_EXIT_MS && !loc_efs_waiting())
				why = repair_only ? "repair not finished in 60 s" : LOC_ON " gone (engine lock work unfinished)";
			else if (now - gone_since >= EFS_EXIT_MS)	/* ws-gnss-g: backstop; loc.c ends the EFS wait at 900 s */
				why = repair_only ? "repair: EFS wait over 960 s" : LOC_ON " gone (EFS wait over 960 s)";
			if (!why && loc_efs_waiting() && now - gone_since >= 4 * GONE_EXIT_MS && !efs_told) {
				efs_told = 1;
				printf("[%6llu] sweetlocd: %s gone: staying up (sending nothing) until the modem's EFS write-back is seen "
				       "(at most 900 s; SIGTERM still exits at once)\n", (unsigned long long)now, LOC_ON);
			}
		}
		if (why && !down_at) {
			loc_shutdown(why);
			down_at = now;
		}
		if (down_at) {
			loc_poll();
			if (loc_quiet() || fd < 0 || now - down_at >= (loc_lock_busy() ? LOCK_SHUTDOWN_MS : SHUTDOWN_MS))
				break;
		}
	}
	printf("[%6llu] sweetlocd: exit (%s)%s: %llu packets in, %llu out, %llu send failures\n", (unsigned long long)link_ms(), why,
	       loc_quiet() ? "" : " with LOC requests unanswered (the kernel's DEL_CLIENT at close ends our client)", n_rx, n_tx, n_txfail);
	if (fd >= 0)
		close(fd);	/* the kernel sends DEL_CLIENT for our ports (xnu 1124) */
	unlink(LOC_STATUS);	/* `sweetlocd status` then says it is not running */
	return 0;
}
