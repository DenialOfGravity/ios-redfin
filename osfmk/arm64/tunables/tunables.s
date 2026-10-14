/*
 * sweet: Apple's per-SoC CPU tunables (implementation-defined HID/ACC
 * register settings applied in start.s) are not in the open source drop,
 * and none of them exist on Cortex-A76/A55. APPLY_TUNABLES does nothing.
 */
.macro APPLY_TUNABLES midr, tmp1, tmp2
.endmacro
