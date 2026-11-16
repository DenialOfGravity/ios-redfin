/* ntp: ws-gnss-k SNTP v4 client for sweetlocd ntp= (ntp.h). One UDP request, no clock change. */
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include "ntp.h"

static void put_ts(uint8_t *p, uint64_t ms)
{
	uint64_t s = ms / 1000 + NTP_UNIX_EPOCH;
	uint64_t f = ((ms % 1000) << 32) / 1000;
	for (int i = 0; i < 4; i++) {
		p[i] = (uint8_t)(s >> (24 - 8 * i));
		p[4 + i] = (uint8_t)(f >> (24 - 8 * i));
	}
}

static int64_t get_ts(const uint8_t *p)	/* ms since 1970 (NTP era 0) */
{
	uint64_t s = (uint64_t)p[0] << 24 | (uint64_t)p[1] << 16 | (uint64_t)p[2] << 8 | p[3];
	uint64_t f = (uint64_t)p[4] << 24 | (uint64_t)p[5] << 16 | (uint64_t)p[6] << 8 | p[7];
	return (int64_t)(s - NTP_UNIX_EPOCH) * 1000 + (int64_t)((f * 1000) >> 32);
}

void ntp_build(uint8_t p[NTP_PKT], uint64_t t1_ms)
{
	memset(p, 0, NTP_PKT);
	p[0] = 0x23;		/* LI 0, VN 4, mode 3 (client) */
	put_ts(p + 40, t1_ms);
}

int ntp_parse(const uint8_t *p, size_t n, uint64_t t1_ms, uint64_t t4_ms, int64_t *off_ms, uint32_t *rtt_ms)
{
	uint8_t ref[8], q[NTP_PKT];
	if (n < NTP_PKT || (p[0] & 7) != 4 || ((p[0] >> 3) & 7) < 3)
		return -1;
	ntp_build(q, t1_ms);
	memcpy(ref, q + 40, 8);
	if (memcmp(p + 24, ref, 8))
		return -2;
	if (p[1] == 0 || p[1] >= 16 || (p[0] >> 6) == 3)
		return -3;
	static const uint8_t z[8];
	if (!memcmp(p + 40, z, 8))
		return -4;
	int64_t t1 = (int64_t)t1_ms, t4 = (int64_t)t4_ms, t2 = get_ts(p + 32), t3 = get_ts(p + 40);
	int64_t rtt = (t4 - t1) - (t3 - t2);
	*off_ms = ((t2 - t1) + (t3 - t4)) / 2;
	*rtt_ms = rtt < 0 ? 0u : (uint32_t)rtt;
	return 0;
}

static uint64_t rt_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint64_t mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

int ntp_query(const char *host, unsigned timeout_ms, int64_t *off_ms, uint32_t *rtt_ms, char *err, size_t en)
{
	struct addrinfo hints, *ai = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	int g = getaddrinfo(host, "123", &hints, &ai);
	if (g || !ai) {
		snprintf(err, en, "resolve %s failed (%s)", host, g ? gai_strerror(g) : "no address");
		return -1;
	}
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		snprintf(err, en, "socket: %s", strerror(errno));
		freeaddrinfo(ai);
		return -1;
	}
	uint8_t p[NTP_PKT], r[256];
	uint64_t t1 = rt_ms(), m0 = mono_ms();
	ntp_build(p, t1);
	ssize_t s = sendto(fd, p, sizeof(p), 0, ai->ai_addr, ai->ai_addrlen);
	freeaddrinfo(ai);
	if (s != (ssize_t)sizeof(p)) {
		snprintf(err, en, "sendto: %s", strerror(errno));
		close(fd);
		return -1;
	}
	for (;;) {
		uint64_t el = mono_ms() - m0;
		if (el >= timeout_ms) {
			snprintf(err, en, "no answer from %s in %u ms", host, timeout_ms);
			close(fd);
			return -1;
		}
		fd_set rs;
		FD_ZERO(&rs);
		FD_SET(fd, &rs);
		struct timeval tv = { (time_t)((timeout_ms - el) / 1000), (int)(((timeout_ms - el) % 1000) * 1000) };
		int k = select(fd + 1, &rs, NULL, NULL, &tv);
		if (k < 0 && errno == EINTR)
			continue;
		if (k <= 0)
			continue;
		ssize_t n = recv(fd, r, sizeof(r), 0);
		uint64_t t4 = rt_ms();
		if (n < 0)
			continue;
		int e = ntp_parse(r, (size_t)n, t1, t4, off_ms, rtt_ms);
		if (e == -2 || e == -1)
			continue;	/* not the answer to our request: keep waiting */
		close(fd);
		if (e) {
			snprintf(err, en, "%s: %s", host, e == -3 ? "server not synchronised" : "zero transmit time");
			return -1;
		}
		return 0;
	}
}
