/*
 * redfin (Pixel 5, SM7250 lito): the speakers' path through the audio DSP. Patch 0952 (WS-A).
 *
 * After HackiOS's bsd/dev/redwood_sound.c (github.com/oosexgang/hackios-xnu-kernel, APSL 2.0; the
 * POCO X5 Pro, SM7325 "yupik"): the APR messages to the DSP's AFE, ADM and ASM services and their
 * layouts carry over unchanged; the numbers are lito's, from the phone's live device tree and the
 * redbull kernel (LineageOS android_kernel_google_redbull: techpack/audio/asoc/kona.c,
 * techpack/audio/asoc/msm-dai-q6-v2.c, techpack/audio/dsp/q6afe.c, techpack/audio/soc/pinctrl-lpi.c,
 * lito-redbull-audio.dtsi, lito-redfin-audio.dtsi) and Android's mixer_paths_noextcodec_snd.xml:
 *
 *   speaker port  QUIN_TDM_RX_0, AFE port 0x9040 in group 0x9140; the two CS35L41 amplifiers
 *                 (sweet_cs35l41.c) listen on it, left in slot 0 and right in slot 1
 *   frame         4 slots of 32 bits at 48 kHz: kona.c's kona_tdm_snd_hw_params() forces
 *                 QUIN_TDM_MAX_SLOTS = 4 when the codec is a CS35L41, whatever the DT's 8 (no
 *                 qcom,tdm-max-slots: default 8) says; the mixer's "QUIN_TDM_RX_0 Channels Two" /
 *                 "Format S32_LE" are the port's 2 active channels; slot offsets 0 and 4 (kona.c
 *                 quin_tdm_dev_config RX_0); group slot mask 0xf, port slot mask 0x3
 *   bit clock     QUIN_TDM_IBIT (0x208), internal, rate x 32 x 4 = 6.144 MHz (kona.c sets it with
 *                 snd_soc_dai_set_sysclk(), which overrides the DT's 12.288 MHz), attribute
 *                 INVERT_COUPLE_NO (msm-dai-q6-v2.c's default; the DT names none)
 *   port          sync mode 0 (short), sync source 1 (internal), data out 0, invert 0, delay 1 (DSP_A); no
 *                 group message: msm-dai-q6-v2.c enables a group only with more than one port (or PRI_TDM_TX)
 *   pins          LPI GPIO 6-9 (lpi_pinctrl @0x33c0000, 0x1000 stride), func2, 16 mA (redfin's
 *                 override), no pull, output-high as Android's pinctrl writes them; LPASS registers,
 *                 so only after AFE_CMD_REMOTE_LPASS_CORE_HW_VOTE_REQUEST for blocks 3 (LPASS_HW_MACRO)
 *                 and 4 (LPASS_HW_DCODEC): without the votes an APPS access to LPASS is an external
 *                 abort (HackiOS redwood_mic.c; pinctrl-lpi.c refuses any access without the vote)
 *   memory        the audio ION stream 0x1401 of the apps SMMU (qcom,msm-audio-ion iommus; its pool
 *                 0x10000000 and qcom,smmu-sid-mask 0xf): a context bank with translation off, as
 *                 phone/wifi/radio.c does for the WLAN, so the DSP's addresses are physical (the
 *                 buffer is GPU-pool RAM below 4 GB); every address the DSP is given carries the
 *                 stream's low four bits (1) at bit 32, which the DSP takes as the SID and strips
 *   path          ADM device open v5 (playback, port 0x9040, no processing 0x10312, 2 x 24 bits,
 *                 48 kHz), ASM session 1 opened for writing PCM16 (no processing 0x10c68), routed to
 *                 the COPP, media format 2 x 16 bits 48 kHz, run; WRITE_V2 of 8 periods of 960 frames
 *                 (20 ms), each written again when its WRITE_DONE comes back
 *
 * Boot-arg sound=N (needs adsp=1; nothing happens without it, which is today's behaviour):
 *   sound=1  set up only: the votes, the LPI pins, the clock, the group and the port, started; held
 *            sndsecs (default 3) seconds, then everything taken back
 *   sound=2  ... and the stream: a 1 kHz tone at -30 dBFS for one second into the DSP, the amplifiers
 *            OFF; the path then stays up feeding silence (poke `a tone MS` plays the tone again)
 *   sound=3  ... and the amplifiers (0953, bsd/dev/sweet_cs35l41.c): probed and configured, powered
 *            down; HackiOS's speaker guard (bsd/dev/sweet_guard.c) armed at their level first; the tone
 *            AUDIBLE for a second at sndbelow (default 20 dB under Android's level; never louder than 6 dB
 *            under); the amplifiers run only while something plays and stop after 2 s of silence
 *   sound=4  what iOS plays (0954): /dev/redwood-audio (bsd/dev/sweet_audio.c) into a ring, the ring into
 *            the periods (silence when iOS writes nothing; the stream never stops), through the guard
 *            (with sndgain eighths of a dB of gain first, default and most 96: iOS keeps its peaks that
 *            far under full scale), the amplifiers on when a write is not silence and off after 2 s of
 *            silence; iOS's volume ioctl adds its `below' to sndbelow, and muted stops them; no boot tone
 * Boot-args: sndsecs=S (sound=1 hold), sndlpi=0 (leave the LPI pins alone: no clock at the amps),
 * sndbelow=E (eighths of a dB under Android's level, sound>=3; default 160), sndgain=E (sound=4),
 * sndgpio=1 (sweet_cs35l41.c: let the kernel drive the amplifiers' PMIC reset lines).
 * poke (bsd/dev/sweet_poke.c, letter a): `a status`, `a tone [MS]` (default 1000, at most 5000),
 * `a gain N` (the amplifiers N dB under Android's level, `-N` accepted too; hard cap 6 dB under), `a off` (the
 * amplifiers off and kept off), `a on` (allowed again), `a mic SECS` (0955, bsd/dev/sweet_mic.c: record SECS
 * seconds from the microphone, mic=1, and print the `files read /dev/pmem PA LEN` that fetches it).
 *
 * Log lines: "SWEET SOUND: ...". What is started is stopped before the DSP is shut down
 * (redwood_sound_dsp_down() from sweet_adsp.c's redwood_adsp_stop()).
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <libkern/libkern.h>
#include <libkern/OSAtomic.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <vm/memory_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/redwood_audio.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern kern_return_t thread_terminate(thread_t thread);
extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern void thread_deallocate(thread_t);
extern int sweet_gpu_pool_alloc(uint64_t bytes, uint64_t *pa);
int sweet_sound_cmd(const char *s, const char *end);

#define SAY(...)                printf("SWEET SOUND: " __VA_ARGS__)

static LCK_GRP_DECLARE(sound_lock_group, "sweet-sound");
static LCK_MTX_DECLARE(sound_lock, &sound_lock_group);  /* the path being set up or taken down */

static uint32_t level, hold_secs = 3, use_lpi = 1;
static uint32_t below = 160;            /* the amplifiers' level, eighths of a dB under Android's (sndbelow) */
static uint32_t below_min = 48;         /* the loudest ever allowed: 6 dB under Android's level (hard cap) */
static uint32_t guard_gain = REDWOOD_GUARD_MAX_GAIN;    /* sndgain, sound=4 */

/* bsd/dev/sweet_cs35l41.c */
extern bool sweet_amp_prepare(void);
extern bool sweet_amp_start(void);
extern bool sweet_amp_volume(uint32_t below);
extern bool sweet_amp_healthy(void);
extern void sweet_amp_stop(void);
extern bool sweet_amp_ready(void);
extern void sweet_amp_status(void);
extern void sweet_amp_reprobe(void);
extern void sweet_amp_select(uint32_t mask);
/* bsd/dev/sweet_mic.c */
extern int sweet_mic_cmd(uint32_t seconds);
extern void sweet_mic_status(void);

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static uint64_t
now_us(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}

static uint32_t __attribute__((noinline))
rd(vm_offset_t address)
{
	uint32_t value;

	__asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
	return value;
}

static void __attribute__((noinline))
wr(vm_offset_t address, uint32_t value)
{
	__asm__ volatile ("dsb sy\n\tstr %w0, [%1]\n\tdsb sy" : : "r" (value), "r" (address) : "memory");
}

/* ---- APR ------------------------------------------------------------------------------------------- */

#define APR_SVC_AFE             4
#define APR_SVC_ASM             7
#define APR_SVC_ADM             8
#define APR_BASIC_RSP_RESULT    0x000110e8u
#define APR_WAIT_MS             1000

static const char *
dsp_error(uint32_t status)
{
	static const char *const names[] = { "ok", "failed", "bad parameter", "unsupported", "version", "unexpected", "panic",
		                             "no resource", "bad handle", "already", "not ready", "pending", "busy", "aborted",
		                             "preempted", "continue", "immediate", "not implemented", "need more", "no memory",
		                             "not there" };

	return status < sizeof(names) / sizeof(names[0]) ? names[status] : "unknown error";
}

/* One command and its answer: true for the basic answer with status 0, or for the opcode `wanted' (then in *answer). */
static bool
cmd(const char *what, uint8_t service, uint16_t port, uint32_t opcode, const void *payload, uint32_t length, uint32_t wanted,
    struct redwood_apr_answer *answer)
{
	struct redwood_apr_answer local;

	if (answer == NULL) {
		answer = &local;
	}
	if (!redwood_adsp_apr(service, port, 0, opcode, payload, length, APR_WAIT_MS, answer)) {
		SAY("%s (opcode %#x, service %u, port %#x): NO ANSWER\n", what, opcode, service, port);
		return false;
	}
	if (answer->opcode == APR_BASIC_RSP_RESULT && answer->words[0] == opcode) {
		if (answer->words[1] != 0) {
			SAY("%s: REFUSED: %s (%u)\n", what, dsp_error(answer->words[1]), answer->words[1]);
		} else {
			SAY("%s: done\n", what);
		}
		return answer->words[1] == 0 && wanted == 0;
	}
	if (wanted != 0 && answer->opcode == wanted) {
		return true;
	}
	SAY("%s: unexpected answer: opcode %#x, %u bytes: %#x %#x\n", what, answer->opcode, answer->length, answer->words[0],
	    answer->words[1]);
	return false;
}

/* ---- the LPASS core votes (what makes APPS access to LPASS/LPI registers legal) ---------------------- */

#define AFE_CMD_REMOTE_LPASS_CORE_HW_VOTE_REQUEST       0x000100f4u
#define AFE_CMD_RSP_REMOTE_LPASS_CORE_HW_VOTE_REQUEST   0x000100f5u
#define AFE_CMD_REMOTE_LPASS_CORE_HW_DEVOTE_REQUEST     0x000100f6u
#define LPASS_HW_MACRO_BLOCK    3
#define LPASS_HW_DCODEC_BLOCK   4

static uint32_t vote_handle[5];
static bool voted[5];

static bool
vote(uint32_t block, const char *client)
{
	struct {
		uint32_t block;
		char client[8];
	} __attribute__((packed)) request = { block, { 0 } };
	struct redwood_apr_answer answer;
	char what[64];

	strlcpy(request.client, client, sizeof(request.client));
	snprintf(what, sizeof(what), "LPASS core vote, block %u (%s)", block, client);
	if (!cmd(what, APR_SVC_AFE, 0, AFE_CMD_REMOTE_LPASS_CORE_HW_VOTE_REQUEST, &request, sizeof(request),
	    AFE_CMD_RSP_REMOTE_LPASS_CORE_HW_VOTE_REQUEST, &answer)) {
		return false;
	}
	vote_handle[block] = answer.words[0];
	voted[block] = true;
	SAY("%s: granted, handle %u\n", what, vote_handle[block]);
	return true;
}

static void
devote(uint32_t block)
{
	uint32_t request[2] = { block, vote_handle[block] };

	if (!voted[block]) {
		return;
	}
	(void)cmd("LPASS core vote, given back", APR_SVC_AFE, 0, AFE_CMD_REMOTE_LPASS_CORE_HW_DEVOTE_REQUEST, request,
	    sizeof(request), 0, NULL);
	voted[block] = false;
}

/* ---- the speaker TDM pins: LPI GPIO 6-9 ---------------------------------------------------------------- */

#define LPI_PA                  0x033c0000u
#define LPI_FIRST               6               /* map gpio6..gpio9 only: 0x033c6000-0x033c9fff */
#define LPI_COUNT               4
#define LPI_VAL(n)              (0x1000u * ((n) - LPI_FIRST))           /* [1:0] pull [5:2] func [8:6] drive [9] out value */
#define LPI_DIR(n)              (0x1000u * ((n) - LPI_FIRST) + 4)       /* bit 1: output enabled */
#define LPI_ACTIVE              (0u | 2u << 2 | 7u << 6 | 1u << 9)      /* no pull, func2, 16 mA, high */

static vm_offset_t lpi;
static uint32_t lpi_found_val[LPI_COUNT], lpi_found_dir[LPI_COUNT];
static bool pins_taken;

static bool
pins_on(void)
{
	if (!use_lpi) {
		SAY("sndlpi=0: the LPI pins (gpio6-9) are left as they are: no clock reaches the amplifiers\n");
		return true;
	}
	if (!voted[LPASS_HW_MACRO_BLOCK] || !voted[LPASS_HW_DCODEC_BLOCK]) {
		SAY("the LPASS votes were not both granted: the LPI pins are NOT touched (an access would abort)\n");
		return false;
	}
	if (lpi == 0) {
		lpi = ml_io_map(LPI_PA + 0x1000u * LPI_FIRST, 0x1000u * LPI_COUNT);
	}
	SAY("about to read the LPI pins gpio6-9 at %#x (after the votes)\n", LPI_PA + 0x1000u * LPI_FIRST);
	for (unsigned i = 0; i < LPI_COUNT; i++) {
		lpi_found_val[i] = rd(lpi + LPI_VAL(LPI_FIRST + i));
		lpi_found_dir[i] = rd(lpi + LPI_DIR(LPI_FIRST + i));
	}
	SAY("LPI gpio6-9 found: val %#x %#x %#x %#x dir %#x %#x %#x %#x\n", lpi_found_val[0], lpi_found_val[1], lpi_found_val[2],
	    lpi_found_val[3], lpi_found_dir[0], lpi_found_dir[1], lpi_found_dir[2], lpi_found_dir[3]);
	pins_taken = true;
	for (unsigned i = 0; i < LPI_COUNT; i++) {
		unsigned n = LPI_FIRST + i;

		wr(lpi + LPI_VAL(n), (lpi_found_val[i] & ~0x3ffu) | LPI_ACTIVE);
		wr(lpi + LPI_DIR(n), 1u << 1);
	}
	SAY("LPI gpio6-9 now: val %#x %#x %#x %#x dir %#x %#x %#x %#x (want val[9:0] %#x dir 0x2; bits above 9 are kept as found)\n", rd(lpi + LPI_VAL(6)),
	    rd(lpi + LPI_VAL(7)), rd(lpi + LPI_VAL(8)), rd(lpi + LPI_VAL(9)), rd(lpi + LPI_DIR(6)), rd(lpi + LPI_DIR(7)),
	    rd(lpi + LPI_DIR(8)), rd(lpi + LPI_DIR(9)), LPI_ACTIVE);
	return true;
}

static void
pins_off(void)
{
	if (!pins_taken) {
		return;
	}
	for (unsigned i = 0; i < LPI_COUNT; i++) {
		wr(lpi + LPI_VAL(LPI_FIRST + i), lpi_found_val[i]);
		wr(lpi + LPI_DIR(LPI_FIRST + i), lpi_found_dir[i] & 2u);
	}
	pins_taken = false;
	SAY("LPI gpio6-9 put back\n");
}

/* ---- AFE: the clock, the group and the port --------------------------------------------------------------- */

#define AFE_PORT_CMD_DEVICE_START               0x000100e5u
#define AFE_PORT_CMD_DEVICE_STOP                0x000100e6u
#define AFE_PORT_CMD_SET_PARAM_V2               0x000100efu
#define AFE_SVC_CMD_SET_PARAM                   0x000100f3u
#define AFE_MODULE_AUDIO_DEV_INTERFACE          0x0001020cu
#define AFE_PARAM_ID_TDM_CONFIG                 0x0001029du
#define AFE_MODULE_TDM                          0x0001028au
#define AFE_PARAM_ID_PORT_SLOT_MAPPING_CONFIG   0x00010297u
#define AFE_MODULE_CLOCK_SET                    0x0001028fu
#define AFE_PARAM_ID_CLOCK_SET                  0x00010290u

#define PORT                    0x9040          /* QUIN_TDM_RX_0 */
#define GROUP                   0x9140          /* QUIN_TDM_RX group (one port: not enabled, as Android) */
#define CLOCK_QUIN_TDM_IBIT     0x208
#define CLOCK_INVERT_COUPLE_NO  4
#define RATE                    48000
#define SLOTS                   4               /* kona.c QUIN_TDM_MAX_SLOTS with CS35L41 */
#define SLOT_BITS               32
#define PORT_BITS               32              /* the mixer's S32_LE */

struct afe_param {
	uint32_t module, param;
	uint16_t size, reserved;
} __attribute__((packed));

struct afe_clock_set {
	uint32_t version, id, hz;
	uint16_t attribute, root;
	uint32_t enable;
} __attribute__((packed));

struct afe_tdm {
	uint32_t version, channels, rate, bits;
	uint16_t data_format, sync_mode, sync_source, slots, data_out, invert_sync, data_delay, slot_bits;
	uint32_t slot_mask;
} __attribute__((packed));

struct afe_slot_mapping {
	uint32_t version;
	uint16_t channels, bits;
	uint32_t align;
	uint16_t offsets[8];
} __attribute__((packed));

/* A parameter of the AFE as a whole (clocks, groups), in band. */
static bool
afe_service_param(const char *what, uint32_t module, uint32_t param, const void *data, uint16_t size)
{
	uint8_t packet[16 + sizeof(struct afe_param) + 64];
	uint32_t *head = (uint32_t *)packet;
	struct afe_param *p = (struct afe_param *)(packet + 16);

	bzero(packet, sizeof(packet));
	head[0] = (uint32_t)sizeof(*p) + size;
	p->module = module;
	p->param = param;
	p->size = size;
	memcpy(packet + 16 + sizeof(*p), data, size);
	return cmd(what, APR_SVC_AFE, 0, AFE_SVC_CMD_SET_PARAM, packet, 16 + (uint32_t)sizeof(*p) + size, 0, NULL);
}

static bool
afe_port_param(const char *what, uint16_t port, uint32_t module, uint32_t param, const void *data, uint16_t size)
{
	uint8_t packet[16 + sizeof(struct afe_param) + 64];
	struct afe_param *p = (struct afe_param *)(packet + 16);

	bzero(packet, sizeof(packet));
	*(uint16_t *)packet = port;
	*(uint16_t *)(packet + 2) = (uint16_t)(sizeof(*p) + size);
	p->module = module;
	p->param = param;
	p->size = size;
	memcpy(packet + 16 + sizeof(*p), data, size);
	return cmd(what, APR_SVC_AFE, 0, AFE_PORT_CMD_SET_PARAM_V2, packet, 16 + (uint32_t)sizeof(*p) + size, 0, NULL);
}

static bool clock_on, port_on;

static bool
afe_clock(bool on)
{
	struct afe_clock_set clock = { 1, CLOCK_QUIN_TDM_IBIT, RATE * SLOT_BITS * SLOTS, CLOCK_INVERT_COUPLE_NO, 0, on };

	return afe_service_param(on ? "QUIN TDM bit clock 0x208 at 6.144 MHz, on" : "QUIN TDM bit clock, off", AFE_MODULE_CLOCK_SET,
	           AFE_PARAM_ID_CLOCK_SET, &clock, sizeof(clock));
}

static bool
afe_on(void)
{
	struct afe_tdm tdm = {
		.version = 1, .channels = 2, .rate = RATE, .bits = PORT_BITS, .data_format = 0, .sync_mode = 0, .sync_source = 1,
		.slots = SLOTS, .data_out = 0, .invert_sync = 0, .data_delay = 1, .slot_bits = SLOT_BITS, .slot_mask = 0x3,
	};
	struct afe_slot_mapping mapping = { .version = 1, .channels = 2, .bits = PORT_BITS, .align = 0,
		                            .offsets = { 0, 4, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff } };
	uint16_t start[2] = { PORT, 0 };

	/*
	 * msm_dai_q6_tdm_prepare()'s order: the clock, then the group -- which Android enables only for a group of
	 * more than one port (or PRI_TDM_TX); QUIN_TDM_RX's has one (qcom,msm-cpudai-tdm-group-num-ports 1), so no
	 * group message at all --, then the port's TDM config, its slot mapping, and its start.
	 */
	clock_on = afe_clock(true);
	if (!clock_on) {
		return false;
	}
	if (!afe_port_param("port 0x9040: 2 ch x 32 bits in 4 slots, sync 0/1, delay 1, mask 0x3", PORT,
	    AFE_MODULE_AUDIO_DEV_INTERFACE, AFE_PARAM_ID_TDM_CONFIG, &tdm, sizeof(tdm)) ||
	    !afe_port_param("port 0x9040 slot mapping: offsets 0 and 4", PORT, AFE_MODULE_TDM,
	    AFE_PARAM_ID_PORT_SLOT_MAPPING_CONFIG, &mapping, sizeof(mapping))) {
		return false;
	}
	port_on = cmd("port 0x9040, started", APR_SVC_AFE, 0, AFE_PORT_CMD_DEVICE_START, start, sizeof(start), 0, NULL);
	return port_on;
}

static void
afe_off(void)
{
	uint16_t stop[2] = { PORT, 0 };

	if (port_on) {
		(void)cmd("port 0x9040, stopped", APR_SVC_AFE, 0, AFE_PORT_CMD_DEVICE_STOP, stop, sizeof(stop), 0, NULL);
		port_on = false;
	}
	if (clock_on) {
		(void)afe_clock(false);
		clock_on = false;
	}
}

/* ---- the DSP's view of our memory: stream 0x1401 through a bank with translation off ------------------------ */

#define SMMU_PA                 0x15000000u
#define AUDIO_SID               0x1401u
#define AUDIO_SID_BITS          (1ull << 32)    /* 0x1401 & 0xf (qcom,smmu-sid-mask 0xf) at bit 32 */
#define CB_CBAR                 0x0001f000u     /* stage 1 context, stage 2 bypass (radio.c, as ABL's banks) */
#define CB_CBA2R                0x00000001u     /* AArch64 */
#define CB_SCTLR                0x000000e0u     /* CFCFG | CFIE | CFRE, M = 0: no translation */

#define PERIODS                 8
#define PERIOD_FRAMES           960             /* 20 ms */
#define PERIOD_BYTES            (PERIOD_FRAMES * 4)
#define PLAY_BYTES              (PERIODS * 4096)        /* a page a period */
#define MIC_BYTES               (16 * 4096)             /* the microphone's (sweet_mic.c), behind the periods */
#define SHARED_BYTES            (PLAY_BYTES + MIC_BYTES)

static struct {
	vm_offset_t g0, gr1, cb;                /* SMMU page 0 (SMR/S2CR), GR1 page (CBAR), our bank's page */
	uint32_t psize, npage, smr_n, cb_n, smr_old, s2cr_old;
	bool on, found_ready;
} sm = { .smr_n = ~0u, .cb_n = ~0u };
static vm_offset_t buffer;                      /* SHARED_BYTES, write-combining, of GPU-pool RAM below 4 GB */
static uint64_t buffer_pa;
static uint32_t space_users;                    /* bit 0 the speakers, bit 1 the microphone */
static LCK_MTX_DECLARE(space_lock, &sound_lock_group);

static bool
sid_match(uint32_t smr, uint32_t sid)
{
	uint32_t id = smr & 0x7fff, mask = (smr >> 16) & 0x7fff;

	return ((id ^ sid) & ~mask & 0x7fff) == 0;
}

static bool
smmu_on(void)
{
	uint32_t id0, id1, nsmr, ncb, free_smr = ~0u;
	uint64_t used[4] = { 0 };

	if (sm.on) {
		return true;
	}
	if (sm.g0 == 0) {
		sm.g0 = ml_io_map(SMMU_PA, 0x1000);
	}
	id0 = rd(sm.g0 + 0x20);
	id1 = rd(sm.g0 + 0x24);
	nsmr = id0 & 0xff;
	ncb = id1 & 0xff;
	sm.psize = (id1 >> 31) ? 0x10000 : 0x1000;
	sm.npage = 1u << (((id1 >> 28) & 7) + 1);
	if (sm.psize != 0x1000 || (rd(sm.g0) & (1u << 3))) {
		SAY("SMMU: page size %#x / 16-bit stream IDs: not handled\n", sm.psize);
		return false;
	}
	if (sm.gr1 == 0) {
		sm.gr1 = ml_io_map(SMMU_PA + sm.psize, 0x1000);
	}
	for (uint32_t i = 0; i < nsmr && i < 256; i++) {
		uint32_t smr = rd(sm.g0 + 0x800 + 4 * i), s2cr = rd(sm.g0 + 0xc00 + 4 * i);

		if (!(smr >> 31)) {
			if (free_smr == ~0u) {
				free_smr = i;
			}
			continue;
		}
		if (((s2cr >> 16) & 3) == 0) {
			used[(s2cr & 0xff) / 64] |= 1ull << ((s2cr & 0xff) % 64);
		}
		if (sid_match(smr, AUDIO_SID)) {
			uint32_t type = (s2cr >> 16) & 3, c = s2cr & 0xff;

			SAY("SMMU: SMR%u %#x already matches %#x: S2CR %#x\n", i, smr, AUDIO_SID, s2cr);
			if (type == 1) {
				SAY("SMMU: stream %#x bypasses the SMMU already: addresses are physical\n", AUDIO_SID);
				sm.found_ready = sm.on = true;
				return true;
			}
			if (type == 0) {
				vm_offset_t cbp = ml_io_map(SMMU_PA + (sm.npage + c) * sm.psize, 0x1000);
				uint32_t sctlr = rd(cbp);

				SAY("SMMU: ... through CB%u, SCTLR %#x\n", c, sctlr);
				if (!(sctlr & 1)) {
					sm.found_ready = sm.on = true;
					return true;
				}
			}
			SAY("SMMU: stream %#x is translated by somebody else's bank: not touched\n", AUDIO_SID);
			return false;
		}
	}
	for (uint32_t c = 0; c < ncb && c < 256 && sm.cb_n == ~0u; c++) {
		vm_offset_t cbp;

		if ((used[c / 64] & (1ull << (c % 64))) || rd(sm.gr1 + 4 * c) != 0) {
			continue;
		}
		cbp = ml_io_map(SMMU_PA + (sm.npage + c) * sm.psize, 0x1000);
		if (rd(cbp) & 1) {
			ml_io_unmap(cbp, 0x1000);
			continue;
		}
		sm.cb_n = c;
		sm.cb = cbp;
	}
	if (free_smr == ~0u || sm.cb_n == ~0u) {
		SAY("SMMU: no free SMR (%d) or bank (%d)\n", (int)free_smr, (int)sm.cb_n);
		return false;
	}
	sm.smr_n = free_smr;
	sm.smr_old = rd(sm.g0 + 0x800 + 4 * free_smr);
	sm.s2cr_old = rd(sm.g0 + 0xc00 + 4 * free_smr);
	SAY("SMMU: stream %#x -> CB%u via SMR%u, translation off (SMR was %#x, S2CR %#x)\n", AUDIO_SID, sm.cb_n, sm.smr_n,
	    sm.smr_old, sm.s2cr_old);
	wr(sm.cb + 0x58, 0xffffffffu);          /* FSR */
	wr(sm.cb + 0x20, 0);                    /* TTBR0 */
	wr(sm.cb + 0x24, 0);
	wr(sm.cb + 0x30, 0);                    /* TCR */
	wr(sm.cb + 0x10, 0);                    /* TCR2 */
	wr(sm.cb + 0x38, 0);                    /* MAIR */
	wr(sm.cb + 0x3c, 0);
	wr(sm.gr1 + 0x800 + 4 * sm.cb_n, CB_CBA2R);
	wr(sm.gr1 + 4 * sm.cb_n, CB_CBAR);
	wr(sm.cb, CB_SCTLR);
	wr(sm.g0 + 0xc00 + 4 * sm.smr_n, sm.cb_n);
	wr(sm.g0 + 0x800 + 4 * sm.smr_n, (1u << 31) | AUDIO_SID);
	wr(sm.g0 + 0x70, 0);                    /* sTLBGSYNC */
	for (unsigned i = 0; i < 1000 && (rd(sm.g0 + 0x74) & 1); i++) {
		sleep_ms(1);
	}
	sm.on = (rd(sm.g0 + 0x800 + 4 * sm.smr_n) >> 31) && (rd(sm.g0 + 0xc00 + 4 * sm.smr_n) & 0x300ff) == sm.cb_n &&
	    !(rd(sm.cb) & 1);
	SAY("SMMU: SMR %#x S2CR %#x CBAR %#x SCTLR %#x -> %s\n", rd(sm.g0 + 0x800 + 4 * sm.smr_n), rd(sm.g0 + 0xc00 + 4 * sm.smr_n),
	    rd(sm.gr1 + 4 * sm.cb_n), rd(sm.cb), sm.on ? "IDENTITY" : "NOT TAKEN");
	return sm.on;
}

static void
smmu_faults(const char *when)
{
	uint32_t fsr;

	if (!sm.on || sm.cb == 0) {
		return;
	}
	fsr = rd(sm.cb + 0x58);
	SAY("SMMU %s: CB%u FSR %#x FAR %#x%08x FSYNR0 %#x, global SGFSR %#x\n", when, sm.cb_n, fsr, rd(sm.cb + 0x64), rd(sm.cb + 0x60),
	    rd(sm.cb + 0x68), rd(sm.g0 + 0x48));
}

static void
smmu_off(void)
{
	if (!sm.on || sm.found_ready) {
		sm.on = false;
		return;
	}
	smmu_faults("at the end");
	wr(sm.g0 + 0x800 + 4 * sm.smr_n, sm.smr_old);
	wr(sm.g0 + 0xc00 + 4 * sm.smr_n, sm.s2cr_old);
	wr(sm.g0 + 0x70, 0);
	sm.on = false;
	SAY("SMMU: SMR%u put back (CB%u left configured, unused)\n", sm.smr_n, sm.cb_n);
}

/* The memory the DSP sees: whoever comes first sets it up, whoever leaves last takes it down. user 0 speakers, 1 mic. */
static bool
space_get(unsigned user)
{
	bool ok = true;

	lck_mtx_lock(&space_lock);
	if (buffer == 0) {
		if (sweet_gpu_pool_alloc(SHARED_BYTES, &buffer_pa) != 0 || buffer_pa + SHARED_BYTES > 0x100000000ull) {
			SAY("no GPU-pool memory below 4 GB for the DSP's buffers\n");
			ok = false;
		} else {
			buffer = ml_io_map_unmappable(buffer_pa, SHARED_BYTES, VM_WIMG_WCOMB);
			ok = buffer != 0;
			if (ok) {
				bzero((void *)buffer, SHARED_BYTES);
				SAY("DSP buffers: %u KB at PA %#llx (DSP address %#llx)\n", SHARED_BYTES / 1024, buffer_pa,
				    buffer_pa | AUDIO_SID_BITS);
			}
		}
	}
	if (ok && space_users == 0) {
		ok = smmu_on();
	}
	if (ok) {
		space_users |= 1u << user;
	}
	lck_mtx_unlock(&space_lock);
	return ok;
}

static void
space_put(unsigned user)
{
	lck_mtx_lock(&space_lock);
	if (space_users & (1u << user)) {
		space_users &= ~(1u << user);
		if (space_users == 0) {
			smmu_off();
		}
	}
	lck_mtx_unlock(&space_lock);
}

bool
redwood_audio_dsp_memory(uintptr_t *memory, uint64_t *dsp_address, uint32_t *bytes)
{
	if (!space_get(1)) {
		return false;
	}
	*memory = buffer + PLAY_BYTES;
	*dsp_address = (buffer_pa + PLAY_BYTES) | AUDIO_SID_BITS;
	*bytes = MIC_BYTES;
	return true;
}

void
redwood_audio_dsp_memory_put(void)
{
	space_put(1);
}

/* 1090: the voice proxy (sweet_vproxy.c) holds the DSP's view of memory (the SMMU stream) for its own buffer: user 2 */
bool
redwood_sound_dsp_space(bool on)
{
	if (!on) {
		space_put(2);
		return true;
	}
	return space_get(2);
}

/* ---- ADM and ASM: the path and the stream ------------------------------------------------------------------- */

#define ADM_CMD_DEVICE_OPEN_V5                  0x00010326u
#define ADM_CMDRSP_DEVICE_OPEN_V5               0x00010329u
#define ADM_CMD_DEVICE_CLOSE_V5                 0x00010327u
#define ADM_CMD_MATRIX_MAP_ROUTINGS_V5          0x00010325u
#define ADM_PATH_PLAYBACK                       1
#define ADM_MATRIX_AUDIO_RX                     0
#define ADM_TOPOLOGY_NONE                       0x00010312u
#define ADM_BITS                                24

#define ASM_CMD_SHARED_MEM_MAP_REGIONS          0x00010d92u
#define ASM_CMDRSP_SHARED_MEM_MAP_REGIONS       0x00010d93u
#define ASM_CMD_SHARED_MEM_UNMAP_REGIONS        0x00010d94u
#define ASM_STREAM_CMD_OPEN_WRITE_V3            0x00010db3u
#define ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2        0x00010d98u
#define ASM_SESSION_CMD_RUN_V2                  0x00010daau
#define ASM_DATA_CMD_WRITE_V2                   0x00010dabu
#define ASM_DATA_EVENT_WRITE_DONE_V2            0x00010d99u
#define ASM_SESSION_EVENT_RX_UNDERFLOW          0x00010c17u
#define ASM_STREAM_CMD_CLOSE                    0x00010bcdu
#define ASM_TOPOLOGY_NONE                       0x00010c68u
#define ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2      0x00010da5u
#define ASM_SHMEM_4K_POOL                       3
#define SESSION                                 1
#define STREAM_PORT                             (SESSION << 8 | 1)
#define WRITE_TOKEN                             0x80000000u     /* | the period */
#define PCM_CHANNEL_FL                          1
#define PCM_CHANNEL_FR                          2

static bool mapped, copp_open, stream_open, stream_runs;
static uint32_t map_handle, copp;
static volatile uint32_t returned;              /* periods the DSP is done with (a bit each) */
static volatile uint32_t written_done, underflows, write_errors;
static int feeder_wake;

/* The DSP's events (on the link's thread): WRITE_DONE and underflows. */
static bool
events(uint8_t service, __unused uint16_t port, uint32_t opcode, uint32_t token, const uint32_t *payload, uint32_t length)
{
	if (service != APR_SVC_ASM) {
		return false;
	}
	if ((opcode == ASM_DATA_EVENT_WRITE_DONE_V2 || opcode == APR_BASIC_RSP_RESULT) && (token & WRITE_TOKEN) &&
	    (token & 0xff) < PERIODS) {
		if (opcode == APR_BASIC_RSP_RESULT || (length >= 16 && payload[3] != 0)) {
			OSIncrementAtomic((volatile SInt32 *)&write_errors);
		}
		OSIncrementAtomic((volatile SInt32 *)&written_done);
		OSBitOrAtomic(1u << (token & 31), &returned);
		thread_wakeup(&feeder_wake);
		return true;
	}
	if (opcode == ASM_SESSION_EVENT_RX_UNDERFLOW) {
		OSIncrementAtomic((volatile SInt32 *)&underflows);
		return true;
	}
	return false;
}

static bool
stream_on(void)
{
	uint64_t address = buffer_pa | AUDIO_SID_BITS;
	struct {
		uint16_t pool, regions;
		uint32_t property;
		uint32_t low, high, size;
	} __attribute__((packed)) map = { ASM_SHMEM_4K_POOL, 1, 0, (uint32_t)address, (uint32_t)(address >> 32), PLAY_BYTES };
	struct {
		uint16_t flags, mode, endpoint1, endpoint2;
		uint32_t topology;
		uint16_t channels, bits;
		uint32_t rate;
		uint8_t map[8];
	} __attribute__((packed)) open = { 0, ADM_PATH_PLAYBACK, PORT, 0xffff, ADM_TOPOLOGY_NONE, 2, ADM_BITS, RATE,
		                           { PCM_CHANNEL_FL, PCM_CHANNEL_FR } };
	struct {
		uint32_t matrix, sessions;
		uint16_t session, copps;
		uint16_t copp[2];
	} __attribute__((packed)) route = { ADM_MATRIX_AUDIO_RX, 1, SESSION, 1, { 0, 0 } };
	struct {
		uint32_t flags;
		uint16_t sink, bits;
		uint32_t topology, format;
	} __attribute__((packed)) write = { 0, 0, 16, ASM_TOPOLOGY_NONE, ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2 };
	struct {
		uint32_t size;
		uint16_t channels, bits;
		uint32_t rate;
		uint16_t is_signed, reserved;
		uint8_t map[8];
	} __attribute__((packed)) format = { 20, 2, 16, RATE, 1, 0, { PCM_CHANNEL_FL, PCM_CHANNEL_FR } };
	uint32_t run[3] = { 0, 0, 0 };
	struct redwood_apr_answer answer;

	if (!cmd("ASM: our periods mapped for the DSP", APR_SVC_ASM, 0, ASM_CMD_SHARED_MEM_MAP_REGIONS, &map, sizeof(map),
	    ASM_CMDRSP_SHARED_MEM_MAP_REGIONS, &answer)) {
		return false;
	}
	mapped = true;
	map_handle = answer.words[0];
	SAY("ASM: %u KB at %#llx are the DSP's handle %#x\n", PLAY_BYTES / 1024, address, map_handle);
	if (!cmd("ADM: device open v5, playback to 0x9040, no processing, 2 x 24 bits, 48 kHz", APR_SVC_ADM, PORT,
	    ADM_CMD_DEVICE_OPEN_V5, &open, sizeof(open), ADM_CMDRSP_DEVICE_OPEN_V5, &answer)) {
		return false;
	}
	if (answer.words[0] != 0) {
		SAY("ADM: device open REFUSED: %s\n", dsp_error(answer.words[0]));
		return false;
	}
	copp_open = true;
	copp = answer.words[1] & 0xffff;
	route.copp[0] = (uint16_t)copp;
	SAY("ADM: COPP %u open\n", copp);
	if (!cmd("ASM: session 1 opened for writing PCM16, no processing", APR_SVC_ASM, STREAM_PORT, ASM_STREAM_CMD_OPEN_WRITE_V3,
	    &write, sizeof(write), 0, NULL)) {
		return false;
	}
	stream_open = true;
	if (!cmd("ADM: session 1 routed to the COPP", APR_SVC_ADM, 0, ADM_CMD_MATRIX_MAP_ROUTINGS_V5, &route, sizeof(route), 0,
	    NULL) ||
	    !cmd("ASM: media format 2 x 16 bits, 48 kHz", APR_SVC_ASM, STREAM_PORT, ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2, &format,
	    sizeof(format), 0, NULL) ||
	    !cmd("ASM: session 1, run", APR_SVC_ASM, STREAM_PORT, ASM_SESSION_CMD_RUN_V2, run, sizeof(run), 0, NULL)) {
		return false;
	}
	stream_runs = true;
	return true;
}

static void
stream_off(void)
{
	uint32_t handle = map_handle;

	if (stream_open) {
		(void)cmd("ASM: stream closed", APR_SVC_ASM, STREAM_PORT, ASM_STREAM_CMD_CLOSE, NULL, 0, 0, NULL);
		stream_open = stream_runs = false;
	}
	if (copp_open) {
		(void)cmd("ADM: COPP closed", APR_SVC_ADM, (uint16_t)copp, ADM_CMD_DEVICE_CLOSE_V5, NULL, 0, 0, NULL);
		copp_open = false;
	}
	if (mapped) {
		(void)cmd("ASM: periods unmapped", APR_SVC_ASM, 0, ASM_CMD_SHARED_MEM_UNMAP_REGIONS, &handle, sizeof(handle), 0, NULL);
		mapped = false;
	}
}

/* ---- sound=4: what iOS writes to /dev/redwood-audio, in a ring (sweet_audio.c's writer never waits) ---------------- */

#define RING_FRAMES             32768           /* 0.68 s; a power of two, the indices run on */
#define SILENCE                 8               /* samples no larger are silence */

static int16_t ring[RING_FRAMES * 2];
static volatile uint32_t ring_head, ring_tail;  /* frames: written by the sink, taken by the feeder */
static volatile uint64_t last_sound_us;         /* when the latest write that was not silence came */
static uint64_t ios_frames, fed_frames, ios_dropped;
static volatile uint32_t ios_peak;

/* From the writer's thread, under sweet_audio.c's lock: never waits. */
static uint32_t
ios_sink(const int16_t *frames, uint32_t count)
{
	uint32_t head = ring_head, room = RING_FRAMES - (head - ring_tail), taken = count < room ? count : room;
	bool sound = false;

	for (uint32_t i = 0; i < taken; i++) {
		int16_t l = frames[2 * i], r = frames[2 * i + 1];
		uint32_t al = l < 0 ? (uint32_t)-(int32_t)l : (uint32_t)l, ar = r < 0 ? (uint32_t)-(int32_t)r : (uint32_t)r;

		ring[2 * ((head + i) % RING_FRAMES)] = l;
		ring[2 * ((head + i) % RING_FRAMES) + 1] = r;
		sound |= al > SILENCE || ar > SILENCE;
		if (al > ios_peak || ar > ios_peak) {
			ios_peak = al > ar ? al : ar;
		}
	}
	OSMemoryBarrier();
	ring_head = head + taken;
	ios_frames += taken;
	ios_dropped += count - taken;
	if (sound) {
		last_sound_us = now_us();
	}
	return taken;
}

/* ---- the feeder: every period the DSP gives back is filled and written again ------------------------------------ */

/* 1 kHz at 48 kHz, -30 dBFS (peak 1036): one cycle */
static const int16_t tone[48] = {
	0, 135, 268, 396, 518, 631, 733, 822, 897, 957, 1001, 1027, 1036, 1027, 1001, 957, 897, 822, 733, 631, 518, 396, 268, 135,
	0, -135, -268, -396, -518, -631, -733, -822, -897, -957, -1001, -1027, -1036, -1027, -1001, -957, -897, -822, -733, -631,
	-518, -396, -268, -135,
};
static uint32_t tone_at;
static volatile uint64_t tone_until_us;          /* the tone plays until then */
static volatile bool stopping, feeder_runs;
static uint64_t sent_periods, failed_sends, tone_periods, silent_periods;
static uint32_t guard_periods;

/*
 * 1090: the call mode (redwood_sound_call_mode(), from sweet_vproxy.c while a call runs on the AFE proxy ports with
 * kern.sweet_voice_proxy 1): periods of 10 ms, at most CALL_QUEUED at the DSP (30 ms instead of 8 x 20 ms = 160 ms),
 * filled from the call ring (the downlink, 48 kHz stereo) with what iOS plays mixed in. Off: 8 x 20 ms again (the
 * periods held back are queued again). Nothing of it runs while call_mode is false.
 */
#define CALL_PERIOD_FRAMES      480             /* 10 ms */
#define CALL_QUEUED             3
#define CALL_RING_FRAMES        8192            /* 170 ms; a power of two, the indices run on */
static volatile bool call_mode;
static bool call_primed;
static int16_t call_ring[CALL_RING_FRAMES * 2];
static volatile uint32_t call_head, call_tail;  /* frames: written by the proxy, taken by the feeder */
static volatile uint32_t call_underruns, call_overruns, call_periods, queued_frames;
static uint32_t period_frames_of[PERIODS], guard_half;

static int16_t
mix16(int16_t a, int16_t b)
{
	int32_t v = (int32_t)a + b;

	return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

static void
period_fill(unsigned period, uint32_t frames)
{
	int16_t *samples = (int16_t *)(buffer + 4096 * period);
	bool toning = now_us() < tone_until_us;
	uint32_t n = 0, c = 0;

	if (call_mode) {
		/* 1090: the call's downlink first */
		uint32_t tail = call_tail, available = call_head - tail;

		if (!call_primed && available >= frames) {
			call_primed = true;
		}
		if (call_primed) {
			c = available < frames ? available : frames;
			if (c < frames) {
				call_underruns++;
				call_primed = false;
			}
			OSMemoryBarrier();
			for (uint32_t i = 0; i < c; i++) {
				samples[2 * i] = call_ring[2 * ((tail + i) % CALL_RING_FRAMES)];
				samples[2 * i + 1] = call_ring[2 * ((tail + i) % CALL_RING_FRAMES) + 1];
			}
			OSMemoryBarrier();
			call_tail = tail + c;
		}
		call_periods++;
	}
	if (!toning && level >= 4) {
		uint32_t tail = ring_tail, available = ring_head - tail;

		n = available < frames ? available : frames;
		OSMemoryBarrier();
		for (uint32_t i = 0; i < n; i++) {
			if (i < c) {
				samples[2 * i] = mix16(samples[2 * i], ring[2 * ((tail + i) % RING_FRAMES)]);
				samples[2 * i + 1] = mix16(samples[2 * i + 1], ring[2 * ((tail + i) % RING_FRAMES) + 1]);
				continue;
			}
			samples[2 * i] = ring[2 * ((tail + i) % RING_FRAMES)];
			samples[2 * i + 1] = ring[2 * ((tail + i) % RING_FRAMES) + 1];
		}
		OSMemoryBarrier();
		ring_tail = tail + n;
		fed_frames += n;
	}
	for (unsigned frame = n > c ? n : c; frame < frames; frame++) {
		int16_t sample = toning ? tone[tone_at++ % 48] : 0;

		samples[2 * frame] = sample;
		samples[2 * frame + 1] = sample;
	}
	if (toning) {
		tone_periods++;
	} else {
		silent_periods++;
	}
	if (level >= 3) {
		/* every period, silence too: its filter and its averages go on (sweet_guard.c) */
		redwood_guard_process(samples, frames);
		if (frames == PERIOD_FRAMES || (++guard_half & 1) == 0) {
			guard_periods++;        /* (1090: counted in 20 ms) */
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");       /* (write-combining: out to memory before the DSP is told) */
}

static bool
period_write(unsigned period, uint32_t frames)
{
	uint64_t address = (buffer_pa + 4096 * period) | AUDIO_SID_BITS;
	uint32_t write[8] = { (uint32_t)address, (uint32_t)(address >> 32), map_handle, frames * 4, period, 0, 0, 0 };

	return redwood_adsp_apr(APR_SVC_ASM, STREAM_PORT, WRITE_TOKEN | period, ASM_DATA_CMD_WRITE_V2, write, sizeof(write), 0, NULL);
}

static void
feeder_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t done_before = written_done;
	uint64_t began = now_us(), last_report = began;
	bool said_none = false;
	uint32_t inflight = 0, idle = 0;        /* 1090: periods at the DSP / given back and held (call mode) */

	returned = 0;
	for (unsigned p = 0; p < PERIODS; p++) {
		period_fill(p, PERIOD_FRAMES);
		if (period_write(p, PERIOD_FRAMES)) {
			sent_periods++;
			inflight |= 1u << p;
			period_frames_of[p] = PERIOD_FRAMES;
		} else {
			failed_sends++;
		}
	}
	SAY("feeder: %u periods of 20 ms queued at the DSP\n", PERIODS);
	while (!stopping) {
		uint32_t back = returned;
		uint64_t now = now_us();
		bool cm = call_mode;
		uint32_t frames = cm ? CALL_PERIOD_FRAMES : PERIOD_FRAMES, most = cm ? CALL_QUEUED : PERIODS, q = 0;

		if (back != 0) {
			OSBitAndAtomic(~back, &returned);
			inflight &= ~back;
			idle |= back;
		}
		/* (without the call mode every period given back is filled and written again at once, lowest first, as before) */
		while (idle != 0 && (uint32_t)__builtin_popcount(inflight) < most) {
			unsigned p = (unsigned)__builtin_ctz(idle);

			idle &= ~(1u << p);
			period_fill(p, frames);
			if (period_write(p, frames)) {
				sent_periods++;
				inflight |= 1u << p;
				period_frames_of[p] = frames;
			} else {
				failed_sends++;
			}
		}
		for (unsigned p = 0; p < PERIODS; p++) {
			q += (inflight & (1u << p)) ? period_frames_of[p] : 0;
		}
		queued_frames = q;
		if (!said_none && written_done == done_before && now - began > 1000000) {
			SAY("feeder: THE DSP HAS GIVEN NO PERIOD BACK IN A SECOND\n");
			smmu_faults("(no WRITE_DONE)");
			said_none = true;
		}
		if (now - last_report >= 60000000ull) {
			last_report = now;
			SAY("feeder: %llu periods sent (%llu failed), %u done (%u errors), %u underflows; %llu tone, %llu silent\n",
			    sent_periods, failed_sends, written_done, write_errors, underflows, tone_periods, silent_periods);
		}
		if (returned == 0 && !stopping) {
			/* (a WRITE_DONE between the test and the wait is picked up at the 40 ms timeout: 160 ms are queued) */
			assert_wait_timeout((event_t)&feeder_wake, THREAD_UNINT, cm ? 5 : 40, NSEC_PER_MSEC);   /* 1090: 30 ms queued in a call */
			thread_block(THREAD_CONTINUE_NULL);
		}
	}
	SAY("feeder stops: %llu periods sent (%llu failed), %u done (%u errors), %u underflows; %llu tone, %llu silent; "
	    "from iOS %llu frames (%llu dropped), %llu fed\n", sent_periods, failed_sends, written_done, write_errors, underflows,
	    tone_periods, silent_periods, ios_frames, ios_dropped, fed_frames);
	feeder_runs = false;
	thread_terminate(current_thread());
}

/* ---- 1090: the call mode's interface (sweet_vproxy.c) ---- */

bool redwood_sound_call_mode_on(bool on);
void
redwood_sound_call_mode(bool on)
{
	(void)redwood_sound_call_mode_on(on);
}

/* true: the stream runs (sound >= 2), so what is put in the call ring is played */
bool
redwood_sound_call_mode_on(bool on)
{
	if (on && !call_mode) {
		call_tail = call_head;
		call_primed = false;
		call_underruns = call_overruns = call_periods = 0;
		OSMemoryBarrier();
		call_mode = true;
		SAY("call mode ON: periods of 10 ms, at most %u at the DSP (was %u x 20 ms; the 20 ms ones drain first)%s\n",
		    CALL_QUEUED, PERIODS, feeder_runs ? "" : " -- NO STREAM RUNS (sound < 2): the call ring is not played");
	} else if (!on && call_mode) {
		call_mode = false;
		SAY("call mode off: %u periods of 10 ms, %u underruns, %u overruns; back to %u x 20 ms\n", call_periods,
		    call_underruns, call_overruns, PERIODS);
	}
	thread_wakeup(&feeder_wake);
	return feeder_runs;
}

/* From the proxy's thread: never waits. 48 kHz stereo frames; what does not fit is dropped (counted). */
uint32_t
redwood_sound_call_put(const int16_t *stereo, uint32_t frames)
{
	uint32_t head = call_head, room = CALL_RING_FRAMES - (head - call_tail), taken = frames < room ? frames : room;

	if (!call_mode) {
		return 0;
	}
	for (uint32_t i = 0; i < taken; i++) {
		call_ring[2 * ((head + i) % CALL_RING_FRAMES)] = stereo[2 * i];
		call_ring[2 * ((head + i) % CALL_RING_FRAMES) + 1] = stereo[2 * i + 1];
	}
	OSMemoryBarrier();
	call_head = head + taken;
	if (taken < frames) {
		call_overruns++;
	}
	return taken;
}

void
redwood_sound_call_state(uint32_t *ring_frames, uint32_t *queued, uint32_t *underruns, uint32_t *periods)
{
	*ring_frames = call_head - call_tail;
	*queued = queued_frames;
	*underruns = call_underruns;
	*periods = call_periods;
}

/* Q15: the feeder's sndgain (sound=4: iOS keeps its peaks that far under full scale) undone for the call's PCM, 1 dB steps */
uint32_t
redwood_sound_call_gain_q15(void)
{
	static const uint32_t db_q15[8] = { 32768, 29205, 26029, 23198, 20675, 18427, 16423, 14637 }; /* 0..-7 dB */
	uint32_t g = level >= 4 ? guard_gain : 0, whole = (g + 4) / 8, v = 32768;

	while (whole >= 6) {
		v /= 2;         /* (6 dB ~ a half) */
		whole -= 6;
	}
	return v * db_q15[whole] / 32768;
}

/* The tone for ms (into whatever plays). */
static void
tone_for(uint32_t ms)
{
	uint32_t done = written_done;

	tone_until_us = now_us() + 1000ull * ms;
	SAY("tone: 1 kHz at -30 dBFS for %u ms into the DSP%s\n", ms, level >= 3 && sweet_amp_ready() ?
	    " (AUDIBLE: the amplifiers follow)" : " (amplifiers off: nothing to hear)");
	sleep_ms(ms + 200);
	SAY("tone over: %u periods came back meanwhile (%u errors, %u underflows): the DSP took %u frames/s\n", written_done - done,
	    write_errors, underflows, (written_done - done) * PERIOD_FRAMES * 1000 / (ms + 200));
}

/* ---- the amplifiers: on while something plays, off after 2 s of silence ------------------------------------------ */

#define QUIET_BEFORE_OFF_US     2000000ull

static volatile bool amps_on, amps_held_off, amps_given_up, watcher_runs;
static volatile uint32_t below_wanted;          /* set by poke `a gain`; applied by the watcher */
/*
 * 1065, a call (redwood_sound_voice(), sweet_voice.c): which amplifiers it holds on (bit 0 = 0x40 earpiece, bit 1 = 0x41
 * speaker; 0 = no call) and at what level; the watcher applies them (voice_applied / voice_below_applied).
 */
static volatile uint32_t voice_amps, voice_below = 240, voice_applied, voice_below_applied;
static bool voice_own_port, voice_direct_amps;         /* (no watcher: what the call brought up itself) */
static uint32_t amp_starts;

static const char *
db(char *text, uint32_t eighths)
{
	snprintf(text, 16, "%u.%03u", eighths / 8, eighths % 8 * 125);
	return text;
}

static void
guard_say(const char *when)
{
	struct redwood_guard_report r;

	redwood_guard_take_report(&r);
	if (r.blocks == 0) {
		return;
	}
	SAY("guard %s: %u blocks, peaks in %u out %u; held down by the 0.2 s/3 s/60 s averages in %u/%u/%u; %u frames "
	    "limited; gain now %d/10 dB (least %d/10)\n", when, r.blocks, r.peak_in, r.peak_out, r.rms_blocks, r.fast_blocks,
	    r.slow_blocks, r.limited_frames, redwood_guard_decibels(r.gain * r.gain >> 30),
	    redwood_guard_decibels(r.least_gain * r.least_gain >> 30));
}

static void
amps_off(const char *why)
{
	char t[16];

	if (!amps_on) {
		return;
	}
	(void)sweet_amp_volume(0xffffffffu);
	sweet_amp_stop();
	amps_on = false;
	SAY("amplifiers OFF (%s); they were at %s dB under Android's level\n", why, db(t, below));
	guard_say("while they were on");
}

/* sound >= 3, on the sound thread until the run ends */
static void
watch(void)
{
	uint64_t last_look = 0;
	uint32_t generation = 0, ios_below = 0, changes = 0;
	bool ios_muted = false;
	char t[16], t2[16], t3[16];

	watcher_runs = true;
	while (!stopping) {
		uint64_t now = now_us(), until = tone_until_us, sounded = last_sound_us;
		uint32_t vmask = voice_amps, vbelow = voice_below;
		bool wanted = (now < until + QUIET_BEFORE_OFF_US && until != 0) ||
		    (level >= 4 && !ios_muted && sounded != 0 && now - sounded < QUIET_BEFORE_OFF_US) || vmask != 0;
		uint32_t b, g;
		bool m;

		if (level >= 4 && redwood_audio_volume(&b, &m, &g) && g != generation) {
			generation = g;
			ios_below = b > 816 ? 816 : b;
			ios_muted = m;
			if (changes++ < 16) {
				SAY("iOS volume: %s%s dB under the loudest\n", m ? "muted, " : "", db(t2, ios_below));
			}
			below = ~0u;            /* (applied just below, with iOS's part) */
		}
		if (vmask != voice_applied || (vmask != 0 && vbelow != voice_below_applied)) {
			/* 1065: a call began, changed amplifier or level, or ended */
			if (vmask != 0) {
				sweet_amp_select(vmask);
				redwood_guard_attenuation(vbelow);
				if (amps_on) {
					(void)sweet_amp_volume(vbelow);
					(void)sweet_amp_start();        /* (the one the call wants, if it was not on) */
				}
				SAY("voice: the call holds amplifier%s %s at %s dB under Android's level\n", vmask == 3 ? "s" : "",
				    vmask == 1 ? "0x40 (earpiece)" : vmask == 2 ? "0x41 (speaker)" : "0x40 and 0x41", db(t, vbelow));
			} else {
				amps_off("the call ended");
				sweet_amp_select(3);
				redwood_guard_attenuation(below + ios_below);
				SAY("voice: the call let the amplifiers go\n");
			}
			voice_applied = vmask;
			voice_below_applied = vbelow;
		}
		if (below_wanted != below) {
			uint32_t effective;

			below = below_wanted;
			effective = below + ios_below;
			redwood_guard_attenuation(voice_applied ? voice_below_applied : effective);
			if (amps_on) {
				(void)sweet_amp_volume(voice_applied ? voice_below_applied : effective);
			}
			SAY("amplifiers' level: %s dB under Android's (sndbelow/gain %s + iOS %s)\n", db(t, effective), db(t2, below),
			    db(t3, ios_below));
		}
		if (wanted && !amps_on && !amps_held_off && !amps_given_up && sweet_amp_ready()) {
			uint32_t at = voice_applied ? voice_below_applied : below + ios_below;      /* 1065: a call's level */

			/* the guard is armed at the level they will play at before they are started */
			redwood_guard_attenuation(at);
			guard_say("(silence before)");
			if (!sweet_amp_start()) {
				SAY("THE AMPLIFIERS DID NOT START: given up\n");
				amps_given_up = true;
				sweet_amp_stop();
			} else {
				amps_on = true;
				amp_starts++;
				/* faded in from 30 dB further down, 6 dB a step */
				for (uint32_t extra = 240; extra > 0; extra -= 48) {
					(void)sweet_amp_volume(at + extra);
					sleep_ms(8);
				}
				(void)sweet_amp_volume(at);
				last_look = now_us();
				ios_peak = 0;
				SAY("amplifiers ON at %s dB under Android's level (start %u%s)\n", db(t, at), amp_starts,
				    voice_applied ? ", for a call" : "");
			}
		} else if (amps_on && (!wanted || amps_held_off)) {
			if (level >= 4) {
				SAY("what iOS wrote while they were on peaked at %u of 32768\n", ios_peak);
			}
			amps_off(amps_held_off ? "poke a off" : ios_muted ? "iOS muted" : vmask != voice_applied ? "the call ended" :
			    "2 s of silence");
		} else if (amps_on && now - last_look >= 200000) {
			last_look = now;
			if (!sweet_amp_healthy()) {
				amps_on = false;
				amps_given_up = true;
				SAY("an amplifier reported an error: given up for this boot\n");
			} else if (guard_periods >= 250) {
				guard_periods = 0;
				guard_say("(5 s)");
			}
		}
		sleep_ms(10);
	}
	amps_off("the run ends");
	watcher_runs = false;
}

/* ---- the whole ------------------------------------------------------------------------------------------------ */

static bool running, path_up, taken_down;

static void
take_down(void)
{
	stopping = true;
	if (level >= 4) {
		redwood_audio_set_sink("nothing", NULL);
	}
	thread_wakeup(&feeder_wake);
	for (unsigned ms = 0; ms < 2000 && (feeder_runs || watcher_runs); ms++) {
		sleep_ms(1);
	}
	lck_mtx_lock(&sound_lock);
	if (running && !taken_down) {
		taken_down = true;
		if (amps_on) {
			sweet_amp_stop();
			amps_on = false;
		}
		stream_off();
		afe_off();
		pins_off();
		devote(LPASS_HW_DCODEC_BLOCK);
		devote(LPASS_HW_MACRO_BLOCK);
		redwood_adsp_apr_events(NULL);
		space_put(0);
		path_up = false;
		SAY("everything of the speakers' path is stopped and put back\n");
	}
	lck_mtx_unlock(&sound_lock);
}

static void
watcher_thread(__unused void *parameter, __unused wait_result_t result)
{
	watch();
	thread_terminate(current_thread());
}

static void
sound_thread(__unused void *parameter, __unused wait_result_t result)
{
	bool ok = false;
	thread_t feeder;

	lck_mtx_lock(&sound_lock);
	SAY("sound=%u: %s\n", level, level == 1 ? "votes, LPI pins, QUIN TDM clock/group/port only" : level == 2 ?
	    "the port and a stream with a -30 dBFS test tone, amplifiers OFF" : level == 3 ?
	    "the port, a stream, the guard and the amplifiers: the test tone AUDIBLE" :
	    "what iOS plays, through the port, a stream, the guard and the amplifiers");
	if (!vote(LPASS_HW_MACRO_BLOCK, "LPASS_HW_MACRO") || !vote(LPASS_HW_DCODEC_BLOCK, "LPASS_HW_DCODEC")) {
		SAY("the LPASS votes were not granted\n");
	}
	if (level >= 2) {
		if (!space_get(0)) {
			goto out;
		}
		redwood_adsp_apr_events(events);
	}
	(void)pins_on();
	if (!afe_on()) {
		goto out;
	}
	if (level == 1) {
		SAY("QUIN TDM port up: holding %u s\n", hold_secs);
		sleep_ms(hold_secs * 1000);
		ok = true;
		goto out;
	}
	if (!stream_on()) {
		goto out;
	}
	feeder_runs = true;
	if (kernel_thread_start(feeder_thread, NULL, &feeder) != KERN_SUCCESS) {
		feeder_runs = false;
		SAY("no feeder thread\n");
		goto out;
	}
	thread_deallocate(feeder);
	path_up = true;
	if (level >= 3) {
		char t[16], t2[16];

		/* the guard first, at the amplifiers' level; then the amplifiers, powered down (sweet_cs35l41.c) */
		redwood_guard_configure(true, level >= 4 ? guard_gain : 0);
		redwood_guard_attenuation(below);
		below_wanted = below;
		SAY("guard armed: high-pass 200 Hz, gain %s dB, limits reckoned at %s dB under Android's level\n",
		    db(t, level >= 4 ? guard_gain : 0), db(t2, below));
		if (!sweet_amp_prepare()) {
			SAY("no amplifier: the tone goes into the DSP unheard\n");
		}
	}
	lck_mtx_unlock(&sound_lock);
	SAY("PATH UP: QUIN_TDM_RX_0 0x9040, COPP %u, ASM session 1 running; poke `a tone MS` plays the test tone\n", copp);
	if (level >= 3) {
		thread_t watcher;

		if (level == 3) {
			tone_until_us = now_us() + 300000ull + 1000000ull;      /* (the amplifiers come up meanwhile) */
		}
		if (kernel_thread_start(watcher_thread, NULL, &watcher) == KERN_SUCCESS) {
			thread_deallocate(watcher);
		}
		if (level >= 4) {
			redwood_audio_set_sink("the speakers (bsd/dev/sweet_sound.c, the ADSP)", ios_sink);
			SAY("WHAT IOS PLAYS GOES TO THE SPEAKERS from here on (sound=4)\n");
		}
		sleep_ms(1500);
		smmu_faults("after the tone");
		SAY("sound=%u: LEVEL DONE (the path stays up; the amplifiers follow what plays)\n", level);
		thread_terminate(current_thread());
	}
	tone_for(1000);
	smmu_faults("after the tone");
	SAY("sound=%u: LEVEL DONE (the path stays up feeding silence)\n", level);
	thread_terminate(current_thread());
out:
	lck_mtx_unlock(&sound_lock);
	take_down();
	SAY("sound=%u: %s\n", level, ok ? "LEVEL DONE" : "STOPPED SHORT");
	thread_terminate(current_thread());
}

void
redwood_sound_dsp_up(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("sound", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (level > 4) {
		level = 4;
	}
	below_min = 48;         /* hard cap: 6 dB under Android's level (ASP bypass, no Halo protection firmware) */
	PE_parse_boot_argn("sndgain", &guard_gain, sizeof(guard_gain));
	guard_gain = guard_gain > REDWOOD_GUARD_MAX_GAIN ? REDWOOD_GUARD_MAX_GAIN : guard_gain;
	PE_parse_boot_argn("sndsecs", &hold_secs, sizeof(hold_secs));
	PE_parse_boot_argn("sndbelow", &below, sizeof(below));
	below = below < below_min ? below_min : below;
	PE_parse_boot_argn("sndlpi", &use_lpi, sizeof(use_lpi));
	running = true;
	if (kernel_thread_start(sound_thread, NULL, &thread) != KERN_SUCCESS) {
		running = false;
		SAY("no thread\n");
		return;
	}
	thread_deallocate(thread);
}

void
redwood_sound_dsp_down(void)
{
	if (voice_own_port || voice_direct_amps) {
		(void)redwood_sound_voice(0, 0);        /* 1065 */
	}
	if (running) {
		take_down();
	}
}

void
redwood_sound_quiesce(void)
{
}

/* ---- 1065: a call (bsd/dev/sweet_voice.c) ---------------------------------------------------------------------- */

/*
 * The port 0x9040 and the amplifiers for a call. With sound>=3 the watcher owns the amplifiers: it is told and
 * waited for. Without (sound=0, or the path down): the call brings up the votes, pins, clock and port itself
 * (no ADM/ASM: the DSP's voice processor is the port's only client then) and drives the amplifiers directly; at
 * the call's end it takes down only what it brought up.
 */
bool
redwood_sound_voice(uint32_t amps, uint32_t level_below)
{
	char t[16];
	bool up;

	amps &= 3;
	level_below = level_below < below_min ? below_min : level_below;
	if (watcher_runs && !stopping) {
		voice_below = level_below;
		voice_amps = amps;
		for (unsigned ms = 0; ms < 2000 && (voice_applied != amps || (amps != 0 && voice_below_applied != level_below)); ms += 10) {
			sleep_ms(10);
		}
		SAY("voice: %s (watcher; port 0x9040 %s, amplifiers %s)\n", amps ? "held" : "released", port_on ? "running" : "DOWN",
		    amps_on ? "on" : amps_held_off ? "held off (poke a off)" : amps_given_up ? "GIVEN UP" : "off");
		return amps != 0 && port_on;
	}
	lck_mtx_lock(&sound_lock);
	if (amps != 0 && !port_on && !voice_own_port) {
		if (running && !taken_down) {
			SAY("voice: sound=%u's path is being set up or is not up: the port is not taken over\n", level);
			lck_mtx_unlock(&sound_lock);
			return false;
		}
		SAY("voice: no speaker path running (sound=%u): the call brings up QUIN_TDM_RX_0 0x9040 itself\n", level);
		PE_parse_boot_argn("sndlpi", &use_lpi, sizeof(use_lpi));
		(void)vote(LPASS_HW_MACRO_BLOCK, "LPASS_HW_MACRO");
		(void)vote(LPASS_HW_DCODEC_BLOCK, "LPASS_HW_DCODEC");
		(void)pins_on();
		voice_own_port = true;
		if (!afe_on()) {
			SAY("voice: port 0x9040 DID NOT START\n");
			afe_off();
			pins_off();
			devote(LPASS_HW_DCODEC_BLOCK);
			devote(LPASS_HW_MACRO_BLOCK);
			voice_own_port = false;
			lck_mtx_unlock(&sound_lock);
			return false;
		}
	}
	if (amps != 0) {
		if (!sweet_amp_prepare()) {
			SAY("voice: no amplifier ready: the call is not heard\n");
		} else {
			sweet_amp_select(amps);
			(void)sweet_amp_volume(level_below);
			if (!sweet_amp_start()) {
				SAY("voice: THE AMPLIFIER DID NOT START\n");
			} else {
				voice_direct_amps = true;
				SAY("voice: amplifier%s %s on at %s dB under Android's level (direct)\n", amps == 3 ? "s" : "",
				    amps == 1 ? "0x40 (earpiece)" : amps == 2 ? "0x41 (speaker)" : "0x40 and 0x41", db(t, level_below));
			}
		}
	} else {
		if (voice_direct_amps) {
			sweet_amp_stop();
			sweet_amp_select(3);
			voice_direct_amps = false;
		}
		if (voice_own_port) {
			afe_off();
			pins_off();
			devote(LPASS_HW_DCODEC_BLOCK);
			devote(LPASS_HW_MACRO_BLOCK);
			voice_own_port = false;
			SAY("voice: the call's own port 0x9040 is stopped and put back\n");
		}
	}
	voice_applied = amps;
	voice_below_applied = level_below;
	up = amps != 0 && port_on;
	lck_mtx_unlock(&sound_lock);
	return up;
}

/* ---- poke `a ...` (bsd/dev/sweet_poke.c) -------------------------------------------------------------------------- */

static const char *
word(const char **p, const char *end, size_t *len)
{
	const char *s = *p, *w;

	while (s < end && (*s == ' ' || *s == '\t')) {
		s++;
	}
	w = s;
	while (s < end && *s != ' ' && *s != '\t' && *s != '\n' && *s != '#') {
		s++;
	}
	*len = (size_t)(s - w);
	*p = s;
	return w;
}

static bool
is(const char *w, size_t len, const char *name)
{
	return len == strlen(name) && strncmp(w, name, len) == 0;
}

static uint32_t
number(const char **p, const char *end, uint32_t otherwise)
{
	size_t len;
	const char *w = word(p, end, &len);
	uint32_t v = 0;

	if (len == 0) {
		return otherwise;
	}
	for (size_t i = 0; i < len; i++) {
		if (w[i] < '0' || w[i] > '9') {
			return otherwise;
		}
		v = v * 10 + (uint32_t)(w[i] - '0');
	}
	return v;
}

int
sweet_sound_cmd(const char *s, const char *end)
{
	size_t len;
	const char *w = word(&s, end, &len);

	if (len == 0 || is(w, len, "status")) {
		SAY("status: sound=%u path %s, votes %d/%d, pins %s, port %s, stream %s; %llu periods sent, %u done (%u errors), "
		    "%u underflows; amplifiers %s%s%s, %u starts\n", level, path_up ? "UP" : "down", voted[LPASS_HW_MACRO_BLOCK],
		    voted[LPASS_HW_DCODEC_BLOCK], pins_taken ? "taken" : "-", port_on ? "on" : "off", stream_runs ? "running" : "-",
		    sent_periods, written_done, write_errors, underflows, amps_on ? "ON" : "off", amps_held_off ? " (held off)" : "",
		    amps_given_up ? " (GIVEN UP)" : "", amp_starts);
		smmu_faults("(status)");
		if (level >= 3) {
			sweet_amp_status();
		}
		sweet_mic_status();
		return 0;
	}
	if (is(w, len, "tone")) {
		uint32_t ms = number(&s, end, 1000);

		if (!path_up) {
			SAY("tone: the path is not up (sound=2 needed)\n");
			return -1;
		}
		ms = ms > 5000 ? 5000 : ms;
		tone_until_us = now_us() + 1000ull * ms;
		SAY("tone: 1 kHz -30 dBFS for %u ms%s\n", ms, level >= 3 && !amps_held_off && !amps_given_up && sweet_amp_ready() ?
		    " (AUDIBLE)" : " (amplifiers off)");
		return 0;
	}
	if (is(w, len, "gain")) {
		uint32_t dbs;
		char t[16], t2[16];

		/* `a gain 10' and `a gain -10' both mean 10 dB under Android's level (0 = Android's own) */
		while (s < end && (*s == ' ' || *s == '\t')) {
			s++;
		}
		if (s < end && *s == '-') {
			s++;
		}
		dbs = number(&s, end, ~0u);
		if (dbs == ~0u) {
			SAY("gain: `a gain N' (N dB under Android's level, 0..102) wants a number\n");
			return -1;
		}
		if (level < 3 || !path_up) {
			SAY("gain: the amplifiers are not in use (sound=%u, path %s)\n", level, path_up ? "up" : "down");
			return -1;
		}
		dbs = dbs > 102 ? 102 : dbs;
		below_wanted = dbs * 8 < below_min ? below_min : dbs * 8;
		if (dbs * 8 < below_min) {
			SAY("gain: the amplifiers %s dB under Android's level from now on (CAPPED: ASP bypass without protection "
			    "firmware, no louder than %s dB under)\n", db(t, below_wanted), db(t2, below_min));
		} else {
			SAY("gain: the amplifiers %s dB under Android's level from now on\n", db(t, below_wanted));
		}
		return 0;
	}
	if (is(w, len, "off")) {
		amps_held_off = true;
		SAY("off: the amplifiers are stopped and kept off (`a on' allows them again)\n");
		return 0;
	}
	if (is(w, len, "probe")) {
		if (level < 3 || !path_up) {
			SAY("probe: the amplifiers are not in use (sound=%u, path %s)\n", level, path_up ? "up" : "down");
			return -1;
		}
		sweet_amp_reprobe();    /* (the amplifiers that did not answer: reset, DEVID, configure; they join at the next ON) */
		return 0;
	}
	if (is(w, len, "mic")) {
		return sweet_mic_cmd(number(&s, end, 3));
	}
	if (is(w, len, "on")) {
		amps_held_off = false;
		SAY("on: the amplifiers may run again\n");
		return 0;
	}
	SAY("poke a: unknown command (status, tone [MS], gain DB, off, on, probe, mic SECS)\n");
	return -1;
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
