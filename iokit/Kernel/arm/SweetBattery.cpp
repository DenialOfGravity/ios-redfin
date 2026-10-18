/*
 * Built-in iOS battery bring-up service. Default: 100%, AC, not charging.
 * sweetbattery=1: read-only PM6150 polling every 30 seconds; voltage-derived
 * SOC is an UNCALIBRATED estimate, not a replacement for the Linux QG algo.
 * See report.md for binary evidence, register provenance and integration.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <device/device_types.h> /* io_buf_ptr_t for private OSKext headers */
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <pexpert/pexpert.h>
/* bsd/dev/sweet_spmi.c */
extern "C" int sweet_spmi_init(void);
extern "C" int sweet_spmi_read(unsigned sid, unsigned addr, uint8_t *out);

class SweetBattery : public IOPMPowerSource {
	OSDeclareDefaultStructors(SweetBattery);
public:
	bool init(OSDictionary *dictionary = NULL) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
	void free(void) APPLE_KEXT_OVERRIDE;
private:
	IOWorkLoop *fLoop;
	IOTimerEventSource *fTimer;
	bool fTimerAdded, fRunning, fReal;
	int fLastMode;
	void putNumber(const char *key, UInt32 value);
	void putBool(const char *key, bool value);
	void publish(unsigned percent, unsigned mv, int ma, bool external,
	    bool capable, bool charging, bool full, bool measured, bool currentValid);
	void poll(void);
	static void timerFired(OSObject *owner, IOTimerEventSource *sender);
	static IOReturn stopPolling(OSObject *owner, void *, void *, void *, void *);
	static bool readWord(unsigned address, uint16_t *value);
	static unsigned estimatePercent(unsigned mv);
};
OSDefineMetaClassAndStructors(SweetBattery, IOPMPowerSource);

bool
SweetBattery::init(OSDictionary *dictionary)
{
	/* IOCatalogue calls init(dictionary). This XNU's IOPMPowerSource only
	 * overrides init(void): explicitly run it to allocate its PS dictionary
	 * and symbols, then copy the matching personality into the registry. */
	if (!IOPMPowerSource::init()) {
		return false;
	}
	fLoop = NULL;
	fTimer = NULL;
	fTimerAdded = fRunning = fReal = false;
	if (dictionary) {
		OSCollectionIterator *it = OSCollectionIterator::withCollection(dictionary);
		if (!it) {
			return false;
		}
		OSObject *object;
		bool ok = true;
		while ((object = it->getNextObject())) {
			const OSSymbol *key = OSDynamicCast(OSSymbol, object);
			if (!key || !setProperty(key, dictionary->getObject(key))) {
				ok = false;
				break;
			}
		}
		it->release();
		if (!ok) {
			return false;
		}
	}
	fLastMode = -1;
	return true;
}

void
SweetBattery::putNumber(const char *key, UInt32 value)
{
	const OSSymbol *symbol = OSSymbol::withCString(key);
	OSNumber *number = OSNumber::withNumber(value, 32);
	if (symbol && number) {
		setPSProperty(symbol, number);
	}
	OSSafeReleaseNULL(number);
	OSSafeReleaseNULL(symbol);
}

void
SweetBattery::putBool(const char *key, bool value)
{
	const OSSymbol *symbol = OSSymbol::withCString(key);
	if (symbol) {
		setPSProperty(symbol, value ? kOSBooleanTrue : kOSBooleanFalse);
		symbol->release();
	}
}

void
SweetBattery::publish(unsigned percent, unsigned mv, int ma, bool external,
    bool capable, bool charging, bool full, bool measured, bool currentValid)
{
	/* Use consistent mAh units (DT nominal=5020). powerd computes C*100/M. */
	const unsigned capacity = (5020u * percent + 50u) / 100u;
	setBatteryInstalled(true);
	setCurrentCapacity(capacity);
	setMaxCapacity(5020);
	setExternalConnected(external);
	setExternalChargeCapable(capable);
	setIsCharging(charging);
	setAtWarnLevel(false);
	/* An uncalibrated voltage curve must not assert a critical shutdown. */
	setAtCriticalLevel(false);
	setVoltage(mv);
	setAmperage(ma);
	setTimeRemaining(-1); /* Unknown; never publish zero minutes on error. */
	setCycleCount(0);     /* Placeholder: cycle learning is not implemented. */
	putBool("FullyCharged", full);
	putBool("AppleRawExternalConnected", external);
	putNumber("AppleRawCurrentCapacity", capacity);
	putNumber("AppleRawMaxCapacity", 5020);
	putNumber("DesignCapacity", 5020);
	putNumber("InstantAmperage", (UInt32)ma);
	/* iOS battery convention: centidegrees C; fixed 25 C, NOT measured. */
	putNumber("Temperature", 2500);
	putBool("SweetBatteryHardwareSampleValid", measured);
	putBool("SweetBatteryCurrentValid", currentValid);
	putBool("SweetBatterySOCEstimated", measured);
	putBool("SweetBatteryTemperatureEstimated", true);
	putBool("SweetBatteryFallback", !measured);
	putNumber("SweetBatteryPercent", percent);

	/* IOPMCopyBatteryInfo's per-source compatibility dictionary. */
	OSDictionary *legacy = OSDictionary::withCapacity(7);
	if (legacy) {
		const char *keys[] = {kIOBatteryCurrentChargeKey, kIOBatteryCapacityKey,
		    kIOBatteryVoltageKey, kIOBatteryAmperageKey, kIOBatteryCycleCountKey,
		    kIOBatteryFlagsKey};
		UInt32 values[] = {capacity, 5020, mv, (UInt32)ma, 0,
		    (UInt32)(kIOBatteryInstalled | (external ? kIOBatteryChargerConnect : 0) |
		    (charging ? kIOBatteryCharge : 0))};
		for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
			OSNumber *number = OSNumber::withNumber(values[i], 32);
			if (number) {
				legacy->setObject(keys[i], number);
				number->release();
			}
		}
		setLegacyIOBatteryInfo(legacy);
		legacy->release();
	}
	updateStatus(); /* Flush PS properties and send 0xe0024100 on change. */
	int mode = measured ? 1 : 0;
	if (mode != fLastMode) {
		IOLog("SWEET BATTERY: %s, %u%% %umV external=%d charging=%d\n",
		    measured ? "PM6150 / estimated SOC" : "static fallback",
		    percent, mv, external, charging);
		fLastMode = mode;
	}
}

/* Keep the proven single-byte observer command. Read two matching whole
 * words to reject most torn ADC samples without writing the QG HOLD bit.
 * This is a bounded consistency check, not a hardware snapshot guarantee. */
bool
SweetBattery::readWord(unsigned address, uint16_t *value)
{
	uint16_t previous = 0;
	for (unsigned attempt = 0; attempt < 4; ++attempt) {
		uint8_t lo, hi;
		if (sweet_spmi_read(0, address, &lo) ||
		    sweet_spmi_read(0, address + 1, &hi)) {
			return false;
		}
		uint16_t next = (uint16_t)((unsigned)lo | ((unsigned)hi << 8));
		if (attempt && next == previous) {
			*value = next;
			return true;
		}
		previous = next;
	}
	return false;
}

unsigned
SweetBattery::estimatePercent(unsigned mv)
{
	/* Generic single-cell curve, intentionally NOT advertised as QG SOC.
	 * Load sag, temperature, aging and charging introduce large errors. */
	static const unsigned volts[] = {3400, 3500, 3600, 3700, 3800, 3900,
	    4000, 4100, 4200, 4350};
	static const unsigned soc[] = {1, 5, 10, 20, 40, 60, 75, 85, 93, 100};
	if (mv <= volts[0]) {
		return soc[0];
	}
	for (unsigned i = 1; i < sizeof(volts) / sizeof(volts[0]); ++i) {
		if (mv < volts[i]) {
			return soc[i - 1] + (mv - volts[i - 1]) * (soc[i] - soc[i - 1]) /
			       (volts[i] - volts[i - 1]);
		}
	}
	return 100;
}

void
SweetBattery::poll(void)
{
	uint8_t type, present, usb, path, charger, error, temperature;
	uint16_t rawVoltage;
	bool valid = fReal &&
	    !sweet_spmi_read(0, 0x4804, &type) && type == 0x0d &&
	    !sweet_spmi_read(0, 0x4808, &present) && (present & 1u) &&
	    readWord(0x48c0, &rawVoltage) && rawVoltage != 0x8000 &&
	    !sweet_spmi_read(0, 0x1310, &usb) &&
	    !sweet_spmi_read(0, 0x110b, &path) &&
	    !sweet_spmi_read(0, 0x1006, &charger) &&
	    !sweet_spmi_read(0, 0x1007, &error) &&
	    !sweet_spmi_read(0, 0x100d, &temperature);
	unsigned mv = valid ? (unsigned)(((uint64_t)rawVoltage * 194637u) / 1000000u) : 0;
	/* Zero, reset, floating, or implausible data cannot become 0% battery. */
	valid = valid && mv >= 2800 && mv <= 4600;
	if (!valid) {
		publish(100, 4200, 0, true, true, false, true, false, false);
		return;
	}
	bool external = (usb & 0x10u) != 0;
	bool capable = external && (path & 0x10u) && (path & 1u) && !(path & 0x40u);
	unsigned state = charger & 7u;
	bool charging = capable && state >= 1 && state <= 4 &&
	    !(error & 2u) && !(temperature & 0x0cu);
	/* Inhibit (0) does not by itself establish full capacity. */
	bool full = external && state == 5 && !(error & 2u);
	int ma = 0;
	uint8_t subtype;
	uint16_t rawCurrent;
	bool currentValid = !sweet_spmi_read(0, 0x4805, &subtype) &&
	    (subtype == 3 || subtype == 4) && readWord(0x48c2, &rawCurrent) &&
	    rawCurrent != 0x8000;
	if (currentValid) {
		int signedRaw = (rawCurrent & 0x8000u) ? (int)rawCurrent - 65536 : (int)rawCurrent;
		/* Linux: negative=charging; IOPMPowerSource: positive=charging, mA. */
		ma = (int)(-((int64_t)signedRaw * (subtype == 3 ? 152588 : 305176)) / 1000000);
	}
	publish(full ? 100 : estimatePercent(mv), mv, ma, external, capable,
	    charging, full, true, currentValid);
}

void
SweetBattery::timerFired(OSObject *owner, IOTimerEventSource *sender)
{
	SweetBattery *self = OSDynamicCast(SweetBattery, owner);
	if (self && self->fRunning) {
		self->poll();
		sender->setTimeoutMS(30000);
	}
}

bool
SweetBattery::start(IOService *provider)
{
	if (!IOPMPowerSource::start(provider)) {
		return false;
	}
	int real = 0;
	(void)PE_parse_boot_argn("sweetbattery", &real, sizeof(real));
	fReal = real == 1 && sweet_spmi_init() == 0;
	/* Publish a complete source BEFORE registerService wakes powerd. */
	poll();
	if (fReal) {
		fLoop = IOWorkLoop::workLoop();
		fTimer = IOTimerEventSource::timerEventSource(this, &SweetBattery::timerFired);
		if (fLoop && fTimer && fLoop->addEventSource(fTimer) == kIOReturnSuccess) {
			fTimerAdded = true;
			fRunning = true;
			fTimer->setTimeoutMS(30000);
		} else {
			fReal = false;
			poll();
			IOLog("SWEET BATTERY: timer unavailable; static fallback\n");
		}
	}
	registerService();
	return true;
}

void
SweetBattery::stop(IOService *provider)
{
	if (fLoop && fTimerAdded) {
		/* Serialize against a running timer callback, including rearming. */
		(void)fLoop->runAction(&SweetBattery::stopPolling, this);
		fLoop->removeEventSource(fTimer);
		fTimerAdded = false;
	}
	IOPMPowerSource::stop(provider);
}

IOReturn
SweetBattery::stopPolling(OSObject *owner, void *, void *, void *, void *)
{
	SweetBattery *self = OSDynamicCast(SweetBattery, owner);
	if (!self) {
		return kIOReturnBadArgument;
	}
	self->fRunning = false;
	self->fTimer->cancelTimeout();
	self->fTimer->disable();
	return kIOReturnSuccess;
}

void
SweetBattery::free(void)
{
	OSSafeReleaseNULL(fTimer);
	OSSafeReleaseNULL(fLoop);
	IOPMPowerSource::free();
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
