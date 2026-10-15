/*
 * Copyright (c) 2021 Apple Inc. All rights reserved.
 */

#include <stdint.h>
#include <libkern/section_keywords.h>
#include <arm64/proc_reg.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <machine/atomic.h>
#include <machine/machine_routines.h>
#include <pexpert/device_tree.h>
#include <pexpert/arm64/board_config.h>


#if HAS_GIC_V3
#define GICR_WAKE_TIMEOUT_NS (1000000000ULL) // timeout for redistributor wakeup (default 1s)
MACHINE_TIMEOUT(gicr_wake_timeout_ns, "gicr-wake-timeout", GICR_WAKE_TIMEOUT_NS, MACHINE_TIMEOUT_UNIT_NSEC, NULL);

#if defined(SWEET)
/* Phone default; QEMU virt wires the architected virtual timer to INTID 27. */
SECURITY_READ_ONLY_LATE(uint32_t) sweet_vtimer_intid = SWEET_VTIMER_INTID;
#endif

static vm_offset_t gicd_base;
static vm_offset_t gicr_base;
static vm_offset_t gicr_size;

static uint32_t
_gic_read32(vm_offset_t addr)
{
	return *((volatile uint32_t *) addr);
}

static uint64_t
_gic_read64(vm_offset_t addr)
{
	return *((volatile uint64_t *) addr);
}

static void
_gic_write32(vm_offset_t addr, uint32_t value)
{
	*((volatile uint32_t *) addr) = value;
}

#define gicd_read32(offset) (_gic_read32(gicd_base + (offset)))
#define gicd_write32(offset, data) (_gic_write32(gicd_base + (offset), (data)))
#define gicr_read32(offset) (_gic_read32(gicr_pe_base + (offset)))
#define gicr_write32(offset, data) (_gic_write32(gicr_pe_base + (offset), (data)))
#define gicr_read64(offset) (_gic_read64(gicr_pe_base + (offset)))

static vm_offset_t
find_gicr_pe_base()
{
	// We only care about aff1 and aff0
	uint32_t phys_id = __builtin_arm_rsr64("MPIDR_EL1") & (MPIDR_AFF1_MASK | MPIDR_AFF0_MASK);

	for (vm_offset_t offset = 0; offset < gicr_size; offset += GICR_PE_SIZE) {
		vm_offset_t gicr_pe_base = gicr_base + offset;
		uint64_t gicr_typer = gicr_read64(GICR_TYPER);
		uint32_t aff_value = (uint32_t) (gicr_typer >> GICR_TYPER_AFFINITY_VALUE_SHIFT) & (MPIDR_AFF1_MASK | MPIDR_AFF0_MASK);

		if (phys_id == aff_value) {
			return gicr_pe_base;
		}

		if (gicr_typer & GICR_TYPER_LAST) {
			break;
		}
	}

	panic("%s: cannot find GICR base for core %u", __func__, ml_get_cpu_number(phys_id));
}

void
pe_init_fiq()
{
	int error;
	DTEntry entry;

	// gicd_base is 0x0 only before it's initialized by the boot processor.
	// Avoid calling SecureDT* routines on secondary processors to avoid
	// race conditions because they are not thread-safe.
	if (!gicd_base) {
		// Find GIC DT node
		error = SecureDTLookupEntry(NULL, "/arm-io/gic", &entry);
		if (error != kSuccess) {
			panic("%s: cannot find GIC node in DT", __func__);
		}

		// Find "reg" property
		void const *prop;
		unsigned int prop_size;
		error = SecureDTGetProperty(entry, "reg", &prop, &prop_size);
		if (error != kSuccess) {
			panic("%s: cannot find GIC MMIO regions in DT", __func__);
		}

		// Need at least GICD base, GICD size, GICR base and GICR size
		if (prop_size < 4 * sizeof(uint64_t)) {
			panic("%s: incorrect reg property size in GIC DT node; expecting 32 bytes but got %u bytes", __func__, prop_size);
		}

#if defined(SWEET)
		/* Read only on the boot CPU, before the GIC and RO-late lockdown. */
		void const *timer_prop;
		unsigned int timer_size;
		if (SecureDTGetProperty(entry, "virtual-timer-intid", &timer_prop, &timer_size) == kSuccess) {
			if (timer_size != sizeof(uint32_t)) {
				panic("%s: invalid virtual-timer-intid size %u", __func__, timer_size);
			}
			uint32_t intid = *(const uint32_t *)timer_prop;
			if (intid < 16 || intid >= 32) {
				panic("%s: virtual timer INTID %u is not a PPI", __func__, intid);
			}
			sweet_vtimer_intid = intid;
		}
		kprintf("SWEET GIC: virtual timer INTID %u\n", sweet_vtimer_intid);
#endif

		vm_offset_t soc_base_phys = pe_arm_get_soc_base_phys();

		uint64_t const gicd_base_prop = ((uint64_t const *) prop)[0];
		uint64_t const gicd_size_prop = ((uint64_t const *) prop)[1];
		uint64_t const gicr_base_prop = ((uint64_t const *) prop)[2];
		uint64_t const gicr_size_prop = ((uint64_t const *) prop)[3];

		// Find GICD base address
		gicd_base = ml_io_map(soc_base_phys + gicd_base_prop, gicd_size_prop);

		if (!gicd_base) {
			panic("%s: cannot map GICD region", __func__);
		}

		// Find GICR base address
		gicr_base = ml_io_map(soc_base_phys + gicr_base_prop, gicr_size_prop);

		if (!gicr_base) {
			panic("%s: cannot map GICR region", __func__);
		}

		gicr_size = gicr_size_prop;
	}

	// Find the redistributor for this processor
	vm_offset_t gicr_pe_base = find_gicr_pe_base();

	// Mark this PE to be awake
	uint32_t gicr_waker = gicr_read32(GICR_WAKER);
	if (gicr_waker & GICR_WAKER_CHILDRENASLEEP) {
		gicr_waker &= ~GICR_WAKER_PROCESSORSLEEP;

		gicr_write32(GICR_WAKER, gicr_waker);

		uint64_t gicr_wake_deadline;
		nanoseconds_to_deadline(os_atomic_load(&gicr_wake_timeout_ns, relaxed), &gicr_wake_deadline);
		while (gicr_read32(GICR_WAKER) & GICR_WAKER_CHILDRENASLEEP) {
			// Spin
			if (gicr_wake_timeout_ns > 0 && mach_absolute_time() > gicr_wake_deadline) {
				panic("%s: core %u timed out waiting for redistributor to wake up",
				    __func__, ml_get_cpu_number_local());
			}
		}
	}

#if defined(SWEET)
	/*
	 * Real hardware with two security states: Group 0 is TrustZone's and
	 * non-secure writes to it are ignored. Everything we own is Group 1 NS
	 * and arrives as an IRQ (see sleh_irq). GICR_IGROUPR0 reads as zero
	 * from the non-secure side even after this write; delivery still works.
	 */
	gicr_write32(GICR_IGROUPR0, 0xFFFFFFFF);
	for (unsigned int i = 0; i < 32; i += 4) {
		gicr_write32(GICR_IPRIORITYR0 + i, 0xA0A0A0A0);
	}
	gicr_write32(GICR_ISENABLER0, (1U << sweet_vtimer_intid) | (1U << SWEET_IPI_SGI));
#else
	// Configure timers and legacy FIQ to be group 0
	gicr_write32(GICR_IGROUPR0, 0x81FFFFFF);

	// Enable PPI 27
	gicr_write32(GICR_ISENABLER0, (1 << 27));
#endif /* SWEET */

	// Enable system register access
	uint64_t icc_sre = __builtin_arm_rsr64("ICC_SRE_EL1");
	icc_sre |= ICC_SRE_SRE;
	__builtin_arm_wsr64("ICC_SRE_EL1", icc_sre);
	__builtin_arm_isb(ISB_SY);

#if defined(SWEET)
	__builtin_arm_wsr64("ICC_BPR1_EL1", 0);
#else
	// Set priority masks and binary point for group 0
	__builtin_arm_wsr64("ICC_BPR0_EL1", 0);
#endif /* SWEET */
	__builtin_arm_wsr64("ICC_PMR_EL1", 0xFF);

	// Set EOI mode of this processor
	uint64_t icc_ctlr = __builtin_arm_rsr64("ICC_CTLR_EL1");
	icc_ctlr &= ~ICC_CTLR_EOIMODE;
	__builtin_arm_wsr64("ICC_CTLR_EL1", icc_ctlr);

#if defined(SWEET)
	// ABL leaves only ARE_NS set; Linux writes ARE_NS | EnableGrp1A | EnableGrp1
	uint32_t gicd_ctlr = gicd_read32(GICD_CTLR);
	gicd_ctlr |= GICD_CTLR_ARE_NS | GICD_CTLR_ENABLEGRP1A | GICD_CTLR_ENABLEGRP1;
	gicd_write32(GICD_CTLR, gicd_ctlr);
	while (gicd_read32(GICD_CTLR) & GICD_CTLR_RWP) {
		;
	}
	__builtin_arm_wsr64("ICC_IGRPEN1_EL1", 1);
	__builtin_arm_isb(ISB_SY);
#else
	// Enable the forwarding of the vtimer interrupt
	uint32_t gicd_ctlr = gicd_read32(GICD_CTLR);
	gicd_ctlr |= GICD_CTLR_ENABLEGRP0;
	gicd_write32(GICD_CTLR, gicd_ctlr);
	__builtin_arm_wsr64("ICC_IGRPEN0_EL1", 1);
	__builtin_arm_isb(ISB_SY);
#endif /* SWEET */
}
#else
void
pe_init_fiq()
{
}
#endif
