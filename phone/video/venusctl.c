/*
 * venusctl: video stage 2 (docs/features/video.md "Stage 2 spec"), the Venus/IRIS1 video engine from userland
 * through /dev/pmem, as adspboot does for the ADSP. Facts: research/notes/venus-stage1-2026-11-02.md (DT lines,
 * downstream videocc-sdmmagpie.c in research/ext/msm-vidc-sunfish/), PLL/RCG/branch sequences as phone/tools/camclk.c.
 * Video 4a: the code below main() lives in venus_core.c (+ venus_h264.c), shared with the daemon venusd; output unchanged.
 *   venusctl probe          read-only: GCC video CBCRs, TBU sf vote/status, apps SMMU SMRs for SIDs 0x1080-0x10e0,
 *                           cmd-db MM3/MM2; videocc (efuse, PLL0, RCG, GDSCs, CBCRs) only with gcc_video_ahb on.
 *                           Never reads the core (0x0aa00000).
 *   venusctl power on [--cvp]   BCM MM3 + MM2 minimal votes (no MX/CX), gcc_video_ahb/axi, PLL0 480 MHz, iris RCG /2
 *                           = 240 MHz, GDSC mvsc then mvs0 (--cvp: + mvs1), branches; then WRAPPER_HW_VERSION
 *   venusctl power off [--cvp] [--force]  the reverse; gcc_video_ahb stays on (critical in mainline gcc-sm7150),
 *                           votes last; its HW_CTL bit goes back if power on cleared it (state in AHB_STATE).
 *                           Refuses while STATE says pas 1 (firmware may run: venusctl shutdown first) unless --force.
 *   venusctl pas-check [--fw DIR] [--no-shutdown]  needs power on: PAS is_supported(9), init_image(9, venus.mdt
 *                           metadata), shutdown(9). No mem_setup, no auth_and_reset. --no-shutdown: STATE pas 1.
 *   venusctl boot [--fw DIR] [--keep]  needs power on: exact SMRs for SIDs 0x1080/0x10a0/0x10c0/0x10e0 (smmu.c
 *                           smmu_identity, mask 0), CPU_CGC_DIS/CPU_CLOCK_CONFIG = 0, PAS init_image(9), mem_setup,
 *                           segments to 0x94800000, auth_and_reset(9), memory protect 0x42000c08, status reads.
 *                           Without --keep: PAS shutdown(9) and the SMRs go again. What it did: STATE.
 *   venusctl init [--phys ADDR]  after boot --keep: 3 MB HFI queue block (PMEM_ALLOC32, else only with --phys, 1 MB
 *                           aligned in 0xa5100000-0xbcc00000) with the queue table, 3 queues and the SFR page, the
 *                           UC/QTBL/SFR/VMIMSG registers, CTRL_INIT = 1, poll CTRL_STATUS (1 s)
 *   venusctl ping [--debug MASK]  after init: SYS_INIT -> INIT_DONE, debug to the debug queue, IMAGE_VERSION,
 *                           SYS_PING -> PING_ACK; polls the message queue (no IRQ), prints every packet. No
 *                           PING_ACK is a note, not a failure (v2c; VENUSCTL_PING_FATAL=1: failure)
 *   venusctl session [--debug MASK]  after init: as ping up to IMAGE_VERSION, then SYS_SESSION_INIT (H.264 decoder)
 *                           -> SESSION_INIT_DONE, SYS_SESSION_END -> SESSION_END_DONE (v2c)
 *   venusctl decode IN.h264 OUT.yuv [--frames N] [--bufphys ADDR] [--debug MASK]  stage 3, after init: as session,
 *                           then the downstream decoder sequence (split mode: UBWC DPB on OUTPUT, linear NV12 on
 *                           OUTPUT2), one access unit per ETB, FTBs, stops after N frames (5); writes the frames
 *                           cropped to the visible size as plain NV12 to OUT, prints per frame FBD flags, filled
 *                           length, timestamp, luma CRC32; then flush, stop, release, session end. Buffers in a window
 *                           after the --phys queue block (or --bufphys). Off switches/knobs: VENUSCTL_PRIMARY=1 (linear
 *                           on OUTPUT, no multi-stream), VENUSCTL_DPB_SETBUF=1, VENUSCTL_ROUTE, VENUSCTL_NOUT,
 *                           VENUSCTL_YSTRIDE/YSCAN, VENUSCTL_BUF_MB (64)
 *                           4e: IN may be HEVC (Annex B, 8-bit 4:2:0 Main; Main10/4:2:2 refused), told apart by its
 *                           first NAL header; --hevc / --h264 force the codec (--h264: as before 4e)
 *   venusctl shutdown       undoes boot --keep/init from STATE: PAS shutdown(9), queue memory, SMRs. Then power off.
 *   venusctl memtest ADDR SIZE write|check  the --phys region check (pre-run review 8.1): fill ADDR..ADDR+SIZE through
 *                           /dev/pmem (as init) with addr ^ 0x5a5a5a5a per word, or count the words that changed
 * Pre-run review fixes (notes section 8.5): STATE and AHB_STATE carry the boot session id; a file from an earlier
 * boot is dropped unread (VENUSCTL_NO_BOOTID=1: keep the old behaviour). smmu release only of a slot that still reads
 * our exact SMR and S2CR; it counts as done once the SMR reads invalid, other differences are warnings (v2c;
 * VENUSCTL_STRICT_RELEASE=1: any difference fails). init/ping re-check the mnoc_sf_0_tbu vote before CTRL_INIT and each doorbell.
 *   -v                      print every register write (address, old, new, read-back)
 * redfin (Pixel 5, SM7250 lito; track V): ported from Yofuness/ios-flame 598ba5b, SoC numbers in venus_core.c's header.
 * Read the SoC names above as lito's: SIDs 0x1380/0x13a0/0x13c0/0x13e0 + 0x1360, firmware at 0x86a00000, video_pll0
 * Lucid, queues/buffers in 0xa2600000-0xb7e00000 (init needs --phys: this kernel has no PMEM_ALLOC32; use 0xa2600000,
 * the buffer window then follows at 0xa2a00000, xnu 1057's sweetvenus window). Firmware: /var/local/fw/venus/venus.{mdt,
 * b00-b04} (copied from the phone's /vendor/firmware, never in git), or /vendor/firmware if mounted.
 * Writes only: GCC video_ahb/video_axi CBCRs, the videocc PLL0/RCG/GDSC/CBCR registers below, RPMh BCMs MM3/MM2,
 * the core registers in allowed() (boot/init/ping), the firmware carveout, the queue block; SMMU writes only through
 * smmu_identity/smmu_release (one exact SMR + S2CR + translation-off bank per SID, never masked: masked SMRs reset
 * the phone in camera tests). Every videocc access is gated on gcc_video_ahb CLK_OFF = 0, every core access on the
 * GDSCs + clocks (core_ok). Polls time out (100 ms; 1 s for the firmware) and say where they stopped. lat2: every
 * timeout is CLOCK_MONOTONIC time and the lines print the real ms (a throttled 1 ms sleep lasted ~14 ms, video.md
 * lat1); PMEM_PASS_WAITS=1: loop passes count as ms, as before.
 * Off switches: VENUSCTL_SID1=1 (only 0x1080), VENUSCTL_NO_CLKCFG=1 (no CPU_CGC_DIS/CLOCK_CONFIG writes),
 * VENUSCTL_NO_MEMPROT=1 (no memory protect call).
 * gcc_video_ahb boots enabled but hardware-gated (0x80000003: HW_CTL set, CLK_OFF reads 1 while idle; Linux
 * clk-branch.c skips the halt check in that mode). power on clears HW_CTL first so CLK_OFF can be polled;
 * VENUSCTL_KEEP_HWCTL=1 skips that (old behaviour).
 */
#include <stdint.h>
#include <unistd.h>
#include <sys/time.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "pmem.h"
#include "venus_core.h"

/* VENUSCTL_TS=1: every output line gets "[seconds since start] " (a pipe + reader thread), to place stalls in time; the
 * host can't, since ssh delivers the output in bursts (the 13 s first-use stall, video.md v4a1/vdboot) */
static int ts_out = -1;
static struct timeval ts_t0;
static void *ts_reader(void *arg)
{
	FILE *in = fdopen((int)(intptr_t)arg, "r");
	char line[4096];
	while (in && fgets(line, sizeof line, in)) {
		struct timeval t;
		gettimeofday(&t, NULL);
		dprintf(ts_out, "[%8.3f] %s", (t.tv_sec - ts_t0.tv_sec) + (t.tv_usec - ts_t0.tv_usec) / 1e6, line);
	}
	return NULL;
}
static pthread_t ts_thread;
static void ts_flush(void)
{
	fflush(stdout);
	close(1);
	pthread_join(ts_thread, NULL);
}
static void ts_start(void)
{
	int p[2];
	if (!getenv("VENUSCTL_TS") || !atoi(getenv("VENUSCTL_TS")) || pipe(p))
		return;
	gettimeofday(&ts_t0, NULL);
	ts_out = dup(1);
	dup2(p[1], 1);
	close(p[1]);
	pthread_create(&ts_thread, NULL, ts_reader, (void *)(intptr_t)p[0]);
	atexit(ts_flush);
}

int main(int argc, char **argv)
{
	const char *fw = NULL, *cmd = NULL, *arg = NULL, *arg2 = NULL, *arg3 = NULL;
	int no_shutdown = 0, keep = 0, frames = 5, hevc = -1;	/* 4e: -1 = from the stream */
	uint64_t phys = 0, bufphys = 0;
	uint32_t debug = 0x18;	/* HFI_DEBUG_MSG_ERROR | FATAL (guess: msm_vidc_debug.c's default is not in our source copy) */
	setvbuf(stdout, NULL, _IOLBF, 0);	/* a hang shows the last line */
	ts_start();
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v"))
			verbose = 1;
		else if (!strcmp(argv[i], "--cvp"))
			cvp = 1;
		else if (!strcmp(argv[i], "--force"))
			force = 1;
		else if (!strcmp(argv[i], "--no-shutdown"))
			no_shutdown = 1;
		else if (!strcmp(argv[i], "--keep"))
			keep = 1;
		else if (!strcmp(argv[i], "--fw") && i + 1 < argc)
			fw = argv[++i];
		else if (!strcmp(argv[i], "--phys") && i + 1 < argc)
			phys = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
			frames = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--bufphys") && i + 1 < argc)
			bufphys = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--hevc"))
			hevc = 1;
		else if (!strcmp(argv[i], "--h264"))
			hevc = 0;
		else if (!strcmp(argv[i], "--debug") && i + 1 < argc)
			debug = (uint32_t)strtoul(argv[++i], NULL, 0);
		else if (!cmd)
			cmd = argv[i];
		else if (!arg)
			arg = argv[i];
		else if (!arg2)
			arg2 = argv[i];
		else if (!arg3)
			arg3 = argv[i];
		else
			goto usage;
	}
	if (!cmd)
		goto usage;
	if (!fw) {	/* the vendor partition's copy, else a copy under /var/local (out of git) */
		struct stat st;
		fw = stat("/vendor/firmware/venus.mdt", &st) == 0 ? "/vendor/firmware" : "/var/local/fw/venus";
	}
	pm_init();
	stale_drop(STATE);
	stale_drop(AHB_STATE);
	if (arg2 && strcmp(cmd, "memtest") && strcmp(cmd, "decode"))
		goto usage;
	if (!strcmp(cmd, "decode") && arg && arg2 && !arg3 && frames > 0)
		return decode(debug, arg, arg2, frames, bufphys, hevc);
	if (!strcmp(cmd, "memtest") && arg && arg2 && arg3 && (!strcmp(arg3, "write") || !strcmp(arg3, "check")))
		return memtest(strtoull(arg, NULL, 0), strtoull(arg2, NULL, 0), !strcmp(arg3, "write"));
	if (!strcmp(cmd, "probe") && !arg)
		return probe();
	if (!strcmp(cmd, "power") && arg && !strcmp(arg, "on"))
		return power_on();
	if (!strcmp(cmd, "power") && arg && !strcmp(arg, "off"))
		return power_off();
	if (!strcmp(cmd, "pas-check") && !arg)
		return pas_check(fw, no_shutdown);
	if (!strcmp(cmd, "boot") && !arg)
		return boot(fw, keep);
	if (!strcmp(cmd, "init") && !arg)
		return init(phys);
	if (!strcmp(cmd, "ping") && !arg)
		return ping(debug);
	if (!strcmp(cmd, "session") && !arg)
		return session(debug);
	if (!strcmp(cmd, "shutdown") && !arg)
		return shutdown_cmd();
usage:
	fprintf(stderr, "usage: venusctl [-v] probe | power on|off [--cvp] [--force] | pas-check [--fw DIR] [--no-shutdown]\n"
		"       | boot [--fw DIR] [--keep] | init [--phys ADDR] | ping [--debug MASK]\n"
		"       | session [--debug MASK] | decode IN.h264|IN.h265 OUT.yuv [--frames N] [--bufphys ADDR] [--debug MASK]\n"
		"         [--hevc|--h264]\n"
		"       | shutdown\n"
		"       | memtest ADDR SIZE write|check\n");
	return 2;
}
