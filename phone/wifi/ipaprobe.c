/*
 * ipaprobe.c: packet decoder + active probes for the cellular data path (see ipaprobe.h).  Claude 2026-11-09.
 *
 * Why: on 2026-11-08 the only DL packets that reached iOS were TCP RST+ACK from port 443, TTL 255, to our CGNAT
 * address: replies to our own SYNs, so the UL path works for some packets while DNS gets no answer.  This module
 * tells per protocol what goes out and what comes back, without iOS in the loop.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>

#include "link.h"
#include "cell.h"
#include "ipaprobe.h"

#define LOG(fmt, ...)	printf("[%6llu] ipaprobe: " fmt "\n", (unsigned long long) link_ms(), ##__VA_ARGS__)

static int word_int(const char *key, int def)
{
	char b[512] = "";
	FILE *f = fopen(CELL_ON, "r");
	char *at;

	if (f) {
		size_t k = fread(b, 1, sizeof(b) - 1, f);
		b[k] = 0;
		fclose(f);
	}
	at = strstr(b, key);
	return at ? atoi(at + strlen(key)) : def;
}

/* ------------------------------------------------------------------ decoder */

#define PKT_LOG_MAX	300
#define PROBE_ICMP_ID	0x5a5a
#define PROBE_DNS_ID	0x5a00		/* + probe index */
#define PROBE_SPORT	40000		/* + probe index (TCP and UDP source ports) */

static int pkt_mode = -1;
static unsigned pkt_logged, rx_logged;

static const char *ipstr(uint32_t a_be, char *b)		/* a_be: network order, 4 bytes as in the header */
{
	const uint8_t *p = (const uint8_t *) &a_be;
	snprintf(b, 16, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
	return b;
}

static void tcpflags(unsigned f, char *b)
{
	b[0] = 0;
	if (f & 0x02) strcat(b, "S");
	if (f & 0x10) strcat(b, "A");
	if (f & 0x04) strcat(b, "R");
	if (f & 0x01) strcat(b, "F");
	if (f & 0x08) strcat(b, "P");
	if (f & 0x20) strcat(b, "U");
	if (!b[0]) strcpy(b, "-");
}

/* is this a reply to one of our probes?  Returns a static description or NULL. */
static const char *probe_match(const uint8_t *ip, size_t n, unsigned proto, unsigned ihl)
{
	static char d[48];
	const uint8_t *l4 = ip + ihl;

	if (proto == 1 && n >= ihl + 8) {
		if (l4[0] == 0 && (l4[4] << 8 | l4[5]) == PROBE_ICMP_ID && (l4[6] & 0x40))
			return NULL;	/* p2keep reply */
		if (l4[0] == 0 && (l4[4] << 8 | l4[5]) == PROBE_ICMP_ID) {
			snprintf(d, sizeof(d), "ICMP echo reply seq %u", l4[6] << 8 | l4[7]);
			return d;
		}
		if ((l4[0] == 3 || l4[0] == 11) && n >= ihl + 8 + 20 + 8) {	/* unreachable / time exceeded: quoted header */
			const uint8_t *q = l4 + 8;
			unsigned qihl = (q[0] & 0xf) * 4;
			if (n >= ihl + 8 + qihl + 8) {
				unsigned qsport = q[qihl] << 8 | q[qihl + 1];
				if (qsport >= PROBE_SPORT && qsport < PROBE_SPORT + 32) {
					snprintf(d, sizeof(d), "ICMP %s code %u for probe %u", l4[0] == 3 ? "unreachable" : "time-exceeded",
						 l4[1], qsport - PROBE_SPORT);
					return d;
				}
			}
		}
	} else if (proto == 17 && n >= ihl + 8 + 12) {
		unsigned dport = l4[2] << 8 | l4[3], id = l4[8] << 8 | l4[9];
		if (dport >= PROBE_SPORT && dport < PROBE_SPORT + 32 && (id & 0xff00) == PROBE_DNS_ID) {
			snprintf(d, sizeof(d), "DNS answer probe %u rcode %u answers %u", id & 0xff, l4[11] & 0xf, l4[14] << 8 | l4[15]);
			return d;
		}
	} else if (proto == 6 && n >= ihl + 20) {
		unsigned dport = l4[2] << 8 | l4[3];
		if (dport >= PROBE_SPORT && dport < PROBE_SPORT + 32) {
			char f[8];
			tcpflags(l4[13], f);
			snprintf(d, sizeof(d), "TCP %s to probe %u", f, dport - PROBE_SPORT);
			return d;
		}
	}
	return NULL;
}

void ipaprobe_pkt(const char *dir, uint8_t mux, const uint8_t *ip, size_t n)
{
	char s[16], t[16], fl[8], extra[64] = "";
	unsigned ihl, proto, ttl, len;
	const uint8_t *l4;
	const char *pm = NULL;
	int is_rx = dir[0] == 'r';

	if (pkt_mode < 0)
		pkt_mode = word_int("p2pkt=", 0);
	if (n < 20 || (ip[0] >> 4) != 4)
		return;
	ihl = (ip[0] & 0xf) * 4;
	if (ihl < 20 || n < ihl)
		return;
	proto = ip[9];
	ttl = ip[8];
	len = ip[2] << 8 | ip[3];
	l4 = ip + ihl;
	if (is_rx)
		pm = probe_match(ip, n, proto, ihl);
	if (pm)
		LOG("probe REPLY: %s", pm);
	if (!(pkt_mode >= 2 || (pkt_mode == 1 && is_rx)))
		return;
	if (is_rx ? rx_logged++ >= PKT_LOG_MAX : pkt_logged++ >= PKT_LOG_MAX)	/* RX and TX capped separately */
		return;
	ipstr(*(const uint32_t *) (ip + 12), s);
	ipstr(*(const uint32_t *) (ip + 16), t);
	switch (proto) {
	case 1:
		if (n >= ihl + 8)
			snprintf(extra, sizeof(extra), "icmp type %u code %u id %u seq %u", l4[0], l4[1], l4[4] << 8 | l4[5],
				 l4[6] << 8 | l4[7]);
		LOG("%s pkt mux %u: %s -> %s ttl %u len %u %s", dir, mux, s, t, ttl, len, extra);
		break;
	case 6:
		if (n >= ihl + 20) {
			tcpflags(l4[13], fl);
			LOG("%s pkt mux %u: tcp %s:%u -> %s:%u ttl %u len %u flags %s win %u", dir, mux, s, l4[0] << 8 | l4[1], t,
			    l4[2] << 8 | l4[3], ttl, len, fl, l4[14] << 8 | l4[15]);
		}
		break;
	case 17:
		if (n >= ihl + 8) {
			unsigned sp = l4[0] << 8 | l4[1], dp = l4[2] << 8 | l4[3];
			if ((sp == 53 || dp == 53) && n >= ihl + 8 + 12)
				snprintf(extra, sizeof(extra), " dns id %04x flags %02x%02x q %u an %u", l4[8] << 8 | l4[9], l4[10], l4[11],
					 l4[12] << 8 | l4[13], l4[14] << 8 | l4[15]);
			LOG("%s pkt mux %u: udp %s:%u -> %s:%u ttl %u len %u%s", dir, mux, s, sp, t, dp, ttl, len, extra);
		}
		break;
	default:
		LOG("%s pkt mux %u: proto %u %s -> %s ttl %u len %u", dir, mux, proto, s, t, ttl, len);
	}
}

/* ------------------------------------------------------------------ probes */

static uint16_t csum16(const uint8_t *p, size_t n, uint32_t acc)
{
	while (n > 1) {
		acc += (uint32_t) (p[0] << 8 | p[1]);
		p += 2;
		n -= 2;
	}
	if (n)
		acc += (uint32_t) p[0] << 8;
	while (acc >> 16)
		acc = (acc & 0xffff) + (acc >> 16);
	return (uint16_t) ~acc;
}

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t) (v >> 8); p[1] = (uint8_t) v; }

/* IPv4 header; src/dst network order */
static size_t ip_hdr(uint8_t *b, uint32_t src, uint32_t dst, unsigned proto, size_t payload, unsigned id)
{
	memset(b, 0, 20);
	b[0] = 0x45;
	put16(b + 2, 20 + payload);
	put16(b + 4, id);
	b[6] = 0x40;		/* DF */
	b[8] = 64;
	b[9] = proto;
	memcpy(b + 12, &src, 4);
	memcpy(b + 16, &dst, 4);
	put16(b + 10, csum16(b, 20, 0));
	return 20;
}

static uint32_t pseudo(uint32_t src, uint32_t dst, unsigned proto, size_t l4len)
{
	const uint8_t *s = (const uint8_t *) &src, *d = (const uint8_t *) &dst;
	return (uint32_t) (s[0] << 8 | s[1]) + (s[2] << 8 | s[3]) + (d[0] << 8 | d[1]) + (d[2] << 8 | d[3]) + proto + l4len;
}

static size_t mk_icmp(uint8_t *b, uint32_t src, uint32_t dst, unsigned seq, unsigned id)
{
	uint8_t *l4 = b + 20;
	size_t pl = 8 + 32;
	memset(l4, 0, pl);
	l4[0] = 8;
	put16(l4 + 4, PROBE_ICMP_ID);
	put16(l4 + 6, seq);
	memcpy(l4 + 8, "redfin-ipaprobe-echo-request...", 31);
	put16(l4 + 2, csum16(l4, pl, 0));
	return ip_hdr(b, src, dst, 1, pl, id) + pl;
}

/* DNS A query for NAME (dotted), id PROBE_DNS_ID + idx, source port PROBE_SPORT + idx */
static size_t mk_dns(uint8_t *b, uint32_t src, uint32_t dst, unsigned idx, const char *name, unsigned id)
{
	uint8_t *l4 = b + 20, *q = l4 + 8 + 12;
	size_t qn = 0;
	const char *p = name;

	memset(l4, 0, 8 + 12);
	put16(l4 + 0, PROBE_SPORT + idx);
	put16(l4 + 2, 53);
	put16(l4 + 8, PROBE_DNS_ID + idx);
	l4[10] = 0x01;				/* RD */
	put16(l4 + 12, 1);			/* QDCOUNT */
	while (*p) {
		const char *dot = strchr(p, '.');
		size_t l = dot ? (size_t) (dot - p) : strlen(p);
		q[qn++] = (uint8_t) l;
		memcpy(q + qn, p, l);
		qn += l;
		p += l;
		if (*p == '.')
			p++;
	}
	q[qn++] = 0;
	put16(q + qn, 1);	qn += 2;	/* A */
	put16(q + qn, 1);	qn += 2;	/* IN */
	{
		size_t ul = 8 + 12 + qn;
		put16(l4 + 4, ul);
		put16(l4 + 6, 0);
		put16(l4 + 6, csum16(l4, ul, pseudo(src, dst, 17, ul)));
		if (!(l4[6] | l4[7]))
			l4[6] = l4[7] = 0xff;
		return ip_hdr(b, src, dst, 17, ul, id) + ul;
	}
}

/* TCP SYN; opts = 1: iOS-like 44-byte header (MSS, NOP, wscale, NOP, NOP, timestamps, SACK-permitted, EOL), which on
 * 11-08 matched the modem's catch-all rule 524 (rt 1) like iOS's own SYNs; opts = 0: plain 24-byte header (MSS only),
 * which matches the modem's TCP-SYN rule 515 (rt 3). */
static size_t mk_syn(uint8_t *b, uint32_t src, uint32_t dst, unsigned idx, unsigned dport, unsigned id, int opts)
{
	uint8_t *l4 = b + 20;
	size_t tl = opts ? 44 : 24;

	memset(l4, 0, tl);
	put16(l4 + 0, PROBE_SPORT + idx);
	put16(l4 + 2, dport);
	l4[4] = 0x5a; l4[5] = 0x5a; l4[6] = (uint8_t) idx; l4[7] = (uint8_t) id;	/* seq */
	l4[12] = (uint8_t) ((tl / 4) << 4);
	l4[13] = 0x02;		/* SYN */
	put16(l4 + 14, 65535);
	l4[20] = 2; l4[21] = 4; put16(l4 + 22, 1400);	/* MSS */
	if (opts) {
		l4[24] = 1;				/* NOP */
		l4[25] = 3; l4[26] = 3; l4[27] = 6;	/* window scale 6 */
		l4[28] = 1; l4[29] = 1;			/* NOP NOP */
		l4[30] = 8; l4[31] = 10;		/* timestamps */
		l4[32] = 0x5a; l4[33] = 0x5a; l4[34] = 0; l4[35] = (uint8_t) id;
		l4[36] = l4[37] = l4[38] = l4[39] = 0;
		l4[40] = 4; l4[41] = 2;			/* SACK permitted */
		l4[42] = 0; l4[43] = 0;			/* EOL */
	}
	put16(l4 + 16, csum16(l4, tl, pseudo(src, dst, 6, tl)));
	return ip_hdr(b, src, dst, 6, tl, id) + tl;
}

static uint32_t a4(unsigned a, unsigned b, unsigned c, unsigned d)
{
	uint8_t x[4] = { (uint8_t) a, (uint8_t) b, (uint8_t) c, (uint8_t) d };
	uint32_t v;
	memcpy(&v, x, 4);
	return v;
}

/* EE1 (modem) GSI channel pointers, read only: ch0 ep5 DL producer, ch2 ep8 DL producer, ch3 pipe 10 consumer, ch7 ep14
 * UL consumer.  GSI EE1 channel context = 0x1e04000 + 0xf000 + 0x4000 + 0x80 * ch; +0x10 RP, +0x18 WP. */
static void ee1_ptrs(uint32_t (*rd)(uint64_t), uint32_t *out)
{
	static const int chs[4] = { 0, 2, 3, 7 };
	for (int i = 0; i < 4; i++) {
		uint64_t c = 0x1e04000ull + 0xf000 + 0x4000 + 0x80 * chs[i];
		out[2 * i] = rd(c + 0x10);
		out[2 * i + 1] = rd(c + 0x18);
	}
}

static int probe_mode = -1, probe_round, probe_step, pend;
static uint64_t probe_at;
static uint32_t ptr0[8];
static unsigned ipid = 0x5a00;

/* p2tput=N (Claude 2026-11-09): one bounded HTTP download of N MB (1-3; the SIM's data cap is small) through the phone's own
 * TCP stack over the utun (IP_BOUND_IF), 20 s after up: speed.cloudflare.com/__down?bytes=... Logs bytes, time, Mbit/s. */
static volatile int tput_state;		/* 0 not started, 1 running, 2 done */
static unsigned tput_mb;
static char tput_if[32];
#ifndef IP_BOUND_IF
#define IP_BOUND_IF 25
#endif
static void *tput_thread(void *arg)
{
	struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *ai = NULL;
	unsigned want = tput_mb * 1048576u;
	int fd = -1, idx = (int) if_nametoindex(tput_if);
	char req[256], buf[16384];
	struct timeval t0, t1, tv = { 60, 0 };
	unsigned long long got = 0;
	int rc = getaddrinfo("speed.cloudflare.com", "80", &hints, &ai);
	if (rc || !ai) {
		LOG("tput: getaddrinfo: %s", gai_strerror(rc));
		goto out;
	}
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		goto out;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	if (idx > 0 && setsockopt(fd, IPPROTO_IP, IP_BOUND_IF, &idx, sizeof(idx)))
		LOG("tput: IP_BOUND_IF %s: %s", tput_if, strerror(errno));
	gettimeofday(&t0, NULL);
	if (connect(fd, ai->ai_addr, ai->ai_addrlen)) {
		LOG("tput: connect: %s", strerror(errno));
		goto out;
	}
	gettimeofday(&t1, NULL);
	LOG("tput: connected to %s via %s in %ld ms, GET %u bytes", inet_ntoa(((struct sockaddr_in *) ai->ai_addr)->sin_addr), tput_if,
	    (long) ((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000), want);
	snprintf(req, sizeof(req), "GET /__down?bytes=%u HTTP/1.1\r\nHost: speed.cloudflare.com\r\nUser-Agent: redfin-ipaprobe\r\nConnection: close\r\n\r\n", want);
	if (write(fd, req, strlen(req)) < 0)
		goto out;
	gettimeofday(&t0, NULL);
	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf));
		if (n <= 0) {
			if (n < 0)
				LOG("tput: read: %s after %llu bytes", strerror(errno), got);
			break;
		}
		got += (unsigned long long) n;
		if (got >= want + 400)
			break;
	}
	gettimeofday(&t1, NULL);
	{
		double sec = (double) (t1.tv_sec - t0.tv_sec) + (double) (t1.tv_usec - t0.tv_usec) / 1e6;
		LOG("tput: RESULT %llu bytes in %.2f s = %.2f Mbit/s (%s)", got, sec, sec > 0 ? got * 8.0 / sec / 1e6 : 0.0,
		    got >= want ? "complete" : "SHORT");
	}
out:
	if (fd >= 0)
		close(fd);
	if (ai)
		freeaddrinfo(ai);
	tput_state = 2;
	return NULL;
}

void ipaprobe_tput(const char *ifname)
{
	static uint64_t at;
	pthread_t th;
	if (tput_state)
		return;
	if (!at) {
		int n = word_int("p2tput=", 0);
		if (n <= 0) {
			tput_state = 2;
			return;
		}
		tput_mb = n > 3 ? 3 : (unsigned) n;
		at = link_ms() + 20000;
		snprintf(tput_if, sizeof(tput_if), "%s", ifname);
		LOG("p2tput=%u: HTTP download of %u MB via %s in 20 s", tput_mb, tput_mb, tput_if);
		return;
	}
	if (link_ms() < at)
		return;
	tput_state = 1;
	if (pthread_create(&th, NULL, tput_thread, NULL))
		tput_state = 2;
	else
		pthread_detach(th);
}

void ipaprobe_poll(const struct ipagsi_call *c, int (*send)(const uint8_t *, size_t), unsigned tx_free,
		   uint32_t (*rd)(uint64_t))
{
	static uint8_t b[256];
	uint32_t p[8];
	size_t n = 0;
	const char *what = "";

	if (probe_mode < 0) {
		probe_mode = word_int("p2probe=", 0);
		if (probe_mode)
			LOG("p2probe=%d: %s rounds of 10 test packets, 20 s apart, from the call's address", probe_mode,
			    probe_mode == 1 ? "6" : "endless");
		probe_at = link_ms() + 5000;		/* first round 5 s after up */
	}
	{	/* p2keep=N (Claude 2026-11-09): one ICMP echo to the gateway every N s (1-30), to see whether steady light UL keeps
		 * the modem's UL pipe from going to sleep. ~120 bytes per round trip. */
		static int keep = -1;
		static uint64_t keep_at;
		static unsigned kseq;
		if (keep < 0) {
			keep = word_int("p2keep=", 0);
			if (keep > 30)
				keep = 30;
			if (keep > 0)
				LOG("p2keep=%d: gateway echo every %d s", keep, keep);
		}
		if (keep > 0 && c->ip && tx_free > 2 && link_ms() >= keep_at) {
			uint8_t kb[96];
			size_t kn = mk_icmp(kb, c->ip, c->gw, 0x4000 + (kseq++ & 0x3fff), ipid++);
			keep_at = link_ms() + 1000ull * (unsigned) keep;
			send(kb, kn);
		}
	}
	if (!probe_mode || !c->ip)
		return;
	if (pend && link_ms() >= probe_at - 17000) {	/* 3 s after a round's last packet: the modem's pointers again */
		pend = 0;
		ee1_ptrs(rd, p);
		LOG("round %d: EE1 deltas ch0 +%u ch2 +%u ch3 +%u ch7 +%u (wp elements; DL producers 0/2, pipe10 consumer 3, UL consumer 7)",
		    probe_round, (p[1] - ptr0[1]) / 16, (p[3] - ptr0[3]) / 16, (p[5] - ptr0[5]) / 16, (p[7] - ptr0[7]) / 16);
	}
	if (tx_free == 0)
		return;
	if (probe_step == 0) {
		if (link_ms() < probe_at)
			return;
		if (probe_mode == 1 && probe_round >= 6)
			return;
		ee1_ptrs(rd, ptr0);
		probe_round++;
		LOG("round %d: EE1 ch0 %08x/%08x ch2 %08x/%08x ch3 %08x/%08x ch7 %08x/%08x (rp/wp before)", probe_round,
		    ptr0[0], ptr0[1], ptr0[2], ptr0[3], ptr0[4], ptr0[5], ptr0[6], ptr0[7]);
	}
	switch (probe_step) {
	case 0: n = mk_icmp(b, c->ip, c->gw, probe_round, ipid++); what = "ICMP echo -> gateway"; break;
	case 1: n = mk_icmp(b, c->ip, a4(8, 8, 8, 8), 0x100 + probe_round, ipid++); what = "ICMP echo -> 8.8.8.8"; break;
	case 2: n = mk_dns(b, c->ip, c->dns1 ? c->dns1 : a4(8, 8, 8, 8), 2, "captive.apple.com", ipid++); what = "DNS A captive.apple.com -> dns1"; break;
	case 3: n = mk_dns(b, c->ip, a4(8, 8, 8, 8), 3, "captive.apple.com", ipid++); what = "DNS A captive.apple.com -> 8.8.8.8"; break;
	case 4: n = mk_dns(b, c->ip, a4(1, 1, 1, 1), 4, "one.one.one.one", ipid++); what = "DNS A one.one.one.one -> 1.1.1.1"; break;
	case 5: n = mk_syn(b, c->ip, a4(1, 1, 1, 1), 5, 80, ipid++, 0); what = "TCP SYN -> 1.1.1.1:80"; break;
	case 6: n = mk_syn(b, c->ip, a4(1, 1, 1, 1), 6, 443, ipid++, 0); what = "TCP SYN -> 1.1.1.1:443"; break;
	case 7: n = mk_syn(b, c->ip, a4(8, 8, 8, 8), 7, 53, ipid++, 0); what = "TCP SYN -> 8.8.8.8:53"; break;
	case 8: n = mk_syn(b, c->ip, a4(104, 18, 14, 92), 8, 443, ipid++, 1); what = "iOS-like SYN (44 B hdr) -> 104.18.14.92:443 (answered RST on 11-08)"; break;
	case 9: n = mk_syn(b, c->ip, a4(104, 18, 14, 92), 9, 443, ipid++, 0); what = "plain SYN -> 104.18.14.92:443"; break;
	}
	if (n) {
		if (send(b, n))
			LOG("probe %d (%s): send failed", probe_step, what);
		else
			LOG("probe %d sent: %s (%zu bytes)", probe_step, what, n);
	}
	if (++probe_step > 9) {
		probe_step = 0;
		probe_at = link_ms() + 20000;
		pend = 1;
	}
}
