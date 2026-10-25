/*
 * redfin: the kernel ends of HackiOS's RedwoodAudio.driver (phone/audio, after
 * github.com/oosexgang/hackios-xnu-userland userland/src/audio_plugin.c, MIT), the CoreAudio HAL
 * plug-in that gives iOS the iPhone X's built-in audio devices:
 *
 *   /dev/redwood-audio   the "Speaker" output: whole frames of 48 kHz 16-bit stereo, written by the
 *                        audio server's real-time thread, never waited on. 0954 (WS-A): handed to the
 *                        sink a driver sets with redwood_audio_set_sink() (sound=4: bsd/dev/sweet_sound.c,
 *                        the speakers through the ADSP); without one, counted and dropped as before.
 *                        ioctl _IOW('r', 1, {below, muted}) is the volume iOS sets, read by the sink with
 *                        redwood_audio_volume().
 *   /dev/redwood-haptic  the "Actuator" output: 48 kHz 16-bit mono, the waveforms iOS plays on an
 *                        iPhone X's Taptic Engine (UIKit feedback, CoreHaptics, system vibrations).
 *                        Each write's peak becomes a drive level for the DRV2624 (sweet_haptic_drive):
 *                        on while the waveform is above HAP_FLOOR, at a level that follows its peak
 *                        (full from a third of full scale, never under HAP_LEVEL_MIN).
 *
 * iOS plays haptics on the actuator only when the device tree's /product/haptics says closed-loop
 * (tools/mkadt.py copies it from the D22 tree). Boot-arg sweetaudio=0 makes neither node.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/ioccom.h>
#include <sys/proc.h>
#include <sys/uio.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <kern/locks.h>
#include <sys/redwood_audio.h>

void sweet_audio_init(void);
extern int sweet_haptic_drive(unsigned ms, unsigned strength);

struct sweet_audio_volume {
	uint32_t below;                 /* eighths of a dB under the loudest */
	uint32_t muted;
};
#define SWEET_AUDIO_SET_VOLUME  _IOW('r', 1, struct sweet_audio_volume)
#define AUDIO_RATE              48000u
#define MINOR_AUDIO             0
#define MINOR_HAPTIC            1
#define HAP_FLOOR               1000    /* peak under this: silence (3 % of full scale) */
#define HAP_FULL                10923   /* a third of full scale: full level */
#define HAP_LEVEL_MIN           48
#define HAP_MIN_MS              14      /* the shortest drive: one tap */

static struct {
	uint64_t audio_frames, haptic_frames, sunk_frames;
	uint32_t audio_writes, haptic_writes, haptic_drives;
	struct sweet_audio_volume volume;
	uint32_t volume_set;
} sa;

/* 0954: the sink (a driver that plays the frames) and the lock that guards its replacement */
static LCK_GRP_DECLARE(sa_lock_group, "sweet-audio");
static LCK_MTX_DECLARE(sa_sink_lock, &sa_lock_group);
static redwood_audio_sink_t sa_sink;
static const char *sa_sink_name = "nothing";

void
redwood_audio_set_sink(const char *name, redwood_audio_sink_t sink)
{
	lck_mtx_lock(&sa_sink_lock);
	sa_sink = sink;
	sa_sink_name = sink != NULL ? name : "nothing";
	lck_mtx_unlock(&sa_sink_lock);
	printf("SWEET AUDIO: /dev/redwood-audio now goes to %s\n", sink != NULL ? name : "nothing (dropped)");
}

bool
redwood_audio_volume(uint32_t *below, bool *muted, uint32_t *generation)
{
	if (sa.volume_set == 0) {
		return false;
	}
	*below = sa.volume.below;
	*muted = sa.volume.muted != 0;
	*generation = sa.volume_set;
	return true;
}

static int
sa_open(__unused dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	return 0;
}

static int
sa_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	return 0;
}

static int
sa_read(__unused dev_t dev, __unused struct uio *uio, __unused int ioflag)
{
	return ENODEV;
}

static void
sa_haptic(const int16_t *s, unsigned n)
{
	int peak = 0;
	for (unsigned i = 0; i < n; i++) {
		int v = s[i] < 0 ? -(int)s[i] : s[i];
		peak = v > peak ? v : peak;
	}
	sa.haptic_frames += n;
	if (peak < HAP_FLOOR) {
		return;
	}
	unsigned level = (unsigned)(127u * (unsigned)peak / HAP_FULL);
	level = level < HAP_LEVEL_MIN ? HAP_LEVEL_MIN : level > 127 ? 127 : level;
	unsigned ms = n * 1000u / AUDIO_RATE + 4;
	ms = ms < HAP_MIN_MS ? HAP_MIN_MS : ms;
	if (sa.haptic_drives++ < 16) {
		printf("SWEET AUDIO: actuator %u frames, peak %d -> level %u for %u ms\n", n, peak, level, ms);
	}
	(void)sweet_haptic_drive(ms, level);
}

static int
sa_write(dev_t dev, struct uio *uio, __unused int ioflag)
{
	int16_t buf[1024];
	bool haptic = minor(dev) == MINOR_HAPTIC;
	while (uio_resid(uio) > 0) {
		int n = (int)MIN(uio_resid(uio), (user_ssize_t)sizeof(buf));
		int e = uiomove((caddr_t)buf, n, uio);
		if (e) {
			return e;
		}
		if (haptic) {
			sa_haptic(buf, (unsigned)n / 2);
		} else {
			sa.audio_frames += (unsigned)n / 4;
			lck_mtx_lock(&sa_sink_lock);
			if (sa_sink != NULL) {
				sa.sunk_frames += sa_sink(buf, (uint32_t)n / 4);
			}
			lck_mtx_unlock(&sa_sink_lock);
		}
	}
	uint32_t w = haptic ? ++sa.haptic_writes : ++sa.audio_writes;
	if (w == 1 || (w & 0xfff) == 0) {
		printf("SWEET AUDIO: %s: %u writes, %llu frames (%llu s)%s%s%s\n", haptic ? "actuator" : "speaker", w,
		    haptic ? sa.haptic_frames : sa.audio_frames, (haptic ? sa.haptic_frames : sa.audio_frames) / AUDIO_RATE,
		    haptic ? "" : ", to ", haptic ? "" : sa_sink_name, haptic || sa_sink != NULL ? "" : " (dropped)");
	}
	return 0;
}

static int
sa_ioctl(dev_t dev, u_long cmd, caddr_t data, __unused int fflag, __unused proc_t p)
{
	if (minor(dev) != MINOR_AUDIO || cmd != SWEET_AUDIO_SET_VOLUME) {
		return ENOTTY;
	}
	sa.volume = *(struct sweet_audio_volume *)data;
	__asm__ volatile ("dmb ish" ::: "memory");      /* (the generation last: redwood_audio_volume() reads it) */
	if (sa.volume_set++ < 8) {
		printf("SWEET AUDIO: speaker volume %u.%u dB under the loudest%s\n", sa.volume.below / 8,
		    sa.volume.below % 8 * 125 / 100, sa.volume.muted ? ", muted" : "");
	}
	return 0;
}

static const struct cdevsw sa_cdevsw = {
	.d_open       = sa_open,
	.d_close      = sa_close,
	.d_read       = sa_read,
	.d_write      = sa_write,
	.d_ioctl      = sa_ioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = eno_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = 0,
};

void
sweet_audio_init(void)
{
	uint32_t on = 1;
	PE_parse_boot_argn("sweetaudio", &on, sizeof(on));
	if (!on) {
		return;
	}
	int major = cdevsw_add(-1, &sa_cdevsw);
	if (major < 0) {
		printf("SWEET AUDIO: cdevsw_add failed (%d)\n", major);
		return;
	}
	void *a = devfs_make_node(makedev(major, MINOR_AUDIO), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "redwood-audio");
	void *h = devfs_make_node(makedev(major, MINOR_HAPTIC), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "redwood-haptic");
	printf("SWEET AUDIO: /dev/redwood-audio %s, /dev/redwood-haptic %s (major %d)\n", a ? "ready" : "failed",
	    h ? "ready" : "failed", major);
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
