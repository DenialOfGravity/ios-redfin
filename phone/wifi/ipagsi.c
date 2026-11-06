/*
 * ipagsi.c: cellular data path, PLUMBING-ONLY STAGE.
 *
 * This build does no GSI or IPA register access and touches no DMA memory: it only proves the wland side on the
 * phone (cell.on words, the call settings handed over from cell.c's Get Current Settings, a second utun with the
 * call's address / peer / MTU, the ipadata= field in cell.status, and the counters line). Packets the stack sends
 * to the utun are read and counted, then dropped. The packet path (spec claude/redfin-cell-data-packets-spec-
 * 2026-11-06.md steps 1-7) is a later stage that replaces the "up" branch of ipagsi_poll().
 *
 * States (ipagsi.h): off -> setup-8 -> up, or failed-8; stopped after ipagsi_stop(). One start per wland run.
 */
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "link.h"
#include "ipagsi.h"
#include "ipnet.h"

#define LOG(fmt, ...)	printf("[%6llu] ipadata: " fmt "\n", (unsigned long long)link_ms(), ##__VA_ARGS__)

#define COUNTERS_MS	10000
#define UTUN_MAX	2048
#define DRAIN_MAX	64		/* packets read per poll */

static struct {
	int active, started_once;
	const char *state;
	struct ipagsi_call call;
	int fd;
	char name[32];
	uint64_t next_counters;
	unsigned long long tx_pkts, tx_bytes;
	unsigned tx_v4, tx_v6, tx_bad_af, rd_err;
} g = { .state = "off", .fd = -1 };

static void fail(const char *why)
{
	LOG("failed-8: %s", why);
	g.state = "failed-8";
	g.active = 0;
	if (g.fd >= 0) {
		close(g.fd);
		g.fd = -1;
	}
}

void ipagsi_start(const struct ipagsi_call *c, int tables)
{
	char ip[16], gw[16];
	if (g.started_once) {
		LOG("start refused: already started once in this wland run (state %s)", g.state);
		return;
	}
	g.started_once = 1;
	g.call = *c;
	if (!g.call.mtu)
		g.call.mtu = 1500;
	inet_ntop(AF_INET, &g.call.ip, ip, sizeof(ip));
	inet_ntop(AF_INET, &g.call.gw, gw, sizeof(gw));
	LOG("start (plumbing only, no GSI/IPA access): %s peer %s mtu %u%s", ip, gw, g.call.mtu,
	    tables ? "; tables not implemented in this stage" : "; ipacmd=0");
	if (!g.call.ip || !g.call.gw) {
		fail("the call has no IPv4 address or gateway");
		return;
	}
	g.active = 1;
	g.state = "setup-8";
}

void ipagsi_stop(const char *why)
{
	if (!g.active)
		return;
	LOG("stop: %s (tx %llu pkts %llu B)", why, g.tx_pkts, g.tx_bytes);
	g.active = 0;
	if (g.fd >= 0) {
		close(g.fd);
		g.fd = -1;
	}
	g.state = "stopped";
}

int ipagsi_running(void)
{
	return g.active;
}

const char *ipagsi_state(void)
{
	return g.state;
}

static void counters(uint64_t now)
{
	if (now < g.next_counters)
		return;
	g.next_counters = now + COUNTERS_MS;
	LOG("counters %s: tx %llu pkts %llu B (v4 %u v6 %u bad-af %u) rx 0 (no packet path in this stage) read-err %u",
	    g.name, g.tx_pkts, g.tx_bytes, g.tx_v4, g.tx_v6, g.tx_bad_af, g.rd_err);
}

void ipagsi_poll(void)
{
	if (!g.active)
		return;
	if (!strcmp(g.state, "setup-8")) {
		LOG("step 8 utun");
		g.fd = ipnet_utun_open(g.name, sizeof(g.name));
		if (g.fd < 0) {
			fail("utun open");
			return;
		}
		if (ipnet_utun_set(g.name, g.call.ip, g.call.gw, g.call.mtu) < 0) {
			fail("utun address / mtu");
			return;
		}
		g.state = "up";
		g.next_counters = link_ms() + COUNTERS_MS;
		LOG("running on %s (plumbing only: packets sent to it are counted and dropped)", g.name);
		return;
	}
	/* up: drain the utun so the stack doesn't queue up; count and drop */
	uint8_t b[UTUN_MAX];
	for (int i = 0; i < DRAIN_MAX; i++) {
		ssize_t n = read(g.fd, b, sizeof(b));
		if (n < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				g.rd_err++;
			break;
		}
		if (n <= 4)
			break;
		uint32_t af;
		memcpy(&af, b, 4);
		af = ntohl(af);
		if (af == AF_INET)
			g.tx_v4++;
		else if (af == AF_INET6)
			g.tx_v6++;
		else {
			g.tx_bad_af++;
			continue;
		}
		g.tx_pkts++;
		g.tx_bytes += (unsigned long long)(n - 4);
	}
	counters(link_ms());
}
