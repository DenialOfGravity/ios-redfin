/*
 * redfin (Pixel 5): in-call audio through the audio DSP's voice services. Patch 1065.
 *
 * A call's voice runs between the modem and the ADSP: the modem's vocoder hands its PCM to the DSP's voice
 * manager (MVM) and stream (CVS), and the voice processor (CVP) plays it on an AFE RX port and takes the
 * microphone from an AFE TX port. The modem owns the call's full-control sessions; Android's apps side (q6voice.c)
 * only creates passive sessions under the modem's VSID name, a CVP on the device ports, attaches it and starts
 * voice. Nothing did that here, so calls connected silent both ways (research/cc/P3-audio-result.md).
 *
 * Reference: the redbull kernel (LineageOS android_kernel_google_redbull) techpack/audio/dsp/q6voice.c
 * voc_start_voice_call() / voice_setup_vocproc() / voc_end_voice_call(), include/dsp/q6voice.h, and the phone's
 * mixer_paths_noextcodec_snd.xml: "voicemmode1-call handset/speaker" = QUIN_TDM_RX_0 (0x9040) + PRI_TDM_TX_0
 * (0x9001); "handset" = "Main AMP Enable Switch" only (the left amplifier 0x40, slot 0, the top/earpiece one),
 * "voice-speaker" -> "speaker-mono" = "R Main AMP" only (0x41, slot 1, bottom). QUIN_TDM_RX_0 runs 2 channels
 * (S32_LE), PRI_TDM_TX_0 one (S16_LE), both 48 kHz.
 *
 * kern.sweet_voice: 0 off (default: nothing here runs), 1 earpiece, 2 speaker. 0 -> 1/2 on this file's thread:
 *   1 the microphones' path held up (sweet_mic.c, as an open /dev/redwood-mic: AFE 0x9001 + the ALC5513)
 *   2 the speakers' port 0x9040 held and the call's amplifier on (sweet_sound.c: the watcher's with sound>=3,
 *     else the call brings the port up itself), at kern.sweet_voice_below_ear / _spk
 *   3 MVM version (VSS_IVERSION_CMD_GET), MVM and CVS passive sessions named after the VSID ("11C05000" =
 *     VoiceMMode1), MVM dual control, CVP full control V3 (V2 if CVD < 2.2): tx 0x9001, rx 0x9040, topology
 *     NONE, network NONE, EC internal mixing; CVD >= 2.2: device channels (tx 1, rx 1) and topology commit;
 *     CVD >= 2.3: the RX/TX endpoint media formats; CVP enable, MVM attach vocproc, CVS TX unmute, MVM start voice
 * 1 <-> 2: only the amplifier changes. -> 0: MVM stop voice, detach vocproc, CVP and MVM destroyed (CVS kept for
 * the next call, as q6voice does for modem voice), the amplifiers and ports let go (released only if nothing
 * else holds them: media keeps 0x9040, an open /dev/redwood-mic keeps 0x9001).
 * Variants (kern.sweet_voice_opts, bits; 0 = the q6voice sequence above):
 *   1 no dual control       2 behave as CVD < 2.2 (V2 create; no device channels / commit)
 *   4 no media formats      8 also MFC output 2 ch (as q6voice for CVP >= 2)      16 topologies TX SM ECNS V2 /
 *   RX default instead of NONE      32 destroy the CVS too at the end      64 RX media format 1 ch / 16 bits
 * 1074: kern.sweet_voice_auto (sweet_qmux.c) drives the same switch from the modem's call states (sweet_voice_auto()).
 * kern.sweet_voice_vsid: the session name's VSID (default 0x11C05000; 0 = "default modem voice").
 * A failed start is undone and kern.sweet_voice reads 0 again. Never returns to fastboot.
 *
 * Log lines: "SWEET VOICE: ...".
 *
 * 1086 (route and gains; self-contained, deletable in one commit; all defaults = 1085's behaviour):
 *   kern.sweet_voice_route 0 (default: unset) / 1 earpiece / 2 speaker: a running call is switched at once (the same
 *     amplifier-only switch as kern.sweet_voice 1 <-> 2: the CVP stays, the call is not torn down), whoever started it;
 *     kern.sweet_voice_auto starts the next call in this mode instead of its own. Anybody may write it.
 *     The same from iOS: /dev/redwood-audio ioctl REDWOOD_AUDIO_SET_CALL_ROUTE (RedwoodAudio, flag file
 *     /var/local/redwood-call-route): applied to a running call; with no call it is kept for the auto start only if it
 *     came within ROUTE_IOS_FRESH_MS before it (iOS routes a new call as it starts; an older value is media's route).
 *   kern.sweet_voice_gain_rx (dB, default 0, -30..+24): the call's amplifier level relative to kern.sweet_voice_below_ear
 *     / _spk (louder = positive); sweet_sound.c's hard cap (6 dB under Android's level) still applies.
 *   kern.sweet_voice_gain_tx (dB, default 0, -12..+24): the mic's ADC gain during a call relative to the boot-arg
 *     micgain (0.75 dB steps, sweet_mic.c redwood_mic_voice_gain()); back to micgain when the call lets the mic go.
 *
 * 1090 (experiment E1 of research/cc/BBA-design-study.md; the AP side in bsd/dev/sweet_vproxy.c):
 *   kern.sweet_voice_proxy 0 (default: today's direct path, nothing below runs) / 1 proxy loopback: read when a call's
 *     voice session starts (by hand or kern.sweet_voice_auto); a change applies to the next call. With 1 the AFE RT
 *     proxy ports come up first (sweet_vproxy_prepare()), the session starts as today on 0x9001/0x9040, then the CVP
 *     is switched (q6voice.c voc_disable_device()/voc_enable_device(): MVM PAUSE_VOICE, CVP SET_DEVICE_V3 tx 0x2001 rx
 *     0x2000, endpoint media info 1 ch x 16 bit at the proxy's rate, topology commit, MVM START_VOICE) and the AP's
 *     data path starts. Any failure: back to 0x9001/0x9040 in the same start (switched back, or the session made again
 *     on them), the reason logged ("SWEET VOICE PROXY: FALLBACK ...").
 *   1091: kern.sweet_voice_proxy 2 = as 1, but the PCM goes through /dev/redwood-voice (bsd/dev/sweet_voicedev.c).
 *   kern.sweet_voice_proxy_opts (bits, 0 default): 1 the CVP created on the proxy ports from the start (no switch);
 *     2 8 kHz instead of 16 kHz; 4 host-PCM probe: one VSS_IVPCM_CMD_START_V2 (TX tap, read only, no memory) to the
 *     CVP after START_VOICE, its status logged only (stopped again if it was taken).
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/redwood_audio.h>

extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern void thread_deallocate(thread_t);

#define SAY(...)                printf("SWEET VOICE: " __VA_ARGS__)

#define APR_SVC_MVM             9
#define APR_SVC_CVS             10
#define APR_SVC_CVP             11
#define APR_BASIC_RSP_RESULT    0x000110e8u
#define APRV2_IBASIC_EVT_ACCEPTED 0x000100beu     /* "queued": sweet_adsp.c waits past it for the result */
#define APR_WAIT_MS             1000
#define VOICE_INDEX             5               /* q6voice.c voice_get_idx_for_session(VOICEMMODE1): the source port */

#define VSS_IVERSION_CMD_GET                            0x00011378u
#define VSS_IVERSION_RSP_GET                            0x00011379u
#define VSS_IMVM_CMD_CREATE_PASSIVE_CONTROL_SESSION     0x000110ffu
#define VSS_IMVM_CMD_SET_POLICY_DUAL_CONTROL            0x00011327u
#define VSS_ISTREAM_CMD_CREATE_PASSIVE_CONTROL_SESSION  0x00011140u
#define VSS_IVOCPROC_CMD_CREATE_FULL_CONTROL_SESSION_V2 0x000112bfu
#define VSS_IVOCPROC_CMD_CREATE_FULL_CONTROL_SESSION_V3 0x00013169u
#define VSS_IVOCPROC_CMD_TOPOLOGY_SET_DEV_CHANNELS      0x00013199u
#define VSS_IVOCPROC_CMD_TOPOLOGY_COMMIT                0x00013198u
#define VSS_ICOMMON_CMD_SET_PARAM_V2                    0x0001133du
#define VSS_IVOCPROC_CMD_ENABLE                         0x000100c6u
#define VSS_IMVM_CMD_ATTACH_VOCPROC                     0x0001123eu
#define VSS_IMVM_CMD_DETACH_VOCPROC                     0x0001123fu
#define VSS_IVOLUME_CMD_MUTE_V2                         0x0001138bu
#define VSS_IMVM_CMD_START_VOICE                        0x00011190u
#define VSS_IMVM_CMD_STOP_VOICE                         0x00011192u
#define APRV2_IBASIC_CMD_DESTROY_SESSION                0x0001003cu

#define VSS_IVOCPROC_TOPOLOGY_ID_NONE                   0x00010f70u
#define VSS_IVOCPROC_TOPOLOGY_ID_TX_SM_ECNS_V2          0x00010f89u
#define VSS_IVOCPROC_TOPOLOGY_ID_RX_DEFAULT             0x00010f77u
#define VSS_ICOMMON_CAL_NETWORK_ID_NONE                 0x0001135eu
#define VSS_IVOCPROC_VOCPROC_MODE_EC_INT_MIXING         0x00010f7cu
#define VSS_IVOCPROC_PORT_ID_NONE                       0xffffu
#define VSS_MODULE_CVD_GENERIC                          0x0001316eu
#define VSS_PARAM_TX_PORT_ENDPOINT_MEDIA_INFO           0x00013253u
#define VSS_PARAM_RX_PORT_ENDPOINT_MEDIA_INFO           0x00013254u
#define AUDPROC_MODULE_ID_MFC                           0x00010912u
#define AUDPROC_PARAM_ID_MFC_OUTPUT_MEDIA_FORMAT        0x00010913u

#define RX_PORT                 0x9040          /* QUIN_TDM_RX_0: the two CS35L41 */
#define TX_PORT                 0x9001          /* PRI_TDM_TX_0: the ALC5513's DMIC 1 */
#define RATE                    48000
#define PCM_CHANNEL_FL          1
#define PCM_CHANNEL_FR          2
#define PCM_CHANNEL_FC          3
#define SESSION_NAME_LEN        20
#define AMP_EAR                 1u              /* sweet_cs35l41.c amps[0] 0x40, slot 0 */
#define AMP_SPEAKER             2u              /* amps[1] 0x41, slot 1 */

#define OPT_NO_DUAL             1u
#define OPT_OLD_CVD             2u
#define OPT_NO_MEDIA_FMT        4u
#define OPT_MFC                 8u
#define OPT_CAL_TOPOLOGIES      16u
#define OPT_DESTROY_CVS         32u
#define OPT_RX_MONO16           64u
/*
 * 1066 (diagnostics and q6voice steps not in 1065's sequence; the unasked events of MVM/CVS/CVP are always logged:
 * "SWEET VOICE: event ...", the first 40 of a call then every 50th, counts at STOP):
 *   128 MVM: listen for the voice activity event class (q6voice's mic-break listener)
 *   256 CVS: RX DTMF detection on: keys pressed on the far phone log "RX DTMF <low>/<high> Hz" = the downlink reaches the DSP
 *   512 CVS and CVP: RX explicitly unmuted (VSS_IVOLUME_CMD_MUTE_V2 direction RX)
 *  1024 CVS: TTY mode 0 (q6voice voice_send_tty_mode_cmd(), always sent there, before START)
 */
#define OPT_LISTEN              128u
#define OPT_RX_DTMF             256u
#define OPT_RX_UNMUTE           512u
#define OPT_TTY                 1024u

#define VSS_INOTIFY_CMD_LISTEN_FOR_EVENT_CLASS          0x00012e56u
#define VSS_INOTIFY_CMD_CANCEL_EVENT_CLASS              0x00012e57u
#define VSS_ICOMMON_EVENT_CLASS_VOICE_ACTIVITY_UPDATE   0x000131efu
#define VSS_ICOMMON_EVT_VOICE_ACTIVITY_UPDATE           0x000131f0u
#define VSS_ISTREAM_CMD_SET_RX_DTMF_DETECTION           0x00011027u
#define VSS_ISTREAM_EVT_RX_DTMF_DETECTED                0x0001101au
#define VSS_ISTREAM_EVT_READY                           0x000110fcu
#define VSS_ISTREAM_EVT_NOT_READY                       0x000110fdu
#define VSS_ISTREAM_CMD_SET_TTY_MODE                    0x00011196u

static int voice_target;                        /* kern.sweet_voice: 0 off, 1 earpiece, 2 speaker */
static int voice_state;                         /* what runs */
static uint32_t voice_opts, voice_vsid = 0x11c05000u, below_ear = 240, below_speaker = 160, below_applied;
static uint16_t mvm_handle, cvs_handle, cvp_handle;
static int cvd_major = -1, cvd_minor;
static bool mic_held, port_held, dual_done, attached, started;
static bool thread_up;
static bool auto_owned;                         /* 1074: the running session was started by kern.sweet_voice_auto */
static int voice_wake;
static uint32_t starts, failures;
static uint32_t cvs_vsid;                       /* 1066: the VSID the kept CVS was named after */
static bool listening, dtmf_on;
static int voice_route;                         /* 1086: kern.sweet_voice_route: 0 unset, 1 earpiece, 2 speaker */
static uint64_t route_ios_at;                   /* 1086: when iOS (the ioctl) set it last, mach_absolute_time(); 0 = by hand */
static int gain_rx_db, gain_tx_db;              /* 1086 */
#define ROUTE_IOS_FRESH_MS      15000
static volatile uint32_t ev_all, ev_ready, ev_not_ready, ev_activity, ev_dtmf, ev_other;

/* 1090: the voice proxy (bsd/dev/sweet_vproxy.c) */
extern bool sweet_vproxy_prepare(int mode, uint32_t opts);
extern bool sweet_vproxy_run(void);
extern void sweet_vproxy_stop(const char *why);
extern void sweet_vproxy_dtmf(uint32_t low, uint32_t high);
extern uint32_t sweet_vproxy_rate(void);
extern void sweet_qmux_vocoder_note(uint32_t rate_hz, int up);  /* 1093: sweet_qmux.c (acts only with kern.sweet_qmi_vocoder 1) */
#define PROXY_RX_PORT           0x2000          /* RT_PROXY_PORT_001_RX: the downlink, read by the AP */
#define PROXY_TX_PORT           0x2001          /* RT_PROXY_PORT_001_TX: the uplink, written by the AP */
#define POPT_CREATE             1u
#define POPT_8K                 2u
#define POPT_HOSTPCM            4u
#define VSS_IMVM_CMD_PAUSE_VOICE                        0x0001137du
#define VSS_IVOCPROC_CMD_SET_DEVICE_V3                  0x0001316au
#define VSS_IVPCM_CMD_START_V2                          0x00011339u
#define VSS_IVPCM_CMD_STOP                              0x0001100bu
#define VSS_IVPCM_TAP_POINT_TX_DEFAULT                  0x00011289u
static int voice_proxy;                         /* kern.sweet_voice_proxy */
static uint32_t proxy_opts;                     /* kern.sweet_voice_proxy_opts */
static bool proxy_up;                           /* this call runs on the proxy ports (the AFE side is up) */
static bool proxy_refused;                      /* this start: the proxy failed once, the session is made again directly */

/* 1066: what MVM/CVS/CVP send unasked (sweet_adsp.c's link thread). Answers to our own commands are not seen here. */
static bool
voice_event(uint8_t service, uint16_t port, uint32_t opcode, uint32_t token, const uint32_t *payload, uint32_t length)
{
	const char *svc = service == APR_SVC_MVM ? "MVM" : service == APR_SVC_CVS ? "CVS" : "CVP";
	uint32_t n = ++ev_all;
	bool say = n <= 40 || n % 50 == 0;

	if (opcode == VSS_ISTREAM_EVT_READY) {
		ev_ready++;
		say = true;
		SAY("event %u: %s port %u: STREAM READY (the modem's vocoder runs on this session)\n", n, svc, port);
	} else if (opcode == VSS_ISTREAM_EVT_NOT_READY) {
		ev_not_ready++;
		say = true;
		SAY("event %u: %s port %u: STREAM NOT READY\n", n, svc, port);
	} else if (opcode == VSS_ISTREAM_EVT_RX_DTMF_DETECTED) {
		ev_dtmf++;
		say = true;
		SAY("event %u: %s port %u: RX DTMF %u/%u Hz (the downlink reaches the DSP)\n", n, svc, port,
		    payload[0] & 0xffff, payload[0] >> 16);
		if (proxy_up) {
			sweet_vproxy_dtmf(payload[0] & 0xffff, payload[0] >> 16);      /* 1090: the latency line */
		}
	} else if (opcode == VSS_ICOMMON_EVT_VOICE_ACTIVITY_UPDATE) {
		ev_activity++;
		if (say) {
			SAY("event %u: %s port %u: voice activity %#x\n", n, svc, port, payload[0]);
		}
	} else {
		ev_other++;
		if (say) {
			SAY("event %u: %s port %u: opcode %#x token %u, %u bytes: %#x %#x %#x\n", n, svc, port, opcode, token, length,
			    payload[0], payload[1], payload[2]);
		}
	}
	return true;
}
static LCK_GRP_DECLARE(voice_lock_group, "sweet-voice");
static LCK_MTX_DECLARE(thread_lock, &voice_lock_group);

static const char *
dsp_error(uint32_t status)
{
	static const char *const names[] = { "ok", "failed", "bad parameter", "unsupported", "version", "unexpected", "panic",
		                             "no resource", "bad handle", "already", "not ready", "pending", "busy", "aborted",
		                             "preempted", "continue", "immediate", "not implemented", "need more", "no memory",
		                             "not there" };

	return status < sizeof(names) / sizeof(names[0]) ? names[status] : "unknown error";
}

/*
 * One command to a voice service, from our source port VOICE_INDEX to `handle' (0: the service itself), its answer
 * waited for: true for the basic answer with status 0 (*answer has it, with the new handle in answer->port), or for
 * the opcode `wanted'.
 */
static bool
vcmd(const char *what, uint8_t service, uint16_t handle, uint32_t opcode, const void *payload, uint32_t length, uint32_t wanted,
    struct redwood_apr_answer *answer)
{
	struct redwood_apr_answer local;

	if (answer == NULL) {
		answer = &local;
	}
	bzero(answer, sizeof(*answer));
	if (!redwood_adsp_apr_ports(service, VOICE_INDEX, handle, 0, opcode, payload, length, APR_WAIT_MS, answer)) {
		SAY("%s (opcode %#x to %s handle %u): NO ANSWER\n", what, opcode, service == APR_SVC_MVM ? "MVM" :
		    service == APR_SVC_CVS ? "CVS" : "CVP", handle);
		return false;
	}
	if (answer->opcode == APR_BASIC_RSP_RESULT && answer->words[0] == opcode) {
		if (answer->words[1] != 0) {
			SAY("%s: REFUSED: %s (%u)\n", what, dsp_error(answer->words[1]), answer->words[1]);
			return false;
		}
		SAY("%s: ok\n", what);
		return wanted == 0;
	}
	if (answer->opcode == APRV2_IBASIC_EVT_ACCEPTED && answer->words[0] == opcode) {
		/* accepted (4 bytes: the opcode), and its result did not come within APR_WAIT_MS: taken as ok */
		SAY("%s: accepted, no result within %u ms: taken as ok\n", what, APR_WAIT_MS);
		return wanted == 0;
	}
	if (wanted != 0 && answer->opcode == wanted) {
		return true;
	}
	if (answer->length >= 8) {
		SAY("%s: unexpected answer: opcode %#x, %u bytes: %#x %#x\n", what, answer->opcode, answer->length,
		    answer->words[0], answer->words[1]);
	} else {
		SAY("%s: unexpected answer: opcode %#x, %u bytes: %#x\n", what, answer->opcode, answer->length,
		    answer->words[0]);
	}
	return false;
}

/* VSS_ICOMMON_CMD_SET_PARAM_V2, in band (mem_handle 0), one parameter with a v1 header, to the CVP */
static bool
cvp_param(const char *what, uint32_t module, uint32_t param, const void *data, uint16_t size)
{
	struct {
		uint32_t mem_handle;
		uint64_t mem_address;
		uint32_t mem_size;
		uint32_t module, param;
		uint16_t size, reserved;
		uint8_t data[96];
	} __attribute__((packed)) p;

	if (size > sizeof(p.data)) {
		return false;
	}
	bzero(&p, sizeof(p));
	p.mem_size = 12u + size;
	p.module = module;
	p.param = param;
	p.size = size;
	memcpy(p.data, data, size);
	return vcmd(what, APR_SVC_CVP, cvp_handle, VSS_ICOMMON_CMD_SET_PARAM_V2, &p, 28u + size, 0, NULL);
}

static void
cvd_version(void)
{
	struct redwood_apr_answer answer;
	char version[32];
	const char *c;
	int major = 0, minor = 0;

	if (cvd_major >= 0) {
		return;
	}
	if (!vcmd("MVM: CVD version", APR_SVC_MVM, 0, VSS_IVERSION_CMD_GET, NULL, 0, VSS_IVERSION_RSP_GET, &answer)) {
		SAY("CVD version unknown: taken as 0.0 (q6voice's default)\n");
		cvd_major = 0;
		cvd_minor = 0;
		return;
	}
	bzero(version, sizeof(version));
	memcpy(version, answer.words, sizeof(version) - 1);
	for (c = version; *c >= '0' && *c <= '9'; c++) {
		major = major * 10 + (*c - '0');
	}
	if (*c == '.') {
		for (c++; *c >= '0' && *c <= '9'; c++) {
			minor = minor * 10 + (*c - '0');
		}
	}
	cvd_major = major;
	cvd_minor = minor;
	SAY("CVD version \"%s\" (%d.%d)\n", version, major, minor);
}

static bool
cvd_at_least(int major, int minor)
{
	return cvd_major > major || (cvd_major == major && cvd_minor >= minor);
}

static uint32_t
amps_for(int state)
{
	return state == 2 ? AMP_SPEAKER : state == 1 ? AMP_EAR : 0;
}

static uint32_t
below_for(int state)
{
	int below = (int)(state == 2 ? below_speaker : below_ear) - gain_rx_db * 8;   /* 1086: gain_rx */

	return below < 0 ? 0 : (uint32_t)below;
}

/* the sessions taken down, the ports and amplifiers let go: whatever of it is there */
/*
 * The CVP's endpoint media formats. 1090: proxy = the RT proxy ports (1 ch x 16 bit at `rate' both ways); otherwise
 * today's 0x9040/0x9001 formats, with today's log lines.
 */
static void
media_formats(bool proxy, uint32_t rate)
{
	bool rx_mono = (voice_opts & OPT_RX_MONO16) != 0;
	struct {
		uint32_t port;
		uint16_t channels, bits;
		uint32_t rate;
		uint8_t map[32];
	} __attribute__((packed)) rx, tx;

	bzero(&rx, sizeof(rx));
	bzero(&tx, sizeof(tx));
	if (proxy) {
		char what[80];

		rx.port = PROXY_RX_PORT;
		rx.channels = 1;
		rx.bits = 16;
		rx.rate = rate;
		rx.map[0] = PCM_CHANNEL_FC;
		tx = rx;
		tx.port = PROXY_TX_PORT;
		snprintf(what, sizeof(what), "CVP: RX endpoint 0x2000, 1 ch x 16 bits, %u Hz", rate);
		(void)cvp_param(what, VSS_MODULE_CVD_GENERIC, VSS_PARAM_RX_PORT_ENDPOINT_MEDIA_INFO, &rx, sizeof(rx));
		snprintf(what, sizeof(what), "CVP: TX endpoint 0x2001, 1 ch x 16 bits, %u Hz", rate);
		(void)cvp_param(what, VSS_MODULE_CVD_GENERIC, VSS_PARAM_TX_PORT_ENDPOINT_MEDIA_INFO, &tx, sizeof(tx));
		return;
	}
	rx.port = RX_PORT;
	rx.channels = rx_mono ? 1 : 2;
	rx.bits = rx_mono ? 16 : 32;
	rx.rate = RATE;
	rx.map[0] = rx_mono ? PCM_CHANNEL_FC : PCM_CHANNEL_FL;
	rx.map[1] = rx_mono ? 0 : PCM_CHANNEL_FR;
	tx.port = TX_PORT;
	tx.channels = 1;
	tx.bits = 16;
	tx.rate = RATE;
	tx.map[0] = PCM_CHANNEL_FC;
	(void)cvp_param(rx_mono ? "CVP: RX endpoint 0x9040, 1 ch x 16 bits, 48 kHz" : "CVP: RX endpoint 0x9040, 2 ch x 32 bits, 48 kHz",
	    VSS_MODULE_CVD_GENERIC, VSS_PARAM_RX_PORT_ENDPOINT_MEDIA_INFO, &rx, sizeof(rx));
	(void)cvp_param("CVP: TX endpoint 0x9001, 1 ch x 16 bits, 48 kHz", VSS_MODULE_CVD_GENERIC,
	    VSS_PARAM_TX_PORT_ENDPOINT_MEDIA_INFO, &tx, sizeof(tx));
}

/*
 * 1090: the running call's CVP moved to the proxy ports (to_proxy) or back to 0x9001/0x9040 (q6voice.c
 * voc_disable_device() + voc_enable_device(): pause, set device V3, media info, commit, start).
 */
static bool
cvp_switch(bool to_proxy, uint32_t rate)
{
	struct {
		uint16_t tx_port;
		uint32_t tx_topology;
		uint16_t rx_port;
		uint32_t rx_topology, mode;
		uint16_t ec_ref_port;
	} __attribute__((packed)) dev;
	uint16_t tx = to_proxy ? PROXY_TX_PORT : TX_PORT, rx = to_proxy ? PROXY_RX_PORT : RX_PORT;
	char what[80];

	if (!vcmd("MVM: pause voice", APR_SVC_MVM, mvm_handle, VSS_IMVM_CMD_PAUSE_VOICE, NULL, 0, 0, NULL)) {
		printf("SWEET VOICE PROXY: MVM pause: FAILED\n");
		return false;
	}
	bzero(&dev, sizeof(dev));
	dev.tx_port = tx;
	dev.rx_port = rx;
	dev.tx_topology = (voice_opts & OPT_CAL_TOPOLOGIES) ? VSS_IVOCPROC_TOPOLOGY_ID_TX_SM_ECNS_V2 : VSS_IVOCPROC_TOPOLOGY_ID_NONE;
	dev.rx_topology = (voice_opts & OPT_CAL_TOPOLOGIES) ? VSS_IVOCPROC_TOPOLOGY_ID_RX_DEFAULT : VSS_IVOCPROC_TOPOLOGY_ID_NONE;
	dev.mode = VSS_IVOCPROC_VOCPROC_MODE_EC_INT_MIXING;
	dev.ec_ref_port = VSS_IVOCPROC_PORT_ID_NONE;
	snprintf(what, sizeof(what), "CVP: set device V3 tx %#x rx %#x", tx, rx);
	if (!vcmd(what, APR_SVC_CVP, cvp_handle, VSS_IVOCPROC_CMD_SET_DEVICE_V3, &dev, sizeof(dev), 0, NULL)) {
		printf("SWEET VOICE PROXY: MVM pause: ok; CVP set device V3 tx %#x rx %#x: FAILED\n", tx, rx);
		return false;
	}
	if (!(voice_opts & OPT_NO_MEDIA_FMT)) {
		media_formats(to_proxy, rate);
	}
	(void)vcmd("CVP: topology commit", APR_SVC_CVP, cvp_handle, VSS_IVOCPROC_CMD_TOPOLOGY_COMMIT, NULL, 0, 0, NULL);
	if (!vcmd("MVM: START_VOICE", APR_SVC_MVM, mvm_handle, VSS_IMVM_CMD_START_VOICE, NULL, 0, 0, NULL)) {
		printf("SWEET VOICE PROXY: MVM pause: ok; CVP set device V3 tx %#x rx %#x: ok; media info: sent; commit: sent; MVM start: "
		    "FAILED\n", tx, rx);
		return false;
	}
	printf("SWEET VOICE PROXY: MVM pause: ok; CVP set device V3 tx %#x rx %#x: ok; media info: %s; commit: ok; MVM start: ok\n",
	    tx, rx, (voice_opts & OPT_NO_MEDIA_FMT) ? "not sent (opts 4)" : "sent");
	return true;
}

/* 1090, opts 4: is host PCM there? One VSS_IVPCM_CMD_START_V2 (TX tap, read only, no memory): only its status logged. */
static void
hostpcm_probe(uint32_t rate)
{
	struct {
		uint32_t mem_handle, taps;
		uint32_t tap_point;
		uint16_t direction, rate, duration;
	} __attribute__((packed)) start = { 0, 1, VSS_IVPCM_TAP_POINT_TX_DEFAULT, 0, (uint16_t)rate, 0 };
	struct redwood_apr_answer answer;

	bzero(&answer, sizeof(answer));
	if (!redwood_adsp_apr_ports(APR_SVC_CVP, VOICE_INDEX, cvp_handle, 0, VSS_IVPCM_CMD_START_V2, &start, sizeof(start),
	    APR_WAIT_MS, &answer)) {
		printf("SWEET VOICE PROXY: host PCM probe: VSS_IVPCM_CMD_START_V2 -> NO ANSWER\n");
		return;
	}
	if (answer.opcode == APR_BASIC_RSP_RESULT && answer.words[0] == VSS_IVPCM_CMD_START_V2) {
		printf("SWEET VOICE PROXY: host PCM probe: VSS_IVPCM_CMD_START_V2 -> %s (%u)%s\n", dsp_error(answer.words[1]),
		    answer.words[1], answer.words[1] == 0 ? ": TAKEN (host PCM exists); stopped again" :
		    answer.words[1] == 3 || answer.words[1] == 17 ? ": not supported here" : "");
		if (answer.words[1] == 0) {
			(void)vcmd("CVP: host PCM stopped (probe)", APR_SVC_CVP, cvp_handle, VSS_IVPCM_CMD_STOP, NULL, 0, 0, NULL);
		}
		return;
	}
	printf("SWEET VOICE PROXY: host PCM probe: VSS_IVPCM_CMD_START_V2 -> answer opcode %#x, %u bytes: %#x %#x\n", answer.opcode,
	    answer.length, answer.words[0], answer.words[1]);
}

static void
voice_down(const char *why)
{
	uint16_t handle;

	SAY("STOP (%s)\n", why);
	SAY("events this call: %u (stream ready %u, not ready %u, activity %u, RX DTMF %u, other %u)\n", ev_all, ev_ready,
	    ev_not_ready, ev_activity, ev_dtmf, ev_other);
	if (dtmf_on && cvs_handle != 0) {
		uint32_t enable = 0;

		(void)vcmd("CVS: RX DTMF detection off", APR_SVC_CVS, cvs_handle, VSS_ISTREAM_CMD_SET_RX_DTMF_DETECTION, &enable,
		    sizeof(enable), 0, NULL);
	}
	dtmf_on = false;
	if (listening && mvm_handle != 0) {
		uint32_t class_id = VSS_ICOMMON_EVENT_CLASS_VOICE_ACTIVITY_UPDATE;

		(void)vcmd("MVM: stop listening", APR_SVC_MVM, mvm_handle, VSS_INOTIFY_CMD_CANCEL_EVENT_CLASS, &class_id, sizeof(class_id),
		    0, NULL);
	}
	listening = false;
	if (started) {
		(void)vcmd("MVM: stop voice", APR_SVC_MVM, mvm_handle, VSS_IMVM_CMD_STOP_VOICE, NULL, 0, 0, NULL);
		started = false;
	}
	if (attached) {
		handle = cvp_handle;
		(void)vcmd("MVM: detach vocproc", APR_SVC_MVM, mvm_handle, VSS_IMVM_CMD_DETACH_VOCPROC, &handle, sizeof(handle), 0, NULL);
		attached = false;
	}
	if (cvp_handle != 0) {
		(void)vcmd("CVP: destroyed", APR_SVC_CVP, cvp_handle, APRV2_IBASIC_CMD_DESTROY_SESSION, NULL, 0, 0, NULL);
		cvp_handle = 0;
	}
	if (cvs_handle != 0 && (voice_opts & OPT_DESTROY_CVS)) {
		(void)vcmd("CVS: destroyed", APR_SVC_CVS, cvs_handle, APRV2_IBASIC_CMD_DESTROY_SESSION, NULL, 0, 0, NULL);
		cvs_handle = 0;
	}
	if (mvm_handle != 0) {
		(void)vcmd("MVM: destroyed", APR_SVC_MVM, mvm_handle, APRV2_IBASIC_CMD_DESTROY_SESSION, NULL, 0, 0, NULL);
		mvm_handle = 0;
	}
	dual_done = false;
	if (proxy_up) {
		sweet_vproxy_stop(why);         /* 1090: after the CVP is gone: the data thread, the call mode, the AFE ports */
		proxy_up = false;
	}
	if (port_held) {
		(void)redwood_sound_voice(0, 0);
		port_held = false;
		SAY("speaker port / amplifiers let go\n");
	}
	if (mic_held) {
		(void)redwood_mic_voice_hold(false, 0);
		mic_held = false;
		SAY("mic path let go\n");
	}
	voice_state = 0;
	sweet_qmux_vocoder_note(0, 0);  /* 1093 */
	SAY("OFF\n");
}

static bool
voice_up(int target)
{
	struct redwood_apr_answer answer;
	char name[SESSION_NAME_LEN];
	bool old_cvd, proxy_tried = false;
	uint16_t handle;
	const char *step = "?";

	starts++;
	SAY("START %u: %s, opts %#x, VSID %#x, amplifier level %u/8 dB under Android's\n", starts, target == 2 ? "speaker" : "earpiece",
	    voice_opts, voice_vsid, below_for(target));

	/* 1 the microphones */
	step = "mic path";
	mic_held = true;
	if (!redwood_mic_voice_hold(true, 3000)) {
		SAY("mic path (PRI_TDM_TX_0 0x9001) NOT UP: the other side will not hear us (going on)\n");
	} else {
		SAY("mic path up: PRI_TDM_TX_0 0x9001 runs\n");
	}

	/* 2 the speakers' port and the call's amplifier */
	step = "speaker port";
	port_held = true;
	below_applied = below_for(target);
	if (!redwood_sound_voice(amps_for(target), below_applied)) {
		SAY("speaker port QUIN_TDM_RX_0 0x9040 NOT RUNNING: we will not hear the other side (going on)\n");
	} else {
		SAY("speaker port 0x9040 runs; amplifier %s held for the call\n", target == 2 ? "0x41 (speaker)" : "0x40 (earpiece)");
	}

	/* 1090: the AFE proxy ports first (the CVP is created on them, or switched to them after START_VOICE) */
	proxy_up = false;
	if (voice_proxy != 0 && !proxy_refused) {
		printf("SWEET VOICE PROXY: call start with kern.sweet_voice_proxy %d, opts %#x: %s\n", voice_proxy, proxy_opts,
		    (proxy_opts & POPT_CREATE) ? "CVP created on the proxy ports" : "CVP switched to the proxy ports after START_VOICE");
		proxy_up = sweet_vproxy_prepare(voice_proxy, proxy_opts);
		proxy_tried = proxy_up;
	}

	/* 3 the voice sessions (q6voice.c voc_start_voice_call) */
	cvd_version();
	old_cvd = (voice_opts & OPT_OLD_CVD) != 0 || !cvd_at_least(2, 2);
	bzero(name, sizeof(name));
	if (voice_vsid == 0) {
		strlcpy(name, "default modem voice", sizeof(name));
	} else {
		snprintf(name, sizeof(name), "%08X", voice_vsid);
	}

	step = "MVM create";
	if (mvm_handle == 0) {
		char what[64];

		snprintf(what, sizeof(what), "MVM: passive control session \"%s\"", name);
		if (!vcmd(what, APR_SVC_MVM, 0, VSS_IMVM_CMD_CREATE_PASSIVE_CONTROL_SESSION, name, sizeof(name), 0, &answer)) {
			goto fail;
		}
		mvm_handle = answer.port;
		SAY("MVM handle %u\n", mvm_handle);
	}
	step = "CVS create";
	if (cvs_handle != 0 && cvs_vsid != voice_vsid) {
		/* 1066: the kept CVS carries the old VSID's name */
		(void)vcmd("CVS: destroyed (VSID changed)", APR_SVC_CVS, cvs_handle, APRV2_IBASIC_CMD_DESTROY_SESSION, NULL, 0, 0, NULL);
		cvs_handle = 0;
	}
	if (cvs_handle == 0) {
		char what[64];

		snprintf(what, sizeof(what), "CVS: passive control session \"%s\"", name);
		if (!vcmd(what, APR_SVC_CVS, 0, VSS_ISTREAM_CMD_CREATE_PASSIVE_CONTROL_SESSION, name, sizeof(name), 0, &answer)) {
			goto fail;
		}
		cvs_handle = answer.port;
		cvs_vsid = voice_vsid;
		SAY("CVS handle %u\n", cvs_handle);
	} else {
		SAY("CVS handle %u (kept from the last call)\n", cvs_handle);
	}
	step = "dual control";
	if (!(voice_opts & OPT_NO_DUAL) && !dual_done) {
		uint8_t enable = 1;     /* struct vss_imvm_cmd_set_policy_dual_control_t { bool enable_flag; } __packed */

		if (!vcmd("MVM: dual control on", APR_SVC_MVM, mvm_handle, VSS_IMVM_CMD_SET_POLICY_DUAL_CONTROL, &enable, sizeof(enable), 0,
		    NULL)) {
			goto fail;
		}
		dual_done = true;
	}

	/* voice_setup_vocproc() */
	step = "CVP create";
	{
		struct {
			uint16_t direction, tx_port;
			uint32_t tx_topology;
			uint16_t rx_port;
			uint32_t rx_topology, profile, mode;
			uint16_t ec_ref_port;
			char name[SESSION_NAME_LEN];
		} __attribute__((packed)) create;
		char what[96];

		bzero(&create, sizeof(create));
		create.direction = 2;
		create.tx_port = TX_PORT;
		create.rx_port = RX_PORT;
		if (proxy_up && (proxy_opts & POPT_CREATE)) {
			create.tx_port = PROXY_TX_PORT;         /* 1090 */
			create.rx_port = PROXY_RX_PORT;
		}
		create.tx_topology = (voice_opts & OPT_CAL_TOPOLOGIES) ? VSS_IVOCPROC_TOPOLOGY_ID_TX_SM_ECNS_V2 : VSS_IVOCPROC_TOPOLOGY_ID_NONE;
		create.rx_topology = (voice_opts & OPT_CAL_TOPOLOGIES) ? VSS_IVOCPROC_TOPOLOGY_ID_RX_DEFAULT : VSS_IVOCPROC_TOPOLOGY_ID_NONE;
		create.profile = VSS_ICOMMON_CAL_NETWORK_ID_NONE;
		create.mode = VSS_IVOCPROC_VOCPROC_MODE_EC_INT_MIXING;
		create.ec_ref_port = VSS_IVOCPROC_PORT_ID_NONE;
		snprintf(what, sizeof(what), "CVP: full control %s, tx %#x rx %#x, topologies %#x/%#x", old_cvd ? "V2" : "V3", create.tx_port,
		    create.rx_port, create.tx_topology, create.rx_topology);
		if (!vcmd(what, APR_SVC_CVP, 0, old_cvd ? VSS_IVOCPROC_CMD_CREATE_FULL_CONTROL_SESSION_V2 :
		    VSS_IVOCPROC_CMD_CREATE_FULL_CONTROL_SESSION_V3, &create, sizeof(create), 0, &answer)) {
			goto fail;
		}
		cvp_handle = answer.port;
		SAY("CVP handle %u\n", cvp_handle);
	}
	if (!old_cvd) {
		uint16_t channels[2] = { 1, 1 };        /* tx, rx: q6voice's VSS_NUM_DEV_CHANNELS_1 for rx with CVP < 2 */

		step = "device channels";
		(void)vcmd("CVP: device channels tx 1, rx 1", APR_SVC_CVP, cvp_handle, VSS_IVOCPROC_CMD_TOPOLOGY_SET_DEV_CHANNELS, channels,
		    sizeof(channels), 0, NULL);
	}
	if (!(voice_opts & OPT_NO_MEDIA_FMT) && !old_cvd && cvd_at_least(2, 3)) {
		step = "media formats";
		media_formats(proxy_up && (proxy_opts & POPT_CREATE), sweet_vproxy_rate());
	}
	if (!old_cvd) {
		step = "topology commit";
		(void)vcmd("CVP: topology commit", APR_SVC_CVP, cvp_handle, VSS_IVOCPROC_CMD_TOPOLOGY_COMMIT, NULL, 0, 0, NULL);
	}
	if (voice_opts & OPT_MFC) {
		struct {
			uint32_t rate;
			uint16_t bits, channels;
			uint16_t type[32];
		} __attribute__((packed)) mfc;

		step = "MFC";
		bzero(&mfc, sizeof(mfc));
		mfc.rate = RATE;
		mfc.bits = 16;
		mfc.channels = 2;
		mfc.type[0] = PCM_CHANNEL_FL;
		mfc.type[1] = PCM_CHANNEL_FR;
		(void)cvp_param("CVP: MFC output 2 ch, 48 kHz", AUDPROC_MODULE_ID_MFC, AUDPROC_PARAM_ID_MFC_OUTPUT_MEDIA_FORMAT, &mfc,
		    sizeof(mfc));
	}
	step = "CVP enable";
	if (!vcmd("CVP: enable", APR_SVC_CVP, cvp_handle, VSS_IVOCPROC_CMD_ENABLE, NULL, 0, 0, NULL)) {
		goto fail;
	}
	step = "attach vocproc";
	handle = cvp_handle;
	if (!vcmd("MVM: attach vocproc", APR_SVC_MVM, mvm_handle, VSS_IMVM_CMD_ATTACH_VOCPROC, &handle, sizeof(handle), 0, NULL)) {
		goto fail;
	}
	attached = true;
	{
		uint16_t mute[3] = { 0, 0, 0 };         /* TX, unmuted, no ramp (q6voice's stream_tx default) */

		(void)vcmd("CVS: TX unmuted", APR_SVC_CVS, cvs_handle, VSS_IVOLUME_CMD_MUTE_V2, mute, sizeof(mute), 0, NULL);
	}
	if (voice_opts & OPT_TTY) {
		uint32_t mode = 0;

		(void)vcmd("CVS: TTY mode 0", APR_SVC_CVS, cvs_handle, VSS_ISTREAM_CMD_SET_TTY_MODE, &mode, sizeof(mode), 0, NULL);
	}
	if (voice_opts & OPT_RX_UNMUTE) {
		uint16_t rx[3] = { 1, 0, 0 };           /* RX, unmuted, no ramp */

		(void)vcmd("CVS: RX unmuted", APR_SVC_CVS, cvs_handle, VSS_IVOLUME_CMD_MUTE_V2, rx, sizeof(rx), 0, NULL);
		(void)vcmd("CVP: RX unmuted", APR_SVC_CVP, cvp_handle, VSS_IVOLUME_CMD_MUTE_V2, rx, sizeof(rx), 0, NULL);
	}
	if (voice_opts & OPT_LISTEN) {
		uint32_t class_id = VSS_ICOMMON_EVENT_CLASS_VOICE_ACTIVITY_UPDATE;

		listening = vcmd("MVM: listen for voice activity", APR_SVC_MVM, mvm_handle, VSS_INOTIFY_CMD_LISTEN_FOR_EVENT_CLASS, &class_id,
		    sizeof(class_id), 0, NULL);
	}
	if (voice_opts & OPT_RX_DTMF) {
		uint32_t enable = 1;

		dtmf_on = vcmd("CVS: RX DTMF detection on", APR_SVC_CVS, cvs_handle, VSS_ISTREAM_CMD_SET_RX_DTMF_DETECTION, &enable,
		    sizeof(enable), 0, NULL);
	}
	ev_all = ev_ready = ev_not_ready = ev_activity = ev_dtmf = ev_other = 0;
	step = "start voice";
	if (!vcmd("MVM: START_VOICE", APR_SVC_MVM, mvm_handle, VSS_IMVM_CMD_START_VOICE, NULL, 0, 0, NULL)) {
		goto fail;
	}
	started = true;
	voice_state = target;
	SAY("START_VOICE ok: CALL AUDIO UP on the %s (MVM %u, CVS %u, CVP %u)\n", target == 2 ? "speaker" : "earpiece", mvm_handle,
	    cvs_handle, cvp_handle);
	if (proxy_up) {
		/* 1090: onto the proxy ports (unless created there), then the AP's data path */
		bool on_proxy = (proxy_opts & POPT_CREATE) != 0;

		if (!on_proxy) {
			on_proxy = cvp_switch(true, sweet_vproxy_rate());
			if (!on_proxy) {
				printf("SWEET VOICE PROXY: FALLBACK: the CVP switch failed: back to 0x9001/0x9040\n");
				if (!cvp_switch(false, RATE)) {
					printf("SWEET VOICE PROXY: FALLBACK: the switch back failed too: the session is made again directly\n");
					sweet_vproxy_stop("switch back failed");
					proxy_up = false;
					step = "proxy switch back";
					goto fail;
				}
			}
		}
		if (on_proxy && !sweet_vproxy_run()) {
			on_proxy = false;
			if (proxy_opts & POPT_CREATE) {
				step = "proxy data thread";
				goto fail;
			}
			printf("SWEET VOICE PROXY: FALLBACK: no data path: back to 0x9001/0x9040\n");
			if (!cvp_switch(false, RATE)) {
				sweet_vproxy_stop("switch back failed");
				proxy_up = false;
				step = "proxy switch back";
				goto fail;
			}
		}
		if (!on_proxy) {
			sweet_vproxy_stop("fallback to the direct path");
			proxy_up = false;
		} else {
			printf("SWEET VOICE PROXY: CALL AUDIO THROUGH THE AP (mode %d, %u Hz): CVP on 0x2001/0x2000\n", voice_proxy,
			    sweet_vproxy_rate());
		}
	}
	if (proxy_opts & POPT_HOSTPCM) {
		hostpcm_probe((proxy_opts & POPT_8K) ? 8000 : 16000);
	}
	sweet_qmux_vocoder_note(proxy_up ? sweet_vproxy_rate() : 0, 1);       /* 1093 */
	return true;
fail:
	failures++;
	SAY("START FAILED at %s (failure %u): undoing\n", step, failures);
	{
		bool retry = proxy_tried;

		voice_down("start failed");
		if (retry && !proxy_refused) {
			/* 1090: the proxy's part failed: the same start once more on today's ports */
			printf("SWEET VOICE PROXY: FALLBACK: the start failed at %s with the proxy: made again on 0x9001/0x9040\n", step);
			proxy_refused = true;
			retry = voice_up(target);
			proxy_refused = false;
			return retry;
		}
	}
	return false;
}

static void
voice_thread(__unused void *parameter, __unused wait_result_t result)
{
	for (;;) {
		int target = voice_target;

		if (target == voice_state) {
			if (voice_state != 0 && below_for(voice_state) != below_applied) {
				below_applied = below_for(voice_state);
				(void)redwood_sound_voice(amps_for(voice_state), below_applied);
				SAY("amplifier level now %u/8 dB under Android's\n", below_applied);
			}
			assert_wait_timeout((event_t)&voice_wake, THREAD_UNINT, 500, NSEC_PER_MSEC);
			thread_block(THREAD_CONTINUE_NULL);
			continue;
		}
		if (target == 0) {
			voice_down("kern.sweet_voice 0");
		} else if (voice_state != 0) {
			SAY("route: %s -> %s\n", voice_state == 2 ? "speaker" : "earpiece", target == 2 ? "speaker" : "earpiece");
			below_applied = below_for(target);
			(void)redwood_sound_voice(amps_for(target), below_applied);
			voice_state = target;
		} else if (!voice_up(target)) {
			if (voice_target == target) {
				voice_target = 0;
			}
		}
	}
}

static int
sysctl_voice(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int v = voice_target, changed = 0;
	int e = sysctl_io_number(req, v, sizeof(v), &v, &changed);

	if (e != 0 || !changed) {
		return e;
	}
	if (v < 0 || v > 2) {
		return EINVAL;
	}
	lck_mtx_lock(&thread_lock);
	if (!thread_up && v != 0) {
		thread_t thread;

		if (kernel_thread_start(voice_thread, NULL, &thread) != KERN_SUCCESS) {
			lck_mtx_unlock(&thread_lock);
			SAY("no thread\n");
			return ENOMEM;
		}
		thread_deallocate(thread);
		thread_up = true;
		redwood_adsp_apr_events_voice(voice_event);    /* 1066 */
	}
	lck_mtx_unlock(&thread_lock);
	SAY("kern.sweet_voice = %d (%s; was %d)\n", v, v == 0 ? "off" : v == 1 ? "earpiece" : "speaker", voice_target);
	auto_owned = false;     /* 1074: set by hand: kern.sweet_voice_auto leaves this session alone */
	voice_target = v;
	thread_wakeup(&voice_wake);
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_voice, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, sysctl_voice, "I",
    "redfin 1065: in-call audio through the ADSP voice services: 0 off (default), 1 earpiece, 2 speaker");

/*
 * 1074: the automatic trigger (kern.sweet_voice_auto in sweet_qmux.c, from the modem's VOICE call states; called on a
 * thread call). target 1/2: start (or re-route) only if nothing runs or the running session is the trigger's own; a
 * session set by hand (kern.sweet_voice) is left alone. target 0: stop only a session the trigger started.
 * The same path as the sysctl: voice_target + the voice thread.
 */
int sweet_voice_auto(int target, const char *why);
int
sweet_voice_auto(int target, const char *why)
{
	bool wake = false;

	if (target < 0 || target > 2) {
		return EINVAL;
	}
	lck_mtx_lock(&thread_lock);
	if (target != 0) {
		if (voice_target != 0 && !auto_owned) {
			lck_mtx_unlock(&thread_lock);
			SAY("auto: %s, but kern.sweet_voice %d was set by hand: left alone\n", why, voice_target);
			return 0;
		}
		if (!thread_up) {
			thread_t thread;

			if (kernel_thread_start(voice_thread, NULL, &thread) != KERN_SUCCESS) {
				lck_mtx_unlock(&thread_lock);
				SAY("auto: no thread\n");
				return ENOMEM;
			}
			thread_deallocate(thread);
			thread_up = true;
			redwood_adsp_apr_events_voice(voice_event);
		}
		auto_owned = true;
		if (voice_route != 0 && voice_target == 0) {   /* 1086: the next call in kern.sweet_voice_route's mode */
			uint64_t ms = 0;

			if (route_ios_at != 0) {
				absolutetime_to_nanoseconds(mach_absolute_time() - route_ios_at, &ms);
				ms /= NSEC_PER_MSEC;
			}
			if (route_ios_at == 0 || ms < ROUTE_IOS_FRESH_MS) {
				SAY("auto: kern.sweet_voice_route %d (%s) instead of %d\n", voice_route, route_ios_at ? "from iOS" : "set by hand",
				    target);
				target = voice_route;
			}
		} else if (voice_route != 0 && voice_target != 0) {
			target = voice_target;  /* running: the route keeps what it chose */
		}
		if (voice_target != target) {
			SAY("auto: %s -> %s (was %d)\n", why, target == 1 ? "earpiece" : "speaker", voice_target);
			voice_target = target;
			wake = true;
		}
	} else if (auto_owned) {
		auto_owned = false;
		if (voice_target != 0) {
			SAY("auto: %s -> off\n", why);
			voice_target = 0;
			wake = true;
		}
	}
	lck_mtx_unlock(&thread_lock);
	if (wake) {
		thread_wakeup(&voice_wake);
	}
	return 0;
}

static int
sysctl_voice_number(__unused struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	uint32_t *p = arg1;
	int v = (int)*p, changed = 0;
	int e = sysctl_io_number(req, v, sizeof(v), &v, &changed);

	if (e == 0 && changed) {
		*p = (uint32_t)v;
		SAY("%s = %#x\n", arg2 == 0 ? "opts" : arg2 == 1 ? "vsid" : arg2 == 2 ? "below_ear" : "below_spk", *p);
		thread_wakeup(&voice_wake);
	}
	return e;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_voice_opts, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, &voice_opts, 0, sysctl_voice_number, "I",
    "redfin 1065: variants (bits): 1 no dual control, 2 as CVD<2.2, 4 no media formats, 8 MFC 2 ch, 16 cal topologies, "
    "32 destroy CVS, 64 RX format 1 ch/16 bit; 1066: 128 MVM activity events, 256 CVS RX DTMF detection, 512 RX unmute, "
    "1024 TTY mode 0");
SYSCTL_PROC(_kern, OID_AUTO, sweet_voice_vsid, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, &voice_vsid, 1, sysctl_voice_number, "I",
    "redfin 1065: the VSID the passive sessions are named after (0x11C05000 VoiceMMode1; 0 = \"default modem voice\")");
SYSCTL_PROC(_kern, OID_AUTO, sweet_voice_below_ear, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, &below_ear, 2, sysctl_voice_number,
    "I", "redfin 1065: earpiece amplifier level, 1/8 dB under Android's (default 240; never under 48)");
SYSCTL_PROC(_kern, OID_AUTO, sweet_voice_below_spk, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, &below_speaker, 3,
    sysctl_voice_number, "I", "redfin 1065: speaker amplifier level, 1/8 dB under Android's (default 160; never under 48)");

/* ---- 1090: the voice proxy's switches ---- */
static int
sysctl_voice_proxy(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int v = voice_proxy, changed = 0;
	int e = sysctl_io_number(req, v, sizeof(v), &v, &changed);

	if (e != 0 || !changed) {
		return e;
	}
	if (v < 0 || v > 2) {
		return EINVAL;
	}
	voice_proxy = v;
	printf("SWEET VOICE PROXY: kern.sweet_voice_proxy = %d (%s; applies at the next call start%s)\n", v, v == 0 ? "direct path" :
	    v == 1 ? "proxy loopback" : "proxy PCM via /dev/redwood-voice", voice_state != 0 ? ", the running call keeps its path" : "");
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_voice_proxy, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, sysctl_voice_proxy, "I",
    "redfin 1090/1091: 0 direct path (default), 1 the call's CVP on the AFE RT proxy ports, PCM looped through the AP, 2 the same "
    "with the PCM through /dev/redwood-voice (userland); next call");

static int
sysctl_voice_proxy_opts(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int v = (int)proxy_opts, changed = 0;
	int e = sysctl_io_number(req, v, sizeof(v), &v, &changed);

	if (e == 0 && changed) {
		proxy_opts = (uint32_t)v & 7u;
		printf("SWEET VOICE PROXY: kern.sweet_voice_proxy_opts = %#x\n", proxy_opts);
	}
	return e;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_voice_proxy_opts, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, sysctl_voice_proxy_opts,
    "I", "redfin 1090: bits 1 CVP created on the proxy ports (no switch), 2 8 kHz (default 16 kHz), 4 host-PCM probe (status only)");

/* ---- 1086: route and gains ---- */
static void
route_set(int route, bool from_ios)
{
	lck_mtx_lock(&thread_lock);
	voice_route = route;
	route_ios_at = from_ios ? mach_absolute_time() : 0;
	if (route != 0 && voice_target != 0 && voice_target != route) {
		SAY("route: %s -> %s (kern.sweet_voice_route%s)\n", voice_target == 2 ? "speaker" : "earpiece",
		    route == 2 ? "speaker" : "earpiece", from_ios ? ", from iOS" : "");
		voice_target = route;
	}
	lck_mtx_unlock(&thread_lock);
	thread_wakeup(&voice_wake);
}

/* sweet_audio.c's REDWOOD_AUDIO_SET_CALL_ROUTE (RedwoodAudio): 1 earpiece (iOS: Receiver), 2 speaker */
int sweet_voice_route_from_ios(int route);
int
sweet_voice_route_from_ios(int route)
{
	if (route < 0 || route > 2) {
		return EINVAL;
	}
	SAY("iOS route: %s%s\n", route == 2 ? "Speaker" : route == 1 ? "Receiver" : "unset",
	    voice_state ? "" : " (no call running: kept for an auto start within 15 s)");
	route_set(route, true);
	return 0;
}

static int
sysctl_voice_route(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int v = voice_route, changed = 0;
	int e = sysctl_io_number(req, v, sizeof(v), &v, &changed);

	if (e != 0 || !changed) {
		return e;
	}
	if (v < 0 || v > 2) {
		return EINVAL;
	}
	SAY("kern.sweet_voice_route = %d (%s)\n", v, v == 0 ? "unset" : v == 1 ? "earpiece" : "speaker");
	route_set(v, false);
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_voice_route, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, NULL, 0,
    sysctl_voice_route, "I", "redfin 1086: 0 unset (default), 1 earpiece, 2 speaker: switches a running call; the next auto call's mode");

static int
sysctl_voice_gain(__unused struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	int *p = arg1;
	int v = *p, changed = 0;
	int e = sysctl_io_number(req, v, sizeof(v), &v, &changed);

	if (e != 0 || !changed) {
		return e;
	}
	if (arg2 == 0 ? (v < -30 || v > 24) : (v < -12 || v > 24)) {
		return EINVAL;
	}
	*p = v;
	if (arg2 == 0) {
		SAY("kern.sweet_voice_gain_rx = %d dB: earpiece %u/8, speaker %u/8 dB under Android's (cap 48/8)\n", v, below_for(1),
		    below_for(2));
		thread_wakeup(&voice_wake);     /* the voice thread re-applies the level of a running call */
	} else {
		SAY("kern.sweet_voice_gain_tx = %d dB (mic ADC gain during calls)\n", v);
		redwood_mic_voice_gain(v);
	}
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_voice_gain_rx, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, &gain_rx_db, 0,
    sysctl_voice_gain, "I", "redfin 1086: call amplifier level, dB relative to below_ear/below_spk (default 0, -30..24; cap 6 dB under Android's)");
SYSCTL_PROC(_kern, OID_AUTO, sweet_voice_gain_tx, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, &gain_tx_db, 1,
    sysctl_voice_gain, "I", "redfin 1086: call mic ADC gain, dB relative to boot-arg micgain (default 0, -12..24)");
#endif /* ARM64_BOARD_CONFIG_SWEET */
