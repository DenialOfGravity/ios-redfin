#!/bin/bash
# nfc-on.sh [FWIF] [DLVER] [APDU_SW] [WAIT]: switch the NFC persona on (test settings, reset at reboot) and restart nfcd.
# Defaults = 2026-11-17 best known: fwif 0x2C5D10 (persona eSE/HCI 0x5D00 + keep A0xx 0x10 + download 0x40000/0x80000 +
# eSE APDU 0x200000 (1151) ...), dl_ver 0x6a01 (PN552_FW_02_01_6A), GET_CONFIG A022/A023 = 01 (SE pipes open), A118 = 4-byte flash write counter, id 4.
FWIF=${1:-0x2C5D10}; DLV=${2:-0x6a01}; SW=${3:-0x6a82}; WAIT=${4:-25}
pa sh "S=/var/local/bin/sctl; \$S kern.sweet_nfc_probe 1; \$S kern.sweet_nfc_id 4; \$S kern.sweet_nfc_dl_ver $DLV; \$S kern.sweet_nfc_apdu_sw $SW 2>/dev/null; \$S kern.sweet_nfc_cfg_set 'A022=01,A023=01,A118=00000000'; \$S kern.sweet_nfc_p1 1; \$S kern.sweet_nfc_fwif_fmt $FWIF; /var/local/bin/psx -k nfrestore_servic 9; /var/local/bin/psx -k nfcd 9; sleep $WAIT; /var/local/bin/psx nfcd"
