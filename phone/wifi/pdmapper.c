/*
 * pd-mapper: the service registry locator (QMI service 0x40), which tells
 * the modem's PD framework which protection domains provide a service. The
 * table is the firmware's own .jsn files (vendor/firmware/modemuw.jsn and
 * the radio's modemr.jsn), as github.com/andersson/pd-mapper reads them.
 *
 * T1 (P3 voice link, Claude 2026-11-13): word pdmapadsp=1 in /var/local/cell.on (read at every query; 0/absent =
 * the modem-only table as before) adds redfin's ADSP/CDSP jsn domains (vendor/firmware adspr, adsps, adspua,
 * cdspr.jsn), as Android's pd-mapper serves them: "avs/audio" -> msm/adsp/audio_pd 74, "tms/servreg" -> 6 domains.
 *
 * pdmapadsp=2 (Claude 2026-11-13, r5bb3a): with pdmapadsp=1 the modem never asked tftp for wlanmdsp.mbn, so wlan_pd
 * (WLFW 0x45) never started and Wi-Fi stayed down. Mode 2 answers like pdmapadsp=0 until the WLFW server appears;
 * a query whose only domains are on the ADSP/CDSP (avs/audio) is held unanswered until then, then answered as mode 1.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "cell.h"
#include "link.h"
#include "pdmapper.h"
#include "qmi.h"

#define GET_DOMAIN_LIST	0x21
#define PFR		0x24	/* protection domain failure report */

static const struct { const char *service, *domain; uint32_t instance; int adsp; } db[] = {
	{ "tms/servreg", "msm/modem/root_pd", 180, 0 },		/* modemr.jsn */
	{ "gps/gps_service", "msm/modem/root_pd", 180, 0 },
	{ "kernel/elf_loader", "msm/modem/wlan_pd", 180, 0 },	/* modemuw.jsn */
	{ "tms/servreg", "msm/modem/wlan_pd", 180, 0 },
	{ "wlan/fw", "msm/modem/wlan_pd", 180, 0 },
	{ "tms/servreg", "msm/adsp/root_pd", 74, 1 },		/* adspr.jsn (pdmapadsp=1 only) */
	{ "tms/servreg", "msm/adsp/sensor_pd", 74, 1 },		/* adsps.jsn */
	{ "tms/servreg", "msm/adsp/audio_pd", 74, 1 },		/* adspua.jsn */
	{ "avs/audio", "msm/adsp/audio_pd", 74, 1 },
	{ "tms/servreg", "msm/cdsp/root_pd", 76, 1 },		/* cdspr.jsn */
};

static int wlan_seen;			/* the WLFW server (wlan_pd) has appeared */
static struct { uint32_t node, port, offset; uint16_t txn, id; char name[80]; int used; } held[8];

/* pdmapadsp= in cell.on (0 when the file or the word is missing) */
static int pdmapadsp(void)
{
	char b[2048];
	int fd = open(CELL_ON, O_RDONLY);
	if (fd < 0)
		return 0;
	ssize_t k = read(fd, b, sizeof(b) - 1);
	close(fd);
	if (k <= 0)
		return 0;
	b[k] = 0;
	const char *w = strstr(b, "pdmapadsp=");
	return w ? atoi(w + 10) : 0;
}

/* a name whose every table entry is an ADSP/CDSP one (and there is at least one) */
static int adsp_only(const char *name)
{
	int n = 0;
	for (unsigned i = 0; i < sizeof(db) / sizeof(db[0]); i++) {
		if (strcmp(db[i].service, name))
			continue;
		if (!db[i].adsp)
			return 0;
		n++;
	}
	return n > 0;
}

/* the GET_DOMAIN_LIST answer's TLVs into q (result already wanted: added here) */
static void domain_answer(struct qmi_buf *q, const char *name, uint32_t offset, int adsp, int mode)
{
	/* domain_list[]: u8 count, then { u8 len + name, u32 instance, u8 valid, u32 data } */
	uint8_t list[512];
	size_t o = 1;
	uint16_t total = 0;
	list[0] = 0;
	for (unsigned i = 0; i < sizeof(db) / sizeof(db[0]); i++) {
		if (strcmp(db[i].service, name) || (db[i].adsp && !adsp))
			continue;
		if (total++ < offset)
			continue;
		uint8_t nl = (uint8_t)strlen(db[i].domain);
		list[o++] = nl;
		memcpy(list + o, db[i].domain, nl);
		o += nl;
		memcpy(list + o, &db[i].instance, 4);
		o += 4;
		list[o++] = 0;
		memset(list + o, 0, 4);
		o += 4;
		list[0]++;
	}
	printf("pd-mapper: \"%s\" -> %u domain(s)%s\n", name, total, mode ? (mode == 2 ? " (pdmapadsp=2)" : " (pdmapadsp=1)") : "");
	qmi_tlv_result(q, 0, 0);
	qmi_tlv(q, 0x10, &total, 2);
	uint16_t rev = 1;
	qmi_tlv(q, 0x11, &rev, 2);
	qmi_tlv(q, 0x12, list, (uint16_t)o);
}

/* wland.c: the WLFW server appeared; answer the held queries with the ADSP/CDSP domains (pdmapadsp=2) */
void pdmapper_wlan_up(void)
{
	if (wlan_seen)
		return;
	wlan_seen = 1;
	for (unsigned i = 0; i < sizeof(held) / sizeof(held[0]); i++) {
		if (!held[i].used)
			continue;
		struct qmi_buf q;
		qmi_start(&q, QMI_RESPONSE, held[i].txn, held[i].id);
		domain_answer(&q, held[i].name, held[i].offset, 1, 2);
		qrtr_send(QRTR_TYPE_DATA, PDMAPPER_PORT, held[i].node, held[i].port, q.b, (uint32_t)qmi_finish(&q));
		held[i].used = 0;
	}
}

int pdmapper_init(void)
{
	if (qrtr_new_server(PDMAPPER_SERVICE, 1, 1, PDMAPPER_PORT))
		return -1;
	printf("pd-mapper: serving at %u:%x\n", QRTR_NODE_APPS, PDMAPPER_PORT);
	return 0;
}

void pdmapper_handle(const struct qrtr_hdr *h, const uint8_t *d, uint32_t n)
{
	struct qmi_msg m;
	struct qmi_buf q;
	if (qmi_parse(d, n, &m) || m.type != QMI_REQUEST)
		return;
	qmi_start(&q, QMI_RESPONSE, m.txn, m.id);
	if (m.id == PFR) {
		/* a protection domain failed: the service name and the reason (the PD's SFR string) */
		uint16_t l1 = 0, l2 = 0;
		const uint8_t *svc = qmi_get(&m, 1, &l1), *why = qmi_get(&m, 2, &l2);
		printf("pd-mapper: PD FAILURE \"%.*s\": %.*s\n", svc ? l1 : 0, svc ? (const char *)svc : "",
		       why ? l2 : 0, why ? (const char *)why : "");
		qmi_tlv_result(&q, 0, 0);
	} else if (m.id != GET_DOMAIN_LIST) {
		qmi_dump(&m);
		qmi_tlv_result(&q, 1, 0x47);
	} else {
		uint16_t l = 0;
		const uint8_t *p = qmi_get(&m, 1, &l);
		char name[80] = { 0 };
		uint32_t offset = 0;
		if (p)
			memcpy(name, p, l < 79 ? l : 79);
		qmi_get_u32(&m, 0x10, &offset);
		int mode = pdmapadsp();
		if (mode == 2 && !wlan_seen && adsp_only(name)) {
			for (unsigned i = 0; i < sizeof(held) / sizeof(held[0]); i++) {
				if (!held[i].used) {
					held[i].used = 1;
					held[i].node = h->src_node;
					held[i].port = h->src_port;
					held[i].offset = offset;
					held[i].txn = m.txn;
					held[i].id = m.id;
					memcpy(held[i].name, name, sizeof(name));
					printf("pd-mapper: \"%s\" held until the WLFW server appears (pdmapadsp=2)\n", name);
					return;
				}
			}
		}
		domain_answer(&q, name, offset, mode == 1 || (mode == 2 && wlan_seen), mode);
	}
	qrtr_send(QRTR_TYPE_DATA, PDMAPPER_PORT, h->src_node, h->src_port, q.b, (uint32_t)qmi_finish(&q));
}
