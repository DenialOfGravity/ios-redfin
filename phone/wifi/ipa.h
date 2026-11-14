#ifndef IPA_H
#define IPA_H

/*
 * `wland ipahw ...` (ipa.c): AP-side IPA v4.7 (lito / SM7250) hardware setup, steps B3, C1, C2 of the cellular
 * data design. ARGV[0] = "ipahw", ARGV[1] = the subcommand. Returns the process exit code.
 */
int ipa_main(int argc, char **argv);

/* The same steps in-process (wland.c ipa=1 bring-up). 0 = done; nonzero = failed or gated (logged). */
int ipa_smmu(void);		/* `ipahw smmu`: IPA DMA streams 0x520/0x522 on translation-off context banks */
int ipa_meminit(void);		/* `ipahw meminit`: zero local memory above the uC area, canaries, LOCAL_PKT_PROC_CNTXT */

#endif
