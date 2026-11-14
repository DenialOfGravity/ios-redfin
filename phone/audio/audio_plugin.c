/*
 * RedwoodAudio.driver: the output device of iOS on this machine.
 *
 * Audio devices reach the audio server's hardware layer (the "HAL" of CoreAudio, inside
 * mediaserverd) in two ways: kernel drivers through IOAudio2 -- that is how an iPhone's own
 * speaker, codec and microphones arrive, and none of it exists here -- and plug-ins in
 * /System/Library/Audio/Plug-Ins/HAL, which is how Bluetooth, AirPlay, CarPlay and USB audio
 * arrive. This is such a plug-in. Its interface is the one macOS documents for user-space
 * audio drivers (AudioServerPlugIn: a CFPlugIn with one COM-style table of functions; the HAL
 * asks for objects and their properties, then runs the I/O); the iOS SDK has no header for it,
 * so what is needed of it is declared below.
 *
 * Above the HAL sits the virtual audio device (/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin),
 * through which everything in iOS is routed. It knows an iPhone's own devices by their names
 * (their UIDs) and makes a port of each: "Speaker" becomes the speaker port, the route an iPhone
 * with nothing plugged in plays through, with a volume, the volume display and the place in
 * Control Center that go with it. So this presents four devices under those names:
 *
 *   "Speaker"   a stereo output whose samples are written to /dev/redwood-audio, where the
 *               kernel takes them to whatever can play them (see sys/redwood_audio.h), and a
 *               stereo input that is always silent (on an iPhone: what the amplifiers measure)
 *   "Hawking"   one silent input and nothing else. The virtual audio device will not build the
 *               speaker route without a second virtual device of its own (its type 'vhaw', a
 *               mandatory companion of the default one in the iPhone X's routing tables) whose
 *               only port is this input; without it everything stays on the null port.
 *   "Actuator"  one output of one channel: the iPhone's Taptic Engine, on which iOS plays its
 *               haptics as waveforms (through the chain 'd22_actuator_general'); its samples go to
 *               the kernel's motor driver, /dev/redwood-haptic (see actuator_write())
 *   "Codec"     one input of one channel: on an iPhone, the audio codec that the built-in
 *               microphones are on, of which the virtual audio device makes the built-in microphone
 *               port; its samples come from the kernel, /dev/redwood-mic (the phone's main
 *               microphone, boot-arg mic=4), which listens only while this input runs (see codec_read())
 *
 * CoreAudio's own null device ("Null_Device", described in the framework's Info.plist, playing
 * into nowhere) stays as it is: it is the only device the virtual audio device takes while it
 * starts, and ours are taken a moment later, when it says it allows all devices.
 *
 * What is played through the speaker port passes the iPhone X's chain of effects for its own
 * speakers first ('d22_speaker_general', from /Library/Audio/Tunings/D22: equalisers,
 * compressors, speaker protection), at the volume iOS has set.
 *
 * The device has no clock but the system's: it claims to play exactly REDWOOD_AUDIO_RATE frames
 * a second of mach_absolute_time(), and the HAL's I/O thread paces itself by that.
 *
 * The speaker's output has a volume control, as the iPhone's own speaker has: what is set on it
 * goes to the kernel (ioctl REDWOOD_AUDIO_SET_VOLUME), which sets the amplifiers' level by it -- on
 * the phone, so far under the loudest it allows. Its scale is 48 dB in a straight line of
 * decibels, 3 dB for each of iOS's sixteen steps; 0 is silence. What iOS does with it (the phone,
 * 15 October): the virtual audio device sets it once, to its top (0 dB), when it takes the
 * device, and leaves it there -- its routing tables keep the iPhone X's speaker in software volume
 * mode, so the volume buttons and Control Center scale the samples themselves, and at 0 what is
 * written is silence (on which the kernel keeps the amplifiers off).
 *
 * The receiver (ws-n-call-route; only with the flag file /var/local/redwood-call-route, read when the plug-in loads:
 * absent or 0 = all of the above and nothing more). VirtualAudio's codec device (Device_Codec_Aspen, the one made for
 * the UID "Codec") makes its output ports from the codec's output mute controls: "Built-In Receiver" when the device
 * has kAudioDevicePropertyMute on output element 3, "Built-In Speaker" on element 4 (AudioObjectHasProperty({'mute',
 * 'outp', 3|4}), VirtualAudio 0x24006c/0x240358). Without a receiver port a call has one output only and the in-call
 * screen shows no route (Speaker) button. So with the flag:
 *   1  "Codec" also gets a stereo output (what iOS plays to the receiver; thrown away: call audio runs in the DSP) and a
 *      mute control on output element 3 = the receiver port
 *   2  as 1, and a mute control on element 4 too (the speaker port then comes from the codec as on an iPhone; an
 *      experiment: today's speaker port comes from "Speaker", and two of them may upset VirtualAudio)
 * What VirtualAudio does with the mutes is how it picks the port: the receiver unmuted = Receiver, the receiver muted
 * (or element 4 unmuted) = Speaker. Each change goes to the kernel (ioctl REDWOOD_AUDIO_SET_CALL_ROUTE, xnu 1086:
 * kern.sweet_voice_route), which moves a running call between the earpiece and speaker amplifiers. Logged by the
 * kernel as "SWEET VOICE: iOS route: Receiver|Speaker".
 *   3  as 1 (no element 4), and a fifth device, "Baseband Voice" (experiment S1a, research/cc/BBA-design-study.md):
 *      VirtualAudio's call routing (RoutingHandler_Call_GenericConfig2) looks up the device with that UID first and gives
 *      up ("could not get baseband device") without it, so no phone-call route is built. This one is a silent stub in the
 *      shape of the iPhone X's I2S baseband device (mca3/mca3a/audio-baseband-voice): one input stream (the downlink, what
 *      the network sends: silence here) and one output stream (the uplink, what iOS sends: thrown away), mono 16-bit,
 *      8000 or 16000 Hz, the system clock (the same as the other devices' clocks), and a clock source control with the one
 *      item 'bb2w' ("Baseband"), which VirtualAudio selects when it is there. Call audio stays in the DSP (xnu 1065/1086);
 *      the stub only lets iOS build the call route. Its I/O, its rate/format/clock-source changes and the mutes are logged.
 *   4  as 3, but "Baseband Voice" carries the call (BBA stage 4; kernel: kern.sweet_voice_proxy 2, xnu 1091 + 1095, and the
 *      IOService "Baseband Voice" of 1092 so that BasebandVoice.driver stands down). While its I/O runs it holds
 *      /dev/redwood-voice: the downlink (10 ms frames of S16 mono that the kernel takes from AFE RT proxy port 0x2000 once
 *      per 10 ms of the AP clock) goes through a small FIFO into the input stream; what iOS writes on the output stream
 *      (its voice-processed microphone) is written to the device, which sends it to port 0x2001. The device's rate
 *      (VirtualAudio sets it to the telephony rate) is given to the kernel for the next call (REDWOOD_VOICE_SET_RATE); a
 *      call that runs at the other rate is converted 2:1 / 1:2 here. And because iOS itself now plays the call: what it
 *      plays to the receiver port ("Codec" output) goes to /dev/redwood-audio while the receiver is unmuted (the kernel's
 *      call amplifier is the earpiece's then: 1086), and the speaker device's writes give way while the receiver plays.
 *      Zero timestamps: the system clock at the nominal rate, as the other devices (the kernel's proxy reads are paced on
 *      that clock, 1095); GET_TIME is only measured (drift in ppm, logged).
 *   5  as 4, with the zero timestamps following GET_TIME: the device's period follows the rate the kernel reports
 *      (frames given vs mach time over the call, at most 1000 ppm from nominal) - the variant if 4 drifts.
 * Without the flag (or 0..3) nothing of 4/5 runs: the device stays closed, the receiver is thrown away as before.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFPlugInCOM.h>
#include <CoreAudioTypes/CoreAudioTypes.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <sys/ioctl.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "redwood_audio.h"

/* ---- the AudioServerPlugIn interface (macOS: CoreAudio/AudioServerPlugIn.h, AudioHardwareBase.h) ---- */

typedef UInt32 AudioObjectID;
typedef struct {
	UInt32 mSelector, mScope, mElement;
} AudioObjectPropertyAddress;
typedef struct {
	AudioStreamBasicDescription mFormat;
	AudioValueRange mSampleRateRange;
} AudioStreamRangedDescription;
typedef struct {
	UInt32 mClientID;
	pid_t mProcessID;
	Boolean mIsNativeEndian;
	CFStringRef mBundleID;
} AudioServerPlugInClientInfo;
typedef struct {
	UInt64 mIOCycleCounter;
	UInt32 mNominalIOBufferFrameSize;
	AudioTimeStamp mCurrentTime, mInputTime, mOutputTime;
	Float64 mMasterHostTicksPerFrame, mDeviceHostTicksPerFrame;
} AudioServerPlugInIOCycleInfo;

typedef struct AudioServerPlugInHostInterface AudioServerPlugInHostInterface;
typedef const AudioServerPlugInHostInterface *AudioServerPlugInHostRef;
struct AudioServerPlugInHostInterface {
	OSStatus (*PropertiesChanged)(AudioServerPlugInHostRef, AudioObjectID, UInt32, const AudioObjectPropertyAddress *);
	OSStatus (*CopyFromStorage)(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef *);
	OSStatus (*WriteToStorage)(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef);
	OSStatus (*DeleteFromStorage)(AudioServerPlugInHostRef, CFStringRef);
	OSStatus (*RequestDeviceConfigurationChange)(AudioServerPlugInHostRef, AudioObjectID, UInt64, void *);
};

typedef struct AudioServerPlugInDriverInterface AudioServerPlugInDriverInterface;
typedef AudioServerPlugInDriverInterface **AudioServerPlugInDriverRef;
struct AudioServerPlugInDriverInterface {
	void *_reserved;
	HRESULT (*QueryInterface)(void *, REFIID, LPVOID *);
	ULONG (*AddRef)(void *);
	ULONG (*Release)(void *);
	OSStatus (*Initialize)(AudioServerPlugInDriverRef, AudioServerPlugInHostRef);
	OSStatus (*CreateDevice)(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo *, AudioObjectID *);
	OSStatus (*DestroyDevice)(AudioServerPlugInDriverRef, AudioObjectID);
	OSStatus (*AddDeviceClient)(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
	OSStatus (*RemoveDeviceClient)(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
	OSStatus (*PerformDeviceConfigurationChange)(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
	OSStatus (*AbortDeviceConfigurationChange)(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
	Boolean (*HasProperty)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *);
	OSStatus (*IsPropertySettable)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, Boolean *);
	OSStatus (*GetPropertyDataSize)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32,
	    const void *, UInt32 *);
	OSStatus (*GetPropertyData)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32,
	    const void *, UInt32, UInt32 *, void *);
	OSStatus (*SetPropertyData)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32,
	    const void *, UInt32, const void *);
	OSStatus (*StartIO)(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
	OSStatus (*StopIO)(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
	OSStatus (*GetZeroTimeStamp)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, Float64 *, UInt64 *, UInt64 *);
	OSStatus (*WillDoIOOperation)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, Boolean *, Boolean *);
	OSStatus (*BeginIOOperation)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32,
	    const AudioServerPlugInIOCycleInfo *);
	OSStatus (*DoIOOperation)(AudioServerPlugInDriverRef, AudioObjectID, AudioObjectID, UInt32, UInt32, UInt32,
	    const AudioServerPlugInIOCycleInfo *, void *, void *);
	OSStatus (*EndIOOperation)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32,
	    const AudioServerPlugInIOCycleInfo *);
};

#define kAudioServerPlugInTypeUUID CFUUIDGetConstantUUIDWithBytes(NULL, 0x44, 0x3A, 0xBA, 0xB8, 0xE7, 0xB3, 0x49, 0x1A, \
	    0xB9, 0x85, 0xBE, 0xB9, 0x18, 0x70, 0x30, 0xDB)
#define kAudioServerPlugInDriverInterfaceUUID CFUUIDGetConstantUUIDWithBytes(NULL, 0xEE, 0xA5, 0x77, 0x3D, 0xCC, 0x43, \
	    0x49, 0xF1, 0x8E, 0x00, 0x8F, 0x96, 0xE7, 0xD2, 0x3B, 0x17)

enum {
	kAudioHardwareUnknownPropertyError = 'who?', kAudioHardwareBadPropertySizeError = '!siz',
	kAudioHardwareIllegalOperationError = 'nope', kAudioHardwareBadObjectError = '!obj',
	kAudioHardwareUnsupportedOperationError = 'unop', kAudioDeviceUnsupportedFormatError = '!dat',

	kAudioObjectPropertyScopeGlobal = 'glob', kAudioObjectPropertyScopeInput = 'inpt', kAudioObjectPropertyScopeOutput = 'outp',

	kAudioObjectClassID = 'aobj', kAudioPlugInClassID = 'aplg', kAudioDeviceClassID = 'adev', kAudioStreamClassID = 'astr',

	kAudioObjectPropertyBaseClass = 'bcls', kAudioObjectPropertyClass = 'clas', kAudioObjectPropertyOwner = 'stdv',
	kAudioObjectPropertyName = 'lnam', kAudioObjectPropertyManufacturer = 'lmak', kAudioObjectPropertyOwnedObjects = 'ownd',

	kAudioPlugInPropertyBoxList = 'box#', kAudioPlugInPropertyTranslateUIDToBox = 'uidb',
	kAudioPlugInPropertyDeviceList = 'dev#', kAudioPlugInPropertyTranslateUIDToDevice = 'uidd',
	kAudioPlugInPropertyResourceBundle = 'rsrc',

	kAudioDevicePropertyDeviceUID = 'uid ', kAudioDevicePropertyModelUID = 'muid', kAudioDevicePropertyTransportType = 'tran',
	kAudioDevicePropertyRelatedDevices = 'akin', kAudioDevicePropertyClockDomain = 'clkd',
	kAudioDevicePropertyDeviceIsAlive = 'livn', kAudioDevicePropertyDeviceIsRunning = 'goin',
	kAudioDevicePropertyDeviceCanBeDefaultDevice = 'dflt', kAudioDevicePropertyDeviceCanBeDefaultSystemDevice = 'sflt',
	kAudioDevicePropertyLatency = 'ltnc', kAudioDevicePropertyStreams = 'stm#', kAudioObjectPropertyControlList = 'ctrl',
	kAudioDevicePropertySafetyOffset = 'saft', kAudioDevicePropertyNominalSampleRate = 'nsrt',
	kAudioDevicePropertyAvailableNominalSampleRates = 'nsr#', kAudioDevicePropertyIsHidden = 'hidn',
	kAudioDevicePropertyPreferredChannelsForStereo = 'dch2', kAudioDevicePropertyZeroTimeStampPeriod = 'ring',
	kAudioSelectorControlClassID = 'slct', kAudioDataSourceControlClassID = 'dsrc', kAudioClockSourceControlClassID = 'clck',
	kAudioSelectorControlPropertyCurrentItem = 'scci', kAudioSelectorControlPropertyAvailableItems = 'scai',
	kAudioSelectorControlPropertyItemName = 'scin', kAudioSelectorControlPropertyItemKind = 'clkk',

	kAudioStreamPropertyIsActive = 'sact', kAudioStreamPropertyDirection = 'sdir', kAudioStreamPropertyTerminalType = 'term',
	kAudioStreamPropertyStartingChannel = 'schn', kAudioStreamPropertyLatency = 'ltnc',
	kAudioStreamPropertyVirtualFormat = 'sfmt', kAudioStreamPropertyAvailableVirtualFormats = 'sfma',
	kAudioStreamPropertyPhysicalFormat = 'pft ', kAudioStreamPropertyAvailablePhysicalFormats = 'pfta',

	kAudioStreamTerminalTypeSpeaker = 'spkr', kAudioStreamTerminalTypeMicrophone = 'micr',

	kAudioServerPlugInIOOperationReadInput = 'read', kAudioServerPlugInIOOperationWriteMix = 'rite',

	kAudioLevelControlClassID = 'levl', kAudioVolumeControlClassID = 'vlme',
	kAudioControlPropertyScope = 'cscp', kAudioControlPropertyElement = 'celm',
	kAudioLevelControlPropertyScalarValue = 'lcsv', kAudioLevelControlPropertyDecibelValue = 'lcdv',
	kAudioLevelControlPropertyDecibelRange = 'lcdr', kAudioLevelControlPropertyConvertScalarToDecibels = 'lcsd',
	kAudioLevelControlPropertyConvertDecibelsToScalar = 'lcds',
};

/* ---- the devices ---------------------------------------------------------------------------------- */

/*
 * Object numbers are ours to choose. A device's streams follow it: its output (if it has one),
 * then its input.
 */
enum {
	OBJECT_PLUGIN = 1,
	OBJECT_SPEAKER, OBJECT_SPEAKER_OUTPUT, OBJECT_SPEAKER_INPUT,
	OBJECT_HAWKING, OBJECT_HAWKING_INPUT,
	OBJECT_ACTUATOR, OBJECT_ACTUATOR_OUTPUT,
	OBJECT_CODEC, OBJECT_CODEC_INPUT,
	OBJECT_SPEAKER_VOLUME,          /* the speaker output's volume control */
	OBJECT_CODEC_SOURCE,            /* the codec input's data source control: its microphones */
	OBJECT_CODEC_OUTPUT,            /* call route: the codec's output (the receiver's), with the flag file only */
	OBJECT_RECEIVER_MUTE,           /* call route: the codec output's mute, element 3 (the receiver port) */
	OBJECT_SPEAKER_MUTE,            /* call route: the codec output's mute, element 4 (flag 2 only) */
	OBJECT_BB, OBJECT_BB_OUTPUT, OBJECT_BB_INPUT,   /* flags 3-5: "Baseband Voice", its uplink and downlink */
	OBJECT_BB_CLOCK,                /* flags 3-5: its clock source control ('bb2w') */
	OBJECT_LAST = OBJECT_BB_CLOCK
};

#define CALL_ROUTE_FILE         "/private/var/local/redwood-call-route"
#define RECEIVER_ELEMENT        3
#define SPEAKER_ELEMENT         4
#define kAudioBooleanControlClassID     'togl'
#define kAudioMuteControlClassID        'mute'
#define kAudioBooleanControlPropertyValue 'bcvl'
static int call_route;                  /* the flag: 0 none, 1 receiver, 2 receiver + speaker on the codec, 3 = 1 + baseband,
                                           4/5 = 3 with the call's PCM through /dev/redwood-voice (stage 4) */
#define BB_PRESENT              (call_route >= 3)
#define BB_LIVE                 (call_route >= 4)
static UInt32 mutes[2] = { 1, 1 };      /* the receiver's and the codec speaker's mute, as VirtualAudio set them */
static int route_told;                  /* what the kernel was told last: 0 nothing yet */
static UInt64 receiver_frames;

#define VOLUME_RANGE_DB         48.0    /* from the loudest the kernel allows down to the quietest step above 0 */

struct device {
	AudioObjectID object;
	CFStringRef uid;                /* the name the virtual audio device knows it by */
	AudioObjectID output, input;    /* its streams; 0 = none */
	UInt32 output_channels, input_channels;
	Float64 rate;                   /* its sample rate; 0 = REDWOOD_AUDIO_RATE */
	/* its clock, running while anybody does I/O */
	unsigned running;               /* how many clients have started I/O */
	UInt64 anchor;                  /* mach_absolute_time() when the first of them did */
	UInt64 periods;                 /* whole timestamp periods since */
};

static struct device devices[] = {
	{ .object = OBJECT_SPEAKER, .uid = CFSTR("Speaker"), .output = OBJECT_SPEAKER_OUTPUT, .input = OBJECT_SPEAKER_INPUT,
	  .output_channels = REDWOOD_AUDIO_CHANNELS, .input_channels = 2 },
	{ .object = OBJECT_HAWKING, .uid = CFSTR("Hawking"), .input = OBJECT_HAWKING_INPUT, .input_channels = 1 },
	{ .object = OBJECT_ACTUATOR, .uid = CFSTR("Actuator"), .output = OBJECT_ACTUATOR_OUTPUT, .output_channels = 1 },
	{ .object = OBJECT_CODEC, .uid = CFSTR("Codec"), .input = OBJECT_CODEC_INPUT, .input_channels = 1 },
	/* flags 3-5 (devices_present = 5): the baseband voice device (3: silent stub; 4, 5: the call through /dev/redwood-voice) */
	{ .object = OBJECT_BB, .uid = CFSTR("Baseband Voice"), .output = OBJECT_BB_OUTPUT, .input = OBJECT_BB_INPUT,
	  .output_channels = 1, .input_channels = 1, .rate = 16000 },
};
static unsigned devices_present = 4;    /* the first so many of devices[] exist: 5 with the flags 3-5 */
#define DEVICES                 devices_present
#define BB_CLOCK_SOURCE         'bb2w'
#define BB_MANUFACTURER         CFSTR("Apple Inc.")     /* as BasebandVoice.driver says; VirtualAudio does not look */
static const Float64 bb_rates[] = { 8000, 16000 };
#define BB_RATES                (sizeof(bb_rates) / sizeof(bb_rates[0]))
static UInt64 bb_read, bb_written;      /* frames since the stub's I/O started: downlink given (silence), uplink taken */
static int bb_peak;                     /* the uplink's peak (what iOS would send to the network) */
static Boolean bb_first_read, bb_first_write;
static UInt64 bb_starts;

/*
 * Stage 4 (flags 4, 5): /dev/redwood-voice (xnu 1091, bsd/dev/sweet_voicedev.c; a copy of its interface, which must
 * stay byte for byte the same). One opener at a time; node uid/gid 501 mode 0660 (mediaserverd runs as mobile; root
 * may open it too). read(): whole 10 ms frames only (a buffer under one frame: EINVAL), O_NONBLOCK: EAGAIN when none,
 * 0 without a call. write(): any even count, never blocks (over 200 ms queued: dropped by the kernel).
 */
#define REDWOOD_VOICE_DEVICE    "/dev/redwood-voice"
struct redwood_voice_state {            /* 32 bytes */
	uint32_t call_up, rate, frame_samples, mode, dl_queued, ul_queued, call_count, reserved;
};
struct redwood_voice_time {             /* 24 bytes */
	uint64_t sample, mach_time;
	uint32_t rate, call_count;
};
struct redwood_voice_counters {         /* 32 bytes, this call */
	uint32_t dl_frames, ul_frames, dl_dropped, ul_dropped, ul_underruns, dl_unread, slips, reserved;
};
#define REDWOOD_VOICE_GET_STATE         _IOR('v', 1, struct redwood_voice_state)
#define REDWOOD_VOICE_SET_RATE          _IOW('v', 2, uint32_t)
#define REDWOOD_VOICE_GET_TIME          _IOR('v', 3, struct redwood_voice_time)
#define REDWOOD_VOICE_GET_COUNTERS      _IOR('v', 4, struct redwood_voice_counters)
_Static_assert(REDWOOD_VOICE_GET_STATE == 0x40207601, "1091 ioctl number");
_Static_assert(REDWOOD_VOICE_SET_RATE == 0x80047602, "1091 ioctl number");
_Static_assert(REDWOOD_VOICE_GET_TIME == 0x40187603, "1091 ioctl number");
_Static_assert(REDWOOD_VOICE_GET_COUNTERS == 0x40207604, "1091 ioctl number");

#define BB_FIFO                 8192            /* downlink samples at the device's rate (512 ms at 16 kHz), power of 2 */
#define BB_KBUF                 1600            /* one read: up to 100 ms at 16 kHz (10 whole frames) */
static int voice = -1;                  /* /dev/redwood-voice while the baseband's I/O runs (flags 4, 5) */
static int voice_errno;                 /* why the last open failed (said once per I/O run) */
static UInt64 voice_retry;              /* mach time of the next open attempt */
static int16_t bb_fifo[BB_FIFO];        /* downlink at the device's rate; only the I/O thread touches it */
static UInt32 bb_fifo_head, bb_fifo_tail;
static Boolean bb_primed;               /* the FIFO filled to its start level since the last underrun */
static UInt32 bb_call_seen;             /* call_count of the call the FIFO belongs to */
static struct redwood_voice_state bb_state;     /* the kernel's, polled every 100 ms from the I/O thread */
static UInt64 bb_state_next, bb_stats_next;
static UInt32 bb_stats_n;
/* this I/O run (zeroed at StartIO) */
static struct {
	UInt64 dl_got, dl_under, dl_trim, dl_conv, ul_put, ul_trim, ul_err, rd_err, stats_dl_got, stats_ul_put;
	int dl_peak, ul_peak, s_dl_peak, s_ul_peak;
	UInt32 calls;
	Boolean said_mismatch, said_rd_err;
} bb;
/* GET_TIME: the call's first point and the rate measured since (ticks per frame); flag 5 uses it for the clock */
static UInt64 bb_t0_sample, bb_t0_mach;
static UInt32 bb_t0_call;
static Float64 bb_tpf;                  /* the baseband clock's mach ticks per frame (flag 5), 0 = nominal */
static UInt64 bb_period_host;           /* flag 5: host time of the device's current zero-timestamp period */
/* the receiver to /dev/redwood-audio (flags 4, 5) */
static volatile UInt64 rx_last;         /* mach time of the receiver's latest write that went to the sink */
static UInt64 rx_played, rx_dropped, spk_yielded;

#define TRANSPORT_BUILT_IN      'bltn'
/*
 * The codec's input has data sources, one for each microphone, and the virtual audio device will not
 * take a codec without them (VirtualAudio asks for kAudioDevicePropertyDataSources on its input, and
 * keeps those it knows: 'imic', 'smic', 'fmic', 'lmic', 'tmic', 'umic', 'imi1' to 'imi4'). A plug-in
 * has them as a data source control on the input, which the HAL answers those properties from. There
 * is one microphone here, at the bottom: 'imic', the iPhone's internal (bottom) one.
 */
#define MIC_SOURCE              'imic'
#define MANUFACTURER            CFSTR("redwood")
/* how often a device says where its clock is, in frames: also the largest I/O buffer the HAL will use */
#define TIMESTAMP_PERIOD        16384

static AudioServerPlugInHostRef host;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int sink = -1;                   /* /dev/redwood-audio */
static Float64 ticks_per_frame;
static Float64 ticks_per_second;
static UInt64 written, refused;         /* frames since the speaker's I/O started */
static Boolean inactive[OBJECT_LAST + 1];       /* streams the HAL has switched off */
static Float32 volume = 1.0f;           /* the speaker's volume control, 0 to 1 (iOS sets it once it routes there) */

static void
say(const char *format, ...)
{
	char line[256];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	write(2, line, (size_t)n);
}

/* The volume control's scale: a straight line of decibels over VOLUME_RANGE_DB. */
static Float32
scalar_to_db(Float32 scalar)
{
	scalar = scalar < 0 ? 0 : scalar > 1 ? 1 : scalar;
	return (Float32)(-VOLUME_RANGE_DB * (1.0 - scalar));
}

static Float32
db_to_scalar(Float32 db)
{
	Float32 scalar = (Float32)(1.0 + db / VOLUME_RANGE_DB);

	return scalar < 0 ? 0 : scalar > 1 ? 1 : scalar;
}

/* The volume, to the kernel. With the lock held. */
static void
volume_to_kernel(void)
{
	struct redwood_audio_volume v = { (uint32_t)lround(-scalar_to_db(volume) * 8), volume <= 0 };

	if (sink < 0 || ioctl(sink, REDWOOD_AUDIO_SET_VOLUME, &v) < 0) {
		say("redwood: audio: the volume %.3f could not be given to the kernel (%s)\n", volume, strerror(errno));
		return;
	}
	say("redwood: audio: the volume is %.3f: %s%.1f dB under the loudest\n", volume, v.muted ? "silence, " : "",
	    v.below / 8.0);
}

/* The device with this object number, or the one this stream belongs to; NULL = neither. */
static struct device *
device_of(AudioObjectID object)
{
	for (unsigned i = 0; i < DEVICES; i++) {
		if (object == devices[i].object || (object != 0 && (object == devices[i].output || object == devices[i].input))) {
			return &devices[i];
		}
	}
	return NULL;
}

/* The one format of a stream: 16-bit samples at REDWOOD_AUDIO_RATE, so many channels. */
static AudioStreamBasicDescription
format_of(AudioObjectID stream)
{
	const struct device *d = device_of(stream);
	UInt32 channels = stream == d->output ? d->output_channels : d->input_channels;

	return (AudioStreamBasicDescription) {
		       .mSampleRate = d->rate != 0 ? d->rate : REDWOOD_AUDIO_RATE, .mFormatID = kAudioFormatLinearPCM,
		       .mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
		       .mBytesPerPacket = 2 * channels, .mFramesPerPacket = 1, .mBytesPerFrame = 2 * channels,
		       .mChannelsPerFrame = channels, .mBitsPerChannel = 16,
	};
}

static HRESULT
query_interface(void *driver, REFIID uuid, LPVOID *interface)
{
	CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, uuid);
	Boolean ours = CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID);

	CFRelease(requested);
	if (interface == NULL || !ours) {
		return E_NOINTERFACE;
	}
	*interface = driver;            /* one driver, never freed: no counting */
	return S_OK;
}

static ULONG
add_ref(__unused void *driver)
{
	return 1;
}

static ULONG
release(__unused void *driver)
{
	return 1;
}

/*
 * Call route: the receiver unmuted = Receiver (1); else the speaker element unmuted (flag 2) or the receiver muted =
 * Speaker (2). Told to the kernel when it changes. With the lock held.
 */
static void
route_to_kernel(const char *why)
{
	uint32_t route = !mutes[0] ? 1 : 2;

	if (call_route < 1 || (int)route == route_told) {
		return;
	}
	if (sink < 0 || ioctl(sink, REDWOOD_AUDIO_SET_CALL_ROUTE, &route) < 0) {
		say("redwood: audio: call route %s (%s) could not be given to the kernel (%s)\n", route == 1 ? "Receiver" : "Speaker", why,
		    strerror(errno));
		return;
	}
	route_told = (int)route;
	say("redwood: audio: call route %s (%s: receiver %s, codec speaker %s)\n", route == 1 ? "Receiver" : "Speaker", why,
	    mutes[0] ? "muted" : "on", mutes[1] ? "muted" : "on");
}

static void
call_route_flag(void)
{
	char buf[8] = "";
	int f = open(CALL_ROUTE_FILE, O_RDONLY);

	if (f >= 0) {
		ssize_t n = read(f, buf, sizeof(buf) - 1);

		buf[n > 0 ? n : 0] = 0;
		close(f);
	}
	call_route = buf[0] >= '1' && buf[0] <= '5' ? buf[0] - '0' : 0;
	if (call_route >= 1) {
		devices[3].output = OBJECT_CODEC_OUTPUT;        /* "Codec" */
		devices[3].output_channels = 2;
	}
	if (BB_PRESENT) {
		devices_present = 5;            /* + "Baseband Voice" */
	}
	say("redwood: audio: call route %d (%s): %s\n", call_route, CALL_ROUTE_FILE, call_route == 0 ? "no receiver (as before)" :
	    call_route == 1 ? "the codec has the receiver (output element 3)" : call_route == 2 ?
	    "the codec has the receiver and a speaker (elements 3, 4)" : call_route == 3 ?
	    "the codec has the receiver (output element 3), and a silent stub device \"Baseband Voice\" (S1a)" : call_route == 4 ?
	    "the codec has the receiver (output element 3, played), and \"Baseband Voice\" carries the call through "
	    REDWOOD_VOICE_DEVICE " (stage 4, system clock)" :
	    "the codec has the receiver (output element 3, played), and \"Baseband Voice\" carries the call through "
	    REDWOOD_VOICE_DEVICE " (stage 4, clock from GET_TIME)");
}

/*
 * Stage 4: the next call's rate to the kernel (REDWOOD_VOICE_SET_RATE; the running call keeps its own). Through the open
 * device while the I/O runs, else opened for the moment. Not on the I/O thread.
 */
static void
voice_set_rate(Float64 rate, const char *why)
{
	uint32_t r = (uint32_t)rate;
	int f = voice, opened_here = 0;

	if (!BB_LIVE) {
		return;
	}
	if (f < 0) {
		f = open(REDWOOD_VOICE_DEVICE, O_RDWR | O_NONBLOCK);
		opened_here = f >= 0;
	}
	if (f < 0 || ioctl(f, REDWOOD_VOICE_SET_RATE, &r) < 0) {
		say("redwood: audio: baseband: rate %u Hz for the next call could not be given to the kernel (%s; %s)\n", r, why,
		    strerror(errno));
	} else {
		say("redwood: audio: baseband: rate %u Hz for the next call given to the kernel (%s)\n", r, why);
	}
	if (opened_here) {
		close(f);
	}
}

static OSStatus
initialize(__unused AudioServerPlugInDriverRef driver, AudioServerPlugInHostRef the_host)
{
	mach_timebase_info_data_t timebase;

	host = the_host;
	mach_timebase_info(&timebase);
	ticks_per_second = 1e9 * timebase.denom / timebase.numer;
	ticks_per_frame = ticks_per_second / REDWOOD_AUDIO_RATE;
	sink = open(REDWOOD_AUDIO_DEVICE, O_WRONLY);
	call_route_flag();
	say("redwood: audio: the speaker is with the HAL (%s: %s)\n", REDWOOD_AUDIO_DEVICE, sink < 0 ? strerror(errno) : "open");
	return 0;
}

static OSStatus
create_device(__unused AudioServerPlugInDriverRef driver, __unused CFDictionaryRef description,
    __unused const AudioServerPlugInClientInfo *client, __unused AudioObjectID *object)
{
	return kAudioHardwareUnsupportedOperationError;
}

static OSStatus
destroy_device(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object)
{
	return kAudioHardwareUnsupportedOperationError;
}

static OSStatus
device_client(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object,
    __unused const AudioServerPlugInClientInfo *client)
{
	return 0;
}

static OSStatus
configuration_change(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object, __unused UInt64 action,
    __unused void *info)
{
	return 0;
}

/* The stub's rate change, asked for by set_property_data() through the host: action = the new rate in Hz. */
static OSStatus
perform_configuration_change(__unused AudioServerPlugInDriverRef driver, AudioObjectID object, UInt64 action,
    __unused void *info)
{
	if (object == OBJECT_BB && BB_PRESENT) {
		pthread_mutex_lock(&lock);
		say("redwood: audio: baseband: rate %.0f -> %llu Hz%s\n", devices[4].rate, action,
		    devices[4].running ? " (while running: clock restarted)" : "");
		devices[4].rate = (Float64)action;
		devices[4].anchor = mach_absolute_time();
		devices[4].periods = 0;
		bb_period_host = devices[4].anchor;
		bb_tpf = 0;
		bb_fifo_head = bb_fifo_tail = 0;        /* the FIFO is at the device's rate: start over */
		bb_primed = false;
		pthread_mutex_unlock(&lock);
		voice_set_rate((Float64)action, "VirtualAudio set the device's rate");
	}
	return 0;
}

/* A property's value: `size` bytes of it, or a list with nothing in it, or (neither) no such property. */
struct value {
	UInt32 size;
	bool empty;
	bool string;                    /* a CFString: whoever is given it gets a reference of their own */
	union {
		UInt32 words[8];
		Float64 number;
		Float32 decimal;
		CFStringRef string;
		AudioValueRange range;
		AudioStreamBasicDescription format;
		AudioStreamRangedDescription ranged;
		AudioStreamRangedDescription ranged_list[2];
		AudioValueRange range_list[2];
	} u;
};

static struct value
property(AudioObjectID object, const AudioObjectPropertyAddress *address, UInt32 qualifier_size, const void *qualifier)
{
	UInt32 selector = address->mSelector, scope = address->mScope;
	const struct device *d = device_of(object);
	struct value v;

	memset(&v, 0, sizeof(v));
#define WORD(x)         (v.u.words[0] = (x), v.size = sizeof(UInt32))
#define LIST(x)         (v.u.words[v.size / sizeof(UInt32)] = (x), v.size += sizeof(UInt32))
#define STRING(x)       (v.u.string = (x), v.string = true, v.size = sizeof(CFStringRef))
#define FLOAT(x)        (v.u.decimal = (x), v.size = sizeof(Float32))
	if (object == OBJECT_SPEAKER_VOLUME) {
		switch (selector) {
		case kAudioObjectPropertyBaseClass:
			WORD(kAudioLevelControlClassID);
			break;
		case kAudioObjectPropertyClass:
			WORD(kAudioVolumeControlClassID);
			break;
		case kAudioObjectPropertyOwner:
			WORD(OBJECT_SPEAKER);
			break;
		case kAudioObjectPropertyOwnedObjects:
			v.empty = true;
			break;
		case kAudioControlPropertyScope:
			WORD(kAudioObjectPropertyScopeOutput);
			break;
		case kAudioControlPropertyElement:
			WORD(0);
			break;
		case kAudioLevelControlPropertyScalarValue:
			FLOAT(volume);
			break;
		case kAudioLevelControlPropertyDecibelValue:
			FLOAT(scalar_to_db(volume));
			break;
		case kAudioLevelControlPropertyDecibelRange:
			v.u.range.mMinimum = -VOLUME_RANGE_DB;
			v.u.range.mMaximum = 0;
			v.size = sizeof(AudioValueRange);
			break;
		case kAudioLevelControlPropertyConvertScalarToDecibels:
		case kAudioLevelControlPropertyConvertDecibelsToScalar:
			FLOAT(0);       /* (converts what it is given: get_property_data()) */
			break;
		}
		return v;
	}
	if ((object == OBJECT_RECEIVER_MUTE && call_route >= 1) || (object == OBJECT_SPEAKER_MUTE && call_route == 2)) {
		switch (selector) {
		case kAudioObjectPropertyBaseClass:
			WORD(kAudioBooleanControlClassID);
			break;
		case kAudioObjectPropertyClass:
			WORD(kAudioMuteControlClassID);
			break;
		case kAudioObjectPropertyOwner:
			WORD(OBJECT_CODEC);
			break;
		case kAudioObjectPropertyOwnedObjects:
			v.empty = true;
			break;
		case kAudioControlPropertyScope:
			WORD(kAudioObjectPropertyScopeOutput);
			break;
		case kAudioControlPropertyElement:
			WORD(object == OBJECT_RECEIVER_MUTE ? RECEIVER_ELEMENT : SPEAKER_ELEMENT);
			break;
		case kAudioBooleanControlPropertyValue:
			WORD(mutes[object == OBJECT_SPEAKER_MUTE]);
			break;
		}
		return v;
	}
	if (object == OBJECT_RECEIVER_MUTE || object == OBJECT_SPEAKER_MUTE) {
		return v;               /* not there without the flag */
	}
	if (object == OBJECT_BB_CLOCK) {
		if (!BB_PRESENT) {
			return v;
		}
		switch (selector) {
		case kAudioObjectPropertyBaseClass:
			WORD(kAudioSelectorControlClassID);
			break;
		case kAudioObjectPropertyClass:
			WORD(kAudioClockSourceControlClassID);
			break;
		case kAudioObjectPropertyOwner:
			WORD(OBJECT_BB);
			break;
		case kAudioObjectPropertyOwnedObjects:
			v.empty = true;
			break;
		case kAudioControlPropertyScope:
			WORD(kAudioObjectPropertyScopeGlobal);
			break;
		case kAudioControlPropertyElement:
			WORD(0);
			break;
		case kAudioSelectorControlPropertyCurrentItem:
		case kAudioSelectorControlPropertyAvailableItems:
			WORD(BB_CLOCK_SOURCE);
			break;
		case kAudioSelectorControlPropertyItemName:
			if (qualifier_size == sizeof(UInt32) && qualifier != NULL && *(const UInt32 *)qualifier == BB_CLOCK_SOURCE) {
				STRING(CFSTR("Baseband"));
			} else {
				STRING(CFSTR(""));
			}
			break;
		}
		return v;
	}
	if (object == OBJECT_CODEC_SOURCE) {
		switch (selector) {
		case kAudioObjectPropertyBaseClass:
			WORD(kAudioSelectorControlClassID);
			break;
		case kAudioObjectPropertyClass:
			WORD(kAudioDataSourceControlClassID);
			break;
		case kAudioObjectPropertyOwner:
			WORD(OBJECT_CODEC);
			break;
		case kAudioObjectPropertyOwnedObjects:
			v.empty = true;
			break;
		case kAudioControlPropertyScope:
			WORD(kAudioObjectPropertyScopeInput);
			break;
		case kAudioControlPropertyElement:
			WORD(0);
			break;
		case kAudioSelectorControlPropertyCurrentItem:
		case kAudioSelectorControlPropertyAvailableItems:
			WORD(MIC_SOURCE);
			break;
		case kAudioSelectorControlPropertyItemName:
			if (qualifier_size == sizeof(UInt32) && qualifier != NULL && *(const UInt32 *)qualifier == MIC_SOURCE) {
				STRING(CFSTR("Bottom"));
			} else {
				STRING(CFSTR(""));
			}
			break;
		}
		return v;
	}
	if (object != OBJECT_PLUGIN && d == NULL) {
		return v;
	}
	switch (selector) {
	case kAudioObjectPropertyBaseClass:
		WORD(kAudioObjectClassID);
		return v;
	case kAudioObjectPropertyClass:
		WORD(object == OBJECT_PLUGIN ? kAudioPlugInClassID : object == d->object ? kAudioDeviceClassID : kAudioStreamClassID);
		return v;
	case kAudioObjectPropertyOwner:
		WORD(object == OBJECT_PLUGIN ? 0 : object == d->object ? OBJECT_PLUGIN : d->object);
		return v;
	}
	if (object == OBJECT_PLUGIN) {
		switch (selector) {
		case kAudioObjectPropertyManufacturer:
			STRING(MANUFACTURER);
			break;
		case kAudioObjectPropertyOwnedObjects:
		case kAudioPlugInPropertyDeviceList:
			for (unsigned i = 0; i < DEVICES; i++) {
				LIST(devices[i].object);
			}
			break;
		case kAudioPlugInPropertyBoxList:
			v.empty = true;
			break;
		case kAudioPlugInPropertyTranslateUIDToBox:
			WORD(0);
			break;
		case kAudioPlugInPropertyTranslateUIDToDevice:
			WORD(0);
			for (unsigned i = 0; i < DEVICES; i++) {
				if (qualifier_size == sizeof(CFStringRef) && qualifier != NULL && *(const CFStringRef *)qualifier != NULL &&
				    CFEqual(*(const CFStringRef *)qualifier, devices[i].uid)) {
					WORD(devices[i].object);
				}
			}
			break;
		case kAudioPlugInPropertyResourceBundle:
			STRING(CFSTR(""));
			break;
		}
	} else if (object == d->object) {
		switch (selector) {
		case kAudioObjectPropertyManufacturer:
			STRING(d->object == OBJECT_BB ? BB_MANUFACTURER : MANUFACTURER);
			break;
		case kAudioObjectPropertyName:
		case kAudioDevicePropertyDeviceUID:
		case kAudioDevicePropertyModelUID:
			STRING(d->uid);
			break;
		case kAudioDevicePropertyTransportType:
			WORD(TRANSPORT_BUILT_IN);
			break;
		case kAudioDevicePropertyRelatedDevices:
			WORD(d->object);
			break;
		case kAudioDevicePropertyClockDomain:
		case kAudioDevicePropertyLatency:
		case kAudioDevicePropertySafetyOffset:
		case kAudioDevicePropertyIsHidden:
			WORD(0);
			break;
		case kAudioDevicePropertyDeviceIsAlive:
			WORD(1);
			break;
		case kAudioDevicePropertyDeviceCanBeDefaultDevice:
		case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
			WORD(d->output != 0 && d->object != OBJECT_BB);     /* the baseband is never a default device */
			break;
		case kAudioDevicePropertyDeviceIsRunning:
			WORD(d->running != 0);
			break;
		case kAudioObjectPropertyOwnedObjects:
		case kAudioDevicePropertyStreams:
			/* the streams of one direction, or of both (and the speaker's volume control, which it owns) */
			if (scope != kAudioObjectPropertyScopeInput && d->output != 0) {
				LIST(d->output);
			}
			if (scope != kAudioObjectPropertyScopeOutput && d->input != 0) {
				LIST(d->input);
			}
			if (selector == kAudioObjectPropertyOwnedObjects && d->object == OBJECT_SPEAKER) {
				LIST(OBJECT_SPEAKER_VOLUME);
			}
			if (selector == kAudioObjectPropertyOwnedObjects && d->object == OBJECT_CODEC && scope != kAudioObjectPropertyScopeOutput) {
				LIST(OBJECT_CODEC_SOURCE);
			}
			if (selector == kAudioObjectPropertyOwnedObjects && d->object == OBJECT_CODEC && scope != kAudioObjectPropertyScopeInput &&
			    call_route >= 1) {
				LIST(OBJECT_RECEIVER_MUTE);
				if (call_route == 2) {
					LIST(OBJECT_SPEAKER_MUTE);
				}
			}
			if (selector == kAudioObjectPropertyOwnedObjects && d->object == OBJECT_BB && scope == kAudioObjectPropertyScopeGlobal) {
				LIST(OBJECT_BB_CLOCK);
			}
			v.empty = v.size == 0;
			break;
		case kAudioObjectPropertyControlList:
			if (d->object == OBJECT_SPEAKER) {
				LIST(OBJECT_SPEAKER_VOLUME);
			}
			if (d->object == OBJECT_CODEC) {
				LIST(OBJECT_CODEC_SOURCE);
				if (call_route >= 1) {
					LIST(OBJECT_RECEIVER_MUTE);
				}
				if (call_route == 2) {
					LIST(OBJECT_SPEAKER_MUTE);
				}
			}
			if (d->object == OBJECT_BB) {
				LIST(OBJECT_BB_CLOCK);
			}
			v.empty = v.size == 0;
			break;
		case kAudioDevicePropertyNominalSampleRate:
			v.u.number = d->rate != 0 ? d->rate : REDWOOD_AUDIO_RATE;
			v.size = sizeof(Float64);
			break;
		case kAudioDevicePropertyAvailableNominalSampleRates:
			if (d->object == OBJECT_BB) {
				for (unsigned i = 0; i < BB_RATES; i++) {
					v.u.range_list[i].mMinimum = v.u.range_list[i].mMaximum = bb_rates[i];
				}
				v.size = BB_RATES * sizeof(AudioValueRange);
				break;
			}
			v.u.range.mMinimum = v.u.range.mMaximum = REDWOOD_AUDIO_RATE;
			v.size = sizeof(AudioValueRange);
			break;
		case kAudioDevicePropertyPreferredChannelsForStereo:
			if (d->object == OBJECT_BB) {
				break;          /* mono: none (asked of it = logged) */
			}
			v.u.words[0] = 1;
			v.u.words[1] = 2;
			v.size = 2 * sizeof(UInt32);
			break;
		case kAudioDevicePropertyZeroTimeStampPeriod:
			WORD(TIMESTAMP_PERIOD);
			break;
		}
	} else {
		switch (selector) {
		case kAudioStreamPropertyIsActive:
			WORD(!inactive[object]);
			break;
		case kAudioStreamPropertyDirection:
			WORD(object == d->input);
			break;
		case kAudioStreamPropertyTerminalType:
			WORD(object == d->input ? kAudioStreamTerminalTypeMicrophone : kAudioStreamTerminalTypeSpeaker);
			break;
		case kAudioStreamPropertyStartingChannel:
			WORD(1);
			break;
		case kAudioStreamPropertyLatency:
			WORD(0);
			break;
		case kAudioStreamPropertyVirtualFormat:
		case kAudioStreamPropertyPhysicalFormat:
			v.u.format = format_of(object);
			v.size = sizeof(AudioStreamBasicDescription);
			break;
		case kAudioStreamPropertyAvailableVirtualFormats:
		case kAudioStreamPropertyAvailablePhysicalFormats:
			if (d->object == OBJECT_BB) {
				/* mono S16 at each of the rates (BasebandVoice.driver's format: LPCM, flags 0xC, 16 bits, 1 channel) */
				for (unsigned i = 0; i < BB_RATES; i++) {
					v.u.ranged_list[i].mFormat = format_of(object);
					v.u.ranged_list[i].mFormat.mSampleRate = bb_rates[i];
					v.u.ranged_list[i].mSampleRateRange.mMinimum = v.u.ranged_list[i].mSampleRateRange.mMaximum = bb_rates[i];
				}
				v.size = BB_RATES * sizeof(AudioStreamRangedDescription);
				break;
			}
			v.u.ranged.mFormat = format_of(object);
			v.u.ranged.mSampleRateRange.mMinimum = v.u.ranged.mSampleRateRange.mMaximum = REDWOOD_AUDIO_RATE;
			v.size = sizeof(AudioStreamRangedDescription);
			break;
		}
	}
#undef WORD
#undef LIST
#undef STRING
#undef FLOAT
	return v;
}

/*
 * What the HAL (or the virtual audio device through it) asks of an object that it does not have,
 * the first times for each object, selector and scope: what a device it will not take may lack.
 */
#define UNKNOWN_TOLD            192
static UInt32 unknown_told[UNKNOWN_TOLD][3];
static unsigned unknown_count;

static void
unknown(AudioObjectID object, const AudioObjectPropertyAddress *address)
{
	UInt32 s = address->mSelector, c = address->mScope;

	for (unsigned i = 0; i < unknown_count; i++) {
		if (unknown_told[i][0] == object && unknown_told[i][1] == s && unknown_told[i][2] == c) {
			return;
		}
	}
	if (unknown_count >= UNKNOWN_TOLD) {
		return;
	}
	unknown_told[unknown_count][0] = object;
	unknown_told[unknown_count][1] = s;
	unknown_told[unknown_count][2] = c;
	unknown_count++;
	say("redwood: audio: asked of object %u (%s) what it does not have: '%c%c%c%c' (%#x), scope '%c%c%c%c', element %u\n",
	    (unsigned)object, device_of(object) ? CFStringGetCStringPtr(device_of(object)->uid, kCFStringEncodingUTF8) : "-",
	    (int)(s >> 24 & 0xff), (int)(s >> 16 & 0xff), (int)(s >> 8 & 0xff), (int)(s & 0xff), (unsigned)s,
	    (int)(c >> 24 & 0xff), (int)(c >> 16 & 0xff), (int)(c >> 8 & 0xff), (int)(c & 0xff), (unsigned)address->mElement);
}

static Boolean
has_property(__unused AudioServerPlugInDriverRef driver, AudioObjectID object, __unused pid_t client,
    const AudioObjectPropertyAddress *address)
{
	struct value v;

	if (address == NULL) {
		return false;
	}
	v = property(object, address, 0, NULL);
	if (v.size == 0 && !v.empty) {
		unknown(object, address);
	}
	return v.size != 0 || v.empty;
}

static OSStatus
is_property_settable(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t client,
    const AudioObjectPropertyAddress *address, Boolean *settable)
{
	const struct device *d = device_of(object);

	if (address == NULL || settable == NULL || !has_property(driver, object, client, address)) {
		return kAudioHardwareUnknownPropertyError;
	}
	/* the rate and the formats can be "set", to the ones there are; the volume can be set */
	if (object == OBJECT_SPEAKER_VOLUME) {
		*settable = address->mSelector == kAudioLevelControlPropertyScalarValue ||
		    address->mSelector == kAudioLevelControlPropertyDecibelValue;
	} else if (object == OBJECT_CODEC_SOURCE || object == OBJECT_BB_CLOCK) {
		*settable = address->mSelector == kAudioSelectorControlPropertyCurrentItem;
	} else if (object == OBJECT_RECEIVER_MUTE || object == OBJECT_SPEAKER_MUTE) {
		*settable = address->mSelector == kAudioBooleanControlPropertyValue;
	} else if (object == OBJECT_PLUGIN) {
		*settable = false;
	} else if (object == d->object) {
		*settable = address->mSelector == kAudioDevicePropertyNominalSampleRate;
	} else {
		*settable = address->mSelector == kAudioStreamPropertyIsActive ||
		    address->mSelector == kAudioStreamPropertyVirtualFormat || address->mSelector == kAudioStreamPropertyPhysicalFormat;
	}
	return 0;
}

static OSStatus
get_property_data_size(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t client,
    const AudioObjectPropertyAddress *address, UInt32 qualifier_size, const void *qualifier, UInt32 *size)
{
	if (address == NULL || size == NULL || !has_property(driver, object, client, address)) {
		return object < OBJECT_PLUGIN || object > OBJECT_LAST ? kAudioHardwareBadObjectError : kAudioHardwareUnknownPropertyError;
	}
	*size = property(object, address, qualifier_size, qualifier).size;
	return 0;
}

static OSStatus
get_property_data(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t client,
    const AudioObjectPropertyAddress *address, UInt32 qualifier_size, const void *qualifier, UInt32 room, UInt32 *size,
    void *data)
{
	struct value v;

	if (address == NULL || size == NULL || data == NULL || !has_property(driver, object, client, address)) {
		return object < OBJECT_PLUGIN || object > OBJECT_LAST ? kAudioHardwareBadObjectError : kAudioHardwareUnknownPropertyError;
	}
	if (object == OBJECT_SPEAKER_VOLUME && (address->mSelector == kAudioLevelControlPropertyConvertScalarToDecibels ||
	    address->mSelector == kAudioLevelControlPropertyConvertDecibelsToScalar)) {
		/* what is in the buffer, converted where it is */
		if (room < sizeof(Float32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		*(Float32 *)data = address->mSelector == kAudioLevelControlPropertyConvertScalarToDecibels ?
		    scalar_to_db(*(Float32 *)data) : db_to_scalar(*(Float32 *)data);
		*size = sizeof(Float32);
		return 0;
	}
	pthread_mutex_lock(&lock);
	v = property(object, address, qualifier_size, qualifier);
	pthread_mutex_unlock(&lock);
	if (v.size > room) {
		/* a list may be asked for in part; anything else must fit */
		if (address->mSelector != kAudioObjectPropertyOwnedObjects && address->mSelector != kAudioDevicePropertyStreams &&
		    address->mSelector != kAudioPlugInPropertyDeviceList && address->mSelector != kAudioObjectPropertyControlList) {
			return kAudioHardwareBadPropertySizeError;
		}
		v.size = room - room % sizeof(UInt32);
	}
	memcpy(data, &v.u, v.size);
	if (v.string) {
		CFRetain(v.u.string);
	}
	*size = v.size;
	return 0;
}

static OSStatus
set_property_data(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t client,
    const AudioObjectPropertyAddress *address, __unused UInt32 qualifier_size, __unused const void *qualifier, UInt32 size,
    const void *data)
{
	Boolean settable = false;

	if (address == NULL || data == NULL || is_property_settable(driver, object, client, address, &settable) != 0) {
		return kAudioHardwareUnknownPropertyError;
	}
	if (!settable) {
		return kAudioHardwareIllegalOperationError;
	}
	if (object == OBJECT_SPEAKER_VOLUME) {
		AudioObjectPropertyAddress changed[2] = {
			{ kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal, 0 },
			{ kAudioLevelControlPropertyDecibelValue, kAudioObjectPropertyScopeGlobal, 0 },
		};
		Float32 wanted;

		if (size != sizeof(Float32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		wanted = *(const Float32 *)data;
		wanted = address->mSelector == kAudioLevelControlPropertyDecibelValue ? db_to_scalar(wanted) :
		    wanted < 0 ? 0 : wanted > 1 ? 1 : wanted;
		pthread_mutex_lock(&lock);
		volume = wanted;
		volume_to_kernel();
		pthread_mutex_unlock(&lock);
		if (host != NULL) {
			host->PropertiesChanged(host, OBJECT_SPEAKER_VOLUME, 2, changed);
		}
		return 0;
	}
	if (object == OBJECT_RECEIVER_MUTE || object == OBJECT_SPEAKER_MUTE) {
		AudioObjectPropertyAddress changed = { kAudioBooleanControlPropertyValue, kAudioObjectPropertyScopeGlobal, 0 };

		if (size != sizeof(UInt32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		pthread_mutex_lock(&lock);
		if (BB_PRESENT) {
			say("redwood: audio: baseband: VirtualAudio sets the %s mute (element %d) to %u\n",
			    object == OBJECT_SPEAKER_MUTE ? "codec speaker" : "receiver",
			    object == OBJECT_SPEAKER_MUTE ? SPEAKER_ELEMENT : RECEIVER_ELEMENT, (unsigned)*(const UInt32 *)data);
		}
		mutes[object == OBJECT_SPEAKER_MUTE] = *(const UInt32 *)data != 0;
		route_to_kernel(object == OBJECT_SPEAKER_MUTE ? "speaker mute" : "receiver mute");
		pthread_mutex_unlock(&lock);
		if (host != NULL) {
			host->PropertiesChanged(host, object, 1, &changed);
		}
		return 0;
	}
	if (object == OBJECT_BB_CLOCK) {
		/* the one clock source there is, 'bb2w' (VirtualAudio's Device_Baseband_Voice selects it) */
		if (size != sizeof(UInt32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		say("redwood: audio: baseband: clock source '%c%c%c%c' asked for%s\n", (int)(*(const UInt32 *)data >> 24 & 0xff),
		    (int)(*(const UInt32 *)data >> 16 & 0xff), (int)(*(const UInt32 *)data >> 8 & 0xff), (int)(*(const UInt32 *)data & 0xff),
		    *(const UInt32 *)data == BB_CLOCK_SOURCE ? "" : " (refused)");
		return *(const UInt32 *)data == BB_CLOCK_SOURCE ? 0 : kAudioHardwareIllegalOperationError;
	}
	if (object == OBJECT_CODEC_SOURCE) {
		/* the one microphone there is */
		return size == sizeof(UInt32) ? (*(const UInt32 *)data == MIC_SOURCE ? 0 : kAudioHardwareIllegalOperationError) :
		       kAudioHardwareBadPropertySizeError;
	}
	if (address->mSelector == kAudioStreamPropertyIsActive) {
		if (size != sizeof(UInt32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		pthread_mutex_lock(&lock);
		inactive[object] = *(const UInt32 *)data == 0;
		pthread_mutex_unlock(&lock);
		if (object == OBJECT_BB_INPUT || object == OBJECT_BB_OUTPUT) {
			say("redwood: audio: baseband: %s stream %s\n", object == OBJECT_BB_INPUT ? "downlink (input)" : "uplink (output)",
			    *(const UInt32 *)data ? "active" : "inactive");
		}
		return 0;
	}
	if (object == OBJECT_BB || object == OBJECT_BB_INPUT || object == OBJECT_BB_OUTPUT) {
		Float64 wanted_rate;

		if (address->mSelector == kAudioDevicePropertyNominalSampleRate) {
			if (size != sizeof(Float64)) {
				return kAudioHardwareBadPropertySizeError;
			}
			wanted_rate = *(const Float64 *)data;
		} else if (size != sizeof(AudioStreamBasicDescription)) {
			return kAudioHardwareBadPropertySizeError;
		} else {
			const AudioStreamBasicDescription *f = data;

			say("redwood: audio: baseband: %s format asked for: %.0f Hz '%c%c%c%c' flags %#x, %u ch, %u bits\n",
			    object == OBJECT_BB_INPUT ? "downlink" : "uplink", f->mSampleRate, (int)(f->mFormatID >> 24 & 0xff),
			    (int)(f->mFormatID >> 16 & 0xff), (int)(f->mFormatID >> 8 & 0xff), (int)(f->mFormatID & 0xff),
			    (unsigned)f->mFormatFlags, (unsigned)f->mChannelsPerFrame, (unsigned)f->mBitsPerChannel);
			if (f->mFormatID != kAudioFormatLinearPCM || f->mChannelsPerFrame != 1 || f->mBitsPerChannel != 16 ||
			    !(f->mFormatFlags & kAudioFormatFlagIsSignedInteger) || (f->mFormatFlags & kAudioFormatFlagIsFloat)) {
				return kAudioDeviceUnsupportedFormatError;
			}
			wanted_rate = f->mSampleRate;
		}
		if (wanted_rate != bb_rates[0] && wanted_rate != bb_rates[1]) {
			say("redwood: audio: baseband: rate %.0f Hz refused (8000 or 16000)\n", wanted_rate);
			return kAudioDeviceUnsupportedFormatError;
		}
		if (wanted_rate == devices[4].rate) {
			say("redwood: audio: baseband: rate %.0f Hz (already)\n", wanted_rate);
			return 0;
		}
		/* a rate change goes through the host, which stops I/O around it and then calls perform_configuration_change() */
		say("redwood: audio: baseband: rate %.0f -> %.0f Hz asked for\n", devices[4].rate, wanted_rate);
		if (host == NULL || host->RequestDeviceConfigurationChange(host, OBJECT_BB, (UInt64)wanted_rate, NULL) != 0) {
			return kAudioHardwareUnsupportedOperationError;
		}
		return 0;
	}
	if (address->mSelector == kAudioDevicePropertyNominalSampleRate) {
		return size == sizeof(Float64) && *(const Float64 *)data == REDWOOD_AUDIO_RATE ? 0 : kAudioDeviceUnsupportedFormatError;
	}
	/* a format: the one we have, or nothing */
	if (size != sizeof(AudioStreamBasicDescription)) {
		return kAudioHardwareBadPropertySizeError;
	} else {
		const AudioStreamBasicDescription *wanted = data, format = format_of(object);

		return wanted->mSampleRate == format.mSampleRate && wanted->mFormatID == format.mFormatID &&
		       wanted->mChannelsPerFrame == format.mChannelsPerFrame && wanted->mBitsPerChannel == format.mBitsPerChannel &&
		       (wanted->mFormatFlags & kAudioFormatFlagIsSignedInteger) ? 0 : kAudioDeviceUnsupportedFormatError;
	}
}

/*
 * The microphone (the device "Codec"): /dev/redwood-mic is open while the input runs, and the kernel
 * listens only while it is open. A read gives what has come and never waits; what it does not
 * give is silence. Said when it starts and stops, with how much came.
 */
static int mic = -1;
static UInt64 mic_frames, mic_heard, mic_short;
static int mic_peak;

static void
codec_open(void)
{
	mic = open("/dev/redwood-mic", O_RDONLY | O_NONBLOCK);
	mic_frames = mic_heard = mic_short = 0;
	mic_peak = 0;
	say("redwood: audio: the microphone is listened to (/dev/redwood-mic: %s)\n", mic < 0 ? strerror(errno) : "open");
}

static void
codec_close(void)
{
	say("redwood: audio: the microphone stops: %llu.%02llu s asked for, %llu.%02llu s of it from the kernel (peak %d of 32768), "
	    "%llu reads short\n", mic_frames / REDWOOD_AUDIO_RATE, mic_frames % REDWOOD_AUDIO_RATE * 100 / REDWOOD_AUDIO_RATE,
	    mic_heard / REDWOOD_AUDIO_RATE, mic_heard % REDWOOD_AUDIO_RATE * 100 / REDWOOD_AUDIO_RATE, mic_peak, mic_short);
	if (mic >= 0) {
		close(mic);
	}
	mic = -1;
}

static void
codec_read(int16_t *samples, UInt32 frames)
{
	ssize_t got = mic >= 0 ? read(mic, samples, (size_t)frames * 2) : -1;
	UInt32 have = got > 0 ? (UInt32)got / 2 : 0;

	if (have > 0 && mic_heard == 0) {
		say("redwood: audio: the microphone's first samples, %llu.%02llu s after the input started\n",
		    mic_frames / REDWOOD_AUDIO_RATE, mic_frames % REDWOOD_AUDIO_RATE * 100 / REDWOOD_AUDIO_RATE);
	}
	for (UInt32 i = 0; i < have; i++) {
		int v = samples[i] < 0 ? -samples[i] : samples[i];

		mic_peak = v > mic_peak ? v : mic_peak;
	}
	if (have < frames) {
		memset(samples + have, 0, (size_t)(frames - have) * 2);
		mic_short += mic_heard > 0;
	}
	mic_frames += frames;
	mic_heard += have;
}

/*
 * Stage 4 (flags 4, 5): "Baseband Voice" on /dev/redwood-voice. Everything below runs on the HAL's I/O thread except
 * bb_live_start()/bb_live_stop() (StartIO/StopIO, lock held). Nothing here waits: the device is O_NONBLOCK.
 *   downlink: each ReadInput drains the kernel's whole frames into bb_fifo (at the device's rate; a call at the other
 *             rate is converted 2:1 or 1:2), then gives the HAL its frames once the FIFO has held one HAL buffer + 10 ms
 *             (after an underrun it fills to that again: silence meanwhile). More than that + 50 ms: trimmed to + 30 ms.
 *   uplink:   each WriteMix is written to the device (converted likewise); while the kernel holds more than 80 ms of it,
 *             the excess over 30 ms is not written (counted).
 * Lines: "redwood: audio: baseband: ..." (open, call up/down, 2 s statistics for the first 20 s of a run, then every 10 s).
 */
static UInt64
ms_ticks(UInt32 ms)
{
	return (UInt64)(ticks_per_second * ms / 1000.0);
}

static void
bb_open(UInt64 now)
{
	uint32_t r = (uint32_t)devices[4].rate;

	if (voice >= 0 || now < voice_retry) {
		return;
	}
	voice = open(REDWOOD_VOICE_DEVICE, O_RDWR | O_NONBLOCK);
	if (voice < 0) {
		if (errno != voice_errno) {
			say("redwood: audio: baseband: %s cannot be opened (%s): the call is silent both ways until it is "
			    "(tried again each second)\n", REDWOOD_VOICE_DEVICE, strerror(errno));
		}
		voice_errno = errno;
		voice_retry = now + ms_ticks(1000);
		return;
	}
	say("redwood: audio: baseband: %s open (fd %d%s)\n", REDWOOD_VOICE_DEVICE, voice, voice_errno ? ", after retries" : "");
	voice_errno = 0;
	if (ioctl(voice, REDWOOD_VOICE_SET_RATE, &r) < 0) {
		say("redwood: audio: baseband: REDWOOD_VOICE_SET_RATE %u failed (%s)\n", r, strerror(errno));
	}
	bb_state_next = 0;
}

static void
bb_live_start(struct device *d)
{
	memset(&bb, 0, sizeof(bb));
	bb_fifo_head = bb_fifo_tail = 0;
	bb_primed = false;
	bb_call_seen = 0;
	bb_t0_call = 0;
	bb_tpf = 0;
	bb_period_host = d->anchor;
	memset(&bb_state, 0, sizeof(bb_state));
	bb_state_next = bb_stats_next = 0;
	bb_stats_n = 0;
	voice_retry = 0;
	voice_errno = 0;
	bb_open(mach_absolute_time());
}

static void
bb_live_stop(struct device *d)
{
	struct redwood_voice_counters c;

	memset(&c, 0, sizeof(c));
	if (voice >= 0) {
		(void)ioctl(voice, REDWOOD_VOICE_GET_COUNTERS, &c);
	}
	say("redwood: audio: baseband: I/O stopped (#%llu) at %.0f Hz, %u call(s): dl %llu frames given, %llu underrun, %llu "
	    "trimmed, %llu converted, peak %d; ul %llu written, %llu trimmed, %llu errors, peak %d\n", bb_starts, d->rate, bb.calls,
	    bb.dl_got, bb.dl_under, bb.dl_trim, bb.dl_conv, bb.dl_peak, bb.ul_put, bb.ul_trim, bb.ul_err, bb.ul_peak);
	say("redwood: audio: baseband: kernel (last call): dl %u frames, %u unread, %u dropped; ul %u frames, %u underrun samples, "
	    "%u dropped\n", c.dl_frames, c.dl_unread, c.dl_dropped, c.ul_frames, c.ul_underruns, c.ul_dropped);
	if (voice >= 0) {
		close(voice);
	}
	voice = -1;
}

/* GET_TIME: the kernel's downlink clock against mach time over this call; flag 5 takes it as the device's period. */
static Float64
bb_clock(void)
{
	struct redwood_voice_time t;
	Float64 ppm = 0, krate = bb_state.rate ? bb_state.rate : 16000, drate = devices[4].rate;

	if (voice < 0 || ioctl(voice, REDWOOD_VOICE_GET_TIME, &t) < 0 || t.mach_time == 0 || t.rate == 0) {
		return 0;
	}
	if (t.call_count != bb_t0_call) {
		bb_t0_call = t.call_count;
		bb_t0_sample = t.sample;
		bb_t0_mach = t.mach_time;
		return 0;
	}
	if (t.sample <= bb_t0_sample || t.mach_time <= bb_t0_mach + ms_ticks(2000)) {
		return 0;
	}
	krate = t.rate;
	{
		Float64 tpf = (Float64)(t.mach_time - bb_t0_mach) / (Float64)(t.sample - bb_t0_sample);   /* per kernel sample */
		Float64 nominal = ticks_per_second / krate;

		ppm = (nominal / tpf - 1) * 1e6;        /* > 0: the kernel's downlink runs fast against mach time */
		if (call_route == 5 && t.mach_time > bb_t0_mach + ms_ticks(10000)) {
			Float64 want = tpf * krate / drate, dev = ticks_per_second / drate;

			want = want < dev * (1 - 1e-3) ? dev * (1 - 1e-3) : want > dev * (1 + 1e-3) ? dev * (1 + 1e-3) : want;
			pthread_mutex_lock(&lock);
			bb_tpf = bb_tpf == 0 ? want : bb_tpf + (want - bb_tpf) / 4;
			pthread_mutex_unlock(&lock);
		}
	}
	return ppm;
}

static void
bb_stats(UInt64 now)
{
	struct redwood_voice_counters c;
	Float64 drate = devices[4].rate, ppm = bb_clock();
	UInt32 fifo_ms = (UInt32)((bb_fifo_head - bb_fifo_tail) * 1000 / (UInt32)drate);

	memset(&c, 0, sizeof(c));
	if (voice >= 0) {
		(void)ioctl(voice, REDWOOD_VOICE_GET_COUNTERS, &c);
	}
	bb_stats_n++;
	bb_stats_next = now + ms_ticks(bb_stats_n < 10 ? 2000 : 10000);
	say("redwood: audio: baseband: stats: call %s #%u at %u Hz (device %.0f) | dl +%llu frames, fifo %u ms, under %llu trim %llu "
	    "peak %d | ul +%llu samples written, peak %d, trim %llu | clock %+.0f ppm%s\n", bb_state.call_up ? "up" : "down", bb_state.call_count,
	    bb_state.rate, drate, bb.dl_got - bb.stats_dl_got, fifo_ms, bb.dl_under, bb.dl_trim, bb.s_dl_peak,
	    bb.ul_put - bb.stats_ul_put, bb.s_ul_peak, bb.ul_trim, ppm, call_route == 5 ? " (followed)" : "");
	say("redwood: audio: baseband: kernel: dl %u frames (%u unread, %u dropped, %u queued), ul %u frames (%u underrun samples, "
	    "%u dropped, %u queued)%s\n", c.dl_frames, c.dl_unread, c.dl_dropped, bb_state.dl_queued, c.ul_frames, c.ul_underruns,
	    c.ul_dropped, bb_state.ul_queued, voice < 0 ? " - device NOT open" : "");
	bb.stats_dl_got = bb.dl_got;
	bb.stats_ul_put = bb.ul_put;
	bb.s_dl_peak = bb.s_ul_peak = 0;
}

static void
bb_poll(UInt64 now, Boolean force)
{
	struct redwood_voice_state s;

	if (voice < 0) {
		bb_open(now);
	}
	if (voice >= 0 && (force || now >= bb_state_next) && ioctl(voice, REDWOOD_VOICE_GET_STATE, &s) == 0) {
		bb_state_next = now + ms_ticks(100);
		if (s.call_up && s.call_count != bb_call_seen) {
			bb_call_seen = s.call_count;
			bb.calls++;
			bb_fifo_head = bb_fifo_tail = 0;
			bb_primed = false;
			bb.said_mismatch = false;
			say("redwood: audio: baseband: call #%u up (kernel mode %u): %u Hz, %u-sample frames; device %.0f Hz%s\n",
			    s.call_count, s.mode, s.rate, s.frame_samples, devices[4].rate, s.rate == (uint32_t)devices[4].rate ? "" :
			    " - converted here (VirtualAudio's rate came after the call started, or the kernel ignored SET_RATE)");
		} else if (!s.call_up && bb_state.call_up) {
			say("redwood: audio: baseband: call #%u down\n", bb_state.call_count);
		}
		bb_state = s;
	}
	if (now >= bb_stats_next) {
		bb_stats(now);
	}
}

static void
bb_fifo_push(int16_t v)
{
	if (bb_fifo_head - bb_fifo_tail >= BB_FIFO) {
		bb_fifo_tail++;
		bb.dl_trim++;
	}
	bb_fifo[bb_fifo_head++ & (BB_FIFO - 1)] = v;
}

/* the kernel's samples (n, at krate) into the FIFO at the device's rate */
static void
bb_fifo_put(const int16_t *s, UInt32 n, UInt32 krate, UInt32 drate)
{
	static int16_t prev;

	for (UInt32 i = 0; i < n; i++) {
		int v = s[i] < 0 ? -s[i] : s[i];

		bb.dl_peak = v > bb.dl_peak ? v : bb.dl_peak;
		bb.s_dl_peak = v > bb.s_dl_peak ? v : bb.s_dl_peak;
	}
	if (krate == drate || (krate != 2 * drate && drate != 2 * krate)) {
		if (krate != drate && !bb.said_mismatch) {
			bb.said_mismatch = true;
			say("redwood: audio: baseband: call at %u Hz, device at %u Hz: not convertible, passed as it is\n", krate, drate);
		}
		for (UInt32 i = 0; i < n; i++) {
			bb_fifo_push(s[i]);
		}
	} else if (krate == 2 * drate) {
		for (UInt32 i = 0; i + 1 < n; i += 2) {
			bb_fifo_push((int16_t)(((int32_t)s[i] + s[i + 1]) / 2));
		}
		bb.dl_conv += n;
	} else {
		for (UInt32 i = 0; i < n; i++) {
			bb_fifo_push((int16_t)(((int32_t)prev + s[i]) / 2));
			bb_fifo_push(s[i]);
			prev = s[i];
		}
		bb.dl_conv += n;
	}
	if (n > 0) {
		prev = s[n - 1];
	}
}

/* ReadInput of the baseband's input stream: the downlink */
static void
bb_downlink(int16_t *out, UInt32 frames)
{
	UInt64 now = mach_absolute_time();
	UInt32 drate = (UInt32)devices[4].rate, dfs = drate / 100, avail, n = 0;

	bb_poll(now, false);
	if (voice >= 0) {
		int16_t k[BB_KBUF];

		for (int i = 0; i < 4; i++) {
			ssize_t got = read(voice, k, sizeof(k));

			if (got < 0) {
				if (errno != EAGAIN && errno != EINTR) {
					bb.rd_err++;
					if (!bb.said_rd_err) {
						bb.said_rd_err = true;
						say("redwood: audio: baseband: read: %s\n", strerror(errno));
					}
				}
				break;
			}
			if (got == 0) {
				break;          /* no call */
			}
			if (!bb_state.call_up) {
				bb_poll(now, true);     /* frames before the 100 ms poll saw the call: its rate now */
			}
			bb_fifo_put(k, (UInt32)got / 2, bb_state.rate ? bb_state.rate : drate, drate);
			if ((size_t)got < sizeof(k)) {
				break;
			}
		}
	}
	avail = bb_fifo_head - bb_fifo_tail;
	if (!bb_primed && avail >= frames + dfs) {
		bb_primed = true;
	}
	if (bb_primed) {
		n = avail < frames ? avail : frames;
		for (UInt32 i = 0; i < n; i++) {
			out[i] = bb_fifo[bb_fifo_tail++ & (BB_FIFO - 1)];
		}
		if (n < frames) {
			bb.dl_under += frames - n;
			bb_primed = false;
		}
		bb.dl_got += n;
		avail = bb_fifo_head - bb_fifo_tail;
		if (avail > frames + 5 * dfs) {
			UInt32 drop = avail - (frames + 3 * dfs);

			bb_fifo_tail += drop;
			bb.dl_trim += drop;
		}
	}
	if (n < frames) {
		memset(out + n, 0, (size_t)(frames - n) * 2);
	}
}

/* WriteMix of the baseband's output stream: the uplink */
static void
bb_uplink(const int16_t *in, UInt32 frames)
{
	UInt64 now = mach_absolute_time();
	UInt32 drate = (UInt32)devices[4].rate, krate, kfs, skip = 0;
	int16_t k[2048];

	for (UInt32 i = 0; i < frames; i++) {
		int v = in[i] < 0 ? -in[i] : in[i];

		bb.ul_peak = v > bb.ul_peak ? v : bb.ul_peak;
		bb.s_ul_peak = v > bb.s_ul_peak ? v : bb.s_ul_peak;
	}
	bb_poll(now, false);
	if (voice < 0 || !bb_state.call_up) {
		return;                 /* no mode-2 call: the kernel would throw it away */
	}
	krate = bb_state.rate ? bb_state.rate : drate;
	kfs = krate / 100;
	if (bb_state.ul_queued > 8 * kfs) {
		skip = bb_state.ul_queued - 3 * kfs;    /* in kernel samples */
		bb_state.ul_queued -= skip;             /* (until the next poll) */
	}
	for (UInt32 at = 0; at < frames; ) {
		UInt32 chunk = frames - at > 1024 ? 1024 : frames - at, m = 0;

		if (krate == drate || (krate != 2 * drate && drate != 2 * krate)) {
			memcpy(k, in + at, (size_t)chunk * 2);
			m = chunk;
		} else if (krate == 2 * drate) {
			for (UInt32 i = 0; i < chunk; i++) {
				k[m++] = in[at + i];
				k[m++] = in[at + i];
			}
		} else {
			for (UInt32 i = 0; i + 1 < chunk; i += 2) {
				k[m++] = (int16_t)(((int32_t)in[at + i] + in[at + i + 1]) / 2);
			}
		}
		at += chunk;
		if (skip >= m) {
			skip -= m;
			bb.ul_trim += m;
			continue;
		}
		if (write(voice, k + skip, (size_t)(m - skip) * 2) < 0) {
			bb.ul_err++;
		} else {
			bb.ul_put += m - skip;
		}
		bb.ul_trim += skip;
		skip = 0;
	}
}

static void actuator_start(void);
static void actuator_stop(void);

static OSStatus
start_io(__unused AudioServerPlugInDriverRef driver, AudioObjectID object, __unused UInt32 client)
{
	struct device *d = device_of(object);

	if (d == NULL || object != d->object) {
		return kAudioHardwareBadObjectError;
	}
	pthread_mutex_lock(&lock);
	if (d->running++ == 0) {
		d->anchor = mach_absolute_time();
		d->periods = 0;
		if (d->output != 0 && d->object != OBJECT_CODEC && d->object != OBJECT_BB) {
			written = refused = 0;
			say("redwood: audio: playing\n");
		}
		if (d->object == OBJECT_BB) {
			bb_read = bb_written = 0;
			bb_peak = 0;
			bb_first_read = bb_first_write = false;
			bb_starts++;
			say("redwood: audio: baseband: I/O started (#%llu) at %.0f Hz: downlink %s, uplink %s; codec %s, speaker %s, "
			    "receiver %s\n", bb_starts, d->rate, inactive[OBJECT_BB_INPUT] ? "inactive" : "active",
			    inactive[OBJECT_BB_OUTPUT] ? "inactive" : "active", devices[3].running ? "running" : "idle",
			    devices[0].running ? "running" : "idle", mutes[0] ? "muted" : "on");
			if (BB_LIVE) {
				bb_live_start(d);
			}
		}
		if (d->object == OBJECT_CODEC) {
			codec_open();
		}
		if (d->object == OBJECT_ACTUATOR) {
			actuator_start();
		}
	}
	pthread_mutex_unlock(&lock);
	return 0;
}

static OSStatus
stop_io(__unused AudioServerPlugInDriverRef driver, AudioObjectID object, __unused UInt32 client)
{
	struct device *d = device_of(object);

	if (d == NULL || object != d->object) {
		return kAudioHardwareBadObjectError;
	}
	pthread_mutex_lock(&lock);
	if (d->running != 0 && --d->running == 0) {
		if (d->object == OBJECT_ACTUATOR) {
			actuator_stop();
		}
		if (d->object == OBJECT_BB && BB_LIVE) {
			bb_live_stop(d);
		} else if (d->object == OBJECT_BB) {
			say("redwood: audio: baseband: I/O stopped (#%llu) at %.0f Hz: %llu.%02llu s of downlink given (silence), "
			    "%llu.%02llu s of uplink taken and thrown away (peak %d of 32768)\n", bb_starts, d->rate,
			    bb_read / (UInt64)d->rate, bb_read % (UInt64)d->rate * 100 / (UInt64)d->rate,
			    bb_written / (UInt64)d->rate, bb_written % (UInt64)d->rate * 100 / (UInt64)d->rate, bb_peak);
		} else if (d->object == OBJECT_CODEC) {
			codec_close();
			if (receiver_frames > 0) {
				say("redwood: audio: the receiver was played %llu frames (%s)\n", receiver_frames, BB_LIVE ?
				    "to /dev/redwood-audio while unmuted" : "thrown away");
				receiver_frames = 0;
			}
			if (BB_LIVE && (rx_played > 0 || rx_dropped > 0 || spk_yielded > 0)) {
				say("redwood: audio: receiver: %llu frames to the sink, %llu while muted (not played); the speaker gave way "
				    "for %llu frames\n", rx_played, rx_dropped, spk_yielded);
				rx_played = rx_dropped = spk_yielded = 0;
			}
		} else if (d->output != 0) {
			say("redwood: audio: stopped after %llu.%02llu s (%llu frames not taken by %s)\n", written / REDWOOD_AUDIO_RATE,
			    written % REDWOOD_AUDIO_RATE * 100 / REDWOOD_AUDIO_RATE, refused, REDWOOD_AUDIO_DEVICE);
		}
	}
	pthread_mutex_unlock(&lock);
	return 0;
}

/* Where the device's clock was last at a multiple of TIMESTAMP_PERIOD frames, and when. */
static OSStatus
get_zero_time_stamp(__unused AudioServerPlugInDriverRef driver, AudioObjectID object, __unused UInt32 client,
    Float64 *sample_time, UInt64 *host_time, UInt64 *seed)
{
	Float64 ticks_per_period;
	UInt64 now = mach_absolute_time();
	struct device *d = device_of(object);

	if (d == NULL) {
		return kAudioHardwareBadObjectError;
	}
	pthread_mutex_lock(&lock);
	if (d->object == OBJECT_BB && call_route == 5) {
		/*
		 * Flag 5: the period follows the rate measured from GET_TIME (bb_tpf, set in bb_stats()); kept as the host time of
		 * the current period, so a new rate moves only the periods after it (no jump backwards).
		 */
		ticks_per_period = (bb_tpf != 0 ? bb_tpf : ticks_per_second / d->rate) * TIMESTAMP_PERIOD;
		if (bb_period_host == 0 || bb_period_host < d->anchor) {
			bb_period_host = d->anchor;
		}
		while (now >= bb_period_host + (UInt64)ticks_per_period) {
			bb_period_host += (UInt64)ticks_per_period;
			d->periods++;
		}
		*sample_time = (Float64)d->periods * TIMESTAMP_PERIOD;
		*host_time = bb_period_host;
		*seed = 1;
		pthread_mutex_unlock(&lock);
		return 0;
	}
	ticks_per_period = (d->rate != 0 ? ticks_per_second / d->rate : ticks_per_frame) * TIMESTAMP_PERIOD;
	while (now >= d->anchor + (UInt64)((Float64)(d->periods + 1) * ticks_per_period)) {
		d->periods++;
	}
	*sample_time = (Float64)d->periods * TIMESTAMP_PERIOD;
	*host_time = d->anchor + (UInt64)((Float64)d->periods * ticks_per_period);
	*seed = 1;
	pthread_mutex_unlock(&lock);
	return 0;
}

static OSStatus
will_do_io_operation(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object, __unused UInt32 client,
    UInt32 operation, Boolean *will_do, Boolean *in_place)
{
	*will_do = operation == kAudioServerPlugInIOOperationReadInput || operation == kAudioServerPlugInIOOperationWriteMix;
	*in_place = true;
	return 0;
}

static OSStatus
begin_end_io_operation(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object, __unused UInt32 client,
    __unused UInt32 operation, __unused UInt32 frames, __unused const AudioServerPlugInIOCycleInfo *cycle)
{
	return 0;
}

/*
 * What iOS plays on the actuator -- the haptics: the system's feedbacks and the ringer's vibration,
 * CoreHaptics' patterns, UIKit's feedbacks, as waveforms of one channel -- goes to the kernel's motor
 * driver (/dev/redwood-haptic, bsd/dev/sweet_audio.c, xnu 0914), which turns each write's peak into a
 * DRV2624 drive level. (iOS plays on this device at all only because the device tree says the phone's
 * haptics are "closed-loop": see mkadt.py.)
 *
 * Haptics apart from sound (ws-haptic-decouple). The virtual audio device runs the actuator together
 * with the speaker: every "playing" comes twice, and the actuator's frames add up to about half the
 * speaker's (r63a / live 2026-11-05: 109 s of actuator for 204 s of speaker), with peaks pinned at
 * 17681 (AUHapticsProtection's ceiling in 'd22_actuator_general'), so what is played reaches the motor
 * and the phone buzzes with its speaker. A DRV2624 is not a Taptic Engine: it cannot render audio, and
 * the kernel's level follower drives it flat out for anything over a third of full scale. So what the
 * actuator plays is judged in bursts (activity over HAP_ACTIVE, ended by HAP_BURST_END of quiet):
 *
 *   mode 0 (default, decoupled): a burst that starts while the speaker is silent is a haptic and is felt
 *          whole (taps, UIKit/CoreHaptics feedback, vibrate, the ringer's vibration with no tone). A burst
 *          that starts with sound on the speaker (within HAP_NEAR of its onset) is felt for its first
 *          HAP_PAIRED ms only -- the tap iOS pairs with a click, the keyboard, the charger -- and then
 *          silenced for as long as the speaker stays audible: what follows is the sound's copy.
 *   mode 1 (coupled, the old behaviour): everything goes to the motor.
 *   mode 2 (strict): nothing reaches the motor while the speaker is audible (within HAP_NEAR).
 *
 * The mode is the number in /private/var/local/haptic-audio (missing or unreadable: 0), read each time
 * the actuator starts, so it changes without a restart. Silenced frames go to the kernel as zeros.
 * Each actuator run ends with one line: its bursts, how they were judged, how long was felt and muted.
 */
#define HAPTIC_MODE_FILE        "/private/var/local/haptic-audio"
#define HAP_ACTIVE              1000    /* the kernel's HAP_FLOOR: under it the motor is off anyway */
#define SPK_AUDIBLE             64      /* a speaker block's peak over this (-54 dBFS): sound */
#define HAP_BURST_END_MS        30
#define HAP_NEAR_MS             30
#define HAP_DECIDE_MS           12      /* a burst's onset is judged once its first 12 ms are in */
#define HAP_PAIRED_MS           60
#define HAP_RECHECK_MS          100     /* ... and once more at 100 ms, when the speaker's blocks are surely in */
#define SPK_BLOCKS              64      /* the speaker's last writes: >= 1.3 s at 1024 frames */
#define HAP_SAID_MAX            400

static int haptic = -1;          /* the motor driver; -2: not there */
static unsigned haptic_said;
static int haptic_mode;

/* The speaker's recent writes, by host time: written on its I/O thread, read on the actuator's. */
static struct {
	volatile UInt64 start, end;
	volatile int peak;
} spk[SPK_BLOCKS];
static volatile unsigned spk_next;

static UInt64
frames_to_ticks(UInt64 frames)
{
	return (UInt64)((Float64)frames * ticks_per_frame);
}

static UInt64
io_host_time(const AudioServerPlugInIOCycleInfo *cycle)
{
	return cycle != NULL && cycle->mOutputTime.mHostTime != 0 ? cycle->mOutputTime.mHostTime : mach_absolute_time();
}

static void
speaker_note(const int16_t *samples, UInt32 frames, UInt64 when)
{
	int peak = 0;
	unsigned i = spk_next % SPK_BLOCKS;

	for (UInt32 k = 0; k < frames * REDWOOD_AUDIO_CHANNELS; k++) {
		int v = samples[k] < 0 ? -samples[k] : samples[k];

		peak = v > peak ? v : peak;
	}
	spk[i].end = 0;                 /* invalid while it changes */
	spk[i].start = when;
	spk[i].peak = peak;
	spk[i].end = when + frames_to_ticks(frames);
	spk_next++;
}

/* Is the speaker audible anywhere in [from, to]? */
static Boolean
speaker_audible(UInt64 from, UInt64 to)
{
	for (unsigned i = 0; i < SPK_BLOCKS; i++) {
		UInt64 end = spk[i].end, start = spk[i].start;

		if (end != 0 && spk[i].peak > SPK_AUDIBLE && start <= to && end >= from) {
			return true;
		}
	}
	return false;
}

/* this actuator run */
static struct {
	Boolean in_burst, decided, with_sound, rechecked;
	UInt64 onset, last_active;      /* host times */
	unsigned bursts, haptic_bursts, sound_bursts;
	UInt64 felt, muted, frames;     /* frames */
	int peak;
} hr;

static void
actuator_start(void)
{
	char buf[16] = "";
	int f = open(HAPTIC_MODE_FILE, O_RDONLY);

	if (f >= 0) {
		ssize_t n = read(f, buf, sizeof(buf) - 1);

		buf[n > 0 ? n : 0] = 0;
		close(f);
	}
	haptic_mode = buf[0] >= '0' && buf[0] <= '2' ? buf[0] - '0' : 0;
	memset(&hr, 0, sizeof(hr));
}

static void
actuator_stop(void)
{
	if ((hr.bursts > 0 || hr.peak > 0) && haptic_said < HAP_SAID_MAX) {
		haptic_said++;
		say("redwood: audio: actuator run %llu.%02llu s, mode %d: %u bursts (%u haptic, %u with sound), felt %llu ms, "
		    "muted %llu ms, peak %d of 32768%s\n", hr.frames / REDWOOD_AUDIO_RATE,
		    hr.frames % REDWOOD_AUDIO_RATE * 100 / REDWOOD_AUDIO_RATE, haptic_mode, hr.bursts, hr.haptic_bursts,
		    hr.sound_bursts, hr.felt * 1000 / REDWOOD_AUDIO_RATE, hr.muted * 1000 / REDWOOD_AUDIO_RATE, hr.peak,
		    haptic >= 0 ? "" : " (no /dev/redwood-haptic: not felt)");
	}
}

/* Should this block (of an actuator run, at host time when, with this peak) reach the motor? */
static Boolean
actuator_felt(int peak, UInt64 when, UInt32 frames)
{
	UInt64 near = frames_to_ticks(REDWOOD_AUDIO_RATE * HAP_NEAR_MS / 1000);
	UInt64 end = when + frames_to_ticks(frames);

	if (peak >= HAP_ACTIVE) {
		if (!hr.in_burst || when > hr.last_active + frames_to_ticks(REDWOOD_AUDIO_RATE * HAP_BURST_END_MS / 1000)) {
			hr.in_burst = true;
			hr.decided = hr.with_sound = hr.rechecked = false;
			hr.onset = when;
			hr.bursts++;
		}
		hr.last_active = end;
	}
	if (haptic_mode == 1) {
		return true;
	}
	if (haptic_mode == 2) {
		return !speaker_audible(when - near, end + near);
	}
	if (!hr.in_burst) {
		return true;            /* quiet: nothing to feel either way */
	}
	if (!hr.decided && end >= hr.onset + frames_to_ticks(REDWOOD_AUDIO_RATE * HAP_DECIDE_MS / 1000)) {
		hr.decided = true;
		hr.with_sound = speaker_audible(hr.onset - near, hr.onset + near);
		if (hr.with_sound) {
			hr.sound_bursts++;
		} else {
			hr.haptic_bursts++;
		}
	}
	if (hr.decided && !hr.with_sound && !hr.rechecked &&
	    end >= hr.onset + frames_to_ticks(REDWOOD_AUDIO_RATE * HAP_RECHECK_MS / 1000)) {
		/* the speaker's blocks for the onset may have come after the first judgement: once more */
		hr.rechecked = true;
		if (speaker_audible(hr.onset - near, hr.onset + near)) {
			hr.with_sound = true;
			hr.haptic_bursts--;
			hr.sound_bursts++;
		}
	}
	if (!hr.decided || !hr.with_sound) {
		return true;
	}
	if (when < hr.onset + frames_to_ticks(REDWOOD_AUDIO_RATE * HAP_PAIRED_MS / 1000)) {
		return true;            /* the tap paired with a sound */
	}
	return !speaker_audible(when - near, end + near);
}

static void
actuator_write(int16_t *samples, UInt32 frames, UInt64 when)
{
	int peak = 0;

	for (UInt32 i = 0; i < frames; i++) {
		int v = samples[i] < 0 ? -samples[i] : samples[i];

		peak = v > peak ? v : peak;
	}
	hr.frames += frames;
	hr.peak = peak > hr.peak ? peak : hr.peak;
	if (!actuator_felt(peak, when, frames)) {
		memset(samples, 0, (size_t)frames * 2);  /* in place: the HAL's own buffer, done with after this */
		hr.muted += frames;
	} else if (peak >= HAP_ACTIVE) {
		hr.felt += frames;
	}
	if (haptic == -1) {
		haptic = open("/dev/redwood-haptic", O_WRONLY | O_NONBLOCK);
		haptic = haptic >= 0 ? haptic : -2;
	}
	if (haptic >= 0) {
		(void)write(haptic, samples, (size_t)frames * 2);
	}
}

static OSStatus
do_io_operation(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object, AudioObjectID stream,
    __unused UInt32 client, UInt32 operation, UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle, void *buffer,
    __unused void *secondary)
{
	const struct device *d = device_of(stream);

	if (d == NULL) {
		return kAudioHardwareBadObjectError;
	}
	if (operation == kAudioServerPlugInIOOperationReadInput && stream == OBJECT_CODEC_INPUT) {
		codec_read(buffer, frames);
	} else if (operation == kAudioServerPlugInIOOperationReadInput && stream == OBJECT_BB_INPUT && BB_LIVE) {
		bb_downlink(buffer, frames);    /* stage 4: the call's downlink from /dev/redwood-voice */
		if (!bb_first_read) {
			bb_first_read = true;
			say("redwood: audio: baseband: first downlink read, %u frames\n", (unsigned)frames);
		}
		bb_read += frames;
	} else if (operation == kAudioServerPlugInIOOperationWriteMix && stream == OBJECT_BB_OUTPUT && BB_LIVE) {
		bb_uplink(buffer, frames);      /* stage 4: iOS's uplink to /dev/redwood-voice */
		if (!bb_first_write) {
			bb_first_write = true;
			say("redwood: audio: baseband: first uplink write, %u frames\n", (unsigned)frames);
		}
		bb_written += frames;
	} else if (operation == kAudioServerPlugInIOOperationReadInput && stream == OBJECT_BB_INPUT) {
		/* the downlink: silence (the call's audio is in the DSP) */
		memset(buffer, 0, (size_t)frames * 2);
		if (!bb_first_read) {
			bb_first_read = true;
			say("redwood: audio: baseband: first downlink read, %u frames\n", (unsigned)frames);
		}
		bb_read += frames;
	} else if (operation == kAudioServerPlugInIOOperationWriteMix && stream == OBJECT_BB_OUTPUT) {
		/* the uplink: thrown away; its peak is kept to show whether iOS sends the microphone here */
		const int16_t *samples = buffer;

		for (UInt32 i = 0; i < frames; i++) {
			int v = samples[i] < 0 ? -samples[i] : samples[i];

			bb_peak = v > bb_peak ? v : bb_peak;
		}
		if (!bb_first_write) {
			bb_first_write = true;
			say("redwood: audio: baseband: first uplink write, %u frames\n", (unsigned)frames);
		}
		bb_written += frames;
	} else if (operation == kAudioServerPlugInIOOperationReadInput) {
		/* the speaker's and Hawking's inputs: silence */
		memset(buffer, 0, (size_t)frames * 2 * d->input_channels);
	} else if (operation == kAudioServerPlugInIOOperationWriteMix && stream == OBJECT_CODEC_OUTPUT && BB_LIVE) {
		/*
		 * Stage 4: iOS plays the call itself; on the receiver port it comes here. Unmuted = the receiver is the route: to
		 * /dev/redwood-audio (same format as the speaker's), where the call's amplifier is the earpiece's (1086 route,
		 * from the mutes). Muted = VirtualAudio's speaker route: not played.
		 */
		receiver_frames += frames;
		if (!mutes[0]) {
			if (sink < 0 || write(sink, buffer, (size_t)frames * REDWOOD_AUDIO_FRAME) < 0) {
				refused += frames;
			}
			rx_last = io_host_time(cycle);
			rx_played += frames;
		} else {
			rx_dropped += frames;
		}
	} else if (operation == kAudioServerPlugInIOOperationWriteMix && stream == OBJECT_CODEC_OUTPUT) {
		receiver_frames += frames;      /* the receiver: thrown away (a call's audio runs in the DSP, xnu 1065) */
	} else if (operation == kAudioServerPlugInIOOperationWriteMix && stream == OBJECT_SPEAKER_OUTPUT && BB_LIVE && !mutes[0] &&
	    rx_last != 0 && io_host_time(cycle) < rx_last + ms_ticks(100)) {
		/* stage 4: the receiver plays (one writer on the sink at a time): the speaker gives way */
		spk_yielded += frames;
		written += frames;
	} else if (operation == kAudioServerPlugInIOOperationWriteMix && stream == OBJECT_ACTUATOR_OUTPUT) {
		actuator_write(buffer, frames, io_host_time(cycle));
	} else if (operation == kAudioServerPlugInIOOperationWriteMix && stream == d->output) {
		speaker_note(buffer, frames, io_host_time(cycle));
		/* the kernel takes it all or leaves it: this thread must not wait */
		if (sink < 0 || write(sink, buffer, (size_t)frames * REDWOOD_AUDIO_FRAME) < 0) {
			refused += frames;
		}
		written += frames;
	}
	return 0;
}

static AudioServerPlugInDriverInterface interface = {
	NULL, query_interface, add_ref, release, initialize, create_device, destroy_device, device_client, device_client,
	perform_configuration_change, configuration_change, has_property, is_property_settable, get_property_data_size,
	get_property_data, set_property_data, start_io, stop_io, get_zero_time_stamp, will_do_io_operation,
	begin_end_io_operation, do_io_operation, begin_end_io_operation,
};
static AudioServerPlugInDriverInterface *driver = &interface;

/* The factory named in the bundle's Info.plist. */
void *
RedwoodAudio_Create(__unused CFAllocatorRef allocator, CFUUIDRef type)
{
	return CFEqual(type, kAudioServerPlugInTypeUUID) ? &driver : NULL;
}
