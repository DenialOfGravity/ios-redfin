/*
 * 0994: kdebug events for the frame tracer (phone/tools/sweettrace.c records them, tools-claude/sweet-trace.py reads
 * them). Class DBG_DRIVERS (6), subclass 0xF5: debugid 0x06F5_0000 | code << 2 | DBG_FUNC_START (1) / END (2).
 * KERNEL_DEBUG_CONSTANT: compiled in on DEVELOPMENT kernels (KDEBUG_LEVEL_STANDARD), and only a load of kdebug_enable
 * and a branch while no trace runs (the arguments are evaluated inside the branch). No boot-arg.
 *
 * Code  Name          Args (a1, a2, a3, a4)
 * 0x01  VSYNC         vsync timer tick: count, sent (1) or held (0), sweetpace mode, INTF1 frame counter
 * 0x02  VSYNC_SEND    vsync (type 5) notification out: count, clients that take it, timestamp sent, period
 * 0x03  NOTIFY        IOMFB async notification: type, client pid, IOReturn of the send (non-zero: dropped), 0
 * 0x04  IOMFB_SEL     IOMFB externalMethod, START: selector, scalar[0], scalar[1], structIn size;
 *                     END: selector, IOReturn, scalarOut[0], swap id (SwapEnd: the record's id)
 * 0x05  SWAP          AppleCLCD::swap, START: swap id, fence submitted, layer mask, 0;
 *                     END: swap id, path (0 pending for the present, 1 direct scanout, 2 swap queue)
 * 0x06  PRESENT       AppleCLCD::present START/END: swap id, END: 1 if put off (busy)
 * 0x07  SHOWN         a frame went to the panel (statShown): swap time (abs), 0, 0, 0
 * 0x08  SWAPDONE      swapDone: where (1 blit fence, 2 CPU copy, 3 scanout latch), 0, 0, 0
 * 0x09  SCAN_SWAP     direct scanout at SwapEnd: swap id, seq, how (0 next, 1 queued, 2 replaced one, 3 not
 *                     direct yet, 4 not a slot surface), chain depth
 * 0x0a  SCAN_FLIP     VIG0/1 pointed at a frame (its fence landed): swap id, seq, SwapEnd->fence (abs), INTF1 frames
 * 0x0b  SCAN_LATCH    flipped frame on screen: swap id, seq, SwapEnd->latch (abs), frames since the flip
 * 0x0c  SCAN_BP       SwapEnd back pressure (gate 0) START/END: seq needed, shown seq
 * 0x10  GPU_SUBMIT    sweet_gpu_submit_v2 (any client, also the kernel's presents), START: pid, IBs, 0, 0 (before the
 *                     scanout gate); END: fence (0: refused), CP wait reference (gate 2), pid, ALWAYS_ON ticks (0 unless
 *                     a GPU_CAL went out with this submit)
 * 0x11  GPU_DONE      a fence's stamps (read at the next submit): fence, start ticks, end ticks, pid
 *                     (19.2 MHz CP_ALWAYS_ON; with sweetgputs=1 the end is RB_DONE_TS)
 * 0x12  GPU_WAIT      sweet_gpu_wait_fence_ns (Turnip's waits, a spin in 20 us steps) when the fence was not done
 *                     yet, START: fence, timeout (ns); END: fence, 0 done / 1 timeout, last done fence
 * 0x13  GPU_CAL       ALWAYS_ON ticks, mach_absolute_time before, after (once per submit, at most every 50 ms)
 * 0x14  GPU_GATE      sweet_scan_submit_gate for the swapper: gate, gate seq, shown seq, CP word; START/END around a
 *                     gate-1 CPU wait: pid, gate seq, shown seq
 * 0x15  ACCEL_SEL     SweetAccelerator v2 method START: method, pid; END: method, IOReturn, fence (submit)
 * 0x20  TOUCH         digitizer report: finger, down, x, y (FTS: the GPIO is polled every 8 ms, no IRQ)
 */
#ifndef SWEET_TRACE_H
#define SWEET_TRACE_H
#include <sys/kdebug.h>

#define SWEET_TRACE_SUBCLASS    0xF5
#define SWT_VSYNC               0x01
#define SWT_VSYNC_SEND          0x02
#define SWT_NOTIFY              0x03
#define SWT_IOMFB_SEL           0x04
#define SWT_SWAP                0x05
#define SWT_PRESENT             0x06
#define SWT_SHOWN               0x07
#define SWT_SWAPDONE            0x08
#define SWT_SCAN_SWAP           0x09
#define SWT_SCAN_FLIP           0x0a
#define SWT_SCAN_LATCH          0x0b
#define SWT_SCAN_BP             0x0c
#define SWT_GPU_SUBMIT          0x10
#define SWT_GPU_DONE            0x11
#define SWT_GPU_WAIT            0x12
#define SWT_GPU_CAL             0x13
#define SWT_GPU_GATE            0x14
#define SWT_ACCEL_SEL           0x15
#define SWT_TOUCH               0x20

#define SWEET_TRACE_ID(code)    KDBG_CODE(DBG_DRIVERS, SWEET_TRACE_SUBCLASS, code)
#define SWEET_TRACE_ON()        __improbable(kdebug_enable != 0)
#define SWEET_TRACE(code, a, b, c, d) \
	KERNEL_DEBUG_CONSTANT(SWEET_TRACE_ID(code) | DBG_FUNC_NONE, a, b, c, d, 0)
#define SWEET_TRACE_START(code, a, b, c, d) \
	KERNEL_DEBUG_CONSTANT(SWEET_TRACE_ID(code) | DBG_FUNC_START, a, b, c, d, 0)
#define SWEET_TRACE_END(code, a, b, c, d) \
	KERNEL_DEBUG_CONSTANT(SWEET_TRACE_ID(code) | DBG_FUNC_END, a, b, c, d, 0)

#endif /* SWEET_TRACE_H */
