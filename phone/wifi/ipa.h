#ifndef IPA_H
#define IPA_H

/*
 * `wland ipahw ...` (ipa.c): AP-side IPA v4.7 (lito / SM7250) hardware setup, steps B3, C1, C2 of the cellular
 * data design. ARGV[0] = "ipahw", ARGV[1] = the subcommand. Returns the process exit code.
 */
int ipa_main(int argc, char **argv);

#endif
