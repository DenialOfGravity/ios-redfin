/*
 * ipaqmi: the modem-side IPA handshake over QMI (cellular data design D2/D3, step X4). Protocol code only: no IPA
 * register access, no SMP2P "ipa" entry, no SMEM item. Those are the IPA bring-up steps (B, C, D1) and must come
 * first on a real run (design risk 1: the modem must never be told about an IPA the AP has not clocked).
 *
 * Off by default. Words in /var/local/cell.on (re-read every 10 s, like cell.c):
 *   ipaqmi=1   announce QMI service 0x31 instance 1 (the AP "IPA host" service, at 1:IPAQMI_PORT) and answer the
 *              modem's requests; open IPAQMI_SOCK for `wland ipa init-driver`. INIT_DRIVER is never sent by itself.
 *   ipaqmi=2   as 1, plus ONE INIT_DRIVER to the modem's 0x31 instance 2, once per wland process, as soon as that
 *              server is known and 15 s after the Wi-Fi bring-up (ipaqmi_allow; or 3 min after start).
 *   ipasmem=N  (optional, hex or decimal) the IPA's SHARED_MEM_BADDR in bytes (downstream smem_restricted_bytes,
 *              mainline ipa->mem_offset), added to every offset in INIT_DRIVER. Default 0. It is a register value
 *              (IPA_SHARED_MEM_SIZE), only readable with IP0 voted (X2): read it there and put it here.
 *   ipa=1      (wland.c, IPA auto) the IPA bring-up sends the one INIT_DRIVER itself, after the IPA is clocked,
 *              loaded and set up and SMP2P "ipa" = 3 is out (ipaqmi_ipa_on / ipaqmi_ipa_send): ipaqmi is switched on
 *              (as ipaqmi=1, whatever cell.on says, for the rest of this wland run) and ipaqmi=2's own send is not made.
 * No word, or ipaqmi=0: nothing is announced (a server announced earlier is withdrawn with DEL_SERVER), the socket is
 * closed, nothing is answered.
 *
 * Messages (IDs: mainline drivers/net/ipa/ipa_qmi_msg.h and downstream include/uapi/linux/ipa_qmi_service_v01.h;
 * TLV layouts: mainline ipa_qmi_msg.c ipa_init_modem_driver_req_ei, downstream ipa_qmi_service_v01.c):
 *   modem -> us (server, 1:IPAQMI_PORT), each answered from that port with result TLV 0x02 = success:
 *     0x20 INDICATION_REGISTER         (0x10 master_driver_init_complete u8, 0x11 quota, 0x12 mhi, 0x13 ep desc, 0x14 bw)
 *     0x35 INIT_MODEM_DRIVER_CMPLT     (0x01 status u8) -- mainline "DRIVER_INIT_COMPLETE", sets its uc_ready
 *     0x27 CONFIG                      (0x10.. 0x1e: logged only)
 *     0x23 INSTALL_FILTER_RULE         (0x15 filter_spec_ex_list: rule ids echoed in response TLV 0x11, see below)
 *     0x24 FILTER_INSTALLED_NOTIF      (logged only)
 *     anything else: logged with its TLVs, NOT answered (Linux qmi_interface drops requests it has no handler for;
 *     Android's and mainline's IPA servers have no other handler that applies here: 0x3b/0x3d are MHI-prime only).
 *   us -> modem:
 *     0x21 INIT_DRIVER (request, from 1:IPAQMI_CLIENT_PORT to the modem's 0x31/2): only by `wland ipa init-driver` or
 *          ipaqmi=2. TLVs from ipa47_mem[] (below). The exact bytes are logged before sending. Never repeated: a
 *          missing answer is logged after 30 s, nothing is resent.
 *     0x22 INIT_COMPLETE (indication, from the server port, txn 0, result TLV 0x02 = success): once per modem session,
 *          when all three are true (mainline ipa_qmi_ready): the modem registered (0x20), it sent 0x35, and it
 *          accepted our INIT_DRIVER. Sent to the address the 0x20 came from (downstream client_sq; mainline uses the
 *          modem's server address, logged alongside).
 *
 * Log lines: "ipaqmi: < msg 0xNN len N type T txn X from N:P: [tlv/len hex..]..." for every request, "ipaqmi: > ..." for
 * what we send.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include "ipaqmi.h"
#include "cell.h"
#include "link.h"
#include "qmi.h"

#define MSG_INDICATION_REGISTER		0x20
#define MSG_INIT_DRIVER			0x21
#define MSG_INIT_COMPLETE		0x22
#define MSG_INSTALL_FILTER_RULE		0x23
#define MSG_FILTER_INSTALLED_NOTIF	0x24
#define MSG_CONFIG			0x27
#define MSG_INIT_MODEM_DRIVER_CMPLT	0x35

#define PLATFORM_MSM_ANDROID	3	/* ipa_platform_type / QMI_IPA_PLATFORM_TYPE_MSM_ANDROID_V01 (Android's log: 3) */
#define CTRL_EP_APPS_WAN_CONS	16	/* downstream ipa_ep_mapping[IPA_4_7][IPA_CLIENT_APPS_WAN_CONS] = { 16, 7, ...}:
					 * pipe 16 (GSI ch 7); mainline v4.7 AP_MODEM_RX ep 16 too */

/*
 * IPA v4.7 local memory as the Pixel 5's own kernel lays it out: ipa_4_7_mem_part in LineageOS
 * android_kernel_google_redbull drivers/platform/msm/ipa/ipa_v3/ipa_utils.c (l.3163). NOT mainline's v4.7 layout
 * (ipa_data-v4.7.c puts the modem region at 0x1f18/0x100c): the modem firmware expects the downstream one (design
 * finding 3, risk 5). Offsets are bytes from the start of IPA shared memory, before SHARED_MEM_BADDR is added.
 */
enum {
	M_UC_INFO, M_V4_FLT_HASH, M_V4_FLT_NHASH, M_V6_FLT_HASH, M_V6_FLT_NHASH, M_V4_RT_HASH, M_V4_RT_NHASH,
	M_V6_RT_HASH, M_V6_RT_NHASH, M_MODEM_HDR, M_APPS_HDR, M_MODEM_HDR_PROC_CTX, M_APPS_HDR_PROC_CTX, M_NAT_TBL,
	M_PDN_CONFIG, M_STATS_QUOTA_Q6, M_STATS_QUOTA_AP, M_STATS_TETHERING, M_STATS_DROP, M_MODEM_COMP_DECOMP, M_MODEM,
	M_END, M_COUNT
};
static const struct { uint32_t ofst, size; const char *name; } ipa47_mem[M_COUNT] = {
	[M_UC_INFO]		= { 0x0080, 0x0200, "uc_info" },		/* uc_info_ofst/size: microcontroller shared area (ofst_start 0x280 follows) */
	[M_V4_FLT_HASH]		= { 0x0288, 0x0078, "v4_flt_hash" },		/* v4_flt_hash_ofst/size (ddr 0x4000): hashed IPv4 filter table -> TLV 0x1d */
	[M_V4_FLT_NHASH]	= { 0x0308, 0x0078, "v4_flt_nhash" },		/* v4_flt_nhash_ofst/size: non-hashed IPv4 filters -> TLV 0x14 */
	[M_V6_FLT_HASH]		= { 0x0388, 0x0078, "v6_flt_hash" },		/* v6_flt_hash_ofst/size: hashed IPv6 filters -> TLV 0x1e */
	[M_V6_FLT_NHASH]	= { 0x0408, 0x0078, "v6_flt_nhash" },		/* v6_flt_nhash_ofst/size: non-hashed IPv6 filters -> TLV 0x15 */
	[M_V4_RT_HASH]		= { 0x0488, 0x0078, "v4_rt_hash" },		/* v4_rt_hash_ofst/size: hashed IPv4 routes -> TLV 0x1b */
	[M_V4_RT_NHASH]		= { 0x0508, 0x0078, "v4_rt_nhash" },		/* v4_rt_nhash_ofst/size: non-hashed IPv4 routes -> TLV 0x12 */
	[M_V6_RT_HASH]		= { 0x0588, 0x0078, "v6_rt_hash" },		/* v6_rt_hash_ofst/size: hashed IPv6 routes -> TLV 0x1c */
	[M_V6_RT_NHASH]		= { 0x0608, 0x0078, "v6_rt_nhash" },		/* v6_rt_nhash_ofst/size: non-hashed IPv6 routes -> TLV 0x13 */
	[M_MODEM_HDR]		= { 0x0688, 0x0240, "modem_hdr" },		/* modem_hdr_ofst/size: the modem's header table -> TLV 0x11 */
	[M_APPS_HDR]		= { 0x08c8, 0x0200, "apps_hdr" },		/* apps_hdr_ofst/size (ddr 0x800): ours, not sent */
	[M_MODEM_HDR_PROC_CTX]	= { 0x0ad0, 0x0ac0, "modem_hdr_proc_ctx" },	/* modem_hdr_proc_ctx_ofst/size -> TLV 0x19 */
	[M_APPS_HDR_PROC_CTX]	= { 0x1590, 0x0200, "apps_hdr_proc_ctx" },	/* apps_hdr_proc_ctx_ofst/size: ours, not sent */
	[M_NAT_TBL]		= { 0x17a0, 0x0d00, "nat_tbl" },		/* nat_tbl_ofst/size: not sent */
	[M_PDN_CONFIG]		= { 0x24a8, 0x0050, "pdn_config" },		/* pdn_config_ofst/size: not sent */
	[M_STATS_QUOTA_Q6]	= { 0x2500, 0x0030, "stats_quota_q6" },		/* stats_quota_q6_ofst/size -> TLVs 0x1f/0x20 */
	[M_STATS_QUOTA_AP]	= { 0x2530, 0x0048, "stats_quota_ap" },		/* stats_quota_ap_ofst/size: ours, not sent */
	[M_STATS_TETHERING]	= { 0x2578, 0x0238, "stats_tethering" },	/* stats_tethering_ofst/size: not sent (flt/rt/fnr stats are 0 on 4.7) */
	[M_STATS_DROP]		= { 0x27b0, 0x0020, "stats_drop" },		/* stats_drop_ofst/size -> TLVs 0x21/0x22 */
	[M_MODEM_COMP_DECOMP]	= { 0x0000, 0x0000, "modem_comp_decomp" },	/* modem_comp_decomp_ofst/size 0: TLV 0x1a (zip) left out */
	[M_MODEM]		= { 0x27d8, 0x0800, "modem" },			/* modem_ofst/size: the modem's own region -> TLV 0x16 */
	[M_END]			= { 0x3000, 0x0000, "end" },			/* end_ofst (uc_descriptor_ram 0x3000/0) */
};
#define IPA47_MODEM_RT_INDEX_HI	7	/* v4_/v6_modem_rt_index_hi (lo 0, apps 8..14 of 15): sent as num_indices, as
					 * downstream does (mainline: modem_route_count - 1 = 7) */

/* INSTALL_FILTER_RULE TLV 0x15 (filter_spec_ex_list): u8 count, then count x ipa_filter_spec_ex_type_v01 as
 * ipa_qmi_service_v01.c encodes it, packed, fixed-length arrays: ip_type u32 (0) + filter_rule 150 (4) + action u32
 * (154) + rt_valid u8 (158) + rt_index u32 (159) + mux_valid u8 (163) + mux_id u32 (164) + rule_id u32 (168) +
 * hashable u8 (172) = 173. filter_rule (ipa_filter_rule_type_v01): bitmap u16, tos 1+1, proto 1+1, 1 + 2 x range16
 * (5), 1 + 2 x meq32 (9), tc 1+1, flow 1+4, 1 + eq16 (3), 1 + eq32 (5), 1 + 2 x meq32, 1 + 2 x meq128 (33), 1 +
 * meq32, frag 1 = 150. So rule_id is at 168; 163 is is_mux_id_valid (HakiOS cycle 28 echoed from 163 and the modem
 * asserted in ipa_ipfltr.c; hakios.mpss.c l.4793 settled on 168). */
#define FSPEC_EX_SIZE		173
#define FSPEC_EX_RULE_ID	168
#define MAX_FILTERS		64	/* QMI_IPA_MAX_FILTERS_V01: the array's bound */
#define MAX_Q6_RULES		35	/* downstream ipa_i.h MAX_NUM_Q6_RULE: at most this many ids answered */

#define CHECK_MS	10000
#define ALLOW_AFTER_MS	15000
#define ALLOW_LATEST_MS	180000
#define REPLY_MS	30000		/* INIT_DRIVER: logged as unanswered after this; never resent */
#define CLI_REQ_MS	2000

static int level, announced, allowed, auto_sent;
static uint32_t smem_base;
static uint64_t next_check, first_poll, allow_ms;
static int m_up;			/* the modem's 0x31 instance 2 */
static uint32_t m_node, m_port;
static int reg_up, reg_want = -1, cmplt_seen, drv_ok, ind_sent;	/* this modem session */
static uint32_t reg_node, reg_port;
static uint16_t txn = 1, pend_txn;
static uint64_t pend_ms;
static int lfd = -1, cfd = -1, c_waiting;	/* IPAQMI_SOCK listener / the one CLI client */
static uint64_t c_dead;
static int ipa_word, forced, ipa2_logged;	/* ipa=1 in cell.on; ipaqmi_ipa_on was called (level >= 1 from then on) */
static uint16_t ipa_txn;			/* the IPA bring-up's INIT_DRIVER */
static int ipa_res;				/* 0 none/pending, 1 accepted, -1 refused, unanswered or server gone */
static uint32_t ipa_ctrl, ipa_def;

#define TS() ((unsigned long long)link_ms())

static void le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* ---- INIT_DRIVER ------------------------------------------------------------------------------------------- */

struct enc { uint8_t *b; size_t cap, n; int bad; };

static void e_tlv(struct enc *e, uint8_t t, const uint8_t *v, uint16_t l)
{
	if (e->bad || e->n + 3 + l > e->cap) {
		e->bad = 1;
		return;
	}
	e->b[e->n] = t;
	e->b[e->n + 1] = (uint8_t)l;
	e->b[e->n + 2] = (uint8_t)(l >> 8);
	memcpy(e->b + e->n + 3, v, l);
	e->n += 3u + l;
}

static void e_u32(struct enc *e, uint8_t t, uint32_t v)
{
	uint8_t w[4];
	le32(w, v);
	e_tlv(e, t, w, 4);
}

static void e_pair(struct enc *e, uint8_t t, uint32_t a, uint32_t b)
{
	uint8_t w[8];
	le32(w, a);
	le32(w + 4, b);
	e_tlv(e, t, w, 8);
}

/* {start, end} of region r, end inclusive (downstream: ofst + size - 1; mainline: start + size - 1) */
static void e_bounds(struct enc *e, uint8_t t, int r, uint32_t base)
{
	e_pair(e, t, base + ipa47_mem[r].ofst, base + ipa47_mem[r].ofst + ipa47_mem[r].size - 1);
}

/*
 * Field order and values as downstream ipa3_qmi_init_modem_send_sync_msg (redbull ipa_qmi_service.c l.503) with
 * ipa_4_7_mem_part, hashing on (lito DT has no qcom,ipa-fltrt-not-hashable), first boot (no 0x18), TLV numbers as
 * mainline ipa_init_modem_driver_req_ei. Wire: TLV = type u8, length u16 LE, value; u32 LE; structs packed.
 */
size_t ipaqmi_encode_init_driver(uint8_t *b, size_t cap, uint16_t t, uint32_t base, int ssr)
{
	struct enc e = { b, cap, 7, cap < 7 };
	if (e.bad)
		return 0;
	b[0] = QMI_REQUEST;
	b[1] = (uint8_t)t;
	b[2] = (uint8_t)(t >> 8);
	b[3] = MSG_INIT_DRIVER;
	b[4] = 0;
	e_u32(&e, 0x10, PLATFORM_MSM_ANDROID);						/* platform_type */
	e_bounds(&e, 0x11, M_MODEM_HDR, base);						/* hdr_tbl_info {start, end} */
	e_pair(&e, 0x12, base + ipa47_mem[M_V4_RT_NHASH].ofst, IPA47_MODEM_RT_INDEX_HI);	/* v4_route_tbl_info {start, max index} */
	e_pair(&e, 0x13, base + ipa47_mem[M_V6_RT_NHASH].ofst, IPA47_MODEM_RT_INDEX_HI);	/* v6_route_tbl_info */
	e_u32(&e, 0x14, base + ipa47_mem[M_V4_FLT_NHASH].ofst);				/* v4_filter_tbl_start */
	e_u32(&e, 0x15, base + ipa47_mem[M_V6_FLT_NHASH].ofst);				/* v6_filter_tbl_start */
	e_pair(&e, 0x16, base + ipa47_mem[M_MODEM].ofst, ipa47_mem[M_MODEM].size);	/* modem_mem_info {start, size} */
	e_u32(&e, 0x17, CTRL_EP_APPS_WAN_CONS);						/* ctrl_comm_dest_end_pt */
	if (ssr) {
		uint8_t one = 1;
		e_tlv(&e, 0x18, &one, 1);						/* is_ssr_bootup / skip_uc_load */
	}
	e_bounds(&e, 0x19, M_MODEM_HDR_PROC_CTX, base);					/* hdr_proc_ctx_tbl_info {start, end} */
	/* 0x1a zip_tbl_info: modem_comp_decomp_size is 0 on 4.7, so not valid (left out) */
	e_pair(&e, 0x1b, base + ipa47_mem[M_V4_RT_HASH].ofst, IPA47_MODEM_RT_INDEX_HI);	/* v4_hash_route_tbl_info */
	e_pair(&e, 0x1c, base + ipa47_mem[M_V6_RT_HASH].ofst, IPA47_MODEM_RT_INDEX_HI);	/* v6_hash_route_tbl_info */
	e_u32(&e, 0x1d, base + ipa47_mem[M_V4_FLT_HASH].ofst);				/* v4_hash_filter_tbl_start */
	e_u32(&e, 0x1e, base + ipa47_mem[M_V6_FLT_HASH].ofst);				/* v6_hash_filter_tbl_start */
	e_u32(&e, 0x1f, base + ipa47_mem[M_STATS_QUOTA_Q6].ofst);			/* hw_stats_quota_base_addr */
	e_u32(&e, 0x20, ipa47_mem[M_STATS_QUOTA_Q6].size);				/* hw_stats_quota_size (a size: no base) */
	e_u32(&e, 0x21, base + ipa47_mem[M_STATS_DROP].ofst);				/* hw_drop_stats_base_addr */
	e_u32(&e, 0x22, ipa47_mem[M_STATS_DROP].size);					/* hw_drop_stats_table_size */
	if (e.bad || e.n - 7 > 0xffff)
		return 0;
	b[5] = (uint8_t)(e.n - 7);
	b[6] = (uint8_t)((e.n - 7) >> 8);
	return e.n;
}

/* ---- logging ------------------------------------------------------------------------------------------------- */

static void hex_line(const char *what, const uint8_t *d, size_t n)
{
	printf("[%6llu] ipaqmi: %s %zu bytes:", TS(), what, n);
	for (size_t i = 0; i < n && i < 512; i++)
		printf(" %02x", d[i]);
	printf("%s\n", n > 512 ? " ..." : "");
}

/* "< msg 0xNN len N": the TLVs, each [type/len hex], at most 48 bytes shown per TLV; bounded by m->len */
static void log_msg(const char *dir, const struct qmi_msg *m, const struct qrtr_hdr *h)
{
	printf("[%6llu] ipaqmi: %s msg 0x%02x len %u type %u txn %u %s %u:%x:", TS(), dir, m->id, m->len, m->type, m->txn,
	       dir[0] == '<' ? "from" : "to", h ? h->src_node : 0, h ? h->src_port : 0);
	uint32_t o = 0;
	while (o + 3 <= m->len) {
		uint16_t l = (uint16_t)(m->tlv[o + 1] | m->tlv[o + 2] << 8);
		uint32_t have = m->len - o - 3;
		printf(" [%#x/%u", m->tlv[o], l);
		for (uint32_t k = 0; k < l && k < have && k < 48; k++)
			printf(" %02x", m->tlv[o + 3 + k]);
		printf("%s]", l > have ? " TRUNCATED" : l > 48 ? " ..." : "");
		if (l > have)
			break;
		o += 3u + l;
	}
	if (o < m->len && o + 3 > m->len)
		printf(" (+%u stray bytes)", m->len - o);
	printf("\n");
}

/* ---- the CLI socket (`wland ipa init-driver`) ------------------------------------------------------------- */

static void cli_say(const char *s)
{
	if (cfd >= 0 && write(cfd, s, strlen(s)) < 0)
		(void)0;
}

static void cli_done(const char *s)
{
	if (cfd < 0)
		return;
	if (s)
		cli_say(s);
	close(cfd);
	cfd = -1;
	c_waiting = 0;
}

static void sock_open(int on)
{
	if (!on) {
		cli_done("ipaqmi: off\n");
		if (lfd >= 0) {
			close(lfd);
			lfd = -1;
			unlink(IPAQMI_SOCK);
		}
		return;
	}
	if (lfd >= 0)
		return;
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", IPAQMI_SOCK);
	unlink(IPAQMI_SOCK);
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	mode_t um = umask(0177);	/* 0600 root: only root may make wland talk to the modem's IPA */
	int bad = fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof(a)) || listen(fd, 1);
	umask(um);
	if (bad) {
		printf("[%6llu] ipaqmi: socket %s: %s\n", TS(), IPAQMI_SOCK, strerror(errno));
		if (fd >= 0)
			close(fd);
		unlink(IPAQMI_SOCK);
		return;
	}
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	lfd = fd;
}

/* ---- sending ---------------------------------------------------------------------------------------------------- */

static int send_from(uint32_t port, uint32_t node, uint32_t dport, const uint8_t *b, size_t n)
{
	return qrtr_send(QRTR_TYPE_DATA, port, node, dport, b, (uint32_t)n);
}

/* one INIT_DRIVER; why = "cli" or "ipaqmi=2". Returns 0 if it went out. */
static int send_init_driver(const char *why)
{
	uint8_t b[7 + 162 + 16];
	if (!m_up) {
		printf("[%6llu] ipaqmi: INIT_DRIVER (%s) not sent: the modem's 0x31/%u is not known\n", TS(), why,
		       IPAQMI_MODEM_INSTANCE);
		return -1;
	}
	if (pend_txn) {
		printf("[%6llu] ipaqmi: INIT_DRIVER (%s) not sent: txn %u still unanswered\n", TS(), why, pend_txn);
		return -1;
	}
	uint16_t t = txn++;
	if (!txn)
		txn = 1;
	size_t n = ipaqmi_encode_init_driver(b, sizeof(b), t, smem_base, 0);
	if (!n) {
		printf("[%6llu] ipaqmi: INIT_DRIVER does not encode\n", TS());
		return -1;
	}
	printf("[%6llu] ipaqmi: > INIT_DRIVER (%s) txn %u to %u:%x, smem base %#x, ipa_4_7_mem_part layout\n", TS(), why, t,
	       m_node, m_port, smem_base);
	hex_line("> INIT_DRIVER", b, n);
	if (send_from(IPAQMI_CLIENT_PORT, m_node, m_port, b, n)) {
		printf("[%6llu] ipaqmi: INIT_DRIVER: link send failed (not repeated)\n", TS());
		return -1;
	}
	pend_txn = t;
	pend_ms = link_ms();
	return 0;
}

static void maybe_init_complete(void)
{
	if (ind_sent || !reg_up || !cmplt_seen || !drv_ok)
		return;
	struct qmi_buf q;
	qmi_start(&q, QMI_INDICATION, 0, MSG_INIT_COMPLETE);
	qmi_tlv_result(&q, 0, 0);
	size_t n = qmi_finish(&q);
	printf("[%6llu] ipaqmi: > INIT_COMPLETE indication to %u:%x (the 0x20 sender; modem server is %u:%x)%s\n", TS(),
	       reg_node, reg_port, m_node, m_port, reg_want == 0 ? " (it registered with master_driver_init_complete=0)" : "");
	hex_line("> INIT_COMPLETE", q.b, n);
	if (send_from(IPAQMI_PORT, reg_node, reg_port, q.b, n))
		printf("[%6llu] ipaqmi: INIT_COMPLETE: link send failed (not repeated)\n", TS());
	ind_sent = 1;		/* once per modem session, sent or not */
}

static void session_reset(void)
{
	reg_up = 0;
	reg_want = -1;
	cmplt_seen = drv_ok = ind_sent = 0;
}

static void announce(int on)
{
	uint32_t pkt[5] = { on ? QRTR_TYPE_NEW_SERVER : QRTR_TYPE_DEL_SERVER, IPAQMI_SERVICE,
			    1u | (uint32_t)IPAQMI_HOST_INSTANCE << 8, QRTR_NODE_APPS, IPAQMI_PORT };
	int r = on ? qrtr_new_server(IPAQMI_SERVICE, 1, IPAQMI_HOST_INSTANCE, IPAQMI_PORT)
		   : qrtr_send(QRTR_TYPE_DEL_SERVER, QRTR_PORT_CTRL, QRTR_NODE_MODEM, QRTR_PORT_CTRL, pkt, sizeof(pkt));
	printf("[%6llu] ipaqmi: %s service %#x instance %u at %u:%x%s\n", TS(), on ? "serving" : "withdrawn", IPAQMI_SERVICE,
	       IPAQMI_HOST_INSTANCE, QRTR_NODE_APPS, IPAQMI_PORT, r ? " (link send failed)" : "");
	announced = on;
}

/* ---- receiving ---------------------------------------------------------------------------------------------- */

static void reply_ok(const struct qrtr_hdr *h, const struct qmi_msg *m)
{
	struct qmi_buf q;
	qmi_start(&q, QMI_RESPONSE, m->txn, m->id);
	qmi_tlv_result(&q, 0, 0);
	send_from(IPAQMI_PORT, h->src_node, h->src_port, q.b, qmi_finish(&q));
}

/* ipanotif=1 (Claude 2026-11-08, opt-in): after answering INSTALL_FILTER_RULE, send the modem FILTER_INSTALLED_NOTIF
 * (0x24, AP -> modem's 0x31/2) as Android's rmnet_ipa.c ipa3_wwan_add_ul_flt_rule_to_ipa() does: 0x01 source pipe u32 =
 * APPS_WAN_PROD (ep 2), 0x02 install status u16 = success, 0x03 filter_index_list = empty (u8 0), 0x17 rule ids (u8
 * count, u32 each) = the ids of the modem's specs.  The modem already put its UL rules in ep2's filter slot itself
 * (ipatbl=1 dump), so only the notification is sent; its answer is logged on the client port. */
static int notif;
static uint16_t notif_txn;

static void send_fltr_notif(const uint8_t *ids, unsigned k)
{
	struct qmi_buf q;
	uint8_t empty = 0;
	if (!m_up) {
		printf("[%6llu] ipaqmi: FILTER_INSTALLED_NOTIF not sent: the modem's 0x31/%u is not known\n", TS(),
		       IPAQMI_MODEM_INSTANCE);
		return;
	}
	notif_txn = txn++;
	if (!txn)
		txn = 1;
	qmi_start(&q, QMI_REQUEST, notif_txn, MSG_FILTER_INSTALLED_NOTIF);
	qmi_tlv_u32(&q, 0x01, 2);
	uint8_t st[2] = { 0, 0 };
	qmi_tlv(&q, 0x02, st, 2);
	qmi_tlv(&q, 0x03, &empty, 1);
	qmi_tlv(&q, 0x17, ids, (uint16_t)(1 + 4 * k));
	size_t n = qmi_finish(&q);
	printf("[%6llu] ipaqmi: > FILTER_INSTALLED_NOTIF txn %u to %u:%x: source pipe 2, success, %u rule ids\n", TS(),
	       notif_txn, m_node, m_port, k);
	hex_line("> FILTER_INSTALLED_NOTIF", q.b, n);
	if (send_from(IPAQMI_CLIENT_PORT, m_node, m_port, q.b, n))
		printf("[%6llu] ipaqmi: FILTER_INSTALLED_NOTIF: link send failed (not repeated)\n", TS());
}

static void on_install_filter(const struct qrtr_hdr *h, const struct qmi_msg *m)
{
	struct qmi_buf q;
	uint16_t l = 0;
	const uint8_t *p = qmi_get(m, 0x15, &l);
	qmi_start(&q, QMI_RESPONSE, m->txn, m->id);
	qmi_tlv_result(&q, 0, 0);
	if (qmi_get(m, 0x16, NULL))
		printf("[%6llu] ipaqmi:   filter rules: 0x16 (filter_spec_ex2_list) present: ids not echoed from it (as downstream)\n",
		       TS());
	if (p) {
		unsigned cnt = l >= 1 ? p[0] : 0;
		if (l < 1 || cnt > MAX_FILTERS || l != 1u + cnt * FSPEC_EX_SIZE) {
			printf("[%6llu] ipaqmi:   filter rules: 0x15 of %u bytes (count %u) is not 1 + count x %u: success alone\n",
			       TS(), l, cnt, FSPEC_EX_SIZE);
		} else {
			uint8_t ids[1 + 4 * MAX_Q6_RULES];
			unsigned k = cnt > MAX_Q6_RULES ? MAX_Q6_RULES : cnt;
			ids[0] = (uint8_t)k;
			for (unsigned r = 0; r < cnt; r++) {
				const uint8_t *s = p + 1 + r * FSPEC_EX_SIZE;
				printf("[%6llu] ipaqmi:   rule %u: ip %u action %u rt %u/%u mux %u/%u id %#x hash %u\n", TS(), r,
				       rd32(s), rd32(s + 154), s[158], rd32(s + 159), s[163], rd32(s + 164),
				       rd32(s + FSPEC_EX_RULE_ID), s[172]);
				if (r < k)
					memcpy(ids + 1 + 4 * r, s + FSPEC_EX_RULE_ID, 4);
			}
			if (cnt > k)
				printf("[%6llu] ipaqmi:   %u rules, ids of the first %u answered (MAX_NUM_Q6_RULE)\n", TS(), cnt, k);
			qmi_tlv(&q, 0x11, ids, (uint16_t)(1 + 4 * k));	/* rule_id list: u8 count, u32 ids */
			if (notif) {
				size_t n0 = qmi_finish(&q);
				hex_line("> INSTALL_FILTER_RULE resp", q.b, n0);
				send_from(IPAQMI_PORT, h->src_node, h->src_port, q.b, n0);
				send_fltr_notif(ids, k);
				return;
			}
		}
	}
	size_t n = qmi_finish(&q);
	hex_line("> INSTALL_FILTER_RULE resp", q.b, n);
	send_from(IPAQMI_PORT, h->src_node, h->src_port, q.b, n);
}

static int noflt;	/* ipanoflt=1: see read_words */

static void on_server_req(const struct qrtr_hdr *h, const struct qmi_msg *m)
{
	uint8_t v = 0;
	switch (m->id) {
	case MSG_INDICATION_REGISTER:
		reply_ok(h, m);
		reg_up = 1;
		reg_node = h->src_node;
		reg_port = h->src_port;
		reg_want = qmi_get_u8(m, 0x10, &v) ? -1 : v;
		printf("[%6llu] ipaqmi:   indication register from %u:%x (master_driver_init_complete %d): answered\n", TS(),
		       reg_node, reg_port, reg_want);
		maybe_init_complete();
		break;
	case MSG_INIT_MODEM_DRIVER_CMPLT:
		reply_ok(h, m);
		cmplt_seen = 1;
		printf("[%6llu] ipaqmi:   modem driver init complete (status %d): answered%s\n", TS(),
		       qmi_get_u8(m, 0x01, &v) ? -1 : v, drv_ok ? "" : " (INIT_DRIVER not accepted yet: no INIT_COMPLETE)");
		maybe_init_complete();
		break;
	case MSG_CONFIG:
	case MSG_FILTER_INSTALLED_NOTIF:
		reply_ok(h, m);
		printf("[%6llu] ipaqmi:   %s: answered with success\n", TS(), m->id == MSG_CONFIG ? "config" : "filters installed");
		break;
	case MSG_INSTALL_FILTER_RULE: {
		/* the whole request's TLVs to /var/local/ipa-ulflt.bin (Claude 2026-11-08): {u8 type, u16 len LE, value}
		 * per TLV, to decode the modem's UL filter specs offline (not a credential or a dump of a partition) */
		FILE *df = fopen("/var/local/ipa-ulflt.bin", "wb");
		if (df) {
			for (unsigned t = 0x10; t <= 0x1f; t++) {
				uint16_t l = 0;
				const uint8_t *v = qmi_get(m, (uint8_t)t, &l);
				if (!v)
					continue;
				uint8_t th[3] = { (uint8_t)t, (uint8_t)l, (uint8_t)(l >> 8) };
				fwrite(th, 1, 3, df);
				fwrite(v, 1, l, df);
			}
			fclose(df);
			printf("[%6llu] ipaqmi:   install filter rule: TLVs saved to /var/local/ipa-ulflt.bin\n", TS());
		}
	}
		if (noflt) {
			printf("[%6llu] ipaqmi:   install filter rule: NOT answered (ipanoflt=1, as mainline)\n", TS());
			break;
		}
		on_install_filter(h, m);
		break;
	default:
		printf("[%6llu] ipaqmi:   request 0x%02x not known here: not answered\n", TS(), m->id);
		break;
	}
}

/* our INIT_DRIVER's answer, or anything else on the client port */
static void on_client(const struct qmi_msg *m)
{
	char out[256];
	if (m->type == QMI_RESPONSE && m->id == MSG_FILTER_INSTALLED_NOTIF && notif_txn && m->txn == notif_txn) {
		uint16_t l = 0;
		const uint8_t *r = qmi_get(m, 0x02, &l);
		printf("[%6llu] ipaqmi: FILTER_INSTALLED_NOTIF -> result %u error %#x\n", TS(),
		       r && l >= 4 ? (unsigned)(r[0] | r[1] << 8) : 0xffff, r && l >= 4 ? (unsigned)(r[2] | r[3] << 8) : 0xffff);
		notif_txn = 0;
		return;
	}
	if (m->type != QMI_RESPONSE || m->id != MSG_INIT_DRIVER || !pend_txn || m->txn != pend_txn) {
		printf("[%6llu] ipaqmi:   not the answer to an INIT_DRIVER we wait for (pending txn %u)\n", TS(), pend_txn);
		return;
	}
	uint16_t l = 0;
	const uint8_t *r = qmi_get(m, 0x02, &l);
	unsigned res = r && l >= 4 ? (unsigned)(r[0] | r[1] << 8) : 0xffff, err = r && l >= 4 ? (unsigned)(r[2] | r[3] << 8) : 0xffff;
	uint32_t ctrl = 0, def = 0;
	uint8_t pend = 0;
	int hc = !qmi_get_u32(m, 0x10, &ctrl), hd = !qmi_get_u32(m, 0x11, &def), hp = !qmi_get_u8(m, 0x12, &pend);
	if (ipa_txn && m->txn == ipa_txn) {
		ipa_res = res == 0 ? 1 : -1;
		ipa_ctrl = hc ? ctrl : 0xffffffffu;
		ipa_def = hd ? def : 0xffffffffu;
		ipa_txn = 0;
	}
	pend_txn = 0;
	drv_ok = res == 0;
	snprintf(out, sizeof(out), "ipaqmi: INIT_DRIVER -> %s result %u error %#x ctrl_ep %s%u default_ep %s%u init_pending %s%u (%llu ms)\n",
		 res == 0 ? "ACCEPTED" : "REFUSED", res, err, hc ? "" : "-", ctrl, hd ? "" : "-", def, hp ? "" : "-", pend,
		 (unsigned long long)(link_ms() - pend_ms));
	printf("[%6llu] %s", TS(), out);
	if (c_waiting)
		cli_done(out);
	maybe_init_complete();
}

void ipaqmi_handle(const struct qrtr_hdr *h, const uint8_t *d, uint32_t n)
{
	struct qmi_msg m;
	if (!level || qmi_parse(d, n, &m))
		return;
	log_msg("<", &m, h);
	if (h->src_node != QRTR_NODE_MODEM) {
		printf("[%6llu] ipaqmi:   not from the modem node: ignored\n", TS());
		return;
	}
	if (h->dst_port == IPAQMI_CLIENT_PORT) {
		on_client(&m);
		return;
	}
	if (m.type == QMI_REQUEST)
		on_server_req(h, &m);
	else
		printf("[%6llu] ipaqmi:   type %u on the server port: ignored\n", TS(), m.type);
}

/* ---- servers coming and going ------------------------------------------------------------------------------ */

void ipaqmi_server(uint32_t service, uint32_t verinst, uint32_t node, uint32_t port)
{
	if (service != IPAQMI_SERVICE || verinst >> 8 != IPAQMI_MODEM_INSTANCE || node != QRTR_NODE_MODEM)
		return;
	m_up = 1;		/* kept even when off, so turning ipaqmi on later knows it; nothing is sent */
	m_node = node;
	m_port = port;
	if (level)
		printf("[%6llu] ipaqmi: modem ipa server 0x31/%u (version %u) at %u:%x\n", TS(), verinst >> 8, verinst & 0xff,
		       node, port);
}

void ipaqmi_server_gone(uint32_t node, uint32_t port)
{
	if (m_up && m_node == node && (port == 0xffffffffu || port == m_port)) {
		m_up = 0;
		if (level)
			printf("[%6llu] ipaqmi: modem ipa server gone%s\n", TS(), pend_txn ? " (INIT_DRIVER unanswered)" : "");
		pend_txn = 0;
		if (ipa_txn) {
			ipa_txn = 0;
			ipa_res = -1;
		}
		if (c_waiting)
			cli_done("ipaqmi: the modem's ipa server went away\n");
		session_reset();	/* its IPA driver restarted or the modem went: a new handshake (mainline ipa_server_bye) */
	} else if (node == QRTR_NODE_MODEM && (port == 0xffffffffu || (reg_up && port == reg_port))) {
		session_reset();
	}
}

/* ---- cell.on ----------------------------------------------------------------------------------------------- */

/* the value of word key= (at the start or after a blank), or -1 */
static long word(const char *b, const char *key)
{
	size_t kl = strlen(key);
	for (const char *s = b; (s = strstr(s, key)); s += kl) {
		if (s != b && s[-1] != ' ' && s[-1] != '\t' && s[-1] != '\n' && s[-1] != '\r')
			continue;
		char *e = NULL;
		long v = strtol(s + kl, &e, 0);
		if (e == s + kl || (*e && *e != ' ' && *e != '\t' && *e != '\n' && *e != '\r'))
			return -1;
		return v;
	}
	return -1;
}

/* ipanoflt=1 (Claude 2026-11-08, opt-in): INSTALL_FILTER_RULE (0x23) from the modem is logged but NOT answered, as
 * mainline's IPA server does (it has no handler for it).  Default: answered with success + the rule ids, which tells
 * the modem its UL rules are on our TX endpoint although nothing installs them. */
static void read_words(int *lv, uint32_t *base)
{
	char b[512] = "";
	*lv = 0;
	*base = 0;
	ipa_word = 0;
	FILE *f = fopen(CELL_ON, "r");
	if (!f)
		return;
	size_t k = fread(b, 1, sizeof(b) - 1, f);
	fclose(f);
	b[k] = 0;
	long v = word(b, "ipaqmi=");
	*lv = v == 1 || v == 2 ? (int)v : 0;
	ipa_word = word(b, "ipa=") == 1;
	noflt = word(b, "ipanoflt=") == 1;
	notif = word(b, "ipanotif=") == 1;
	v = word(b, "ipasmem=");
	*base = v > 0 && v < 0x10000 ? (uint32_t)v : 0;	/* IPA shared memory is 12 KB + base: anything bigger is a typo */
}

static void check_words(void)
{
	int lv;
	uint32_t base;
	read_words(&lv, &base);
	if (forced && !lv)
		lv = 1;			/* the IPA bring-up switched ipaqmi on: kept on (the modem was told about the IPA) */
	if (lv == level && base == smem_base)
		return;
	int was = level;
	level = lv;
	smem_base = base;
	if (lv || was)
		printf("[%6llu] ipaqmi: %s (ipaqmi=%d ipasmem=%#x)%s\n", TS(), lv ? "on" : "off", lv, base,
		       lv == 2 ? (auto_sent ? ": INIT_DRIVER already sent once in this wland run" : ": one INIT_DRIVER when the modem's server is known") : "");
	if (lv && !announced) {
		session_reset();
		announce(1);
		if (m_up)
			printf("[%6llu] ipaqmi: modem ipa server known at %u:%x\n", TS(), m_node, m_port);
	} else if (!lv && announced) {
		announce(0);
		session_reset();
		pend_txn = 0;
	}
	sock_open(lv != 0);
}

void ipaqmi_init(void)
{
	next_check = link_ms() + CHECK_MS;
	check_words();
}

void ipaqmi_allow(void)
{
	if (!allowed) {
		allowed = 1;
		allow_ms = link_ms() + ALLOW_AFTER_MS;
	}
}

static void cli_poll(uint64_t now)
{
	if (lfd < 0)
		return;
	if (cfd < 0) {
		int fd = accept(lfd, NULL, NULL);
		if (fd < 0)
			return;
		uid_t uid = (uid_t)-1;
		gid_t gid = (gid_t)-1;
		if (getpeereid(fd, &uid, &gid) || uid != 0) {
			close(fd);
			return;
		}
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
		fcntl(fd, F_SETFD, FD_CLOEXEC);
		cfd = fd;
		c_waiting = 0;
		c_dead = now + CLI_REQ_MS;
		return;
	}
	if (c_waiting) {
		if (now > pend_ms && now - pend_ms > REPLY_MS + 1000)
			cli_done("ipaqmi: no answer\n");
		return;
	}
	char r[8];
	ssize_t k = read(cfd, r, sizeof(r));
	if (k > 0 && r[0] == 'I') {
		if (send_init_driver("cli")) {
			cli_done(m_up ? "ipaqmi: not sent (one is still unanswered, or the link failed)\n" : "ipaqmi: not sent: the modem's ipa server (0x31/2) is not known\n");
			return;
		}
		cli_say("ipaqmi: INIT_DRIVER sent, waiting (see wland.log for the bytes)\n");
		c_waiting = 1;
		return;
	}
	if (k > 0 || k == 0 || errno != EAGAIN || now > c_dead)
		cli_done("ipaqmi: bad request\n");
}

void ipaqmi_poll(void)
{
	uint64_t now = link_ms();
	if (!first_poll)
		first_poll = now ? now : 1;
	if (!allowed && now - first_poll > ALLOW_LATEST_MS) {
		allowed = 1;
		allow_ms = now;
	}
	if (now >= next_check) {
		next_check = now + CHECK_MS;
		check_words();
	}
	if (!level)
		return;
	cli_poll(now);
	if (pend_txn && now > pend_ms && now - pend_ms > REPLY_MS) {
		printf("[%6llu] ipaqmi: INIT_DRIVER txn %u unanswered in %d s (not resent; a late answer is still logged)\n", TS(),
		       pend_txn, REPLY_MS / 1000);
		pend_txn = 0;
		if (ipa_txn) {
			ipa_txn = 0;
			ipa_res = -1;
		}
		if (c_waiting)
			cli_done("ipaqmi: INIT_DRIVER unanswered in 30 s\n");
	}
	if (level == 2 && ipa_word && !auto_sent) {
		if (!ipa2_logged)
			printf("[%6llu] ipaqmi: ipaqmi=2 with ipa=1: the IPA bring-up sends INIT_DRIVER (not ipaqmi=2)\n", TS());
		ipa2_logged = 1;
		return;
	}
	if (level == 2 && !auto_sent && m_up && allowed && now >= allow_ms && !pend_txn) {
		auto_sent = 1;		/* once per wland process, whatever happens */
		send_init_driver("ipaqmi=2");
	}
}

/* ---- the IPA bring-up (wland.c ipa=1) -------------------------------------------------------------------------- */

int ipaqmi_ipa_on(void)
{
	if (!forced)
		printf("[%6llu] ipaqmi: switched on by the IPA bring-up (ipa=1)\n", TS());
	forced = 1;
	check_words();
	return m_up;
}

int ipaqmi_ipa_send(void)
{
	if (!level || ipa_txn || ipa_res)
		return -1;
	auto_sent = 1;		/* ipaqmi=2's own send is not made after this one either */
	if (send_init_driver("ipa=1"))
		return -1;
	ipa_txn = pend_txn;
	return 0;
}

int ipaqmi_ipa_result(uint32_t *ctrl_ep, uint32_t *default_ep)
{
	if (ctrl_ep)
		*ctrl_ep = ipa_ctrl;
	if (default_ep)
		*default_ep = ipa_def;
	return ipa_res;
}

/* ---- `wland ipa ...` ---------------------------------------------------------------------------------------- */

int ipaqmi_cmd(int argc, char **argv)
{
	if (argc > 2 && !strcmp(argv[2], "show")) {
		/* wland ipa show [SMEM]: the INIT_DRIVER bytes, nothing sent */
		uint8_t b[7 + 162 + 16];
		uint32_t base = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 0) : 0;
		size_t n = ipaqmi_encode_init_driver(b, sizeof(b), 1, base, 0);
		printf("INIT_DRIVER (txn 1, smem base %#x), %zu bytes:", base, n);
		for (size_t i = 0; i < n; i++)
			printf("%s%02x", i % 16 ? " " : "\n  ", b[i]);
		printf("\n");
		for (int r = 0; r < M_COUNT; r++)
			printf("  %-20s %#06x + %#06x\n", ipa47_mem[r].name, ipa47_mem[r].ofst, ipa47_mem[r].size);
		return n ? 0 : 1;
	}
	if (argc > 2 && !strcmp(argv[2], "init-driver")) {
		/* ask the running wland (ipaqmi=1 or 2 in cell.on) to send one INIT_DRIVER; print its answer */
		struct sockaddr_un a = { .sun_family = AF_UNIX };
		snprintf(a.sun_path, sizeof(a.sun_path), "%s", IPAQMI_SOCK);
		int fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a))) {
			printf("ipa: %s: %s (wland running with ipaqmi=1 or 2 in %s?)\n", IPAQMI_SOCK, strerror(errno), CELL_ON);
			if (fd >= 0)
				close(fd);
			return 1;
		}
		if (write(fd, "I\n", 2) != 2) {
			close(fd);
			return 1;
		}
		char buf[512];
		int rc = 1;
		for (int waited = 0; waited < 40;) {
			struct pollfd p = { .fd = fd, .events = POLLIN };
			int pr = poll(&p, 1, 1000);
			if (pr == 0) {
				waited++;
				continue;
			}
			ssize_t k = read(fd, buf, sizeof(buf) - 1);
			if (k <= 0)
				break;
			buf[k] = 0;
			fputs(buf, stdout);
			if (strstr(buf, "ACCEPTED"))
				rc = 0;
		}
		close(fd);
		return rc;
	}
	fprintf(stderr, "usage: wland ipa show [SMEM] | init-driver   (init-driver needs ipaqmi=1 or 2 in %s)\n", CELL_ON);
	return 2;
}
