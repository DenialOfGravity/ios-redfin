/*
 * Darwin-on-redwood: Apple80211, answered in the kernel.
 *
 * iOS's Wi-Fi side -- wifid, and through it Settings and the status bar -- talks to a Wi-Fi
 * driver in requests that were once ioctls and are, where the driver offers no user client,
 * ioctls still: SIOCGA80211 and SIOCSA80211 on any socket, with a struct apple80211req that
 * names the interface, a request type, a value, and a buffer in the caller's memory
 * (IO80211.framework: Apple80211Get/Set and their kin all end there). XNU hands an interface
 * ioctl it does not know to the interface, so they arrive in bsd/dev/redwood_net.c, which
 * passes them here when its link is a radio.
 *
 * What is asked is answered from the station boundary (sys/redwood_sta.h): the radio's power,
 * its channels, a scan and what it heard, the network joined. What happens there by itself is
 * told to iOS the way a driver tells it: kernel events of class 6 (IEEE 802.11), subclass 1,
 * whose code is Apple80211's own event number and whose data begins with the interface's name
 * in 16 bytes (IO80211.framework reads them from a PF_SYSTEM socket and compares the name).
 *
 * Every request is still written to the log -- who asks, get or set, the type, the value, the
 * length -- because which of them wifid needs is found that way. Never what a request carries:
 * a network's name or its key may be in there.
 *
 * The request types and the layouts are Apple's. The old ones are as the headers of its open
 * IO80211 sources once described them (apple80211_ioctl.h, apple80211_var.h); where iOS 16.5
 * differs the layout was read out of IO80211.framework of 16.5, which fills and reads the same
 * structures on the other side (Apple80211GetWithIOCTL, _getScanData, _addScanResultToList,
 * _performAssociation, _dispatchAsyncAssocEvent, _dispatchLinkChangedEvent).
 *
 * A join: ASSOCIATE (20) carries the network's name, perhaps its access point, and the
 * passphrase as typed (the framework does not make the key of it), and is answered at once;
 * when the station has joined or failed, event 9 says so, and ASSOCIATE_RESULT (21) gives the
 * outcome as one of Apple80211's result numbers, which the framework turns into its errors:
 * 1 success, 21 "invalid PMK" (a wrong passphrase), 9 a timeout, 2 anything else.
 */
#include <kern/kalloc.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/kern_event.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/redwood_input.h>
#include <sys/redwood_net.h>
#include <sys/redwood_sta.h>
#include <sys/sockio.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#define SIOCSA80211             0x802869c8u     /* _IOW('i', 200, struct apple80211req) */
#define SIOCGA80211             0xc02869c9u     /* _IOWR('i', 201, struct apple80211req) */

struct apple80211req {
	char req_if_name[16];
	int32_t req_type;
	int32_t req_val;
	uint32_t req_len;
	user_addr_t req_data __attribute__((aligned(8)));
};

/* (as far as they have come by: the names help reading the log) */
static const char *
type_name(int32_t type)
{
	static const struct {
		int32_t type;
		const char *name;
	} names[] = {
		{ 1, "SSID" }, { 2, "AUTH_TYPE" }, { 3, "CIPHER_KEY" }, { 4, "CHANNEL" }, { 5, "POWERSAVE" }, { 6, "PROTMODE" },
		{ 7, "TXPOWER" }, { 8, "RATE" }, { 9, "BSSID" }, { 10, "SCAN_REQ" }, { 11, "SCAN_RESULT" }, { 12, "CARD_CAPABILITIES" },
		{ 13, "STATE" }, { 14, "PHY_MODE" }, { 15, "OP_MODE" }, { 16, "RSSI" }, { 17, "NOISE" }, { 18, "INT_MIT" }, { 19, "POWER" },
		{ 20, "ASSOCIATE" }, { 21, "ASSOCIATE_RESULT" }, { 22, "DISASSOCIATE" }, { 23, "STATUS_DEV_NAME" }, { 24, "IBSS_MODE" },
		{ 25, "HOST_AP_MODE" }, { 26, "AP_MODE" }, { 27, "SUPPORTED_CHANNELS" }, { 28, "LOCALE" }, { 29, "DEAUTH" },
		{ 30, "COUNTERMEASURES" }, { 32, "RATE_SET" }, { 42, "STATION_LIST" }, { 43, "DRIVER_VERSION" },
		{ 44, "HARDWARE_VERSION" }, { 46, "RSN_IE" }, { 47, "BACKGROUND_SCAN" }, { 48, "AP_IE_LIST" }, { 49, "STATS" },
		{ 50, "ASSOCIATION_STATUS" }, { 51, "COUNTRY_CODE" }, { 54, "RADIO_INFO" }, { 57, "MCS" }, { 66, "MCS_INDEX_SET" },
		{ 79, "STA_STATS" }, { 80, "ROAM_THRESH" }, { 85, "IE" }, { 86, "SCAN_REQ_MULTIPLE" }, { 87, "BTCOEX_MODE" },
		{ 90, "SCANCACHE_CLEAR" }, { 94, "VIRTUAL_IF_CREATE" }, { 95, "VIRTUAL_IF_DELETE" }, { 103, "CURRENT_NETWORK" },
		{ 106, "RSSI_BOUNDS" }, { 107, "ROAM" }, { 207, "CHANNELS_INFO" }, { 254, "HW_SUPPORTED_CHANNELS" },
	};

	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (names[i].type == type) {
			return names[i].name;
		}
	}
	return "?";
}

/* ---- the layouts ------------------------------------------------------------------------------------------ */

#define CHANNEL_20MHZ           0x2u
#define CHANNEL_2GHZ            0x8u
#define CHANNEL_5GHZ            0x10u
#define CHANNEL_ACTIVE          0x80u           /* the radio may send there */
#define CHANNEL_DFS             0x100u

struct apple80211_channel {
	uint32_t version, channel, flags;
};

/* SUPPORTED_CHANNELS (27) and HW_SUPPORTED_CHANNELS (254): 4824 bytes */
struct apple80211_channels {
	uint32_t version, count;
	struct apple80211_channel channels[401];
	uint32_t unused;
};

/* CHANNELS_INFO (207): 10016 bytes; of each channel a byte in each of several rows, and a word of flags */
struct apple80211_channels_info {
	uint8_t unknown_0[12];
	uint16_t count;
	uint8_t unknown_1[800];
	uint8_t number[400], indoor[400], dfs[400], passive[400], wide_40[400], wide_80[400];
	uint8_t unknown_2[1602];
	uint32_t flags[400];
	uint8_t unknown_3[3600];
} __attribute__((packed));

/* SCAN_REQ (10): 5456 bytes */
struct apple80211_scan_request {
	uint32_t version, bss_type;
	uint8_t bssid[6], unused_0[2];
	uint32_t ssid_length;
	uint8_t ssid[32];
	uint32_t scan_type;                     /* 1: asking, 2: listening only */
	uint32_t phy_mode;
	uint16_t dwell_ms, unused_1;
	uint32_t rest_ms;
	uint32_t channel_count;
	struct apple80211_channel channels[400];
	uint64_t flags;
	uint32_t unused_2;
	uint32_t name_count;                    /* networks asked for by name, each in 56 bytes */
	uint8_t names[10][56];
	uint32_t short_ssid, unused_3;
};

/* SCAN_RESULT (11) and CURRENT_NETWORK (103): 1164 bytes */
struct apple80211_scan_result {
	uint32_t version;
	struct apple80211_channel channel;
	uint16_t noise_known;
	int16_t noise, snr, rssi;
	uint16_t beacon_interval, capability;
	uint8_t bssid[6];
	uint8_t rate_count, unused_0;
	uint32_t rates[15];                     /* Mbit/s */
	uint8_t ssid_length, ssid[32], unused_1[3];
	uint32_t age_ms;
	uint8_t flags, unused_2;
	uint16_t elements_length;               /* comes in as the room there is */
	uint8_t elements[1024];
};

/* ASSOCIATE (20): 592 bytes; of what follows the key (the RSN element wifid built, flags) nothing is used */
struct apple80211_assoc {
	uint32_t version;
	uint16_t mode, auth_lower, auth_upper, unused_0;
	uint32_t ssid_length;
	uint8_t ssid[32];
	uint8_t bssid[6], unused_1[2];
	uint32_t key_version, key_length, key_cipher;   /* cipher 10: a passphrase as typed; 6: the key (from 64 hex digits) */
	uint16_t key_flags, key_index;
	uint8_t key[64];
	uint8_t rest[0x250 - 0x88];
};

#define CIPHER_PMK              6
#define CIPHER_PASSPHRASE       10

/* the link event's data (at least 32 bytes, or the framework drops the event) */
struct apple80211_link_changed {
	uint8_t down, unused_0[3];
	int32_t rssi;
	uint16_t snr, noise;
	uint8_t unused_1[4];
	uint8_t involuntary, unused_2[3];
	uint32_t reason, subreason;
	uint8_t unused_3[4];
};

/*
 * redfin 1051: the RSSI event's data (Scion-Project/hackios-xnu-kernel PR #191, read from IO80211.framework 16.5:
 * the handler of event 39 wants at least 0xb4 bytes). Each group with a flag counts only with it set; the
 * counters stay 0 (wifid's link quality reads a loss rate out of them and from nothing concludes nothing).
 */
struct apple80211_rssi_changed {
	uint8_t has_rssi, unused_0[3];
	int32_t rssi;
	uint8_t has_per_core, per_core[2];
	uint8_t has_snr;
	int16_t snr;
	uint8_t has_noise, unused_1;
	int16_t noise;
	uint8_t has_cca, cca;
	uint32_t tx_fail, tx_retrans, tx_frames, rx_retries, rx_frames, rx_beacons, beacons_due;
	uint8_t has_beacons, has_firmware_counts, unused_2[2];
	uint32_t firmware_tx_fail, firmware_tx_retrans, firmware_tx_frames;
	uint32_t tx_rate, tx_fallback_rate, rx_rate;    /* Mbit/s */
	uint8_t rest[0xb4 - 0x4c];
};

_Static_assert(sizeof(struct apple80211_rssi_changed) == 0xb4 && offsetof(struct apple80211_rssi_changed, tx_fail) == 0x14 &&
    offsetof(struct apple80211_rssi_changed, has_beacons) == 0x30 && offsetof(struct apple80211_rssi_changed, tx_rate) == 0x40,
    "the RSSI event as iOS 16.5 reads it");
_Static_assert(sizeof(struct apple80211_assoc) == 592 && offsetof(struct apple80211_assoc, key) == 0x48,
    "the association request as iOS 16.5 has it");
_Static_assert(sizeof(struct apple80211_link_changed) == 32 && offsetof(struct apple80211_link_changed, reason) == 0x14,
    "the link event as iOS 16.5 reads it");
_Static_assert(sizeof(struct apple80211_channels) == 4824, "the channel list as iOS 16.5 has it");
_Static_assert(sizeof(struct apple80211_channels_info) == 10016 && offsetof(struct apple80211_channels_info, number) == 0x32e &&
    offsetof(struct apple80211_channels_info, flags) == 0x12d0, "the channels' description as iOS 16.5 has it");
_Static_assert(sizeof(struct apple80211_scan_request) == 5456 && offsetof(struct apple80211_scan_request, channels) == 0x48 &&
    offsetof(struct apple80211_scan_request, flags) == 0x1308 && offsetof(struct apple80211_scan_request, names) == 0x1318,
    "the scan request as iOS 16.5 has it");
_Static_assert(sizeof(struct apple80211_scan_result) == 1164 && offsetof(struct apple80211_scan_result, ssid_length) == 0x60 &&
    offsetof(struct apple80211_scan_result, age_ms) == 0x84 && offsetof(struct apple80211_scan_result, elements) == 0x8c,
    "a scan's result as iOS 16.5 has it");

/* Apple80211's events (APPLE80211_M_...) */
enum { EVENT_POWER_CHANGED = 1, EVENT_SSID_CHANGED = 2, EVENT_BSSID_CHANGED = 3, EVENT_LINK_CHANGED = 4, EVENT_ASSOC_DONE = 9,
       EVENT_SCAN_DONE = 10, EVENT_RSSI_CHANGED = 39 };

static const char interface_name[16] = "en0";   /* (bsd/dev/redwood_net.c makes one interface, and that is its name) */

static LCK_GRP_DECLARE(apple80211_lock_group, "redwood-apple80211");
static LCK_MTX_DECLARE(apple80211_lock, &apple80211_lock_group);
static unsigned result_next;                    /* which of what the last scan heard a SCAN_RESULT request gets next */
static uint32_t join_result;                    /* of the last join asked for: 0 while it runs, then Apple80211's number */
/*
 * 0193: the country code iOS sets (COUNTRY_CODE, 51) is kept and given back, never sent to the firmware;
 * set 383 is taken and dropped. sweetwifiacc=0 refuses both again, as before.
 * apple80211_country_code_data (apple80211_var.h): { u32 version; u8 cc[3]; }, padded to 8.
 */
static int wifi_accept = -1;
static char country[3] = "XX";

/*
 * redfin 1051: the joined network's signal for wifid, without 1014's association-time code. Bits of boot-arg
 * sweetwifisig= / sysctl kern.sweet_wifi_sig (default 0 = as before 1051):
 *   0x01  each FW_RSSI from wland (joined: every 2-10 s) posts RSSI_CHANGED (39) in the 0xb4-byte layout: the only
 *         thing wifid scales the status-bar bars from (Scion PR #191). Posted from wland's write, no kernel timer.
 *   0x02  RSSI (16) and CURRENT_NETWORK (103) give that live signal instead of the last scan's before the join
 *   0x04  scan results and CURRENT_NETWORK say a noise floor (-95 dBm, noise_known 1) and snr (auto-join test:
 *         wifid refuses known networks at -40 dBm "Below minimum RSSI threshold" while noise/snr read 0)
 *   0x10  a link wland made by itself (/var/local/wlan.conf at boot, before wifid asks) is told to wifid as a
 *         join's (SSID/BSSID/LINK changed): the boot auto-join wifid refuses is then wland's (bsd/dev/flame_sta.c)
 *   0x20  (redfin 1143) RSSI_BOUNDS (106) is answered: version 1, RSSI_MIN / RSSI_MAX / RSSI_THRESHOLD (dBm) from
 *         kern.sweet_wifi_rssi_min / _max / _thr (-100 / -20 / -80). Without it 106 is refused (EOPNOTSUPP), and
 *         IO80211 then gives wifid no bounds: the auto-join test for "Below minimum RSSI threshold" at -40 dBm.
 *         wifid asks once when it attaches to en0: set the bit, then restart wifid (killall wifid).
 * kern.sweet_wifi_live_rssi (read only): the last signal wland reported, 0 when none. "SWEET WIFI 1051" lines.
 */
#define NOISE_FLOOR     (-95)
static int wifi_sig = 3;	/* default 3 since 2026-11-05 (sweetwifisig=0 = as before) */
static bool wifi_sig_read;
static int wifi_live_rssi;                      /* the joined network's last signal from wland (0: none since the link came) */
SYSCTL_INT(_kern, OID_AUTO, sweet_wifi_sig, CTLFLAG_RW | CTLFLAG_LOCKED, &wifi_sig, 0, "redfin 1051: Wi-Fi signal for wifid (bits)");
SYSCTL_INT(_kern, OID_AUTO, sweet_wifi_live_rssi, CTLFLAG_RD | CTLFLAG_LOCKED, &wifi_live_rssi, 0, "redfin 1051: last signal, dBm");
/* redfin 1143: RSSI_BOUNDS (106) answer, only with kern.sweet_wifi_sig & 0x20 (dBm; RSSI_THRESHOLD = the floor for joins) */
static int wifi_rssi_min = -100, wifi_rssi_max = -20, wifi_rssi_thr = -80;
SYSCTL_INT(_kern, OID_AUTO, sweet_wifi_rssi_min, CTLFLAG_RW | CTLFLAG_LOCKED, &wifi_rssi_min, 0, "redfin 1143: RSSI_BOUNDS min, dBm");
SYSCTL_INT(_kern, OID_AUTO, sweet_wifi_rssi_max, CTLFLAG_RW | CTLFLAG_LOCKED, &wifi_rssi_max, 0, "redfin 1143: RSSI_BOUNDS max, dBm");
SYSCTL_INT(_kern, OID_AUTO, sweet_wifi_rssi_thr, CTLFLAG_RW | CTLFLAG_LOCKED, &wifi_rssi_thr, 0, "redfin 1143: RSSI_BOUNDS threshold, dBm");

int redwood_apple80211_sig(void);
int
redwood_apple80211_sig(void)
{
	if (!wifi_sig_read) {
		int bits;

		wifi_sig_read = true;
		if (PE_parse_boot_argn("sweetwifisig", &bits, sizeof(bits))) {
			wifi_sig = bits;
		}
	}
	return wifi_sig;
}

static bool
accepting(void)
{
	if (wifi_accept < 0) {
		int on = 1;

		PE_parse_boot_argn("sweetwifiacc", &on, sizeof(on));
		wifi_accept = on != 0;
	}
	return wifi_accept != 0;
}

/* ---- channels ---------------------------------------------------------------------------------------- */

static unsigned
channel_number(unsigned frequency)
{
	return frequency == 2484 ? 14 : frequency < 4000 ? (frequency - 2407) / 5 : (frequency - 5000) / 5;
}

static unsigned
channel_frequency(const struct apple80211_channel *channel)
{
	bool low = channel->flags & CHANNEL_2GHZ ? true : channel->flags & CHANNEL_5GHZ ? false : channel->channel <= 14;

	if (channel->channel == 0 || channel->channel > 196 || (channel->flags & 0x2000)) {     /* (0x2000: 6 GHz) */
		return 0;
	}
	return low ? (channel->channel == 14 ? 2484 : 2407 + 5 * channel->channel) : 5000 + 5 * channel->channel;
}

static void
channel_fill(struct apple80211_channel *channel, unsigned frequency, unsigned flags)
{
	channel->version = 1;
	channel->channel = channel_number(frequency);
	channel->flags = CHANNEL_20MHZ | (frequency < 4000 ? CHANNEL_2GHZ : CHANNEL_5GHZ) | (flags & REDWOOD_STA_CHANNEL_PASSIVE ?
	    0 : CHANNEL_ACTIVE) | (flags & REDWOOD_STA_CHANNEL_RADAR ? CHANNEL_DFS : 0);
}

/* ---- what the caller is given back ------------------------------------------------------------------- */

/* At most as many bytes as it has room for. */
static int
give(const struct apple80211req *request, const void *data, size_t length)
{
	return request->req_data == USER_ADDR_NULL ? 0 : copyout(data, request->req_data, length < request->req_len ? length :
	       request->req_len);
}

/* An access point in the form a scan's result has; the caller has said how much room for elements there is. */
static int
give_network(const struct apple80211req *request, const struct redwood_sta_bss *bss, bool say)
{
	static unsigned dropped_said;
	struct apple80211_scan_result *result = kalloc_data(sizeof(*result), Z_WAITOK | Z_ZERO);
	unsigned room = sizeof(result->elements), asked_room = ~0u;
	int error;

	if (result == NULL) {
		return ENOMEM;
	}
	if (request->req_len >= sizeof(*result) && copyin(request->req_data, result, offsetof(struct apple80211_scan_result,
	    elements)) == 0) {
		asked_room = result->elements_length;
		room = result->elements_length < room ? result->elements_length : room;
	}
	bzero(result, sizeof(*result));
	result->version = 1;
	channel_fill(&result->channel, bss->frequency, 0);
	result->rssi = bss->rssi;
	if (redwood_apple80211_sig() & 0x4) {                   /* 1051 */
		result->noise_known = 1;
		result->noise = NOISE_FLOOR;
		result->snr = (int16_t)(bss->rssi > NOISE_FLOOR ? bss->rssi - NOISE_FLOOR : 0);
	}
	result->beacon_interval = bss->beacon_interval;
	result->capability = bss->capability;
	memcpy(result->bssid, bss->bssid, sizeof(result->bssid));
	result->ssid_length = bss->ssid_length;
	memcpy(result->ssid, bss->ssid, bss->ssid_length);
	result->age_ms = bss->age_ms;
	result->elements_length = (uint16_t)(bss->elements_length <= room ? bss->elements_length : 0);
	memcpy(result->elements, bss->elements, result->elements_length);
	/* 0188: what the caller had room for and what it got (once a scan; drops up to 10 times a run) */
	if (say) {
		printf("SWEET WIFI: scan result elements: caller's length %d (%u bytes asked for), have %u, returned %u\n",
		    asked_room == ~0u ? -1 : (int)asked_room, request->req_len, bss->elements_length, result->elements_length);
	}
	if (bss->elements_length > room && dropped_said < 10) {
		dropped_said++;
		printf("SWEET WIFI: scan result elements dropped (room %u, have %u)\n", room, bss->elements_length);
	}
	/* the rates it says it has (elements 1 and 50: units of 500 kbit/s, the top bit marks the ones everybody must have) */
	for (unsigned at = 0; at + 2 <= bss->elements_length && at + 2 + bss->elements[at + 1] <= bss->elements_length;
	    at += 2 + bss->elements[at + 1]) {
		for (unsigned i = 0; (bss->elements[at] == 1 || bss->elements[at] == 50) && i < bss->elements[at + 1]; i++) {
			unsigned rate = (bss->elements[at + 2 + i] & 0x7f) / 2;

			if (rate != 0 && rate <= 54 && result->rate_count < 15) {
				result->rates[result->rate_count++] = rate;
			}
		}
	}
	error = give(request, result, sizeof(*result));
	kfree_data(result, sizeof(*result));
	return error;
}

/* ---- get ---------------------------------------------------------------------------------------------- */

static int
get(struct apple80211req *request)
{
	struct redwood_sta_state *state;
	int error = 0;

	switch (request->req_type) {
	case 43:
	case 44: {                              /* the driver's and the hardware's name: the text alone, in 256 bytes */
		static const char name[256] = "redwood: ath11k for the WCN6750";

		return give(request, name, sizeof(name));
	}
	case 12: {
		/*
		 * What the card can do, a bit each (apple80211_var.h: APPLE80211_CAP_*): the 13 bytes are the
		 * bits alone, from the first (the framework hands in a buffer that begins after the old
		 * header's version word). AES, AES-CCM, power management, short slot and preamble, WPA2, WME,
		 * short guard interval at 20 MHz. Read wrong, the framework sees no WPA2 and builds no join
		 * for a WPA2 network ("Unable to join").
		 */
		uint8_t bits[13] = { 0 };
		static const uint8_t has[] = { 2, 3, 6, 9, 10, 14, 17, 19 };

		for (unsigned i = 0; i < sizeof(has); i++) {
			bits[has[i] / 8] |= (uint8_t)(1u << (has[i] % 8));
		}
		return give(request, bits, sizeof(bits));
	}
	case 19: {                              /* the radio's power: one radio. Until the driver is up it counts as on: it */
		struct {                        /* is being brought up, and iOS asks this once, early */
			uint32_t version, radios, state[4];
		} answer = { .version = 1, .radios = 1, .state = { !redwood_sta_present() || redwood_sta_powered() } };

		return give(request, &answer, sizeof(answer));
	}
	case 15:                                /* the mode: a station (APPLE80211_M_STA) */
		request->req_val = 1;
		return 0;
	case 27:
	case 254: {                             /* the channels the radio may tune to */
		struct apple80211_channels *answer;
		struct redwood_sta_channel channels[96];
		unsigned count = redwood_sta_channels(channels, 96);

		if (count == 0 || (answer = kalloc_data(sizeof(*answer), Z_WAITOK | Z_ZERO)) == NULL) {
			return count == 0 ? ENXIO : ENOMEM;
		}
		answer->version = 1;
		answer->count = count;
		for (unsigned i = 0; i < count; i++) {
			channel_fill(&answer->channels[i], channels[i].frequency, channels[i].flags);
		}
		error = give(request, answer, sizeof(*answer));
		kfree_data(answer, sizeof(*answer));
		return error;
	}
	case 207: {                             /* ... and more of each */
		struct apple80211_channels_info *answer;
		struct redwood_sta_channel channels[96];
		unsigned count = redwood_sta_channels(channels, 96);

		if (count == 0 || (answer = kalloc_data(sizeof(*answer), Z_WAITOK | Z_ZERO)) == NULL) {
			return count == 0 ? ENXIO : ENOMEM;
		}
		answer->count = (uint16_t)count;
		for (unsigned i = 0; i < count; i++) {
			struct apple80211_channel channel;

			channel_fill(&channel, channels[i].frequency, channels[i].flags);
			answer->number[i] = (uint8_t)channel.channel;
			answer->dfs[i] = (channels[i].flags & REDWOOD_STA_CHANNEL_RADAR) != 0;
			answer->passive[i] = (channels[i].flags & REDWOOD_STA_CHANNEL_PASSIVE) != 0;
			answer->flags[i] = channel.flags;
		}
		error = give(request, answer, sizeof(*answer));
		kfree_data(answer, sizeof(*answer));
		return error;
	}
	case 106: {                             /* 1143: RSSI_BOUNDS, 16 bytes as IO80211 16.7 reads them (only with bit 0x20) */
		struct {
			uint32_t version;
			int32_t min, max, threshold;
		} answer = { .version = 1, .min = wifi_rssi_min, .max = wifi_rssi_max, .threshold = wifi_rssi_thr };
		static unsigned told;

		if (!(redwood_apple80211_sig() & 0x20)) {
			break;                          /* refused, as before 1143 */
		}
		if (told < 10) {
			told++;
			printf("SWEET WIFI 1143: RSSI_BOUNDS answered: min %d max %d threshold %d dBm\n", answer.min, answer.max,
			    answer.threshold);
		}
		return give(request, &answer, sizeof(answer));
	}
	case 21:                                /* how the last join went (0: not over yet) */
		lck_mtx_lock(&apple80211_lock);
		request->req_val = (int32_t)join_result;
		lck_mtx_unlock(&apple80211_lock);
		return 0;
	case 51: {                              /* 0193: the country code last set ("XX" until then) */
		struct {
			uint32_t version;
			uint8_t cc[4];
		} answer = { .version = 1 };

		if (!accepting()) {
			break;
		}
		lck_mtx_lock(&apple80211_lock);
		memcpy(answer.cc, country, sizeof(country));
		lck_mtx_unlock(&apple80211_lock);
		return give(request, &answer, sizeof(answer));
	}
	case 11: {                              /* what the last scan heard, one access point a request; EIO: that was all */
		struct redwood_sta_bss *bss = kalloc_data(sizeof(*bss), Z_WAITOK);
		unsigned index;
		bool there;

		if (bss == NULL) {
			return ENOMEM;
		}
		lck_mtx_lock(&apple80211_lock);
		index = result_next++;
		lck_mtx_unlock(&apple80211_lock);
		there = redwood_sta_heard(index, bss);
		error = there ? give_network(request, bss, index == 0) : EIO;
		kfree_data(bss, sizeof(*bss));
		return error;
	}
	default:
		break;
	}

	/* the rest is about the network joined, and there is none unless the station says so */
	if ((state = kalloc_data(sizeof(*state), Z_WAITOK)) == NULL) {
		return ENOMEM;
	}
	redwood_sta_state(state);
	if (state->phase == REDWOOD_STA_JOINED && (redwood_apple80211_sig() & 0x2) && wifi_live_rssi != 0) {
		state->bss.rssi = (int16_t)wifi_live_rssi;      /* 1051 */
	}
	switch (request->req_type) {
	case 13:                                /* the state: APPLE80211_S_INIT 0, _SCAN 1, _AUTH 2, _ASSOC 3, _RUN 4 */
		request->req_val = state->phase == REDWOOD_STA_JOINED ? 4 : state->phase == REDWOOD_STA_JOINING ? 2 :
		    state->phase == REDWOOD_STA_SCANNING ? 1 : 0;
		break;
	case 1:                                 /* its name; the length goes back in the request */
		if (state->phase != REDWOOD_STA_JOINED) {
			error = ENXIO;
		} else if ((error = give(request, state->bss.ssid, state->bss.ssid_length)) == 0) {
			request->req_len = state->bss.ssid_length;
		}
		break;
	case 9:                                 /* its access point's address */
		error = state->phase != REDWOOD_STA_JOINED ? ENXIO : give(request, state->bss.bssid, 6);
		break;
	case 4: {                               /* its channel */
		struct {
			uint32_t version;
			struct apple80211_channel channel;
		} answer = { .version = 1 };

		if (state->phase != REDWOOD_STA_JOINED) {
			error = ENXIO;
		} else {
			channel_fill(&answer.channel, state->bss.frequency, 0);
			error = give(request, &answer, sizeof(answer));
		}
		break;
	}
	case 16: {                              /* how strongly it is heard: dBm, per radio and all together, twice over */
		struct {
			uint32_t version, radios, unit;
			int32_t each[4], all, each_ext[4], all_ext;
		} answer = { .version = 1, .radios = 1, .each = { state->bss.rssi }, .all = state->bss.rssi,
			     .each_ext = { state->bss.rssi }, .all_ext = state->bss.rssi };

		error = state->phase != REDWOOD_STA_JOINED ? ENXIO : give(request, &answer, sizeof(answer));
		break;
	}
	case 103:                               /* all of it, as a scan's result */
		error = state->phase != REDWOOD_STA_JOINED ? ENXIO : give_network(request, &state->bss, false);
		break;
	case 2: {                               /* how it was joined: open system, then WPA2 with a passphrase */
		struct {
			uint32_t version, lower, upper, extended;
		} answer = { .version = 1, .lower = 1, .upper = 8 };

		error = state->phase != REDWOOD_STA_JOINED ? ENXIO : give(request, &answer, sizeof(answer));
		break;
	}
	default:
		error = EOPNOTSUPP;
		break;
	}
	kfree_data(state, sizeof(*state));
	return error;
}

/* ---- set ---------------------------------------------------------------------------------------------- */

static int
set_scan(const struct apple80211req *request, char *said, size_t said_size)
{
	struct apple80211_scan_request *asked;
	struct redwood_sta_scan *what;
	unsigned unknown = 0;
	int error;

	if (request->req_len != sizeof(*asked)) {
		return EINVAL;
	}
	if (!redwood_sta_present()) {
		return EBUSY;                   /* (the radio is still being brought up: the framework tries again) */
	}
	asked = kalloc_data(sizeof(*asked), Z_WAITOK);
	what = kalloc_data(sizeof(*what), Z_WAITOK | Z_ZERO);
	if (asked == NULL || what == NULL || (error = copyin(request->req_data, asked, sizeof(*asked))) != 0) {
		error = asked == NULL || what == NULL ? ENOMEM : error;
		goto out;
	}
	if (asked->ssid_length <= sizeof(what->ssid)) {
		what->ssid_length = (uint8_t)asked->ssid_length;
		memcpy(what->ssid, asked->ssid, asked->ssid_length);
	}
	what->passive = asked->scan_type == 2;
	what->dwell_ms = asked->dwell_ms;
	/* (more channels than there is room to name: every channel) */
	for (unsigned i = 0; i < asked->channel_count && i < 400 && asked->channel_count <= 64; i++) {
		unsigned frequency = channel_frequency(&asked->channels[i]);

		if (frequency != 0) {
			what->frequencies[what->frequency_count++] = (uint16_t)frequency;
		} else {
			unknown++;
		}
	}
	snprintf(said, said_size, " (a scan: type %u, %u channels of which %u are not the radio's, %u ms on each, a name of %u bytes, "
	    "%u more names, flags %#llx)", asked->scan_type, asked->channel_count, unknown, asked->dwell_ms, asked->ssid_length,
	    asked->name_count, asked->flags);
	if (asked->channel_count != 0 && asked->channel_count <= 64 && what->frequency_count == 0) {
		error = EINVAL;                 /* (nothing of what is asked for is a channel of this radio's) */
	} else {
		lck_mtx_lock(&apple80211_lock);
		result_next = 0;
		lck_mtx_unlock(&apple80211_lock);
		error = redwood_sta_ask_scan(what);
	}
out:
	if (asked != NULL) {
		kfree_data(asked, sizeof(*asked));
	}
	if (what != NULL) {
		kfree_data(what, sizeof(*what));
	}
	return error;
}

static int
set_join(const struct apple80211req *request, char *said, size_t said_size)
{
	struct apple80211_assoc *asked;
	struct redwood_sta_join *what;
	int error;

	if (request->req_len < offsetof(struct apple80211_assoc, rest)) {
		return EINVAL;
	}
	asked = kalloc_data(sizeof(*asked), Z_WAITOK | Z_ZERO);
	what = kalloc_data(sizeof(*what), Z_WAITOK | Z_ZERO);
	if (asked == NULL || what == NULL || (error = copyin(request->req_data, asked, offsetof(struct apple80211_assoc, rest))) != 0) {
		error = asked == NULL || what == NULL ? ENOMEM : error;
		goto out;
	}
	/* (lengths and kinds only: the name, the access point and the key stay out of the log) */
	snprintf(said, said_size, " (a join: a name of %u bytes, %s access point, authentication %#x/%#x, a key of %u bytes of kind %u)",
	    asked->ssid_length, asked->bssid[0] | asked->bssid[1] | asked->bssid[2] | asked->bssid[3] | asked->bssid[4] |
	    asked->bssid[5] ? "a given" : "any", asked->auth_lower, asked->auth_upper, asked->key_length, asked->key_cipher);
	if (asked->ssid_length == 0 || asked->ssid_length > sizeof(what->ssid)) {
		error = EINVAL;
		goto out;
	}
	what->ssid_length = (uint8_t)asked->ssid_length;
	memcpy(what->ssid, asked->ssid, asked->ssid_length);
	memcpy(what->bssid, asked->bssid, sizeof(what->bssid));
	if (asked->key_length == 0) {
		what->security = REDWOOD_STA_OPEN;
	} else if (asked->key_cipher == CIPHER_PASSPHRASE && asked->key_length >= 8 && asked->key_length <= 63) {
		what->security = REDWOOD_STA_WPA2_PSK;
		what->passphrase_length = (uint8_t)asked->key_length;
		memcpy(what->passphrase, asked->key, asked->key_length);
	} else if (asked->key_cipher == CIPHER_PMK && asked->key_length == sizeof(what->pmk)) {
		what->security = REDWOOD_STA_WPA2_PSK;
		memcpy(what->pmk, asked->key, sizeof(what->pmk));
	} else {
		error = EINVAL;
		goto out;
	}
	lck_mtx_lock(&apple80211_lock);
	join_result = 0;
	lck_mtx_unlock(&apple80211_lock);
	error = redwood_sta_ask_join(what);
out:
	if (asked != NULL) {
		memset_s(asked, sizeof(*asked), 0, sizeof(*asked));
		kfree_data(asked, sizeof(*asked));
	}
	if (what != NULL) {
		memset_s(what, sizeof(*what), 0, sizeof(*what));
		kfree_data(what, sizeof(*what));
	}
	return error;
}

static int
set(const struct apple80211req *request, char *said, size_t said_size)
{
	switch (request->req_type) {
	case 20:
		return set_join(request, said, said_size);
	case 22:                                /* leave the network */
		return redwood_sta_ask_leave();
	case 19: {                              /* the radio's power */
		struct {
			uint32_t version, radios, state[4];
		} asked;
		int error;

		if (request->req_len < sizeof(asked) || (error = copyin(request->req_data, &asked, sizeof(asked))) != 0) {
			return request->req_len < sizeof(asked) ? EINVAL : error;
		}
		snprintf(said, said_size, " (power: %s)", asked.state[0] ? "on" : "off");
		return redwood_sta_ask_power(asked.state[0] != 0);
	}
	case 10:
		return set_scan(request, said, said_size);
	case 383: {                             /* 0188: not built; its first 64 bytes once a run, to learn what it is */
		static bool shown;
		uint8_t head[64];
		unsigned length = request->req_len < sizeof(head) ? request->req_len : sizeof(head);

		if (!shown && request->req_data != USER_ADDR_NULL && copyin(request->req_data, head, length) == 0) {
			shown = true;
			printf("SWEET WIFI: set 383: %u bytes:", request->req_len);
			for (unsigned i = 0; i < length; i++) {
				printf(" %02x", head[i]);
			}
			printf("\n");
		}
		return accepting() ? 0 : EOPNOTSUPP;      /* 0193: taken, nothing done */
	}
	case 51: {                              /* 0193: the country code: kept, not sent to the firmware */
		static bool shown;
		struct {
			uint32_t version;
			uint8_t cc[4];
		} asked = { 0 };
		unsigned length = request->req_len < sizeof(asked) ? request->req_len : sizeof(asked);
		int error;

		if (!accepting()) {
			return EOPNOTSUPP;
		}
		if (length < 6 || (error = copyin(request->req_data, &asked, length)) != 0) {
			return length < 6 ? EINVAL : error;
		}
		lck_mtx_lock(&apple80211_lock);
		memcpy(country, asked.cc, sizeof(country));
		country[2] = 0;
		lck_mtx_unlock(&apple80211_lock);
		if (!shown) {
			const uint8_t *raw = (const uint8_t *)&asked;

			shown = true;
			printf("SWEET WIFI: country code set to %.2s (%u bytes: %02x %02x %02x %02x %02x %02x %02x %02x)\n",
			    (const char *)asked.cc, request->req_len, raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7]);
		}
		snprintf(said, said_size, " (country %.2s)", (const char *)asked.cc);
		return 0;
	}
	default:
		return EOPNOTSUPP;
	}
}

/* ---- events -------------------------------------------------------------------------------------------- */

static void
post(uint32_t code, const void *data, uint32_t length)
{
	struct kev_msg message = { .vendor_code = KEV_VENDOR_APPLE, .kev_class = KEV_IEEE80211_CLASS, .kev_subclass = 1,
		                   .event_code = code };
	char name[16];

	memcpy(name, interface_name, sizeof(name));
	message.dv[0].data_length = sizeof(name);
	message.dv[0].data_ptr = name;
	if (length != 0) {
		message.dv[1].data_length = length;
		message.dv[1].data_ptr = __DECONST(void *, data);
	}
	kev_post_msg(&message);
}

/*
 * 1051: wland's report of the joined network's signal (bsd/dev/flame_wlan.c, FW_RSSI, from wland's write with no
 * lock held; wland sends it only while joined, when it moved 2 dB or every 10 s).
 */
void redwood_apple80211_signal(int rssi, unsigned tx_rate);
void
redwood_apple80211_signal(int rssi, unsigned tx_rate)
{
	static unsigned said;
	int bits = redwood_apple80211_sig();

	if (rssi >= 0 || rssi < -127) {
		return;
	}
	wifi_live_rssi = rssi;
	if (bits & 0x1) {
		struct apple80211_rssi_changed event = { .has_rssi = 1, .rssi = rssi, .tx_rate = tx_rate,
			                                 .tx_fallback_rate = tx_rate, .rx_rate = tx_rate };

		post(EVENT_RSSI_CHANGED, &event, sizeof(event));
	}
	if (said < 40) {
		said++;
		printf("SWEET WIFI 1051: signal %d dBm, %u Mbit/s%s\n", rssi, tx_rate, bits & 0x1 ? ": RSSI_CHANGED posted" : "");
	}
}

/* What the station side says has happened (bsd/dev/redwood_sta.c calls this, on its thread or the station's). */
void
redwood_apple80211_event(enum redwood_sta_event event, int status)
{
	static unsigned said;

	if (said < 200) {
		said++;
		printf("apple80211: the station says: %s, %d\n", event == REDWOOD_STA_POWER ? "power" : event == REDWOOD_STA_SCAN_DONE ?
		    "a scan is over" : event == REDWOOD_STA_JOIN_DONE ? "a join is over" : "the link", status);
	}
	switch (event) {
	case REDWOOD_STA_POWER:
		post(EVENT_POWER_CHANGED, NULL, 0);
		break;
	case REDWOOD_STA_SCAN_DONE:
		lck_mtx_lock(&apple80211_lock);
		result_next = 0;
		lck_mtx_unlock(&apple80211_lock);
		post(EVENT_SCAN_DONE, NULL, 0);
		break;
	case REDWOOD_STA_JOIN_DONE:
		lck_mtx_lock(&apple80211_lock);
		join_result = status == 0 ? 1 : status == EAUTH ? 21 : status == ETIMEDOUT ? 9 : 2;
		lck_mtx_unlock(&apple80211_lock);
		post(EVENT_ASSOC_DONE, NULL, 0);
		break;
	case REDWOOD_STA_LINK: {
		struct apple80211_link_changed link = { .down = status != 1, .involuntary = status != 1 && status != 0,
			                                .reason = status != 1 && status != 0 ? 4 : 0 };      /* (4: gone, not asked) */
		struct redwood_sta_state *state = kalloc_data(sizeof(*state), Z_WAITOK | Z_ZERO);

		if (state != NULL) {
			redwood_sta_state(state);
			link.rssi = status == 1 ? state->bss.rssi : 0;
			kfree_data(state, sizeof(*state));
		}
		wifi_live_rssi = 0;                     /* 1051: a new link (or none): wland reports again */
		post(EVENT_SSID_CHANGED, NULL, 0);
		post(EVENT_BSSID_CHANGED, NULL, 0);
		post(EVENT_LINK_CHANGED, &link, sizeof(link));
		break;
	}
	}
}

/* ---- the requests as they arrive --------------------------------------------------------------------- */

/*
 * How often each kind of request came and how it fared: said when the run ends, because the
 * lines said along the way can be lost with the middle of a long log.
 */
static struct {
	int32_t type;
	bool set;
	unsigned times, answered;
	int last_error;
} kinds[96];
static unsigned kind_count;

static void
note(const struct apple80211req *request, bool is_set, int error)
{
	unsigned i;

	lck_mtx_lock(&apple80211_lock);
	for (i = 0; i < kind_count && !(kinds[i].type == request->req_type && kinds[i].set == is_set); i++) {
	}
	if (i < sizeof(kinds) / sizeof(kinds[0])) {
		if (i == kind_count) {
			kinds[kind_count++] = (typeof(kinds[0])) { .type = request->req_type, .set = is_set };
		}
		kinds[i].times++;
		kinds[i].answered += error == 0;
		kinds[i].last_error = error != 0 ? error : kinds[i].last_error;
	}
	lck_mtx_unlock(&apple80211_lock);
}

static void
summary_say(void)
{
	for (unsigned i = 0; i < kind_count; i++) {
		printf("apple80211: in this run: %s %d (%s) %u times, answered %u times%s", kinds[i].set ? "set" : "get", kinds[i].type,
		    type_name(kinds[i].type), kinds[i].times, kinds[i].answered, kinds[i].last_error == 0 ? "\n" : "");
		if (kinds[i].last_error != 0) {
			printf("; the last refusal: %d%s\n", kinds[i].last_error, kinds[i].last_error == EOPNOTSUPP ? " (not built)" : "");
		}
	}
}

void
redwood_apple80211_summary(void)
{
	static bool said;

	if (said) {
		return;
	}
	said = true;
	summary_say();
}

/* 0188: the summary every 60 s while requests come, too, so a run that never ends still says it */
static void
summary_now_and_then(void)
{
	static uint64_t last;
	uint64_t now = mach_absolute_time(), ns;

	if (last == 0) {
		last = now;
		return;
	}
	absolutetime_to_nanoseconds(now - last, &ns);
	if (ns >= 60ull * NSEC_PER_SEC) {
		last = now;
		summary_say();
	}
}

/* 0188: the first time each (get/set, request, result) comes, so a rare one is not lost behind the cap */
static bool
first_time(int32_t type, bool is_set, int error)
{
	static struct {
		int32_t type;
		bool set;
		int error;
	} seen[256];
	static unsigned seen_count;
	bool first = true;

	lck_mtx_lock(&apple80211_lock);
	for (unsigned i = 0; i < seen_count && first; i++) {
		first = !(seen[i].type == type && seen[i].set == is_set && seen[i].error == error);
	}
	if (first && seen_count < sizeof(seen) / sizeof(seen[0])) {
		seen[seen_count++] = (typeof(seen[0])) { .type = type, .set = is_set, .error = error };
	}
	lck_mtx_unlock(&apple80211_lock);
	return first;
}

/* An Apple80211 request for the radio's interface, from the process that is running now. */
errno_t
redwood_apple80211_ioctl(u_long command, void *data)
{
	static unsigned said;
	struct apple80211req *request = data;
	char who[32] = "?", what[200] = "";
	bool is_set = command == SIOCSA80211;
	int error;

	if (command != SIOCSA80211 && command != SIOCGA80211) {
		return EOPNOTSUPP;
	}
	proc_selfname(who, sizeof(who));
	error = is_set ? set(request, what, sizeof(what)) : get(request);
	note(request, is_set, error);
	summary_now_and_then();
	/* (a request that is repeated for ever is said a few thousand times; 0188: each kind's first always) */
	if (first_time(request->req_type, is_set, error) || said < 4000) {
		said += said < 4000;
		printf("apple80211: %s %s %d (%s), value %d, %u bytes%s: %s", who, is_set ? "sets" : "gets", request->req_type,
		    type_name(request->req_type), request->req_val, request->req_len, what, error == 0 ? "answered\n" : error ==
		    EOPNOTSUPP ? "REFUSED\n" : "");
		if (error != 0 && error != EOPNOTSUPP) {
			printf("error %d\n", error);
		}
	}
	return error;
}
