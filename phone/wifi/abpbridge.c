/*
 * abpbridge (P1 of the real CommCenter, Claude 2026-11-09): the userland end of kernel 1061's /dev/abpbridge.
 * Apple's CommCenter (its in-process AppleBasebandManager) talks QMI to "the baseband" over two AppleBasebandPCI
 * pipes, qmux1 and qmux2, each one whole QMUX stream (01 | len16 | flags | svc | cid | SDU). The kernel stand-in
 * hands every frame CommCenter writes to us and turns our frames into completions of CommCenter's reads.
 *
 * Off by default: only with the word abpbridge=1 in /var/local/cell.on (re-read every 10 s; without it the device
 * is closed). P1 stage: NOTHING goes to the modem. Every frame is logged both ways (hex + decode; a frame that starts
 * with a mav_mux header A5 EF BE is flagged), CTL (service 0) is answered here (client ids per service, Sync, data
 * format, power save, version info), every other request gets a QMI error (abperr=N, default 0x5e NOT_SUPPORTED),
 * so the conversation can be observed safely. "abp:" lines in wland's log; the full hex also in ABP_LOG.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "abpbridge.h"
#include "cell.h"
#include "link.h"

#define ABP_DEV		"/dev/abpbridge"
#define ABP_LOG		"/var/local/abpbridge.log"
#define ABPB_MAGIC	0xAB50
#define ABP_MAXFRAME	65536
#define NPIPES		2
#define QMI_ERR_NOT_SUPPORTED	0x5e

struct abpb_hdr {
	uint16_t magic;
	uint8_t pipe;
	uint8_t type;
	uint32_t len;
} __attribute__((packed));

static const char *pipe_name[NPIPES] = { "qmux1", "qmux2" };
static int fd = -1, want, err_code = QMI_ERR_NOT_SUPPORTED;
static uint64_t next_check, t0;
static FILE *lf;
static uint8_t rbuf[sizeof(struct abpb_hdr) * 8 + ABP_MAXFRAME * 2];
static uint8_t cids[256][32];		/* [svc][cid] in use (cid 1..255 as bits) */
static unsigned nframes_in, nframes_out, nseen;
static struct { uint8_t svc; uint16_t msg; } seen[512];

static const char *svc_name(unsigned s)
{
	switch (s) {
	case 0x00: return "CTL"; case 0x01: return "WDS"; case 0x02: return "DMS"; case 0x03: return "NAS";
	case 0x04: return "QOS"; case 0x05: return "WMS"; case 0x06: return "PDS"; case 0x08: return "AT";
	case 0x09: return "VOICE"; case 0x0a: return "CAT"; case 0x0b: return "UIM"; case 0x0c: return "PBM";
	case 0x10: return "LOC"; case 0x1a: return "WDA"; case 0x22: return "COEX"; case 0x24: return "PDC";
	case 0x29: return "RFRPE"; case 0x2a: return "DSD"; case 0x2b: return "SSCTL"; case 0x2c: return "MFSE";
	case 0x2f: return "DPM"; case 0x30: return "DFS"; case 0x52: return "MS";
	case 0xe1: return "AUDIO(apple)"; case 0xe2: return "BSP(apple)"; case 0xe3: return "CIQ(apple)";
	case 0xe4: return "AWD(apple)"; case 0xe5: return "VINYL(apple)"; case 0xe6: return "MAVIMS(apple)";
	case 0xe7: return "ELQM(apple)"; case 0xe8: return "P2P(apple)"; case 0xe9: return "APPS(apple)";
	case 0xea: return "SFT(apple)";
	}
	return "?";
}

static const char *ctl_name(unsigned m)
{
	switch (m) {
	case 0x20: return "SetInstanceId"; case 0x21: return "GetVersionInfo"; case 0x22: return "GetClientId";
	case 0x23: return "ReleaseClientId"; case 0x24: return "RevokeClientIdInd"; case 0x25: return "InvalidClientIdInd";
	case 0x26: return "SetDataFormat"; case 0x27: return "Sync"; case 0x28: return "RegPwrSaveMode";
	case 0x29: return "ConfigPwrSaveSettings"; case 0x2a: return "SetPwrSaveMode"; case 0x2b: return "GetPwrSaveMode";
	}
	return "?";
}

static double secs(void)
{
	return (double)(link_ms() - t0) / 1000.0;
}

static void hexline(char *out, size_t outlen, const uint8_t *p, uint32_t n)
{
	size_t o = 0;
	for (uint32_t i = 0; i < n && o + 4 < outlen; i++)
		o += (size_t)snprintf(out + o, outlen - o, "%02x%s", p[i], (i + 1) % 4 == 0 && i + 1 < n ? " " : "");
	out[o] = 0;
}

/* the TLVs of an SDU, "t[len]=hex ..." (values cut at 16 bytes) */
static void tlvs(char *out, size_t outlen, const uint8_t *p, uint32_t n)
{
	size_t o = 0;
	out[0] = 0;
	while (n >= 3 && o + 8 < outlen) {
		unsigned t = p[0], l = p[1] | (p[2] << 8);
		if (l > n - 3) {
			o += (size_t)snprintf(out + o, outlen - o, " BAD-TLV");
			break;
		}
		char h[64];
		hexline(h, sizeof(h), p + 3, l < 16 ? l : 16);
		o += (size_t)snprintf(out + o, outlen - o, " %02x[%u]=%s%s", t, l, h, l > 16 ? ".." : "");
		p += 3 + l;
		n -= 3 + l;
	}
}

static void note_seen(const char *pn, unsigned svc, unsigned msg)
{
	for (unsigned i = 0; i < nseen; i++)
		if (seen[i].svc == svc && seen[i].msg == msg)
			return;
	if (nseen < sizeof(seen) / sizeof(seen[0])) {
		seen[nseen].svc = (uint8_t)svc;
		seen[nseen].msg = (uint16_t)msg;
		nseen++;
	}
	printf("[%6llu] abp: NEW #%u t+%.1fs %s svc 0x%02x %s msg 0x%04x%s%s\n", (unsigned long long)link_ms(), nseen, secs(), pn,
	       svc, svc_name(svc), msg, svc == 0 ? " " : "", svc == 0 ? ctl_name(msg) : "");
}

/* log one frame; dir "CC->" (CommCenter wrote it) or "->CC" (our answer) */
static void log_frame(unsigned pipe, const char *dir, const uint8_t *f, uint32_t n)
{
	char h[3 * 96 + 8], t[600];
	const char *pn = pipe < NPIPES ? pipe_name[pipe] : "?";
	hexline(h, sizeof(h), f, n < 80 ? n : 80);
	if (lf) {
		char full[3 * 2048];
		hexline(full, sizeof(full), f, n < 1500 ? n : 1500);
		fprintf(lf, "%.3f %s %s len %u: %s\n", secs(), pn, dir, n, full);
		fflush(lf);
	}
	if (n >= 3 && f[0] == 0xa5 && f[1] == 0xef && f[2] == 0xbe) {
		printf("[%6llu] abp: %s %s len %u MAV_MUX header (A5 EF BE): %s\n", (unsigned long long)link_ms(), pn, dir, n, h);
		return;
	}
	if (n < 6 || f[0] != 0x01) {
		printf("[%6llu] abp: %s %s len %u NOT QMUX: %s\n", (unsigned long long)link_ms(), pn, dir, n, h);
		return;
	}
	unsigned qlen = f[1] | (f[2] << 8), flags = f[3], svc = f[4], cid = f[5];
	const uint8_t *s = f + 6;
	uint32_t sn = n - 6;
	if (svc == 0 && sn >= 6) {
		unsigned sf = s[0], tx = s[1], msg = s[2] | (s[3] << 8), ml = s[4] | (s[5] << 8);
		tlvs(t, sizeof(t), s + 6, sn - 6 < ml ? sn - 6 : ml);
		printf("[%6llu] abp: %s %s CTL %s(0x%02x) %s tx %u len %u qlen %u qflags 0x%02x:%s\n", (unsigned long long)link_ms(), pn,
		       dir, ctl_name(msg), msg, sf == 0 ? "req" : sf == 1 ? "resp" : sf == 2 ? "ind" : "?", tx, n, qlen, flags, t);
		if (dir[0] == 'C')
			note_seen(pn, 0, msg);
	} else if (sn >= 7) {
		unsigned sf = s[0], tx = s[1] | (s[2] << 8), msg = s[3] | (s[4] << 8), ml = s[5] | (s[6] << 8);
		tlvs(t, sizeof(t), s + 7, sn - 7 < ml ? sn - 7 : ml);
		printf("[%6llu] abp: %s %s %s(0x%02x) cid %u msg 0x%04x %s tx %u len %u qflags 0x%02x:%s\n", (unsigned long long)link_ms(),
		       pn, dir, svc_name(svc), svc, cid, msg, sf == 0 ? "req" : sf == 2 ? "resp" : sf == 4 ? "ind" : "?", tx, n, flags, t);
		if (dir[0] == 'C')
			note_seen(pn, svc, msg);
	} else {
		printf("[%6llu] abp: %s %s short QMUX len %u: %s\n", (unsigned long long)link_ms(), pn, dir, n, h);
	}
}

static int send_frame(unsigned pipe, const uint8_t *f, uint32_t n)
{
	uint8_t b[sizeof(struct abpb_hdr) + 1024];
	struct abpb_hdr h = { ABPB_MAGIC, (uint8_t)pipe, 0, n };
	if (fd < 0 || n > sizeof(b) - sizeof(h))
		return -1;
	memcpy(b, &h, sizeof(h));
	memcpy(b + sizeof(h), f, n);
	log_frame(pipe, "->CC", f, n);
	nframes_out++;
	if (write(fd, b, sizeof(h) + n) != (ssize_t)(sizeof(h) + n)) {
		printf("[%6llu] abp: write to %s failed: %s\n", (unsigned long long)link_ms(), ABP_DEV, strerror(errno));
		return -1;
	}
	return 0;
}

/* append TLV t to buf at *o */
static void put_tlv(uint8_t *b, uint32_t *o, unsigned t, const void *v, unsigned l)
{
	b[(*o)++] = (uint8_t)t;
	b[(*o)++] = (uint8_t)l;
	b[(*o)++] = (uint8_t)(l >> 8);
	memcpy(b + *o, v, l);
	*o += l;
}

static void finish_qmux(uint8_t *b, uint32_t n, unsigned svc, unsigned cid)
{
	b[0] = 0x01;
	b[1] = (uint8_t)((n - 1) & 0xff);
	b[2] = (uint8_t)((n - 1) >> 8);
	b[3] = 0x80;			/* from the service */
	b[4] = (uint8_t)svc;
	b[5] = (uint8_t)cid;
}

static int cid_alloc(unsigned svc)
{
	for (unsigned c = 1; c < 255; c++)
		if (!(cids[svc][c / 8] & (1u << (c % 8)))) {
			cids[svc][c / 8] |= (uint8_t)(1u << (c % 8));
			return (int)c;
		}
	return -1;
}

/* CTL request: answered here */
static void ctl_answer(unsigned pipe, const uint8_t *s, uint32_t sn)
{
	unsigned tx = s[1], msg = s[2] | (s[3] << 8), ml = s[4] | (s[5] << 8);
	const uint8_t *tl = s + 6;
	uint32_t tn = sn - 6 < ml ? sn - 6 : ml;
	uint8_t b[600];
	uint32_t o = 12;		/* QMUX 6 + CTL SDU header 6 */
	uint16_t res[2] = { 0, 0 };	/* result success, error none */
	int req_svc = -1;
	for (uint32_t i = 0; i + 3 <= tn;) {	/* TLV 0x01 = service (GetClientId) or {svc, cid} (ReleaseClientId) */
		unsigned t = tl[i], l = tl[i + 1] | (tl[i + 2] << 8);
		if (t == 0x01 && l >= 1 && i + 3 + l <= tn)
			req_svc = tl[i + 3];
		if (t == 0x01 && msg == 0x23 && l >= 2 && i + 3 + l <= tn) {
			unsigned sv = tl[i + 3], c = tl[i + 4];
			cids[sv][c / 8] &= (uint8_t) ~(1u << (c % 8));
		}
		i += 3 + l;
	}
	if (msg == 0x22) {		/* GetClientId */
		int c = req_svc >= 0 ? cid_alloc((unsigned)req_svc) : -1;
		if (c < 0) {
			res[0] = 1;
			res[1] = 0x05;	/* QMI_ERR_CLIENT_IDS_EXHAUSTED */
			put_tlv(b, &o, 0x02, res, 4);
		} else {
			put_tlv(b, &o, 0x02, res, 4);
			uint8_t v[2] = { (uint8_t)req_svc, (uint8_t)c };
			put_tlv(b, &o, 0x01, v, 2);
			printf("[%6llu] abp: %s gave %s(0x%02x) client id %d\n", (unsigned long long)link_ms(), pipe_name[pipe],
			       svc_name((unsigned)req_svc), req_svc, c);
		}
	} else if (msg == 0x23) {	/* ReleaseClientId: echo {svc, cid} */
		put_tlv(b, &o, 0x02, res, 4);
		for (uint32_t i = 0; i + 3 <= tn;) {
			unsigned t = tl[i], l = tl[i + 1] | (tl[i + 2] << 8);
			if (t == 0x01 && l == 2 && i + 5 <= tn)
				put_tlv(b, &o, 0x01, tl + i + 3, 2);
			i += 3 + l;
		}
	} else if (msg == 0x21) {	/* GetVersionInfo: the standard services the SM7250 modem has */
		static const uint8_t list[] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x09, 0x0a, 0x0b, 0x0c, 0x1a, 0x22, 0x24, 0x2a, 0x2b, 0x30 };
		uint8_t v[1 + sizeof(list) * 5];
		v[0] = (uint8_t)sizeof(list);
		for (unsigned i = 0; i < sizeof(list); i++) {
			v[1 + i * 5] = list[i];
			v[2 + i * 5] = 1;	/* major 1 */
			v[3 + i * 5] = 0;
			v[4 + i * 5] = 0x40;	/* minor 64 */
			v[5 + i * 5] = 0;
		}
		put_tlv(b, &o, 0x02, res, 4);
		put_tlv(b, &o, 0x01, v, sizeof(v));
	} else if (msg == 0x26) {	/* SetDataFormat: echo the link protocol TLV 0x10 if any */
		put_tlv(b, &o, 0x02, res, 4);
		for (uint32_t i = 0; i + 3 <= tn;) {
			unsigned t = tl[i], l = tl[i + 1] | (tl[i + 2] << 8);
			if (t == 0x10 && i + 3 + l <= tn && l <= 4)
				put_tlv(b, &o, 0x10, tl + i + 3, l);
			i += 3 + l;
		}
	} else {			/* Sync, power save, instance id, anything else: success */
		put_tlv(b, &o, 0x02, res, 4);
	}
	b[6] = 0x01;			/* response */
	b[7] = (uint8_t)tx;
	b[8] = (uint8_t)msg;
	b[9] = (uint8_t)(msg >> 8);
	b[10] = (uint8_t)((o - 12) & 0xff);
	b[11] = (uint8_t)((o - 12) >> 8);
	finish_qmux(b, o, 0, 0);
	send_frame(pipe, b, o);
}

/* any other service's request: a QMI error, nothing goes to the modem (P1) */
static void svc_reject(unsigned pipe, unsigned svc, unsigned cid, const uint8_t *s)
{
	uint8_t b[32];
	uint32_t o = 13;		/* QMUX 6 + SDU header 7 */
	uint16_t res[2] = { 1, (uint16_t)err_code };
	put_tlv(b, &o, 0x02, res, 4);
	b[6] = 0x02;			/* response */
	b[7] = s[1];			/* txid u16 */
	b[8] = s[2];
	b[9] = s[3];			/* msg id */
	b[10] = s[4];
	b[11] = (uint8_t)(o - 13);
	b[12] = 0;
	finish_qmux(b, o, svc, cid);
	send_frame(pipe, b, o);
}

static void handle(unsigned pipe, unsigned type, const uint8_t *d, uint32_t n)
{
	if (pipe >= NPIPES)
		return;
	if (type != 0) {
		static const char *ev[] = { "data", "OPEN", "CLOSE", "START", "ABORT" };
		unsigned long long v = 0;
		if (n >= 8)
			memcpy(&v, d, 8);
		printf("[%6llu] abp: t+%.1fs %s %s (arg 0x%llx)\n", (unsigned long long)link_ms(), secs(), pipe_name[pipe],
		       type < 5 ? ev[type] : "?", v);
		if (lf) {
			fprintf(lf, "%.3f %s event %s arg 0x%llx\n", secs(), pipe_name[pipe], type < 5 ? ev[type] : "?", v);
			fflush(lf);
		}
		return;
	}
	nframes_in++;
	log_frame(pipe, "CC->", d, n);
	if (n < 6 || d[0] != 0x01)
		return;			/* not plain QMUX: logged, not answered */
	unsigned svc = d[4], cid = d[5];
	const uint8_t *s = d + 6;
	uint32_t sn = n - 6;
	if (svc == 0) {
		if (sn >= 6 && s[0] == 0x00)
			ctl_answer(pipe, s, sn);
	} else if (sn >= 7 && s[0] == 0x00) {
		svc_reject(pipe, svc, cid, s);
	}
}

static void read_word(void)
{
	char b[1024] = "";
	FILE *f = fopen(CELL_ON, "r");
	want = 0;
	if (f) {
		size_t k = fread(b, 1, sizeof(b) - 1, f);
		fclose(f);
		b[k] = 0;
		const char *w = strstr(b, "abpbridge=");
		want = w && (w == b || w[-1] == ' ' || w[-1] == '\n') ? atoi(w + 10) : 0;
		w = strstr(b, "abperr=");
		err_code = w ? (int)strtol(w + 7, NULL, 0) : QMI_ERR_NOT_SUPPORTED;
	}
}

void abpbridge_poll(void)
{
	uint64_t now = link_ms();
	if (now >= next_check) {
		next_check = now + 10000;
		read_word();
		if (want && fd < 0) {
			fd = open(ABP_DEV, O_RDWR | O_NONBLOCK);
			if (fd < 0) {
				static int told;
				if (!told++)
					printf("[%6llu] abp: abpbridge=1 but %s: %s (kernel 1061 with sweetabp=1?)\n",
					       (unsigned long long)now, ABP_DEV, strerror(errno));
			} else {
				t0 = now;
				memset(cids, 0, sizeof(cids));
				nseen = 0;
				if (!lf)
					lf = fopen(ABP_LOG, "a");
				if (lf) {
					fprintf(lf, "---- open at wland %llu ms, abperr 0x%x\n", (unsigned long long)now, err_code);
					fflush(lf);
				}
				printf("[%6llu] abp: %s open (P1: CTL answered here, other services get QMI error 0x%x, nothing to the modem)\n",
				       (unsigned long long)now, ABP_DEV, err_code);
			}
		} else if (!want && fd >= 0) {
			close(fd);
			fd = -1;
			printf("[%6llu] abp: abpbridge word gone: %s closed (%u frames in, %u out)\n", (unsigned long long)now,
			       ABP_DEV, nframes_in, nframes_out);
		}
	}
	if (fd < 0)
		return;
	for (int round = 0; round < 8; round++) {
		ssize_t r = read(fd, rbuf, sizeof(rbuf));
		if (r <= 0) {
			if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
				printf("[%6llu] abp: read: %s\n", (unsigned long long)now, strerror(errno));
			return;
		}
		uint32_t off = 0;
		while (off + sizeof(struct abpb_hdr) <= (uint32_t)r) {
			struct abpb_hdr h;
			memcpy(&h, rbuf + off, sizeof(h));
			if (h.magic != ABPB_MAGIC || h.len > (uint32_t)r - off - sizeof(h)) {
				printf("[%6llu] abp: bad record at %u (magic 0x%x len %u)\n", (unsigned long long)now, off, h.magic, h.len);
				return;
			}
			handle(h.pipe, h.type, rbuf + off + sizeof(h), h.len);
			off += (uint32_t)sizeof(h) + h.len;
		}
	}
}
