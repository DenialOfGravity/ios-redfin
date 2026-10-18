/*
 * Board config for the Redmi Note 10 Pro ("sweet", Qualcomm SM7150 /
 * Snapdragon 732G): Kryo 470 = Cortex-A76 + Cortex-A55, ARMv8.2, no PAC.
 *
 * Generic ARM like BCM2837 (no APPLE_ARM64_ARCH_FAMILY: none of Apple's
 * implementation-defined system registers exist), 16K pages and GICv3 like
 * VMAPPLE. Unlike VMAPPLE the GIC has two security states (GICD_TYPER
 * SecurityExtn=1): Group 0 belongs to TrustZone, so every interrupt,
 * including the timer, is a Group 1 IRQ taken through sleh_irq().
 */

#ifndef _PEXPERT_ARM64_SWEET_H
#define _PEXPERT_ARM64_SWEET_H

#define NO_MONITOR                1
#define NO_ECORE                  1

#define SWEET                     1

/*
 * All eight cores (six A55 + two A76) as one homogeneous SMP cluster:
 * cpus=N is clamped here. Same ISA level (ARMv8.2), 64-byte lines, PSCI
 * CPU_ON; the scheduler does not know the A76s are faster (no AMP/P-E
 * clusters yet).
 */
#define SWEET_SMP_MAX_CPUS         8

#define ARM_ARCH_TIMER
#define __ARM_ARCH__              8
#define __ARM_VMSA__              8
#define __ARM_VFP__               4
#define __ARM_COHERENT_CACHE__    1
#define __ARM_DEBUG__             7
#define __ARM_ENABLE_SWAP__       1
#define __ARM_V8_CRYPTO_EXTENSIONS__ 1
#define __ARM_PAN_AVAILABLE__     1
#define __ARM_16K_PG__            1

#define HAS_GIC_V3                1

#ifndef ASSEMBLER
#define PLATFORM_PANIC_LOG_DISABLED
#endif /* ! ASSEMBLER */

/*
 * Qualcomm numbers the architected timer PPIs 1/2/3/0 (secure phys, phys,
 * virt, hyp) instead of the usual 13/14/11/10: the virtual timer XNU uses
 * (CNTV) is INTID 16 + 3 = 19.
 */
#define SWEET_VTIMER_INTID        19
#define SWEET_IPI_SGI             1       // SGI used for cross-CPU signals

#define GIC_SPURIOUS_IRQ          1023    // IRQ no. for GIC spurious interrupt

#define GICR_PE_SIZE              0x20000 // Size of each redistributor region

/* GICv3 register definitions; see GICv3 spec (Arm IHI 0069G) */
#define GICD_CTLR                 0x0

/* Non-secure view of GICD_CTLR (DS=0) */
#define GICD_CTLR_ENABLEGRP1      0x1
#define GICD_CTLR_ENABLEGRP1A     0x2
#define GICD_CTLR_ARE_NS          0x10
#define GICD_CTLR_RWP             0x80000000

#define GICR_TYPER                              0x08
#define GICR_WAKER                              0x14
#define GICR_IGROUPR0                           0x10080
#define GICR_ISENABLER0                         0x10100
#define GICR_IPRIORITYR0                        0x10400

#define GICR_TYPER_AFFINITY_VALUE_SHIFT         32
#define GICR_TYPER_LAST                         0x10

#define GICR_WAKER_PROCESSORSLEEP               0x2
#define GICR_WAKER_CHILDRENASLEEP               0x4

#define ICC_CTLR_EOIMODE                        0x2

#define ICC_SRE_SRE                             0x1
/* End of GICv3 register definitions */

#endif /* ! _PEXPERT_ARM64_SWEET_H */
