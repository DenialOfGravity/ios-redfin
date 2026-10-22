/*
 * flame: the station side of sys/redwood_sta.h (ios-topaz 0900's topaz_sta.c) over the WCN3990 radio,
 * which runs in userland (phone/wifi wland) and reaches the kernel through bsd/dev/flame_wlan.c (its
 * C API, flame_wlan_*, the one topaz_wlan_* was). When wland says the radio is up, en0 is made (a radio,
 * link down until a join) and the station attached, so that wifid finds a Wi-Fi interface.
 */
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/redwood_net.h>
#include <sys/redwood_sta.h>
#include <sys/systm.h>

extern kern_return_t thread_terminate(thread_t thread);

struct flame_wlan_bss {
	uint8_t bssid[6];
	uint8_t ssid[32], ssid_length;
	uint16_t frequency;
	int16_t rssi;
	uint16_t beacon_interval, capability;
	uint32_t age_ms;
	uint16_t elements_length;
	uint8_t elements[768];
};
extern void (*flame_wlan_ready_hook)(void);
extern bool flame_wlan_ready(void);
extern int flame_wlan_scan(void);
extern bool flame_wlan_heard(unsigned index, struct flame_wlan_bss *bss);
extern void flame_wlan_address(uint8_t address[6]);
extern int flame_wlan_join(const uint8_t *ssid, unsigned ssid_len, const uint8_t *bssid, const uint8_t *pass, unsigned pass_len);
extern int flame_wlan_join_pmk(const uint8_t *ssid, unsigned ssid_len, const uint8_t *bssid, const uint8_t pmk[32]);
extern int flame_wlan_leave(void);
extern bool flame_wlan_joined(void);
extern bool flame_wlan_transmit(const uint8_t *frame, size_t length);
extern void (*flame_wlan_rx_hook)(const uint8_t *frame, size_t length);
extern void (*flame_wlan_link_hook)(bool up);
extern bool flame_wlan_link_bss(struct flame_wlan_bss *bss);

static struct redwood_sta_bss sta_joined_bss;
static int sta_last_error;

static const uint16_t sta_freqs[] = {
	2412, 2417, 2422, 2427, 2432, 2437, 2442, 2447, 2452, 2457, 2462, 2467, 2472,
	5180, 5200, 5220, 5240, 5260, 5280, 5300, 5320,
	5500, 5520, 5540, 5560, 5580, 5600, 5620, 5640, 5660, 5680, 5700,
	5745, 5765, 5785, 5805, 5825,
};

static int
sta_power(__unused bool on)
{
	return 0;                               /* the radio stays up with the modem */
}

static int
sta_scan(__unused const struct redwood_sta_scan *what)
{
	return flame_wlan_scan();
}

static void
sta_heard(void (*each)(const struct redwood_sta_bss *bss, void *context), void *context)
{
	static struct flame_wlan_bss t;
	static struct redwood_sta_bss b;

	for (unsigned i = 0; flame_wlan_heard(i, &t); i++) {
		if (t.age_ms > 60000) {
			continue;                       /* not heard in the last minute */
		}
		bzero(&b, sizeof(b));
		memcpy(b.bssid, t.bssid, 6);
		memcpy(b.ssid, t.ssid, t.ssid_length);
		b.ssid_length = t.ssid_length;
		b.frequency = t.frequency;
		b.rssi = t.rssi;
		b.beacon_interval = t.beacon_interval;
		b.capability = t.capability;
		b.age_ms = t.age_ms;
		b.elements_length = t.elements_length < REDWOOD_STA_ELEMENTS_MOST ? t.elements_length : REDWOOD_STA_ELEMENTS_MOST;
		memcpy(b.elements, t.elements, b.elements_length);
		each(&b, context);
	}
}

static int
sta_join(const struct redwood_sta_join *what)
{
	static struct flame_wlan_bss t;
	bool any = false;
	int error;

	for (int i = 0; i < 6; i++) {
		any |= what->bssid[i] != 0;
	}
	if (what->security == REDWOOD_STA_WPA2_PSK && what->passphrase_length == 0) {
		error = flame_wlan_join_pmk(what->ssid, what->ssid_length, any ? what->bssid : NULL, what->pmk);
	} else error = flame_wlan_join(what->ssid, what->ssid_length, any ? what->bssid : NULL,
	    what->security == REDWOOD_STA_WPA2_PSK ? what->passphrase : NULL,
	    what->security == REDWOOD_STA_WPA2_PSK ? what->passphrase_length : 0);
	sta_last_error = error;
	bzero(&sta_joined_bss, sizeof(sta_joined_bss));
	if (error == 0) {
		for (unsigned i = 0; flame_wlan_heard(i, &t); i++) {
			if (t.ssid_length == what->ssid_length && memcmp(t.ssid, what->ssid, t.ssid_length) == 0) {
				memcpy(sta_joined_bss.bssid, t.bssid, 6);
				memcpy(sta_joined_bss.ssid, t.ssid, t.ssid_length);
				sta_joined_bss.ssid_length = t.ssid_length;
				sta_joined_bss.frequency = t.frequency;
				sta_joined_bss.rssi = t.rssi;
				sta_joined_bss.beacon_interval = t.beacon_interval;
				sta_joined_bss.capability = t.capability;
				sta_joined_bss.elements_length = t.elements_length < REDWOOD_STA_ELEMENTS_MOST ? t.elements_length : REDWOOD_STA_ELEMENTS_MOST;
				memcpy(sta_joined_bss.elements, t.elements, sta_joined_bss.elements_length);
				break;
			}
		}
	}
	printf("flame wifi: join -> %d\n", error);
	return error;
}

static int
sta_leave(void)
{
	return flame_wlan_leave();
}

static void
sta_state(struct redwood_sta_state *state)
{
	bzero(state, sizeof(*state));
	state->phase = flame_wlan_joined() ? REDWOOD_STA_JOINED : REDWOOD_STA_IDLE;
	if (state->phase == REDWOOD_STA_JOINED) {
		state->bss = sta_joined_bss;
		if (state->bss.ssid_length == 0) {
			/* flame: wland joined by itself (its wlan.conf, before wifid asked): the network it reports */
			struct flame_wlan_bss t;        /* (on any thread: not static) */
			if (flame_wlan_link_bss(&t)) {
				memcpy(state->bss.bssid, t.bssid, 6);
				memcpy(state->bss.ssid, t.ssid, t.ssid_length);
				state->bss.ssid_length = t.ssid_length;
				state->bss.frequency = t.frequency;
				state->bss.rssi = t.rssi;
				state->bss.beacon_interval = t.beacon_interval;
				state->bss.capability = t.capability;
				state->bss.elements_length = t.elements_length < REDWOOD_STA_ELEMENTS_MOST ? t.elements_length : REDWOOD_STA_ELEMENTS_MOST;
				memcpy(state->bss.elements, t.elements, state->bss.elements_length);
			}
		}
	}
	state->last_error = sta_last_error;
}

static unsigned
sta_channels(struct redwood_sta_channel *channels, unsigned most)
{
	unsigned n = 0;
	for (; n < most && n < sizeof(sta_freqs) / sizeof(sta_freqs[0]); n++) {
		channels[n].frequency = sta_freqs[n];
		channels[n].flags = REDWOOD_STA_CHANNEL_PASSIVE;
	}
	return n;
}

static struct redwood_sta_ops sta_ops = {
	.power = sta_power,
	.scan = sta_scan,
	.heard = sta_heard,
	.join = sta_join,
	.leave = sta_leave,
	.state = sta_state,
	.channels = sta_channels,
};

static bool
sta_transmit(mbuf_t packet, size_t length)
{
	static uint8_t frame[REDWOOD_NET_FRAME_MAX];   /* (one sender at a time: redwood_net's tx lock) */
	if (length > sizeof(frame) || mbuf_copydata(packet, 0, length, frame) != 0) {
		return false;
	}
	return flame_wlan_transmit(frame, length);
}

static void
sta_rx(const uint8_t *frame, size_t length)
{
	redwood_net_input(frame, length);
}

static void
sta_link_changed(bool up)
{
	redwood_net_link_state(up);
	if (!up && sta_last_error == 0) {
		redwood_sta_lost(ECONNRESET);
	}
}

static struct redwood_net_link sta_link = {
	.name = "wcn3990",
	.wireless = true,
	.up = false,
	.transmit = sta_transmit,
	.poll = NULL,
};

/* from the modem thread, once the radio is up: keep it short, the work goes on a thread */
static void
sta_bring_up(__unused void *arg, __unused wait_result_t wr)
{
	flame_wlan_address(sta_link.address);
	memcpy(sta_ops.address, sta_link.address, 6);
	flame_wlan_rx_hook = sta_rx;
	flame_wlan_link_hook = sta_link_changed;
	if (redwood_net_attach(&sta_link)) {
		redwood_sta_attach(&sta_ops);
		printf("flame wifi: en0 and the station are up for iOS\n");
	}
	thread_terminate(current_thread());
}

static void
sta_ready(void)
{
	thread_t thread;
	if (kernel_thread_start(sta_bring_up, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

void MiniIONetworkStackStart(void);   /* iokit/Kernel/arm/MiniIONetworkStack.cpp */
void flame_sta_init(void);
void
flame_sta_init(void)
{
	MiniIONetworkStackStart();              /* configd waits for it before it calls the registry quiet */
	flame_wlan_ready_hook = sta_ready;
}
