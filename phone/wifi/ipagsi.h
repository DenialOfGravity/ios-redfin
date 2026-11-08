#ifndef IPAGSI_H
#define IPAGSI_H
/*
 * ipagsi.c: the AP side of the IPA v4.7 data path (redfin / lito), polled from wland's idle loop over /dev/pmem:
 * GSI channels 5 (AP_COMMAND_TX, endpoint 7), 2 (AP_MODEM_TX, endpoint 2) and 7 (AP_MODEM_RX, endpoint 16),
 * QMAP framing, and a utun carrying the IP packets.  The IPA table/header init by immediate commands on
 * channel 5 is the part still missing (see README.ipagsi.md).  Only with ipadata=1 in cell.on, started by
 * cell.c once X1's Start Network + Get Current Settings succeeded.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* the call's IPv4 settings (WDS Get Current Settings, cell.c x1_settings); network byte order */
struct ipagsi_call {
	uint32_t ip, gw, mask, dns1, dns2;
	unsigned mtu;		/* 0: not given (1500 used) */
};

void ipagsi_start(const struct ipagsi_call *c, int tables);	/* tables: 0 = no channel 5 / table init (ipacmd=0).
							   One attempt per wland run: after it has run (up, failed
							   or stopped) a second start is refused with a log. */
void ipagsi_stop(const char *why);	/* polling and the utun end; the GSI/IPA are left as they are (no reset) */
int ipagsi_running(void);		/* setup reached "up": X1 holds the call while it runs (x1hold=1) */
const char *ipagsi_state(void);		/* for cell.status: off | setup-1..setup-8 | up | failed-N | stopped */
void ipagsi_poll(void);			/* wland.c net_idle */

/*
 * QMAP v1 (rmnet_map_header): byte 0 = cd bit 7 (1 = command frame) | bit 6 reserved | pad length 5:0, byte 1 = mux id,
 * bytes 2-3 = payload length big-endian (padding included). Header-only so the host harness can test them.
 */
#define QMAP_HDR	4

/* one IP packet behind a QMAP header (no padding, as rmnet's egress); total length, or 0 if it does not fit */
static inline size_t qmap_frame(uint8_t *out, size_t cap, const uint8_t *ip, size_t len, uint8_t mux)
{
	if (len == 0 || len > 0xffff || len + QMAP_HDR > cap)
		return 0;
	out[0] = 0;
	out[1] = mux;
	out[2] = (uint8_t)(len >> 8);
	out[3] = (uint8_t)len;
	memcpy(out + QMAP_HDR, ip, len);
	return len + QMAP_HDR;
}

/* QMAP v5 with the checksum header (rmnet MAP_CKSUMV5 egress, Linux rmnet_map_add_map_header +
 * rmnet_map_v5_checksum_uplink_packet; Claude 2026-11-08): byte 0 = next-header bit 6, no padding, pkt_len = IP length
 * only; then the 4-byte v5 header: header_info = type 2 (CSUM_OFFLOAD) << 1, csum_info 0 (no checksum requested), 2
 * reserved.  Total length, or 0 if it does not fit */
#define QMAP5_CSUM_HDR	4
static inline size_t qmap5_frame(uint8_t *out, size_t cap, const uint8_t *ip, size_t len, uint8_t mux)
{
	if (len == 0 || len > 0xffff || len + QMAP_HDR + QMAP5_CSUM_HDR > cap)
		return 0;
	out[0] = 0x40;
	out[1] = mux;
	out[2] = (uint8_t)(len >> 8);
	out[3] = (uint8_t)len;
	out[4] = 2u << 1;
	out[5] = 0;
	out[6] = 0;
	out[7] = 0;
	memcpy(out + QMAP_HDR + QMAP5_CSUM_HDR, ip, len);
	return len + QMAP_HDR + QMAP5_CSUM_HDR;
}

struct qmap_stats {
	unsigned pkts, cmds, bad;	/* data frames handed on, command frames skipped, buffers cut short */
};

/* an aggregated receive buffer: each data frame's IP packet (padding stripped) to cb; command frames skipped */
static inline void qmap_deagg(const uint8_t *b, size_t len, void (*cb)(void *ctx, uint8_t mux, const uint8_t *p, size_t n),
			      void *ctx, struct qmap_stats *st)
{
	size_t at = 0;
	while (at + QMAP_HDR <= len) {
		unsigned pad = b[at] & 0x3f, cmd = b[at] & 0x80, mux = b[at + 1];
		size_t plen = (size_t)b[at + 2] << 8 | b[at + 3];
		if (plen == 0 && !cmd && !pad && mux == 0)
			break;		/* zero fill after the last frame */
		/* QMAP v5 data frame: the next-header bit (6) says a 4-byte v5 header follows, not counted in pkt_len
		 * (Linux rmnet_map_validate_packet_len); bit 6 is 0 in QMAP v1, so v1 frames are unaffected */
		size_t xh = (!cmd && (b[at] & 0x40)) ? QMAP5_CSUM_HDR : 0;
		if (plen == 0 || at + QMAP_HDR + xh + plen > len || pad > plen) {
			st->bad++;
			break;
		}
		if (cmd)
			st->cmds++;
		else {
			st->pkts++;
			cb(ctx, (uint8_t)mux, b + at + QMAP_HDR + xh, plen - pad);
		}
		at += QMAP_HDR + xh + plen;
	}
}

#endif
