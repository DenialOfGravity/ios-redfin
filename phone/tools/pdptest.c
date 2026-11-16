/*
 * pdptest: checks for the pdp_ip cellular interfaces of xnu patch 1075 (sweetbb & 0x10). Root only.
 *   pdptest list              every pdp_ip interface: flags, MTU, if_type, packet/byte counters (getifaddrs AF_LINK,
 *                             the same data CommCenter reads)
 *   pdptest ping N [COUNT]    puts 10.99.N.1 -> 10.99.N.2 (point-to-point) on pdp_ipN, sets it up, sends COUNT
 *                             (default 4) ICMP echo requests bound to it (IP_BOUND_IF) and waits 1 s for each reply.
 *                             Needs the kernel self-test first: sctl kern.sweet_pdp_test N
 * Output lines start with "pdptest:"; the last line of ping is "pdptest: pdp_ipN R/COUNT replies". Exit 0 when every
 * echo was answered, 1 otherwise, 2 on usage errors.
 */
#include <errno.h>
#include <ifaddrs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/types.h>

#ifndef IP_BOUND_IF
#define IP_BOUND_IF 25
#endif
#ifndef IFT_CELLULAR
#define IFT_CELLULAR 0xff
#endif

static int
list(void)
{
	struct ifaddrs *ifa, *a;
	int n = 0;
	if (getifaddrs(&ifa) != 0) {
		printf("pdptest: getifaddrs: %s\n", strerror(errno));
		return 1;
	}
	for (a = ifa; a; a = a->ifa_next) {
		if (strncmp(a->ifa_name, "pdp_ip", 6) != 0 || a->ifa_addr == NULL || a->ifa_addr->sa_family != AF_LINK ||
		    a->ifa_data == NULL) {
			continue;
		}
		struct if_data *d = (struct if_data *)a->ifa_data;
		printf("pdptest: %s flags 0x%x mtu %u type 0x%x%s rx %u pkts %u B tx %u pkts %u B oerr %u\n", a->ifa_name,
		    a->ifa_flags, d->ifi_mtu, d->ifi_type, d->ifi_type == IFT_CELLULAR ? " (cellular)" : "", d->ifi_ipackets,
		    d->ifi_ibytes, d->ifi_opackets, d->ifi_obytes, d->ifi_oerrors);
		n++;
	}
	freeifaddrs(ifa);
	printf("pdptest: %d pdp_ip interfaces\n", n);
	return n ? 0 : 1;
}

static uint16_t
cksum(const void *p, size_t len)
{
	const uint8_t *b = p;
	uint32_t s = 0;
	for (size_t i = 0; i + 1 < len; i += 2) {
		s += (uint32_t)(b[i] << 8 | b[i + 1]);
	}
	if (len & 1) {
		s += (uint32_t)b[len - 1] << 8;
	}
	while (s >> 16) {
		s = (s & 0xffff) + (s >> 16);
	}
	return (uint16_t)~s;
}

static uint64_t
now_us(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

static int
ping(int unit, int count)
{
	char name[IFNAMSIZ];
	snprintf(name, sizeof(name), "pdp_ip%d", unit);
	unsigned idx = if_nametoindex(name);
	if (idx == 0) {
		printf("pdptest: %s not found\n", name);
		return 1;
	}
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	struct ifaliasreq ar;
	memset(&ar, 0, sizeof(ar));
	strlcpy(ar.ifra_name, name, sizeof(ar.ifra_name));
	struct sockaddr_in *sa = (struct sockaddr_in *)&ar.ifra_addr, *da = (struct sockaddr_in *)&ar.ifra_broadaddr,
	    *ma = (struct sockaddr_in *)&ar.ifra_mask;
	sa->sin_len = da->sin_len = ma->sin_len = sizeof(*sa);
	sa->sin_family = da->sin_family = ma->sin_family = AF_INET;
	char local[20], peer[20];
	snprintf(local, sizeof(local), "10.99.%d.1", unit);
	snprintf(peer, sizeof(peer), "10.99.%d.2", unit);
	inet_pton(AF_INET, local, &sa->sin_addr);
	inet_pton(AF_INET, peer, &da->sin_addr);
	ma->sin_addr.s_addr = 0xffffffff;
	if (ioctl(s, SIOCAIFADDR, &ar) != 0 && errno != EEXIST) {
		printf("pdptest: SIOCAIFADDR %s %s -> %s: %s\n", name, local, peer, strerror(errno));
		return 1;
	}
	struct ifreq fr;
	memset(&fr, 0, sizeof(fr));
	strlcpy(fr.ifr_name, name, sizeof(fr.ifr_name));
	if (ioctl(s, SIOCGIFFLAGS, &fr) == 0 && !(fr.ifr_flags & IFF_UP)) {
		fr.ifr_flags |= IFF_UP;
		if (ioctl(s, SIOCSIFFLAGS, &fr) != 0) {
			printf("pdptest: SIOCSIFFLAGS up: %s\n", strerror(errno));
		}
	}
	close(s);
	printf("pdptest: %s %s -> %s up (index %u)\n", name, local, peer, idx);

	int r = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
	if (r < 0) {
		printf("pdptest: raw socket: %s\n", strerror(errno));
		return 1;
	}
	if (setsockopt(r, IPPROTO_IP, IP_BOUND_IF, &idx, sizeof(idx)) != 0) {
		printf("pdptest: IP_BOUND_IF: %s\n", strerror(errno));
	}
	struct sockaddr_in to;
	memset(&to, 0, sizeof(to));
	to.sin_len = sizeof(to);
	to.sin_family = AF_INET;
	to.sin_addr = da->sin_addr;
	uint16_t id = (uint16_t)getpid();
	int got = 0;
	for (int seq = 1; seq <= count; seq++) {
		uint8_t pkt[64];
		memset(pkt, 0, sizeof(pkt));
		pkt[0] = 8;
		pkt[4] = (uint8_t)(id >> 8);
		pkt[5] = (uint8_t)id;
		pkt[6] = (uint8_t)(seq >> 8);
		pkt[7] = (uint8_t)seq;
		memcpy(pkt + 8, "redfin-1075-pdp-selftest", 24);
		uint16_t c = cksum(pkt, sizeof(pkt));
		pkt[2] = (uint8_t)(c >> 8);
		pkt[3] = (uint8_t)c;
		uint64_t t0 = now_us();
		if (sendto(r, pkt, sizeof(pkt), 0, (struct sockaddr *)&to, sizeof(to)) != (ssize_t)sizeof(pkt)) {
			printf("pdptest: %s seq %d send: %s\n", name, seq, strerror(errno));
			continue;
		}
		int ok = 0;
		while (!ok) {
			uint64_t el = now_us() - t0;
			if (el >= 1000000u) {
				break;
			}
			struct timeval tv = { 0, (int)(1000000u - el) };
			fd_set fs;
			FD_ZERO(&fs);
			FD_SET(r, &fs);
			if (select(r + 1, &fs, NULL, NULL, &tv) <= 0) {
				break;
			}
			uint8_t in[256];
			struct sockaddr_in from;
			socklen_t fl = sizeof(from);
			ssize_t n = recvfrom(r, in, sizeof(in), 0, (struct sockaddr *)&from, &fl);
			if (n < 28) {
				continue;
			}
			int hl = (in[0] & 0xf) * 4;
			if (n < hl + 8 || in[hl] != 0 || (in[hl + 4] << 8 | in[hl + 5]) != id || (in[hl + 6] << 8 | in[hl + 7]) != seq) {
				continue;
			}
			ok = 1;
			got++;
			printf("pdptest: %s seq %d reply from %s in %llu us\n", name, seq, inet_ntoa(from.sin_addr),
			    (unsigned long long)(now_us() - t0));
		}
		if (!ok) {
			printf("pdptest: %s seq %d no reply\n", name, seq);
		}
	}
	close(r);
	printf("pdptest: %s %d/%d replies\n", name, got, count);
	return got == count ? 0 : 1;
}

int
main(int argc, char **argv)
{
	if (argc >= 2 && strcmp(argv[1], "list") == 0) {
		return list();
	}
	if (argc >= 3 && strcmp(argv[1], "ping") == 0) {
		int unit = atoi(argv[2]), count = argc >= 4 ? atoi(argv[3]) : 4;
		if (unit < 0 || unit > 15 || count < 1 || count > 100) {
			return 2;
		}
		return ping(unit, count);
	}
	fprintf(stderr, "usage: pdptest list | pdptest ping N [COUNT]\n");
	return 2;
}
