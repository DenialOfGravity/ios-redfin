#ifndef NTP_H
#define NTP_H
/* ntp: ws-gnss-k one-shot SNTP v4 client (RFC 4330) for sweetlocd's ntp= word. It never sets the system clock: it only
 * measures the offset of the AP's CLOCK_REALTIME against the server and the round trip, as Android's NtpTrustedTime does
 * before GnssLocationProvider injects time (uncertainty = round trip / 2). */
#include <stddef.h>
#include <stdint.h>

#define NTP_PKT		48
#define NTP_UNIX_EPOCH	2208988800ull	/* seconds from 1900-01-01 to 1970-01-01 */

/* a client request (LI 0, VN 4, mode 3) whose transmit timestamp is t1 (ms since 1970) */
void ntp_build(uint8_t p[NTP_PKT], uint64_t t1_ms);
/* the server's answer to the request built with t1 (sent) and received at t4 (both ms since 1970, AP clock):
 * 0 ok (offset = server - AP in ms, rtt in ms), -1 short/not a server reply, -2 originate timestamp mismatch,
 * -3 unsynchronised server (stratum 0/16 or LI 3), -4 zero transmit time */
int ntp_parse(const uint8_t *p, size_t n, uint64_t t1_ms, uint64_t t4_ms, int64_t *off_ms, uint32_t *rtt_ms);
/* resolve host (name or IPv4 literal), send one request on UDP 123, wait up to timeout_ms; 0 ok, else -1 with err set */
int ntp_query(const char *host, unsigned timeout_ms, int64_t *off_ms, uint32_t *rtt_ms, char *err, size_t en);
#endif
