/*
 * venus_core: venusctl's Venus code as a library (video 4a), used by venusctl and venusd. See venus_core.c and the
 * venusctl.c header for what each step does and its safety rules. The steps keep venusctl's state in STATE (and
 * AHB_STATE), so a venusctl shutdown + power off can always undo what venusd left behind.
 */
#ifndef VENUS_CORE_H
#define VENUS_CORE_H
#include <stddef.h>
#include <stdint.h>
#include "venus_h264.h"

#ifndef STATE	/* -D for a host test build */
#define AHB_STATE	"/var/tmp/venusctl.ahb"	/* gcc_video_ahb CBCR before power on cleared HW_CTL */
#define STATE		"/var/tmp/venusctl.state"
/* ws-venus-enc: the hardware encoder (venus_enc.inc, included by venus_core.c): raw NV12 file -> Annex B H.264/HEVC */
struct venc_cfg {
	int w, h, hevc, fps, gop, rc;	/* rc: 0 VBR_CFR, 1 CBR_CFR, 2 off; gop = frames per IDR (1 = all intra) */
	uint32_t bitrate, debug;
	int profile, level, frames;	/* HFI profile/level (0 = H.264 High 4.1 / HEVC Main 4 main tier); frames 0 = all */
	uint64_t bufphys;
};
int encode(const struct venc_cfg *c, const char *in, const char *outp);
int cvp_on(void);	/* venusctl power cvp: GDSC mvs1 + mvs1 branches (after power on / boot) */
#endif

extern int verbose, cvp, force;	/* venusctl -v, --cvp, --force */
extern int vc_quiet;		/* 1: no per-buffer lines (ETB/FTB commands, EBD/FBD packets, ETB notes) */
/* vdstop: a STOP (a clock or GDSC read off VENUS_CLOCK_READS = 3 times 1 ms apart, a blocked write) calls this
 * instead of exit(1); it must not return (venusd longjmps out). NULL (venusctl): exit(1) */
extern void (*vc_stop_hook)(const char *what);
extern int vc_fake_off;		/* vdstop test: the next N clock checks read off (venusd VENUSD_TEST_CLKOFF) */
int map_sid(uint32_t sid);		/* redfin: venusctl map-sid SID (after boot --keep) */
void smmu_snapshot(const char *when);
int smmu_cmd(void);
int watch(unsigned secs, int sysinit, uint32_t debug);	/* redfin: venusctl watch */	/* redfin: venusctl smmu */	/* redfin: read-only SMMU picture of the 0x13xx streams */
extern const char *vc_stop_at;	/* redfin: venusctl decode --stop-at STAGE (--dry = ftb) */
extern int vc_frame_tag;	/* vdstop: 1: firmware debug lines during a session as "[frame N] fw [...]" */
int vc_core_ok(void);		/* the core gate's check: AHB, AXI, GDSCs, branches on (no STOP) */

/* ws-vt-venc: the encoder in steps (venus_enc.inc), for venusd's encode protocol. Constant QP (rate control off). */
struct venc_params {
	int w, h, hevc;
	uint32_t fps_q16;		/* frame rate, Q16 (0: 30) */
	uint32_t qp_i, qp_p;		/* constant QP (0: 26 / I + 2) */
	int gop;			/* frames per IDR (0: 2 s) */
	int profile, level;		/* HFI values; 0: H.264 High / HEVC Main, level chosen by the firmware */
	int seqhdr;			/* 1: SPS/PPS in band before every IDR; 0 (stock): one CODECCONFIG buffer at the start */
	int full_range, primaries, transfer, matrix;	/* VIDEO_SIGNAL_INFO (H.264/HEVC VUI codes; primaries 0: not sent) */
	int nin, nout;			/* buffer counts (0: 8 + 8) */
	uint32_t debug;			/* firmware debug mask */
};
struct venc_out {			/* one bitstream buffer back (FBD) */
	const uint8_t *data;		/* Annex B (start codes) */
	uint32_t len, flags, pic_type, tag;	/* flags: HFI buffer flags (0x20 sync, 0x80 codec config, 0x1 EOS) */
	uint64_t ts;			/* the ETB's timestamp (us) */
};
extern void (*venc_sink)(const struct venc_out *o);
extern void (*venc_in_done)(uint32_t tag, uint32_t pa);	/* an input buffer back (EBD): its tag and address */
int venc_open(const struct venc_params *p);	/* 0 running; 1 failed, no session; 2 failed, vdec_close owed */
int venc_free_slot(void);			/* a free input slot or -1 */
uint8_t *venc_slot_cpu(int k, uint32_t *size, uint32_t *stride, uint32_t *yscan);	/* vmmu only */
void venc_layout(uint32_t *stride, uint32_t *yscan, uint32_t *uvoff, uint32_t *size);
int venc_etb(int k, uint32_t pa, uint32_t alloc, uint64_t ts_us, uint32_t tag, int sync);	/* pa 0: slot k's own */
int venc_etb_off(int k, uint32_t pa, uint32_t off, uint32_t alloc, uint64_t ts_us, uint32_t tag, int sync);
int venc_eos(void);
int venc_set_qp(uint32_t qi, uint32_t qp);
uint32_t venc_fps_q16(void);
extern int vc_prop_ms;	/* the drain after each set property, ms (-1: VENUSCTL_PROP_MS / 20) */

const char *boot_id(void);
void stale_drop(const char *path);	/* a state file from an earlier boot: deleted unread */

/* venusctl's commands; 0 = ok */
int probe(void);
int power_on(void);
int power_off(void);
int pas_check(const char *fw, int no_shutdown);
int boot(const char *fw, int keep);
int init(uint64_t phys);
int ping(uint32_t debug);
int session(uint32_t debug);
int decode(uint32_t debug, const char *in, const char *outp, int want, uint64_t bufphys, int hevc);	/* -1: detect */
/* ws-video-dec: keep an active decode session fed for `seconds` of wall clock (loop the clip if `loop`), no YUV out,
 * for the idle-reset test (logs/research/video-reset-2026-11-16.md). 0 = PASS (survived, frames > 0, no error). */
int decode_loop(uint32_t debug, const char *clip, int seconds, int loop, uint64_t bufphys, int hevc);
int memtest(uint64_t a, uint64_t len, int write);
int shutdown_cmd(void);
int fwdump(uint64_t addr);	/* P0d: read-only dump of the HFI block left at addr (0: 0xa2600000) after a reset */
int hwctl_cmd(int on);	/* venus-hw: mvs0 GDSC HW_CONTROL on/off (msm_vidc __hand_off_regulators, qcom,support-hw-trigger) */
int intmask_cmd(void);	/* venus-hw: WRAPPER_INTR_MASK: unmask A2HWD (0x10) and A2H (0x4) as Linux's interrupt init */
int suspend_cmd(void);	/* venus-hw: Linux power collapse (PC_PREP, TZ suspend, power off; image kept) */
int resume_cmd(void);	/* venus-hw: power on, TZ resume, UC regs, CTRL_INIT (no PIL reload) */
int bus_cmd(uint64_t kbps);	/* rpmh-agg: venusctl bus KBPS */

/* ---- the stage 3 decoder in steps (venusctl decode runs them in its loop; venusd per client message) ---- */

struct vdec_frame {	/* one decoded picture as read out of its OUTPUT2 buffer */
	const uint8_t *buf;	/* len = stride * scanlines * 3 / 2: Y, then interleaved CbCr (Venus NV12 layout) */
	size_t len;
	uint32_t stride, scanlines;
	int cx, cy, cw, ch;	/* visible crop (SPS, or the firmware's SEQUENCE_CHANGED) */
	uint64_t ts;		/* the ETB timestamp it came from */
	uint32_t flags, filled, fbd_w, fbd_h, fbd_x, fbd_y, pic_type;	/* FBD words, as venusctl prints them */
	int index;		/* frames before this one in the session */
	double copy_ms;		/* the copy-out (pread of the window) */
	int placed;		/* vdcredit: read straight into vdec_place's planes (buf NULL, len 0) */
};
/* NULL (default): venusctl's frame handling (crop, luma CRC32, append to OUT, one line per frame) */
extern void (*vdec_sink)(const struct vdec_frame *f);
/* vdcredit (venusd's credit path): with vdec_sink set, asked before each copy-out with the picture's layout (buf NULL);
 * 1 = read the visible rect straight into d's planes (Y rows, then CbCr rows, each the visible width rounded up to 4
 * bytes; one read per plane when there's no x crop and the row pitch is Venus's stride), the sink then gets placed 1.
 * 0 = the whole buffer into the copy-out buffer as before. Called with f NULL after a failed placed read: undo it */
struct vdec_dest { uint8_t *y, *c; size_t ybpr, cbpr; };
extern int (*vdec_place)(const struct vdec_frame *f, struct vdec_dest *d);

/* SYS_INIT if due, debug config, the buffer window (bufphys, or the next 1 MB after the --phys queue block; mb MB),
 * NV12 layout for the SPS's coded size. Up to want frames are handed out. hevc (4e): an HEVC session (8-bit Main;
 * the caller checks the SPS), else H.264. 0 = ok (no session yet). */
int vdec_prepare(uint32_t debug, uint64_t bufphys, uint64_t mb, int want, const struct h264_sps *sps, int hevc);
/* SESSION_INIT, properties, internal/input/output buffers (inputs at least min_in bytes), LOAD_RESOURCES, START,
 * FTBs. 0 = running; 1 = failed with no session; 2 = failed with a session: vdec_close owed */
int vdec_start(uint32_t min_in);
int vdec_can_etb(void);		/* 1: a free input buffer and no reconfig hold */
/* one access unit (or eos: the empty EOS buffer) into a free input buffer; n = its number for the log line.
 * 0 = queued, -1 = failed, -2 = longer than the input buffers (nothing sent) */
int vdec_etb(const uint8_t *au, uint32_t len, uint64_t ts, int eos, int n);
int vdec_poll(unsigned ms);	/* a due reconfig, then the queues for up to ms (1 ms steps); 0 = ok, -1 = stop */
int vdec_close(void);		/* flush, stop, release, session end (as far as it started); 0 = all answered */

struct vdec_status {
	int frames, eos, err, fw_dead, started;	/* err: the session error code (EVENT_NOTIFY), 0 none */
	unsigned acts;				/* msgq packets handled (progress) */
	double cp_sum, cp_max;			/* copy-out seconds */
	uint32_t in_size;			/* input buffer size */
	int in_fw;				/* vdstall: input buffers with the firmware (ETB, no EBD yet) */
	unsigned ebds;				/* vdstall: EBDs handled (changes: an input came back) */
};
void vdec_status(struct vdec_status *st);
void vdec_fw_reset(void);	/* after a shutdown: the next bring-up starts with a live firmware again */
void vdec_abort(void);		/* vdstop: forget the session after a STOP unwound it (no packets handled, no commands) */
int vc_bus_session(unsigned w, unsigned h, unsigned fps);	/* venus-robust (f): VENUSCTL_BUS=1 session vote */
uint64_t vc_bus_kbps_for(unsigned w, unsigned h, unsigned fps);
unsigned vc_clk_for(unsigned w, unsigned h, unsigned fps, int two_sessions);	/* (f): 240/338/365 MHz by load */
int vc_fw_watchdog(void);	/* venus-robust (e): read-only A2HWD check (core on, queues up); -1 = firmware dead */
void vdec_sfr(void);		/* vdstop: print the firmware's SFR (STATE's queue block; DRAM only, no core access) */

#endif
