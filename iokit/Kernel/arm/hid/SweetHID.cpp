/*
 * SWEET: HID event services for the iOS userland (backboardd's HID event
 * system).
 *
 * backboardd watches for IOService objects with HIDServiceSupport = true,
 * opens them with IOServiceOpen type 'esuc' (IOHIDEventServiceUserClient,
 * compiled here from IOHIDFamily-1915.140.3, the version the iOS 16.7
 * userland ships) and reads serialized IOHIDEvents from the user client's
 * shared queue. This file has the minimal IOHIDEventService base class and
 * two services:
 *
 *   SweetHIDButtons  consumer-control keys (power, volume, home)
 *   SweetHIDTouch    the built-in touch screen (digitizer collections)
 *
 * Hardware drivers feed them through the C entry points sweet_hid_key()
 * and sweet_hid_touch(). redfin 0971 adds the sensor services at the end
 * (SweetHIDAccel/Gyro/ALS/Prox/Compass/Pressure/Orientation, fed through
 * sweet_hid_sensor(): SweetMotion.h). Boot-arg sweethidtest=N (seconds) presses Home
 * every N seconds, to test the event path without input hardware.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSArray.h>
#include <pexpert/pexpert.h>

#include "IOHIDEventService.h"
#include "IOHIDEventServiceUserClient.h"
#include "IOHIDEvent.h"
#include "IOHIDKeys.h"
#include "IOHIDPrivateKeys.h"
#include "IOHIDUsageTables.h"
#include "../SweetDisplayMask.h"
#include "../SweetTrace.h"   /* 0994 */

extern "C" uint64_t mach_absolute_time(void);

/* One open client: the user client and its event callback. */
class IOHIDEventServiceClient : public OSObject {
	OSDeclareDefaultStructors(IOHIDEventServiceClient);
public:
	bool                       initClient() { return OSObject::init(); }
	IOService                 *client;
	void                      *context;
	IOHIDEventService::Action  action;
};
OSDefineMetaClassAndStructors(IOHIDEventServiceClient, OSObject);

#define super IOService
OSDefineMetaClassAndStructors(IOHIDEventService, IOService);

bool
IOHIDEventService::start(IOService *provider)
{
	if (!super::start(provider)) {
		return false;
	}
	_clientLock = IOLockAlloc();
	_clients = OSArray::withCapacity(2);
	_hidWorkLoop = getWorkLoop();
	if (!_clientLock || !_clients || !_hidWorkLoop) {
		return false;
	}
	_hidWorkLoop->retain();
	_hidGate = IOCommandGate::commandGate(this);
	if (!_hidGate || _hidWorkLoop->addEventSource(_hidGate) != kIOReturnSuccess) {
		return false;
	}

	OSString *transport = getTransport();
	OSString *product = getProduct();
	setProperty(kIOHIDTransportKey, transport);
	setProperty(kIOHIDProductKey, product);
	setProperty(kIOHIDManufacturerKey, "Sweet");
	setProperty(kIOHIDVendorIDKey, getVendorID(), 32);
	setProperty(kIOHIDProductIDKey, getProductID(), 32);
	setProperty(kIOHIDLocationIDKey, (unsigned long long)getProductID(), 32);
	setProperty(kIOHIDReportIntervalKey, getReportInterval(), 32);
	setProperty(kIOHIDPrimaryUsagePageKey, getPrimaryUsagePage(), 32);
	setProperty(kIOHIDPrimaryUsageKey, getPrimaryUsage(), 32);
	setProperty(kIOHIDBuiltInKey, kOSBooleanTrue);
	OSSafeReleaseNULL(transport);
	OSSafeReleaseNULL(product);

	OSDictionary *pair = OSDictionary::withCapacity(2);
	OSArray *pairs = OSArray::withCapacity(1);
	OSNumber *page = OSNumber::withNumber(getPrimaryUsagePage(), 32);
	OSNumber *usage = OSNumber::withNumber(getPrimaryUsage(), 32);
	if (pair && pairs && page && usage) {
		pair->setObject(kIOHIDDeviceUsagePageKey, page);
		pair->setObject(kIOHIDDeviceUsageKey, usage);
		pairs->setObject(pair);
		setProperty(kIOHIDDeviceUsagePairsKey, pairs);
	}
	OSSafeReleaseNULL(pair);
	OSSafeReleaseNULL(pairs);
	OSSafeReleaseNULL(page);
	OSSafeReleaseNULL(usage);

	/* What IOHIDFamily's "IOHIDEventServiceUserClient" personality merges in. */
	setProperty("HIDServiceSupport", kOSBooleanTrue);
	setProperty("IOUserClientClass", "IOHIDEventServiceUserClient");
	return true;
}

void
IOHIDEventService::free(void)
{
	if (_hidGate && _hidWorkLoop) {
		_hidWorkLoop->removeEventSource(_hidGate);
	}
	OSSafeReleaseNULL(_hidGate);
	OSSafeReleaseNULL(_hidWorkLoop);
	OSSafeReleaseNULL(_clients);
	if (_clientLock) {
		IOLockFree(_clientLock);
		_clientLock = NULL;
	}
	super::free();
}

bool
IOHIDEventService::open(IOService *client, IOOptionBits options, void *context, Action action)
{
	IOHIDEventServiceClient *data = OSTypeAlloc(IOHIDEventServiceClient);
	bool ok = false;

	if (data && data->initClient()) {
		data->client = client;
		data->context = context;
		data->action = action;
		ok = super::open(client, options, data);
	}
	OSSafeReleaseNULL(data);
	IOLog("SWEET HID: %s open by %s -> %d\n", getName(), client ? client->getName() : "?", ok);
	return ok;
}

void
IOHIDEventService::close(IOService *forClient, IOOptionBits options)
{
	super::close(forClient, options);
}

bool
IOHIDEventService::handleOpen(IOService *client, IOOptionBits options __unused, void *argument)
{
	IOHIDEventServiceClient *data = OSDynamicCast(IOHIDEventServiceClient, (OSObject *)argument);
	bool ok = false;

	if (!data) {
		return false;
	}
	IOLockLock(_clientLock);
	if (!handleIsOpenLocked(client)) {
		/* Copy on write: dispatchEvent iterates a snapshot without the lock. */
		OSArray *next = OSArray::withArray(_clients, _clients->getCount() + 1);
		if (next && next->setObject(data)) {
			_clients->release();
			_clients = next;
			ok = true;
		} else {
			OSSafeReleaseNULL(next);
		}
	}
	IOLockUnlock(_clientLock);
	return ok;
}

void
IOHIDEventService::handleClose(IOService *client, IOOptionBits options __unused)
{
	IOLockLock(_clientLock);
	OSArray *next = OSArray::withCapacity(_clients->getCount());
	if (next) {
		for (unsigned int i = 0; i < _clients->getCount(); i++) {
			IOHIDEventServiceClient *data = (IOHIDEventServiceClient *)_clients->getObject(i);
			if (data->client != client) {
				next->setObject(data);
			}
		}
		_clients->release();
		_clients = next;
	}
	IOLockUnlock(_clientLock);
}

bool
IOHIDEventService::handleIsOpenLocked(const IOService *client) const
{
	for (unsigned int i = 0; i < _clients->getCount(); i++) {
		IOHIDEventServiceClient *data = (IOHIDEventServiceClient *)_clients->getObject(i);
		if (client == NULL || data->client == client) {
			return true;
		}
	}
	return false;
}

bool
IOHIDEventService::handleIsOpen(const IOService *client) const
{
	IOLockLock(_clientLock);
	bool open = handleIsOpenLocked(client);
	IOLockUnlock(_clientLock);
	return open;
}

void
IOHIDEventService::dispatchEvent(IOHIDEvent *event, IOOptionBits options)
{
	OSArray *clients;

	event->setSenderID(getRegistryEntryID());
	IOLockLock(_clientLock);
	clients = _clients;
	clients->retain();
	IOLockUnlock(_clientLock);

	for (unsigned int i = 0; i < clients->getCount(); i++) {
		IOHIDEventServiceClient *data = (IOHIDEventServiceClient *)clients->getObject(i);
		if (data->action) {
			(*data->action)(data->client, this, data->context, event, options);
		}
	}
	clients->release();
}

IOHIDEvent *
IOHIDEventService::copyEvent(IOHIDEventType type __unused, IOHIDEvent *matching __unused, IOOptionBits options __unused)
{
	return NULL;
}

IOHIDEvent *
IOHIDEventService::copyMatchingEvent(OSDictionary *matching __unused)
{
	return NULL;
}

IOReturn
IOHIDEventService::setElementValue(UInt32 usagePage __unused, UInt32 usage __unused, UInt32 value __unused)
{
	return kIOReturnUnsupported;
}

IOReturn
IOHIDEventService::setProperties(OSObject *properties)
{
	/* The event system pushes its settings here; nothing to configure. */
	return OSDynamicCast(OSDictionary, properties) ? kIOReturnSuccess : kIOReturnBadArgument;
}

UInt32
IOHIDEventService::getPrimaryUsagePage()
{
	return 0;
}

UInt32
IOHIDEventService::getPrimaryUsage()
{
	return 0;
}

OSString *
IOHIDEventService::getTransport()
{
	return OSString::withCString("SPU");
}

OSString *
IOHIDEventService::getProduct()
{
	return OSString::withCString(getName());
}

UInt32
IOHIDEventService::getVendorID()
{
	return 0x05ac;
}

UInt32
IOHIDEventService::getProductID()
{
	return 0;
}

UInt32
IOHIDEventService::getReportInterval()
{
	return 10000;	/* us */
}

IOReturn
IOHIDEventService::newUserClient(task_t owningTask, void *securityID, UInt32 type,
    OSDictionary *properties, IOUserClient **handler)
{
	IOUserClient *client;

	if (type != kIOHIDEventServiceUserClientType) {
		IOLog("SWEET HID: %s user client type 0x%x unsupported\n", getName(), (unsigned)type);
		return super::newUserClient(owningTask, securityID, type, properties, handler);
	}
	client = OSTypeAlloc(IOHIDEventServiceUserClient);
	if (!client) {
		return kIOReturnNoMemory;
	}
	if (!client->initWithTask(owningTask, securityID, type)) {
		client->release();
		IOLog("SWEET HID: %s user client init refused\n", getName());
		return kIOReturnBadArgument;
	}
	if (!client->attach(this)) {
		client->release();
		return kIOReturnUnsupported;
	}
	if (!client->start(this)) {
		client->detach(this);
		client->release();
		return kIOReturnUnsupported;
	}
	*handler = client;
	return kIOReturnSuccess;
}

/*
 * Buttons. iOS takes the side/volume/home buttons as consumer-control
 * keyboard events (AppleM68Buttons publishes the same usage pair).
 */
class SweetHIDButtons : public IOHIDEventService {
	OSDeclareDefaultStructors(SweetHIDButtons);
public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void key(UInt32 usage, bool down);
protected:
	virtual UInt32 getPrimaryUsagePage() APPLE_KEXT_OVERRIDE { return kHIDPage_Consumer; }
	virtual UInt32 getPrimaryUsage() APPLE_KEXT_OVERRIDE { return kHIDUsage_Csmr_ConsumerControl; }
	virtual UInt32 getProductID() APPLE_KEXT_OVERRIDE { return 1; }
private:
	static void testFired(OSObject *owner, IOTimerEventSource *sender);
	IOTimerEventSource *fTest;
	UInt32 fTestSecs;
	UInt32 fTestCount;
};
OSDefineMetaClassAndStructors(SweetHIDButtons, IOHIDEventService);

/*
 * Touch screen: digitizer collection events with finger children,
 * coordinates normalized to 0..1 like IOHIDEventDriver produces for
 * absolute digitizers.
 */
#define SWEET_TOUCH_FINGERS 10
/*
 * Contacts that start this close to a panel edge (pixels, ~3 mm) carry
 * kIOHIDDigitizerEventFromEdgeTip for their whole life, as Apple's touch
 * firmware reports them. UIKit derives UITouch's edge type from that bit
 * (_UIEventHIDTouchEdgeTypeForHIDEvent); without it there is no home or
 * app-switcher gesture.
 */
#define SWEET_TOUCH_EDGE_PX 48
extern "C" int sweet_display_fit_x, sweet_display_fit_y, sweet_display_fit_w, sweet_display_fit_h;

/* Touch reports panel pixels; sunfish's panel is 1080x2340 (SWEET.h). */
#define SWEET_PANEL_W 1080
#define SWEET_PANEL_H 2340
class SweetHIDTouch : public IOHIDEventService {
	OSDeclareDefaultStructors(SweetHIDTouch);
public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void touch(int finger, bool down, int x, int y, uint64_t stamp = 0);
protected:
	virtual UInt32 getPrimaryUsagePage() APPLE_KEXT_OVERRIDE { return kHIDPage_Digitizer; }
	virtual UInt32 getPrimaryUsage() APPLE_KEXT_OVERRIDE { return kHIDUsage_Dig_TouchScreen; }
	virtual UInt32 getProductID() APPLE_KEXT_OVERRIDE { return 2; }
private:
	IOLock *fLock;
	int fLogicalW, fLogicalH, fOffX, fOffY;
	uint32_t fCornerRadius;
	bool fDown[SWEET_TOUCH_FINGERS];
	bool fEdge[SWEET_TOUCH_FINGERS];
	IOFixed fX[SWEET_TOUCH_FINGERS];
	IOFixed fY[SWEET_TOUCH_FINGERS];
};
OSDefineMetaClassAndStructors(SweetHIDTouch, IOHIDEventService);

static SweetHIDButtons *gSweetButtons;
static SweetHIDTouch *gSweetTouch;

bool
SweetHIDButtons::start(IOService *provider)
{
	if (!IOHIDEventService::start(provider)) {
		return false;
	}
	if (PE_parse_boot_argn("sweethidtest", &fTestSecs, sizeof(fTestSecs)) && fTestSecs) {
		fTest = IOTimerEventSource::timerEventSource(this, &SweetHIDButtons::testFired);
		if (fTest && _hidWorkLoop->addEventSource(fTest) == kIOReturnSuccess) {
			fTest->setTimeoutMS(fTestSecs * 1000);
		}
	}
	gSweetButtons = this;
	registerService();
	IOLog("SWEET HID: buttons registered (test every %u s)\n", fTestSecs);
	return true;
}

void
SweetHIDButtons::key(UInt32 usage, bool down)
{
	IOHIDEvent *event = IOHIDEvent::keyboardEvent(mach_absolute_time(), kHIDPage_Consumer, usage, down);

	if (event) {
		IOLog("SWEET HID: key 0x%x %s (%u clients)\n", (unsigned)usage, down ? "down" : "up",
		    _clients ? _clients->getCount() : 0);
		dispatchEvent(event);
		event->release();
	}
}

void
SweetHIDButtons::testFired(OSObject *owner, IOTimerEventSource *sender)
{
	SweetHIDButtons *self = (SweetHIDButtons *)owner;

	/*
	 * Home (Menu) press, then the release 100 ms later; Setup's "Hello"
	 * screen waits for it. Even counts press, odd counts release.
	 */
	bool press = (self->fTestCount & 1) == 0;
	self->key(kHIDUsage_Csmr_Menu, press);
	if (++self->fTestCount < 60) {
		sender->setTimeoutMS(press ? 100 : self->fTestSecs * 1000);
	}
}

bool
SweetHIDTouch::start(IOService *provider)
{
	fLock = IOLockAlloc();
	if (!fLock || !IOHIDEventService::start(provider)) {
		return false;
	}
	int native = 0;
	PE_parse_boot_argn("sweetfb_native", &native, sizeof(native));
	fLogicalW = native ? 1080 : 1125;
	fLogicalH = native ? SWEET_PANEL_H : 2436;
	fOffX = (fLogicalW - SWEET_PANEL_W) / 2;
	fOffY = (fLogicalH - SWEET_PANEL_H) / 2;
	fCornerRadius = SWEET_DISPLAY_CORNER_DEFAULT;
	PE_parse_boot_argn("sweetfb_corner_radius", &fCornerRadius, sizeof(fCornerRadius));
	if (fCornerRadius > SWEET_DISPLAY_CORNER_MAX) {
		fCornerRadius = SWEET_DISPLAY_CORNER_MAX;
	}
	/* The digitizer covers the main display (backboardd maps it 1:1). */
	setProperty("DisplayIntegrated", kOSBooleanTrue);
	setProperty("SurfaceWidth", (unsigned long long)(68 << 16), 32);	/* mm, 16.16 */
	setProperty("SurfaceHeight", (unsigned long long)(151 << 16), 32);
	gSweetTouch = this;
	registerService();
	IOLog("SWEET HID: touch registered\n");
	return true;
}

/*
 * 0945: what iOS sees, per source: real fingers, and finger 9 (tap / swipe
 * from the command channel). Intervals between the HID timestamps of one
 * contact's consecutive moves, stalls (> 20 ms while moving >= 8 px), and the
 * time dispatchEvent takes. One line every 5 s, or when the source lifts its
 * last finger after >= 10 moves (one scroll or one swipe).
 */
extern "C" void absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *result);
extern "C" void nanoseconds_to_absolutetime(uint64_t nanoseconds, uint64_t *result);
#define SWEET_TS_N 1024u
static struct {
	uint64_t start, disp_sum;
	uint32_t n, moves, gaps, disp_max, disp_n;
	uint32_t dt[SWEET_TS_N];
} gTouchStat[2];
static uint64_t gTouchLastT[SWEET_TOUCH_FINGERS];
static int gTouchLastX[SWEET_TOUCH_FINGERS], gTouchLastY[SWEET_TOUCH_FINGERS];

static uint32_t
sweetTouchUs(uint64_t abs)
{
	uint64_t ns;
	absolutetime_to_nanoseconds(abs, &ns);
	return ns / 1000u > 0xffffffffull ? 0xffffffffu : (uint32_t)(ns / 1000u);
}

static void
sweetTouchStat(IOLock *lock, const bool *fingersDown, int finger, bool was, bool down, bool moved,
    uint64_t stamp, int x, int y, uint32_t dispUs)
{
	int src = finger == 9 ? 1 : 0;
	uint32_t v[5] = {0}, n, moves, gaps, dmax, davg;
	bool report = false;
	uint64_t now = mach_absolute_time(), win;

	nanoseconds_to_absolutetime(5000000000ull, &win);
	IOLockLock(lock);
	auto &st = gTouchStat[src];
	if (!st.start && down) {
		st.start = now;
	}
	st.disp_sum += dispUs;
	st.disp_n++;
	st.disp_max = dispUs > st.disp_max ? dispUs : st.disp_max;
	if (was && down && moved) {
		st.moves++;
		if (gTouchLastT[finger] && stamp >= gTouchLastT[finger]) {
			uint32_t dt = sweetTouchUs(stamp - gTouchLastT[finger]);
			int d = (x > gTouchLastX[finger] ? x - gTouchLastX[finger] : gTouchLastX[finger] - x) +
			    (y > gTouchLastY[finger] ? y - gTouchLastY[finger] : gTouchLastY[finger] - y);
			if (st.n < SWEET_TS_N) {
				st.dt[st.n++] = dt;
			}
			if (dt > 20000u && d >= 8) {
				st.gaps++;
			}
		}
	}
	gTouchLastT[finger] = down ? stamp : 0;
	gTouchLastX[finger] = x;
	gTouchLastY[finger] = y;
	bool lifted = was && !down;
	for (int i = 0; lifted && i < SWEET_TOUCH_FINGERS; i++) {
		if (fingersDown[i] && (i == 9) == (src == 1)) {
			lifted = false;
		}
	}
	if (st.start && (now - st.start >= win || (lifted && st.n >= 10))) {
		for (uint32_t i = 1; i < st.n; ++i) {   /* insertion sort, n <= 1024 */
			uint32_t t = st.dt[i], j = i;
			while (j > 0 && st.dt[j - 1] > t) {
				st.dt[j] = st.dt[j - 1];
				--j;
			}
			st.dt[j] = t;
		}
		if (st.n) {
			v[0] = st.dt[(st.n - 1) * 5 / 100];
			v[1] = st.dt[(st.n - 1) * 50 / 100];
			v[2] = st.dt[(st.n - 1) * 95 / 100];
			v[3] = st.dt[st.n - 1];
		}
		report = true;
	}
	n = st.n;
	moves = st.moves;
	gaps = st.gaps;
	dmax = st.disp_max;
	davg = st.disp_n ? (uint32_t)(st.disp_sum / st.disp_n) : 0;
	if (report || (lifted && st.n < 10)) {
		bzero(&st, sizeof(st));
	}
	IOLockUnlock(lock);
	if (report) {
		IOLog("SWEET HID: %s: moves %u (dt %u) dt p5/p50/p95/max %u.%u/%u.%u/%u.%u/%u.%u ms"
		    " | gaps>20ms %u | dispatch avg/max %u/%u us\n", src ? "swipe" : "real", moves, n,
		    v[0] / 1000, v[0] % 1000 / 100, v[1] / 1000, v[1] % 1000 / 100,
		    v[2] / 1000, v[2] % 1000 / 100, v[3] / 1000, v[3] % 1000 / 100, gaps, davg, dmax);
	}
}

extern "C" void sweet_cpufreq_boost(int why);     /* 0970: bsd/dev/sweet_cpufreq.c */
extern "C" void sweet_touch_trace_event(uint64_t irq, uint64_t hid);     /* 1107 */
extern "C" uint32_t sweet_pace_trace;                                   /* 1114: SweetFramebuffer.cpp */
extern "C" void sweet_pace_trace_touch(int finger, bool down, bool was, uint64_t now);
/* 1044: the newest two touch event times (IRQ time with sectouchx 0x1), for SweetFramebuffer's touch -> latch */
extern "C" volatile uint64_t sweet_hid_touch_abs[2];
volatile uint64_t sweet_hid_touch_abs[2];

void
SweetHIDTouch::touch(int finger, bool down, int x, int y, uint64_t stamp)
{
	uint64_t now = stamp ? stamp : mach_absolute_time();
	if (now != sweet_hid_touch_abs[0]) {
		sweet_hid_touch_abs[1] = sweet_hid_touch_abs[0];        /* 1044 */
		sweet_hid_touch_abs[0] = now;
	}
	if (down) {
		sweet_cpufreq_boost(0);         /* 0970: touch boost (w boost 1 / sweetgov) */
	}
	IOHIDEvent *collection;
	bool any = false;

	if (finger < 0 || finger >= SWEET_TOUCH_FINGERS) {
		return;
	}
	x = x < 0 ? 0 : (x > SWEET_PANEL_W - 1 ? SWEET_PANEL_W - 1 : x);
	y = y < 0 ? 0 : (y > SWEET_PANEL_H - 1 ? SWEET_PANEL_H - 1 : y);

	IOLockLock(fLock);
	bool was = fDown[finger];
	int fitX = sweet_display_fit_x, fitY = sweet_display_fit_y;
	int fitW = sweet_display_fit_w, fitH = sweet_display_fit_h;
	if (fitW <= 0 || fitH <= 0) {
		fitX = fitY = 0;
		fitW = SWEET_PANEL_W;
		fitH = SWEET_PANEL_H;
	}
	/*
	 * No contact starts in a masked corner of the image (SweetDisplayMask.h);
	 * cx/cy: from the nearest image edge, -1 outside a corner (or in the bars).
	 */
	int ext = fCornerRadius ? (int)sweet_display_corner_extent(fCornerRadius) : 0;
	int cx = x - fitX < ext ? x - fitX : (fitX + fitW - 1 - x < ext ? fitX + fitW - 1 - x : -1);
	int cy = y - fitY < ext ? y - fitY : (fitY + fitH - 1 - y < ext ? fitY + fitH - 1 - y : -1);
	if (!was && down && cx >= 0 && cy >= 0 &&
	    !sweet_display_corner_point_visible((uint32_t)cx, (uint32_t)cy, fCornerRadius)) {
		IOLockUnlock(fLock);
		return;
	}
	/*
	 * Panel pixels -> the presented 1125x2436 display (SweetFramebuffer.cpp):
	 * the centre crop of the CPU copy, or the GPU's fit (sweet_display_fit_*).
	 */
	IOFixed nx, ny;
	if (sweet_display_fit_w > 0 && sweet_display_fit_h > 0) {
		int64_t fx = ((int64_t)(x - fitX) * 65536 + fitW / 2) / fitW;
		nx = (IOFixed)(fx < 0 ? 0 : (fx > 0xffff ? 0xffff : fx));
		int64_t fy = ((int64_t)(y - fitY) * 65536 + fitH / 2) / fitH;
		ny = (IOFixed)(fy < 0 ? 0 : (fy > 0xffff ? 0xffff : fy));
	} else {
		nx = (IOFixed)(((int64_t)(x + fOffX) << 16) / fLogicalW);
		ny = (IOFixed)(((int64_t)(y + fOffY) << 16) / fLogicalH);
	}
	bool moved = down && (nx != fX[finger] || ny != fY[finger]);
	if (was == down && !moved) {
		IOLockUnlock(fLock);
		return;
	}
	if (!was && down) {
		fEdge[finger] = x < SWEET_TOUCH_EDGE_PX || x > SWEET_PANEL_W - 1 - SWEET_TOUCH_EDGE_PX ||
		    y < SWEET_TOUCH_EDGE_PX || y > SWEET_PANEL_H - 1 - SWEET_TOUCH_EDGE_PX;
	}

	collection = IOHIDEvent::digitizerEvent(now, 0, kIOHIDDigitizerTransducerTypeHand, false, 0, 0, 0);
	if (!collection) {
		IOLockUnlock(fLock);
		return;
	}
	collection->setIntegerValue(kIOHIDEventFieldDigitizerCollection, TRUE);

	UInt32 cmask = 0;
	for (int i = 0; i < SWEET_TOUCH_FINGERS; i++) {
		bool d = (i == finger) ? down : fDown[i];
		if (i != finger && !d) {
			continue;
		}
		IOFixed fx = (i == finger && down) ? nx : fX[i];
		IOFixed fy = (i == finger && down) ? ny : fY[i];
		IOHIDEvent *child = IOHIDEvent::digitizerEvent(now, (UInt32)i + 2, kIOHIDDigitizerTransducerTypeFinger,
		    d, d ? 1 : 0, fx, fy);
		if (!child) {
			continue;
		}
		UInt32 mask = fEdge[i] ? kIOHIDDigitizerEventFromEdgeTip : 0;
		if (i == finger) {
			if (was != down) {
				mask |= kIOHIDDigitizerEventTouch | kIOHIDDigitizerEventRange;
			}
			if (moved) {
				mask |= kIOHIDDigitizerEventPosition;
			}
			if (!was && down) {
				mask |= kIOHIDDigitizerEventIdentity;
			}
		}
		child->setIntegerValue(kIOHIDEventFieldDigitizerIndex, i);
		child->setIntegerValue(kIOHIDEventFieldDigitizerIdentity, i + 2);
		child->setIntegerValue(kIOHIDEventFieldDigitizerTouch, d);
		child->setIntegerValue(kIOHIDEventFieldDigitizerEventMask, mask);
		collection->appendChild(child);
		child->release();
		cmask |= mask;
		any |= d;
	}
	fDown[finger] = down;
	if (down) {
		fX[finger] = nx;
		fY[finger] = ny;
	}
	collection->setFixedValue(kIOHIDEventFieldDigitizerX, nx);
	collection->setFixedValue(kIOHIDEventFieldDigitizerY, ny);
	collection->setIntegerValue(kIOHIDEventFieldDigitizerRange, any);
	collection->setIntegerValue(kIOHIDEventFieldDigitizerTouch, any);
	collection->setIntegerValue(kIOHIDEventFieldDigitizerEventMask, cmask);
	collection->setIntegerValue(kIOHIDEventFieldDigitizerButtonMask, any ? 1 : 0);
	IOLockUnlock(fLock);
	if (__improbable(sweet_pace_trace & 4)) {
		sweet_pace_trace_touch(finger, down, was, now);         /* 1114: touch state / move intervals for SWEET PACE */
	}

	if (was != down) {
		IOLog("SWEET HID: touch %d %s at %d,%d%s\n", finger, down ? "down" : "up", x, y,
		    fEdge[finger] ? " edge" : "");
	}
	uint64_t t1 = mach_absolute_time();
	dispatchEvent(collection);
	uint64_t t2 = mach_absolute_time();
	sweet_touch_trace_event(now, t2);       /* 1107: SweetFramebuffer.cpp, touch -> latch stages */
	uint32_t dispUs = sweetTouchUs(t2 - t1);
	collection->release();
	sweetTouchStat(fLock, fDown, finger, was, down, moved, now, x, y, dispUs);
}

/* key: 0 power (side button), 1 volume up, 2 volume down, 3 home */
extern "C" void
sweet_hid_key(int key, int down)
{
	static const UInt32 usages[] = {
		kHIDUsage_Csmr_Power, kHIDUsage_Csmr_VolumeIncrement,
		kHIDUsage_Csmr_VolumeDecrement, kHIDUsage_Csmr_Menu,
	};

	if (gSweetButtons && key >= 0 && key < (int)(sizeof(usages) / sizeof(usages[0]))) {
		gSweetButtons->key(usages[key], down != 0);
	}
}

/* 0945: with the sample's own time (mach absolute) as the HID timestamp. */
extern "C" void
sweet_hid_touch_at(int finger, int down, int x, int y, uint64_t stamp)
{
	SWEET_TRACE(SWT_TOUCH, finger, down, x, y);     /* 0994 */
	if (gSweetTouch) {
		gSweetTouch->touch(finger, down != 0, x, y, stamp);
	}
}

extern "C" void
sweet_hid_touch(int finger, int down, int x, int y)
{
	SWEET_TRACE(SWT_TOUCH, finger, down, x, y);     /* 0994 */
	if (gSweetTouch) {
		gSweetTouch->touch(finger, down != 0, x, y);
	}
}

/*
 * redfin 0971: motion and environment sensors. One IOHIDEventService per sensor with the usage pair an
 * iPhone's SPU sensor service publishes (Apple vendor page 0xff00: accel 3, ALS 4, prox 8, gyro 9, compass
 * 0xa, from IOHIDFamily-1035.70.7's AppleHIDUsageTables.h; pressure: HID sensor page atmospheric pressure;
 * orientation: HID sensor page device orientation 0x20/0x8a, what locationd matches). Samples come from /dev/redwood-motion (sensd, 0972) or
 * `poke m fake` through sweet_hid_sensor() (SweetMotion.h). Every open, user client, copyEvent type and
 * setProperties key/value is logged ("SWEET MOTION:"): that is how we learn what backboardd, locationd and
 * CoreBrightness ask for; user-space property reads of the services are logged too. Boot-arg sweetmotion=MASK
 * (default 0x77 = all but prox, bit = service index, 0 = none; 0x100 = Transport "SPU" instead of "Sweet";
 * 0x200 = initial copyEvent answers on every service).
 */
#include <libkern/c++/OSSerialize.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSBoolean.h>
#include <kern/clock.h>
#include <sys/errno.h>
#include "AppleHIDUsageTables.h"
#include "IOHIDEventFieldDefs.h"
#include "SweetMotion.h"

extern "C" int proc_selfpid(void);
extern "C" void proc_selfname(char *buf, int size);

struct SweetSensorInfo {
	const char     *name;
	UInt32          page, usage;
	UInt32          motionType;
	UInt32          productID;
	UInt32          interval;       /* default ReportInterval, us */
	IOHIDEventType  eventType;
};
static const SweetSensorInfo gSweetSensorInfo[SWEET_SENSOR_COUNT] = {
	{ "accel",       kHIDPage_AppleVendor,       kHIDUsage_AppleVendor_Accelerometer,      13, 0x10, 10000,  kIOHIDEventTypeAccelerometer },
	{ "gyro",        kHIDPage_AppleVendor,       kHIDUsage_AppleVendor_Gyro,               20, 0x11, 10000,  kIOHIDEventTypeGyro },
	{ "als",         kHIDPage_AppleVendor,       kHIDUsage_AppleVendor_AmbientLightSensor, 12, 0x12, 100000, kIOHIDEventTypeAmbientLightSensor },
	{ "prox",        kHIDPage_AppleVendor,       kHIDUsage_AppleVendor_ProximitySensor,    14, 0x13, 100000, kIOHIDEventTypeProximity },
	{ "compass",     kHIDPage_AppleVendor,       kHIDUsage_AppleVendor_Compass,            21, 0x14, 20000,  kIOHIDEventTypeCompass },
	{ "pressure",    kHIDPage_Sensor,            kHIDUsage_Snsr_Environmental_AtmosphericPressure, 31, 0x15, 200000, kIOHIDEventTypeAtmosphericPressure },
	/* locationd's CLOrientationNotifier (the AOP path) matches {0x20, 0x8a} and takes DeviceOrientationUsage 0x64-0x6a */
	{ "orientation", kHIDPage_Sensor,            kHIDUsage_Snsr_Orientation_DeviceOrientation, 10, 0x16, 0,  kIOHIDEventTypeOrientation },
};

/* Runtime knobs (`poke m knob NAME VALUE`); the defaults are guesses the phone has to confirm. */
static struct {
	int accsign;            /* -1: iOS g = -(Android m/s²) / 9.80665 (CoreMotion: face up reads z = -1 g) */
	int gyrodeg;            /* 1: gyro events in deg/s (HID sensor page units) instead of rad/s */
	uint32_t proxmask;      /* detection mask of a "near" proximity event */
	int orientbase;         /* orientation event usage = orientbase + UIDeviceOrientation: 100, i.e. 0x65 portrait ..
	                         * 0x6a face down (0x64 ambiguous), the range locationd's CLOrientationNotifier accepts */
	int orientauto;         /* derive orientation events from accel samples in the kernel */
	uint32_t logmax;        /* setProperties/copyEvent log lines per service */
	uint32_t every;         /* log every Nth posted sample (the first 3 always) */
} gSweetKnobs = { -1, 0, kIOHIDProximityDetectionLargeBodyContact | kIOHIDProximityDetectionReceiver, 100, 0, 64, 1000 };

/*
 * Default: everything but prox (0x77). On the phone (2026-10-26) backboardd crash-loops with
 * -[NSNull applyUIMode:] (after liblog_aop asserts) when the prox service (0x08) is registered (bisect: 0x43,
 * 0x23 fine, 0x0b crash). It reads DisplayIntegrated, ProtectedAccess and SensorPropertySupported from prox
 * (absent before): now published, and prox answers copyEvent with "far". Bit 0x100: Transport "SPU".
 * Bit 0x200: every sensor service answers copyEvent with a neutral sample until a real one exists (the ALS
 * always does), in case the NSNull is a nil copyEvent.
 */
#define SWEET_MOTION_SPU_TRANSPORT 0x100
#define SWEET_MOTION_INITIAL_EVENTS 0x200
static uint32_t gSweetMotionMask = 0x77;
static IOLock *gSweetReadLock;          /* SweetHIDSensor::logRead */
static bool gSweetMotionMaskRead;

extern "C" uint32_t
sweet_motion_mask(void)
{
	if (!gSweetMotionMaskRead) {
		PE_parse_boot_argn("sweetmotion", &gSweetMotionMask, sizeof(gSweetMotionMask));
		gSweetMotionMaskRead = true;
	}
	return gSweetMotionMask;
}

extern "C" int
sweet_motion_index(uint32_t type)
{
	for (int i = 0; i < SWEET_SENSOR_COUNT; i++) {
		if (gSweetSensorInfo[i].motionType == type) {
			return i;
		}
	}
	return -1;
}

/*
 * IEEE754 single (bits) * num / den as a fixed-point number with frac fraction bits, saturating, integer
 * arithmetic only (the kernel does no FP). num must stay below 2^20.
 */
static int64_t
sweet_f2q(uint32_t bits, int64_t num, int64_t den, int frac)
{
	int exp = (int)((bits >> 23) & 0xff);
	uint64_t mant = bits & 0x7fffff;
	bool neg = (bits >> 31) != 0;

	if (exp == 0xff || den <= 0) {
		return 0;
	}
	if (num < 0) {
		num = -num;
		neg = !neg;
	}
	if (exp == 0) {
		exp = 1;                /* denormal */
	} else {
		mant |= 0x800000;
	}
	uint64_t t = mant * (uint64_t)num;
	int sh = exp - 150 + frac;
	if (sh >= 0) {
		t = (sh >= 63 || t >= (1ULL << (63 - sh))) ? (1ULL << 62) : t << sh;
	} else {
		t = (-sh >= 64) ? 0 : t >> -sh;
	}
	t /= (uint64_t)den;
	if (t > 0x7fffffffULL) {
		t = 0x7fffffffULL;
	}
	return neg ? -(int64_t)t : (int64_t)t;
}

class SweetHIDSensor : public IOHIDEventService {
	OSDeclareDefaultStructors(SweetHIDSensor);
public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual bool open(IOService *client, IOOptionBits options, void *context, Action action) APPLE_KEXT_OVERRIDE;
	virtual void close(IOService *forClient, IOOptionBits options = 0) APPLE_KEXT_OVERRIDE;
	virtual IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
	virtual IOHIDEvent *copyEvent(IOHIDEventType type, IOHIDEvent *matching = 0, IOOptionBits options = 0) APPLE_KEXT_OVERRIDE;
	virtual IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
	    OSDictionary *properties, IOUserClient **handler) APPLE_KEXT_OVERRIDE;
	using IOHIDEventService::copyProperty;
	virtual OSPtr<OSObject> copyProperty(const char *aKey) const APPLE_KEXT_OVERRIDE;
	virtual bool serializeProperties(OSSerialize *s) const APPLE_KEXT_OVERRIDE;
	virtual UInt32 getPrimaryUsagePage() APPLE_KEXT_OVERRIDE { return gSweetSensorInfo[sensorIndex()].page; }
	virtual UInt32 getPrimaryUsage() APPLE_KEXT_OVERRIDE { return gSweetSensorInfo[sensorIndex()].usage; }

	int post(const struct sweet_motion_sample *s, IOHIDEvent **built = NULL);
	void noteReal(void) { OSIncrementAtomic64((volatile SInt64 *)&fReal); }     /* 1099: a sample from sensd/poke */
	int status(char *buf, int len);
	void want(struct sweet_motion_want *w);
	void relog();
protected:
	virtual int sensorIndex() { return SWEET_SENSOR_ACCEL; }
	virtual OSString *getProduct() APPLE_KEXT_OVERRIDE { return OSString::withCString(gSweetSensorInfo[sensorIndex()].name); }
	virtual UInt32 getProductID() APPLE_KEXT_OVERRIDE { return gSweetSensorInfo[sensorIndex()].productID; }
	virtual UInt32 getReportInterval() APPLE_KEXT_OVERRIDE { return gSweetSensorInfo[sensorIndex()].interval; }
	virtual OSString *getTransport() APPLE_KEXT_OVERRIDE
	{
		return OSString::withCString((sweet_motion_mask() & SWEET_MOTION_SPU_TRANSPORT) ? "SPU" : "Sweet");
	}
private:
	void logProperty(const OSSymbol *key, OSObject *value, int pid, const char *who);
	void logRead(const char *key, bool found) const;
	IOHIDEvent *initialEvent();
	static void initialFired(OSObject *owner, IOTimerEventSource *sender);
	IOTimerEventSource *fInitTimer;
	IOLock         *fLock;
	IOHIDEvent     *fLast;
	OSDictionary   *fSeen;          /* key -> last logged value text */
	uint32_t        fLogged, fCopyTypes, fClients;
	uint32_t        fInterval, fIntervalSet;
	uint64_t        fEvents;
	uint64_t        fReal;          /* 1099: samples from user space (sensd, poke m) */
	uint64_t        fKeep;          /* 1099: keep-alive samples sent */
	void            keepAliveArm(void);
};
OSDefineMetaClassAndStructors(SweetHIDSensor, IOHIDEventService);

#define SWEET_SENSOR_CLASS(cls, idx)                                                    \
	class cls : public SweetHIDSensor {                                             \
	        OSDeclareDefaultStructors(cls);                                         \
	protected:                                                                      \
	        virtual int sensorIndex() APPLE_KEXT_OVERRIDE { return idx; }           \
	};                                                                              \
	OSDefineMetaClassAndStructors(cls, SweetHIDSensor)

SWEET_SENSOR_CLASS(SweetHIDAccel, SWEET_SENSOR_ACCEL);
SWEET_SENSOR_CLASS(SweetHIDGyro, SWEET_SENSOR_GYRO);
SWEET_SENSOR_CLASS(SweetHIDALS, SWEET_SENSOR_ALS);
SWEET_SENSOR_CLASS(SweetHIDProx, SWEET_SENSOR_PROX);
SWEET_SENSOR_CLASS(SweetHIDCompass, SWEET_SENSOR_COMPASS);
SWEET_SENSOR_CLASS(SweetHIDPressure, SWEET_SENSOR_PRESSURE);
SWEET_SENSOR_CLASS(SweetHIDOrientation, SWEET_SENSOR_ORIENT);

static SweetHIDSensor *gSweetSensors[SWEET_SENSOR_COUNT];

bool
SweetHIDSensor::start(IOService *provider)
{
	int i = sensorIndex();
	const SweetSensorInfo *info = &gSweetSensorInfo[i];

	if (!(sweet_motion_mask() & (1u << i))) {
		IOLog("SWEET MOTION: %s off (sweetmotion=0x%x)\n", info->name, sweet_motion_mask());
		return false;
	}
	if (!gSweetReadLock) {
		IOLock *l = IOLockAlloc();
		if (l && !OSCompareAndSwapPtr(NULL, l, (void * volatile *)&gSweetReadLock)) {
			IOLockFree(l);
		}
	}
	fLock = IOLockAlloc();
	fSeen = OSDictionary::withCapacity(8);
	if (!fLock || !fSeen || !IOHIDEventService::start(provider)) {
		return false;
	}
	fInterval = info->interval;
	if (i == SWEET_SENSOR_ALS) {
		fInitTimer = IOTimerEventSource::timerEventSource(this, &SweetHIDSensor::initialFired);
		if (fInitTimer && _hidWorkLoop->addEventSource(fInitTimer) != kIOReturnSuccess) {
			OSSafeReleaseNULL(fInitTimer);
		}
	}
	setProperty("SweetSensor", info->name);
	if (i == SWEET_SENSOR_PROX) {
		/* What BackBoard's proximity handling read and found absent (r23c); values are guesses. */
		setProperty("DisplayIntegrated", kOSBooleanTrue);
		setProperty("ProtectedAccess", kOSBooleanFalse);
		setProperty("SensorPropertySupported", kOSBooleanTrue);
	}
	setProperty(kIOHIDBatchIntervalKey, 0ULL, 32);
	setProperty(kIOHIDSensorPropertySampleIntervalKey, (unsigned long long)info->interval, 32);
	gSweetSensors[i] = this;
	registerService();
	IOLog("SWEET MOTION: %s registered: usage page 0x%x usage 0x%x, event type %u, ReportInterval %u us, Transport %s\n",
	    info->name, (unsigned)info->page, (unsigned)info->usage, (unsigned)info->eventType, (unsigned)info->interval,
	    (sweet_motion_mask() & SWEET_MOTION_SPU_TRANSPORT) ? "SPU" : "Sweet");
	return true;
}

void
SweetHIDSensor::free(void)
{
	if (fInitTimer) {
		fInitTimer->cancelTimeout();
		if (_hidWorkLoop) {
			_hidWorkLoop->removeEventSource(fInitTimer);
		}
		OSSafeReleaseNULL(fInitTimer);
	}
	OSSafeReleaseNULL(fLast);
	OSSafeReleaseNULL(fSeen);
	if (fLock) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	IOHIDEventService::free();
}

IOReturn
SweetHIDSensor::newUserClient(task_t owningTask, void *securityID, UInt32 type,
    OSDictionary *properties, IOUserClient **handler)
{
	char who[32] = "?";
	proc_selfname(who, sizeof(who));
	IOReturn ret = IOHIDEventService::newUserClient(owningTask, securityID, type, properties, handler);
	IOLog("SWEET MOTION: %s user client type '%c%c%c%c' for pid %d (%s) -> 0x%x\n",
	    gSweetSensorInfo[sensorIndex()].name, (char)(type >> 24), (char)(type >> 16), (char)(type >> 8),
	    (char)type, proc_selfpid(), who, ret);
	return ret;
}

bool
SweetHIDSensor::open(IOService *client, IOOptionBits options, void *context, Action action)
{
	char who[32] = "?";
	proc_selfname(who, sizeof(who));
	bool ok = IOHIDEventService::open(client, options, context, action);
	if (ok) {
		OSIncrementAtomic((volatile SInt32 *)&fClients);
		if (fInitTimer && fEvents == 0) {
			fInitTimer->setTimeoutMS(500);     /* an ALS reading soon after each open, as a real ALS sends */
		}
	}
	IOLog("SWEET MOTION: %s opened by pid %d (%s) options 0x%x -> %d, %u clients\n",
	    gSweetSensorInfo[sensorIndex()].name, proc_selfpid(), who, (unsigned)options, ok, fClients);
	return ok;
}

void
SweetHIDSensor::close(IOService *forClient, IOOptionBits options)
{
	char who[32] = "?";
	proc_selfname(who, sizeof(who));
	if (fClients) {
		OSDecrementAtomic((volatile SInt32 *)&fClients);
	}
	IOLog("SWEET MOTION: %s closed by pid %d (%s), %u clients\n", gSweetSensorInfo[sensorIndex()].name,
	    proc_selfpid(), who, fClients);
	IOHIDEventService::close(forClient, options);
}

static void
sweetFormat(OSObject *o, char *buf, size_t len)
{
	OSNumber *n;
	OSString *s;
	OSData *d;
	OSBoolean *b;

	if ((n = OSDynamicCast(OSNumber, o))) {
		snprintf(buf, len, "%llu (0x%llx, %u-bit)", n->unsigned64BitValue(), n->unsigned64BitValue(),
		    n->numberOfBits());
	} else if ((b = OSDynamicCast(OSBoolean, o))) {
		snprintf(buf, len, "%s", b->isTrue() ? "true" : "false");
	} else if ((s = OSDynamicCast(OSString, o))) {
		snprintf(buf, len, "\"%s\"", s->getCStringNoCopy());
	} else if ((d = OSDynamicCast(OSData, o))) {
		const uint8_t *p = (const uint8_t *)d->getBytesNoCopy();
		size_t at = (size_t)snprintf(buf, len, "<%u bytes:", d->getLength());
		for (unsigned i = 0; i < d->getLength() && i < 24 && at + 4 < len; i++) {
			at += (size_t)snprintf(buf + at, len - at, " %02x", p[i]);
		}
		snprintf(buf + at, len > at ? len - at : 0, ">");
	} else if (o) {
		OSSerialize *ser = OSSerialize::withCapacity(256);
		if (ser && o->serialize(ser)) {
			snprintf(buf, len, "%.180s", ser->text());
		} else {
			snprintf(buf, len, "<%s>", o->getMetaClass()->getClassName());
		}
		OSSafeReleaseNULL(ser);
	} else {
		snprintf(buf, len, "NULL");
	}
}

void
SweetHIDSensor::logProperty(const OSSymbol *key, OSObject *value, int pid, const char *who)
{
	char text[200];
	sweetFormat(value, text, sizeof(text));
	OSString *now = OSString::withCString(text);
	OSString *was = OSDynamicCast(OSString, fSeen->getObject(key));
	if (now && (!was || !was->isEqualTo(now))) {
		if (fLogged < gSweetKnobs.logmax) {
			IOLog("SWEET MOTION: %s setProperties %s = %s (pid %d %s)\n", gSweetSensorInfo[sensorIndex()].name,
			    key->getCStringNoCopy(), text, pid, who);
			if (++fLogged == gSweetKnobs.logmax) {
				IOLog("SWEET MOTION: %s: log limit reached (poke m log / m knob logmax N)\n",
				    gSweetSensorInfo[sensorIndex()].name);
			}
		}
		fSeen->setObject(key, now);
	}
	OSSafeReleaseNULL(now);
}

IOReturn
SweetHIDSensor::setProperties(OSObject *properties)
{
	OSDictionary *dict = OSDynamicCast(OSDictionary, properties);
	char who[32] = "?";
	int pid = proc_selfpid();

	if (!dict) {
		return kIOReturnBadArgument;
	}
	proc_selfname(who, sizeof(who));
	OSCollectionIterator *it = OSCollectionIterator::withCollection(dict);
	if (!it) {
		return kIOReturnNoMemory;
	}
	bool armKeep = false;
	IOLockLock(fLock);
	while (const OSSymbol *key = OSDynamicCast(OSSymbol, it->getNextObject())) {
		OSObject *value = dict->getObject(key);
		logProperty(key, value, pid, who);
		OSNumber *n = OSDynamicCast(OSNumber, value);
		if (n && key->isEqualTo(kIOHIDReportIntervalKey)) {
			fInterval = n->unsigned32BitValue();
			fIntervalSet = 1;
			armKeep = true;
		}
		/* Keep the sensor settings visible in the registry, as IOHIDEventService does. */
		if (n && (key->isEqualTo(kIOHIDReportIntervalKey) || key->isEqualTo(kIOHIDBatchIntervalKey) ||
		    key->isEqualTo(kIOHIDSensorPropertySampleIntervalKey) ||
		    key->isEqualTo(kIOHIDSensorPropertyMaxFIFOEventsKey))) {
			setProperty(key, n);
		}
	}
	IOLockUnlock(fLock);
	it->release();
	if (armKeep) {
		keepAliveArm();         /* 1099 */
	}
	return kIOReturnSuccess;
}

/*
 * redfin 1099 (kern.sweet_bl_fix & 4, default on): until sensd's first ALS sample, a neutral 100 lux sample at the
 * client's ReportInterval (100 ms at the fastest), as long as the client asks for samples (ReportInterval > 0).
 * At a cold boot CoreBrightness configures the ALS and turns the display on at ~55 s, but sensd only starts at
 * ~85 s: CoreBrightness then had one 100 lux sample (the 500 ms initial event, sent before it set ReportInterval)
 * and nothing else; after a backboardd restart it starts with sensd's samples flowing. Log: "SWEET MOTION: als
 * keep-alive". Stops for good at the first real sample.
 */
extern "C" uint32_t sweet_bl_fix;
void
SweetHIDSensor::keepAliveArm(void)
{
	if (sensorIndex() != SWEET_SENSOR_ALS || !(sweet_bl_fix & 4) || fReal != 0 || fInitTimer == NULL) {
		return;
	}
	uint32_t us = fInterval;
	if (us == 0) {
		return;
	}
	fInitTimer->setTimeoutUS(us < 100000 ? 100000 : us);
}

IOHIDEvent *
SweetHIDSensor::copyEvent(IOHIDEventType type, IOHIDEvent *matching __unused, IOOptionBits options)
{
	IOHIDEvent *event = NULL;
	char who[32] = "?";

	IOLockLock(fLock);
	if (type < 32 && !(fCopyTypes & (1u << type)) && fLogged < gSweetKnobs.logmax) {
		fCopyTypes |= 1u << type;
		fLogged++;
		proc_selfname(who, sizeof(who));
		IOLog("SWEET MOTION: %s copyEvent type %u options 0x%x by pid %d (%s): %s\n",
		    gSweetSensorInfo[sensorIndex()].name, (unsigned)type, (unsigned)options, proc_selfpid(), who,
		    fLast ? "last sample" : "none yet");
	}
	if (fLast && (type == gSweetSensorInfo[sensorIndex()].eventType || type == kIOHIDEventTypeNULL)) {
		event = fLast;
		event->retain();
	}
	IOLockUnlock(fLock);
	if (!event && (sensorIndex() == SWEET_SENSOR_ALS || sensorIndex() == SWEET_SENSOR_PROX ||
	    sensorIndex() == SWEET_SENSOR_ORIENT ||
	    (sweet_motion_mask() & SWEET_MOTION_INITIAL_EVENTS)) &&
	    (type == gSweetSensorInfo[sensorIndex()].eventType || type == kIOHIDEventTypeNULL)) {
		event = initialEvent();         /* a nil copyEvent may be the NSNull */
	}
	return event;
}

/*
 * A neutral sample until a real (or poked) one exists: upright portrait, still, 100 lux, far, sea level,
 * portrait (Android-frame float bits, as sensd would write them).
 */
IOHIDEvent *
SweetHIDSensor::initialEvent()
{
	static const uint32_t v[SWEET_SENSOR_COUNT][4] = {
		{ 0, 0x411ce80a /* 9.80665 */, 0, 0 }, { 0, 0, 0, 0 }, { 0x42c80000 /* 100 */, 0, 0, 0 }, { 0, 0, 0, 0 },
		{ 0x41a00000 /* 20 */, 0, 0xc2200000 /* -40 */, 0 }, { 0x42caa666 /* 101.325 */, 0, 0, 0 },
		{ 0x3f800000 /* 1 */, 0, 0, 0 },
	};
	int i = sensorIndex();
	struct sweet_motion_sample s = { gSweetSensorInfo[i].motionType, 0, 0, { v[i][0], v[i][1], v[i][2], v[i][3] } };
	IOHIDEvent *event = NULL;
	(void)post(&s, &event);
	return event;
}

void
SweetHIDSensor::initialFired(OSObject *owner, IOTimerEventSource *sender __unused)
{
	SweetHIDSensor *self = (SweetHIDSensor *)owner;
	if (self->fEvents == 0) {
		struct sweet_motion_sample s = { 12, 3, 0, { 0x42c80000 /* 100.0f */, 0, 0, 0 } };
		IOLog("SWEET MOTION: als: initial 100 lux event (%u clients)\n", self->fClients);
		self->post(&s);
		self->keepAliveArm();   /* 1099 */
	} else if (self->sensorIndex() == SWEET_SENSOR_ALS && (sweet_bl_fix & 4) && self->fReal == 0 &&
	    self->fClients != 0 && self->fIntervalSet && self->fInterval != 0) {
		/* 1099: keep-alive until sensd */
		struct sweet_motion_sample s = { 12, 3, 0, { 0x42c80000 /* 100.0f */, 0, 0, 0 } };
		uint64_t k = ++self->fKeep;
		if (k <= 3 || (k & (k - 1)) == 0) {
			IOLog("SWEET MOTION: als keep-alive #%llu: 100 lux every %u us until sensd (1099, kern.sweet_bl_fix & 4)\n",
			    k, self->fInterval);
		}
		self->post(&s);
		self->keepAliveArm();
	}
}

/* User-space reads of the service's properties (IORegistryEntryCreateCFProperty), each key once. */
static struct {
	const void *svc;
	uint32_t hash;
} gSweetReads[256];
static uint32_t gSweetReadCount;

void
SweetHIDSensor::logRead(const char *key, bool found) const
{
	int pid = proc_selfpid();
	if (pid <= 0 || !gSweetReadLock) {
		return;         /* the kernel's own reads (matching, the user client) */
	}
	uint32_t h = 2166136261u;
	for (const char *p = key; *p; p++) {
		h = (h ^ (uint8_t)*p) * 16777619u;
	}
	h ^= found ? 1 : 0;
	IOLockLock(gSweetReadLock);
	bool seen = false;
	for (uint32_t i = 0; i < gSweetReadCount; i++) {
		if (gSweetReads[i].svc == this && gSweetReads[i].hash == h) {
			seen = true;
			break;
		}
	}
	if (!seen && gSweetReadCount < 256) {
		gSweetReads[gSweetReadCount].svc = this;
		gSweetReads[gSweetReadCount].hash = h;
		gSweetReadCount++;
	}
	IOLockUnlock(gSweetReadLock);
	if (!seen && gSweetReadCount < 256) {
		char who[32] = "?";
		proc_selfname(who, sizeof(who));
		IOLog("SWEET MOTION: %s property read %s: %s (pid %d %s)\n",
		    gSweetSensorInfo[const_cast<SweetHIDSensor *>(this)->sensorIndex()].name, key,
		    found ? "present" : "ABSENT", pid, who);
	}
}

OSPtr<OSObject>
SweetHIDSensor::copyProperty(const char *aKey) const
{
	OSPtr<OSObject> obj = IOHIDEventService::copyProperty(aKey);
	if (aKey) {
		logRead(aKey, obj != NULL);
	}
	return obj;
}

bool
SweetHIDSensor::serializeProperties(OSSerialize *s) const
{
	logRead("(all properties)", true);
	return IOHIDEventService::serializeProperties(s);
}

void
SweetHIDSensor::relog()
{
	IOLockLock(fLock);
	fSeen->flushCollection();
	fLogged = 0;
	fCopyTypes = 0;
	IOLockUnlock(fLock);
}

static int
sweetMilli(int64_t q16)
{
	return (int)((q16 * 1000) / 65536);
}

/* Kernel orientation filter (knob orientauto): UIDeviceOrientation from an Android-frame accel sample. */
static int gSweetOrientCur, gSweetOrientCand;
static uint64_t gSweetOrientCandTs;

static int
sweetOrientationOf(int64_t x, int64_t y, int64_t z)  /* 16.16 m/s² */
{
	int64_t ax = x < 0 ? -x : x, ay = y < 0 ? -y : y, az = z < 0 ? -z : z;
	int64_t m = ax > ay ? ax : ay;
	if (ax + ay < 3 * 65536 || az * 10 > 16 * m) {
		return 0;
	}
	if (ay * 10 > 13 * ax) {
		return y > 0 ? 1 : 2;
	}
	if (ax * 10 > 13 * ay) {
		return x > 0 ? 3 : 4;
	}
	return 0;
}

int
SweetHIDSensor::post(const struct sweet_motion_sample *s, IOHIDEvent **built)
{
	int i = sensorIndex();
	uint64_t now = mach_absolute_time(), ts = s->ts_mach, sec;
	IOHIDEvent *event = NULL;
	char line[160];
	bool synthetic = (s->flags & SWEET_MOTION_SYNTHETIC) != 0;

	nanoseconds_to_absolutetime(NSEC_PER_SEC, &sec);
	if (ts == 0 || ts > now || now - ts > sec) {
		ts = now;
	}
	line[0] = 0;
	switch (i) {
	case SWEET_SENSOR_ACCEL: {
		/* Android m/s² -> g, sign flipped (knob accsign) */
		int64_t num = 100000 * gSweetKnobs.accsign;
		IOFixed x = (IOFixed)sweet_f2q(s->v[0], num, 980665, 16);
		IOFixed y = (IOFixed)sweet_f2q(s->v[1], num, 980665, 16);
		IOFixed z = (IOFixed)sweet_f2q(s->v[2], num, 980665, 16);
		event = IOHIDEvent::accelerometerEvent(ts, x, y, z);
		snprintf(line, sizeof(line), "x %d y %d z %d mg", sweetMilli(x), sweetMilli(y), sweetMilli(z));
		if (!built && gSweetKnobs.orientauto && gSweetSensors[SWEET_SENSOR_ORIENT]) {
			int o = sweetOrientationOf(sweet_f2q(s->v[0], 1, 1, 16), sweet_f2q(s->v[1], 1, 1, 16),
			    sweet_f2q(s->v[2], 1, 1, 16));
			uint64_t settle;
			nanoseconds_to_absolutetime(300 * NSEC_PER_MSEC, &settle);
			if (o && o != gSweetOrientCand) {
				gSweetOrientCand = o;
				gSweetOrientCandTs = ts;
			}
			if (o && o != gSweetOrientCur && ts - gSweetOrientCandTs >= settle) {
				struct sweet_motion_sample os = { 10, s->flags, ts, { 0, 0, 0, 0 } };
				static const uint32_t f32[7] = { 0, 0x3f800000, 0x40000000, 0x40400000, 0x40800000, 0x40a00000, 0x40c00000 };
				gSweetOrientCur = o;
				os.v[0] = f32[o];
				gSweetSensors[SWEET_SENSOR_ORIENT]->post(&os);
			}
		}
		break;
	}
	case SWEET_SENSOR_GYRO: {
		int64_t num = gSweetKnobs.gyrodeg ? 5729578 / 10 : 1, den = gSweetKnobs.gyrodeg ? 10000 : 1;
		IOFixed x = (IOFixed)sweet_f2q(s->v[0], num, den, 16);
		IOFixed y = (IOFixed)sweet_f2q(s->v[1], num, den, 16);
		IOFixed z = (IOFixed)sweet_f2q(s->v[2], num, den, 16);
		event = IOHIDEvent::gyroEvent(ts, x, y, z);
		snprintf(line, sizeof(line), "x %d y %d z %d m%s", sweetMilli(x), sweetMilli(y), sweetMilli(z),
		    gSweetKnobs.gyrodeg ? "deg/s" : "rad/s");
		break;
	}
	case SWEET_SENSOR_COMPASS: {
		IOFixed x = (IOFixed)sweet_f2q(s->v[0], 1, 1, 16);
		IOFixed y = (IOFixed)sweet_f2q(s->v[1], 1, 1, 16);
		IOFixed z = (IOFixed)sweet_f2q(s->v[2], 1, 1, 16);
		event = IOHIDEvent::compassEvent(ts, x, y, z);
		snprintf(line, sizeof(line), "x %d y %d z %d nT", sweetMilli(x), sweetMilli(y), sweetMilli(z));
		break;
	}
	case SWEET_SENSOR_ALS: {
		int64_t lux = sweet_f2q(s->v[0], 1, 1, 0), cct = sweet_f2q(s->v[1], 1, 1, 0);
		lux = lux < 0 ? 0 : lux;
		event = IOHIDEvent::ambientLightSensorEvent(ts, (UInt32)lux, (UInt32)lux);
		if (event) {
			event->setIntegerValue(kIOHIDEventFieldAmbientLightSensorIlluminance, (SInt32)lux);
			if (cct > 0) {
				event->setIntegerValue(kIOHIDEventFieldAmbientLightSensorColorTemperature, (SInt32)cct);
			}
		}
		snprintf(line, sizeof(line), "%lld lux, %lld K", lux, cct);
		break;
	}
	case SWEET_SENSOR_PROX: {
		bool near = (s->v[0] & 0x7fffffff) != 0;
		int64_t raw = sweet_f2q(s->v[1], 1, 1, 0);
		event = IOHIDEvent::proximityEvent(ts, near ? gSweetKnobs.proxmask : 0, (UInt32)(raw < 0 ? 0 : raw));
		snprintf(line, sizeof(line), "%s (mask 0x%x), raw %lld", near ? "near" : "far",
		    near ? gSweetKnobs.proxmask : 0, raw);
		break;
	}
	case SWEET_SENSOR_PRESSURE: {
		IOFixed p = (IOFixed)sweet_f2q(s->v[0], 1, 1, 16);
		event = IOHIDEvent::atmosphericPressureEvent(ts, p, (UInt32)fEvents);
		snprintf(line, sizeof(line), "%d Pa", sweetMilli(p));
		break;
	}
	case SWEET_SENSOR_ORIENT: {
		int64_t o = sweet_f2q(s->v[0], 1, 1, 0);
		event = IOHIDEvent::orientationEvent(ts, kIOHIDOrientationTypeCMUsage);
		if (event) {
			event->setIntegerValue(kIOHIDEventFieldOrientationDeviceOrientationUsage,
			    (SInt32)(gSweetKnobs.orientbase + o));
			/* 1031: CoreMotion (sensorTimeFromHidChildEvent) wants child 0 = vendor event
			 * {0xff15, 2} with an 8-byte sensor time; without it every sample is "stale". */
			uint64_t st = ts;
			IOHIDEvent *child = IOHIDEvent::vendorDefinedEvent(ts, 0xff15, 2, 0, (UInt8 *)&st,
			    (UInt32)sizeof(st));
			if (child) {
				event->appendChild(child);      /* the children array retains it */
				child->release();
			}
		}
		snprintf(line, sizeof(line), "UIDeviceOrientation %lld (usage %lld)", o, gSweetKnobs.orientbase + o);
		break;
	}
	}
	if (!event) {
		return ENOMEM;
	}
	if (built) {            /* initialEvent(): build only, no dispatch */
		*built = event;
		return 0;
	}
	uint64_t n = OSIncrementAtomic64((volatile SInt64 *)&fEvents) + 1;
	if (synthetic || n <= 3 || (gSweetKnobs.every && n % gSweetKnobs.every == 0)) {
		IOLog("SWEET MOTION: %s #%llu%s: %s (%u clients)\n", gSweetSensorInfo[i].name, n,
		    synthetic ? " (fake)" : "", line, fClients);
	}
	dispatchEvent(event);
	IOLockLock(fLock);
	IOHIDEvent *old = fLast;
	fLast = event;          /* keeps the reference */
	IOLockUnlock(fLock);
	OSSafeReleaseNULL(old);
	return 0;
}

int
SweetHIDSensor::status(char *buf, int len)
{
	const SweetSensorInfo *info = &gSweetSensorInfo[sensorIndex()];
	return snprintf(buf, len, "%-11s page 0x%04x usage 0x%02x clients %u ReportInterval %u us%s events %llu%s\n",
	    info->name, (unsigned)info->page, (unsigned)info->usage, fClients, fInterval,
	    fIntervalSet ? " (set by iOS)" : " (default)", fEvents, fLast ? "" : " (none yet)");
}

void
SweetHIDSensor::want(struct sweet_motion_want *w)
{
	int i = sensorIndex();
	w->present |= 1u << i;
	w->clients[i] = fClients;
	w->interval_us[i] = fInterval;
	w->interval_set[i] = fIntervalSet;
}

extern "C" int
sweet_hid_sensor(const struct sweet_motion_sample *s)
{
	int i = sweet_motion_index(s->type);
	if (i < 0) {
		return EINVAL;
	}
	if (!gSweetSensors[i]) {
		return ENODEV;
	}
	gSweetSensors[i]->noteReal();   /* 1099: ends the ALS keep-alive */
	return gSweetSensors[i]->post(s);
}

extern "C" int
sweet_hid_sensor_status(char *buf, int len)
{
	int at = 0;
	for (int i = 0; i < SWEET_SENSOR_COUNT && at < len; i++) {
		if (gSweetSensors[i]) {
			at += gSweetSensors[i]->status(buf + at, len - at);
		} else {
			at += snprintf(buf + at, len - at, "%-11s not registered (sweetmotion=0x%x)\n",
			    gSweetSensorInfo[i].name, sweet_motion_mask());
		}
	}
	return at < len ? at : len - 1;
}

extern "C" void
sweet_hid_sensor_want(struct sweet_motion_want *w)
{
	bzero(w, sizeof(*w));
	w->version = 1;
	for (int i = 0; i < SWEET_SENSOR_COUNT; i++) {
		if (gSweetSensors[i]) {
			gSweetSensors[i]->want(w);
		}
	}
}

extern "C" void
sweet_hid_sensor_relog(void)
{
	for (int i = 0; i < SWEET_SENSOR_COUNT; i++) {
		if (gSweetSensors[i]) {
			gSweetSensors[i]->relog();
		}
	}
}

extern "C" int
sweet_hid_sensor_knob(const char *name, int64_t value)
{
	if (!strcmp(name, "accsign")) {
		gSweetKnobs.accsign = value < 0 ? -1 : 1;
	} else if (!strcmp(name, "gyrodeg")) {
		gSweetKnobs.gyrodeg = value != 0;
	} else if (!strcmp(name, "proxmask")) {
		gSweetKnobs.proxmask = (uint32_t)value;
	} else if (!strcmp(name, "orientbase")) {
		gSweetKnobs.orientbase = (int)value;
	} else if (!strcmp(name, "orientauto")) {
		gSweetKnobs.orientauto = value != 0;
		gSweetOrientCur = gSweetOrientCand = 0;
	} else if (!strcmp(name, "logmax")) {
		gSweetKnobs.logmax = (uint32_t)value;
	} else if (!strcmp(name, "every")) {
		gSweetKnobs.every = (uint32_t)value;
	} else {
		return -1;
	}
	return 0;
}

extern "C" void
sweet_hid_sensor_knobs_print(void)
{
	IOLog("SWEET MOTION: knobs accsign %d gyrodeg %d proxmask 0x%x orientbase %d orientauto %d logmax %u every %u\n",
	    gSweetKnobs.accsign, gSweetKnobs.gyrodeg, gSweetKnobs.proxmask, gSweetKnobs.orientbase,
	    gSweetKnobs.orientauto, gSweetKnobs.logmax, gSweetKnobs.every);
}
