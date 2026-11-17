/*
 * ws-qsee-b: load / send / unload of one QSEE app, included by qseeprobe.c (uses its say(), qsmc(), lookup_one()).
 * Sequence as Linux 4.19 redbull drivers/misc/qseecom.c (QSEE >= 4.0, 64-bit forms, reentrancy phase 2):
 *   load:   __qseecom_load_fw 4750-4888: one contiguous buffer = NAME.mdt followed by NAME.b00..b(e_phnum-1);
 *           TZ_OS_APP_START x2 mdt_len, x3 img_len (mdt + all segments), x4 pa. SUCCESS -> x3 = app id.
 *           cmnlib64 is not loaded first: the DT has qcom,commonlib64-loaded-by-uefi (qseecom.c 9459) and the app
 *           region notification is skipped because ABL passes androidboot.keymaster=1 (is_apps_region_protected).
 *   send:   __qseecom_send_cmd 3710-3876 + qseecom_scm_call2: x2 app_id, x3 req pa, x4 req len, x5 = pa of a page
 *           holding args[3..9] (scm.c allocate_extra_arg_buffer: rsp pa, rsp len[, sglistinfo pa, len]).
 *           With TZ whitelist feature >= 1.0.0 Linux uses SEND_DATA_WITH_WHITELIST and a 4-entry sglistinfo table
 *           naming the modfd offsets (here: one buffer pa patched at sb+4, as the FPC HAL's send_modified_cmd).
 *   unload: __qseecom_unload_app: TZ_OS_APP_SHUTDOWN x2 app_id.
 *   INCOMPLETE (x1 = 1): TZ wants listener x3. We have none, so we answer as Linux does for a listener that
 *           does not exist (__qseecom_process_incomplete_cmd 2117-2300: "Listener Svc %d does not exist"):
 *           TZ_OS_LISTENER_RESPONSE_HANDLER x2 listener id, x3 QSEOS_RESULT_FAILURE, and loop on the new result.
 *   BLOCKED_ON_LISTENER (x1 = 2): x3 listener, x2 session; Linux waits for the listener to be free, then
 *           TZ_OS_CONTINUE_BLOCKED_REQUEST_SMCINVOKE(session) (falls back to ..._REQUEST(app id)), which comes
 *           back INCOMPLETE and is answered as above.
 * Every listener id met is kept in seen_lstnr[] and printed, and the exit code is 4 when one was met.
 */
#define MAX_ABORTS 6
static uint32_t seen_lstnr[8];
static unsigned n_seen;

static const char *lstnr_name(uint32_t id)
{
	switch (id) {
	case RPMB_SERVICE: return "RPMB_SERVICE";
	case SSD_SERVICE: return "SSD_SERVICE";
	default: return "unknown to qseecom.c (qseecomd/HAL listener)";
	}
}

static void note_lstnr(uint32_t id)
{
	for (unsigned i = 0; i < n_seen; i++)
		if (seen_lstnr[i] == id)
			return;
	if (n_seen < 8)
		seen_lstnr[n_seen++] = id;
}

/* x[] holds the result of an app-manager or send-data SMC; resolve INCOMPLETE/BLOCKED by refusing the listener. */
static void abort_listeners(uint64_t x[8], uint32_t app_id, const char *what)
{
	for (int i = 0; i < MAX_ABORTS; i++) {
		if ((int64_t)x[0])
			return;
		uint32_t result = (uint32_t)x[1], type = (uint32_t)x[2], data = (uint32_t)x[3];
		if (result == QSEOS_RESULT_BLOCKED) {
			note_lstnr(data);
			say("%s: BLOCKED_ON_LISTENER: listener %#x (%s), session %#x: continue-blocked", what, data,
			    lstnr_name(data), type);
			uint64_t y[8] = { QSEE_SMC_CONT_BLOCKED_SI, QSEE_ARGS_CONT_BLOCKED, type };
			if (qsmc(y)) {
				uint64_t z[8] = { QSEE_SMC_CONT_BLOCKED, QSEE_ARGS_CONT_BLOCKED, app_id };
				qsmc(z);
				memcpy(x, z, 4 * sizeof(uint64_t));
			} else {
				memcpy(x, y, 4 * sizeof(uint64_t));
			}
			continue;
		}
		if (result == QSEOS_RESULT_INCOMPLETE) {
			note_lstnr(data);
			say("%s: INCOMPLETE: TZ requests listener %#x (%u, %s), resp_type %#x; no listener here: answering "
			    "QSEOS_RESULT_FAILURE", what, data, data, lstnr_name(data), type);
			uint64_t y[8] = { QSEE_SMC_LSTNR_RESP, QSEE_ARGS_LSTNR_RESP, data, QSEOS_RESULT_FAILURE };
			qsmc(y);
			memcpy(x, y, 4 * sizeof(uint64_t));
			continue;
		}
		return;
	}
	say("%s: still INCOMPLETE/BLOCKED after %d aborts, giving up", what, MAX_ABORTS);
}

static void print_seen(void)
{
	for (unsigned i = 0; i < n_seen; i++)
		say("LISTENER REQUESTED: id %#x (%u) %s", seen_lstnr[i], seen_lstnr[i], lstnr_name(seen_lstnr[i]));
}

/* registration of one pmem buffer with the shmbridge (as qtee_shmbridge_register, VMID_HLOS R|W) */
static int bridge_reg(uint64_t pa, uint64_t size, uint64_t *handle)
{
	size = (size + 4095) & ~4095ull;
	uint64_t x[8] = { QSEE_SMC_SHMB_CREATE, QSEE_ARGS_SHMB_CREATE, pa | SHMB_PERM_RW, pa | SHMB_PERM_RW,
			  size | (1ull << SHMB_NUM_VM_SHIFT), VMID_HLOS };
	int64_t rc = qsmc(x);
	if (rc || x[1]) {
		say("shmbridge: create %#llx+%#llx failed (x0 %lld, ret0 %#llx)", (unsigned long long)pa,
		    (unsigned long long)size, (long long)rc, (unsigned long long)x[1]);
		return -1;
	}
	*handle = x[2];
	say("shmbridge: %#llx+%#llx registered, handle %#llx", (unsigned long long)pa, (unsigned long long)size,
	    (unsigned long long)*handle);
	return 0;
}

static int bridge_dereg(uint64_t handle)
{
	uint64_t x[8] = { QSEE_SMC_SHMB_DELETE, QSEE_ARGS_SHMB_DELETE, handle };
	int64_t rc = qsmc(x);
	if (rc || x[1]) {
		say("shmbridge: delete of handle %#llx failed (x0 %lld, ret0 %#llx)", (unsigned long long)handle,
		    (long long)rc, (unsigned long long)x[1]);
		return -1;
	}
	return 0;
}

/* a set of pmem buffers for one command; bridged when QSEE_SHMBRIDGE=1; freed (or leaked if a bridge sticks) */
struct pbuf {
	uint64_t pa, size, handle;
	int bridged;
};

static int pbuf_get(struct pbuf *b, uint64_t size, uint64_t align, int below4g, uint64_t fake)
{
	memset(b, 0, sizeof(*b));
	b->size = (size + 4095) & ~4095ull;
	if (dryrun) {
		b->pa = fake;
		return 0;
	}
	b->pa = below4g ? pm_alloc32(b->size, align) : pm_alloc(b->size, align);
	if (!b->pa && align > 4096)
		b->pa = below4g ? pm_alloc32(b->size, 4096) : pm_alloc(b->size, 4096);
	if (!b->pa)
		return -1;
	if (shmbridge) {
		if (bridge_reg(b->pa, b->size, &b->handle)) {
			pm_free(b->pa);
			b->pa = 0;
			return -1;
		}
		b->bridged = 1;
	}
	return 0;
}

static void pbuf_put(struct pbuf *b)
{
	if (!b->pa)
		return;
	if (b->bridged && bridge_dereg(b->handle)) {
		say("pbuf: %#llx NOT freed (still bridged)", (unsigned long long)b->pa);
		return;
	}
	if (!dryrun)
		pm_free(b->pa);
	b->pa = 0;
}

static int wr_chunks(uint64_t pa, const uint8_t *p, size_t len)
{
	if (dryrun)
		return 0;
	for (size_t o = 0; o < len; o += 65536) {
		size_t n = len - o < 65536 ? len - o : 65536;
		if (pm_wr(pa + o, p + o, n))
			return -1;
	}
	return 0;
}

static uint8_t *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		say("load: %s: %s", path, strerror(errno));
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *p = n > 0 ? malloc((size_t)n) : NULL;
	if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) {
		say("load: %s: read failed", path);
		free(p);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*len = (size_t)n;
	return p;
}

/* lookup in its own name page; 0 loaded (*id set), 1 not loaded, 2 error, 3 stop */
static int lookup_name(const char *name, uint32_t *id)
{
	struct pbuf b;
	lookup_app_id = 0;
	if (pbuf_get(&b, 4096, 4096, 0, 0xdead0000ull))
		return 2;
	buf_pa = b.pa;
	int r = lookup_one(name);
	pbuf_put(&b);
	*id = lookup_app_id;
	return r;
}

/* the guard for send/unload: APPID must be what QSEE_APP (default fpctzappfingerprint) is loaded as */
static int check_appid(uint32_t appid)
{
	const char *name = getenv("QSEE_APP");
	uint32_t id = 0, km = 0;
	if (!name || !*name)
		name = "fpctzappfingerprint";
	if (dryrun)
		return 0;
	if (!lookup_name("keymaster64", &km) && km == appid) {
		say("refused: app id %u is keymaster64", appid);
		return -1;
	}
	if (lookup_name(name, &id) || id != appid) {
		say("refused: %s is not loaded as app id %u (lookup id %u)", name, appid, id);
		return -1;
	}
	return 0;
}

static int cmd_load(const char *name, const char *dir)
{
	char path[512];
	size_t mdt_len, total;
	uint32_t id = 0;
	if (strlen(name) >= QSEE_APP_NAME_MAX || strchr(name, '/')) {
		say("load: bad name %s", name);
		return 2;
	}
	snprintf(path, sizeof(path), "%s/%s.mdt", dir, name);
	uint8_t *mdt = slurp(path, &mdt_len);
	if (!mdt)
		return 2;
	if (mdt_len < 0x40 || memcmp(mdt, "\177ELF", 4)) {
		say("load: %s is not an ELF", path);
		return 2;
	}
	unsigned cls = mdt[4], phnum = cls == 2 ? (unsigned)(mdt[0x38] | mdt[0x39] << 8) : (unsigned)(mdt[0x2c] | mdt[0x2d] << 8);
	if (cls != 2) {
		say("load: %s is ELFCLASS%u: a 32-bit app needs cmnlib loaded first (not done by this tool)", path, cls);
		return 2;
	}
	/* image = mdt + b00..b(phnum-1), as __qseecom_get_fw_data */
	uint8_t *img = malloc(mdt_len);
	memcpy(img, mdt, mdt_len);
	total = mdt_len;
	for (unsigned i = 0; i < phnum; i++) {
		size_t sl;
		snprintf(path, sizeof(path), "%s/%s.b%02u", dir, name, i);
		uint8_t *seg = slurp(path, &sl);
		if (!seg)
			return 2;
		img = realloc(img, total + sl);
		memcpy(img + total, seg, sl);
		total += sl;
		free(seg);
	}
	say("load %s: ELF64, %u segments, mdt_len %#zx, img_len %#zx (%zu)", name, phnum, mdt_len, total, total);
	int lr = lookup_name(name, &id);
	if (lr == 0 && !dryrun) {
		say("load %s: already LOADED as app id %u, not loading again", name, id);
		return 0;
	}
	if (lr >= 2)
		return lr;
	struct pbuf b;
	if (pbuf_get(&b, total, 0x100000, 0, 0xbeef00000ull))
		return 2;
	say("load %s: image buffer pa %#llx size %#llx%s", name, (unsigned long long)b.pa, (unsigned long long)b.size,
	    b.bridged ? " (shmbridge registered)" : " (not bridged)");
	if (wr_chunks(b.pa, img, total)) {
		pbuf_put(&b);
		return 2;
	}
	uint64_t x[8] = { QSEE_SMC_APP_START, QSEE_ARGS_APP_START, mdt_len, total, b.pa };
	qsmc(x);
	say("load %s: APP_START raw x0 %#llx x1 %#llx x2 %#llx x3 %#llx", name, (unsigned long long)x[0],
	    (unsigned long long)x[1], (unsigned long long)x[2], (unsigned long long)x[3]);
	abort_listeners(x, 0, "load");
	pbuf_put(&b);	/* Linux frees the image right after APP_START */
	int rc;
	uint32_t result = (uint32_t)x[1];
	if ((int64_t)x[0]) {
		say("load %s: smc failed (x0 %lld)", name, (long long)x[0]);
		rc = 2;
	} else if (result == QSEOS_RESULT_SUCCESS && !n_seen) {
		say("load %s: SUCCESS, app id %u (resp_type %#llx)", name, (unsigned)x[3], (unsigned long long)x[2]);
		rc = 0;
	} else {
		say("load %s: result %#x resp_type %#llx data %#llx%s", name, result, (unsigned long long)x[2],
		    (unsigned long long)x[3], result == QSEOS_RESULT_FAILURE ? " (QSEOS_RESULT_FAILURE)" : "");
		rc = n_seen ? 4 : 2;
	}
	print_seen();
	if (n_seen && !dryrun && !lookup_name(name, &id)) {
		/* Linux unloads after a failed incomplete-cmd round; so do we */
		say("load %s: loaded as %u after a listener abort: shutting it down", name, id);
		uint64_t y[8] = { QSEE_SMC_APP_SHUTDOWN, QSEE_ARGS_APP_SHUTDOWN, id };
		qsmc(y);
		abort_listeners(y, id, "unload");
	}
	free(img);
	free(mdt);
	return rc;
}

static uint32_t rd_u32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void put_u32(uint8_t *p, uint32_t v) { p[0] = v, p[1] = v >> 8, p[2] = v >> 16, p[3] = v >> 24; }
static void put_u64(uint8_t *p, uint64_t v) { put_u32(p, (uint32_t)v), put_u32(p + 4, (uint32_t)(v >> 32)); }

/*
 * FPC g12/1 get_build_info, laid out as the HAL (hal.dis fpc_tee_get_build_info @0xd59c, fpc_tac_transfer_platform
 * @0xfd1c): a 0x418-byte bulk buffer {u32 group 12, u32 id 1, u32 ta_status (zeroed), u32 result, u32 0,
 * u32 0x400 (size), char build[0x400]}; the 128-byte sb holds {u32 0x418, u32 pa(bulk)} (the pa patched at
 * cmd_buf_offset 4, 32-bit: QSEECom_send_modified_cmd), req = sb[0,0x40), resp = sb[0x40,0x80).
 */
static int cmd_send_build_info(uint32_t appid)
{
	if (check_appid(appid))
		return 2;
	int wl = 1;
	const char *e = getenv("QSEE_SEND");
	if (e && !strcmp(e, "legacy"))
		wl = 0;
	else if (!(e && !strcmp(e, "wl")) && !dryrun) {
		uint64_t x[8] = { QSEE_SMC_FEAT_VERSION, QSEE_ARGS_FEAT_VERSION, QSEE_FEATURE_WHITELIST };
		wl = !qsmc(x) && (uint32_t)x[1] >= QSEE_WHITELIST_V1;
		say("send: whitelist feature version %#x -> %s", (unsigned)x[1], wl ? "SEND_DATA_WITH_WHITELIST" : "SEND_DATA");
	}
	struct pbuf sb, bulk, arg;
	if (pbuf_get(&sb, 4096, 4096, 0, 0xa0000000ull))
		return 2;
	if (pbuf_get(&bulk, 4096, 4096, 1, 0xa0001000ull)) {
		pbuf_put(&sb);
		return 2;
	}
	if (pbuf_get(&arg, 4096, 4096, 0, 0xa0002000ull)) {
		pbuf_put(&bulk);
		pbuf_put(&sb);
		return 2;
	}
	if (bulk.pa >> 32) {
		say("send: bulk pa %#llx above 4 GB (the modfd patch is 32-bit)", (unsigned long long)bulk.pa);
		pbuf_put(&arg), pbuf_put(&bulk), pbuf_put(&sb);
		return 2;
	}
	uint8_t bb[0x418] = { 0 }, s[0x80] = { 0 }, a[0x100] = { 0 };
	put_u32(bb + 0, 12), put_u32(bb + 4, 1), put_u32(bb + 0x14, 0x400);
	put_u32(s + 0, sizeof(bb)), put_u32(s + 4, (uint32_t)bulk.pa);
	uint64_t req = sb.pa, rsp = sb.pa + 0x40, sgl = arg.pa + 0x80;
	put_u64(a + 0, rsp), put_u64(a + 8, 0x40);	/* args[3], args[4] */
	if (wl) {
		put_u64(a + 16, sgl), put_u64(a + 24, 8 * MAX_ION_FD);	/* args[5], args[6] */
		put_u32(a + 0x80, SGLIST_SET(1, 0, 4)), put_u32(a + 0x84, (uint32_t)bulk.size);	/* one contiguous buf */
	}
	if (wr_chunks(bulk.pa, bb, sizeof(bb)) || wr_chunks(sb.pa, s, sizeof(s)) || wr_chunks(arg.pa, a, sizeof(a))) {
		pbuf_put(&arg), pbuf_put(&bulk), pbuf_put(&sb);
		return 2;
	}
	say("send: app %u g12/1 get_build_info: sb %#llx (req 0x40, rsp +0x40 len 0x40), bulk %#llx len 0x418, "
	    "extra args %#llx%s", appid, (unsigned long long)sb.pa, (unsigned long long)bulk.pa, (unsigned long long)arg.pa,
	    wl ? ", sglistinfo at +0x80 {0x80000004, 0x1000}" : "");
	uint64_t x[8] = { wl ? QSEE_SMC_SEND_DATA_WL : QSEE_SMC_SEND_DATA, wl ? QSEE_ARGS_SEND_DATA_WL : QSEE_ARGS_SEND_DATA,
			  appid, req, 0x40, arg.pa };
	qsmc(x);
	say("send: raw x0 %#llx x1 %#llx x2 %#llx x3 %#llx", (unsigned long long)x[0], (unsigned long long)x[1],
	    (unsigned long long)x[2], (unsigned long long)x[3]);
	abort_listeners(x, appid, "send");
	int rc = 2;
	uint32_t result = (uint32_t)x[1];
	if ((int64_t)x[0]) {
		say("send: smc failed (x0 %lld)", (long long)x[0]);
	} else if (result != QSEOS_RESULT_SUCCESS) {
		say("send: result %#x resp_type %#llx data %#llx", result, (unsigned long long)x[2], (unsigned long long)x[3]);
		rc = n_seen ? 4 : 2;
	} else if (!dryrun && !pm_rd(sb.pa, s, sizeof(s)) && !pm_rd(bulk.pa, bb, sizeof(bb))) {
		char build[0x401];
		memcpy(build, bb + 0x18, 0x400);
		build[0x400] = 0;
		for (char *c = build; *c; c++)
			if ((unsigned char)*c < 0x20 && *c != '\n')
				*c = '.';
		say("send: SUCCESS. sb resp[0] (QSEECom-level status) %#x; bulk: group %u id %u ta_status %#x result %#x "
		    "size %#x", rd_u32(s + 0x40), rd_u32(bb), rd_u32(bb + 4), rd_u32(bb + 8), rd_u32(bb + 0xc),
		    rd_u32(bb + 0x14));
		say("send: build info: \"%.400s\"", build);
		rc = rd_u32(s + 0x40) || rd_u32(bb + 8) || rd_u32(bb + 0xc) ? 5 : 0;
	} else if (dryrun) {
		rc = 0;
	}
	print_seen();
	pbuf_put(&arg), pbuf_put(&bulk), pbuf_put(&sb);
	return rc;
}

static int cmd_unload(uint32_t appid)
{
	if (check_appid(appid))
		return 2;
	uint64_t x[8] = { QSEE_SMC_APP_SHUTDOWN, QSEE_ARGS_APP_SHUTDOWN, appid };
	qsmc(x);
	say("unload: raw x0 %#llx x1 %#llx x2 %#llx x3 %#llx", (unsigned long long)x[0], (unsigned long long)x[1],
	    (unsigned long long)x[2], (unsigned long long)x[3]);
	abort_listeners(x, appid, "unload");
	print_seen();
	if ((int64_t)x[0] || (uint32_t)x[1] != QSEOS_RESULT_SUCCESS) {
		say("unload %u: FAILED (x0 %lld result %#x)", appid, (long long)x[0], (unsigned)x[1]);
		return n_seen ? 4 : 2;
	}
	say("unload %u: SUCCESS", appid);
	return 0;
}
