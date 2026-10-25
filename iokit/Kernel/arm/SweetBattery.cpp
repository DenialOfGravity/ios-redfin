/*
 * Built-in iOS battery bring-up service. Default: 100%, AC, not charging.
 * sweetbattery=1: read-only PM7250B polling; SOC is our own estimate, not
 * the Linux QG algorithm.
 * See report.md for binary evidence, register provenance and integration.
 *
 * redfin (0966): every 5 s; capacity 4000 mAh (redfin's cell, DT
 * google,chg-battery-capacity); SOC seeded from the QG SDAM (0xb146 valid,
 * 0xb147 the last SOC Android's QG stored) when it is within 15 % of the
 * voltage estimate, else from the voltage estimate, then coulomb-counted from
 * the QG current (last sample 0x48c2; the burst average 0x48c6 is logged) with a slow
 * pull toward the voltage estimate while the current is small; the shown %
 * still moves at most 1 % per poll. IsCharging = measured current > +50 mA.
 * Temperature: the JEITA zone of CHGR_STATUS_7 (0x100d) instead of a fixed
 * 25 C (normal 25, soft cold 10 / hot 45, hard cold 0 / hot 50).
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <device/device_types.h> /* io_buf_ptr_t for private OSKext headers */
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
/* redfin: QG fuel gauge (0x4800) and charger (0x1000) are on PM7250B, SID 2
 * (pm7250b.dtsi qcom,qgauge@4800); sunfish had them on PM6150 SID 0. */
#define BATT_SID 2

/* redfin (0966): 4000 mAh cell, poll every 5 s */
static const unsigned kCapacityMah = 4000;
static const unsigned kPollMs = 5000;

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
	int fShown;		/* sunfish: smoothed % shown to iOS, -1 until the first reading */
	unsigned fEmaMv, fPolls;
	int64_t fCharge;	/* 0966: coulomb count, uAh; -1 until seeded */
	uint64_t fLastAbs;	/* time of the last counted sample */
	unsigned fTempC10;	/* 0966: published temperature, centi-degC */
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
	fShown = -1;
	fEmaMv = 0;
	fPolls = 0;
	fCharge = -1;
	fLastAbs = 0;
	fTempC10 = 2500;
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
	/* Use consistent mAh units (redfin 4000, 0966). powerd computes C*100/M. */
	const unsigned capacity = (kCapacityMah * percent + 50u) / 100u;
	setBatteryInstalled(true);
	setCurrentCapacity(capacity);
	setMaxCapacity(kCapacityMah);
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
	putNumber("AppleRawMaxCapacity", kCapacityMah);
	putNumber("DesignCapacity", kCapacityMah);
	putNumber("InstantAmperage", (UInt32)ma);
	/* iOS battery convention: centidegrees C; 0966: from the charger's JEITA zone, not a thermistor reading. */
	putNumber("Temperature", fTempC10);
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
		UInt32 values[] = {capacity, kCapacityMah, mv, (UInt32)ma, 0,
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
		    measured ? "PM7250B / counted SOC" : "static fallback",
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
		if (sweet_spmi_read(BATT_SID, address, &lo) ||
		    sweet_spmi_read(BATT_SID, address + 1, &hi)) {
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
	    !sweet_spmi_read(BATT_SID, 0x4804, &type) && type == 0x0d &&
	    !sweet_spmi_read(BATT_SID, 0x4808, &present) && (present & 1u) &&
	    readWord(0x48c0, &rawVoltage) && rawVoltage != 0x8000 &&
	    !sweet_spmi_read(BATT_SID, 0x1310, &usb) &&
	    !sweet_spmi_read(BATT_SID, 0x110b, &path) &&
	    !sweet_spmi_read(BATT_SID, 0x1006, &charger) &&
	    !sweet_spmi_read(BATT_SID, 0x1007, &error) &&
	    !sweet_spmi_read(BATT_SID, 0x100d, &temperature);
	unsigned mv = valid ? (unsigned)(((uint64_t)rawVoltage * 194637u) / 1000000u) : 0;
	/* Zero, reset, floating, or implausible data cannot become 0% battery. */
	valid = valid && mv >= 2800 && mv <= 4600;
	if (!valid) {
		/* sunfish: one bad read used to flash 100%; keep the last value. */
		if (fShown >= 0) {
			return;
		}
		publish(100, 4200, 0, true, true, false, true, false, false);
		return;
	}
	bool external = (usb & 0x10u) != 0;
	bool capable = external && (path & 0x10u) && (path & 1u) && !(path & 0x40u);
	unsigned state = charger & 7u;
	bool hwCharging = capable && state >= 1 && state <= 4 &&
	    !(error & 2u) && !(temperature & 0x0cu);
	/* Inhibit (0) does not by itself establish full capacity. */
	bool full = external && state == 5 && !(error & 2u);
	/* JEITA zone (smb5-reg.h BAT_TEMP_STATUS_*): hard 3:2, soft 5:4 */
	fTempC10 = (temperature & 0x08u) ? 5000 : (temperature & 0x04u) ? 0 :
	    (temperature & 0x20u) ? 4500 : (temperature & 0x10u) ? 1000 : 2500;
	int ma = 0;
	uint8_t subtype;
	uint16_t rawCurrent;
	bool currentValid = !sweet_spmi_read(BATT_SID, 0x4805, &subtype) &&
	    (subtype == 3 || subtype == 4) && readWord(0x48c2, &rawCurrent) && rawCurrent != 0x8000;
	if (currentValid) {
		int signedRaw = (rawCurrent & 0x8000u) ? (int)rawCurrent - 65536 : (int)rawCurrent;
		/* Linux: negative=charging; IOPMPowerSource: positive=charging, mA. */
		ma = (int)(-((int64_t)signedRaw * (subtype == 3 ? 152588 : 305176)) / 1000000);
	}
	/* 0966: the bolt only when the battery really gains charge (a weak port can drain it while "charging"). */
	bool charging = currentValid ? (external && ma > 50) : hwCharging;

	/* Voltage estimate: IR-corrected (~150 mOhm), averaged; used to seed and to correct the count. */
	unsigned ocv = mv;
	if (currentValid) {
		int adj = -(ma * 150) / 1000;	/* ma > 0 charging: the terminal reads high */
		ocv = (unsigned)((int)mv + adj);
	}
	fEmaMv = fEmaMv ? (fEmaMv * 3u + ocv) / 4u : ocv;
	int estimate = full ? 100 : (int)estimatePercent(fEmaMv);
	const int64_t cap = (int64_t)kCapacityMah * 1000;
	uint64_t now = mach_absolute_time();
	if (fCharge < 0) {
		uint8_t sdamValid = 0, sdamSoc = 0xff;
		bool sdam = !sweet_spmi_read(BATT_SID, 0xb146, &sdamValid) &&
		    !sweet_spmi_read(BATT_SID, 0xb147, &sdamSoc) && sdamValid == 1 && sdamSoc <= 100;
		int diff = sdam ? (int)sdamSoc - estimate : 0;
		bool useSdam = sdam && diff <= 15 && diff >= -15;
		int seed = useSdam ? (int)sdamSoc : estimate;
		fCharge = cap * seed / 100;
		IOLog("SWEET BATTERY: SOC seeded %d%% from %s (SDAM valid %u soc %u, voltage estimate %d%% at %u mV)\n",
		    seed, useSdam ? "SDAM" : "voltage", sdamValid, sdamSoc, estimate, fEmaMv);
	} else if (currentValid) {
		uint64_t ns = 0;
		absolutetime_to_nanoseconds(now - fLastAbs, &ns);
		uint64_t ms = ns / 1000000;
		if (ms > 4 * kPollMs) {
			ms = 4 * kPollMs;	/* a stalled poll must not integrate a stale current */
		}
		fCharge += (int64_t)ma * (int64_t)ms / 3600;	/* mA * ms -> uAh */
	}
	fLastAbs = now;
	int counted = (int)((fCharge * 100 + cap / 2) / cap);
	/* Pull toward the voltage estimate at rest only (no load/charge sag): 0.2 % per poll. */
	if (full) {
		fCharge = cap;
	} else if (currentValid && ma > -100 && ma < 100 && (counted - estimate > 10 || estimate - counted > 10)) {
		fCharge += (estimate > counted ? 1 : -1) * cap / 500;
	}
	if (fCharge < 0) {
		fCharge = 0;
	} else if (fCharge > cap) {
		fCharge = cap;
	}
	int target = (int)((fCharge * 100 + cap / 2) / cap);
	if (fShown < 0) {
		fShown = target;
	} else {
		/* Direction from the measured current; at rest either way toward the count. */
		bool gaining = full || (currentValid ? ma > -50 : charging);
		bool losing = currentValid ? ma < 50 : !external;
		if (target > fShown && gaining) {
			fShown++;
		} else if (target < fShown && losing) {
			fShown--;
		}
	}
	if ((fPolls++ % 12u) == 0) {
		/* the QG burst average (0x48c6) only logged, for comparison with the last sample we count */
		uint16_t rawBurst = 0;
		int burst = 0;
		if (currentValid && readWord(0x48c6, &rawBurst)) {
			int b = (rawBurst & 0x8000u) ? (int)rawBurst - 65536 : (int)rawBurst;
			burst = (int)(-((int64_t)b * (subtype == 3 ? 152588 : 305176)) / 1000000);
		}
		IOLog("SWEET BATTERY: %umV %dmA (burst %d) ocv %u est %d%% count %d%% (%lld mAh) shown %d%% ext=%d chg=%d "
		    "hw=%u temp %u\n", mv, ma, burst, fEmaMv, estimate, target, fCharge / 1000, fShown, external, charging,
		    state, fTempC10);
	}
	publish((unsigned)fShown, mv, ma, external, capable,
	    charging, full, true, currentValid);
}

void
SweetBattery::timerFired(OSObject *owner, IOTimerEventSource *sender)
{
	SweetBattery *self = OSDynamicCast(SweetBattery, owner);
	if (self && self->fRunning) {
		self->poll();
		sender->setTimeoutMS(kPollMs);
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
			fTimer->setTimeoutMS(kPollMs);
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
