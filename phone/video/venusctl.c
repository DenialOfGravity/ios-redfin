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
 *   --step [--step-file F]  redfin: every /dev/pmem write, SMC and RPMh vote, every HFI command and packet, buffer
 *                           allocations, decode stages, and (while waiting) core + SMMU global fault status every
 *                           500 ms, to F (default /var/local/venus-step.log), fsynced BEFORE each action: after a
 *                           reset the last line is the last thing done. The file is a fixed 4 MB, lines written in
 *                           place (F_FULLFSYNC each); read it with tr -d '\000'. Lines also go to /dev/console.
 * SMMU (redfin, rn4x): by default the 5 venus_ns SIDs go through ONE translating bank mapping only 0xa2600000-
 *                           0xaca00000 (IOVA = PA), tables at 0xa29fe000; any other address faults in that bank (FSR/
 *                           FAR printed at timeouts and in --step status). VENUSCTL_SMMU=identity: the old
 *                           translation-off banks (identity for 0x1360 reset the phone); =hybrid: identity for
 *                           the 0x1380 group, the window bank for 0x1360 only. boot prints an SMMU snapshot (SMRs
 *                           matching 0x1300-0x13ff, sGFSR) before, after mapping, after auth and after mem protect.
 *   venusctl smmu           that snapshot alone (read-only)
 *   venusctl watch SECS [--sysinit] [--debug MASK]  after init: CTRL/CPU/INTR status (watchdog bit), video NoC error
 *                           logger, SMMU global faults, GDSC/PLL, firmware queues, every 200 ms (with --step: console
 *                           + fsynced file); --sysinit first sends SYS_INIT + debug config + image version
 * power on (redfin): enables video_cc_xo (0x980) if it reads off (critical clock under Linux; VENUSCTL_XO=0: report)
 *   venusctl map-sid SID    after boot --keep: map one more SID now (VENUSCTL_SMMU mode, hybrid = window), recorded in
 *                           STATE; e.g. boot with VENUSCTL_NSID=4, init, then map-sid 0x1360
 *   decode ... --stop-at STAGE  redfin: stop after STAGE (sessinit props workmode intl alloc load start ftb), watch the
 *                           queues 2 s, close; --dry = --stop-at ftb (buffers set, START, FTBs, no input queued)
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
 * P0 (video-plan-2026-11-16, venus_rails.c; all default off):
 *   venusctl rails          read only: cmd-db cx.lvl/mx.lvl/mmcx.lvl (lito has no mmcx) and their level tables, the
 *                           apps DRV votes (kern.sweet_rpmh_votes, xnu 1131; TCS registers), the TBU vote, the
 *                           rails state file, and what VENUSCTL_RAILS would do. Writes nothing.
 *   VENUSCTL_RAILS=1|CORNER power on raises mx then cx to NOM (256) or CORNER first, raise only; power off puts the
 *                           earlier vote back (never 0; unknown before = top level voted and left). /var/tmp/venusctl.rails
 *   VENUSCTL_TBUVOTE=1      power on clears SW_COLLAPSE of the mnoc_sf_0_tbu hlos1 vote (GCC 0x17d054), waits PWR_ON;
 *                           power off sets it again if it was set
 *   watch SECS              without firmware (no STATE init) = power/bus only; a line at least every second with
 *                           the uptime, cx/mx votes and the TBU state
 * P0d (logs/research/video-reset-2026-11-16.md Q1/Q4; all default off, Linux = msm-4.19 techpack/video hfi_common.c):
 *   VENUSCTL_PC=1           watch, once SYS_INIT is done: after VENUSCTL_PC_DELAY_MS (1500 = pwr_collapse_delay) as
 *                           __prepare_pc_common: needs WFI + idle, sends HFI_CMD_SYS_PC_PREP (0x10002), waits up to 2 s
 *                           for CTRL_STATUS PC_READY (bit 8) + WRAPPER_CPU_STATUS WFI (bit 0), logs both with times.
 *                           No power off: clocks + GDSCs stay on (SW mode), no vote changes. Watch line: pc/pc_ready.
 *   VENUSCTL_INTCLR=1       watch: INTR_STATUS != 0 -> CPU_CS_A2HSOFTINTCLR = 1, WRAPPER_INTR_CLEAR = status (as
 *                           __core_clear_interrupt_common), then drain + decode the queues. INTR_MASK never written.
 *   venusctl fwdump [ADDR]  read-only, on the boot after a reset, before any re-init: the HFI block at ADDR
 *                           (0xa2600000): table still ours?, queue headers, SFR (+0x2fe000), dbgq ring strings,
 *                           unread msgq packets, failure strings; the core only if its gate reads on
 * ws-video-dec (logs/research/video-reset-2026-11-16.md): decode --clip FILE [--seconds S] [--loop] keeps an active
 *   H.264/HEVC decode session fed for S s of wall clock (default 900), looping the clip if --loop, to test whether an
 *   active session avoids the ~270 s idle PMIC reset. No YUV file, no per-frame log (NOCOPY + quiet); counts frames,
 *   fps, SYS/SESSION errors; prints "survived S s, phone alive" (absent if a reset killed us). As insurance a SYS_PING
 *   keepalive fires on any idle gap (VENUSCTL_PING_MS, default 1000 ms); VENUSCTL_INTCLR=1 also acks interrupts.
 *   watch SECS --sysinit with VENUSCTL_PING_MS=1000 is a ping-only keepalive test (no decode). init --phys first.
 * ws-venus-enc (logs/research/venus-encoder-2026-11-20.md): venusctl encode IN.yuv OUT WxH [--hevc] [--fps N]
 *   [--bitrate BPS] [--gop N] [--rc vbr|cbr|off] [--frames N] [--profile P --level L (HFI values)] [--bufphys A]
 *   [--stop-at sessinit|props|alloc|load|start|ftb]: after boot --keep + init, a Venus encoder session on tightly
 *   packed NV12 frames; the bitstream (Annex B, SPS/PPS before each IDR) goes to OUT. Code in venus_enc.inc.
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
#include "venus_rails.h"

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
	int sysinit = 0;	/* redfin: watch --sysinit */
	int no_shutdown = 0, keep = 0, frames = 5, hevc = -1;	/* 4e: -1 = from the stream */
	uint64_t phys = 0, bufphys = 0;
	const char *clip = NULL;	/* ws-video-dec: decode --clip FILE --seconds S --loop */
	int seconds = 0, do_loop = 0;
	struct venc_cfg ec = { .fps = 30, .gop = 30, .bitrate = 2000000 };	/* ws-venus-enc */
	const char *steplog = NULL;	/* redfin: --step [--step-file F] */
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
			frames = ec.frames = atoi(argv[++i]);	/* encode: a limit (default all) */
		else if (!strcmp(argv[i], "--bufphys") && i + 1 < argc)
			bufphys = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--step"))
			steplog = steplog ? steplog : "/var/local/venus-step.log";
		else if (!strcmp(argv[i], "--step-file") && i + 1 < argc)
			steplog = argv[++i];
		else if (!strcmp(argv[i], "--stop-at") && i + 1 < argc)
			vc_stop_at = argv[++i];
		else if (!strcmp(argv[i], "--sysinit"))
			sysinit = 1;
		else if (!strcmp(argv[i], "--dry"))
			vc_stop_at = "ftb";
		else if (!strcmp(argv[i], "--clip") && i + 1 < argc)
			clip = argv[++i];
		else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
			seconds = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--loop"))
			do_loop = 1;
		else if (!strcmp(argv[i], "--hevc"))
			hevc = 1;
		else if (!strcmp(argv[i], "--h264"))
			hevc = 0;
		else if (!strcmp(argv[i], "--fps") && i + 1 < argc)
			ec.fps = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--bitrate") && i + 1 < argc)
			ec.bitrate = (uint32_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--gop") && i + 1 < argc)
			ec.gop = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--rc") && i + 1 < argc) {
			i++;
			ec.rc = !strcmp(argv[i], "cbr") ? 1 : !strcmp(argv[i], "off") ? 2 : 0;
		} else if (!strcmp(argv[i], "--profile") && i + 1 < argc)
			ec.profile = (int)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--level") && i + 1 < argc)
			ec.level = (int)strtoul(argv[++i], NULL, 0);
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
	if (steplog) {
		if (pm_step_open(steplog))
			return 1;
		char cl[400];
		size_t o = 0;
		for (int i = 0; i < argc && o < sizeof(cl) - 1; i++)
			o += (size_t)snprintf(cl + o, sizeof(cl) - o, "%s%s", i ? " " : "", argv[i]);
		pm_step("venusctl: %s", cl);
		printf("venus: step log %s (fsync per line)\n", steplog);
	}
	stale_drop(STATE);
	stale_drop(AHB_STATE);
	if (arg2 && strcmp(cmd, "memtest") && strcmp(cmd, "decode") && strcmp(cmd, "encode"))
		goto usage;
	if (!strcmp(cmd, "encode") && arg && arg2 && arg3 && sscanf(arg3, "%dx%d", &ec.w, &ec.h) == 2 && ec.fps > 0) {
		ec.hevc = hevc == 1;
		ec.debug = debug;
		ec.bufphys = bufphys;
		return encode(&ec, arg, arg2);
	}
	if (!strcmp(cmd, "decode") && clip && !arg)
		return decode_loop(debug, clip, seconds, do_loop, bufphys, hevc);
	if (!strcmp(cmd, "decode") && arg && arg2 && !arg3 && frames > 0)
		return decode(debug, arg, arg2, frames, bufphys, hevc);
	if (!strcmp(cmd, "memtest") && arg && arg2 && arg3 && (!strcmp(arg3, "write") || !strcmp(arg3, "check")))
		return memtest(strtoull(arg, NULL, 0), strtoull(arg2, NULL, 0), !strcmp(arg3, "write"));
	if (!strcmp(cmd, "rails") && !arg)
		return rails_report();
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
	if (!strcmp(cmd, "watch") && arg)
		return watch((unsigned)atoi(arg), sysinit, debug);
	if (!strcmp(cmd, "map-sid") && arg)
		return map_sid((uint32_t)strtoul(arg, NULL, 0));
	if (!strcmp(cmd, "smmu") && !arg)
		return smmu_cmd();
	if (!strcmp(cmd, "fwdump"))
		return fwdump(arg ? strtoull(arg, NULL, 0) : 0);
	if (!strcmp(cmd, "hwctl") && arg && (!strcmp(arg, "on") || !strcmp(arg, "off")))
		return hwctl_cmd(!strcmp(arg, "on"));
	if (!strcmp(cmd, "intmask") && !arg)
		return intmask_cmd();
	if (!strcmp(cmd, "suspend") && !arg)
		return suspend_cmd();
	if (!strcmp(cmd, "resume") && !arg)
		return resume_cmd();
usage:
	fprintf(stderr, "usage: venusctl [-v] probe | rails (read-only) | power on|off [--cvp] [--force] | pas-check [--fw DIR] [--no-shutdown]\n"
		"       | boot [--fw DIR] [--keep] | init [--phys ADDR] | ping [--debug MASK]\n"
		"       | hwctl on|off (venus-hw: mvs0 GDSC HW_CONTROL, Linux's vcodec hand-off after boot) | intmask (unmask A2H+A2HWD) | suspend | resume (Linux power collapse)\n"
		"       | session [--debug MASK] | decode IN.h264|IN.h265 OUT.yuv [--frames N] [--bufphys ADDR] [--debug MASK]\n"
		"       | decode --clip FILE [--seconds S] [--loop]  (ws-video-dec: active session S s, idle-reset test)\n"
		"         [--hevc|--h264] [--stop-at sessinit|props|workmode|intl|alloc|load|start|ftb] [--dry]\n"
		"       any command: [--step] [--step-file FILE]  (default /var/local/venus-step.log)\n"
		"       | shutdown | smmu (read-only: SMRs for 0x1300-0x13ff, global faults) | map-sid SID (after boot --keep)\n"
		"       | watch SECONDS [--sysinit] [--debug MASK] (firmware/NoC/SMMU/GDSC state, a line a second; power-only without init)\n"
		"       env (default off): VENUSCTL_RAILS=1|CORNER VENUSCTL_RAILS_UNKNOWN=1 VENUSCTL_TBUVOTE=1 (power on/off)\n"
		"       VENUSCTL_SMMU=window (default) | identity | hybrid (identity 0x1380 group, window 0x1360)\n"
		"       | encode IN.yuv OUT WxH [--hevc] [--fps N] [--bitrate BPS] [--gop N] [--rc vbr|cbr|off] [--frames N]\n"
		"         [--profile P] [--level L] [--bufphys A] [--stop-at sessinit|props|alloc|load|start|ftb]\n"
		"         (after boot --keep + init: NV12 frames -> Annex B; env VENUSCTL_ENC_SKIP=prop,prop ...)\n"
		"       | memtest ADDR SIZE write|check\n"
		"       | fwdump [ADDR] (read-only, after a reset: SFR, debug ring, queues of the HFI block, default 0xa2600000)\n"
		"       P0d watch env (default off): VENUSCTL_PC=1 [VENUSCTL_PC_DELAY_MS=1500] VENUSCTL_INTCLR=1\n"
		"       ws-video-dec: decode --clip env VENUSCTL_PING_MS (idle keepalive ms, default 1000) VENUSCTL_NOCOPY;\n"
		"       watch --sysinit VENUSCTL_PING_MS=1000 = ping-only keepalive test\n");
	return 2;
}
