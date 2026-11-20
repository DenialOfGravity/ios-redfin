/*
 * ws-vt-venc: venusd's ENCODER sessions (venus_proto.h VD_ENC_*), included by venusd.c after open_common().
 * One Venus session at a time (decoder or encoder), the same owner/sess state machine, the same close (vdec_close), the
 * same power collapse after it. venus_enc.inc's step API does the HFI side.
 * Input per ENC_FRAME: the client's IOSurface by id (vsurf_get: looked up once, kept for the session).
 *  - zero-copy: a surface already in Venus's NV12 layout (plane 0 at offset 0, both planes' bytes per row = the Venus
 *    stride, the CbCr plane at stride x Y scanlines, big enough) is mapped into the Venus bank by id (vmmu_map_surface,
 *    once per surface, up to EMAP_MAX), cleaned (the producer may have written it through the CPU) and given to the
 *    firmware as is; ENC_INDONE when Venus hands it back (EBD).
 *  - copy: any other 420v/420f layout: the planes are copied (CPU, read-only lock) into the slot's own Venus buffer;
 *    ENC_INDONE at once.
 *  Switches (env of venusd): VENUSD_ENC_ZC (1; 0: always copy), VENUSD_ENC_SEQHDR (unset: the client's choice),
 *  VENUSD_ENC_QP=I[:P] (overrides the client's), VENUSD_ENC_LOG (1: one line per frame).
 */
#define EMAP_MAX	24	/* zero-copy surfaces mapped per session */
#define ETAG_N		64	/* frames in flight we remember (tag -> seq/pts) */

static struct { uint32_t id, iova, len; int busy; unsigned long use; } emap[EMAP_MAX];
static unsigned long emap_clock;
static struct { uint32_t tag, seq, surface; uint64_t pts; int zc; } etag[ETAG_N];
static uint32_t etag_next = 1;
static unsigned enc_zc, enc_copy, enc_bad, enc_out, enc_cfg;
static double enc_copy_ms, enc_copy_max;
static int enc_zc_on = 1, enc_log;
static unsigned enc_qp_n;
static uint32_t enc_w, enc_h, enc_qi, enc_qp;

static double enc_ms(void)
{
	return mono() * 1e3;
}

/* the session's end (or a new one): unmap the zero-copy surfaces, forget the tags */
static void enc_reset(void)
{
	for (int k = 0; k < EMAP_MAX; k++)
		if (emap[k].iova)
			vmmu_unmap(emap[k].iova);
	memset(emap, 0, sizeof(emap));
	memset(etag, 0, sizeof(etag));
	enc_zc = enc_copy = enc_bad = enc_out = enc_cfg = enc_qp_n = 0;
	enc_copy_ms = enc_copy_max = 0;
}

/* the client's bit rate -> a constant QP for rate control off: ~bits per pixel, H.264 QP 30 at 0.1 bpp, 6 QP per
 * doubling (the usual log rule); clamped to 16..42 */
static void enc_qp_for(uint32_t bitrate, uint32_t w, uint32_t h, double fps, int hevc, uint32_t *qi, uint32_t *qp)
{
	double bpp = bitrate && w && h && fps > 0 ? bitrate / ((double)w * h * fps) : 0.1;
	double q = 30.0 - 6.0 * log2(bpp / 0.1) - (hevc ? 2.0 : 0.0);
	int v = (int)lround(q);
	v = v < 16 ? 16 : v > 42 ? 42 : v;
	*qi = (uint32_t)(v - 1), *qp = (uint32_t)(v + 1);
}

static void enc_reply_open(int i, int32_t status)
{
	struct venusd_msg r = { .type = VD_ENC_OPEN, .status = status };
	if (!status) {
		uint32_t st, ys, uvo, sz;
		venc_layout(&st, &ys, &uvo, &sz);
		r.stride = st, r.scanlines = ys, r.crop_x = uvo, r.filled = sz, r.width = enc_w, r.height = enc_h;
		r.fbd_w = enc_qi, r.fbd_h = enc_qp;	/* the starting constant QP (I, P) */
	}
	send_msg(i, &r, NULL);
}

static void do_enc_open(int i, const struct venusd_msg *m, const uint8_t *payload)
{
	struct venusd_enc_open eo;
	memset(&eo, 0, sizeof(eo));
	if (m->len)
		memcpy(&eo, payload, m->len < sizeof(eo) ? m->len : sizeof(eo));
	int32_t rc = open_common(i, m, 1);
	if (rc) {
		enc_reply_open(i, rc);
		return;
	}
	enc_reset();
	owner = i;
	sess_enc = 1;
	sess = S_OPEN;
	nsess++;
	sent = 0;
	hint_w = m->width, hint_h = m->height;
	sess_hevc = m->codec == VD_CODEC_HEVC;
	t_open = mono();
	t_first = t_last_frame = 0;
	t_run = t_etb = t_ebd = 0, stalled = 0;
	cred_drop();
	n_placed = n_copied = n_returned = path_logged = 0;
	wall_due = 0;
	nopay = 0;
	enc_zc_on = (!getenv("VENUSD_ENC_ZC") || atoi(getenv("VENUSD_ENC_ZC"))) && !(eo.flags & 1);
	enc_log = getenv("VENUSD_ENC_LOG") && atoi(getenv("VENUSD_ENC_LOG"));
	struct venc_params p = { .w = (int)m->width, .h = (int)m->height, .hevc = sess_hevc, .fps_q16 = eo.fps_q16,
		.qp_i = eo.qp_i, .qp_p = eo.qp_p, .gop = eo.gop, .profile = eo.profile, .level = eo.level, .seqhdr = eo.seqhdr,
		.full_range = eo.full_range, .primaries = eo.primaries, .transfer = eo.transfer, .matrix = eo.matrix,
		.debug = debug };
	double fps = eo.fps_q16 ? eo.fps_q16 / 65536.0 : 30.0;
	if (!p.qp_i)
		enc_qp_for(eo.bitrate, m->width, m->height, fps, sess_hevc, &p.qp_i, &p.qp_p);
	const char *e = getenv("VENUSD_ENC_QP");
	if (e && *e) {
		unsigned a = 0, b = 0;
		int n = sscanf(e, "%u:%u", &a, &b);
		if (n >= 1 && a)
			p.qp_i = a, p.qp_p = n == 2 && b ? b : a + 2;
	}
	if (getenv("VENUSD_ENC_SEQHDR"))
		p.seqhdr = atoi(getenv("VENUSD_ENC_SEQHDR"));
	enc_w = m->width, enc_h = m->height, enc_qi = p.qp_i, enc_qp = p.qp_p;
	note("venusd: session %u: encoder opened (fd %d, %s %ux%u, %.2f fps, %u bit/s -> QP %u/%u, gop %d, profile %d level %d,"
	     " seqhdr %d, colour %d/%d/%d full %d, zero-copy %s)", nsess, cl[i].fd, sess_hevc ? "HEVC" : "H.264", m->width,
	     m->height, fps, eo.bitrate, p.qp_i, p.qp_p, p.gop, p.profile, p.level, p.seqhdr, p.primaries, p.transfer,
	     p.matrix, p.full_range, enc_zc_on ? "on" : "off");
	/* the 20 ms drain after each property (venusctl's, for reading a refusal next to its property) made the open ~1.2 s;
	 * a refusal still shows (session error at the next drain) */
	vc_prop_ms = getenv("VENUSD_ENC_PROP_MS") ? atoi(getenv("VENUSD_ENC_PROP_MS")) : 2;
	double t_vo = mono();
	int r = venc_open(&p);
	vc_prop_ms = -1;
	note("venusd: session %u: venc_open %.0f ms", nsess, 1e3 * (mono() - t_vo));
	if (r) {
		struct vdec_status st;
		vdec_status(&st);
		hfi = r == 2;
		int32_t err = st.err ? st.err : -EIO;
		enc_reply_open(i, err);
		sess = S_FAILED;
		sess_stop();
		sess_log("encoder open failed");
		return;
	}
	hfi = 1;
	sess = S_RUN;
	t_prog = t_run = mono();
	struct vdec_status st;
	vdec_status(&st);
	stall_ebds = st.ebds;
	note("venusd: session %u: encoder running %.0f ms after ENC_OPEN", nsess, 1e3 * (mono() - t_open));
	enc_reply_open(i, 0);
}

static void enc_indone(uint32_t seq, uint32_t surface, int32_t status, int zc)
{
	struct venusd_msg r = { .type = VD_ENC_INDONE, .seq = seq, .surface = surface, .status = status,
		.flags = zc ? 1u : 0u };
	send_msg(owner, &r, NULL);
}

/* venus_core: an input buffer back; the zero-copy frame's surface is free again */
static void enc_in_done(uint32_t tag, uint32_t pa)
{
	(void)pa;
	if (!sess_enc)
		return;
	for (int k = 0; k < ETAG_N; k++)
		if (etag[k].tag == tag && tag) {
			if (etag[k].zc) {
				for (int j = 0; j < EMAP_MAX; j++)
					if (emap[j].id == etag[k].surface && emap[j].busy)
						emap[j].busy--;
				enc_indone(etag[k].seq, etag[k].surface, 0, 1);
			}
			etag[k].zc = 0;	/* the tag stays for the FBD (seq, pts) */
			return;
		}
}

/* venus_core: one bitstream buffer -> ENC_DATA */
static void enc_sink(const struct venc_out *o)
{
	if (!sess_enc)
		return;
	struct venusd_msg r = { .type = VD_ENC_DATA, .len = o->len, .flags = o->flags, .pic_type = o->pic_type };
	if (o->flags & 0x80u) {
		enc_cfg++;
	} else {
		for (int k = 0; k < ETAG_N; k++)
			if (etag[k].tag == o->tag && o->tag) {
				r.seq = etag[k].seq, r.pts = etag[k].pts;
				etag[k].tag = 0;
				break;
			}
		if (!enc_out++)
			t_first = mono();
		t_last_frame = mono();
	}
	r.frames = enc_out;
	if (enc_log)
		printf("venusd: enc out seq %u: %u bytes flags %#x pic %#x\n", r.seq, o->len, o->flags, o->pic_type);
	send_msg(owner, &r, o->data);
}

/* the surface's layout: 0 = zero-copy can take it; why not otherwise */
static const char *enc_misfit(IOSurfaceRef s, uint32_t *len_out)
{
	uint32_t fmt = IOSurfaceGetPixelFormat(s);
	if (IOSurfaceGetPlaneCount(s) != 2 || (fmt != FCC('4', '2', '0', 'v') && fmt != FCC('4', '2', '0', 'f')))
		return "not 2-plane 420v/420f";
	if (IOSurfaceGetWidthOfPlane(s, 0) != enc_w || IOSurfaceGetHeightOfPlane(s, 0) != enc_h)
		return "size";
	uint32_t st, ys, uvo, sz;
	venc_layout(&st, &ys, &uvo, &sz);
	uint8_t *b = IOSurfaceGetBaseAddress(s), *y = IOSurfaceGetBaseAddressOfPlane(s, 0),
		*c = IOSurfaceGetBaseAddressOfPlane(s, 1);
	if (!b || !y || !c)
		return "no base address";
	size_t yo = (size_t)(y - b);
	if (yo & 0xfff)
		return "plane 0 not page aligned in the surface";	/* the Venus buffer starts at plane 0 */
	if (IOSurfaceGetBytesPerRowOfPlane(s, 0) != st || IOSurfaceGetBytesPerRowOfPlane(s, 1) != st)
		return "bytes per row";
	if ((size_t)(c - y) != uvo)
		return "CbCr offset";
	if (IOSurfaceGetAllocSize(s) < yo + (size_t)uvo + (size_t)st * ((enc_h + 1) / 2))
		return "alloc size";
	*len_out = (uint32_t)yo;	/* (the caller adds the mapped length) */
	return NULL;
}

/* a mapped zero-copy surface (once per surface per session; the least recently used idle one goes) */
static int enc_map(uint32_t id, uint32_t *iova, uint32_t *len)
{
	int lru = -1;
	for (int k = 0; k < EMAP_MAX; k++) {
		if (emap[k].iova && emap[k].id == id) {
			emap[k].use = ++emap_clock;
			*iova = emap[k].iova, *len = emap[k].len;
			return k;
		}
		if (!emap[k].iova)
			lru = lru < 0 || emap[lru].iova ? k : lru;
		else if (!emap[k].busy && (lru < 0 || (emap[lru].iova && emap[k].use < emap[lru].use)))
			lru = k;
	}
	if (lru < 0)
		return -1;
	if (emap[lru].iova) {
		vmmu_unmap(emap[lru].iova);
		emap[lru].iova = 0;
	}
	uint32_t info[4], l = 0, v = vmmu_map_surface(id, &l, info);
	if (!v)
		return -1;
	emap[lru] = (typeof(emap[0])){ id, v, l, 0, ++emap_clock };
	*iova = v, *len = l;
	return lru;
}

/* ENC_FRAME: the surface into a free input slot (the main loop reads only when one is free) */
static void enc_frame(const struct venusd_msg *m)
{
	int k = venc_free_slot();
	IOSurfaceRef s = m->surface ? vsurf_get(m->surface) : NULL;
	if (k < 0 || !s) {
		if (enc_bad++ < 5)
			printf("venusd: enc frame %u: %s\n", m->seq, k < 0 ? "no free input (dropped)" : "surface lookup failed");
		enc_indone(m->seq, m->surface, k < 0 ? -ENOSPC : -ENOENT, 0);
		return;
	}
	uint32_t tag = etag_next++;
	if (!etag_next)
		etag_next = 1;
	int slot = (int)(tag % ETAG_N);
	etag[slot] = (typeof(etag[0])){ tag, m->seq, m->surface, m->pts, 0 };
	uint32_t len = 0, iova = 0, yo = 0;
	const char *why = enc_misfit(s, &yo);
	int j = -1;
	if (!why && enc_zc_on) {
		j = enc_map(m->surface, &iova, &len);
		if (j >= 0 && emap[j].busy)
			j = -1, why = "already with Venus";	/* the same surface twice in flight: copy this one */
		else if (j < 0)
			why = "map failed";
	} else if (!why) {
		why = "zero-copy off";
	}
	uint64_t ts = (uint64_t)sent * 1000000ull * 65536ull / (venc_fps_q16() ? venc_fps_q16() : (30u << 16));
	int idr = (m->flags & 1) != 0;
	if (j >= 0) {
		uint32_t st, ys, uvo, sz;
		venc_layout(&st, &ys, &uvo, &sz);
		iova += yo, len -= yo;	/* the Venus buffer = plane 0 on */
		if (vmmu_sync(iova, sz < len ? sz : len, 1)) {
			enc_indone(m->seq, m->surface, -EIO, 0);
			return;
		}
		emap[j].busy++;
		etag[slot].zc = 1;
		if (venc_etb(k, iova, len, ts, tag, idr)) {
			emap[j].busy--;
			sess_fail(-EIO, m->seq);
			return;
		}
		if (!enc_zc++)
			note("venusd: session %u: frame %u zero-copy from IOSurface %u (plane 0 at IOVA %#x, +%u in the surface,"
			     " %u bytes)", nsess, m->seq, m->surface, iova, yo, len);
	} else {
		double t0 = enc_ms();
		uint32_t size, S, YS;
		uint8_t *dst = venc_slot_cpu(k, &size, &S, &YS);
		uint32_t fmt = IOSurfaceGetPixelFormat(s);
		if (!dst || IOSurfaceGetPlaneCount(s) != 2 || (fmt != FCC('4', '2', '0', 'v') && fmt != FCC('4', '2', '0', 'f')) ||
		    IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL)) {
			if (enc_bad++ < 5)
				printf("venusd: enc frame %u: surface %u can't be read (%s)\n", m->seq, m->surface,
				       !dst ? "no CPU buffer" : "format/lock");
			enc_indone(m->seq, m->surface, -EINVAL, 0);
			return;
		}
		size_t w = IOSurfaceGetWidthOfPlane(s, 0), h = IOSurfaceGetHeightOfPlane(s, 0);
		size_t cw = IOSurfaceGetWidthOfPlane(s, 1) * 2, ch = IOSurfaceGetHeightOfPlane(s, 1);
		w = w < enc_w ? w : enc_w, h = h < enc_h ? h : enc_h;
		cw = cw < w ? cw : w, ch = ch < (h + 1) / 2 ? ch : (h + 1) / 2;
		const uint8_t *y = IOSurfaceGetBaseAddressOfPlane(s, 0), *c = IOSurfaceGetBaseAddressOfPlane(s, 1);
		size_t yb = IOSurfaceGetBytesPerRowOfPlane(s, 0), cb = IOSurfaceGetBytesPerRowOfPlane(s, 1);
		if (yb == S && w == enc_w && w + 128 > S) {
			memcpy(dst, y, yb * h);
		} else {
			for (size_t r = 0; r < h; r++)
				memcpy(dst + r * S, y + r * yb, w);
		}
		uint8_t *dc_ = dst + (size_t)S * YS;
		if (cb == S && cw == enc_w && cw + 128 > S) {
			memcpy(dc_, c, cb * ch);
		} else {
			for (size_t r = 0; r < ch; r++)
				memcpy(dc_ + r * S, c + r * cb, cw);
		}
		IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
		double ms = enc_ms() - t0;
		enc_copy_ms += ms;
		enc_copy_max = ms > enc_copy_max ? ms : enc_copy_max;
		enc_indone(m->seq, m->surface, 0, 0);	/* the client may reuse it now */
		if (venc_etb(k, 0, 0, ts, tag, idr)) {
			sess_fail(-EIO, m->seq);
			return;
		}
		if (!enc_copy++)
			note("venusd: session %u: frame %u copied from IOSurface %u (%s; %zux%zu, bytes per row %zu/%zu, planes at +%td"
			     " +%td, %zu bytes, %.2f ms)", nsess, m->seq, m->surface, why, w, h, yb, cb,
			     y - (const uint8_t *)IOSurfaceGetBaseAddress(s), c - (const uint8_t *)IOSurfaceGetBaseAddress(s),
			     IOSurfaceGetAllocSize(s), ms);
	}
	sent++;
	t_etb = mono();
	if (enc_log)
		printf("venusd: enc frame %u -> slot %d tag %u%s\n", m->seq, k, tag, j >= 0 ? " zero-copy" : " copied");
}

static void enc_command(int i, const struct venusd_msg *m)
{
	(void)i;
	if (m->type == VD_ENC_FRAME) {
		if (sess != S_RUN) {
			enc_indone(m->seq, m->surface, -EINVAL, 0);
			return;
		}
		enc_frame(m);
		return;
	}
	if (m->type == VD_ENC_QP) {
		if (sess == S_RUN && m->crop_x) {
			venc_set_qp(m->crop_x, m->crop_y ? m->crop_y : m->crop_x + 2);
			if (enc_log || enc_qp_n++ < 3)
				note("venusd: session %u: QP now %u/%u (frame %u in)", nsess, m->crop_x,
				     m->crop_y ? m->crop_y : m->crop_x + 2, sent);
		}
		return;
	}
	if (m->type == VD_EOS) {
		if (sess != S_RUN || venc_eos()) {
			sess_fail(sess == S_RUN ? -EIO : -EINVAL, m->seq);
			return;
		}
		sess = S_EOS;
		t_eos = t_prog = mono();
		return;
	}
	reply(i, VD_ERROR, -EINVAL, m->seq);
}

/* the session's numbers, at its end (sess_log adds the general line) */
static void enc_log_end(void)
{
	if (!sess_enc)
		return;
	note("venusd: session %u encoder: %u frames in (%u zero-copy, %u copied: mean %.2f ms max %.2f ms), %u out, %u codec"
	     " config, %u refused", nsess, sent, enc_zc, enc_copy, enc_copy ? enc_copy_ms / enc_copy : 0.0, enc_copy_max,
	     enc_out, enc_cfg, enc_bad);
}
