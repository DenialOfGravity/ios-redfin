/*
 * qsee_lib (ws-fpcd-a): the QSEE / FPC TA code of qseeprobe (ws-qsee-a/b) as a small library, shared by qseeprobe and
 * sweetfpcd (phone/fpc). Moved, not rewritten: qsee_lib.c holds what qseeprobe.c, qsee_app.inc and qsee_fpc.inc had
 * (say/qsmc, lookup, load, listener abort, pmem buffers, FPC g10 sensor and g11 bio commands, the gpio112 wait).
 * Every SMC still goes through qsmc() and the hard whitelist (qsee_wl.c).
 *
 * Not thread-safe: one SMC at a time. A multi-threaded user (sweetfpcd) serialises every call with its own mutex.
 * Process exclusion: qsee_lib_lock() takes an flock on QSEE_LOCK_PATH (env QSEE_LOCK overrides the path, QSEE_NOLOCK=1
 * skips it), so only one QSEE/SMC user (sweetfpcd or one qseeprobe) runs at a time.
 */
#ifndef QSEE_LIB_H
#define QSEE_LIB_H
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* options (env, set by the caller): QSEE_DRYRUN, QSEE_SHMBRIDGE; say_quiet drops say() lines; con = console fd mirror */
extern int dryrun, shmbridge, con, say_quiet;
extern const char *say_tag;		/* console prefix, default "qseeprobe: " */
extern uint64_t buf_pa;			/* lookup_one()'s name page */
extern uint32_t lookup_app_id;		/* set by lookup_one() on LOADED */
extern uint64_t bridge_handle;
extern int bridged;
extern uint32_t seen_lstnr[8];		/* listener ids TZ asked for (all refused) */
extern unsigned n_seen;

void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int64_t qsmc(uint64_t x[8]);		/* the only path to PMEM_SMC; exits(3) on a call outside the whitelist */

/* app manager */
int bridge_create(void);
int bridge_delete(void);
int lookup_one(const char *name);	/* in the page at buf_pa: 0 loaded, 1 not, 2 error, 3 stop */
int lookup_name(const char *name, uint32_t *id);	/* the same in its own page */
int check_appid(uint32_t appid);	/* 0 iff appid is QSEE_APP's (default fpctzappfingerprint) and not keymaster64 */
int cmd_load(const char *name, const char *dir);	/* DIR/NAME.mdt + .bNN; 0 ok/already loaded, 2 error, 4 listener */
int qsee_unload(uint32_t appid);	/* TZ_OS_APP_SHUTDOWN, no guard (callers check_appid() first); 0 ok */
void abort_listeners(uint64_t x[8], uint32_t app_id, const char *what);
void note_lstnr(uint32_t id);
void print_seen(void);

/* pmem buffers handed to TZ */
struct pbuf {
	uint64_t pa, size, handle;
	int bridged;
};
int bridge_reg(uint64_t pa, uint64_t size, uint64_t *handle);
int bridge_dereg(uint64_t handle);
int pbuf_get(struct pbuf *b, uint64_t size, uint64_t align, int below4g, uint64_t fake);
void pbuf_put(struct pbuf *b);
int wr_chunks(uint64_t pa, const uint8_t *p, size_t len);
uint8_t *slurp(const char *path, size_t *len);
uint32_t rd_u32(const uint8_t *p);
void put_u32(uint8_t *p, uint32_t v);
void put_u64(uint8_t *p, uint64_t v);

/* FPC TA: group 10 sensor commands, group 11 bio commands, one 0x58-byte buffer, 64-bit send layout */
#define MAX_ABORTS 6
#define FPC_SENSOR_LEN 0x58
#define FPC_TA_BUSY (-12)
#define FPC_MAX_FINGERS 5
int fpc_send64(uint32_t appid, uint8_t *data, size_t len, const char *what);
void fpc_dump(const char *what, const uint8_t *d, size_t len);
int fpc_sensor_cmd(uint32_t appid, uint8_t *d, uint32_t cmd, int32_t *status);	/* 0 ok, 3 TZ, 4 listener, 5 TA status */
int fd_cmd(uint32_t appid, uint8_t *d, uint32_t cmd, int32_t *status);	/* fpc_sensor_cmd, quiet */
uint32_t fpc_sysctl_u32(const char *name);
void fpc_gpio_state(const char *what);
long fpc_ms_since(const struct timespec *t0);
int fpc_wait_high(int ms);		/* kern.sweet_fpc_wait: 1 gpio112 high, 0 timeout, -1 error */
int fpc_bio(uint32_t appid, uint8_t *d, uint32_t cmd, int32_t *status);	/* g11/cmd; transport rc, *status = TA */
int fpc_get_ids(uint32_t appid, uint32_t out[5], uint32_t *count, int32_t *status);
int fpc_print_ids(uint32_t appid, const char *what, uint32_t *count);
int fpc_delete(uint32_t appid, uint32_t fid);
int fpc_cleanup_all(uint32_t appid, const char *what);	/* delete every listed id; 0 iff the list ends empty */
int fpc_capture(uint32_t appid, const struct timespec *t0, int ms, int32_t *st4, long *wake_ms);
const char *enrol_code(int32_t s);

/* HAL capture loop, cancellable (qsee_lib.c fpc_capture_hal) */
#define FPC_EARLY_WAKE_MS	60	/* a wake within this many ms of arming is the normal early wake, not a finger */
#define FPC_STORM_IRQS		200	/* kern.sweet_fpc_irq rise within 1 s that counts as an IRQ storm */
enum { FPC_HAL_GOOD, FPC_HAL_CANCEL, FPC_HAL_TIMEOUT, FPC_HAL_ERR, FPC_HAL_FPCERR, FPC_HAL_STORM };
struct fpc_hal_cb {
	void *ctx;
	int slice_ms;					/* wait slice (default 250) */
	long cap3_ms;					/* > 0: max wait after cmd 3 (default: until the deadline) */
	int (*stop)(void *ctx);				/* non-zero: cancel */
	void (*lock)(void *ctx), (*unlock)(void *ctx);	/* around each SMC */
	void (*wake)(void *ctx, long ms_after_arm);
	void (*idle)(void *ctx, long ms_since_arm);
	void (*retry)(void *ctx, int32_t st4);		/* a round ended without a good image (then re-arm) */
};
int fpc_capture_hal(uint32_t appid, long deadline_ms, const struct fpc_hal_cb *cb, int32_t *st4, int *rounds);

/* one QSEE/SMC user at a time */
#define QSEE_LOCK_PATH "/var/local/tz/qsee.lock"
int qsee_lib_lock(const char *who);		/* 0 held (or skipped), -1 another process holds it */

#endif
