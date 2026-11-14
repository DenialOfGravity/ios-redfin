/*
 * ssctl (n12h, Claude 2026-11-12): tell the modem the ADSP is up, as Linux/Android sysmon does.
 *
 * Hypothesis (research/cc/P3-audio-result.md "Silent session", PC): kernel 1065/1066 sets up the ADSP voice session and
 * every APR step is accepted, yet no PCM flows: the modem never attaches its vocoder, because nothing told it the ADSP
 * came up. On Linux, sysmon_start() of the modem walks the other subsystems already at AFTER_POWERUP and sends each one's
 * state to the starting modem (ssctl_send_event, SSCTL v2). Here the ADSP is up before wland boots MPSS, so that is the
 * one message: "adsp" AFTER_POWERUP. (Linux would also send "cdsp" if the CDSP were running; not sent here.)
 *
 * Word ssctladsp=1 in /var/local/cell.on (re-read every 10 s; 0/absent = off, unchanged behaviour). Sent once per modem
 * SSCTL server appearance (a modem restart sends it again), as soon as the server is known AND cell has started
 * (cell_started: Wi-Fi bring-up + 15 s, or 3 min after the link). One retry after 5 s without an answer, then given up.
 *
 * Wire (Linux drivers/remoteproc/qcom_sysmon.c): QMI service 0x2b (QMI_SERVICE_ID_SSCTL), version 2, instance =
 * qcom,ssctl-instance-id (modem 0x12; QRTR instance word 0x1202 = version | instance << 8, seen in wland.log at 0:1).
 * SSCTL_SUBSYS_EVENT_REQ 0x23:
 *   TLV 0x01 subsys_name: u8 length + chars (max 15)  "adsp"
 *   TLV 0x02 event u32: SSCTL_SSR_EVENT_AFTER_POWERUP = 1 (BEFORE_POWERUP 0, BEFORE_SHUTDOWN 2, AFTER_SHUTDOWN 3)
 *   TLV 0x10 evt_driven u32 (optional): SSCTL_SSR_EVENT_FORCED = 0 (Linux always sends it, FORCED)
 * Response 0x23: TLV 0x02 standard QMI result. (0x21 is SSCTL_SHUTDOWN_REQ: never sent.)
 * Log: "ssctl: SSCTL: adsp after-powerup -> result ok|err 0x..|no answer".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "ssctl.h"
#include "cell.h"
#include "link.h"
#include "qmi.h"

#define SSCTL_SERVICE		0x2b
#define SSCTL_INST_MODEM	0x12
#define SSCTL_SUBSYS_EVENT_REQ	0x23
#define SSCTL_EVT_AFTER_POWERUP	1
#define SSCTL_EVT_FORCED	0

static int on;				/* ssctladsp= value */
static struct { uint32_t node, port; int up; } srv;
static int done, tries;			/* per server appearance */
static uint16_t txn = 0x5c00, want;
static uint64_t sent_ms, next_check;

void ssctl_server(uint32_t service, uint32_t instance, uint32_t node, uint32_t port)
{
	if (service != SSCTL_SERVICE || (instance & 0xff) != 2 || (instance >> 8) != SSCTL_INST_MODEM)
		return;
	srv.node = node;
	srv.port = port;
	srv.up = 1;
	done = tries = 0;
	want = 0;
	if (on)
		printf("[%6llu] ssctl: modem SSCTL v2 (instance %#x) at %u:%x\n", (unsigned long long)link_ms(), instance, node, port);
}

void ssctl_server_gone(uint32_t node, uint32_t port)
{
	if (srv.up && srv.node == node && (port == 0xffffffffu || srv.port == port)) {
		srv.up = 0;
		want = 0;
		if (on)
			printf("[%6llu] ssctl: modem SSCTL server gone\n", (unsigned long long)link_ms());
	}
}

static void send_event(uint64_t now)
{
	static const char name[] = "adsp";
	uint8_t nb[1 + sizeof(name) - 1];
	nb[0] = (uint8_t)(sizeof(name) - 1);
	memcpy(nb + 1, name, sizeof(name) - 1);
	struct qmi_buf *q = malloc(sizeof(*q));
	if (!q)
		return;
	want = ++txn;
	qmi_start(q, QMI_REQUEST, want, SSCTL_SUBSYS_EVENT_REQ);
	qmi_tlv(q, 0x01, nb, sizeof(nb));
	qmi_tlv_u32(q, 0x02, SSCTL_EVT_AFTER_POWERUP);
	qmi_tlv_u32(q, 0x10, SSCTL_EVT_FORCED);
	size_t len = qmi_finish(q);
	sent_ms = now;
	tries++;
	printf("[%6llu] ssctl: > SUBSYS_EVENT 0x23 'adsp' after-powerup forced (txn %#x, try %d) to %u:%x\n",
	       (unsigned long long)now, want, tries, srv.node, srv.port);
	if (len)
		qrtr_send(QRTR_TYPE_DATA, SSCTL_PORT, srv.node, srv.port, q->b, (uint32_t)len);
	free(q);
}

void ssctl_handle(const struct qrtr_hdr *h, const uint8_t *d, uint32_t n)
{
	struct qmi_msg m;
	if (qmi_parse(d, n, &m))
		return;
	if (m.type != QMI_RESPONSE || m.id != SSCTL_SUBSYS_EVENT_REQ || !want || m.txn != want) {
		printf("[%6llu] ssctl: < type %u msg %#x txn %#x (not expected)\n", (unsigned long long)link_ms(), m.type, m.id, m.txn);
		return;
	}
	want = 0;
	done = 1;
	uint16_t l;
	const uint8_t *p = qmi_get(&m, 2, &l);
	if (!p || l < 4) {
		printf("[%6llu] SSCTL: adsp after-powerup -> result ? (no result TLV, %u bytes)\n", (unsigned long long)link_ms(), m.len);
		return;
	}
	uint16_t r, e;
	memcpy(&r, p, 2);
	memcpy(&e, p + 2, 2);
	if (!r)
		printf("[%6llu] SSCTL: adsp after-powerup -> result ok (0)\n", (unsigned long long)link_ms());
	else
		printf("[%6llu] SSCTL: adsp after-powerup -> result err %u error %#x\n", (unsigned long long)link_ms(), r, e);
}

static int read_word(void)
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
	const char *w = strstr(b, "ssctladsp=");
	return w ? atoi(w + 10) : 0;
}

void ssctl_poll(void)
{
	uint64_t now = link_ms();
	if (now >= next_check) {
		next_check = now + 10000;
		int v = read_word();
		if (v != on) {
			printf("[%6llu] ssctl: ssctladsp=%d (was %d); modem SSCTL server %s\n", (unsigned long long)now, v, on,
			       srv.up ? "up" : "not seen yet");
			on = v;
		}
	}
	if (on != 1 || !srv.up || done)
		return;
	if (want) {
		if (now - sent_ms < 5000)
			return;
		printf("[%6llu] ssctl: no answer to txn %#x in 5 s\n", (unsigned long long)now, want);
		want = 0;
		if (tries >= 2) {
			printf("[%6llu] SSCTL: adsp after-powerup -> result no answer (given up)\n", (unsigned long long)now);
			done = 1;
			return;
		}
		qrtr_kick(SSCTL_PORT, srv.node, srv.port);
	}
	if (!cell_started())
		return;
	send_event(now);
}
