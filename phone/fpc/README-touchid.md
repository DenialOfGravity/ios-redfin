# Touch ID on redfin (phone baseline 2026-11-18)

All kernel features default off; a boot without the Touch ID switches behaves like the baseline (no Touch ID, no
passcode). Owner-tested and declared the phone baseline on kernel ws-tid-k ec02376 + sweetfpcd ws-fpcd-e 8752bdc + SweetFPS ws-u-tidui 4f9fe4b.

## Works (owner-tested)
- Passcode lock (SweetKeyStore), Touch ID enrolment in Settings (both phases, haptic per touch).
- Unlock: rest finger on the cover sheet (opens to home), finger on the passcode pad (native InBioUnlock path),
  first touch matches (update_template after every identify, as the stock FPC HAL).
- Screen off: right finger is matched with the display dark, then the phone wakes and unlocks; wrong finger stays dark.
- Wrong fingers: "Try again" per touch, matching continues, lockout after 5 until the passcode (as iOS).
- Persistence: fingerprints survive reboots (biometrickitd catacomb = FPC TA template db, master record).
- Lock screen: native Face ID-style padlock driven by Touch ID (SweetFPS hook, /var/local/sweettidui = "h").
- Short non-native unlock buzz (kern.sweet_passcode_bio_buzz, 12 ms).
- sweetfpcd runs as a launchd job (com.redfin.sweetfpcd, KeepAlive, FPCD_KEEP_TA=1, own capped log).

## Pieces
- Kernel (xnu/patches): 1186 fb sleep blank, 1190 FPC GPIOs, 1191 UFS RPMB pass-through, 1192 SweetMesa persona
  (AppleMesaSEPDriver / AppleBiometricServices stand-in + SweetMesaBridge), 1193 storm guard, 1194 SweetABP prepare
  fix, 1195-1197 passcode + SweetACM + keybag events, 1198-1205 match/unlock rules + op ends, 1206/1218 catacomb,
  1207 offline answers, 1210-1215 op queue fixes, 1214 InBioUnlock, 1216 ACM bind (off), 1217 buzz, 1225 lockout,
  1235 screen-off wake, 1245 fwd wait, 1246 lost-load reload, 1247 + 1256 boot switch sweettouchid=1 (sets the whole set at boot via direct setters), 1255 match first.
- Image r5tid17 (= agent/cycle-base.txt): base mesa-a1 (adds /arm-io/mesa) + cmdline agent/stage/cmdline.bb19u15-ubwc2-xr13-tid2 (sweettouchid=1;
  590 bytes: keepsyms dropped, sweetfbx=263; mkimage limit 591).
- Userland: phone/fpc/sweetfpcd (FPC TA in TrustZone via phone/qsee), launchd job phone/fpc/com.redfin.sweetfpcd.plist,
  biometrickitd mesa job, phone/fps/SweetTIDUI.m (padlock), relinked iPhone 8 libBKDM1 + MG touch-id keys (not in git).

## Switches (applied at boot by sweettouchid=1; kern.sweet_touchid_boot reports OK/FAILED per key; tools-claude/tid-bringup.sh only for kernels without 1256)
kern.sweet_fpc=3, sweet_passcode=1, sweet_fb_sleep_blank=1, sweet_rpmb=1, sweet_mesa_match=2 (unverified),
sweet_mesa_catacomb=7, sweet_mesa_offline=1, sweet_passcode_biostate=1, sweet_mesa_fix=0x17, sweet_mesa_lockout=0xf,
sweet_passcode_bio_buzz=0x7f000c, sweet_mesa_wake=12 (match first; 5 = wake then match), sweet_mesa_fwd_wait_ms=15000,
sweet_mesa_reload=1, then sweet_mesa=3 + biometrickitd restart.

## Open
- Matches unverified (HAT fields check out; keymaster verify not wired). SweetACM is a stand-in.
- Native success haptic does not play on rest-to-open (SpringBoard plays it only while the lock screen is visible).
- Display engine hang on sleep (not Touch ID): logs/research/freeze-tid23-2026-11-18.md on the PC.
Research notes (PC, not in git): logs/research/touchid-*.md, fingerprint-*.md.
