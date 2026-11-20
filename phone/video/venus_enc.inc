/*
 * ws-venus-enc: the Venus/IRIS1 hardware ENCODER, `venusctl encode IN.yuv OUT WxH` (logs/research/venus-encoder-2026-11-20.md).
 * Included at the end of venus_core.c (it shares the queue code, the bump allocator in the buffer window, the decoder's
 * session state `dc`, set_prop/get_bufreq/buf_cmd/ftb and the close sequence; one session at a time).
 * Protocol reference: the downstream msm-4.19 vidc driver of this phone's kernel (redbull techpack/video: msm_venc.c
 * msm_venc_set_properties order, msm_vidc.c start_streaming, msm_vidc_clocks.c *_iris1 work mode/route,
 * msm_vidc_buffer_calculations.c counts and sizes, hfi_packetization.c packets, vidc_hfi_helper.h numbers); written
 * here from those numbers, no code copied. IRIS1 has no host buffer calculator downstream: the internal buffer sizes
 * come from the firmware (GET_PROPERTY BUFFER_REQUIREMENTS), as for the decoder.
 * Sequence: SYS_INIT if due (sys_up), SESSION_INIT domain encoder + codec, properties (frame sizes, frame rate,
 * realtime, NV12 input, SPS/PPS before every IDR, profile/level, 8x8 + CABAC (H.264 High), rate control, timestamp RC,
 * start codes, IDR period, intra period, bitrate), buffer counts, work mode/route, requirements, output size minimum,
 * internal buffers (scratch*, persist*), input + bitstream buffers, LOAD_RESOURCES, START, FTB every bitstream buffer,
 * then one ETB per raw frame (and an empty EOS buffer), FBDs written to OUT in order, drain to EOS, the decoder's close
 * (flush, stop, release resources, release internal buffers, session end).
 * Input: tightly packed NV12 (Y w*h, then interleaved CbCr w*h/2) per frame, copied into Venus's NV12 layout (Y stride
 * align(w,128), Y scanlines align(h,32), CbCr at stride*scanlines with align(h/2,16) scanlines).
 * Switches (env): VENUSCTL_ENC_SKIP=name,name (properties not sent, names as printed), VENUSCTL_ENC_STRICT=1 (a
 * refused property (0x1002/0x1007/0x1008) stops the session; default: noted, skipped), VENUSCTL_ENC_NIN/NOUT (counts),
 * VENUSCTL_ENC_LAYER (bitrate/qp layer id, default 0xff = all layers), VENUSCTL_ENC_WORKMODE (1|2, default 2; 1 adds
 * LOW_LATENCY as downstream), VENUSCTL_ENC_DEBUG=MASK (firmware debug mask from LOAD on), VENUSCTL_ENC_TSRC=1 (firmware derives time deltas from the frame rate), VENUSCTL_BUF_MB.
 * --stop-at STAGE: sessinit props alloc load start ftb (watch 2 s, then close).
 */
#define VE_DOMAIN_ENC	1u		/* HFI_VIDEO_DOMAIN_ENCODER */
#define VE_CODEC_H264	2u
#define VE_P_FRAME_RATE	0x2001u		/* CONFIG_FRAME_RATE {buffer_type, fps Q16} */
#define VE_P_REALTIME	0x202002u	/* CONFIG_REALTIME {enable} */
#define VE_P_SEQHDR	0x2006008u	/* CONFIG_VENC_SYNC_FRAME_SEQUENCE_HEADER {enable}: SPS/PPS before each IDR */
#define VE_P_8X8	0x2005025u	/* PARAM_VENC_H264_8X8_TRANSFORM {enable} */
#define VE_P_ENTROPY	0x2005002u	/* PARAM_VENC_H264_ENTROPY_CONTROL {mode, cabac_model} */
#define VE_P_RC		0x2005004u	/* PARAM_VENC_RATE_CONTROL {mode} */
#define VE_P_RC_TS	0x2005027u	/* PARAM_VENC_DISABLE_RC_TIMESTAMP {enable} */
#define VE_P_NALFMT	0x100bu		/* PARAM_NAL_STREAM_FORMAT_SELECT {format} */
#define VE_P_IDR	0x2006002u	/* CONFIG_VENC_IDR_PERIOD {period} */
#define VE_P_INTRA	0x2006003u	/* CONFIG_VENC_INTRA_PERIOD {pframes, bframes} */
#define VE_P_BR_TYPE	0x2005031u	/* PARAM_VENC_BITRATE_TYPE {enable}: 0 = cumulative */
#define VE_P_BITRATE	0x2006001u	/* CONFIG_VENC_TARGET_BITRATE {bitrate, layer_id} */
#define VE_P_LOWLAT	0x2005022u	/* PARAM_VENC_LOW_LATENCY_MODE {enable} */
#define VE_P_BLUR	0x2006010u	/* CONFIG_VENC_BLUR_FRAME_SIZE {buffer_type, width, height} */
#define VE_P_CAC	0x2005038u	/* PARAM_VENC_BITRATE_SAVINGS {enable} (content-adaptive coding) */
#define VE_RC_OFF	0x1000001u
#define VE_RC_VBR_CFR	0x1000003u
#define VE_RC_CBR_CFR	0x1000005u
#define VE_F_SYNC	0x20u		/* HFI_BUFFERFLAG_SYNCFRAME */
#define VE_F_CODECCFG	0x80u		/* HFI_BUFFERFLAG_CODECCONFIG */
#define VE_MAXF		4096

static struct {
	const struct venc_cfg *c;
	FILE *out;
	int nframes, etbs, fbds, bytes_frames, eos_sent;
	long long total;
	double t_etb[VE_MAXF], t_fbd_first, t_fbd_last, lat_sum, lat_max;
	int lat_n;
	uint8_t *rd;
	size_t rd_len;
	int pre[8];		/* the frame each input buffer was filled with before START (-1: none) */
} ve;

uint32_t np_persist(uint32_t size, uint32_t iova);
static int ve_skip(const char *name)
{
	const char *s = getenv("VENUSCTL_ENC_SKIP");
	size_t n = strlen(name);
	while (s && *s) {
		if (!strncmp(s, name, n) && (s[n] == ',' || !s[n]))
			return 1;
		s = strchr(s, ',');
		s = s ? s + 1 : NULL;
	}
	return 0;
}

/* a property of 1-3 body words unless VENUSCTL_ENC_SKIP names it; -1 on a session error */
static int ve_prop(const char *name, uint32_t prop, uint32_t a, uint32_t b, uint32_t c, unsigned n)
{
	if (ve_skip(name)) {
		printf("venus: enc: %s skipped (VENUSCTL_ENC_SKIP)\n", name);
		return 0;
	}
	if (set_prop(prop, a, b, c, n, name)) {
		uint32_t e = (uint32_t)dc.err;
		printf("venus: enc: property %s (%#x) refused (session error %#x)\n", name, prop, e);
		pm_step("enc: property %s refused, err %#x", name, e);
		/* unsupported property/setting, invalid parameter: noted, the session goes on (VENUSCTL_ENC_STRICT=1: stop) */
		if ((e == 0x1007 || e == 0x1008 || e == 0x1002) && !getenv("VENUSCTL_ENC_STRICT")) {
			printf("venus: enc: note: %s refused with %#x, going on without it\n", name, e);
			dc.err = 0;
			return 0;
		}
		return -1;
	}
	return 0;
}

/* one bitstream buffer back from the firmware: hfi_msg_session_fill_buffer_done_compressed_packet words */
static void ve_fbd(const uint32_t *p, int n)
{
	uint32_t err = p[5], flags = p[6], off = p[10], len = p[12], tag = p[13], ptype = p[15], pa = p[16];
	struct vbuf *b = find_buf(dc.opb, dc.nopb, pa);
	double t = now_s();
	if (b)
		b->fw = 0;
	ve.fbds++;
	if (err || !b)
		printf("venus: enc FBD buf %#x error %#x%s\n", pa, err, b ? "" : " (not ours)");
	if (len && b && (uint64_t)off + len <= b->size) {
		size_t need = align_up(off + len, 4);
		if (ve.rd_len < need) {
			free(ve.rd);
			ve.rd = malloc(need);
			ve.rd_len = ve.rd ? need : 0;
		}
		if (!ve.rd || big_io(pa, ve.rd, need, 0)) {
			printf("venus: enc: can't read the bitstream at %#x\n", pa);
		} else {
			if (ve.out) {	/* flushed + synced each time: a reset keeps what came out */
				fwrite(ve.rd + off, 1, len, ve.out);
				fflush(ve.out);
				fsync(fileno(ve.out));
			}
			ve.total += len;
			const uint8_t *s = ve.rd + off;
			int nal = len > 4 ? (s[2] == 1 ? s[3] : s[4]) : -1;	/* first NAL header after 00 00 01 / 00 00 00 01 */
			double lat = -1;
			if (tag >= 1 && tag <= (uint32_t)ve.etbs && tag <= VE_MAXF && !(flags & VE_F_CODECCFG))
				lat = 1e3 * (t - ve.t_etb[tag - 1]);
			if (lat >= 0) {
				ve.lat_sum += lat;
				ve.lat_n++;
				if (lat > ve.lat_max)
					ve.lat_max = lat;
			}
			if (!(flags & VE_F_CODECCFG)) {
				if (!ve.bytes_frames)
					ve.t_fbd_first = t;
				ve.t_fbd_last = t;
				ve.bytes_frames++;
			}
			printf("venus: enc out %d: %u bytes flags %#x%s%s pic type %#x tag %u nal %#x%s ts %llu, ETB->FBD %.2f ms\n",
			       ve.fbds - 1, len, flags, flags & VE_F_SYNC ? " sync" : "", flags & VE_F_CODECCFG ? " config" : "",
			       ptype, tag, (unsigned)(nal < 0 ? 0 : nal), flags & F_EOS ? " EOS" : "",
			       ((unsigned long long)p[3] << 32) | p[4], lat);
			pm_step("enc FBD %d len %u flags %#x tag %u lat %.2f ms", ve.fbds - 1, len, flags, tag, lat);
		}
	} else if (!(flags & F_EOS) || len) {
		printf("venus: enc FBD buf %#x: empty or bad (offset %u, filled %u, flags %#x)\n", pa, off, len, flags);
	}
	if (flags & F_EOS) {
		dc.eos = 1;
		printf("venus: enc: EOS back\n");
	}
	if (b && !dc.eos && !dc.err)
		ftb(0, b);
	(void)n;
}

static int enc_pkt(int n)
{
	uint32_t p[64];
	memcpy(p, pkt, 4u * (unsigned)(n < 64 ? n : 64));
	dc.acts++;
	if (p[1] == M_EBD && n >= 10) {	/* hfi_msg_session_empty_buffer_done_packet: packet_buffer word 7 */
		struct vbuf *b = find_buf(dc.in, dc.nin, p[7]);
		if (b)
			b->fw = 0;
		dc.ebds++;
		if (p[3] || !b)
			printf("venus: enc EBD tag %u buf %#x error %#x flags %#x%s\n", p[6], p[7], p[3], p[9],
			       b ? "" : " (not ours)");
	} else if (p[1] == M_FBD && n >= 18) {
		ve_fbd(p, n);
	} else if (p[1] == M_EVENT && n >= 6) {
		if (p[3] == EV_SESSION_ERR) {
			printf("venus: enc session error %#x data2 %#x\n", p[4], p[5]);
			pm_step("enc session error %#x data2 %#x", p[4], p[5]);
			dc.err = (int)p[4];
		} else {
			printf("venus: enc event %#x data %#x %#x\n", p[3], p[4], p[5]);
		}
	} else if (p[1] != M_EBD && p[1] != M_FBD) {
		printf("venus: enc msg %#x size %u\n", p[1], p[0]);
	}
	return 0;
}

/* Venus NV12 (msm_media_info.h COLOR_FMT_NV12): Y stride 128, Y scanlines 32, CbCr scanlines 16, + a 4 KB pad */
static uint32_t ve_uvscan(void)
{
	return align_up(((uint32_t)dc.h + 1) / 2, 16);
}

static uint32_t ve_insize(void)
{
	uint32_t y = dc.ystride * dc.yscan, uv = dc.ystride * ve_uvscan() + 4096;
	return align_up(y + uv, 4096);
}

/* one raw frame (tight NV12) into an input buffer in Venus's layout, then ETB (hfi_cmd_session_empty_buffer_
 * uncompressed_plane0_packet: size, type, sid, view_id, ts hi/lo, flags, mark x2, alloc_len, filled_len, offset,
 * input_tag, packet_buffer, extradata, rgData); eos: the empty EOS buffer */
static int ve_fill(struct vbuf *b, const uint8_t *frame)
{
	uint32_t W = (uint32_t)dc.w, H = (uint32_t)dc.h, S = dc.ystride;
	{
		static uint8_t *img;
		static size_t img_len;
		size_t need = (size_t)ve_insize();
		if (img_len < need) {
			free(img);
			img = calloc(1, need);
			img_len = img ? need : 0;
		}
		if (!img)
			return -1;
		for (uint32_t y = 0; y < H; y++)
			memcpy(img + (size_t)y * S, frame + (size_t)y * W, W);
		uint8_t *uv = img + (size_t)S * dc.yscan;
		for (uint32_t y = 0; y < (H + 1) / 2; y++)
			memcpy(uv + (size_t)y * S, frame + (size_t)W * H + (size_t)y * W, W);
		if (need > b->size || big_io(b->pa, img, need, 1))
			return -1;
	}
	return 0;
}

static int ve_etb(const uint8_t *frame, int idx, int eos)
{
	struct vbuf *b = free_in();
	if (!b)
		return -1;
	uint32_t fill = 0;
	if (!eos) {
		int k = (int)(b - dc.in);
		fill = ve_insize();	/* the whole Venus NV12 buffer, as a V4L2 client's bytesused = sizeimage */
		static int nofill = -1;	/* venus-iommu: VENUSCTL_ENC_NOFILL=1: the input buffer keeps what it holds (a
					 * camera surface the producer filled by DMA: no CPU touch at all per frame) */
		if (nofill < 0)
			nofill = getenv("VENUSCTL_ENC_NOFILL") && atoi(getenv("VENUSCTL_ENC_NOFILL"));
		if (k >= 0 && k < 8 && ve.pre[k] == idx)
			ve.pre[k] = -1;	/* filled before START */
		else if (nofill && idx >= dc.nin)
			;
		else if (ve_fill(b, frame))
			return -1;
	}
	uint64_t ts = (uint64_t)idx * 1000000ull / (uint64_t)(ve.c->fps > 0 ? ve.c->fps : 30);
	uint32_t e[16] = { 64, S_ETB, SESSION_ID, 0, (uint32_t)(ts >> 32), (uint32_t)ts, eos ? F_EOS : 0, 0, 0, b->size,
		fill, 0, (uint32_t)idx + 1, b->pa, 0, 0 };
	b->fw = 1;
	if (!eos && idx < VE_MAXF)
		ve.t_etb[idx] = now_s();
	if (!vc_quiet)
		printf("venus: enc ETB %d: %u bytes at %#x%s\n", idx, fill, b->pa, eos ? " EOS" : "");
	pm_step("enc ETB %d fill %u%s", idx, fill, eos ? " EOS" : "");
	return q_write(e) ? -1 : 0;
}

static int ve_stage(const char *s, double t0)
{
	printf("venus: enc stage %s at %.1f ms\n", s, mono_ms() - t0);
	return stop_here(s);
}

int encode(const struct venc_cfg *c, const char *in, const char *outp)
{
	int W = c->w, H = c->h;
	if (W < 32 || H < 32 || W > 4096 || H > 2304 || (W & 1) || (H & 1)) {
		printf("venus: refused: frame size %dx%d (even, 32..4096 x 32..2304)\n", W, H);
		return 1;
	}
	size_t flen = 0, fsz = (size_t)W * H * 3 / 2;
	uint8_t *src = slurp(in, &flen);
	if (!src || flen < fsz) {
		printf("venus: refused: %s: can't read one %dx%d NV12 frame (%zu bytes)\n", in, W, H, fsz);
		return 1;
	}
	int nf = (int)(flen / fsz), nfile = nf;
	if (c->frames > 0 && c->frames < nf)
		nf = c->frames;
	if (c->frames > nf && getenv("VENUSCTL_ENC_LOOP") && atoi(getenv("VENUSCTL_ENC_LOOP")))
		nf = c->frames;	/* venus-iommu: the file's frames again and again (speed runs) */
	if (nf > VE_MAXF)
		nf = VE_MAXF;
	memset(&ve, 0, sizeof(ve));
	ve.c = c;
	ve.nframes = nf;
	uint32_t codec = c->hevc ? VD_HFI_CODEC_HEVC : VE_CODEC_H264;
	printf("venus: encode %s: %d frames %dx%d NV12 -> %s %s, %d fps, %u bit/s %s, gop %d\n", in, nf, W, H,
	       c->hevc ? "HEVC" : "H.264", outp, c->fps, c->bitrate, c->rc == 1 ? "CBR" : c->rc == 2 ? "RC off" : "VBR",
	       c->gop);
	struct h264_sps sps = { .w = W, .h = H, .cw = W, .ch = H };
	uint64_t mb = getenv("VENUSCTL_BUF_MB") ? strtoull(getenv("VENUSCTL_BUF_MB"), NULL, 0) : 64;
	double t0 = mono_ms();
	if (vdec_prepare(c->debug, c->bufphys, mb, 1 << 30, &sps, c->hevc))
		return 1;
	dc.split = 0;
	ve.out = fopen(outp, "wb");
	if (!ve.out) {
		printf("venus: refused: can't write %s\n", outp);
		return 1;
	}
	int rc = 1, started = 0;
	double ts_sys = mono_ms() - t0;
	/* SYS_SESSION_INIT {session_domain, session_codec} (create_pkt_cmd_sys_session_init) */
	uint32_t si[5] = { 20, 0x10007, SESSION_ID, VE_DOMAIN_ENC, codec };
	pm_step("enc SESSION_INIT codec %#x", codec);
	if (q_write(si) || wait_msg(0x20006, 1000)) {
		fclose(ve.out);
		return 1;
	}
	printf("venus: enc SESSION_INIT_DONE error %#x\n", pkt[3]);
	if (pkt[3]) {
		fclose(ve.out);
		return 1;
	}
	on_pkt = enc_pkt;
	if (ve_stage("sessinit", t0))
		goto watch;
	uint32_t prof, lvl, layer = getenv("VENUSCTL_ENC_LAYER") ? (uint32_t)strtoul(getenv("VENUSCTL_ENC_LAYER"), NULL, 0)
		: 0xffu;
	if (c->hevc) {
		prof = c->profile ? (uint32_t)c->profile : 0x1u;	/* HFI_HEVC_PROFILE_MAIN */
		lvl = c->level ? (uint32_t)c->level : (0x20u | (1u << 28));	/* HFI_HEVC_LEVEL_4, tier main << 28 */
	} else {
		prof = c->profile ? (uint32_t)c->profile : 0x4u;	/* HFI_H264_PROFILE_HIGH */
		lvl = c->level ? (uint32_t)c->level : 0x1000u;	/* HFI_H264_LEVEL_41 */
	}
	uint32_t rcm = c->rc == 1 ? VE_RC_CBR_CFR : c->rc == 2 ? VE_RC_OFF : VE_RC_VBR_CFR;
	int wm = getenv("VENUSCTL_ENC_WORKMODE") ? atoi(getenv("VENUSCTL_ENC_WORKMODE")) : 2;
	/* msm_venc_set_properties order (the parts a plain IPPP session uses) */
	if (ve_prop("input-size", P_FRAME_SIZE, B_INPUT, (uint32_t)W, (uint32_t)H, 3) ||
	    ve_prop("output-size", P_FRAME_SIZE, B_OUTPUT, (uint32_t)W, (uint32_t)H, 3) ||
	    ve_prop("frame-rate", VE_P_FRAME_RATE, B_OUTPUT, (uint32_t)c->fps << 16, 0, 2) ||
	    ve_prop("realtime", VE_P_REALTIME, 1, 0, 0, 1) ||
	    ve_prop("input-nv12", P_FMT_SEL, B_INPUT, CF_NV12, 0, 2) ||
	    ve_prop("seq-header", VE_P_SEQHDR, 1, 0, 0, 1) ||
	    ve_prop("profile-level", P_PROFILE, prof, lvl, 0, 2) ||
	    (!c->hevc && (prof == 0x4u || prof == 0x40u) && ve_prop("8x8", VE_P_8X8, 1, 0, 0, 1)) ||
	    (!c->hevc && prof != 0x1u && prof != 0x20u && ve_prop("cabac", VE_P_ENTROPY, 2, 3, 0, 2)) ||
	    ve_prop("rate-control", VE_P_RC, rcm, 0, 0, 1) ||
	    /* CAC (content-adaptive "bitrate savings") runs a CVP texture pass (VPSS FTEXTURE) on the CVP core before each
	     * frame: e6 hung there with mvs1 off. Off (0) unless VENUSCTL_ENC_CAC=1; downstream allows off with VBR_CFR only */
	    ve_prop("bitrate-savings", VE_P_CAC, getenv("VENUSCTL_ENC_CAC") ? (uint32_t)atoi(getenv("VENUSCTL_ENC_CAC")) : 0,
		    0, 0, 1) ||
	    ve_prop("rc-timestamp", VE_P_RC_TS, getenv("VENUSCTL_ENC_TSRC") ? 1u : 0u, 0, 0, 1) ||
	    ve_prop("startcodes", VE_P_NALFMT, 1, 0, 0, 1) ||
	    ve_prop("idr-period", VE_P_IDR, 1, 0, 0, 1) ||
	    ve_prop("intra-period", VE_P_INTRA, c->gop > 1 ? (uint32_t)c->gop - 1 : 0, 0, 0, 2) ||
	    (rcm != VE_RC_OFF && (ve_prop("bitrate-type", VE_P_BR_TYPE, 0, 0, 0, 1) ||
				  ve_prop("bitrate", VE_P_BITRATE, c->bitrate, layer, 0, 2))) ||
	    /* blur (a VPSS/CVP pre-process): {INPUT, W, H} = all blur off (msm_venc_set_blur_resolution's 2), stock sends
	     * {INPUT, 0, 0} = internal blur on. Off unless VENUSCTL_ENC_BLUR=1 (internal) */
	    ve_prop("blur", VE_P_BLUR, B_INPUT, getenv("VENUSCTL_ENC_BLUR") ? 0u : (uint32_t)W,
		    getenv("VENUSCTL_ENC_BLUR") ? 0u : (uint32_t)H, 3))
		goto close;
	if (get_bufreq())
		goto close;
	uint32_t *ri = req_of(B_INPUT), *ro = req_of(B_OUTPUT);
	int nin = ri && (int)ri[5] > 4 ? (int)ri[5] : 4, nout = ro && (int)ro[5] > 4 ? (int)ro[5] : 4;
	if (getenv("VENUSCTL_ENC_NIN"))
		nin = atoi(getenv("VENUSCTL_ENC_NIN"));
	if (getenv("VENUSCTL_ENC_NOUT"))
		nout = atoi(getenv("VENUSCTL_ENC_NOUT"));
	nin = nin < 1 ? 1 : nin > 8 ? 8 : nin;
	nout = nout < 1 ? 1 : nout > 32 ? 32 : nout;
	/* bitstream size: msm_vidc_calculate_enc_output_frame_size (YUV*2 up to 480p, /2 up to 4K; RC off *2) */
	uint32_t aw = align_up((uint32_t)W, 32), ah = align_up((uint32_t)H, 32), osz = aw * ah * 3;
	if ((aw / 16) * (ah / 16) > 1620)	/* NUM_MBS_480P: 720x480 */
		osz >>= 2;
	if (rcm == VE_RC_OFF)
		osz <<= 1;
	if (c->hevc)
		osz += osz >> 2;
	if (ro && ro[1] > osz)
		osz = ro[1];
	osz = align_up(osz, 4096);
	uint32_t isz = ve_insize();
	if (ri && ri[1] > isz)
		isz = align_up(ri[1], 4096);
	if (ve_prop("input-count", P_BUF_COUNT, B_INPUT, (uint32_t)nin, (uint32_t)nin, 3) ||
	    ve_prop("output-count", P_BUF_COUNT, B_OUTPUT, (uint32_t)nout, (uint32_t)nout, 3) ||
	    ve_prop("work-mode", P_WORK_MODE, (uint32_t)wm, 0, 0, 1) ||
	    (wm == 1 && ve_prop("low-latency", VE_P_LOWLAT, 1, 0, 0, 1)) ||
	    ve_prop("work-route", P_WORK_ROUTE, 2, 0, 0, 1) ||
	    get_bufreq() ||
	    ve_prop("output-size-min", P_BUF_SIZE_MIN, B_OUTPUT, osz, 0, 2))
		goto close;
	if (ve_stage("props", t0))
		goto watch;
	/* internal buffers: scratch, scratch_1, scratch_2, persist, persist_1 (msm_comm_set_scratch/persist_buffers) */
	static const uint32_t itypes[] = { 6, 7, 8, 4, 5 };
	for (unsigned t = 0; t < 5; t++) {
		uint32_t type = itypes[t], *r = req_of(type), cnt = r ? r[5] : 0;
		if (r && r[1] && !cnt && r[3])
			cnt = r[3];
		for (uint32_t i = 0; r && r[1] && i < cnt && dc.nintl < 16; i++) {
			uint32_t pa;
			if (!strcmp(bname(type), "persist") &&
			    ((getenv("VENUSCTL_NP_BANK") && atoi(getenv("VENUSCTL_NP_BANK"))) ||	/* venus-cvp */
			     (vmmu_active() && getenv("VENUSCTL_NP_ASSIGN") && atoi(getenv("VENUSCTL_NP_ASSIGN")))))
				/* hyp-assigned to CP_NON_PIXEL, as stock's secure heap; NP_BANK: its own bank as stock */
				pa = np_persist(r[1], 0x25800000u - align_up(r[1], 0x10000) * (i + 1));
			else if (vmmu_active() && !strcmp(bname(type), "persist") && getenv("VENUSCTL_NP_PERSIST") &&
			    atoi(getenv("VENUSCTL_NP_PERSIST")))	/* venus-iommu CVP test: as stock, in the non-pixel range */
				pa = vmmu_alloc_at(r[1], "persist (non-pixel IOVA)", 0x25800000u - align_up(r[1], 0x10000) * (i + 1));
			else
				pa = balloc(r[1], bname(type));
			if (!pa || buf_cmd(type, pa, r[1], 0))
				goto close;
			dc.intl[dc.nintl] = (struct vbuf){ pa, r[1], 1 };
			dc.intl_type[dc.nintl++] = type;
			printf("venus: enc %s buffer %#x (%u bytes) set\n", bname(type), pa, r[1]);
		}
	}
	if (wait_msg(0, 20) || dc.err)
		goto close;
	/* venus-iommu: VENUSCTL_ENC_SURF=1 (with VENUSCTL_VMMU=1): each input buffer is an NV12 IOSurface in Venus's
	 * layout mapped into the bank by id, as a camera surface would be (zero-copy input) */
	int esurf = vmmu_active() && getenv("VENUSCTL_ENC_SURF") && atoi(getenv("VENUSCTL_ENC_SURF"));
	for (dc.nin = 0; dc.nin < nin; dc.nin++) {
		uint32_t pa, sid = 0;
		void *sref = NULL;
		if (esurf) {
			pa = vmmu_new_surface((uint32_t)W, (uint32_t)H, dc.ystride, dc.yscan, isz, &sid, &sref);
			if (pa)
				printf("venus: enc input %d = IOSurface %u at IOVA %#x (zero-copy)\n", dc.nin, sid, pa);
		} else
			pa = balloc(isz, "enc input");
		if (!pa)
			goto close;
		dc.in[dc.nin] = (struct vbuf){ pa, isz, 0 };
	}
	for (dc.nopb = 0; dc.nopb < nout; dc.nopb++) {
		uint32_t pa = balloc(osz, "bitstream");
		if (!pa)
			goto close;
		dc.opb[dc.nopb] = (struct vbuf){ pa, osz, 0 };
	}
	/* the first frames go into the input buffers now, so the ETBs follow START at once (VENUSCTL_ENC_PREFILL=0: no) */
	for (int k = 0; k < 8; k++)
		ve.pre[k] = -1;
	if (!getenv("VENUSCTL_ENC_PREFILL") || atoi(getenv("VENUSCTL_ENC_PREFILL")))
		for (int k = 0; k < dc.nin && k < nf; k++) {
			if (ve_fill(&dc.in[k], src + fsz * (size_t)k))
				goto close;
			ve.pre[k] = k;
		}
	printf("venus: enc: %d inputs of %u bytes (NV12 stride %u, scanlines %u/%u), %d bitstream buffers of %u bytes, "
	       "window used %llu KB\n", nin, isz, dc.ystride, dc.yscan, ve_uvscan(), nout, osz,
	       (unsigned long long)((dc.top - dc.base) >> 10));
	if (ve_stage("alloc", t0))
		goto watch;
	if (getenv("VENUSCTL_ENC_DEBUG")) {	/* the firmware's debug mask raised only now (fewer lines during setup) */
		uint32_t dbg[7] = { 28, 0x10005, 1, 0x1, (uint32_t)strtoul(getenv("VENUSCTL_ENC_DEBUG"), NULL, 0), 1, 0 };
		if (q_write(dbg) || wait_msg(0, 5))
			goto close;
	}
	if (sess_cmd(S_LOAD_RES) || wait_msg(M_LOAD_DONE, 1000))
		goto close;
	if (ve_stage("load", t0))
		goto watch;
	if (sess_cmd(S_START))
		goto close;
	started = dc.started = 1;
	if (wait_msg(M_START_DONE, 1000))
		goto close;
	printf("venus: enc START_DONE error %#x\n", pkt[0] >= 16 ? pkt[3] : 0);
	if (ve_stage("start", t0))
		goto watch;
	for (int i = 0; i < dc.nopb; i++)
		if (ftb(0, &dc.opb[i]))
			goto close;
	double t_ready = mono_ms() - t0;
	if (ve_stage("ftb", t0))
		goto watch;

	/* one raw frame per free input buffer, then the EOS buffer; until EOS back, an error, 3 s without a message
	 * or 20 s in all */
	int next = 0;
	double te = now_s(), tact = te;
	unsigned acts = 0;
	while (!dc.eos && !dc.err) {
		if (free_in() && (next < nf || !ve.eos_sent)) {
			int eos = next >= nf;
			if (ve_etb(eos ? NULL : src + fsz * (size_t)(next % nfile), next, eos))
				goto close;
			if (eos)
				ve.eos_sent = 1;
			else
				ve.etbs = ++next;
			continue;
		}
		if (wait_msg(0, 5))
			goto close;
		double t = now_s();
		if (dc.acts != acts) {
			acts = dc.acts;
			tact = t;
		}
		if (t - tact > 3 || t - te > 20) {
			printf("venus: note: encode stopped after %.1f s (%.1f s without a message)\n", t - te, t - tact);
			core_status("encode timeout");
			break;
		}
	}
	double tdone = now_s();
	printf("venus: encoded %d of %d frames (%d FBDs, %lld bytes)%s%s in %.3f s; setup (SYS_INIT..FTBs) %.1f ms, "
	       "sys_up %.1f ms\n", ve.bytes_frames, nf, ve.fbds, ve.total, dc.eos ? ", EOS" : "",
	       dc.err ? ", session error" : "", tdone - te, t_ready, ts_sys);
	if (ve.lat_n)
		printf("venus: enc ETB->FBD latency: mean %.2f ms, max %.2f ms over %d frames\n", ve.lat_sum / ve.lat_n,
		       ve.lat_max, ve.lat_n);
	if (vmmu_active())
		vmmu_status("after encode");
	if (ve.bytes_frames > 1 && ve.t_fbd_last > ve.t_fbd_first)
		printf("venus: enc throughput: first->last FBD %.3f s = %.1f fps (%.2f ms/frame); first ETB->last FBD %.2f "
		       "ms/frame; %.0f kbit/s at %d fps\n", ve.t_fbd_last - ve.t_fbd_first,
		       (ve.bytes_frames - 1) / (ve.t_fbd_last - ve.t_fbd_first),
		       1e3 * (ve.t_fbd_last - ve.t_fbd_first) / (ve.bytes_frames - 1),
		       1e3 * (ve.t_fbd_last - ve.t_etb[0]) / ve.bytes_frames,
		       ve.total * 8.0 * c->fps / ve.bytes_frames / 1e3, c->fps);
	pm_step("enc done: %d frames %lld bytes eos %d err %#x", ve.bytes_frames, ve.total, dc.eos, (unsigned)dc.err);
	rc = ve.bytes_frames >= nf && !dc.err ? 0 : 1;
	goto close;
watch:
	{
		double td = now_s();
		while (now_s() - td < 2 && !dc.err)
			if (wait_msg(0, 5))
				break;
		step_status(1);
		rc = dc.err ? 1 : 0;
	}
close:
	if (dec_close(started))
		rc = 1;
	if (ve.out)
		fclose(ve.out);
	ve.out = NULL;
	free(src);
	core_status("after encode");
	printf("venus: encode %s: wrote %lld bytes to %s; venusctl shutdown, then power off\n", rc ? "FAILED" : "ok",
	       ve.total, outp);
	return rc;
}

/* ws-venus-enc: `venusctl power cvp` -- the CVP domain (GDSC mvs1, then mvs1_axi + mvs1_core) on its own, e.g. after
 * boot --keep: the encoder's texture pass (CAC, VPSS FTEXTURE) runs on the CVP core. power off takes it down again
 * (it checks mvs1's PWR_ON). The same steps as power on --cvp's mvs1 part. */
int cvp_on(void)
{
	if (!ahb_on() || !core_ok()) {
		printf("venus: refused: power on first (video AHB/core clocks off)\n");
		return 1;
	}
	if (gdsc_on(GDSC_MVS1, "mvs1"))
		return 1;
	for (unsigned i = 0; i < NBR; i++)
		if (branches[i].cvp && branch_on(branches[i].pa, branches[i].name))
			return 1;
	pm_step("cvp on: mvs1 %#010x", vrd(GDSC_MVS1));
	return 0;
}

/* ---- ws-vt-venc: the encoder in steps, for venusd's encode protocol (venus_proto.h VD_ENC_*) and the VideoToolbox
 * encoder in mediaserverd (vtvenc.c). Same session state `dc` as the decoder (one session at a time), same close
 * (vdec_close). Differences from venusctl encode (stock msm_venc defaults, venus-stock-vs-ours 4a):
 *  - rate control OFF (constant QP: CVP is needed for RC, see venus-encoder-2026-11-20.md), the QP from the client
 *    (CONFIG_VENC_FRAME_QP; venusd maps the bitrate hint to it);
 *  - SYNC_FRAME_SEQUENCE_HEADER 0 by default: the parameter sets come once as their own CODECCONFIG buffer (stock);
 *  - profile High/Main, level 0 = the firmware picks it (stock); VIDEO_SIGNAL_INFO when the client gives colour;
 *    OPERATING_RATE, PERF_MODE 1 (stock); 8 inputs + 8 bitstream buffers (stock BUFFER_COUNT_ACTUAL);
 *  - inputs: venc_etb() either the slot's own Venus-layout buffer (filled by the caller through venc_slot_cpu) or any
 *    IOVA (an IOSurface mapped with vmmu_map_surface: zero-copy); EBD -> venc_in_done(tag); FBD -> venc_sink.
 */
#define VE_P_OPRATE	0x2003u		/* CONFIG_OPERATING_RATE {rate Q16} */
#define VE_P_SIGNAL	0x200501du	/* PARAM_VENC_VIDEO_SIGNAL_INFO {enable, format, full, desc, prim, transfer, matrix} */
#define VE_P_SYNCREQ	0x2006004u	/* CONFIG_VENC_REQUEST_SYNC_FRAME {enable} */
#define VE_P_PERF	0x200600eu	/* CONFIG_VENC_PERF_MODE {mode}: 1 max quality (stock) */
#define VE_P_FRAMEQP	0x2006012u	/* CONFIG_VENC_FRAME_QP hfi_quantization {qp I|P<<8|B<<16, layer, enable 7, 3 rsvd} */

void (*venc_sink)(const struct venc_out *o);
void (*venc_in_done)(uint32_t tag, uint32_t pa);
static struct {
	struct venc_params p;
	uint32_t own[8];	/* each slot's own input buffer (IOVA / window PA) */
	uint32_t osz, isz;
	uint8_t *rd;
	size_t rd_len;
	unsigned outs, cfgs, ins;
} vs;

/* a property with up to 8 body words (set_prop has 3) */
static int ve_propv(const char *name, uint32_t prop, const uint32_t *body, unsigned n)
{
	if (ve_skip(name)) {
		printf("venus: enc: %s skipped (VENUSCTL_ENC_SKIP)\n", name);
		return 0;
	}
	uint32_t p[13] = { 20 + 4 * n, S_SET_PROP, SESSION_ID, 1, prop };
	memcpy(p + 5, body, 4 * (n > 8 ? 8 : n));
	printf("venus: set %s (%#x):", name, prop);
	for (unsigned k = 0; k < n; k++)
		printf(" %#x", body[k]);
	printf("\n");
	if (q_write(p) || wait_msg(0, vc_prop_ms >= 0 ? (unsigned)vc_prop_ms : 20u))
		return -1;
	if (dc.err) {
		uint32_t e = (uint32_t)dc.err;
		printf("venus: enc: property %s (%#x) refused (session error %#x)\n", name, prop, e);
		if ((e == 0x1007 || e == 0x1008 || e == 0x1002) && !getenv("VENUSCTL_ENC_STRICT")) {
			printf("venus: enc: note: %s refused with %#x, going on without it\n", name, e);
			dc.err = 0;
			return 0;
		}
		return -1;
	}
	return 0;
}

static int venc_pkt(int n)
{
	uint32_t p[64];
	memcpy(p, pkt, 4u * (unsigned)(n < 64 ? n : 64));
	dc.acts++;
	if (p[1] == M_EBD && n >= 10) {
		struct vbuf *b = find_buf(dc.in, dc.nin, p[7]);
		if (b)
			b->fw = 0;
		dc.ebds++;
		if (p[3] || !b)
			printf("venus: enc EBD tag %u buf %#x error %#x flags %#x%s\n", p[6], p[7], p[3], p[9],
			       b ? "" : " (not ours)");
		if (venc_in_done)
			venc_in_done(p[6], p[7]);
	} else if (p[1] == M_FBD && n >= 18) {
		uint32_t err = p[5], flags = p[6], off = p[10], len = p[12], tag = p[13], ptype = p[15], pa = p[16];
		struct vbuf *b = find_buf(dc.opb, dc.nopb, pa);
		if (b)
			b->fw = 0;
		if (err || !b)
			printf("venus: enc FBD buf %#x error %#x%s\n", pa, err, b ? "" : " (not ours)");
		if (len && b && (uint64_t)off + len <= b->size) {
			size_t need = align_up(off + len, 4);
			if (vs.rd_len < need) {
				free(vs.rd);
				vs.rd = malloc(need);
				vs.rd_len = vs.rd ? need : 0;
			}
			if (!vs.rd || big_io(pa, vs.rd, need, 0)) {
				printf("venus: enc: can't read the bitstream at %#x\n", pa);
			} else {
				struct venc_out o = { vs.rd + off, len, flags, ptype, tag, ((uint64_t)p[3] << 32) | p[4] };
				if (flags & VE_F_CODECCFG)
					vs.cfgs++;
				else
					vs.outs++, dc.frames++;
				if (venc_sink)
					venc_sink(&o);
			}
		}
		if (flags & F_EOS) {
			dc.eos = 1;
			printf("venus: enc: EOS back after %u frames\n", vs.outs);
		}
		if (b && !dc.eos && !dc.err)
			ftb(0, b);
	} else if (p[1] == M_EVENT && n >= 6) {
		if (p[3] == EV_SESSION_ERR) {
			printf("venus: enc session error %#x data2 %#x\n", p[4], p[5]);
			dc.err = (int)p[4];
		} else {
			printf("venus: enc event %#x data %#x %#x\n", p[3], p[4], p[5]);
		}
	} else {
		printf("venus: enc msg %#x size %u\n", p[1], p[0]);
	}
	return 0;
}

int venc_open(const struct venc_params *pp)
{
	struct venc_params P = *pp;
	int W = P.w, H = P.h;
	if (W < 32 || H < 32 || W > 4096 || H > 2304 || (W & 1) || (H & 1)) {
		printf("venus: enc: refused: frame size %dx%d (even, 32..4096 x 32..2304)\n", W, H);
		return 1;
	}
	if (!P.fps_q16)
		P.fps_q16 = 30u << 16;
	unsigned fps = (P.fps_q16 + 0x8000) >> 16;
	if (!fps)
		fps = 1;
	if (P.gop <= 0)
		P.gop = (int)fps * 2;
	if (!P.qp_i)
		P.qp_i = 26;
	if (!P.qp_p)
		P.qp_p = P.qp_i + 2;
	if (P.nin <= 0)
		P.nin = 8;
	if (P.nout <= 0)
		P.nout = 8;
	P.nin = P.nin > 8 ? 8 : P.nin;
	P.nout = P.nout > 32 ? 32 : P.nout;
	memset(&vs.own, 0, sizeof(vs.own));
	vs.p = P;
	vs.outs = vs.cfgs = vs.ins = 0;
	struct h264_sps sps = { .w = W, .h = H, .cw = W, .ch = H };
	uint64_t mb = getenv("VENUSCTL_BUF_MB") ? strtoull(getenv("VENUSCTL_BUF_MB"), NULL, 0) : 128;
	double t0 = mono_ms();
	if (vdec_prepare(P.debug, 0, mb, 1 << 30, &sps, P.hevc))
		return 1;
	dc.split = 0;
	uint32_t codec = P.hevc ? VD_HFI_CODEC_HEVC : VE_CODEC_H264;
	printf("venus: enc open %dx%d %s, %.2f fps, QP %u/%u, gop %d, profile %#x level %#x, seqhdr %d, %d+%d buffers%s\n",
	       W, H, P.hevc ? "HEVC" : "H.264", P.fps_q16 / 65536.0, P.qp_i, P.qp_p, P.gop, P.profile, P.level, P.seqhdr,
	       P.nin, P.nout, vmmu_active() ? ", vmmu" : ", window");
	uint32_t si[5] = { 20, 0x10007, SESSION_ID, VE_DOMAIN_ENC, codec };
	if (q_write(si) || wait_msg(0x20006, 1000))
		return 1;
	if (pkt[3]) {
		printf("venus: enc SESSION_INIT_DONE error %#x\n", pkt[3]);
		return 1;
	}
	on_pkt = venc_pkt;
	vc_bus_session((unsigned)W, (unsigned)H, fps);
	uint32_t prof = P.profile ? (uint32_t)P.profile : P.hevc ? 0x1u : 0x4u;	/* HEVC Main / H.264 High */
	uint32_t lvl = (uint32_t)P.level;	/* 0: the firmware's choice (stock) */
	if (P.hevc && lvl && !(lvl >> 28))
		lvl |= 1u << 28;	/* main tier */
	uint32_t layer = 0xffu;
	uint32_t qp[6] = { (P.qp_i & 0xff) | (P.qp_p & 0xff) << 8 | (P.qp_p & 0xff) << 16, layer, 7, 0, 0, 0 };
	uint32_t sig[7] = { 1, 5, P.full_range ? 1u : 0u, 1, (uint32_t)P.primaries, (uint32_t)P.transfer, (uint32_t)P.matrix };
	uint32_t opr = P.fps_q16;
	if (ve_prop("input-size", P_FRAME_SIZE, B_INPUT, (uint32_t)W, (uint32_t)H, 3) ||
	    ve_prop("output-size", P_FRAME_SIZE, B_OUTPUT, (uint32_t)W, (uint32_t)H, 3) ||
	    ve_prop("frame-rate", VE_P_FRAME_RATE, B_OUTPUT, P.fps_q16, 0, 2) ||
	    ve_prop("realtime", VE_P_REALTIME, 1, 0, 0, 1) ||
	    ve_prop("input-nv12", P_FMT_SEL, B_INPUT, CF_NV12, 0, 2) ||
	    ve_prop("seq-header", VE_P_SEQHDR, P.seqhdr ? 1u : 0u, 0, 0, 1) ||
	    ve_prop("profile-level", P_PROFILE, prof, lvl, 0, 2) ||
	    (!P.hevc && (prof == 0x4u || prof == 0x40u) && ve_prop("8x8", VE_P_8X8, 1, 0, 0, 1)) ||
	    (!P.hevc && prof != 0x1u && prof != 0x20u && ve_prop("cabac", VE_P_ENTROPY, 2, 3, 0, 2)) ||
	    ve_prop("rate-control", VE_P_RC, VE_RC_OFF, 0, 0, 1) ||
	    ve_prop("bitrate-savings", VE_P_CAC, 0, 0, 0, 1) ||
	    ve_prop("rc-timestamp", VE_P_RC_TS, 0, 0, 0, 1) ||
	    ve_prop("startcodes", VE_P_NALFMT, 1, 0, 0, 1) ||
	    ve_prop("idr-period", VE_P_IDR, 1, 0, 0, 1) ||
	    ve_prop("intra-period", VE_P_INTRA, P.gop > 1 ? (uint32_t)P.gop - 1 : 0, 0, 0, 2) ||
	    ve_propv("frame-qp", VE_P_FRAMEQP, qp, 6) ||
	    (P.primaries && ve_propv("signal-info", VE_P_SIGNAL, sig, 7)) ||
	    ve_prop("blur", VE_P_BLUR, B_INPUT, (uint32_t)W, (uint32_t)H, 3) ||
	    ve_propv("operating-rate", VE_P_OPRATE, &opr, 1))
		return 2;
	if (get_bufreq())
		return 2;
	uint32_t *ri = req_of(B_INPUT), *ro = req_of(B_OUTPUT);
	uint32_t aw = align_up((uint32_t)W, 32), ah = align_up((uint32_t)H, 32), osz = aw * ah * 3;
	if ((aw / 16) * (ah / 16) > 1620)
		osz >>= 2;
	osz <<= 1;	/* RC off */
	if (P.hevc)
		osz += osz >> 2;
	if (ro && ro[1] > osz)
		osz = ro[1];
	osz = align_up(osz, 4096);
	uint32_t isz = ve_insize();
	if (ri && ri[1] > isz)
		isz = align_up(ri[1], 4096);
	uint32_t perf = 1;
	if (ve_prop("input-count", P_BUF_COUNT, B_INPUT, (uint32_t)P.nin, (uint32_t)P.nin, 3) ||
	    ve_prop("output-count", P_BUF_COUNT, B_OUTPUT, (uint32_t)P.nout, (uint32_t)P.nout, 3) ||
	    ve_prop("work-mode", P_WORK_MODE, 2, 0, 0, 1) ||
	    ve_prop("work-route", P_WORK_ROUTE, 2, 0, 0, 1) ||
	    ve_propv("perf-mode", VE_P_PERF, &perf, 1) ||
	    get_bufreq() ||
	    ve_prop("output-size-min", P_BUF_SIZE_MIN, B_OUTPUT, osz, 0, 2))
		return 2;
	static const uint32_t itypes[] = { 6, 7, 8, 4, 5 };
	for (unsigned t = 0; t < 5; t++) {
		uint32_t type = itypes[t], *r = req_of(type), cnt = r ? r[5] : 0;
		if (r && r[1] && !cnt && r[3])
			cnt = r[3];
		for (uint32_t i = 0; r && r[1] && i < cnt && dc.nintl < 16; i++) {
			uint32_t pa = balloc(r[1], bname(type));
			if (!pa || buf_cmd(type, pa, r[1], 0))
				return 2;
			dc.intl[dc.nintl] = (struct vbuf){ pa, r[1], 1 };
			dc.intl_type[dc.nintl++] = type;
		}
	}
	if (wait_msg(0, 20) || dc.err)
		return 2;
	for (dc.nin = 0; dc.nin < P.nin; dc.nin++) {
		uint32_t pa = balloc(isz, "enc input");
		if (!pa)
			return 2;
		vs.own[dc.nin] = pa;
		dc.in[dc.nin] = (struct vbuf){ pa, isz, 0 };
	}
	for (dc.nopb = 0; dc.nopb < P.nout; dc.nopb++) {
		uint32_t pa = balloc(osz, "bitstream");
		if (!pa)
			return 2;
		dc.opb[dc.nopb] = (struct vbuf){ pa, osz, 0 };
	}
	vs.isz = isz, vs.osz = osz;
	dc.isz = isz;
	if (sess_cmd(S_LOAD_RES) || wait_msg(M_LOAD_DONE, 1000))
		return 2;
	if (sess_cmd(S_START))
		return 2;
	dc.started = 1;
	if (wait_msg(M_START_DONE, 1000) || dc.err)
		return 2;
	for (int i = 0; i < dc.nopb; i++)
		if (ftb(0, &dc.opb[i]))
			return 2;
	printf("venus: enc running in %.0f ms: %d inputs of %u bytes (NV12 stride %u, scanlines %u/%u), %d bitstream buffers"
	       " of %u bytes\n", mono_ms() - t0, dc.nin, isz, dc.ystride, dc.yscan, ve_uvscan(), dc.nopb, osz);
	return 0;
}

int venc_free_slot(void)
{
	for (int i = 0; i < dc.nin; i++)
		if (!dc.in[i].fw)
			return i;
	return -1;
}

/* the CPU pointer of slot k's own buffer (vmmu only; NULL on the window path) and its Venus NV12 layout */
uint8_t *venc_slot_cpu(int k, uint32_t *size, uint32_t *stride, uint32_t *yscan)
{
	if (k < 0 || k >= dc.nin)
		return NULL;
	if (size)
		*size = vs.isz;
	if (stride)
		*stride = dc.ystride;
	if (yscan)
		*yscan = dc.yscan;
	return vmmu_cpu(vs.own[k], vs.isz);
}

/* the Venus NV12 layout this session wants: stride, Y scanlines, the CbCr plane's offset */
void venc_layout(uint32_t *stride, uint32_t *yscan, uint32_t *uvoff, uint32_t *size)
{
	*stride = dc.ystride, *yscan = dc.yscan, *uvoff = dc.ystride * dc.yscan, *size = ve_insize();
}

/* one picture: slot k; pa 0 = the slot's own buffer (written through venc_slot_cpu: cleaned here), else that IOVA
 * (alloc bytes there, already clean); sync: an IDR now */
int venc_etb(int k, uint32_t pa, uint32_t alloc, uint64_t ts_us, uint32_t tag, int sync)
{
	return venc_etb_off(k, pa, 0, alloc, ts_us, tag, sync);
}

/* the same with the picture at offset off in the buffer at pa (ETB offset, as V4L2's data_offset downstream) */
int venc_etb_off(int k, uint32_t pa, uint32_t off, uint32_t alloc, uint64_t ts_us, uint32_t tag, int sync)
{
	if (k < 0 || k >= dc.nin || dc.in[k].fw)
		return -1;
	uint32_t fill = ve_insize();
	if (!pa) {
		pa = vs.own[k];
		alloc = vs.isz;
		if (vmmu_cpu(pa, fill) && vmmu_sync(pa, fill, 1))
			return -1;
	}
	if (fill + off > alloc)
		fill = alloc > off ? alloc - off : 0;
	if (sync) {
		uint32_t one = 1;
		uint32_t p[6] = { 24, S_SET_PROP, SESSION_ID, 1, VE_P_SYNCREQ, one };
		if (q_write(p))
			return -1;
	}
	uint32_t e[16] = { 64, S_ETB, SESSION_ID, 0, (uint32_t)(ts_us >> 32), (uint32_t)ts_us, 0, 0, 0, alloc, fill, off, tag,
		pa, 0, 0 };
	dc.in[k].pa = pa;
	dc.in[k].size = alloc;
	dc.in[k].fw = 1;
	vs.ins++;
	return q_write(e) ? -1 : 0;
}

int venc_eos(void)
{
	int k = venc_free_slot();
	if (k < 0)
		return -1;
	uint32_t e[16] = { 64, S_ETB, SESSION_ID, 0, 0, 0, F_EOS, 0, 0, vs.isz, 0, 0, 0, vs.own[k], 0, 0 };
	dc.in[k].pa = vs.own[k];
	dc.in[k].fw = 1;
	return q_write(e) ? -1 : 0;
}

/* dynamic: a new constant QP (I, P) */
int venc_set_qp(uint32_t qi, uint32_t qpp)
{
	uint32_t qp[6] = { (qi & 0xff) | (qpp & 0xff) << 8 | (qpp & 0xff) << 16, 0xffu, 7, 0, 0, 0 };
	uint32_t p[11] = { 44, S_SET_PROP, SESSION_ID, 1, VE_P_FRAMEQP };
	memcpy(p + 5, qp, sizeof(qp));
	return q_write(p) ? -1 : 0;
}

uint32_t venc_fps_q16(void)
{
	return vs.p.fps_q16;
}
