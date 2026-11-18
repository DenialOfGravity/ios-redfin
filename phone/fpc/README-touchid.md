# Touch ID on redfin (2026-11-18, test line)

All kernel features default off; a boot with these defaults behaves like the baseline (no Touch ID, no passcode).

## Pieces
- Kernel (xnu/patches): 1186 fb sleep blank, 1190 FPC GPIOs, 1191 UFS RPMB pass-through, 1192 SweetMesa persona
  (AppleMesaSEPDriver / AppleBiometricServices stand-in + SweetMesaBridge), 1193 gpio112 storm guard, 1194 SweetABP
  prepare fix, 1195 passcode (SweetKeyStore) + SweetACM, 1196 keybag events, 1197 no grace hold while locked,
  1198-1202 match/unlock rules, 1203-1205 op end statuses + unlock delay, 1206 catacomb (template persistence, WIP),
  1207 offline answers + re-init on bridge connect, 1210-1213 op queue fixes (implicit op end, stale-event drop,
  AlignmentData node placement, no 0x41 after enrol).
- Image: base mesa-a1 (adds /arm-io/mesa) + cmdline agent/stage/cmdline.bb19u15-ubwc2-xr13 (needs sweetbb=19).
- Userland: phone/fpc/sweetfpcd (FPC TA in TrustZone via phone/qsee/qsee_lib), relinked iPhone 8 libBKDM1 (not in git),
  biometrickitd mesa launchd job (phone/fpc/com.apple.biometrickitd.mesa.plist), MobileGestalt touch-id keys.

## Switches (bring-up order: tools-claude/tid-bringup.sh)
kern.sweet_fpc=3, kern.sweet_passcode=1, kern.sweet_fb_sleep_blank=1, kern.sweet_rpmb=1, kern.sweet_mesa_match=2
(unverified matches), kern.sweet_mesa_catacomb=1, kern.sweet_mesa_offline=1, kern.sweet_mesa=3 (restart
biometrickitd), then start sweetfpcd with FPCD_KEEP_TA=1, respring. kern.sweet_mesa_fix (default 0x7: bit0 implicit
op end, bit1 stale-event drop, bit2 AlignmentData, bit3 0x41 after enrol).

## State (owner-tested on ws-tid-d 99a6c39 + sweetfpcd d85803d)
Works: passcode lock, enrolment in Settings (both phases, haptics), rest-finger-to-open from the cover sheet.
Open: finger on the passcode pad fails once then stops; templates not yet persistent (TA store_template_db -6);
matches unverified (HAT fields now check out); no unlock feedback; launchd job for sweetfpcd not installed.
Research notes (PC, not in git): logs/research/touchid-*.md, fingerprint-*.md.
