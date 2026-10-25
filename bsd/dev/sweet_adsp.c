/*
 * redfin (Pixel 5, SM7250 lito): the audio DSP started and spoken to. Patch 0915: HackiOS's
 * bsd/dev/redwood_adsp.c (github.com/oosexgang/hackios-xnu-kernel, APSL 2.0, for the POCO X5 Pro
 * SM6375) with lito's numbers. The two SoCs agree on everything this file touches but one: the
 * shared memory at 0x80900000 and its items (SMP2P 429/443, last words 423, GLINK 478-480), IPCC
 * client 3 (LPASS) signals 0 and 2 (live DT qcom,smp2p-adsp, qcom,glink/adsp), the AOP's QMP at
 * 0x0c300000, cmd-db at 0x80860000, rails lcx.lvl/lmx.lvl (pm8150_l18_level/pm8150_l4_level here),
 * PAS id 1. Different: the DSP's memory, pil_adsp_region 0x89200000, 40 MB (its image's segments
 * cover exactly that). Changed for this kernel: the firmware comes from /private/var/local/fw on the
 * root (Android's /vendor/firmware/adsp.mdt, adsp.b00..b33); the rails are asked through
 * bsd/dev/sweet_rpmh.c, which shares the APPS RSC with the GPU, display and touch votes; there is no
 * loader to return to or log for; the battery manager (SM6375's ADSP keeps the fuel gauge) is not
 * started (battmgr=1 to try: lito's battery is on the PMIC, bsd/dev/sweet_battery.c); and both
 * adsp=1 and adsp_step=N run on a thread of their own once the root is mounted.
 *
 * What follows is HackiOS's description, which holds for redfin with the numbers above.
 *
 * The audio DSP of the POCO X5 Pro (redwood), started, and spoken to: step 1 of sound. Every
 * address below belongs to this phone, and nothing here is touched unless a boot-arg asks for it.
 *
 * The SoC's audio (LPASS: its clocks, its I2S and SoundWire, the codec's macros) does not belong
 * to the application processor. It is the audio DSP's -- a Hexagon of its own inside LPASS,
 * Android's "adsp" -- which stays in reset until somebody loads it, exactly as the Wi-Fi's does
 * (redwood_wifi.c): the phone's own signed image, adsp.mdt and adsp.b02 to adsp.b36 from
 * /vendor/firmware_mnt/image of its Android (20 MB: in the system image under /fw, or with ADSP=1 in
 * the small one, see image_file()), goes into the 64 MB reserved for it, TrustZone is
 * asked to authenticate and start it, and from there everything is conversation through the
 * shared memory: SMP2P for "up", "given up", "stopped"; GLINK for channels; QRTR on the channel
 * "IPCRTR" for the services the DSP offers. (Reading an LPASS clock register from here ends the
 * kernel with an external abort: the DSP owns that block, and talks about it only this way.)
 * What audio needs of it later -- the audio services, the codec, the speaker's path -- is for the
 * steps after this one; this file only brings the DSP up and the link with it.
 *
 * The way is the Wi-Fi DSP's, taken over from redwood_wifi.c with the audio DSP's numbers: its
 * comments say why each thing is done so. What is different is said here:
 *   - processor 2 (not 13) in the shared memory; its SMP2P items 443 (its) and 429 (ours), its
 *     last words in item 423; GLINK's three items in our partition with it
 *   - its doorbells are IPCC client 3 (LPASS): signal 0 GLINK, signal 2 SMP2P
 *   - TrustZone knows it as image 1; its memory is 0x86700000, 64 MB (dt: qcom,lpass@3700000,
 *     reserved-memory adsp@86700000), every byte of which the image takes
 *   - the always-on processor is told of it as "adsp"
 *   - the Wi-Fi DSP may be running at the same time (wifi=1): the mailbox to the always-on
 *     processor and the lock for allocating in the shared memory are the two things both use,
 *     each behind a lock both take (redwood_wifi.c); the doorbells are only counted here, and
 *     only the audio DSP's are cleared from here
 * The code is a copy, not a shared library, for now: step 1 is not to change what the Wi-Fi does.
 *
 * Boot-arg adsp_step=N performs steps 1 to N and stops; as with wifi_step, a step says what it is
 * about to touch before it touches it, and a step that does not find what it needs ends the
 * sequence. adsp=1 is all of them, on a thread beside the system's start.
 *
 *   1  reads   the shared memory: our partition with the DSP, its items, whether it has run
 *              since the phone was started (its SMP2P item is there if so), its last words
 *   2  reads   the firmware in the small image: is it whole, does it fit the reserved memory
 *   3  writes  the always-on processor's mailbox open, unless it is (the Wi-Fi may have opened it)
 *   4  asks    TrustZone to take the image's header (image 1) and where the image will be; a
 *              DSP that a kernel before this one left running is asked to stop and shut down first
 *   5  writes  the image into the DSP's memory, and reads it back
 *   6  writes  our SMP2P item (429) into the shared memory, and our two marks of GLINK to zero
 *   7  asks    RPMh for the DSP's rails, writes "adsp: on" to the always-on processor, asks
 *              TrustZone to authenticate and start the DSP, and watches for its first words
 *              (answering in our item what SMP2P has us answer)
 *   8  reads   GLINK's queues
 *   9  writes  our side of GLINK: the versions, the channel "IPCRTR" opened both ways, our intents;
 *              from here a thread of ours listens to the DSP
 *  10  talks   QRTR on that channel: hello both ways, and the services the DSP tells of, written
 *              down for adsp_listen seconds (default 3)
 *  11  talks   APR, on the channel apr_audio_svc that the DSP opens and we answer as well: whether
 *              the audio services are up, and which there are (see step_apr())
 *
 * Once everything is up with adsp=1, the sound's path takes over (redwood_sound.c, boot-arg sound=N):
 * redwood_adsp_apr() is how it speaks to the audio services (any service, port and token; what
 * nobody waits for goes to its handler of events), and it is stopped before the DSP is.
 *
 * What is started is stopped: when the steps are over (unless adsp_keep=1, and then when the run
 * ends), and on a panic's way back to the loader. The DSP is asked first (bit 0 of our entry
 * "master-kernel"), then TrustZone shuts it down, then the always-on processor hears "adsp: off".
 * A DSP that a kernel before this one left running is found by its item and treated the same way
 * before this one is started; one that was shut down without having said it stopped is not
 * started again unless adsp_again=1 (the Wi-Fi DSP reset the phone, twice, when that was tried).
 *
 * More boot-args: adsp_new=K (steps K on have not been seen to work, and wait adsp_pause=MS after
 * each announcement, default 2200: what the loader needs to note the end of the log), adsp_wait=S
 * (how long step 7 watches for "up", default 10), adsp_keep=1, adsp_again=1, adsp_return=1 (back
 * to the loader when the steps are over).
 *
 * Nothing here writes a clock or changes a pin. One request goes to RPMh, as Linux makes it: the
 * DSP's two rails, LCX and LMX, at level 384 while it boots, given back when it says it has asked
 * for its own (see rails()).
 *
 * Addresses are from the phone's own device tree (qcom,lpass@3700000, qcom,smp2p-adsp,
 * qcom,glink/adsp, qcom,ipcc@408000, power-controller@c300000, reserved-memory), layouts from
 * Linux (see redwood_wifi.c for the files).
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <os/atomic_private.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <vm/memory_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <vm/vm_kern.h>
#include <sys/redwood_audio.h>
/* (0950: /dev/redwood-fastrpc) */
#include <sys/param.h>
#include <sys/conf.h>
#include <sys/fcntl.h>
#include <sys/ioccom.h>
#include <sys/proc.h>
#include <sys/select.h>
#include <sys/uio.h>
#include <miscfs/devfs/devfs.h>
#include <vm/pmap.h>
#include <arm/caches_internal.h>
#include <sys/sysctl.h>                 /* (0956: kern.sweet_adsp_services) */
extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);

#if defined(ARM64_BOARD_CONFIG_SWEET)
/* (sys/redwood_input.h of HackiOS: what the battery manager below would hand on) */
struct redwood_battery_state {
	uint32_t size;
	int32_t percent_hundredths, voltage_uv, current_ua, temperature_hundredths, present, external, status;
	int32_t charge_uah, full_uah, design_uah, cycles;
	int32_t usb_type, usb_voltage_uv, usb_current_ua;
};
static void
redwood_battery_report(const struct redwood_battery_state *state)
{
	static unsigned said;
	if (said++ < 4) {
		printf("adsp: battmgr: %d.%02d %%, %d uV, %d uA\n", state->percent_hundredths / 100,
		    state->percent_hundredths % 100, state->voltage_uv, state->current_ua);
	}
}
/* The sound's and the microphones' paths (redwood_sound.c, redwood_mic.c): not here yet. */
__attribute__((weak)) void redwood_sound_dsp_up(void) {}
__attribute__((weak)) void redwood_sound_dsp_down(void) {}
__attribute__((weak)) void redwood_sound_quiesce(void) {}
void redwood_mic_dsp_up(void);
void redwood_mic_dsp_down(void);
__attribute__((weak)) void redwood_mic_dsp_up(void) {}
__attribute__((weak)) void redwood_mic_dsp_down(void) {}
extern int sweet_rpmh_write(const char *res, unsigned n, const uint32_t *offsets, const uint32_t *values);
#endif

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern uint64_t kvtophys(vm_offset_t va);
extern kern_return_t thread_terminate(thread_t thread);

static LCK_GRP_DECLARE(adsp_lock_group, "redwood-adsp");
/* (redwood_wifi.c's locks; on redfin the modem's Wi-Fi runs in userland, phone/wifi, which opens the AOP mailbox once at its start) */
static LCK_MTX_DECLARE(redwood_aop_mailbox_lock, &adsp_lock_group);
static LCK_MTX_DECLARE(redwood_smem_alloc_lock, &adsp_lock_group);

#define STEPS                   11

static uint32_t last_step, pause_ms = 2200, listen_seconds = 3;
static uint32_t step;                   /* the one being performed */
static uint32_t new_from;               /* the first of the steps that have not been seen to work: they wait */
static char last_write[96] = "nothing written";
/*
 * 0950: adsp_sleep=1: our SMP2P item carries every entry Android's kernel makes in it (dt: the children of
 * qcom,smp2p-adsp with qcom,smem-state-cells, in order), at Android's boot values: "master-kernel" 0,
 * "sleepstate" 1 (smp2p-sleepstate: bit 0 = APPS awake, set at its probe), "rdbg" 0. adsp_sleep=0 (the
 * default): "master-kernel" only, as before.
 * adsp_qlog=1: every QRTR datagram on IPCRTR (both ways: header and the first 48 bytes) and every intent
 * event of that channel logged, the first 200 lines, then one line per 100 datagrams.
 */
static uint32_t adsp_sleep, adsp_qlog;
static uint32_t qlog_lines, qlog_packets, qlog_in, qlog_out;

static bool
qlog_line(void)
{
	if (!adsp_qlog || qlog_lines >= 200) {
		return false;
	}
	qlog_lines++;
	return true;
}
static bool leaving;                    /* on the way back to the loader after a panic: no lock is taken, nothing sleeps */

/*
 * Every line goes to the log the loader reads from another core as well (as in redwood_wifi.c);
 * with adsp=1 only the few that say how it went (tell()), the rest to the kernel's own log.
 */
static void
loader_log_putc(__unused char c)
{
}

static LCK_MTX_DECLARE(say_lock, &adsp_lock_group);
static bool quiet;

static void
say_line(const char *line, bool aloud)
{
	if (leaving) {
		for (const char *c = line; *c != 0; c++) {
			loader_log_putc(*c);
		}
		return;
	}
	lck_mtx_lock(&say_lock);
	if (!quiet || aloud) {
		for (const char *c = line; *c != 0; c++) {
			loader_log_putc(*c);
		}
	}
	printf("%s", line);
	lck_mtx_unlock(&say_lock);
}

static int say(const char *format, ...) __printflike(1, 2);
static int tell(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	say_line(line, false);
	return length;
}

static int
tell(const char *format, ...)
{
	char line[400];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	say_line(line, true);
	return length;
}
#define printf say

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static uint64_t
now_us(void)
{
	uint64_t nanoseconds;

	absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
	return nanoseconds / 1000;
}

/* Until every write to memory so far has been carried out. */
static void
settle(void)
{
	__asm__ volatile ("dsb sy");
}

static bool
option(const char *name)
{
	uint32_t value = 0;

	return PE_parse_boot_argn(name, &value, sizeof(value)) && value != 0;
}

/* ---- register access (as in redwood_wifi.c) ------------------------------------------------------- */

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
	__asm__ volatile ("str %w0, [%1]" : : "r" (value), "r" (address) : "memory");
}

struct block {
	const char *name;
	uint32_t phys, size;
	vm_offset_t base;
};

static void
need(struct block *b)
{
	if (b->base == 0) {
		b->base = ml_io_map(b->phys, b->size);
	}
}

/* Time for the loader to note what has just been said, where it is not yet known to be harmless. */
static void
hold(void)
{
	if (step >= new_from && pause_ms && !leaving) {
		sleep_ms(pause_ms);
	}
}

static void
announce(const char *what)
{
	printf("adsp: step %u: %s\n", step, what);
	hold();
}

static uint32_t
get(struct block *b, const char *name, uint32_t offset)
{
	uint32_t value;

	need(b);
	value = rd(b->base + offset);
	printf("adsp: %u: %s %s (%#x) = %#x\n", step, b->name, name, b->phys + offset, value);
	return value;
}

static void
put(struct block *b, const char *name, uint32_t offset, uint32_t value)
{
	need(b);
	printf("adsp: %u: about to write %s %s at %#x: %#x\n", step, b->name, name, b->phys + offset, value);
	snprintf(last_write, sizeof(last_write), "step %u wrote %#x to %s %s at %#x", step, value, b->name, name, b->phys + offset);
	wr(b->base + offset, value);
}

/* ---- the shared memory (redwood_wifi.c, step 1) --------------------------------------------------- */

#define SMEM_PHYS               0x80900000UL
#define SMEM_SIZE               0x00200000u
#define SMEM_US                 0
#define SMEM_ADSP               2               /* dt: qcom,remote-pid of qcom,smp2p-adsp and qcom,glink/adsp */
#define SMEM_GLOBAL             0xfffe
#define SMEM_CANARY             0xa5a5

struct smem_table_entry {
	uint32_t offset, size, flags;
	uint16_t host0, host1;
	uint32_t cacheline, reserved[7];
};
struct smem_table {
	uint8_t magic[4];
	uint32_t version, count, reserved[5];
};
struct smem_partition {
	uint8_t magic[4];
	uint16_t host0, host1;
	uint32_t size, free_uncached, free_cached, reserved[3];
};
struct smem_entry {
	uint16_t canary, item;
	uint32_t size;
	uint16_t padding_data, padding_header;
	uint32_t reserved;
};
struct smem_item {
	uint32_t item, offset, size;
};

/*
 * The DSP's items (dt: qcom,smem of qcom,smp2p-adsp, <443 429>: the item the DSP writes first, then
 * ours -- the Wi-Fi's <617 616> are in the same order; taken the other way round, the DSP made
 * our item its own and never said "up" --, and qcom,smem-id of qcom,lpass@3700000).
 */
#define ITEM_SMP2P_OURS         429
#define ITEM_SMP2P_THEIRS       443
#define ITEM_CRASH_REASON       423

static const volatile uint8_t *smem;

static bool
smem_peek(uint64_t offset, void *to, uint32_t size)
{
	if (smem == NULL) {
		smem = (const volatile uint8_t *)ml_io_map_unmappable(SMEM_PHYS, SMEM_SIZE, VM_WIMG_WCOMB);
	}
	if (offset + size > SMEM_SIZE) {
		return false;
	}
	for (uint32_t i = 0; i < size; i++) {
		((uint8_t *)to)[i] = smem[offset + i];
	}
	return true;
}

static void
smem_poke(uint32_t offset, uint32_t value)
{
	wr((vm_offset_t)smem + offset, value);
}

static uint32_t
smem_word(uint32_t offset)
{
	return rd((vm_offset_t)smem + offset);
}

static uint32_t
smem_partition(uint16_t one, uint16_t other, uint32_t *cacheline, uint32_t *size)
{
	struct smem_table table;
	struct smem_table_entry entry;
	struct smem_partition header;

	if (!smem_peek(SMEM_SIZE - 0x1000, &table, sizeof(table)) || memcmp(table.magic, "$TOC", 4) != 0 || table.version != 1 ||
	    table.count > 64) {
		return 0;
	}
	for (uint32_t i = 0; i < table.count; i++) {
		if (!smem_peek(SMEM_SIZE - 0x1000 + sizeof(table) + i * sizeof(entry), &entry, sizeof(entry)) || entry.offset == 0 ||
		    entry.size == 0) {
			continue;
		}
		if (!((entry.host0 == one && entry.host1 == other) || (entry.host0 == other && entry.host1 == one))) {
			continue;
		}
		if ((uint64_t)entry.offset + entry.size > SMEM_SIZE - 0x1000 || !smem_peek(entry.offset, &header, sizeof(header)) ||
		    memcmp(header.magic, "$PRT", 4) != 0 || header.size != entry.size) {
			return 0;
		}
		*cacheline = entry.cacheline;
		*size = entry.size;
		return entry.offset;
	}
	return 0;
}

static unsigned
smem_items(uint32_t partition, uint32_t cacheline, struct smem_item *found, unsigned max)
{
	struct smem_partition header;
	struct smem_entry entry;
	uint32_t at, step_back;
	unsigned count = 0;

	if (partition == 0 || !smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.size ||
	    header.free_cached > header.size) {
		return 0;
	}
	for (at = sizeof(header); at + sizeof(entry) <= header.free_uncached;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > header.size) {
			break;
		}
		if (count < max) {
			found[count].item = entry.item;
			found[count].offset = partition + at + sizeof(entry) + entry.padding_header;
			found[count].size = entry.size - entry.padding_data;
		}
		count++;
		at += sizeof(entry) + entry.padding_header + entry.size;
	}
	if (cacheline == 0 || (cacheline & (cacheline - 1))) {
		return count;
	}
	step_back = ((uint32_t)sizeof(entry) + cacheline - 1) & ~(cacheline - 1);
	for (at = header.size - step_back; at > header.free_cached && at < header.size;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > at) {
			break;
		}
		if (count < max) {
			found[count].item = entry.item;
			found[count].offset = partition + at - entry.size;
			found[count].size = entry.size - entry.padding_data;
		}
		count++;
		if (entry.size + step_back > at) {
			break;
		}
		at -= entry.size + step_back;
	}
	return count;
}

/* An item the DSP and we share: in the partition of the two, or else in the global one. Where it is; 0: nowhere. */
static uint32_t
smem_item(uint32_t item, uint32_t *size)
{
	static struct smem_item found[256];
	static const uint16_t hosts[2][2] = { { SMEM_US, SMEM_ADSP }, { SMEM_GLOBAL, SMEM_GLOBAL } };

	for (unsigned h = 0; h < 2; h++) {
		uint32_t cacheline = 0, partition_size = 0;
		uint32_t partition = smem_partition(hosts[h][0], hosts[h][1], &cacheline, &partition_size);
		unsigned count = smem_items(partition, cacheline, found, 256);

		for (unsigned i = 0; i < count && i < 256; i++) {
			if (found[i].item == item) {
				*size = found[i].size;
				return found[i].offset;
			}
		}
	}
	return 0;
}

static void
show_partition(void)
{
	static struct smem_item found[256];
	struct smem_partition header;
	uint32_t cacheline = 0, size = 0, partition = smem_partition(SMEM_US, SMEM_ADSP, &cacheline, &size);
	unsigned count;
	char line[320];

	if (partition == 0 || !smem_peek(partition, &header, sizeof(header))) {
		printf("adsp: %u: there is no partition for us and the audio DSP (0 and 2)\n", step);
		return;
	}
	count = smem_items(partition, cacheline, found, 256);
	printf("adsp: %u: the partition for us and the audio DSP (0 and 2): at %#lx, %#x bytes, of which %#x free; %u items\n", step,
	    SMEM_PHYS + partition, size, header.free_cached - header.free_uncached, count);
	for (unsigned i = 0; i < count && i < 256;) {
		int at = 0;

		for (unsigned n = 0; n < 12 && i < count && i < 256; n++, i++) {
			at += snprintf(line + at, sizeof(line) - (size_t)at, " %u (%u)", found[i].item, found[i].size);
		}
		printf("adsp: %u:   items (bytes):%s\n", step, line);
	}
}

/* ---- SMP2P ---------------------------------------------------------------------------------------- */

struct smp2p {
	uint32_t magic;
	uint32_t version;
	uint16_t local, remote, total, valid;
	uint32_t flags;
	struct {
		char name[16];
		uint32_t value;
	} entry[16];
};
#define SMP2P_MAGIC             0x504d5324
#define SMP2P_VERSION           1
#define SMP2P_RESTART_ACKS      1u
#define SMP2P_RESTART_DONE      (1u << 0)
#define SMP2P_RESTART_ACK       (1u << 1)
#define SMP2P_AT_VERSION        4
#define SMP2P_AT_PROCESSORS     8
#define SMP2P_AT_COUNTS         12
#define SMP2P_AT_FLAGS          16
#define SMP2P_AT_NAME(i)        (20 + 20 * (i))
#define SMP2P_AT_VALUE(i)       (36 + 20 * (i))

/* The bits of the DSP's entry "slave-kernel" (dt: the interrupts of qcom,lpass@3700000). */
#define DSP_FATAL               (1u << 0)
#define DSP_READY               (1u << 1)
#define DSP_HANDOVER            (1u << 2)
#define DSP_STOP_ACK            (1u << 3)
#define DSP_SHUTDOWN_ACK        (1u << 7)

/* Our entry (dt: the child "master-kernel" of qcom,smp2p-adsp; "qcom,force-stop" of qcom,lpass@3700000). */
#define OUR_ENTRY_STOP          "master-kernel"
#define DSP_STOP                (1u << 0)

static bool
dsp_words(struct smp2p *words)
{
	static uint32_t at;
	uint32_t size = 0;

	if (at == 0) {
		at = smem_item(ITEM_SMP2P_THEIRS, &size);
		if (at == 0 || size < sizeof(*words)) {
			at = 0;
			return false;
		}
	}
	return smem_peek(at, words, sizeof(*words)) && words->magic == SMP2P_MAGIC;
}

static int
smp2p_entry(const struct smp2p *words, const char *name)
{
	for (unsigned i = 0; i < words->valid && i < 16; i++) {
		if (strncmp(words->entry[i].name, name, sizeof(words->entry[i].name)) == 0) {
			return (int)i;
		}
	}
	return -1;
}

static uint32_t
dsp_state(const struct smp2p *words)
{
	int entry = smp2p_entry(words, "slave-kernel");

	return entry < 0 ? 0 : words->entry[entry].value;
}

static void
show_words(const char *prefix, const struct smp2p *words)
{
	char line[320];
	int at = 0;

	line[0] = 0;
	for (unsigned i = 0; i < words->valid && i < 16 && at < (int)sizeof(line) - 40; i++) {
		at += snprintf(line + at, sizeof(line) - (size_t)at, " %.16s = %#x;", words->entry[i].name, words->entry[i].value);
	}
	printf("adsp: %u: %s: SMP2P version %u, features %#x, from processor %u to %u, flags %#x, %u of %u entries:%s\n", step, prefix,
	    words->version & 0xff, words->version >> 8, words->local, words->remote, words->flags, words->valid, words->total, line);
}

static void
show_crash_reason(void)
{
	char text[81];
	uint32_t size = 0, at = smem_item(ITEM_CRASH_REASON, &size);

	if (at == 0 || size == 0) {
		printf("adsp: %u: item %u, the DSP's last words: not there\n", step, ITEM_CRASH_REASON);
		return;
	}
	bzero(text, sizeof(text));
	smem_peek(at, text, size < sizeof(text) - 1 ? size : sizeof(text) - 1);
	if (text[0] == 0) {
		printf("adsp: %u: item %u, the DSP's last words (%u bytes): none\n", step, ITEM_CRASH_REASON, size);
	} else {
		printf("adsp: %u: THE AUDIO DSP'S LAST WORDS (item %u, %u bytes): \"%s\"\n", step, ITEM_CRASH_REASON, size, text);
	}
}

/* ---- the doorbells, and the mailbox to the always-on processor (redwood_wifi.c, steps 2 and 7) ------ */

#define QMP_MAGIC               0x00
#define QMP_MAGIC_VALUE         0x4d41494c
#define QMP_VERSION             0x04
#define QMP_THEIR_LINK          0x0c
#define QMP_THEIR_LINK_ACK      0x10
#define QMP_THEIR_CHANNEL       0x14
#define QMP_THEIR_CHANNEL_ACK   0x18
#define QMP_OUR_LINK            0x24
#define QMP_OUR_LINK_ACK        0x28
#define QMP_OUR_CHANNEL         0x2c
#define QMP_OUR_CHANNEL_ACK     0x30
#define QMP_OUR_BOX_SIZE        0x34
#define QMP_OUR_BOX             0x38
#define QMP_UP                  0x0000ffffu
#define QMP_DOWN                0xffff0000u
#define QMP_MESSAGE             64

static struct block aoss = { "mailbox", 0x0c300000, 0x400 };

#define IPCC_SEND               0x0c
#define IPCC_RECEIVED           0x10
#define IPCC_ENABLE             0x14
#define IPCC_DISABLE            0x18
#define IPCC_CLEAR              0x1c
#define IPCC_NOBODY             0xffffffffu
#define IPCC_AOP                0u
#define IPCC_LPASS              3u              /* dt: mboxes of qcom,smp2p-adsp and qcom,glink/adsp */
#define IPCC_GLINK              0u
#define IPCC_SMP2P              2u

static struct block ipcc = { "ipcc", 0x00408000, 0x1000 };

static struct {
	uint32_t glink, smp2p, said;
	uint64_t since;
} doorbell;

/*
 * The audio DSP's rings, counted and cleared. Another's ring (the Wi-Fi DSP's, the always-on
 * processor's) is left for whoever listens for it: IPCC names the same one until it is cleared,
 * so the look ends there.
 */
static void
doorbells(void)
{
	for (unsigned i = 0; i < 8; i++) {
		uint32_t who = rd(ipcc.base + IPCC_RECEIVED);

		if (who == IPCC_NOBODY || who >> 16 != IPCC_LPASS) {
			return;
		}
		wr(ipcc.base + IPCC_CLEAR, who);
		doorbell.glink += (who & 0xffff) == IPCC_GLINK;
		doorbell.smp2p += (who & 0xffff) == IPCC_SMP2P;
		if (doorbell.said < 24) {
			uint64_t now = now_us() - doorbell.since;

			doorbell.said++;
			printf("adsp: doorbell: +%llu.%03llu ms: signal %u of the audio DSP; heard and cleared\n", now / 1000, now % 1000,
			    who & 0xffff);
		}
	}
}

static uint32_t our_box;                /* where in the message memory our mailbox is; 0: not open */

static bool
mailbox_wait(uint32_t offset, uint32_t value, uint32_t ms)
{
	for (uint32_t i = 0; i < ms * 10; i++) {
		if (rd(aoss.base + offset) == value) {
			return true;
		}
		sleep_us(100);
	}
	return rd(aoss.base + offset) == value;
}

static void
ring_aop(void)
{
	wr(ipcc.base + IPCC_SEND, IPCC_AOP << 16 | 0);
}

/* Linux's qmp_open(), as redwood_wifi.c does it; what is done already is found done. Under the mailbox's lock. */
static bool
mailbox_open(void)
{
	uint32_t box, size;

	need(&aoss);
	need(&ipcc);
	if (get(&aoss, "magic", QMP_MAGIC) != QMP_MAGIC_VALUE || get(&aoss, "version", QMP_VERSION) != 1) {
		printf("adsp: %u: not a mailbox this knows\n", step);
		return false;
	}
	box = get(&aoss, "our mailbox", QMP_OUR_BOX);
	size = get(&aoss, "our mailbox's size", QMP_OUR_BOX_SIZE);
	if (size < QMP_MESSAGE + 4 || box < 0x3c || box + size > aoss.size || (box & 3)) {
		printf("adsp: %u: our mailbox is not where a message fits\n", step);
		return false;
	}
	if (rd(aoss.base + QMP_OUR_LINK) == QMP_UP && rd(aoss.base + QMP_OUR_LINK_ACK) == QMP_UP &&
	    rd(aoss.base + QMP_OUR_CHANNEL) == QMP_UP && rd(aoss.base + QMP_THEIR_CHANNEL) == QMP_UP &&
	    rd(aoss.base + QMP_OUR_CHANNEL_ACK) == QMP_UP) {
		printf("adsp: %u: the mailbox is open already (the Wi-Fi's start, or a kernel before this one, opened it)\n", step);
		our_box = box;
		return true;
	}
	put(&aoss, "their link, seen by us", QMP_THEIR_LINK_ACK, rd(aoss.base + QMP_THEIR_LINK));
	put(&aoss, "our link", QMP_OUR_LINK, QMP_UP);
	ring_aop();
	if (!mailbox_wait(QMP_OUR_LINK_ACK, QMP_UP, 1000)) {
		printf("adsp: %u: THE ALWAYS-ON PROCESSOR DOES NOT ACKNOWLEDGE OUR LINK within a second; taken down again\n", step);
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring_aop();
		return false;
	}
	put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_UP);
	ring_aop();
	if (!mailbox_wait(QMP_THEIR_CHANNEL, QMP_UP, 1000)) {
		printf("adsp: %u: THE ALWAYS-ON PROCESSOR DOES NOT OPEN ITS CHANNEL within a second; ours is taken down again\n", step);
		put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_DOWN);
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring_aop();
		return false;
	}
	put(&aoss, "their channel, seen by us", QMP_THEIR_CHANNEL_ACK, QMP_UP);
	ring_aop();
	if (!mailbox_wait(QMP_OUR_CHANNEL_ACK, QMP_UP, 1000)) {
		printf("adsp: %u: THE ALWAYS-ON PROCESSOR DOES NOT ACKNOWLEDGE OUR CHANNEL within a second; taken down again\n", step);
		put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_DOWN);
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring_aop();
		return false;
	}
	printf("adsp: %u: THE MAILBOX TO THE ALWAYS-ON PROCESSOR IS OPEN\n", step);
	our_box = box;
	return true;
}

/* A message to the always-on processor (Linux's qmp_send()), under the lock the Wi-Fi's start takes too. */
static bool
mailbox_send(const char *text)
{
	uint32_t words[QMP_MESSAGE / 4] = { 0 };
	bool sent = false;

	if (!leaving) {
		lck_mtx_lock(&redwood_aop_mailbox_lock);
	}
	if (our_box == 0 && (leaving || !mailbox_open())) {
		goto out;
	}
	strlcpy((char *)words, text, sizeof(words));
	printf("adsp: %u: about to tell the always-on processor \"%s\"\n", step, text);
	snprintf(last_write, sizeof(last_write), "step %u sent the always-on processor a message", step);
	if (rd(aoss.base + our_box) != 0) {
		printf("adsp: %u: our mailbox still holds a message of %u bytes: not sent\n", step, rd(aoss.base + our_box));
		goto out;
	}
	for (unsigned i = 0; i < QMP_MESSAGE / 4; i++) {
		wr(aoss.base + our_box + 4 + 4 * i, words[i]);
	}
	wr(aoss.base + our_box, QMP_MESSAGE);
	(void)rd(aoss.base + our_box);
	ring_aop();
	if (!mailbox_wait(our_box, 0, 1000)) {
		printf("adsp: %u: THE ALWAYS-ON PROCESSOR DOES NOT TAKE THE MESSAGE within a second; it is taken back\n", step);
		wr(aoss.base + our_box, 0);
		goto out;
	}
	printf("adsp: %u: the always-on processor has taken it\n", step);
	sent = true;
out:
	if (!leaving) {
		lck_mtx_unlock(&redwood_aop_mailbox_lock);
	}
	return sent;
}

/* ---- RPMh: the DSP's two rails, asked for while it boots ------------------------------------------------ */

/*
 * The audio DSP's logic and memories run from LPASS's own rails, LCX and LMX (dt: vdd_cx and
 * vdd_mx of qcom,lpass@3700000 are PM7325 L5 and L4 "level" regulators, RPMh resources lcx.lvl and
 * lmx.lvl), which the boot loader leaves at retention (16). Linux and Android ask for level 384
 * while the DSP boots, until it says it has asked for its own (bit 2, "proxy-unvote"); without it
 * the DSP rang once and left "SFR Init: wdog or kernel error suspected." in its last words.
 *
 * A request goes to RPMh through the application processor's sequencer (dt: rsc@18200000, DRV 2,
 * qcom,tcs-offset 0xd00, qcom,tcs-config: TCSs 0 and 1 are for requests made at once), as Linux's
 * rpmh-rsc.c writes one: the command's message id, address and value into a TCS that is idle, the
 * command enabled and marked to be waited for, the TCS put in "active" mode and triggered, and its
 * bit in the status of completions looked for. A rail asked for by level is asked by position in
 * the list of its levels that the boot loader's command database keeps (cmd-db.c), the first
 * position at or above the level wanted.
 */
#define CMD_DB_PHYS             0x80860000UL
#define CMD_DB_SIZE             0x20000UL

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

static const struct cmd_db_header *cmd_db;

#define RSC_TCS_BASE            0xd00
#define RSC_IRQ_STATUS          (RSC_TCS_BASE + 0x04)   /* a bit per TCS: done */
#define RSC_IRQ_CLEAR           (RSC_TCS_BASE + 0x08)
#define RSC_TCS(n)              (RSC_TCS_BASE + 672 * (n))
#define TCS_WAIT_FOR_DONE       0x10
#define TCS_CONTROL             0x14
#define TCS_STATUS              0x18                    /* not zero: idle */
#define TCS_ENABLED             0x1c
#define TCS_COMMAND(c)          (0x30 + 20 * (c))       /* message id, address, value, status */
#define TCS_ACTIVE_MODE         (1u << 16)
#define TCS_TRIGGER             (1u << 24)
#define MSGID_WRITE_WAITED      (8u | 1u << 8 | 1u << 16)       /* four bytes, an answer wanted, a write */
#define RPMH_TCS                0                       /* the first of the two for requests made at once */

/* The level position RPMh takes for that rail and level; -1: not found. */
static int
rail_position(const char *name, uint32_t level, uint32_t *address)
{
	char id[8] = { 0 };

	if (cmd_db == NULL) {
		cmd_db = (const struct cmd_db_header *)ml_io_map_unmappable(CMD_DB_PHYS, CMD_DB_SIZE, VM_WIMG_WCOMB);
	}
	if (!(cmd_db->magic[0] == 0xdb && cmd_db->magic[1] == 0x30 && cmd_db->magic[2] == 0x03 && cmd_db->magic[3] == 0x0c)) {
		printf("adsp: %u: the boot loader's command database is not there\n", step);
		return -1;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &cmd_db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(cmd_db->data + slave->header_offset);

		if (slave->id == 0 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			const uint8_t *data;
			char levels[160];
			int at;

			if (memcmp(entry[i].id, id, sizeof(id)) != 0) {
				continue;
			}
			if ((unsigned)slave->data_offset + entry[i].offset + entry[i].length > CMD_DB_SIZE - 0x1000) {
				return -1;
			}
			data = cmd_db->data + slave->data_offset + entry[i].offset;
			at = snprintf(levels, sizeof(levels), "%s at %#x, levels:", name, entry[i].address);
			for (unsigned l = 0; l < entry[i].length / 2u && l < 16; l++) {
				unsigned value = data[2 * l] | data[2 * l + 1] << 8;

				if (l > 0 && value == 0) {
					break;
				}
				at += snprintf(levels + at, sizeof(levels) - (size_t)at, " %u", value);
			}
			printf("adsp: %u: RPMh has %s\n", step, levels);
			for (unsigned l = 0; l < entry[i].length / 2u && l < 16; l++) {
				if ((uint32_t)(data[2 * l] | data[2 * l + 1] << 8) >= level) {
					*address = entry[i].address;
					return (int)l;
				}
			}
			return -1;
		}
	}
	printf("adsp: %u: RPMh has no \"%s\"\n", step, name);
	return -1;
}

/* The two rails at that level (Linux's proxy request), or given back (level 0). True: RPMh has said done. */
static bool
rails(uint32_t level)
{
	static const char *const names[2] = { "lcx.lvl", "lmx.lvl" };
	uint32_t addresses[2] = { 0, 0 }, values[2] = { 0, 0 }, done = 0;
	uint64_t began;

	for (unsigned r = 0; r < 2; r++) {
		int position = level == 0 ? 0 : rail_position(names[r], level, &addresses[r]);

		if (level == 0) {
			rail_position(names[r], 0, &addresses[r]);
		}
		if (position < 0 || addresses[r] == 0) {
			printf("adsp: %u: no position for %s at level %u: the rails are not asked\n", step, names[r], level);
			return false;
		}
		values[r] = (uint32_t)position;
	}
	(void)began;
	printf("adsp: %u: about to ask RPMh (bsd/dev/sweet_rpmh.c) for lcx.lvl (%#x) = %u and lmx.lvl (%#x) = %u (level %u%s)\n",
	    step, addresses[0], values[0], addresses[1], values[1], level, level == 0 ? ": given back" : "");
	snprintf(last_write, sizeof(last_write), "step %u asked RPMh for the audio DSP's rails at level %u", step, level);
	hold();
	{
		const uint32_t offset = 0;
		int e0 = EBUSY, e1 = EBUSY;

		for (unsigned k = 0; k < 50 && e0 == EBUSY; k++) {     /* (another vote may be on its way: sweet_rpmh.c's lock) */
			e0 = sweet_rpmh_write(names[0], 1, &offset, &values[0]);
			if (e0 == EBUSY) {
				sleep_ms(2);
			}
		}
		for (unsigned k = 0; k < 50 && e1 == EBUSY; k++) {
			e1 = sweet_rpmh_write(names[1], 1, &offset, &values[1]);
			if (e1 == EBUSY) {
				sleep_ms(2);
			}
		}

		done = e0 == 0 && e1 == 0;
		printf("adsp: %u: RPMh %s (%d, %d)\n", step, done ? "HAS DONE IT" : "HAS NOT SAID DONE", e0, e1);
	}
	return done != 0;
}

/* ---- step 1: the shared memory --------------------------------------------------------------------- */

static bool
step_survey(void)
{
	struct smp2p words;
	uint32_t size = 0;

	announce("read our partition with the audio DSP in the shared memory (0x80900000), and its items");
	show_partition();
	if (dsp_words(&words)) {
		show_words("the DSP's item (443) is there: it has run since the phone was started", &words);
	} else {
		printf("adsp: %u: the DSP's SMP2P item (443) is not there: it has not run since the phone was started\n", step);
	}
	printf("adsp: %u: our SMP2P item (429): %s; GLINK's descriptor (478): %s, our queue (479): %s, the DSP's (480): %s\n", step,
	    smem_item(ITEM_SMP2P_OURS, &size) ? "there" : "not there", smem_item(478, &size) ? "there" : "NOT THERE",
	    smem_item(479, &size) ? "there" : "NOT THERE", smem_item(480, &size) ? "there" : "not there (the DSP makes it)");
	show_crash_reason();
	need(&ipcc);
	printf("adsp: %u: the doorbells say %#x\n", step, rd(ipcc.base + IPCC_RECEIVED));
	return smem_partition(SMEM_US, SMEM_ADSP, &size, &size) != 0;
}

/* ---- step 2: the firmware -------------------------------------------------------------------------- */

#define ADSP_MEMORY             0x89200000u     /* redfin dt: reserved-memory pil_adsp_region@89200000, memory-region of qcom,lpass@3000000 */
#define ADSP_MEMORY_SIZE        0x02800000u
#define PAS_ADSP                1               /* dt: qcom,pas-id */


/*
 * A file of /fw on the root file system, read whole into memory of ours: the system image, where
 * userland/build.sh puts fw-audio/ when it makes that image. It is compressed there, which the file
 * system undoes and the reading of the small image's memory below does not; and it is there only
 * once the root is mounted, so only adsp=1, which waits for that on its own thread, finds it so.
 */
static const uint8_t *
root_file(const char *name, uint64_t *size)
{
	char path[64];
	vfs_context_t context = vfs_context_kernel();
	vnode_t vnode = NULLVP;
	struct vnode_attr attributes;
	uint8_t *data;
	int left = 0, error;

	if (rootvnode == NULLVP) {
		return NULL;
	}
	snprintf(path, sizeof(path), "/private/var/local/fw/%s", name);
	if (vnode_lookup(path, 0, &vnode, context) != 0) {
		return NULL;
	}
	VATTR_INIT(&attributes);
	VATTR_WANTED(&attributes, va_data_size);
	if (vnode_getattr(vnode, &attributes, context) != 0 || attributes.va_data_size == 0 ||
	    attributes.va_data_size > ADSP_MEMORY_SIZE) {
		vnode_put(vnode);
		return NULL;
	}
	data = kalloc_data(attributes.va_data_size, Z_WAITOK);
	if (data == NULL) {
		vnode_put(vnode);
		return NULL;
	}
	error = vn_rdwr(UIO_READ, vnode, (caddr_t)data, (int)attributes.va_data_size, 0, UIO_SYSSPACE, 0,
	    vfs_context_ucred(context), &left, vfs_context_proc(context));
	vnode_put(vnode);
	if (error != 0 || left != 0) {
		printf("adsp: %u: %s: read error %d, %d bytes left\n", step, path, error, left);
		kfree_data(data, attributes.va_data_size);
		return NULL;
	}
	*size = attributes.va_data_size;
	return data;
}

static bool from_root;                  /* the image is in memory of ours (root_file()), to be given back once written */

/* For the sound (redwood_sound.c): a file of /fw on the root, read whole into memory of ours, and given back. */
const uint8_t *
redwood_adsp_fw_file(const char *name, uint64_t *size)
{
	return root_file(name, size);
}

void
redwood_adsp_fw_free(const uint8_t *data, uint64_t size)
{
	uint8_t *ours = __DECONST(uint8_t *, data);

	kfree_data(ours, size);
}

/* A file of /private/var/local/fw on the root (Android's /vendor/firmware, tools/redfin-install-adsp.sh). */
static const uint8_t *
image_file(const char *name, uint64_t *size)
{
	const uint8_t *data = root_file(name, size);

	if (data != NULL) {
		from_root = true;
		return data;
	}
	printf("adsp: %u: no /private/var/local/fw/%s: copy Android's /vendor/firmware/%s there (tools/redfin-install-adsp.sh)\n",
	    step, name, name);
	return NULL;
}


#define MDT_HASH(flags)         (((flags) >> 24 & 7) == 2)
#define MAX_SEGMENTS            48

struct elf_segment {
	uint32_t type, offset, vaddr, paddr, file_size, memory_size, flags, align;
};

static struct {
	const uint8_t *header;
	uint64_t header_size;
	struct {
		uint32_t at;
		uint32_t file_size, memory_size;
		const uint8_t *file;
	} segment[MAX_SEGMENTS];
	unsigned segments;
	uint32_t size;
} image;

/* The image's files given back, if they were read into memory of ours. */
static void
image_forget(void)
{
	if (!from_root) {
		return;
	}
	if (image.header != NULL) {
		void *data = (void *)(uintptr_t)image.header;

		kfree_data(data, image.header_size);
	}
	for (unsigned s = 0; s < image.segments; s++) {
		if (image.segment[s].file != NULL) {
			void *data = (void *)(uintptr_t)image.segment[s].file;

			kfree_data(data, image.segment[s].file_size);
		}
	}
	bzero(&image, sizeof(image));
	from_root = false;
}

static bool
step_firmware(void)
{
	struct elf_segment segment;
	const uint8_t *header;
	uint64_t size = 0, files = 0;
	uint32_t count, hashes = 0, lowest = ~0u, highest = 0;

	announce("find the audio DSP's image in the small image or on the root, and check it against its own header");
	image_forget();
	image.segments = 0;
	header = image_file("adsp.mdt", &size);
	if (header == NULL) {
		return false;
	}
	image.header = header;
	image.header_size = size;
	count = size >= 52 + 2 * 32 ? header[44] | header[45] << 8 : 0;
	if (memcmp(header, "\177ELF\1", 5) != 0 || count < 3 || count > MAX_SEGMENTS || 52 + 32 * count > size || size > PAGE_SIZE) {
		printf("adsp: %u: fw/adsp.mdt is not such a header (%02x %02x %02x %02x, %u segments, %llu bytes)\n", step, header[0],
		    header[1], header[2], header[3], count, size);
		goto fail;
	}
	for (uint32_t i = 0; i < count; i++) {
		char name[16];

		memcpy(&segment, header + 52 + 32 * i, sizeof(segment));
		if (MDT_HASH(segment.flags)) {
			hashes = segment.file_size;
		}
		if (segment.type != 1 || MDT_HASH(segment.flags) || segment.memory_size == 0) {
			continue;
		}
		if (segment.paddr < ADSP_MEMORY || segment.file_size > segment.memory_size ||
		    (uint64_t)segment.paddr + segment.memory_size > (uint64_t)ADSP_MEMORY + ADSP_MEMORY_SIZE) {
			printf("adsp: %u: segment %u (%#x, %#x bytes) is not within the DSP's memory\n", step, i, segment.paddr,
			    segment.memory_size);
			goto fail;
		}
		image.segment[image.segments].at = segment.paddr - ADSP_MEMORY;
		image.segment[image.segments].file_size = segment.file_size;
		image.segment[image.segments].memory_size = segment.memory_size;
		image.segment[image.segments].file = NULL;
		if (segment.file_size != 0) {
			uint64_t file_size = 0;

			snprintf(name, sizeof(name), "adsp.b%02u", i);
			image.segment[image.segments].file = image_file(name, &file_size);
			if (image.segment[image.segments].file == NULL) {
				goto fail;
			}
			if (file_size != segment.file_size) {
				printf("adsp: %u: fw/%s has %llu bytes, the header says %u\n", step, name, file_size, segment.file_size);
				image.segments++;               /* (for its file to be given back) */
				goto fail;
			}
			files += file_size;
		}
		lowest = segment.paddr < lowest ? segment.paddr : lowest;
		highest = segment.paddr + segment.memory_size > highest ? segment.paddr + segment.memory_size : highest;
		image.segments++;
	}
	if (image.segments == 0 || lowest != ADSP_MEMORY || 52 + 32 * count + (uint64_t)hashes != size) {
		printf("adsp: %u: not what TrustZone takes: %u segments to load, the lowest at %#x; the header file has %llu bytes, "
		    "the header %u and the hashes %u\n", step, image.segments, lowest, size, 52 + 32 * count, hashes);
		goto fail;
	}
	image.header = header;
	image.header_size = size;
	image.size = ((highest + 0xfff) & ~0xfffu) - ADSP_MEMORY;
	printf("adsp: %u: the image is whole (%s): a header of %llu bytes (%u of hashes), %u segments, %llu bytes of files, %#x "
	    "bytes of the DSP's %#x from %#x\n", step, from_root ? "from /fw on the root" : "from the small image", size, hashes,
	    image.segments, files, image.size, ADSP_MEMORY_SIZE, ADSP_MEMORY);
	return true;
fail:
	image_forget();
	image.segments = 0;
	return false;
}

/* ---- step 3: the mailbox --------------------------------------------------------------------------- */

static bool
step_mailbox(void)
{
	bool open;

	announce("open the mailbox to the always-on processor, unless it is open: words of its message memory, and its doorbell");
	lck_mtx_lock(&redwood_aop_mailbox_lock);
	open = mailbox_open();
	lck_mtx_unlock(&redwood_aop_mailbox_lock);
	return open;
}

/* ---- TrustZone, and stopping the DSP --------------------------------------------------------------- */

#define SCM_64                  (1ull << 30)
#define SCM_SIP                 (2ull << 24)
#define SCM_CALL(service, command) ((uint64_t)(service) << 8 | (command))
#define SCM_PAS_INIT_IMAGE      SCM_CALL(2, 1)
#define SCM_PAS_MEM_SETUP       SCM_CALL(2, 2)
#define SCM_PAS_AUTH_AND_RESET  SCM_CALL(2, 5)
#define SCM_PAS_SHUTDOWN        SCM_CALL(2, 6)
#define SCM_ONE_VALUE           1
#define SCM_THREE_VALUES        3
#define SCM_VALUE_AND_BUFFER    (2 | 2 << 6)
#define SCM_INTERRUPTED         1

static LCK_MTX_DECLARE(dsp_lock, &adsp_lock_group);     /* the DSP being started or stopped */
static bool rails_asked;                /* our request for its rails stands (until it asks for its own, or is shut down) */
static bool dsp_started;
static bool stopped_by_its_word;
static uint32_t our_item;
static bool smp2p_agreed, smp2p_restart_acks, smp2p_restart_acked;

static int32_t
scm(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third, uint64_t *answer)
{
	uint64_t session = 0;

	function |= SCM_64 | SCM_SIP;
	printf("adsp: %u: about to ask TrustZone %s: smc %#llx, arguments %#llx, %#llx, %#llx\n", step, what, function, first,
	    second, third);
	snprintf(last_write, sizeof(last_write), "step %u asked TrustZone %#llx(%#llx, %#llx)", step, function, first, second);
	for (int i = 0; i < 100; i++) {
		register uint64_t x0 __asm__("x0") = i == 0 ? function : SCM_INTERRUPTED;
		register uint64_t x1 __asm__("x1") = arguments;
		register uint64_t x2 __asm__("x2") = first;
		register uint64_t x3 __asm__("x3") = second;
		register uint64_t x4 __asm__("x4") = third;
		register uint64_t x5 __asm__("x5") = 0;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != SCM_INTERRUPTED) {
			printf("adsp: %u: TrustZone returns %d, answer %#llx\n", step, (int32_t)x0, x1);
			*answer = x1;
			return (int32_t)x0;
		}
	}
	printf("adsp: %u: TrustZone keeps saying the call was interrupted\n", step);
	return -1;
}

static uint32_t
our_item_find(void)
{
	uint32_t size = 0, at = smem_item(ITEM_SMP2P_OURS, &size);

	return at != 0 && size >= sizeof(struct smp2p) ? at : 0;
}

/* What Linux does when the DSP rings for SMP2P: agree on the features once, and answer a restart. */
static void
dsp_listen(const struct smp2p *theirs)
{
	struct smp2p ours;

	if (our_item == 0 || !smem_peek(our_item, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC) {
		return;
	}
	if (!smp2p_agreed) {
		uint32_t features = (ours.version >> 8) & (theirs->version >> 8);

		if ((theirs->version & 0xff) != (ours.version & 0xff)) {
			return;
		}
		if (features != ours.version >> 8) {
			smem_poke(our_item + SMP2P_AT_VERSION, features << 8 | (ours.version & 0xff));
		}
		smp2p_restart_acks = features & SMP2P_RESTART_ACKS;
		smp2p_agreed = true;
		printf("adsp: %u: the DSP and we agree on SMP2P: version %u, features %#x\n", step, ours.version & 0xff, features);
	}
	if (smp2p_restart_acks && ((theirs->flags & SMP2P_RESTART_DONE) != 0) != smp2p_restart_acked) {
		smp2p_restart_acked = !smp2p_restart_acked;
		printf("adsp: %u: the DSP's flags say it has restarted (%#x): answered in our flags (bit 1 = %u), and its SMP2P doorbell\n",
		    step, theirs->flags, smp2p_restart_acked);
		smem_poke(our_item + SMP2P_AT_FLAGS, (ours.flags & ~SMP2P_RESTART_ACK) | (smp2p_restart_acked ? SMP2P_RESTART_ACK : 0));
		settle();
		wr(ipcc.base + IPCC_SEND, IPCC_LPASS << 16 | IPCC_SMP2P);
	}
}

/* Linux's qcom_q6v5_request_stop(): our bit "stop", the DSP's "stop ack" waited for, our bit taken back. */
static bool
dsp_ask_to_stop(uint32_t bound_ms)
{
	struct smp2p ours, theirs;
	uint32_t at = our_item_find(), state, value_at;
	uint64_t asked, now;
	int entry = -1;

	if (at == 0 || !smem_peek(at, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC || (ours.version & 0xff) != SMP2P_VERSION ||
	    (entry = smp2p_entry(&ours, OUR_ENTRY_STOP)) < 0) {
		printf("adsp: %u: the DSP is not asked to stop first: there is no SMP2P item of ours to ask it with\n", step);
		return false;
	}
	if (!dsp_words(&theirs) || smp2p_entry(&theirs, "slave-kernel") < 0) {
		printf("adsp: %u: the DSP is not asked to stop first: its item has no entry to answer in\n", step);
		return false;
	}
	state = dsp_state(&theirs);
	if (state & DSP_STOP_ACK) {
		printf("adsp: %u: the DSP is not asked to stop: its entry says %#x, that it has stopped when asked before\n", step, state);
		return true;
	}
	if (!(state & DSP_READY) || (state & DSP_FATAL)) {
		printf("adsp: %u: the DSP is not asked to stop first: its entry says %#x, not \"up\"\n", step, state);
		return false;
	}
	need(&ipcc);
	value_at = at + SMP2P_AT_VALUE((uint32_t)entry);
	printf("adsp: %u: about to ask the DSP to stop: bit 0 of our entry \"%s\" at %#lx, and its SMP2P doorbell\n", step,
	    OUR_ENTRY_STOP, SMEM_PHYS + value_at);
	snprintf(last_write, sizeof(last_write), "step %u asked the audio DSP to stop", step);
	smem_poke(value_at, ours.entry[entry].value | DSP_STOP);
	settle();
	wr(ipcc.base + IPCC_SEND, IPCC_LPASS << 16 | IPCC_SMP2P);
	asked = now = now_us();
	while (now - asked < bound_ms * 1000ull) {
		if (dsp_words(&theirs) && (dsp_state(&theirs) & (DSP_STOP_ACK | DSP_FATAL))) {
			break;
		}
		sleep_us(200);
		now = now_us();
	}
	state = dsp_words(&theirs) ? dsp_state(&theirs) : 0;
	printf("adsp: %u: %s after %llu.%03llu ms; its entry says %#x\n", step, state & DSP_STOP_ACK ?
	    "THE AUDIO DSP ACKNOWLEDGES THE STOP" : "THE AUDIO DSP DOES NOT ACKNOWLEDGE THE STOP", (now - asked) / 1000,
	    (now - asked) % 1000, state);
	smem_poke(value_at, ours.entry[entry].value & ~DSP_STOP);
	settle();
	wr(ipcc.base + IPCC_SEND, IPCC_LPASS << 16 | IPCC_SMP2P);
	return (state & DSP_STOP_ACK) != 0;
}

static void link_stop(void);

/* Asked to stop, then shut down by TrustZone, then the always-on processor told that the image is gone. */
static bool
dsp_shutdown(const char *why)
{
	uint64_t answer = ~0ull;
	int32_t result;

	printf("adsp: %u: %s: the audio DSP is asked to stop, then TrustZone to shut it down\n", step, why);
	if (!leaving) {
		link_stop();
	}
	stopped_by_its_word = dsp_ask_to_stop(leaving ? 2000 : 5000);
	result = scm("to shut the audio DSP down", SCM_PAS_SHUTDOWN, SCM_ONE_VALUE, PAS_ADSP, 0, 0, &answer);
	if (result != 0 || answer != 0) {
		printf("adsp: %u: TRUSTZONE DOES NOT SHUT THE AUDIO DSP DOWN: the call returns %d, its answer is %#llx\n", step, result,
		    answer);
		return false;
	}
	printf("adsp: %u: THE AUDIO DSP IS SHUT DOWN\n", step);
	need(&ipcc);
	wr(ipcc.base + IPCC_DISABLE, IPCC_LPASS << 16 | IPCC_SMP2P);
	doorbells();
	dsp_started = false;
	mailbox_send("{class: image, res: load_state, name: adsp, val: off}");
	if (rails_asked) {
		rails(0);
		rails_asked = false;
	}
	return true;
}

/* ---- step 4: TrustZone takes the header ------------------------------------------------------------ */

static bool
step_header(void)
{
	struct smp2p words;
	vm_offset_t page = 0;
	uint64_t answer = ~0ull;
	int32_t result;

	if (image.segments == 0) {
		return false;
	}
	if (dsp_words(&words)) {
		show_words("the DSP has left words in the shared memory", &words);
		announce("ask TrustZone to shut down an audio DSP that a kernel before this one may have left running");
		if (!dsp_shutdown("it may be running")) {
			return false;
		}
		if (!stopped_by_its_word && !option("adsp_again")) {
			printf("adsp: %u: THE AUDIO DSP IS NOT STARTED AGAIN: it was shut down without having acknowledged a stop, and "
			    "with the Wi-Fi's DSP a start after that reset the phone (adsp_again=1 to try all the same)\n", step);
			return false;
		}
	} else {
		printf("adsp: %u: the DSP has left nothing in the shared memory: it has not run since the phone was started\n", step);
	}

	announce("hand the image's header to TrustZone to check its signature (image 1), in a page of ours");
	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("adsp: %u: no memory\n", step);
		return false;
	}
	memcpy((void *)page, image.header, image.header_size);
	for (vm_offset_t line = page; line < page + PAGE_SIZE; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	settle();
	result = scm("to take the audio DSP's header", SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER, PAS_ADSP, kvtophys(page), 0, &answer);
	if (result != 0 || answer != 0) {
		printf("adsp: %u: TRUSTZONE DOES NOT TAKE THE HEADER: the call returns %d, its answer is %#llx\n", step, result, answer);
		return false;
	}
	printf("adsp: %u: TRUSTZONE ACCEPTS THE AUDIO DSP'S SIGNATURE FROM THIS KERNEL\n", step);

	announce("tell TrustZone where the image will be");
	result = scm("where the image will be", SCM_PAS_MEM_SETUP, SCM_THREE_VALUES, PAS_ADSP, ADSP_MEMORY, image.size, &answer);
	if (result != 0 || answer != 0) {
		printf("adsp: %u: TRUSTZONE DOES NOT TAKE THE PLACE: the call returns %d, its answer is %#llx\n", step, result, answer);
		return false;
	}
	printf("adsp: %u: TrustZone takes the place: %#x bytes at %#x\n", step, image.size, ADSP_MEMORY);
	return true;
}

/* ---- step 5: the image, in the DSP's memory --------------------------------------------------------- */

static bool image_written;

static uint32_t
segment_word(unsigned s, uint32_t at)
{
	uint32_t word = 0;

	for (unsigned b = 0; b < 4 && at + b < image.segment[s].file_size; b++) {
		word |= (uint32_t)image.segment[s].file[at + b] << (8 * b);
	}
	return word;
}

/*
 * redfin: in four-byte words (HackiOS wrote eight): lito's adsp image has segments that begin or end
 * at a multiple of four only (e.g. 0x89bb5000, 0x94 bytes, then 0x89bb5094).
 */
static bool
step_image(void)
{
	volatile uint32_t *dsp_memory;
	uint64_t written = 0, cleared = 0, wrong = 0;

	if (image.segments == 0) {
		return false;
	}
	announce("put the image's segments into the audio DSP's memory (0x89200000, 40 MB: memory of nobody's until TrustZone "
	    "starts the DSP), with zeroes where a segment is longer than its file, and read them back");
	dsp_memory = (volatile uint32_t *)ml_io_map_unmappable(ADSP_MEMORY, ADSP_MEMORY_SIZE, VM_WIMG_WCOMB);
	snprintf(last_write, sizeof(last_write), "step %u wrote the image at %#x", step, ADSP_MEMORY);
	for (unsigned s = 0; s < image.segments; s++) {
		volatile uint32_t *to = dsp_memory + image.segment[s].at / 4;

		if ((image.segment[s].at | image.segment[s].memory_size) & 3) {
			printf("adsp: %u: a segment that does not begin and end at a multiple of four bytes: not written\n", step);
			ml_io_unmap((vm_offset_t)dsp_memory, ADSP_MEMORY_SIZE);
			return false;
		}
		for (uint32_t i = 0; i < image.segment[s].memory_size; i += 4) {
			to[i / 4] = segment_word(s, i);
		}
		written += image.segment[s].file_size;
		cleared += image.segment[s].memory_size - image.segment[s].file_size;
	}
	settle();
	for (unsigned s = 0; s < image.segments; s++) {
		volatile uint32_t *from = dsp_memory + image.segment[s].at / 4;

		for (uint32_t i = 0; i < image.segment[s].memory_size; i += 4) {
			wrong += from[i / 4] != segment_word(s, i);
		}
	}
	printf("adsp: %u: %u segments, %llu bytes written and %llu cleared; read back, %llu words of four bytes differ\n", step,
	    image.segments, written, cleared, wrong);
	settle();
	ml_io_unmap((vm_offset_t)dsp_memory, ADSP_MEMORY_SIZE);
	image_written = wrong == 0;
	image_forget();                         /* (it is in the DSP's memory now: what was read of the root, given back) */
	return image_written;
}

/* ---- step 6: our SMP2P item ------------------------------------------------------------------------ */

static bool glink_items_make(void);
#define ITEM_GLINK_DESCRIPTOR   478
#define ITEM_GLINK_OUR_QUEUE    479
#define ITEM_GLINK_THEIR_QUEUE  480
static struct block smem_lock = { "shared memory's lock", 0x01f43000, 0x1000 };
#define LOCK_OWNER              0x000
#define LOCK_US                 1u

/* Our item made in the partition, as Linux's qcom_smem_alloc_private() makes one (redwood_wifi.c: our_item_make()). */
static bool
our_item_alloc(uint32_t partition, uint32_t item, uint32_t wanted)
{
	struct smem_partition header;
	struct smem_entry entry;
	const uint32_t item_size = (wanted + 7) & ~7u, need_bytes = sizeof(entry) + item_size;
	uint32_t owner = ~0u, at;
	bool made = false;

	lck_mtx_lock(&redwood_smem_alloc_lock);         /* (the Wi-Fi's start allocates under the same hardware lock) */
	need(&smem_lock);
	printf("adsp: %u: about to take the lock (%#x), write an entry for item %u with %u bytes behind it at the partition's mark, "
	    "move the mark by %u, and release the lock\n", step, smem_lock.phys + LOCK_OWNER, item, item_size, need_bytes);
	snprintf(last_write, sizeof(last_write), "step %u took the shared memory's lock", step);
	for (uint32_t ms = 0; ms <= 1000; ms++) {
		wr(smem_lock.base + LOCK_OWNER, LOCK_US);
		owner = rd(smem_lock.base + LOCK_OWNER);
		if (owner == LOCK_US) {
			break;
		}
		sleep_ms(1);
	}
	if (owner != LOCK_US) {
		printf("adsp: %u: THE LOCK IS HELD BY PROCESSOR %u AND STAYS SO FOR A SECOND: nothing is written\n", step, owner);
		lck_mtx_unlock(&redwood_smem_alloc_lock);
		return false;
	}
	if (!smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.free_cached ||
	    header.free_cached > header.size) {
		printf("adsp: %u: the partition's marks make no sense: nothing is written\n", step);
		goto out;
	}
	for (at = sizeof(header); at < header.free_uncached;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.item == item ||
		    entry.size > header.size) {
			break;
		}
		at += sizeof(entry) + entry.padding_header + entry.size;
	}
	if (at != header.free_uncached) {
		printf("adsp: %u: the partition's entries do not end at its mark (+%#x, the mark is +%#x), or the item has appeared: "
		    "nothing is written\n", step, at, header.free_uncached);
		goto out;
	}
	if (at + need_bytes > header.free_cached) {
		printf("adsp: %u: no room: %u bytes wanted, %u free: nothing is written\n", step, need_bytes, header.free_cached - at);
		goto out;
	}
	smem_poke(partition + at, SMEM_CANARY | item << 16);
	smem_poke(partition + at + 4, item_size);
	smem_poke(partition + at + 8, item_size - wanted);
	smem_poke(partition + at + 12, 0);
	settle();
	smem_poke(partition + __offsetof(struct smem_partition, free_uncached), at + need_bytes);
	settle();
	made = true;
out:
	wr(smem_lock.base + LOCK_OWNER, 0);
	printf("adsp: %u: the lock is released (it reads %u)\n", step, rd(smem_lock.base + LOCK_OWNER));
	lck_mtx_unlock(&redwood_smem_alloc_lock);
	return made;
}

static bool
our_item_make(uint32_t partition)
{
	return our_item_alloc(partition, ITEM_SMP2P_OURS, sizeof(struct smp2p));
}

/*
 * redfin: as Linux's qcom_glink_smem_register() at the DSP's start, our side makes GLINK's descriptor
 * (478, 32 bytes) and our queue (479, FIFO_0, 16 KB) if they are not there; the DSP makes its own (480).
 * On lito nothing before us has (r14b: the DSP made 478 and 480, and step 8 found no 479).
 */
static bool
glink_items_make(void)
{
	uint32_t size = 0, partition = smem_partition(SMEM_US, SMEM_ADSP, &size, &size);

	if (partition == 0) {
		return false;
	}
	if (smem_item(ITEM_GLINK_DESCRIPTOR, &size) == 0 && !our_item_alloc(partition, ITEM_GLINK_DESCRIPTOR, 32)) {
		return false;
	}
	if (smem_item(ITEM_GLINK_OUR_QUEUE, &size) == 0) {
		if (!our_item_alloc(partition, ITEM_GLINK_OUR_QUEUE, 16384)) {
			return false;
		}
		printf("adsp: %u: GLINK's descriptor (478) and our queue (479, 16 KB) are there\n", step);
	}
	return true;
}

static void glink_reset(void);

static bool
step_our_item(void)
{
	struct smem_partition header;
	struct smp2p ours;
	uint32_t cacheline = 0, size = 0, partition, name[4] = { 0 };

	announce("look for an SMP2P item of ours (429) in our partition with the audio DSP, make it if there is none, and fill it in");
	partition = smem_partition(SMEM_US, SMEM_ADSP, &cacheline, &size);
	if (partition == 0 || !smem_peek(partition, &header, sizeof(header))) {
		printf("adsp: %u: there is no such partition\n", step);
		return false;
	}
	our_item = our_item_find();
	if (our_item != 0) {
		printf("adsp: %u: item %u is there, at %#lx: a kernel before this one made it, and it is used again\n", step,
		    ITEM_SMP2P_OURS, SMEM_PHYS + our_item);
	} else {
		if (!our_item_make(partition)) {
			return false;
		}
		our_item = our_item_find();
		if (our_item == 0) {
			printf("adsp: %u: THE ITEM IS NOT FOUND where it was just made\n", step);
			return false;
		}
		printf("adsp: %u: item %u made, at %#lx\n", step, ITEM_SMP2P_OURS, SMEM_PHYS + our_item);
	}
	/* zeroes; from processor 0 to 2, room for 16 entries, "master-kernel" at zero, feature bit 0; last the version */
	snprintf(last_write, sizeof(last_write), "step %u filled in our SMP2P item", step);
	for (uint32_t at = 0; at < sizeof(ours); at += 4) {
		smem_poke(our_item + at, 0);
	}
	settle();
	smem_poke(our_item, SMP2P_MAGIC);
	smem_poke(our_item + SMP2P_AT_PROCESSORS, SMEM_US | SMEM_ADSP << 16);
	strlcpy((char *)name, OUR_ENTRY_STOP, sizeof(name));
	for (uint32_t word = 0; word < 4; word++) {
		smem_poke(our_item + SMP2P_AT_NAME(0) + 4 * word, name[word]);
	}
	uint32_t entries = 1;
	if (adsp_sleep) {
		static const struct {
			const char *name;
			uint32_t value;
		} more[] = { { "sleepstate", 1 }, { "rdbg", 0 } };

		for (unsigned e = 0; e < sizeof(more) / sizeof(more[0]); e++, entries++) {
			bzero(name, sizeof(name));
			strlcpy((char *)name, more[e].name, sizeof(name));
			for (uint32_t word = 0; word < 4; word++) {
				smem_poke(our_item + SMP2P_AT_NAME(entries) + 4 * word, name[word]);
			}
			smem_poke(our_item + SMP2P_AT_VALUE(entries), more[e].value);
		}
		printf("adsp: %u: adsp_sleep=1: our item also has \"sleepstate\" = 1 (APPS awake) and \"rdbg\" = 0, as Android's\n", step);
	}
	smem_poke(our_item + SMP2P_AT_COUNTS, 16 | entries << 16);
	smem_poke(our_item + SMP2P_AT_VERSION, SMP2P_RESTART_ACKS << 8);
	settle();
	smem_poke(our_item + SMP2P_AT_VERSION, SMP2P_RESTART_ACKS << 8 | SMP2P_VERSION);
	settle();
	smp2p_agreed = smp2p_restart_acks = smp2p_restart_acked = false;
	need(&ipcc);
	wr(ipcc.base + IPCC_SEND, IPCC_LPASS << 16 | IPCC_SMP2P);
	if (!smem_peek(our_item, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC || smp2p_entry(&ours, OUR_ENTRY_STOP) < 0) {
		printf("adsp: %u: OUR ITEM DOES NOT READ BACK AS WRITTEN\n", step);
		return false;
	}
	show_words("our item reads back", &ours);
	if (!glink_items_make()) {               /* redfin: before the DSP starts, as Linux */
		return false;
	}
	glink_reset();
	return true;
}

/* ---- step 7: the DSP, started ---------------------------------------------------------------------- */

/*
 * Android's kernel (this phone, dmesg): the image loaded at 8.92 s, "brought out of reset" at
 * 9.10, "error ready" 40 ms later; the audio services' protection domain up at 9.39.
 * Here (runs 085341 and later): the DSP's SMP2P item appears 17 ms after TrustZone returns, with
 * a ring of its SMP2P and GLINK doorbells; at 41 ms "slave-kernel" says 6, up and asking for its
 * own power, and our request for the rails is given back; at 2.2 s it adds "sleepstate_see".
 * Asked to stop, it acknowledges within 25 ms (0x10e), and it starts again in the same session.
 */
static bool
step_start(void)
{
	struct smp2p words, last;
	uint64_t answer = ~0ull, started, now, ready_at = 0, limit;
	uint32_t state = 0, rang, changes = 0, wait_seconds = 10;
	int32_t result;
	bool have, spoke = false, handed_over = false;

	if (!image_written) {
		printf("adsp: %u: the image is not in place\n", step);
		return false;
	}
	PE_parse_boot_argn("adsp_wait", &wait_seconds, sizeof(wait_seconds));
	bzero(&last, sizeof(last));
	have = dsp_words(&last);
	announce("listen for the audio DSP's doorbells, ask RPMh for its rails, tell the always-on processor that its image is "
	    "loaded, and ask TrustZone to authenticate the image and let the DSP out of reset");
	need(&ipcc);
	for (unsigned i = 0; i < 8 && (rang = rd(ipcc.base + IPCC_RECEIVED)) != IPCC_NOBODY && rang >> 16 == IPCC_LPASS; i++) {
		put(&ipcc, "heard", IPCC_CLEAR, rang);
	}
	put(&ipcc, "listened for", IPCC_ENABLE, IPCC_LPASS << 16 | IPCC_SMP2P);
	put(&ipcc, "listened for", IPCC_ENABLE, IPCC_LPASS << 16 | IPCC_GLINK);
	bzero(&doorbell, sizeof(doorbell));
	lck_mtx_lock(&dsp_lock);
	if (!rails(384)) {
		lck_mtx_unlock(&dsp_lock);
		return false;
	}
	rails_asked = true;
	if (!mailbox_send("{class: image, res: load_state, name: adsp, val: on}")) {
		rails(0);
		rails_asked = false;
		lck_mtx_unlock(&dsp_lock);
		return false;
	}
	result = scm("to authenticate the audio DSP's image and start it", SCM_PAS_AUTH_AND_RESET, SCM_ONE_VALUE, PAS_ADSP, 0, 0,
	    &answer);
	doorbell.since = started = now_us();
	image_written = false;
	if (result != 0 || answer != 0) {
		printf("adsp: %u: TRUSTZONE DOES NOT START THE AUDIO DSP: the call returns %d, its answer is %#llx\n", step, result,
		    answer);
		mailbox_send("{class: image, res: load_state, name: adsp, val: off}");
		rails(0);
		rails_asked = false;
		lck_mtx_unlock(&dsp_lock);
		return false;
	}
	dsp_started = true;
	lck_mtx_unlock(&dsp_lock);
	printf("adsp: %u: TRUSTZONE HAS AUTHENTICATED THE IMAGE AND LET THE AUDIO DSP OUT OF RESET; watching for %u s\n", step,
	    wait_seconds);
	limit = started + wait_seconds * 1000000ull;
	for (;;) {
		now = now_us();
		if (now >= limit || !dsp_started) {
			break;
		}
		doorbells();
		spoke = spoke || doorbell.glink + doorbell.smp2p != 0;
		if (dsp_words(&words) && (!have || memcmp(&words, &last, sizeof(words)) != 0)) {
			char when[64];

			snprintf(when, sizeof(when), "+%llu.%03llu ms: the DSP's item %s", (now - started) / 1000, (now - started) % 1000,
			    have ? "changes" : "APPEARS");
			if (changes++ < 40) {
				show_words(when, &words);
			}
			last = words;
			have = spoke = true;
			dsp_listen(&words);
			state = dsp_state(&words);
			if ((state & DSP_READY) && ready_at == 0) {
				ready_at = now;
				limit = now + 3000000ull;
				printf("adsp: %u: THE AUDIO DSP SAYS IT IS UP: \"slave-kernel\" says %#x, %llu.%03llu ms after TrustZone started "
				    "it\n", step, state, (now - started) / 1000, (now - started) % 1000);
			}
			if ((state & DSP_HANDOVER) && !handed_over) {
				handed_over = true;
				printf("adsp: %u: the DSP says it has asked for its own power (bit 2), %llu.%03llu ms after the start: ours is "
				    "given back\n", step, (now - started) / 1000, (now - started) % 1000);
				lck_mtx_lock(&dsp_lock);
				if (rails_asked) {
					rails(0);
					rails_asked = false;
				}
				lck_mtx_unlock(&dsp_lock);
			}
			if (state & DSP_FATAL) {
				printf("adsp: %u: THE AUDIO DSP SAYS IT HAS GIVEN UP\n", step);
				show_crash_reason();
				break;
			}
		}
		sleep_us(200);
	}
	printf("adsp: %u: the watch is over after %llu ms; its doorbells heard: GLINK %u, SMP2P %u\n", step,
	    (now_us() - started) / 1000, doorbell.glink, doorbell.smp2p);
	show_partition();
	if (our_item != 0 && smem_peek(our_item, &words, sizeof(words))) {
		show_words("our item (429) now", &words);
	}
	{
		/* (what is at the bottom of our partition, entry by entry, as it is now) */
		uint32_t cacheline = 0, size = 0, partition = smem_partition(SMEM_US, SMEM_ADSP, &cacheline, &size);
		struct smem_partition header;
		struct smem_entry entry;

		if (partition != 0 && smem_peek(partition, &header, sizeof(header))) {
			printf("adsp: %u: the partition's header: hosts %u and %u, size %#x, free from the bottom at +%#x, from the top at "
			    "+%#x; the table's line size %u\n", step, header.host0, header.host1, header.size, header.free_uncached,
			    header.free_cached, cacheline);
			for (uint32_t at = sizeof(header), n = 0; at + sizeof(entry) <= header.size && n < 8; n++) {
				smem_peek(partition + at, &entry, sizeof(entry));
				printf("adsp: %u:   +%#x: canary %#x item %u size %u padding %u/%u\n", step, at, entry.canary, entry.item,
				    entry.size, entry.padding_data, entry.padding_header);
				if (entry.canary != SMEM_CANARY) {
					break;
				}
				at += sizeof(entry) + entry.padding_header + entry.size;
			}
		}
	}
	if (state & DSP_FATAL) {
		return false;
	}
	if (ready_at == 0) {
		printf("adsp: %u: %s\n", step, spoke ? "THE AUDIO DSP RUNS BUT HAS NOT SAID \"UP\"" : "NOT A WORD FROM THE AUDIO DSP");
		show_crash_reason();
	}
	return ready_at != 0;
}

/* ---- GLINK (redwood_wifi.c; items 478 to 480 of our partition with the audio DSP) ------------------- */

#define ITEM_GLINK_DESCRIPTOR   478
#define ITEM_GLINK_OUR_QUEUE    479
#define ITEM_GLINK_THEIR_QUEUE  480
#define GLINK_OUR_TAIL          0
#define GLINK_OUR_HEAD          4
#define GLINK_THEIR_TAIL        8
#define GLINK_THEIR_HEAD        12
#define GLINK_RESERVE           16

enum {
	GLINK_VERSION = 0, GLINK_VERSION_ACK = 1, GLINK_OPEN = 2, GLINK_CLOSE = 3, GLINK_OPEN_ACK = 4, GLINK_INTENT = 5,
	GLINK_DONE = 6, GLINK_INTENT_REQUEST = 7, GLINK_INTENT_REQUEST_ACK = 8, GLINK_DATA = 9, GLINK_CLOSE_ACK = 11,
	GLINK_DATA_MORE = 12, GLINK_READ_NOTIFY = 13, GLINK_DONE_REUSE = 14, GLINK_SIGNALS = 15,
};
#define GLINK_VERSION_1         1
#define GLINK_INTENT_REUSE      1u

struct glink_message {
	uint16_t command, param1;
	uint32_t param2;
};
struct glink_packet {
	struct glink_packet *next;
	uint32_t length;
	uint8_t data[];
};

/*
 * The channels we answer when the DSP opens them, each with our number for it and the intents
 * we give it when it is open both ways (how many buffers of which size the DSP may send into):
 * IPCRTR, which carries QRTR (dt: qcom,intents of qcom,adsp_qrtr), and apr_audio_svc, which
 * carries APR, the audio services' own protocol (below; Android's apr_tal_rpmsg.c asks for no
 * particular intents: what the DSP sends there is a few hundred bytes, up to a few kilobytes).
 */
enum { CHANNEL_QRTR, CHANNEL_APR, CHANNEL_PMIC, CHANNEL_FASTRPC, CHANNEL_SSR, CHANNELS };

struct glink_channel {
	const char *name;
	uint32_t ours;                          /* our number for it */
	struct {
		uint32_t size, count;
	} intents[3];
	uint32_t theirs;                        /* the DSP's number for it; 0: it has not opened it */
	bool asked, acknowledged, open;         /* our open sent; acknowledged; both ways done and our intents told */
	bool request_answered, request_granted; /* our last request for an intent */
	struct {
		uint32_t id, size;
		bool there, in_use;
	} their_intents[64];                    /* what we may send into */
	struct {
		uint32_t size, fill;
		uint8_t *data;
	} our_intents[80];                      /* what the DSP may send into; an intent's number is its place here + 1 (0950: 24 -> 80, FastRPC's 64) */
	uint32_t our_intent_count;
	struct glink_packet *first, *last;      /* received, in order */
};

static LCK_MTX_DECLARE(glink_lock, &adsp_lock_group);
static struct {
	uint32_t descriptor, ours, ours_size, theirs, theirs_size;
	bool up, broken;
	uint32_t features;
	bool agreed;
	struct glink_channel channel[CHANNELS];
	uint32_t said, heard, trace;
	char others[4][33];                     /* the channels the DSP opens that nobody here speaks (for the log) */
} glink = {
	.channel = {
		[CHANNEL_QRTR] = { .name = "IPCRTR", .ours = 1, .intents = { { 0x800, 5 }, { 0x2000, 3 }, { 0x4400, 2 } } },
		[CHANNEL_APR] = { .name = "apr_audio_svc", .ours = 2, .intents = { { 0x200, 8 }, { 0x1000, 4 }, { 0x2000, 2 } } },
		/* the battery manager's (battmgr below): answers of a few dozen bytes, a name of 128 */
		[CHANNEL_PMIC] = { .name = "PMIC_RTR_ADSP_APPS", .ours = 3, .intents = { { 0x200, 6 }, { 0x1000, 2 }, { 0, 0 } } },
		/*
		 * 0950: FastRPC (dt: qcom,msm_fastrpc_rpmsg qcom,intents <0x64 0x40>: the DSP's answers, 16 to 32 bytes;
		 * /dev/redwood-fastrpc below), and glink_ssr (dt: qcom,adsp_glink_ssr), answered so it is not left
		 * hanging; what comes on it is only logged. adsp_fastrpc=0: neither is answered (the old way).
		 */
		[CHANNEL_FASTRPC] = { .name = "fastrpcglink-apps-dsp", .ours = 4, .intents = { { 0x64, 0x40 }, { 0, 0 }, { 0, 0 } } },
		[CHANNEL_SSR] = { .name = "glink_ssr", .ours = 5, .intents = { { 0x100, 4 }, { 0, 0 }, { 0, 0 } } },
	},
};

/* 0950: FastRPC's and glink_ssr's channels are answered unless adsp_fastrpc=0 (below, /dev/redwood-fastrpc) */
static uint32_t fastrpc_mode = 1;
static volatile bool fastrpc_dsp_gone;
static bool fastrpc_start_on_open;             /* adsp=2: the DSP is started by the first open of the device */
static bool fastrpc_started_by_open;
static void fastrpc_start_dsp(void);           /* (below redwood_adsp_init) */

static bool
glink_wanted(unsigned c)
{
	if (c == CHANNEL_FASTRPC || c == CHANNEL_SSR) {
		if (fastrpc_mode == 0) {
			return false;
		}
		fastrpc_dsp_gone = false;       /* (the DSP has (re)opened them: it is there) */
	}
	return true;
}

static struct glink_channel *
glink_theirs(uint32_t theirs)
{
	for (unsigned c = 0; c < CHANNELS; c++) {
		if (glink.channel[c].theirs != 0 && glink.channel[c].theirs == theirs) {
			return &glink.channel[c];
		}
	}
	return NULL;
}

static bool
glink_find(void)
{
	uint32_t size = 0;

	glink.descriptor = smem_item(ITEM_GLINK_DESCRIPTOR, &size);
	if (glink.descriptor == 0 || size != 32) {
		return false;
	}
	glink.ours = smem_item(ITEM_GLINK_OUR_QUEUE, &glink.ours_size);
	glink.theirs = smem_item(ITEM_GLINK_THEIR_QUEUE, &glink.theirs_size);
	return glink.ours != 0 && glink.theirs != 0 && glink.ours_size >= 1024 && glink.theirs_size >= 1024 &&
	       !(glink.ours_size & 7) && !(glink.theirs_size & 7);
}

static void
glink_reset(void)
{
	uint32_t size = 0, descriptor = smem_item(ITEM_GLINK_DESCRIPTOR, &size);

	if (descriptor == 0 || size != 32) {
		printf("adsp: %u: GLINK's descriptor (item %u) is not there: nothing to reset\n", step, ITEM_GLINK_DESCRIPTOR);
		return;
	}
	printf("adsp: %u: GLINK's descriptor at %#lx: our queue read to %#x and written to %#x, the DSP's read to %#x and written "
	    "to %#x\n", step, SMEM_PHYS + descriptor, smem_word(descriptor + GLINK_OUR_TAIL), smem_word(descriptor + GLINK_OUR_HEAD),
	    smem_word(descriptor + GLINK_THEIR_TAIL), smem_word(descriptor + GLINK_THEIR_HEAD));
	if (smem_word(descriptor + GLINK_OUR_HEAD) != 0 || smem_word(descriptor + GLINK_THEIR_TAIL) != 0) {
		printf("adsp: %u: about to write our two marks there (+4, +8): 0\n", step);
		smem_poke(descriptor + GLINK_OUR_HEAD, 0);
		smem_poke(descriptor + GLINK_THEIR_TAIL, 0);
		settle();
	}
}

static uint32_t
glink_waiting(void)
{
	uint32_t head = smem_word(glink.descriptor + GLINK_THEIR_HEAD), tail = smem_word(glink.descriptor + GLINK_THEIR_TAIL);

	if (head >= glink.theirs_size || tail >= glink.theirs_size) {
		return 0;
	}
	return head >= tail ? head - tail : glink.theirs_size - tail + head;
}

static void
glink_peek(void *to, uint32_t offset, uint32_t count)
{
	uint32_t at = (smem_word(glink.descriptor + GLINK_THEIR_TAIL) + offset) % glink.theirs_size;

	for (uint32_t i = 0; i < count; i++) {
		((uint8_t *)to)[i] = smem[glink.theirs + (at + i) % glink.theirs_size];
	}
}

static void
glink_advance(uint32_t count)
{
	smem_poke(glink.descriptor + GLINK_THEIR_TAIL, (smem_word(glink.descriptor + GLINK_THEIR_TAIL) + count) % glink.theirs_size);
}

static uint32_t
glink_room(void)
{
	uint32_t head = smem_word(glink.descriptor + GLINK_OUR_HEAD), tail = smem_word(glink.descriptor + GLINK_OUR_TAIL), room;

	if (head >= glink.ours_size || tail >= glink.ours_size) {
		return 0;
	}
	room = tail <= head ? glink.ours_size - head + tail : tail - head;
	return room < GLINK_RESERVE ? 0 : room - GLINK_RESERVE;
}

static bool
glink_put(const void *first, uint32_t first_length, const void *second, uint32_t second_length)
{
	volatile uint8_t *queue = (volatile uint8_t *)(uintptr_t)smem + glink.ours;
	uint32_t head = smem_word(glink.descriptor + GLINK_OUR_HEAD), length = first_length + second_length;
	uint32_t padded = (length + 7) & ~7u;

	if (!glink.up || glink_room() < padded) {
		return false;
	}
	for (uint32_t i = 0; i < padded; i++) {
		queue[(head + i) % glink.ours_size] = i < first_length ? ((const uint8_t *)first)[i] :
		    i < length ? ((const uint8_t *)second)[i - first_length] : 0;
	}
	settle();
	smem_poke(glink.descriptor + GLINK_OUR_HEAD, (head + padded) % glink.ours_size);
	settle();
	wr(ipcc.base + IPCC_SEND, IPCC_LPASS << 16 | IPCC_GLINK);
	glink.said++;
	return true;
}

static const char *
glink_name(unsigned command)
{
	static const char *const names[] = {
		"version", "version-ack", "open", "close", "open-ack", "intent", "done", "intent-request", "intent-request-ack",
		"data", "10", "close-ack", "data-more", "read-notify", "done-reuse", "signals"
	};

	return command < sizeof(names) / sizeof(names[0]) ? names[command] : "?";
}

static void
glink_say(unsigned command, uint32_t param1, uint32_t param2, const void *more, uint32_t more_length)
{
	struct glink_message message = { .command = (uint16_t)command, .param1 = (uint16_t)param1, .param2 = param2 };
	bool sent = glink_put(&message, sizeof(message), more, more_length);

	if (glink.trace > 0 || !sent) {
		glink.trace -= glink.trace > 0;
		printf("adsp: glink: > %s %u %#x%s\n", glink_name(command), param1, param2, sent ? "" : ": NO ROOM IN OUR QUEUE, not sent");
	}
}

static bool
glink_offer(struct glink_channel *channel, uint32_t size)
{
	uint32_t words[2] = { size, channel->our_intent_count + 1 }, i = channel->our_intent_count;

	if (i >= sizeof(channel->our_intents) / sizeof(channel->our_intents[0]) || size == 0 || size > 0x10000) {
		return false;
	}
	if (channel->our_intents[i].data == NULL || channel->our_intents[i].size != size) {
		channel->our_intents[i].data = kalloc_data(size, Z_WAITOK | Z_ZERO);
	}
	if (channel->our_intents[i].data == NULL) {
		return false;
	}
	channel->our_intents[i].size = size;
	channel->our_intents[i].fill = 0;
	channel->our_intent_count++;
	glink_say(GLINK_INTENT, channel->ours, 1, words, sizeof(words));
	return true;
}

/* Everything the DSP has put into its queue, taken out and acted on (Linux's qcom_glink_native_rx()). With glink_lock held. */
static void
glink_pump(void)
{
	struct glink_message message;
	struct glink_channel *channel;
	uint32_t words[4];
	char name[33];

	while (glink.up && !glink.broken) {
		uint32_t waiting = glink_waiting(), length = sizeof(message);

		if (waiting < sizeof(message)) {
			return;
		}
		glink_peek(&message, 0, sizeof(message));
		if (message.command == GLINK_OPEN) {
			length += message.param2;
		} else if (message.command == GLINK_INTENT) {
			length += 8 * message.param2;
		} else if (message.command == GLINK_DATA || message.command == GLINK_DATA_MORE) {
			if (waiting < 16) {
				return;
			}
			glink_peek(words, 8, 8);
			length = 16 + words[0];
		}
		length = (length + 7) & ~7u;
		if (length > glink.theirs_size - 8) {
			printf("adsp: glink: A MESSAGE OF %u BYTES (%s %u %#x): not believed, and nothing more is read\n", length,
			    glink_name(message.command), message.param1, message.param2);
			glink.broken = true;
			return;
		}
		if (waiting < length) {
			return;
		}
		glink.heard++;
		if (glink.trace > 0 && message.command != GLINK_DATA && message.command != GLINK_DATA_MORE &&
		    message.command != GLINK_INTENT && message.command != GLINK_DONE_REUSE) {
			glink.trace--;
			printf("adsp: glink: < %s %u %#x\n", glink_name(message.command), message.param1, message.param2);
		}
		switch (message.command) {
		case GLINK_VERSION:
			if (message.param1 == GLINK_VERSION_1) {
				glink.features &= message.param2;
			}
			glink_advance(length);
			glink_say(GLINK_VERSION_ACK, GLINK_VERSION_1, glink.features, NULL, 0);
			break;
		case GLINK_VERSION_ACK:
			glink_advance(length);
			if (message.param1 == GLINK_VERSION_1 && message.param2 == glink.features) {
				glink.agreed = true;
			} else if (message.param1 != 0) {
				glink.features &= message.param2;
				glink_say(GLINK_VERSION, GLINK_VERSION_1, glink.features, NULL, 0);
			}
			break;
		case GLINK_OPEN:
			bzero(name, sizeof(name));
			glink_peek(name, sizeof(message), message.param2 < sizeof(name) - 1 ? message.param2 : sizeof(name) - 1);
			glink_advance(length);
			channel = NULL;
			for (unsigned c = 0; c < CHANNELS; c++) {
				if (strcmp(name, glink.channel[c].name) == 0 && glink_wanted(c)) {
					channel = &glink.channel[c];
				}
			}
			if (channel == NULL) {
				printf("adsp: glink: the DSP opens \"%s\" as its channel %u: left unanswered, nobody here speaks it\n", name,
				    message.param1);
				for (unsigned i = 0; i < 4; i++) {
					if (glink.others[i][0] == 0) {
						strlcpy(glink.others[i], name, sizeof(glink.others[i]));
						break;
					}
				}
				break;
			}
			printf("adsp: glink: the DSP opens \"%s\" as its channel %u: acknowledged, and opened from our side as channel %u\n",
			    name, message.param1, channel->ours);
			channel->theirs = message.param1;
			glink_say(GLINK_OPEN_ACK, channel->theirs, 0, NULL, 0);
			if (!channel->asked) {
				uint32_t size = (uint32_t)strlen(channel->name) + 1;

				bzero(name, sizeof(name));
				strlcpy(name, channel->name, sizeof(name));
				glink_say(GLINK_OPEN, channel->ours, size, name, (size + 7) & ~7u);
				channel->asked = true;
			}
			break;
		case GLINK_OPEN_ACK:
			glink_advance(length);
			for (unsigned c = 0; c < CHANNELS; c++) {
				if (message.param1 == glink.channel[c].ours) {
					glink.channel[c].acknowledged = true;
				}
			}
			break;
		case GLINK_INTENT:
			channel = glink_theirs(message.param1);
			for (uint32_t i = 0; channel != NULL && i < message.param2; i++) {
				glink_peek(words, 8 + 8 * i, 8);
				for (unsigned n = 0; n < sizeof(channel->their_intents) / sizeof(channel->their_intents[0]); n++) {
					if (!channel->their_intents[n].there || channel->their_intents[n].id == words[1]) {
						channel->their_intents[n].there = true;
						channel->their_intents[n].in_use = false;
						channel->their_intents[n].size = words[0];
						channel->their_intents[n].id = words[1];
						if (channel == &glink.channel[CHANNEL_QRTR] && qlog_line()) {
							printf("SWEET QLOG: IPCRTR: the DSP offers intent %u of %u bytes (slot %u)\n", words[1],
							    words[0], n);
						}
						break;
					}
				}
			}
			glink_advance(length);
			break;
		case GLINK_DONE:
		case GLINK_DONE_REUSE:
			channel = glink_theirs(message.param1);
			for (unsigned n = 0; channel != NULL && n < sizeof(channel->their_intents) / sizeof(channel->their_intents[0]); n++) {
				if (channel->their_intents[n].there && channel->their_intents[n].id == message.param2) {
					channel->their_intents[n].in_use = false;
					channel->their_intents[n].there = message.command == GLINK_DONE_REUSE;
					if (channel == &glink.channel[CHANNEL_QRTR] && qlog_line()) {
						printf("SWEET QLOG: IPCRTR: the DSP is done with intent %u%s\n", message.param2,
						    message.command == GLINK_DONE_REUSE ? " (reused)" : " (gone)");
					}
				}
			}
			glink_advance(length);
			break;
		case GLINK_INTENT_REQUEST:
			glink_advance(length);
			if ((channel = glink_theirs(message.param1)) != NULL) {
				bool granted = glink_offer(channel, message.param2);

				glink_say(GLINK_INTENT_REQUEST_ACK, channel->ours, granted, NULL, 0);
			}
			break;
		case GLINK_INTENT_REQUEST_ACK:
			glink_advance(length);
			for (unsigned c = 0; c < CHANNELS; c++) {
				if (message.param1 == glink.channel[c].theirs || message.param1 == glink.channel[c].ours) {
					glink.channel[c].request_answered = true;
					glink.channel[c].request_granted = message.param2 != 0;
					if (c == CHANNEL_QRTR && qlog_line()) {
						printf("SWEET QLOG: IPCRTR: our intent request %s\n", message.param2 ? "GRANTED" : "REFUSED");
					}
				}
			}
			break;
		case GLINK_DATA:
		case GLINK_DATA_MORE: {
			uint32_t intent = message.param2 - 1;

			channel = glink_theirs(message.param1);
			if (channel == NULL || intent >= channel->our_intent_count ||
			    words[0] > channel->our_intents[intent].size - channel->our_intents[intent].fill) {
				printf("adsp: glink: < data of %u bytes on channel %u for intent %u: NOT OURS OR TOO MUCH, dropped\n", words[0],
				    message.param1, message.param2);
				glink_advance(length);
				break;
			}
			glink_peek(channel->our_intents[intent].data + channel->our_intents[intent].fill, 16, words[0]);
			channel->our_intents[intent].fill += words[0];
			glink_advance(length);
			if (words[1] == 0) {
				struct glink_packet *packet = kalloc_data(sizeof(*packet) + channel->our_intents[intent].fill, Z_WAITOK);

				if (packet != NULL) {
					packet->next = NULL;
					packet->length = channel->our_intents[intent].fill;
					memcpy(packet->data, channel->our_intents[intent].data, packet->length);
					if (channel->last != NULL) {
						channel->last->next = packet;
					} else {
						channel->first = packet;
					}
					channel->last = packet;
				}
				channel->our_intents[intent].fill = 0;
				glink_say(GLINK_DONE_REUSE, channel->ours, message.param2, NULL, 0);
			}
			break;
		}
		case GLINK_READ_NOTIFY:
			glink_advance(length);
			wr(ipcc.base + IPCC_SEND, IPCC_LPASS << 16 | IPCC_GLINK);
			break;
		case GLINK_CLOSE:
			glink_advance(length);
			glink_say(GLINK_CLOSE_ACK, message.param1, 0, NULL, 0);
			if ((channel = glink_theirs(message.param1)) != NULL) {
				printf("adsp: glink: THE DSP CLOSES \"%s\"\n", channel->name);
				channel->open = false;
				channel->theirs = 0;
			}
			break;
		case GLINK_CLOSE_ACK:
		case GLINK_SIGNALS:
			glink_advance(length);
			break;
		default:
			printf("adsp: glink: A MESSAGE THIS DOES NOT KNOW (%u, %u, %#x): nothing more is read\n", message.command,
			    message.param1, message.param2);
			glink.broken = true;
			return;
		}
		for (unsigned c = 0; c < CHANNELS; c++) {
			channel = &glink.channel[c];
			if (channel->theirs != 0 && channel->acknowledged && !channel->open) {
				channel->open = true;
				printf("adsp: glink: THE CHANNEL \"%s\" IS OPEN both ways (ours %u, the DSP's %u); telling the DSP our intents\n",
				    channel->name, channel->ours, channel->theirs);
				for (unsigned i = 0; i < 3; i++) {
					for (unsigned n = 0; n < channel->intents[i].count; n++) {
						glink_offer(channel, channel->intents[i].size);
					}
				}
			}
		}
	}
}

/* One whole message to the DSP on a channel; waits up to five seconds for an intent of the DSP's that it fits. */
static int
glink_send(struct glink_channel *channel, const void *data, uint32_t length)
{
	struct {
		struct glink_message message;
		uint32_t size, left;
	} header = { { GLINK_DATA, (uint16_t)channel->ours, 0 }, length, 0 };
	bool asked = false, fits;
	int found = -1;

	lck_mtx_lock(&glink_lock);
	if (glink.up && length + sizeof(header) + 8 > glink.ours_size - GLINK_RESERVE) {
		lck_mtx_unlock(&glink_lock);
		return EMSGSIZE;
	}
	for (unsigned waited = 0;; waited++) {
		glink_pump();
		if (!glink.up || !channel->open || glink.broken) {
			lck_mtx_unlock(&glink_lock);
			return ENOTCONN;
		}
		found = -1;
		fits = false;
		for (unsigned n = 0; n < sizeof(channel->their_intents) / sizeof(channel->their_intents[0]); n++) {
			if (!channel->their_intents[n].there || channel->their_intents[n].size < length) {
				continue;
			}
			fits = true;
			if (!channel->their_intents[n].in_use &&
			    (found < 0 || channel->their_intents[n].size < channel->their_intents[found].size)) {
				found = (int)n;
			}
		}
		if (found >= 0 && glink_room() >= ((length + sizeof(header) + 7) & ~7u)) {
			break;
		}
		if (!fits && !asked) {
			channel->request_answered = false;
			glink_say(GLINK_INTENT_REQUEST, channel->ours, length, NULL, 0);
			asked = true;
			if (channel == &glink.channel[CHANNEL_QRTR] && qlog_line()) {
				printf("SWEET QLOG: IPCRTR: no intent of the DSP's holds %u bytes: one asked for\n", length);
			}
		}
		if (waited >= 5000 || (!fits && asked && channel->request_answered && !channel->request_granted)) {
			if (channel == &glink.channel[CHANNEL_QRTR] && qlog_line()) {
				unsigned there = 0, busy = 0;
				uint32_t biggest = 0;

				for (unsigned n = 0; n < sizeof(channel->their_intents) / sizeof(channel->their_intents[0]); n++) {
					if (channel->their_intents[n].there) {
						there++;
						busy += channel->their_intents[n].in_use;
						biggest = MAX(biggest, channel->their_intents[n].size);
					}
				}
				printf("SWEET QLOG: IPCRTR: %u bytes NOT SENT after %u ms: %u intents of the DSP's (%u in use, the biggest %u "
				    "bytes), %u bytes of room in our queue\n", length, waited, there, busy, biggest, glink_room());
			}
			lck_mtx_unlock(&glink_lock);
			return found < 0 ? ENOBUFS : EAGAIN;
		}
		lck_mtx_unlock(&glink_lock);
		sleep_ms(1);
		lck_mtx_lock(&glink_lock);
	}
	channel->their_intents[found].in_use = true;
	header.message.param2 = channel->their_intents[found].id;
	glink_put(&header, sizeof(header), data, length);
	lck_mtx_unlock(&glink_lock);
	return 0;
}

/* ---- QRTR: hello, and the services the DSP tells of (redwood_wifi.c, without ports of our own yet) ---- */

#define QRTR_US                 1u
#define QRTR_CONTROL            0xfffffffeu
enum { QRTR_DATA = 1, QRTR_HELLO = 2, QRTR_BYE = 3, QRTR_NEW_SERVER = 4, QRTR_DEL_SERVER = 5, QRTR_DEL_CLIENT = 6, QRTR_RESUME_TX = 7,
       QRTR_NEW_LOOKUP = 10, QRTR_DEL_LOOKUP = 11 };

struct qrtr_header {
	uint32_t version, type, from_node, from_port, confirm, size, to_node, to_port;
};
struct qrtr_control {
	uint32_t command;
	uint32_t words[4];
};

static struct {
	uint32_t service, instance, node, port;
	bool there;
} qrtr_servers[64];
static uint32_t qrtr_peer, qrtr_in, qrtr_out, qrtr_hellos, qrtr_lookups;

/* adsp_qlog=1: one datagram (dir "<" from the DSP, ">" to it) */
static void
qlog_packet(const char *dir, const struct qrtr_header *h, const uint8_t *data, uint32_t length, int error)
{
	char hex[3 * 48 + 1];
	uint32_t n = MIN(length, 48u);

	if (!adsp_qlog) {
		return;
	}
	qlog_packets++;
	if (*dir == '<') {
		qlog_in++;
	} else {
		qlog_out++;
	}
	if (!qlog_line()) {
		if (qlog_packets % 100 == 0) {
			printf("SWEET QLOG: %u datagrams (%u from the DSP, %u to it)\n", qlog_packets, qlog_in, qlog_out);
		}
		return;
	}
	for (uint32_t i = 0; i < n; i++) {
		snprintf(hex + 3 * i, 4, " %02x", data[i]);
	}
	hex[3 * n] = '\0';
	printf("SWEET QLOG: %s type %u %u:%#x -> %u:%#x confirm %u, %u bytes%s%s:%s\n", dir, h->type, h->from_node, h->from_port,
	    h->to_node, h->to_port, h->confirm, length, error ? ", NOT SENT: error " : "", error ? (error == ENOBUFS ? "ENOBUFS" :
	    error == EAGAIN ? "EAGAIN" : error == ENOTCONN ? "ENOTCONN" : "other") : "", hex);
}

static int
qrtr_out_packet(uint32_t type, uint32_t from_port, uint32_t to_node, uint32_t to_port, const void *data, uint32_t length)
{
	uint32_t padded = (length + 3) & ~3u;
	struct qrtr_header *packet;
	int error;

	packet = kalloc_data(sizeof(*packet) + padded, Z_WAITOK | Z_ZERO);
	if (packet == NULL) {
		return ENOMEM;
	}
	packet->version = 1;
	packet->type = type;
	packet->from_node = QRTR_US;
	packet->from_port = from_port;
	packet->size = length;
	packet->to_node = to_node;
	packet->to_port = to_port;
	memcpy(packet + 1, data, length);
	error = glink_send(&glink.channel[CHANNEL_QRTR], packet, (uint32_t)sizeof(*packet) + padded);
	qlog_packet(">", packet, data, length, error);
	kfree_data(packet, sizeof(*packet) + padded);
	qrtr_out += error == 0;
	return error;
}

/*
 * 0951: pd-mapper, the service registry locator (QMI service 0x40), in the kernel: the DSP's PD
 * framework asks it which protection domain offers a service (tms/servreg, avs/audio) before it
 * hands out notifications; Linux's pd-mapper (andersson/pd-mapper) answers from the firmware's
 * adspr.jsn, adsps.jsn, adspua.jsn, ios-topaz (0144) from this table. It is announced to the DSP
 * after its HELLO (as Linux's name service announces local services) and given to its lookups.
 * A PD's failure report (0x24) is logged: it is the sensors PD's own last words.
 * Boot-arg adsp_pdm=0: not served.
 */
#define PDM_SERVICE             0x40u
#define PDM_INSTANCE            (1u | 1u << 8)  /* version 1, instance 1 (pd-mapper's SERVREG_QMI_VERSION 257) */
#define PDM_PORT                0x4001u
#define PDM_DOMAIN_INSTANCE     74u             /* adsp*.jsn qmi_instance_id */
#define PDM_GET_DOMAIN_LIST     0x21u
#define PDM_FAILURE_REPORT      0x24u

static const struct {
	const char *service, *domain;
} pdm_table[] = {
	{ "tms/servreg", "msm/adsp/root_pd" },
	{ "tms/servreg", "msm/adsp/sensor_pd" },
	{ "tms/servreg", "msm/adsp/audio_pd" },
	{ "avs/audio", "msm/adsp/audio_pd" },
};
static uint32_t pdm_on = 1, pdm_requests, pdm_announced;

static int qrtr_out_packet(uint32_t type, uint32_t from_port, uint32_t to_node, uint32_t to_port, const void *data, uint32_t length);

static void
pdm_announce(uint32_t node, uint32_t port)
{
	struct qrtr_control server = { .command = QRTR_NEW_SERVER, .words = { PDM_SERVICE, PDM_INSTANCE, QRTR_US, PDM_PORT } };
	int error;

	if (!pdm_on) {
		return;
	}
	error = qrtr_out_packet(QRTR_NEW_SERVER, QRTR_CONTROL, node, port, &server, sizeof(server));
	pdm_announced++;
	printf("SWEET PDMAP: pd-mapper (service 0x40) announced to node %u port %#x at our port %#x: %s\n", node, port, PDM_PORT,
	    error ? "NOT SENT" : "sent");
}

/* A QMI TLV of the request (QMI: u8 type, u16 transaction, u16 message, u16 length, then u8 t, u16 l, value). */
static const uint8_t *
pdm_tlv(const uint8_t *message, uint32_t length, uint8_t type, uint16_t *found)
{
	for (uint32_t at = 7; at + 3 <= length;) {
		uint16_t l = (uint16_t)(message[at + 1] | message[at + 2] << 8);

		if (at + 3 + l > length) {
			break;
		}
		if (message[at] == type) {
			*found = l;
			return message + at + 3;
		}
		at += 3 + l;
	}
	return NULL;
}

static uint32_t
pdm_put(uint8_t *answer, uint32_t at, uint8_t type, const void *value, uint16_t length)
{
	answer[at] = type;
	answer[at + 1] = (uint8_t)length;
	answer[at + 2] = (uint8_t)(length >> 8);
	memcpy(answer + at + 3, value, length);
	return at + 3 + length;
}

static void
pdm_input(const struct qrtr_header *header, const uint8_t *message, uint32_t length)
{
	uint8_t answer[512], list[320];
	char name[80] = "", reason[160] = "";
	uint16_t l = 0, transaction, id, result[2] = { 0, 0 }, total = 0, revision = 1;
	const uint8_t *value;
	uint32_t at = 7, listed = 1, offset = 0, instance = PDM_DOMAIN_INSTANCE, zero = 0;

	if (length < 7 || message[0] != 0) {
		return;
	}
	transaction = (uint16_t)(message[1] | message[2] << 8);
	id = (uint16_t)(message[3] | message[4] << 8);
	if ((value = pdm_tlv(message, length, 1, &l)) != NULL) {
		memcpy(name, value, l < sizeof(name) - 1 ? l : sizeof(name) - 1);
	}
	pdm_requests++;
	answer[0] = 2;                          /* a response */
	answer[1] = (uint8_t)transaction;
	answer[2] = (uint8_t)(transaction >> 8);
	answer[3] = (uint8_t)id;
	answer[4] = (uint8_t)(id >> 8);
	if (id == PDM_GET_DOMAIN_LIST) {
		if ((value = pdm_tlv(message, length, 0x10, &l)) != NULL && l == 4) {
			memcpy(&offset, value, 4);
		}
		list[0] = 0;
		for (unsigned i = 0; i < sizeof(pdm_table) / sizeof(pdm_table[0]); i++) {
			uint8_t dl = (uint8_t)strlen(pdm_table[i].domain);

			if (strcmp(pdm_table[i].service, name) != 0 || total++ < offset) {
				continue;
			}
			list[listed++] = dl;
			memcpy(list + listed, pdm_table[i].domain, dl);
			listed += dl;
			memcpy(list + listed, &instance, 4);
			listed += 4;
			list[listed++] = 0;             /* service_data_valid */
			memcpy(list + listed, &zero, 4);
			listed += 4;
			list[0]++;
		}
		at = pdm_put(answer, at, 2, result, sizeof(result));
		at = pdm_put(answer, at, 0x10, &total, 2);
		at = pdm_put(answer, at, 0x11, &revision, 2);
		if (list[0] != 0) {
			at = pdm_put(answer, at, 0x12, list, (uint16_t)listed);
		}
		printf("SWEET PDMAP: node %u port %#x asks for \"%s\" (offset %u): %u domain(s)\n", header->from_node, header->from_port,
		    name, offset, total);
	} else if (id == PDM_FAILURE_REPORT) {
		if ((value = pdm_tlv(message, length, 2, &l)) != NULL) {
			memcpy(reason, value, l < sizeof(reason) - 1 ? l : sizeof(reason) - 1);
		}
		printf("SWEET PDMAP: A PROTECTION DOMAIN FAILED: \"%s\": \"%s\"\n", name, reason);
		at = pdm_put(answer, at, 2, result, sizeof(result));
	} else {
		result[0] = 1;                  /* QMI_RESULT_FAILURE, QMI_ERR_NOT_SUPPORTED */
		result[1] = 0x5e;
		at = pdm_put(answer, at, 2, result, sizeof(result));
		printf("SWEET PDMAP: message %#x from node %u port %#x (%u bytes): not supported\n", id, header->from_node,
		    header->from_port, length);
	}
	answer[5] = (uint8_t)(at - 7);
	answer[6] = (uint8_t)((at - 7) >> 8);
	qrtr_out_packet(QRTR_DATA, PDM_PORT, header->from_node, header->from_port, answer, at);
}

static void
qrtr_control(const struct qrtr_header *header, const struct qrtr_control *control)
{
	struct qrtr_control hello = { .command = QRTR_HELLO };
	int found = -1, error;

	switch (control->command) {
	case QRTR_HELLO:
		if (qrtr_hellos++ != 0) {
			break;
		}
		error = qrtr_out_packet(QRTR_HELLO, QRTR_CONTROL, header->from_node, QRTR_CONTROL, &hello, sizeof(hello));
		printf("adsp: qrtr: HELLO FROM NODE %u; ours to it: %s\n", header->from_node, error == 0 ? "SENT" : "NOT SENT");
		pdm_announce(header->from_node, QRTR_CONTROL);  /* 0951 */
		break;
	case QRTR_NEW_SERVER:
		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			if (qrtr_servers[i].there && qrtr_servers[i].node == control->words[2] && qrtr_servers[i].port == control->words[3]) {
				found = (int)i;
				break;
			}
			if (!qrtr_servers[i].there && found < 0) {
				found = (int)i;
			}
		}
		if (found >= 0) {
			qrtr_servers[found].there = true;
			qrtr_servers[found].service = control->words[0];
			qrtr_servers[found].instance = control->words[1];
			qrtr_servers[found].node = control->words[2];
			qrtr_servers[found].port = control->words[3];
		}
		printf("adsp: qrtr: a service: %u (%#x), version %u, instance %u, at node %u port %u\n", control->words[0],
		    control->words[0], control->words[1] & 0xff, control->words[1] >> 8, control->words[2], control->words[3]);
		break;
	case QRTR_DEL_SERVER:
		printf("adsp: qrtr: service %u at node %u port %u has gone\n", control->words[0], control->words[2], control->words[3]);
		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			if (qrtr_servers[i].there && qrtr_servers[i].node == control->words[2] && qrtr_servers[i].port == control->words[3]) {
				qrtr_servers[i].there = false;
			}
		}
		break;
	case QRTR_NEW_LOOKUP:
		/* the DSP looks for a service of ours (a protection domain's locator, say): there are none yet */
		if (qrtr_lookups++ < 16) {
			printf("adsp: qrtr: the DSP looks for service %u (%#x), instance %u: %s\n", control->words[0],
			    control->words[0], control->words[1], pdm_on && (control->words[0] == 0 || control->words[0] == PDM_SERVICE) ?
			    "pd-mapper is ours" : "nobody here offers it");
		}
		/* 0951: our servers that match, then the end of the list (an empty one), as Linux's name service answers */
		if (pdm_on && (control->words[0] == 0 || control->words[0] == PDM_SERVICE)) {
			pdm_announce(header->from_node, header->from_port);
		}
		{
			struct qrtr_control end = { .command = QRTR_NEW_SERVER };

			qrtr_out_packet(QRTR_NEW_SERVER, QRTR_CONTROL, header->from_node, header->from_port, &end, sizeof(end));
		}
		break;
	case QRTR_BYE:
		printf("adsp: qrtr: BYE from node %u\n", header->from_node);
		bzero(qrtr_servers, sizeof(qrtr_servers));
		break;
	default:
		break;
	}
}

static bool qrtr_deliver(const struct qrtr_header *header, const uint8_t *data, uint32_t length);   /* 0956 */

static void
qrtr_input(const uint8_t *data, uint32_t length)
{
	struct qrtr_header header;
	uint32_t at;

	if (length >= sizeof(header) && data[0] == 1) {
		memcpy(&header, data, sizeof(header));
		at = sizeof(header);
	} else if (length >= 16 && data[0] == 3) {
		bzero(&header, sizeof(header));
		header.type = data[1];
		header.confirm = data[2] & 1;
		memcpy(&header.size, data + 4, 4);
		header.from_node = (uint32_t)(data[8] | data[9] << 8);
		header.from_port = (uint32_t)(data[10] | data[11] << 8);
		header.to_node = (uint32_t)(data[12] | data[13] << 8);
		header.to_port = (uint32_t)(data[14] | data[15] << 8);
		header.from_port = header.from_port == 0xfffe ? QRTR_CONTROL : header.from_port;
		header.to_port = header.to_port == 0xfffe ? QRTR_CONTROL : header.to_port;
		at = 16 + data[3];
	} else {
		printf("adsp: qrtr: %u bytes that are no datagram (they begin %#x)\n", length, length ? data[0] : 0);
		return;
	}
	if (header.size == 0 || at + header.size > length) {
		printf("adsp: qrtr: a datagram of %u bytes in a message of %u: dropped\n", header.size, length);
		return;
	}
	qrtr_in++;
	if (qrtr_peer == 0) {
		qrtr_peer = header.from_node;
	}
	qlog_packet("<", &header, data + at, header.size, 0);
	if (header.type == QRTR_RESUME_TX) {
		return;
	}
	if (header.confirm) {
		struct qrtr_control resume = { .command = QRTR_RESUME_TX, .words = { header.to_node, header.to_port } };

		qrtr_out_packet(QRTR_RESUME_TX, header.to_port, header.from_node, header.from_port, &resume, sizeof(resume));
	}
	if (header.to_port == QRTR_CONTROL || header.type == QRTR_HELLO || header.type == QRTR_BYE ||
	    header.type == QRTR_NEW_SERVER || header.type == QRTR_DEL_SERVER || header.type == QRTR_NEW_LOOKUP) {
		struct qrtr_control control = { 0 };

		memcpy(&control, data + at, header.size < sizeof(control) ? header.size : sizeof(control));
		qrtr_control(&header, &control);
		return;
	}
	if (header.to_port == PDM_PORT && pdm_on) {
		pdm_input(&header, data + at, header.size);     /* 0951 */
		return;
	}
	if (qrtr_deliver(&header, data + at, header.size)) {
		return;                         /* 0956: a port of /dev/redwood-qrtr */
	}
	printf("adsp: qrtr: a datagram of %u bytes from node %u port %u to our port %u: nobody here listens there\n", header.size,
	    header.from_node, header.from_port, header.to_port);
}

/* ---- APR: the audio services' protocol, on the channel apr_audio_svc ------------------------------------- */

/*
 * What Android's audio drivers say to the DSP's audio services (Linux: drivers/soc/qcom/apr.c,
 * include/linux/soc/qcom/apr.h; Android's q6core.c): a packet of a header of eight words -- a
 * word of kind, header length and version and the packet's length, who it is from and to (a
 * service, a domain, a port each: we are domain 5, the DSP 4), a token the answer carries back,
 * an opcode -- and what the opcode takes. The DSP's core service (3) answers whether the audio
 * services are up (AVCS_CMD_ADSP_EVENT_GET_STATE, which Android asks again and again after the DSP
 * has started, until the answer is 1) and which services it has and in which versions.
 */
#define APR_DOMAIN_ADSP                 4
#define APR_DOMAIN_APPS                 5
#define APR_SVC_ADSP_CORE               3
#define APR_MSG_TYPE_SEQ_CMD            2
#define APR_HDR_FIELD(type, bytes)      ((uint16_t)((type) << 8 | ((bytes) / 4) << 4))      /* version 0 */
#define APR_BASIC_RSP_RESULT            0x000110e8u
#define AVCS_GET_VERSIONS               0x00012905u
#define AVCS_GET_VERSIONS_RSP           0x00012906u
#define AVCS_CMD_ADSP_EVENT_GET_STATE   0x0001290cu
#define AVCS_CMDRSP_ADSP_EVENT_GET_STATE 0x0001290du

struct apr_header {
	uint16_t field, size;
	uint8_t from_service, from_domain;
	uint16_t from_port;
	uint8_t to_service, to_domain;
	uint16_t to_port;
	uint32_t token, opcode;
};

static struct {
	uint32_t opcode, token, length;
	uint32_t words[64];
	volatile bool there;
} apr_answer;
static uint32_t apr_in, apr_out, apr_next_token = 1;
static bool audio_up;                   /* the core service has said that the audio services are up (step 11) */
static uint32_t apr_waited_for;         /* the token a question waits for the answer to; 0: none */
static redwood_apr_events_t apr_events; /* the sound's (redwood_sound.c): what nobody waits for */
static redwood_apr_events_t apr_events_second;  /* the microphones' (redwood_mic.c) */
static LCK_MTX_DECLARE(apr_lock, &adsp_lock_group);     /* one question at a time */

/* A packet from the DSP's audio services. From the link's thread. */
static void
apr_input(const uint8_t *data, uint32_t length)
{
	struct apr_header header;
	uint32_t header_length, payload;

	if (length < sizeof(header)) {
		printf("adsp: apr: %u bytes, no APR packet\n", length);
		return;
	}
	memcpy(&header, data, sizeof(header));
	header_length = ((header.field >> 4) & 0xf) * 4;
	if (header_length < sizeof(header) || header_length > length) {
		printf("adsp: apr: a header of %u bytes in a packet of %u: dropped\n", header_length, length);
		return;
	}
	payload = (header.size < length ? header.size : length) - header_length;
	apr_in++;
	if (header.token != apr_waited_for || apr_waited_for == 0) {
		redwood_apr_events_t events = apr_events;
		uint32_t words[64];

		bzero(words, sizeof(words));
		memcpy(words, data + header_length, payload < sizeof(words) ? payload : sizeof(words));
		if (events != NULL && events(header.from_service, header.from_port, header.opcode, header.token, words,
		    payload < sizeof(words) ? payload : sizeof(words))) {
			return;
		}
		events = apr_events_second;
		if (events != NULL && events(header.from_service, header.from_port, header.opcode, header.token, words,
		    payload < sizeof(words) ? payload : sizeof(words))) {
			return;
		}
	}
	if (apr_in <= 40) {
		uint32_t w[4] = { 0, 0, 0, 0 };

		memcpy(w, data + header_length, payload < sizeof(w) ? payload : sizeof(w));
		printf("adsp: apr: < from service %u of domain %u (port %u) to %u of %u (port %u): opcode %#x, token %u, %u bytes: %#x %#x "
		    "%#x %#x\n", header.from_service, header.from_domain, header.from_port, header.to_service, header.to_domain,
		    header.to_port, header.opcode, header.token, payload, w[0], w[1], w[2], w[3]);
	}
	bzero(apr_answer.words, sizeof(apr_answer.words));
	memcpy(apr_answer.words, data + header_length, payload < sizeof(apr_answer.words) ? payload : sizeof(apr_answer.words));
	apr_answer.length = payload;
	apr_answer.opcode = header.opcode;
	apr_answer.token = header.token;
	apr_answer.there = true;
}

/*
 * A command to one of the DSP's services (and its answer waited for, with ms): true when it was
 * sent and, if waited for, answered; the answer is in apr_answer and, if asked for, in *answer.
 * The port is the service's own numbering of what the command is for (ASM: session << 8 | stream).
 */
bool
redwood_adsp_apr(uint8_t service, uint16_t port, uint32_t token, uint32_t opcode, const void *payload, uint32_t length,
    uint32_t ms, struct redwood_apr_answer *answer)
{
	uint8_t packet[sizeof(struct apr_header) + 256];
	struct apr_header *header = (struct apr_header *)packet;
	bool answered = false;
	int error;

	if (length > sizeof(packet) - sizeof(*header) || !glink.channel[CHANNEL_APR].open) {
		return false;
	}
	bzero(packet, sizeof(packet));
	header->field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD, sizeof(*header));
	header->size = (uint16_t)(sizeof(*header) + length);
	header->from_service = service;
	header->from_domain = APR_DOMAIN_APPS;
	header->from_port = port;
	header->to_service = service;
	header->to_domain = APR_DOMAIN_ADSP;
	header->to_port = port;
	header->opcode = opcode;
	if (length != 0) {
		memcpy(packet + sizeof(*header), payload, length);
	}
	if (ms == 0) {
		/* (sent and not waited for: what comes back is the events' handler's) */
		header->token = token;
		error = glink_send(&glink.channel[CHANNEL_APR], packet, (uint32_t)sizeof(*header) + length);
		apr_out += error == 0;
		return error == 0;
	}
	lck_mtx_lock(&apr_lock);
	if (token == 0) {
		token = apr_next_token++;
		if (apr_next_token >= 0x01000000u) {
			apr_next_token = 1;
		}
	}
	header->token = token;
	apr_answer.there = false;
	apr_waited_for = token;
	error = glink_send(&glink.channel[CHANNEL_APR], packet, (uint32_t)sizeof(*header) + length);
	if (error != 0) {
		printf("adsp: apr: > opcode %#x to service %u: NOT SENT (%d)\n", opcode, service, error);
	} else {
		apr_out++;
		for (uint32_t waited = 0; waited < ms && !answered; waited++) {
			if (apr_answer.there && apr_answer.token == token) {
				answered = true;
				if (answer != NULL) {
					answer->opcode = apr_answer.opcode;
					answer->token = apr_answer.token;
					answer->length = apr_answer.length;
					memcpy(answer->words, apr_answer.words, sizeof(answer->words));
				}
				break;
			}
			sleep_ms(1);
		}
	}
	apr_waited_for = 0;
	lck_mtx_unlock(&apr_lock);
	return answered;
}

void
redwood_adsp_apr_events(redwood_apr_events_t handler)
{
	apr_events = handler;
}

void
redwood_adsp_apr_events_second(redwood_apr_events_t handler)
{
	apr_events_second = handler;
}

/* A command to one of the DSP's services, and its answer waited for: true, and the answer in apr_answer, if it came. */
static bool
apr_ask(uint8_t service, uint32_t opcode, const void *payload, uint32_t length, uint32_t ms)
{
	return redwood_adsp_apr(service, 0, 0, opcode, payload, length, ms, NULL);
}

/* ---- the battery manager, on the channel PMIC_RTR_ADSP_APPS ------------------------------------------ */

/*
 * On this SoC the audio DSP also runs the battery's and the charger's management -- the fuel gauge,
 * the charging -- and the operating system only asks it how things are: Android's
 * qti_battery_charger.c, Linux's qcom_battmgr.c over pmic_glink.c, on the channel PMIC_RTR_ADSP_APPS
 * that the DSP opens. A message is a header (owner 32778, the battery manager; type 1, request and
 * answer, or 2, a notice; an opcode) and its words; a property is asked for with opcode 0x30 (the
 * battery's) or 0x32 (the USB port's), the battery's number (0), the property's number and a word
 * that a get does not use, and is answered with the property's number, its value and a result (0:
 * good). Only gets are sent from here: nothing is set, no limit, no current, no notice asked for.
 * With adsp=1 (battmgr=0: not) it is asked every BATTMGR_SECONDS while the DSP runs, and what it says
 * goes to bsd/dev/redwood_battery.c (redwood_battery_report()), and from there to the power source
 * that iOS reads (iokit/Kernel/RedwoodBattery.cpp). battmgr=2: every property once, for the log.
 * Units: capacity in hundredths of a percent, voltage in uV, current in uA (more than 0 while the
 * battery takes charge), the temperature in hundredths of a degree, charges in uAh.
 * The properties' numbers are this phone's firmware's, read with battmgr=2 and checked against what
 * Android shows at the same moment (dumpsys battery, /sys/class/power_supply/battery): up to 9 they
 * are those of Linux's qcom_battmgr.c (sm8350), from there on one more property comes before the
 * temperature, so the temperature is 13, not 12, and the charge counter, the cycles, the designed and
 * the learnt full charge are 15 to 18.
 */
#define BATTMGR_OWNER           32778
#define BATTMGR_REQUEST         1
#define BATTMGR_NOTICE          2
#define BATTMGR_BATTERY_GET     0x30
#define BATTMGR_USB_GET         0x32
#define BATT_STATUS             0               /* Linux's POWER_SUPPLY_STATUS_*: 1 charging, 2 discharging, 3 not charging, 4 full */
#define BATT_HEALTH             1
#define BATT_PRESENT            2
#define BATT_CAPACITY           4
#define BATT_VOLT_NOW           7
#define BATT_CURR_NOW           9
#define BATT_TEMP               13              /* hundredths of a degree: 3900 when Android says 39.0 */
#define BATT_CHG_COUNTER        15              /* the charge in the battery now, uAh */
#define BATT_CYCLE_COUNT        16
#define BATT_CHG_FULL_DESIGN    17              /* 5000000 uAh */
#define BATT_CHG_FULL           18              /* the full charge as the fuel gauge has learnt it, uAh */
#define USB_VOLT_NOW            1               /* uV */
#define USB_CURR_NOW            3               /* uA, what the port gives */
#define USB_TYPE                6               /* Linux's POWER_SUPPLY_USB_TYPE_*: 1 here, a computer's port */
#define USB_ONLINE              0
#define BATTMGR_SECONDS         10

struct battmgr_message {
	uint32_t owner, type, opcode;
	uint32_t words[3];                      /* asked: battery, property, value; answered: property, value, result */
};

static struct {
	volatile uint32_t answers, notices;
	uint32_t opcode, property, value, result, length;
	bool wanted;
	volatile bool runs;
} battmgr;

static void
battmgr_input(const uint8_t *data, uint32_t length)
{
	struct battmgr_message m;

	bzero(&m, sizeof(m));
	memcpy(&m, data, length < sizeof(m) ? length : sizeof(m));
	if (m.owner != BATTMGR_OWNER || length < 12) {
		printf("adsp: battmgr: < %u bytes of owner %u, type %u, opcode %#x: not the battery manager's\n", length, m.owner,
		    m.type, m.opcode);
		return;
	}
	if (m.type == BATTMGR_NOTICE) {
		if (battmgr.notices++ < 8) {
			tell("adsp: battmgr: < a notice, opcode %#x, %u bytes (%#x %#x %#x)\n", m.opcode, length, m.words[0], m.words[1],
			    m.words[2]);
		}
		return;
	}
	battmgr.opcode = m.opcode;
	battmgr.property = m.words[0];
	battmgr.value = m.words[1];
	battmgr.result = m.words[2];
	battmgr.length = length;
	os_atomic_inc(&battmgr.answers, release);
}

/* One property asked for and its answer waited for (a second at most); one thread asks at a time (battmgr_thread). */
static bool
battmgr_get(uint32_t opcode, uint32_t property, uint32_t *value)
{
	struct battmgr_message q = { BATTMGR_OWNER, BATTMGR_REQUEST, opcode, { 0, property, 0 } };
	uint32_t before = os_atomic_load(&battmgr.answers, acquire);

	if (glink_send(&glink.channel[CHANNEL_PMIC], &q, sizeof(q)) != 0) {
		return false;
	}
	for (unsigned ms = 0; ms < 1000; ms++) {
		if (os_atomic_load(&battmgr.answers, acquire) != before) {
			if (battmgr.opcode != opcode || battmgr.property != property || battmgr.length < sizeof(struct battmgr_message)) {
				before = battmgr.answers;       /* (somebody else's answer: wait on) */
				continue;
			}
			if (battmgr.result != 0) {
				return false;
			}
			*value = battmgr.value;
			return true;
		}
		sleep_ms(1);
	}
	return false;
}

static void
battmgr_thread(__unused void *parameter, __unused wait_result_t result)
{
	battmgr.runs = true;
	for (unsigned ms = 0; ms < 15000 && battmgr.wanted && !glink.channel[CHANNEL_PMIC].open; ms += 10) {
		sleep_ms(10);
	}
	if (!glink.channel[CHANNEL_PMIC].open) {
		tell("adsp: battmgr: the DSP has not opened PMIC_RTR_ADSP_APPS: nothing is asked\n");
		battmgr.runs = false;
		thread_terminate(current_thread());
	}
	{
		/* boot-arg battmgr=2: every property once, for the log (which number is which on this phone's firmware) */
		uint32_t mode = 1;

		PE_parse_boot_argn("battmgr", &mode, sizeof(mode));
		for (uint32_t p = 0; mode == 2 && p < 32; p++) {
			uint32_t v = 0;
			bool ok = battmgr_get(BATTMGR_BATTERY_GET, p, &v);

			tell("adsp: battmgr: battery property %u: %s %u (%d)\n", p, ok ? "" : "NO ANSWER", v, (int)v);
		}
		for (uint32_t p = 0; mode == 2 && p < 12; p++) {
			uint32_t v = 0;
			bool ok = battmgr_get(BATTMGR_USB_GET, p, &v);

			tell("adsp: battmgr: USB property %u: %s %u (%d)\n", p, ok ? "" : "NO ANSWER", v, (int)v);
		}
	}
	for (unsigned round = 0; battmgr.wanted && glink.up && glink.channel[CHANNEL_PMIC].open; round++) {
		struct redwood_battery_state b = { .size = sizeof(b) };
		uint32_t capacity = 0, status = 0, present = 0, volt = 0, curr = 0, temp = 0, usb = 0;
		uint32_t usb_type = 0, usb_volt = 0, usb_curr = 0, charge = 0;
		static uint32_t full, design, cycles;
		bool good = battmgr_get(BATTMGR_BATTERY_GET, BATT_CAPACITY, &capacity) &&
		    battmgr_get(BATTMGR_BATTERY_GET, BATT_STATUS, &status) &&
		    battmgr_get(BATTMGR_BATTERY_GET, BATT_PRESENT, &present) &&
		    battmgr_get(BATTMGR_BATTERY_GET, BATT_VOLT_NOW, &volt) &&
		    battmgr_get(BATTMGR_BATTERY_GET, BATT_CURR_NOW, &curr) &&
		    battmgr_get(BATTMGR_BATTERY_GET, BATT_TEMP, &temp) &&
		    battmgr_get(BATTMGR_USB_GET, USB_ONLINE, &usb);

		if (good) {
			/* (what may be missing without harm, and the capacities and the cycles only now and then: they hardly change) */
			(void)battmgr_get(BATTMGR_BATTERY_GET, BATT_CHG_COUNTER, &charge);
			if (usb) {
				(void)battmgr_get(BATTMGR_USB_GET, USB_TYPE, &usb_type);
				(void)battmgr_get(BATTMGR_USB_GET, USB_VOLT_NOW, &usb_volt);
				(void)battmgr_get(BATTMGR_USB_GET, USB_CURR_NOW, &usb_curr);
			}
			if (round % 30 == 0 || full == 0) {
				(void)battmgr_get(BATTMGR_BATTERY_GET, BATT_CHG_FULL, &full);
				(void)battmgr_get(BATTMGR_BATTERY_GET, BATT_CHG_FULL_DESIGN, &design);
				(void)battmgr_get(BATTMGR_BATTERY_GET, BATT_CYCLE_COUNT, &cycles);
			}
			b.percent_hundredths = (int32_t)capacity;
			b.voltage_uv = (int32_t)volt;
			b.current_ua = (int32_t)curr;
			b.temperature_hundredths = (int32_t)temp;
			b.present = present != 0;
			b.external = usb != 0;
			b.status = (int32_t)status;
			b.charge_uah = (int32_t)charge;
			b.full_uah = (int32_t)full;
			b.design_uah = (int32_t)design;
			b.cycles = (int32_t)cycles;
			b.usb_type = (int32_t)usb_type;
			b.usb_voltage_uv = (int32_t)usb_volt;
			b.usb_current_ua = (int32_t)usb_curr;
			redwood_battery_report(&b);
		}
		if (round < 3 || round % 6 == 0 || !good) {
			tell("adsp: battmgr: %s capacity %u (hundredths of a percent), status %u, present %u, %d uV, %d uA, temperature %d "
			    "(hundredths of a degree), charge %u uAh, USB online %u (type %u, %u uV, %u uA)\n",
			    good ? "the DSP says:" : "NO ANSWER to all of", capacity, status, present, (int)volt, (int)curr, (int)temp,
			    charge, usb, usb_type, usb_volt, usb_curr);
			if (round % 30 == 0 && good) {
				tell("adsp: battmgr: full charge %u uAh, as designed %u uAh, %u cycles\n", full, design, cycles);
			}
		}
		for (unsigned ms = 0; ms < BATTMGR_SECONDS * 1000 && battmgr.wanted && glink.up; ms += 50) {
			sleep_ms(50);
		}
	}
	battmgr.runs = false;
	thread_terminate(current_thread());
}

static void
battmgr_start(void)
{
	thread_t thread;

	uint32_t wanted = 0;            /* redfin: off unless battmgr=1 (the battery is the PMIC's here) */

	PE_parse_boot_argn("battmgr", &wanted, sizeof(wanted));
	if (wanted == 0 || battmgr.runs) {
		return;
	}
	if (option("battery")) {
		/* (the loader's way, through Qualcomm's UEFI charger drivers: one way to the battery at a time) */
		tell("adsp: battmgr: not with battery=1: the battery is asked about the loader's way\n");
		return;
	}
	battmgr.wanted = true;
	if (kernel_thread_start(battmgr_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

static void
battmgr_stop(void)
{
	battmgr.wanted = false;
	for (unsigned ms = 0; ms < 2000 && battmgr.runs; ms++) {
		sleep_ms(1);
	}
}

/* ---- the link's thread ----------------------------------------------------------------------------- */

static volatile bool link_wanted, link_runs;

/* ---- FastRPC (0950): the channel fastrpcglink-apps-dsp, /dev/redwood-fastrpc, the DSP's SMMU streams ---- */

/*
 * The DSP's FastRPC framework is how its protection domains call the operating system: the
 * sensors PD reads its configuration and registry with it (Android's adsprpc.c and the
 * adsp_listener of libadsprpc; Linux's drivers/misc/fastrpc.c and hexagonrpcd). Every call is one
 * message of 40 bytes on the channel fastrpcglink-apps-dsp, which the DSP opens and we answer
 * (as Linux does when its rpmsg driver binds), and one answer of 16 (or 32) bytes back:
 *   message: s32 client (0 for the kernel's own calls), s32 thread, u64 ctx (idx << 4 | PD),
 *            u32 handle, u32 scalars, u64 addr, u64 size of the arguments' buffer
 *   answer:  u64 ctx, s32 result (32 bytes: + flags, early-wake time, version)
 * Everything else -- the buffer's layout, the contexts, the listener loop that serves the DSP's
 * calls to us -- is userland's: phone/fastrpc/fastrpcd (a port of hexagonrpcd) writes messages to
 * /dev/redwood-fastrpc and reads the answers back. This file only carries them and checks that an
 * address is one the DSP can reach:
 *   addr = IOVA | session << 32: session 3, 4 or 5 is the context bank adsprpc's DT gives the
 *          streams 0x1403-0x1405 (adsprpc.c: smmu.cb = SID & 0xf). FASTRPC_SMMU_IDENTITY sends such
 *          a stream to a context bank of the apps SMMU (0x15000000) with translation off, as the
 *          camera's IFE streams (0921) and the WLAN's (phone/wifi/radio.c) go: IOVA == PA, so the
 *          buffers must be below 4 GiB. A message whose session has not been set up is refused.
 *   FASTRPC_ALLOC (an addition to the plan's ABI): physically contiguous RAM below 4 GiB, from the
 *          GPU's pool (sweet_gpu_pool_alloc(), as the camera's frames: HLOS RAM below 4 GiB that XNU
 *          never maps -- XNU's own RAM starts at 4 GiB on this port, so kmem_alloc_contig can never
 *          give any). The kernel zeroes it through a device mapping; /dev/pmem reads and writes it the
 *          same way (uncached, so no cache maintenance; 32-bit aligned offsets and lengths). What is
 *          handed out is never given back (the DSP may keep a mapping of it after the daemon has gone):
 *          a closed device's buffers are handed out again to the next opener (same size).
 *   read() blocks for an answer (O_NONBLOCK: EAGAIN), and returns ECONNRESET once the DSP has given
 *          up, so the daemon ends and launchd starts it again for a DSP that is back. Before the DSP
 *          has opened the channel, write() returns ENOTCONN and read() waits (EAGAIN).
 *   adsp=2: the DSP is started when the device is first opened (fastrpcd is running, so its sensors
 *          PD finds its files), not beside the system's start as adsp=1 does; adsp_fastrpc=0 with
 *          adsp=2 starts it at once.
 * The device is one opener at a time; the channel stays answered while the DSP keeps it open,
 * whether the device is open or not. Boot-arg adsp_fastrpc=0: neither the channel nor glink_ssr is
 * answered and there is no device (the old way); adsp_fastrpc=2: every message and answer logged.
 */
struct fastrpc_message {
	int32_t client, thread;
	uint64_t ctx;
	uint32_t handle, scalars;
	uint64_t addr, size;
};
struct fastrpc_smmu_identity {
	uint32_t sid;
};
struct fastrpc_alloc {
	uint64_t size, pa;
};
#define FASTRPC_SMMU_IDENTITY   _IOW('f', 1, struct fastrpc_smmu_identity)
#define FASTRPC_ALLOC           _IOWR('f', 2, struct fastrpc_alloc)
#define FASTRPC_SID_FIRST       0x1403u         /* dt: qcom,msm_fastrpc_compute_cb10-12 iommus <&apps_smmu 0x1403..0x1405 0> */
#define FASTRPC_SIDS            3u
#define FASTRPC_SESSION(sid)    ((sid) & 0xfu)
#define FASTRPC_ANSWERS         64
#define FASTRPC_ANSWER_MAX      64
#define FASTRPC_POOL            32
#define FASTRPC_ALLOC_MAX       (16u << 20)
#define FASTRPC_LOGGED          64
#define SMMU_PA                 0x15000000u     /* dt: apps-smmu@15000000 */

static LCK_MTX_DECLARE(fastrpc_lock, &adsp_lock_group);
static struct {
	bool open;
	pid_t pid;
	uint8_t answer[FASTRPC_ANSWERS][FASTRPC_ANSWER_MAX];
	uint8_t answer_length[FASTRPC_ANSWERS];
	uint32_t head, tail;                    /* answers waiting: tail..head */
	uint32_t sent, answered, dropped, refused, logged;
	struct fastrpc_message last;            /* the last message sent (for the log when the DSP gives up) */
	struct selinfo select;
	struct {
		uint64_t pa, size;              /* pa 0: an empty entry */
		bool in_use;
	} pool[FASTRPC_POOL];
	uint64_t pool_lo, pool_hi;              /* the range handed out, for the log */
	/* the apps SMMU */
	vm_offset_t gr0, gr1, cb_page;
	uint32_t psize, npage, cb;              /* cb: ours + 1; 0: none yet */
	bool identity[FASTRPC_SIDS];
	uint32_t smr[FASTRPC_SIDS];
} fastrpc;
static int fastrpc_major = -1;

/* An answer from the DSP. From the link's thread. */
static void
fastrpc_input(const uint8_t *data, uint32_t length)
{
	uint64_t ctx = 0;
	int32_t result = 0;
	bool dropped = false;

	if (length >= 12) {
		memcpy(&ctx, data, 8);
		memcpy(&result, data + 8, 4);
	}
	lck_mtx_lock(&fastrpc_lock);
	if (!fastrpc.open || fastrpc.head - fastrpc.tail >= FASTRPC_ANSWERS) {
		fastrpc.dropped++;
		dropped = true;
	} else {
		uint32_t at = fastrpc.head % FASTRPC_ANSWERS;

		bzero(fastrpc.answer[at], FASTRPC_ANSWER_MAX);
		memcpy(fastrpc.answer[at], data, length < FASTRPC_ANSWER_MAX ? length : FASTRPC_ANSWER_MAX);
		fastrpc.answer_length[at] = (uint8_t)(length < FASTRPC_ANSWER_MAX ? length : FASTRPC_ANSWER_MAX);
		fastrpc.head++;
		fastrpc.answered++;
		wakeup(&fastrpc.head);
		selwakeup(&fastrpc.select);
	}
	lck_mtx_unlock(&fastrpc_lock);
	if (fastrpc_mode >= 2 || dropped || result != 0 || fastrpc.answered <= FASTRPC_LOGGED) {
		printf("SWEET FASTRPC: < ctx %#llx result %d (%u bytes)%s\n", ctx, result, length,
		    dropped ? (fastrpc.open ? ": NO ROOM, dropped" : ": nobody has the device open, dropped") : "");
	}
}

/* The DSP has given up, or the link is down: whoever waits for an answer hears ECONNRESET. */
static void
fastrpc_dsp_down(const char *why)
{
	fastrpc_dsp_gone = true;
	lck_mtx_lock(&fastrpc_lock);
	wakeup(&fastrpc.head);
	selwakeup(&fastrpc.select);
	lck_mtx_unlock(&fastrpc_lock);
	if (fastrpc_mode != 0) {
		printf("SWEET FASTRPC: the DSP is gone (%s): %u messages sent, %u answers, %u dropped, %u refused; the last message: "
		    "handle %u scalars %#x (method %u) ctx %#llx addr %#llx\n", why, fastrpc.sent, fastrpc.answered, fastrpc.dropped,
		    fastrpc.refused, fastrpc.last.handle, fastrpc.last.scalars, (fastrpc.last.scalars >> 24) & 0x1f, fastrpc.last.ctx,
		    fastrpc.last.addr);
	}
}

/*
 * The apps SMMU: stream SID through a context bank with translation off (sweet_cam.c cap_smmu_up(),
 * phone/wifi/radio.c smmu_wlan_identity()). One bank for the three streams, one match register each
 * (mask 0). Only the pages used are mapped: global space 0 (ID, SMRs, S2CRs), 1 (CBARs) and the bank's.
 */
static int
fastrpc_smmu_identity(uint32_t sid)
{
	unsigned s = sid - FASTRPC_SID_FIRST;
	uint32_t id0, id1, nsmr, ncb, free_smr = ~0u, cb = ~0u;
	uint64_t used[4] = { 0 };
	vm_offset_t page = 0;
	int error = 0;

	if (sid < FASTRPC_SID_FIRST || s >= FASTRPC_SIDS) {
		return EINVAL;
	}
	lck_mtx_lock(&fastrpc_lock);
	if (fastrpc.identity[s]) {
		lck_mtx_unlock(&fastrpc_lock);
		return 0;
	}
	if (fastrpc.gr0 == 0) {
		fastrpc.gr0 = ml_io_map(SMMU_PA, 0x1000);
	}
	id0 = rd(fastrpc.gr0 + 0x20);
	id1 = rd(fastrpc.gr0 + 0x24);
	nsmr = id0 & 0xff;
	ncb = id1 & 0xff;
	fastrpc.psize = (id1 >> 31) ? 0x10000 : 0x1000;
	fastrpc.npage = 1u << (((id1 >> 28) & 7) + 1);
	if ((uint64_t)fastrpc.npage * fastrpc.psize * 2 != 0x100000 || nsmr > 128) {
		printf("SWEET FASTRPC: SMMU ID0 %#x ID1 %#x: a layout not known here\n", id0, id1);
		error = ENODEV;
		goto out;
	}
	if (fastrpc.gr1 == 0) {
		fastrpc.gr1 = ml_io_map(SMMU_PA + fastrpc.psize, 0x1000);
	}
	for (uint32_t i = 0; i < nsmr; i++) {
		uint32_t smr = rd(fastrpc.gr0 + 0x800 + 4 * i), s2cr = rd(fastrpc.gr0 + 0xc00 + 4 * i);
		uint32_t id = smr & 0x7fff, mask = (smr >> 16) & 0x7fff;

		if (!(smr >> 31)) {
			free_smr = free_smr == ~0u ? i : free_smr;
			continue;
		}
		if (((s2cr >> 16) & 3) == 0) {
			used[(s2cr & 0xff) / 64] |= 1ull << ((s2cr & 0xff) % 64);
		}
		if (((id ^ sid) & ~mask & 0x7fff) != 0) {
			continue;
		}
		/* somebody (the boot loader, an earlier run) already routes this stream */
		if (((s2cr >> 16) & 3) == 1) {
			printf("SWEET FASTRPC: SMMU: SID %#x already bypasses the SMMU (SMR%u %#x, S2CR %#x): used as it is\n", sid, i, smr,
			    s2cr);
			fastrpc.identity[s] = true;
			fastrpc.smr[s] = i;
			goto out;
		}
		if (((s2cr >> 16) & 3) == 0 && (s2cr & 0xff) + 1 == fastrpc.cb) {
			fastrpc.identity[s] = true;
			fastrpc.smr[s] = i;
			goto out;
		}
		printf("SWEET FASTRPC: SMMU: SID %#x is already routed by SMR%u %#x (S2CR %#x, CBAR %#x): LEFT ALONE, refused\n", sid,
		    i, smr, s2cr, ((s2cr >> 16) & 3) == 0 ? rd(fastrpc.gr1 + 4 * (s2cr & 0xff)) : 0);
		error = EBUSY;
		goto out;
	}
	if (fastrpc.cb != 0) {
		cb = fastrpc.cb - 1;
	} else {
		/* the lowest bank that nothing points at, nobody set up, and that is not translating */
		for (uint32_t c = 0; c < ncb && c < 256 && cb == ~0u; c++) {
			if ((used[c / 64] & (1ull << (c % 64))) || rd(fastrpc.gr1 + 4 * c) != 0) {
				continue;
			}
			page = ml_io_map_unmappable(SMMU_PA + (fastrpc.npage + c) * fastrpc.psize, 0x1000, VM_WIMG_IO);
			if (page != 0 && !(rd(page) & 1u)) {
				cb = c;
				break;
			}
			page = 0;               /* (a translating bank: its mapping is left, kernel_map has room; no unmap of a part page) */
		}
	}
	if (free_smr == ~0u || cb == ~0u) {
		printf("SWEET FASTRPC: SMMU: no free match register (%d) or context bank (%d)\n", (int)free_smr, (int)cb);
		error = ENOSPC;
		goto out;
	}
	if (fastrpc.cb == 0) {
		fastrpc.cb_page = page;
		fastrpc.cb = cb + 1;
		wr(page + 0x58, 0xffffffffu);           /* FSR */
		wr(page + 0x20, 0);                     /* TTBR0 */
		wr(page + 0x24, 0);
		wr(page + 0x30, 0);                     /* TCR */
		wr(page + 0x10, 0);                     /* TCR2 */
		wr(page + 0x38, 0);                     /* MAIR */
		wr(page + 0x3c, 0);
		wr(fastrpc.gr1 + 0x800 + 4 * cb, 1);    /* CBA2R: AArch64 */
		wr(fastrpc.gr1 + 4 * cb, 0x1f000);      /* CBAR: S1, S2 bypass, as ABL's banks */
		wr(page, 0xe0);                         /* SCTLR: fault config, M = 0 (no translation) */
	}
	wr(fastrpc.gr0 + 0xc00 + 4 * free_smr, cb);                     /* S2CR: translate (type 0) through it */
	wr(fastrpc.gr0 + 0x800 + 4 * free_smr, 1u << 31 | sid);        /* SMR: valid, this stream only, last */
	wr(fastrpc.gr0 + 0x70, 0);                                      /* sTLBGSYNC */
	for (int i = 0; i < 1000 && (rd(fastrpc.gr0 + 0x74) & 1u); i++) {
		sleep_us(10);
	}
	fastrpc.identity[s] = true;
	fastrpc.smr[s] = free_smr;
	printf("SWEET FASTRPC: SMMU: SID %#x (session %u) -> CB%u (translation off) via SMR%u: SMR %#x S2CR %#x CBAR %#x SCTLR %#x\n",
	    sid, FASTRPC_SESSION(sid), cb, free_smr, rd(fastrpc.gr0 + 0x800 + 4 * free_smr), rd(fastrpc.gr0 + 0xc00 + 4 * free_smr),
	    rd(fastrpc.gr1 + 4 * cb), rd(fastrpc.cb_page));
out:
	lck_mtx_unlock(&fastrpc_lock);
	return error;
}

/* The bank's faults, if any (poke f 5): what the DSP touched that it may not. */
static void
fastrpc_smmu_faults(void)
{
	uint32_t fsr;

	if (fastrpc.cb == 0 || fastrpc.cb_page == 0) {
		printf("SWEET FASTRPC: SMMU: no bank of ours yet\n");
		return;
	}
	fsr = rd(fastrpc.cb_page + 0x58);
	printf("SWEET FASTRPC: SMMU CB%u: FSR %#x FAR %#x%08x FSYNR0 %#x; global FSR %#x FSYNR0 %#x FSYNR1 %#x%s\n", fastrpc.cb - 1,
	    fsr, rd(fastrpc.cb_page + 0x64), rd(fastrpc.cb_page + 0x60), rd(fastrpc.cb_page + 0x68), rd(fastrpc.gr0 + 0x48),
	    rd(fastrpc.gr0 + 0x50), rd(fastrpc.gr0 + 0x54), (fsr & 0x800001feu) ? ": A FAULT (cleared now)" : ": no fault");
	if (fsr & 0x800001feu) {
		wr(fastrpc.cb_page + 0x58, fsr);
	}
}

extern int sweet_gpu_pool_alloc(uint64_t bytes, uint64_t *pa);       /* bsd/dev/sweet_gpu.c */
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);

/* Zero pool RAM (XNU has no mapping of it): through a device mapping, 2 MB at a time, 64-bit stores. */
static bool
fastrpc_zero(uint64_t pa, uint64_t size)
{
	const uint64_t win = 2u << 20;

	for (uint64_t at = 0; at < size; at += win) {
		uint64_t n = MIN(win, size - at);
		vm_offset_t va = ml_io_map_unmappable((vm_offset_t)(pa + at), (vm_size_t)n, VM_WIMG_IO);
		volatile uint64_t *w = (volatile uint64_t *)va;

		if (va == 0) {
			return false;
		}
		for (uint64_t i = 0; i < n / 8; i++) {
			w[i] = 0;
		}
		__asm__ volatile ("dsb sy" ::: "memory");
		ml_io_unmap(va, (vm_size_t)n);
	}
	return true;
}

static int
fastrpc_alloc(struct fastrpc_alloc *a)
{
	vm_size_t size = round_page(a->size);
	uint64_t pa = 0;
	int slot = -1, reused = -1;

	if (size == 0 || size > FASTRPC_ALLOC_MAX) {
		return EINVAL;
	}
	lck_mtx_lock(&fastrpc_lock);
	for (int i = 0; i < FASTRPC_POOL; i++) {
		if (fastrpc.pool[i].pa != 0 && !fastrpc.pool[i].in_use && fastrpc.pool[i].size == size) {
			fastrpc.pool[i].in_use = true;
			pa = fastrpc.pool[i].pa;
			reused = i;
			break;
		}
		if (fastrpc.pool[i].pa == 0 && fastrpc.pool[i].size == 0 && slot < 0) {
			slot = i;
		}
	}
	if (reused < 0) {
		if (slot < 0) {
			lck_mtx_unlock(&fastrpc_lock);
			printf("SWEET FASTRPC: all %u buffers for the DSP handed out (%#llx-%#llx): no %lu bytes more\n", FASTRPC_POOL,
			    fastrpc.pool_lo, fastrpc.pool_hi, (unsigned long)size);
			return ENOSPC;
		}
		fastrpc.pool[slot].size = size;         /* reserved while the pool is asked */
		fastrpc.pool[slot].in_use = true;
	}
	lck_mtx_unlock(&fastrpc_lock);
	if (reused < 0) {
		if (sweet_gpu_pool_alloc(size, &pa) != 0 || pa == 0 || pa + size > 0x100000000ull) {
			printf("SWEET FASTRPC: no %lu bytes below 4 GiB in the GPU pool%s\n", (unsigned long)size,
			    pa ? " (above 4 GiB: kept back, not used)" : "");
			if (pa == 0) {
				lck_mtx_lock(&fastrpc_lock);
				fastrpc.pool[slot].size = 0;
				fastrpc.pool[slot].in_use = false;
				lck_mtx_unlock(&fastrpc_lock);
			}
			return ENOMEM;
		}
	}
	if (!fastrpc_zero(pa, size)) {
		printf("SWEET FASTRPC: %lu bytes at %#llx: no mapping to zero them; not handed out\n", (unsigned long)size, pa);
		lck_mtx_lock(&fastrpc_lock);
		if (reused >= 0) {
			fastrpc.pool[reused].in_use = false;
		} else {
			fastrpc.pool[slot].pa = pa;     /* ours from the pool: kept, free for the next try */
			fastrpc.pool[slot].in_use = false;
		}
		lck_mtx_unlock(&fastrpc_lock);
		return ENOMEM;
	}
	a->pa = pa;
	if (reused >= 0) {
		return 0;
	}
	lck_mtx_lock(&fastrpc_lock);
	fastrpc.pool[slot].pa = pa;
	if (fastrpc.pool_lo == 0 || pa < fastrpc.pool_lo) {
		fastrpc.pool_lo = pa;
	}
	if (pa + size > fastrpc.pool_hi) {
		fastrpc.pool_hi = pa + size;
	}
	lck_mtx_unlock(&fastrpc_lock);
	printf("SWEET FASTRPC: %lu bytes for the DSP at %#llx (GPU pool; buffer %d, all of them within %#llx-%#llx)\n",
	    (unsigned long)size, pa, slot, fastrpc.pool_lo, fastrpc.pool_hi);
	return 0;
}

static int
fastrpc_open(__unused dev_t dev, __unused int flags, __unused int devtype, proc_t p)
{
	lck_mtx_lock(&fastrpc_lock);
	if (fastrpc.open) {
		lck_mtx_unlock(&fastrpc_lock);
		return EBUSY;
	}
	fastrpc.open = true;
	fastrpc.pid = proc_pid(p);
	fastrpc.head = fastrpc.tail = 0;
	bool start = fastrpc_start_on_open && !fastrpc_started_by_open;
	if (start) {
		fastrpc_started_by_open = true;
		fastrpc_dsp_gone = false;       /* (a DSP about to start has not given up: writes see ENOTCONN, reads wait) */
	}
	lck_mtx_unlock(&fastrpc_lock);
	printf("SWEET FASTRPC: /dev/redwood-fastrpc opened by pid %d; the channel is %s%s%s\n", proc_pid(p),
	    glink.channel[CHANNEL_FASTRPC].open ? "OPEN" : "NOT OPEN (yet)", fastrpc_dsp_gone ? ", THE DSP HAS GIVEN UP" : "",
	    start ? "; adsp=2: starting the DSP now" : "");
	if (start) {
		fastrpc_start_dsp();
	}
	return 0;
}

static int
fastrpc_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	lck_mtx_lock(&fastrpc_lock);
	fastrpc.open = false;
	fastrpc.head = fastrpc.tail = 0;
	for (int i = 0; i < FASTRPC_POOL; i++) {
		fastrpc.pool[i].in_use = false;
	}
	selthreadclear(&fastrpc.select);
	lck_mtx_unlock(&fastrpc_lock);
	printf("SWEET FASTRPC: /dev/redwood-fastrpc closed (pid %d): %u messages sent, %u answers, %u dropped, %u refused\n",
	    fastrpc.pid, fastrpc.sent, fastrpc.answered, fastrpc.dropped, fastrpc.refused);
	return 0;
}

static int
fastrpc_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct fastrpc_message m;
	uint32_t session;
	int error;

	if (uio_resid(uio) != sizeof(m)) {
		return EINVAL;
	}
	if ((error = uiomove((caddr_t)&m, sizeof(m), uio)) != 0) {
		return error;
	}
	if (fastrpc_dsp_gone) {
		return ECONNRESET;
	}
	if (!glink.channel[CHANNEL_FASTRPC].open) {
		return ENOTCONN;
	}
	session = (uint32_t)(m.addr >> 32);
	if ((m.addr != 0 || m.size != 0) && (session < FASTRPC_SESSION(FASTRPC_SID_FIRST) ||
	    session >= FASTRPC_SESSION(FASTRPC_SID_FIRST) + FASTRPC_SIDS ||
	    !fastrpc.identity[session - FASTRPC_SESSION(FASTRPC_SID_FIRST)] || m.size > (64u << 20))) {
		if (fastrpc.refused++ < 8) {
			printf("SWEET FASTRPC: a message with addr %#llx size %#llx: its session has no stream set up "
			    "(FASTRPC_SMMU_IDENTITY first), refused\n", m.addr, m.size);
		}
		return EPERM;
	}
	error = glink_send(&glink.channel[CHANNEL_FASTRPC], &m, sizeof(m));
	if (error == 0) {
		fastrpc.last = m;
		fastrpc.sent++;
	}
	if (fastrpc_mode >= 2 || error != 0 || fastrpc.sent <= FASTRPC_LOGGED) {
		printf("SWEET FASTRPC: > client %d thread %d ctx %#llx handle %u scalars %#x (method %u, %u in %u out) addr %#llx size "
		    "%#llx%s\n", m.client, m.thread, m.ctx, m.handle, m.scalars, (m.scalars >> 24) & 0x1f, (m.scalars >> 16) & 0xff,
		    (m.scalars >> 8) & 0xff, m.addr, m.size, error ? ": NOT SENT" : "");
	}
	return error;
}

static int
fastrpc_read(__unused dev_t dev, struct uio *uio, int ioflag)
{
	uint8_t answer[FASTRPC_ANSWER_MAX];
	uint32_t length;
	int error = 0;

	lck_mtx_lock(&fastrpc_lock);
	while (fastrpc.head == fastrpc.tail) {
		if (fastrpc_dsp_gone) {
			lck_mtx_unlock(&fastrpc_lock);
			return ECONNRESET;
		}
		if (ioflag & IO_NDELAY) {
			lck_mtx_unlock(&fastrpc_lock);
			return EWOULDBLOCK;
		}
		error = msleep(&fastrpc.head, &fastrpc_lock, PCATCH | PZERO, "fastrpc", NULL);
		if (error != 0) {
			lck_mtx_unlock(&fastrpc_lock);
			return error;
		}
	}
	length = fastrpc.answer_length[fastrpc.tail % FASTRPC_ANSWERS];
	memcpy(answer, fastrpc.answer[fastrpc.tail % FASTRPC_ANSWERS], length);
	fastrpc.tail++;
	lck_mtx_unlock(&fastrpc_lock);
	return uiomove((caddr_t)answer, (int)MIN((user_ssize_t)length, uio_resid(uio)), uio);
}

static int
fastrpc_select(__unused dev_t dev, int which, void *wql, proc_t p)
{
	int ready = 1;

	if (which == FREAD) {
		lck_mtx_lock(&fastrpc_lock);
		ready = fastrpc.head != fastrpc.tail || fastrpc_dsp_gone;
		if (!ready) {
			selrecord(p, &fastrpc.select, wql);
		}
		lck_mtx_unlock(&fastrpc_lock);
	}
	return ready;
}

static int
fastrpc_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int flag, __unused proc_t p)
{
	switch (cmd) {
	case FASTRPC_SMMU_IDENTITY:
		return fastrpc_smmu_identity(((struct fastrpc_smmu_identity *)data)->sid);
	case FASTRPC_ALLOC:
		return fastrpc_alloc((struct fastrpc_alloc *)data);
	default:
		return ENOTTY;
	}
}

static const struct cdevsw fastrpc_cdevsw = {
	.d_open       = fastrpc_open,
	.d_close      = fastrpc_close,
	.d_read       = fastrpc_read,
	.d_write      = fastrpc_write,
	.d_ioctl      = fastrpc_ioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = fastrpc_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = 0,
};

static void
fastrpc_init(void)
{
	PE_parse_boot_argn("adsp_fastrpc", &fastrpc_mode, sizeof(fastrpc_mode));
	if (fastrpc_mode == 0) {
		printf("SWEET FASTRPC: off (adsp_fastrpc=0): fastrpcglink-apps-dsp and glink_ssr are left unanswered\n");
		return;
	}
	fastrpc_major = cdevsw_add(-1, &fastrpc_cdevsw);
	if (fastrpc_major < 0) {
		printf("SWEET FASTRPC: cdevsw_add failed (%d)\n", fastrpc_major);
		return;
	}
	void *node = devfs_make_node(makedev(fastrpc_major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0600, "redwood-fastrpc");
	printf("SWEET FASTRPC: /dev/redwood-fastrpc %s (major %d, adsp_fastrpc=%u)\n", node ? "ready" : "FAILED", fastrpc_major,
	    fastrpc_mode);
}

/* What arrives on glink_ssr (nothing is expected: the DSP answers there what we never ask). */
static void
ssr_input(const uint8_t *data, uint32_t length)
{
	uint32_t w[4] = { 0, 0, 0, 0 };

	memcpy(w, data, length < sizeof(w) ? length : sizeof(w));
	printf("SWEET FASTRPC: glink_ssr: %u bytes: %#x %#x %#x %#x\n", length, w[0], w[1], w[2], w[3]);
}

/* ---- /dev/redwood-qrtr (0956): QRTR ports of ours on node 1, for userland's QMI clients ---------------- */

/*
 * What Linux's AF_QIPCRTR sockets give its QMI clients (libssc's sensors client among them), for
 * the DSP only, as qol-plan.md 3.1 fixes it:
 *   open()   one port of ours (0x5000 + n, n < 16), a clone of /dev/redwood-qrtr per open
 *   write()  struct { u32 node; u32 port; u8 data[]; }: one datagram to the DSP's node and port
 *   read()   struct { u32 node; u32 port; u8 data[]; }: one datagram (blocks; O_NONBLOCK: EAGAIN),
 *            cut to the reader's buffer; at most 64 KiB wait per port, more is dropped (and counted)
 *   ioctl QRTR_LOOKUP {service, instance, node, port}: the DSP's server of the service (instance 0:
 *            any; the instance is the announcement's word >> 8, its low byte being the version),
 *            node 0 port 0 when it has not told of one (yet)
 *   close()  a DEL_CLIENT to the DSP, so its services forget the port
 * sysctl kern.sweet_adsp_services: "svc:inst@node:port,..." of every service the DSP has told of.
 */
struct qrtr_lookup {
	uint32_t service, instance, node, port;
};
#define QRTR_LOOKUP             _IOWR('q', 1, struct qrtr_lookup)
#define QRTR_PORTS              16
#define QRTR_PORT_BASE          0x5000u
#define QRTR_QUEUE_MAX          (64u << 10)
#define QRTR_WRITE_MAX          0x4000u

struct qrtr_datagram {
	struct qrtr_datagram *next;
	uint32_t node, port, length;
	uint8_t data[];
};
static LCK_MTX_DECLARE(qport_lock, &adsp_lock_group);
static struct {
	uint32_t claimed;                       /* (taken by the clone function, under devfs's lock: atomically) */
	bool open;
	pid_t pid;
	struct qrtr_datagram *first, *last;
	uint32_t queued, received, sent, dropped;
	struct selinfo select;
} qport[QRTR_PORTS];
static int qport_major = -1;

/* A datagram from the DSP to a port of ours: queued for its reader. From the link's thread. */
static bool
qrtr_deliver(const struct qrtr_header *header, const uint8_t *data, uint32_t length)
{
	unsigned n = header->to_port - QRTR_PORT_BASE;
	struct qrtr_datagram *datagram;
	bool dropped = false;

	if (header->to_port < QRTR_PORT_BASE || n >= QRTR_PORTS) {
		return false;
	}
	datagram = kalloc_data(sizeof(*datagram) + length, Z_WAITOK);
	lck_mtx_lock(&qport_lock);
	if (!qport[n].open || datagram == NULL || qport[n].queued + length > QRTR_QUEUE_MAX) {
		dropped = true;
		qport[n].dropped++;
	} else {
		datagram->next = NULL;
		datagram->node = header->from_node;
		datagram->port = header->from_port;
		datagram->length = length;
		memcpy(datagram->data, data, length);
		if (qport[n].last != NULL) {
			qport[n].last->next = datagram;
		} else {
			qport[n].first = datagram;
		}
		qport[n].last = datagram;
		qport[n].queued += length;
		qport[n].received++;
		wakeup(&qport[n]);
		selwakeup(&qport[n].select);
		datagram = NULL;
	}
	lck_mtx_unlock(&qport_lock);
	if (datagram != NULL) {
		kfree_data(datagram, sizeof(*datagram) + length);
	}
	if (dropped && qport[n].dropped <= 8) {
		printf("SWEET QRTR: %u bytes from node %u port %u to our port %#x: %s, dropped\n", length, header->from_node,
		    header->from_port, header->to_port, qport[n].open ? "ITS QUEUE IS FULL" : "the port is not open");
	}
	return true;
}

static int
qport_clone(__unused dev_t dev, int action)
{
	if (action == DEVFS_CLONE_ALLOC) {
		for (int n = 0; n < QRTR_PORTS; n++) {
			if (os_atomic_cmpxchg(&qport[n].claimed, 0u, 1u, acq_rel)) {
				return n;
			}
		}
		return -1;
	}
	return -1;
}

static int
qport_open(dev_t dev, __unused int flags, __unused int devtype, proc_t p)
{
	unsigned n = (unsigned)minor(dev);

	if (n >= QRTR_PORTS || !qport[n].claimed) {
		return ENXIO;
	}
	lck_mtx_lock(&qport_lock);
	qport[n].open = true;
	qport[n].pid = proc_pid(p);
	qport[n].first = qport[n].last = NULL;
	qport[n].queued = qport[n].received = qport[n].sent = qport[n].dropped = 0;
	lck_mtx_unlock(&qport_lock);
	printf("SWEET QRTR: port %#x opened by pid %d\n", QRTR_PORT_BASE + n, proc_pid(p));
	return 0;
}

static int
qport_close(dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	unsigned n = (unsigned)minor(dev);
	struct qrtr_datagram *datagram, *next;
	struct qrtr_control bye = { .command = QRTR_DEL_CLIENT, .words = { QRTR_US, QRTR_PORT_BASE + n } };

	if (n >= QRTR_PORTS) {
		return 0;
	}
	lck_mtx_lock(&qport_lock);
	qport[n].open = false;
	datagram = qport[n].first;
	qport[n].first = qport[n].last = NULL;
	qport[n].queued = 0;
	selthreadclear(&qport[n].select);
	lck_mtx_unlock(&qport_lock);
	for (; datagram != NULL; datagram = next) {
		next = datagram->next;
		kfree_data(datagram, sizeof(*datagram) + datagram->length);
	}
	if (glink.channel[CHANNEL_QRTR].open && qrtr_peer != 0) {
		qrtr_out_packet(QRTR_DEL_CLIENT, QRTR_CONTROL, qrtr_peer, QRTR_CONTROL, &bye, sizeof(bye));
	}
	printf("SWEET QRTR: port %#x closed (pid %d): %u datagrams sent, %u received, %u dropped\n", QRTR_PORT_BASE + n,
	    qport[n].pid, qport[n].sent, qport[n].received, qport[n].dropped);
	os_atomic_store(&qport[n].claimed, 0u, release);
	return 0;
}

static int
qport_write(dev_t dev, struct uio *uio, __unused int ioflag)
{
	unsigned n = (unsigned)minor(dev);
	uint32_t address[2], length;
	uint8_t *data;
	int error;

	if (n >= QRTR_PORTS || uio_resid(uio) < (user_ssize_t)sizeof(address) ||
	    uio_resid(uio) > (user_ssize_t)(sizeof(address) + QRTR_WRITE_MAX)) {
		return EINVAL;
	}
	length = (uint32_t)uio_resid(uio) - (uint32_t)sizeof(address);
	if ((error = uiomove((caddr_t)address, sizeof(address), uio)) != 0) {
		return error;
	}
	if (!glink.channel[CHANNEL_QRTR].open || qrtr_peer == 0) {
		return ENOTCONN;
	}
	if (address[0] != qrtr_peer) {
		return EHOSTUNREACH;
	}
	if (length == 0) {
		return EINVAL;
	}
	data = kalloc_data(length, Z_WAITOK);
	if (data == NULL) {
		return ENOMEM;
	}
	error = uiomove((caddr_t)data, (int)length, uio);
	if (error == 0) {
		error = qrtr_out_packet(QRTR_DATA, QRTR_PORT_BASE + n, address[0], address[1], data, length);
	}
	kfree_data(data, length);
	if (error == 0) {
		qport[n].sent++;
	}
	return error;
}

static int
qport_read(dev_t dev, struct uio *uio, int ioflag)
{
	unsigned n = (unsigned)minor(dev);
	struct qrtr_datagram *datagram;
	uint32_t address[2];
	int error = 0;

	if (n >= QRTR_PORTS || uio_resid(uio) < (user_ssize_t)sizeof(address)) {
		return EINVAL;
	}
	lck_mtx_lock(&qport_lock);
	while ((datagram = qport[n].first) == NULL) {
		if (ioflag & IO_NDELAY) {
			lck_mtx_unlock(&qport_lock);
			return EWOULDBLOCK;
		}
		error = msleep(&qport[n], &qport_lock, PCATCH | PZERO, "qrtr", NULL);
		if (error != 0) {
			lck_mtx_unlock(&qport_lock);
			return error;
		}
	}
	qport[n].first = datagram->next;
	qport[n].last = qport[n].first == NULL ? NULL : qport[n].last;
	qport[n].queued -= datagram->length;
	lck_mtx_unlock(&qport_lock);
	address[0] = datagram->node;
	address[1] = datagram->port;
	error = uiomove((caddr_t)address, sizeof(address), uio);
	if (error == 0) {
		error = uiomove((caddr_t)datagram->data, (int)MIN((user_ssize_t)datagram->length, uio_resid(uio)), uio);
	}
	kfree_data(datagram, sizeof(*datagram) + datagram->length);
	return error;
}

static int
qport_select(dev_t dev, int which, void *wql, proc_t p)
{
	unsigned n = (unsigned)minor(dev);
	int ready = 1;

	if (which == FREAD && n < QRTR_PORTS) {
		lck_mtx_lock(&qport_lock);
		ready = qport[n].first != NULL;
		if (!ready) {
			selrecord(p, &qport[n].select, wql);
		}
		lck_mtx_unlock(&qport_lock);
	}
	return ready;
}

static int
qport_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int flag, __unused proc_t p)
{
	struct qrtr_lookup *lookup = (struct qrtr_lookup *)data;

	if (cmd != QRTR_LOOKUP) {
		return ENOTTY;
	}
	lookup->node = lookup->port = 0;
	for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
		if (qrtr_servers[i].there && qrtr_servers[i].service == lookup->service &&
		    (lookup->instance == 0 || (qrtr_servers[i].instance >> 8) == lookup->instance)) {
			lookup->instance = qrtr_servers[i].instance >> 8;
			lookup->node = qrtr_servers[i].node;
			lookup->port = qrtr_servers[i].port;
			break;
		}
	}
	return 0;
}

static const struct cdevsw qport_cdevsw = {
	.d_open       = qport_open,
	.d_close      = qport_close,
	.d_read       = qport_read,
	.d_write      = qport_write,
	.d_ioctl      = qport_ioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = qport_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = 0,
};

static void
qport_init(void)
{
	qport_major = cdevsw_add(-1, &qport_cdevsw);
	if (qport_major < 0) {
		printf("SWEET QRTR: cdevsw_add failed (%d)\n", qport_major);
		return;
	}
	void *node = devfs_make_node_clone(makedev(qport_major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, qport_clone,
	    "redwood-qrtr");
	printf("SWEET QRTR: /dev/redwood-qrtr %s (major %d, ports %#x-%#x)\n", node ? "ready" : "FAILED", qport_major,
	    QRTR_PORT_BASE, QRTR_PORT_BASE + QRTR_PORTS - 1);
}

static int
sysctl_sweet_adsp_services SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char text[64 * 28];
	int at = 0;

	text[0] = 0;
	for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]) && at < (int)sizeof(text) - 28; i++) {
		if (qrtr_servers[i].there) {
			at += snprintf(text + at, sizeof(text) - (size_t)at, "%s%u:%u@%u:%u", at ? "," : "", qrtr_servers[i].service,
			    qrtr_servers[i].instance >> 8, qrtr_servers[i].node, qrtr_servers[i].port);
		}
	}
	return SYSCTL_OUT(req, text, strlen(text) + 1);
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_adsp_services, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, sysctl_sweet_adsp_services, "A", "SWEET ADSP: QRTR services the DSP has told of, svc:inst@node:port");

/* poke f N [A]: see NOTES.md (0950, 0951, 0956) */
int sweet_adsp_poke(unsigned what, uint64_t a, uint64_t b);
int
sweet_adsp_poke(unsigned what, __unused uint64_t a, __unused uint64_t b)
{
	struct smp2p words;
	unsigned services = 0;

	switch (what) {
	case 0:
		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			services += qrtr_servers[i].there;
		}
		printf("SWEET FASTRPC: status: DSP %s%s, link %s; channels: IPCRTR %s, apr %s, fastrpc %s, glink_ssr %s; device %s (pid %d); "
		    "%u sent, %u answers (%u waiting), %u dropped, %u refused; SMMU CB%d, sessions %d%d%d; QRTR node %u, %u services\n",
		    dsp_started ? "started" : "NOT STARTED", fastrpc_dsp_gone ? " BUT GONE" : "", link_runs ? "up" : "down",
		    glink.channel[CHANNEL_QRTR].open ? "open" : "-", glink.channel[CHANNEL_APR].open ? "open" : "-",
		    glink.channel[CHANNEL_FASTRPC].open ? "open" : "-", glink.channel[CHANNEL_SSR].open ? "open" : "-",
		    fastrpc.open ? "open" : "closed", fastrpc.pid, fastrpc.sent, fastrpc.answered, fastrpc.head - fastrpc.tail,
		    fastrpc.dropped, fastrpc.refused, (int)fastrpc.cb - 1, fastrpc.identity[0], fastrpc.identity[1], fastrpc.identity[2],
		    qrtr_peer, services);
		return 0;
	case 1:
		if (dsp_words(&words)) {
			show_words("the DSP's SMP2P words", &words);
		}
		show_crash_reason();
		return 0;
	case 2:
		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			if (qrtr_servers[i].there) {
				printf("SWEET QRTR: service %u (%#x) version %u instance %u at node %u port %u\n", qrtr_servers[i].service,
				    qrtr_servers[i].service, qrtr_servers[i].instance & 0xff, qrtr_servers[i].instance >> 8,
				    qrtr_servers[i].node, qrtr_servers[i].port);
			}
		}
		for (unsigned n = 0; n < QRTR_PORTS; n++) {     /* 0956 */
			if (qport[n].open) {
				printf("SWEET QRTR: our port %#x (pid %d): %u sent, %u received, %u dropped, %u bytes waiting\n",
				    QRTR_PORT_BASE + n, qport[n].pid, qport[n].sent, qport[n].received, qport[n].dropped, qport[n].queued);
			}
		}
		return 0;
	case 3:
		return fastrpc_smmu_identity((uint32_t)a);
	case 4:
		/* 0951: pd-mapper announced again (to the DSP's QRTR node, or node 5 before its hello) */
		pdm_announce(qrtr_peer ? qrtr_peer : 5, QRTR_CONTROL);
		printf("SWEET PDMAP: %s, %u requests, announced %u times\n", pdm_on ? "on" : "OFF (adsp_pdm=0)", pdm_requests,
		    pdm_announced);
		return 0;
	case 5:
		fastrpc_smmu_faults();
		return 0;
	default:
		printf("SWEET FASTRPC: poke f: 0 status, 1 the DSP's words and last words, 2 QRTR services, 3 SID: SMMU identity, "
		    "4 announce pd-mapper again, 5 SMMU faults\n");
		return EINVAL;
	}
}


static void
link_thread(__unused void *parameter, __unused wait_result_t result)
{
	struct smp2p words, last;
	struct glink_packet *packet;
	bool gave_up = false;

	bzero(&last, sizeof(last));
	dsp_words(&last);
	while (link_wanted) {
		sleep_us(1000);
		doorbells();
		if (dsp_words(&words) && memcmp(&words, &last, sizeof(words)) != 0) {
			last = words;
			dsp_listen(&words);
			if ((dsp_state(&words) & DSP_FATAL) && !gave_up) {
				gave_up = true;
				printf("adsp: link: THE AUDIO DSP SAYS IT HAS GIVEN UP (\"slave-kernel\" = %#x)\n", dsp_state(&words));
				show_crash_reason();
				fastrpc_dsp_down("it has given up");
			}
		}
		for (;;) {
			unsigned from = CHANNELS;

			lck_mtx_lock(&glink_lock);
			glink_pump();
			packet = NULL;
			for (unsigned c = 0; c < CHANNELS && packet == NULL; c++) {
				struct glink_channel *channel = &glink.channel[c];

				if ((packet = channel->first) != NULL) {
					channel->first = packet->next;
					channel->last = channel->first == NULL ? NULL : channel->last;
					from = c;
				}
			}
			lck_mtx_unlock(&glink_lock);
			if (packet == NULL) {
				break;
			}
			if (from == CHANNEL_QRTR) {
				qrtr_input(packet->data, packet->length);
			} else if (from == CHANNEL_PMIC) {
				battmgr_input(packet->data, packet->length);
			} else if (from == CHANNEL_FASTRPC) {
				fastrpc_input(packet->data, packet->length);
			} else if (from == CHANNEL_SSR) {
				ssr_input(packet->data, packet->length);
			} else {
				apr_input(packet->data, packet->length);
			}
			kfree_data(packet, sizeof(*packet) + packet->length);
		}
	}
	link_runs = false;
	thread_terminate(current_thread());
}

static void
link_stop(void)
{
	struct glink_packet *packet;

	if (link_runs) {
		link_wanted = false;
		for (unsigned ms = 0; ms < 2000 && link_runs; ms++) {
			sleep_ms(1);
		}
		printf("adsp: link: %s; GLINK said %u and heard %u messages, QRTR sent %u and received %u datagrams, APR %u and %u "
		    "packets; its doorbells heard: GLINK %u, SMP2P %u\n", link_runs ? "THE THREAD DOES NOT END" : "down", glink.said,
		    glink.heard, qrtr_out, qrtr_in, apr_out, apr_in, doorbell.glink, doorbell.smp2p);
	}
	lck_mtx_lock(&glink_lock);
	glink.up = false;
	for (unsigned c = 0; c < CHANNELS; c++) {
		struct glink_channel *channel = &glink.channel[c];

		channel->open = false;
		while ((packet = channel->first) != NULL) {
			channel->first = packet->next;
			kfree_data(packet, sizeof(*packet) + packet->length);
		}
		channel->last = NULL;
	}
	lck_mtx_unlock(&glink_lock);
	bzero(qrtr_servers, sizeof(qrtr_servers));
	qrtr_peer = 0;
	fastrpc_dsp_down("the link is down");
}

/* ---- step 8: GLINK's queues, looked at -------------------------------------------------------------- */

static bool
step_queues(void)
{
	struct glink_message message;
	uint32_t head, tail;

	announce("find GLINK's descriptor and the two queues, and read what the audio DSP has put into its own");
	if (!glink_items_make()) {
		return false;
	}
	if (!glink_find()) {
		printf("adsp: %u: GLINK's items are not all there, or not of a shape this knows (descriptor %#x, ours %#x of %u bytes, "
		    "the DSP's %#x of %u)\n", step, glink.descriptor, glink.ours, glink.ours_size, glink.theirs, glink.theirs_size);
		return false;
	}
	head = smem_word(glink.descriptor + GLINK_THEIR_HEAD);
	tail = smem_word(glink.descriptor + GLINK_THEIR_TAIL);
	printf("adsp: %u: our queue: %u bytes at %#lx, written to %#x, read by the DSP to %#x; the DSP's: %u bytes at %#lx, written "
	    "to %#x, read by us to %#x\n", step, glink.ours_size, SMEM_PHYS + glink.ours, smem_word(glink.descriptor + GLINK_OUR_HEAD),
	    smem_word(glink.descriptor + GLINK_OUR_TAIL), glink.theirs_size, SMEM_PHYS + glink.theirs, head, tail);
	for (uint32_t at = 0, shown = 0; at + sizeof(message) <= head && head < glink.theirs_size && tail == 0 && shown < 8; shown++) {
		char name[33] = "";
		uint32_t length = sizeof(message);

		glink_peek(&message, at, sizeof(message));
		if (message.command == GLINK_OPEN) {
			glink_peek(name, at + 8, message.param2 < 32 ? message.param2 : 32);
			length += message.param2;
		}
		printf("adsp: %u:   in the DSP's queue at %#x: %s %u %#x %s\n", step, at, glink_name(message.command), message.param1,
		    message.param2, name);
		if (message.command != GLINK_VERSION && message.command != GLINK_OPEN) {
			break;
		}
		at += (length + 7) & ~7u;
	}
	return true;
}

/* ---- step 9: GLINK, spoken -------------------------------------------------------------------------- */

static bool
step_link(void)
{
	thread_t thread;
	uint64_t began;

	if (!dsp_started || glink.descriptor == 0) {
		return false;
	}
	announce("take up GLINK with the audio DSP: our version, then whatever its messages call for; a thread of ours listens "
	    "from here on, every millisecond");
	snprintf(last_write, sizeof(last_write), "step %u took up GLINK", step);
	lck_mtx_lock(&glink_lock);
	glink.features = GLINK_INTENT_REUSE;
	glink.agreed = glink.broken = false;
	glink.said = glink.heard = 0;
	for (unsigned c = 0; c < CHANNELS; c++) {
		struct glink_channel *channel = &glink.channel[c];

		channel->asked = channel->acknowledged = channel->open = false;
		channel->theirs = channel->our_intent_count = 0;
		bzero(channel->their_intents, sizeof(channel->their_intents));
	}
	apr_in = apr_out = 0;
	audio_up = false;
	bzero(glink.others, sizeof(glink.others));
	glink.trace = 60;
	glink.up = true;
	qrtr_hellos = qrtr_in = qrtr_out = qrtr_lookups = 0;
	doorbell.since = began = now_us();
	glink_say(GLINK_VERSION, GLINK_VERSION_1, glink.features, NULL, 0);
	lck_mtx_unlock(&glink_lock);
	link_wanted = link_runs = true;
	if (kernel_thread_start(link_thread, NULL, &thread) != KERN_SUCCESS) {
		link_wanted = link_runs = false;
		printf("adsp: %u: no thread\n", step);
		return false;
	}
	thread_deallocate(thread);
	for (unsigned ms = 0; ms < 5000 && !glink.channel[CHANNEL_QRTR].open && !glink.broken; ms++) {
		sleep_ms(1);
	}
	sleep_ms(50);
	printf("adsp: %u: %llu ms after our version: versions %s (features %#x); the channel \"IPCRTR\" is %s, \"apr_audio_svc\" %s; "
	    "the DSP also opened \"%s\" \"%s\" \"%s\" \"%s\"\n", step, (now_us() - began) / 1000, glink.agreed ? "agreed" :
	    "NOT AGREED", glink.features, glink.channel[CHANNEL_QRTR].open ? "OPEN" : "NOT OPEN",
	    glink.channel[CHANNEL_APR].open ? "OPEN" : "NOT OPEN", glink.others[0], glink.others[1], glink.others[2], glink.others[3]);
	return glink.channel[CHANNEL_QRTR].open;
}

/* ---- step 10: QRTR ---------------------------------------------------------------------------------- */

static bool
step_qrtr(void)
{
	unsigned services = 0;

	if (!glink.channel[CHANNEL_QRTR].open) {
		return false;
	}
	PE_parse_boot_argn("adsp_listen", &listen_seconds, sizeof(listen_seconds));
	announce("listen to QRTR on the channel: the DSP's hello (answered from the link's thread), and the services it tells of");
	for (unsigned ms = 0; ms < listen_seconds * 1000; ms++) {
		sleep_ms(1);
	}
	for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
		services += qrtr_servers[i].there;
	}
	printf("adsp: %u: after %u s: %s; the DSP is node %u and has told of %u services; it looked for %u of ours; QRTR received %u "
	    "and sent %u datagrams\n", step, listen_seconds, qrtr_hellos ? "HELLO BOTH WAYS" : "NO HELLO FROM THE DSP", qrtr_peer,
	    services, qrtr_lookups, qrtr_in, qrtr_out);
	return qrtr_hellos != 0;
}

/* ---- step 11: APR ---------------------------------------------------------------------------------- */

static bool
step_apr(void)
{
	uint32_t wait_seconds = 10, asked = 0, state = 0;
	uint64_t began = now_us();

	PE_parse_boot_argn("adsp_apr_wait", &wait_seconds, sizeof(wait_seconds));
	announce("ask the audio DSP's core service over APR whether the audio services are up (again and again, as Android "
	    "does), then which services it has");
	for (unsigned ms = 0; ms < 3000 && !glink.channel[CHANNEL_APR].open; ms++) {
		sleep_ms(1);
	}
	if (!glink.channel[CHANNEL_APR].open) {
		printf("adsp: %u: the channel \"apr_audio_svc\" is not open: the DSP has not opened it\n", step);
		return false;
	}
	while (now_us() - began < wait_seconds * 1000000ull) {
		asked++;
		if (apr_ask(APR_SVC_ADSP_CORE, AVCS_CMD_ADSP_EVENT_GET_STATE, NULL, 0, 500) &&
		    apr_answer.opcode == AVCS_CMDRSP_ADSP_EVENT_GET_STATE) {
			state = apr_answer.words[0];
			if (state == 1) {
				break;
			}
		}
		sleep_ms(200);
	}
	audio_up = state == 1;
	printf("adsp: %u: %s after %llu ms and %u questions (the last answer: opcode %#x, %u bytes, %#x)\n", step, state == 1 ?
	    "THE AUDIO SERVICES ARE UP" : "THE AUDIO SERVICES DO NOT SAY THEY ARE UP", (now_us() - began) / 1000, asked,
	    apr_answer.opcode, apr_answer.length, apr_answer.words[0]);
	if (apr_ask(APR_SVC_ADSP_CORE, AVCS_GET_VERSIONS, NULL, 0, 1000)) {
		char line[400];
		int at = 0;

		for (uint32_t i = 0; i < apr_answer.length / 4 && i < 64 && at < (int)sizeof(line) - 12; i++) {
			at += snprintf(line + at, sizeof(line) - (size_t)at, " %#x", apr_answer.words[i]);
		}
		printf("adsp: %u: the services and their versions (opcode %#x, %u bytes):%s\n", step, apr_answer.opcode,
		    apr_answer.length, line);
	} else {
		printf("adsp: %u: no answer to the question of the services' versions\n", step);
	}
	return state == 1;
}

/* ---- the steps, and their end ----------------------------------------------------------------------- */

static bool (*const steps[STEPS])(void) = {
	step_survey, step_firmware, step_mailbox, step_header, step_image, step_our_item, step_start, step_queues, step_link,
	step_qrtr, step_apr,
};

static bool keep;                       /* the DSP and the link stay up while the system runs (adsp_keep, adsp=1) */

static void
steps_over(uint32_t done)
{
	lck_mtx_lock(&dsp_lock);
	if (dsp_started && done == last_step && keep) {
		printf("adsp: THE AUDIO DSP STAYS ON while the system runs%s\n", link_runs ? ", and the link with it" : "");
	} else if (dsp_started) {
		dsp_shutdown("the steps are over");
	}
	lck_mtx_unlock(&dsp_lock);
}

/* When the run ends in order: a DSP that runs is shut down, for the next kernel to start again. */
void
redwood_adsp_stop(void)
{
	battmgr_stop();
	redwood_sound_dsp_down();               /* the amplifiers, the stream and the port first, while the DSP still listens */
	redwood_mic_dsp_down();                 /* and what the microphones' path has asked for */
	lck_mtx_lock(&dsp_lock);
	if (dsp_started) {
		unsigned services = 0;
		uint32_t node = qrtr_peer;
		bool down;

		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			services += qrtr_servers[i].there;
		}
		down = dsp_shutdown("the run is about to end");
		/* (said again at the end: a long run's log keeps its beginning and its end, and the middle may be gone) */
		tell("adsp: the audio DSP is off: it had been up%s (node %u, %u services; audio services %s), it %s, and TrustZone %s\n",
		    node ? " and linked" : ", NOT LINKED", node, services, audio_up ? "up" : "NOT UP", stopped_by_its_word ?
		    "stopped when asked" : "DID NOT SAY IT HAD STOPPED", down ? "has shut it down" : "DID NOT SHUT IT DOWN");
	}
	lck_mtx_unlock(&dsp_lock);
}

/* The same on a panic's way back to the loader: one processor, interrupts off, no lock, no sleep. */
void
redwood_adsp_quiesce(void)
{
	redwood_sound_quiesce();                /* the amplifiers off, whatever the DSP's state */
	if (!dsp_started) {
		return;
	}
	leaving = true;
	new_from = ~0u;
	dsp_shutdown("the kernel is on its way back to the loader");
}

static void
run_steps(void)
{
	uint32_t done = 0;

	for (step = 1; step <= last_step; step++) {
		if (!steps[step - 1]()) {
			printf("adsp: step %u: FAILED; the steps after it are not attempted\n", step);
			break;
		}
		printf("adsp: step %u: done\n", step);
		done = step;
	}
	step = done;
	new_from = STEPS + 1;
	steps_over(done);
	printf("adsp: over after step %u; %s\n", done, last_write);
}

static void
whole_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint64_t began = now_us();
	unsigned services = 0;

	/* (the firmware may be on the root file system, which is not mounted yet when this begins: a minute at most) */
	for (unsigned waited = 0; waited < 300 && rootvnode == NULLVP; waited++) {
		sleep_ms(200);
	}
	run_steps();
	for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
		services += qrtr_servers[i].there;
	}
	if (step == STEPS && dsp_started) {
		tell("adsp: THE AUDIO DSP IS UP AND LINKED, its audio services up (APR), %llu ms after the kernel began on it (the "
		    "root's waited for included): node %u, %u services; it stays on\n", (now_us() - began) / 1000, qrtr_peer, services);
		redwood_sound_dsp_up();         /* (boot-arg sound=N: the speakers' path, redwood_sound.c) */
		redwood_mic_dsp_up();           /* (boot-arg mic=N: the microphones' path, redwood_mic.c) */
		battmgr_start();                /* (how the battery is, from the DSP; battmgr=0: not asked) */
	} else {
		tell("adsp: THE AUDIO DSP DID NOT COME UP: step %u failed (%s); see the kernel's own log for the steps\n", step + 1,
		    last_write);
	}
	thread_terminate(current_thread());
}

/* Called while the system starts (bsd/dev/redwood_input.c), after the Wi-Fi's start has been set going. */
void
redwood_adsp_init(void)
{
	uint32_t on = 0;
	thread_t thread;

	PE_parse_boot_argn("adsp_step", &last_step, sizeof(last_step));
	if (last_step == 0 && (!PE_parse_boot_argn("adsp", &on, sizeof(on)) || (on != 1 && on != 2))) {
		return;
	}
	PE_parse_boot_argn("adsp_sleep", &adsp_sleep, sizeof(adsp_sleep));    /* 0950: Android's SMP2P entries */
	PE_parse_boot_argn("adsp_qlog", &adsp_qlog, sizeof(adsp_qlog));       /* 0950: IPCRTR datagrams logged */
	fastrpc_init();                         /* 0950: /dev/redwood-fastrpc (adsp_fastrpc=0: not) */
	qport_init();                           /* 0956: /dev/redwood-qrtr */
	PE_parse_boot_argn("adsp_pdm", &pdm_on, sizeof(pdm_on));       /* 0951: pd-mapper (adsp_pdm=0: not) */
	if (last_step == 0) {
		/* all of it, on a thread of its own, and left up */
		last_step = STEPS;
		keep = true;
		quiet = true;
		pause_ms = 0;
		listen_seconds = 1;
		new_from = STEPS + 1;
		if (on == 2 && fastrpc_mode != 0) {
			/* adsp=2: when fastrpcd first opens /dev/redwood-fastrpc (fastrpc_open), so the sensors PD finds its files */
			fastrpc_start_on_open = true;
			tell("adsp: the audio DSP will be brought up when /dev/redwood-fastrpc is first opened (adsp=2)\n");
			return;
		}
		if (kernel_thread_start(whole_thread, NULL, &thread) != KERN_SUCCESS) {
			printf("adsp: no thread: the audio DSP is not brought up\n");
			return;
		}
		thread_deallocate(thread);
		tell("adsp: the audio DSP is being brought up beside the system's start (adsp=%u)\n", on);
		return;
	}
	if (last_step > STEPS) {
		last_step = STEPS;
	}
	keep = option("adsp_keep");
	new_from = last_step;
	PE_parse_boot_argn("adsp_new", &new_from, sizeof(new_from));
	PE_parse_boot_argn("adsp_pause", &pause_ms, sizeof(pause_ms));
	printf("adsp: steps up to %u; those from %u on are new and wait %u ms after each announcement\n", last_step, new_from,
	    pause_ms);
	/* redfin: on a thread too, once the root (the firmware) is there */
	if (kernel_thread_start(whole_thread, NULL, &thread) != KERN_SUCCESS) {
		printf("adsp: no thread: the audio DSP is not brought up\n");
		return;
	}
	thread_deallocate(thread);
}

/* adsp=2: the first open of /dev/redwood-fastrpc starts the steps (all of them, left up), as adsp=1 does at boot. */
static void
fastrpc_start_dsp(void)
{
	thread_t thread;

	if (kernel_thread_start(whole_thread, NULL, &thread) != KERN_SUCCESS) {
		printf("adsp: no thread: the audio DSP is not brought up (adsp=2)\n");
		return;
	}
	thread_deallocate(thread);
	tell("adsp: the audio DSP is being brought up: /dev/redwood-fastrpc was opened (adsp=2)\n");
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
