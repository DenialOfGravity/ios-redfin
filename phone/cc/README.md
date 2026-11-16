# CommCenter cellular activation-ticket test (Route 2) — ws-cc-ticket

Goal: present a **minimal, non-empty, locally-crafted** ActivationTicket to CommCenter and see
whether the cellular-data gate's `isTicketAvailable` flips true, then capture what the *next*
gate (path B) does. Interoperability test on our own hardware. No Apple server, no impersonation.
See `logs/research/activation-ticket-2026-11-16.md` and `research/cc/M3-gate-analysis.md`.

## The gate (proven by disasm)
`sub_100842344` reads `ActivationTicket` from the activation-record dict, base64-decodes it, and
stores *non-empty?* into `isTicketAvailable` (field +0x448, logged `source = kPlist`). **No
signature, SEP, or keybag check.** Today every boot logs `isTicketAvailable = false source = kPlist`
because the seeded record has no ticket; path A then blocks cellular data with `id=-2`
("context is not assigned"). Voice/SMS/registration do **not** pass through this gate.

## Route chosen: 2 (data-ark ActivationTicket), NOT route 1 (XPC kPostponementSetTicket)
Route 1 was the brief's preferred option, but the disasm rules it out for *this* test:

* The XPC `kPostponementSetTicket` handler (`sub_10063e3a0`) stores the ticket via the service-map
  setter `sub_100363e5c`, which only takes a mutex and writes an **in-memory** member — no
  CFPreferences, no plist, no file write. So the injection is **wiped by a CommCenter restart** and
  never produces `source = kPlist`. The one CHECK is "restart CommCenter → `isTicketAvailable = true`",
  which route 1 structurally cannot satisfy, and the setter writes a different object than the DSC
  field (+0x448 / vf178) that path A reads.
* Route 1 also needs the `com.apple.private.MobileActivation` entitlement honoured on an ad-hoc
  helper under patched AMFI, plus the exact mach-service name and XPC message schema — all
  unverified in the research and unavailable to pin down without the phone.

Route 2 feeds `sub_100842344` directly (the proven write site, `source = kPlist`), is **persistent
across the CommCenter restart** in the CHECK, needs no entitlement/AMFI/XPC unknowns, and is undone
by restoring one file. Risk: whether mobileactivationd re-validates a ticket it serves from the ark
is unproven (research §8). Mitigation: it already serves `-ActivationState`/`-BrickState` verbatim
(the Activated state that makes voice/SMS work), and those carry no per-field signature; the
Hactivation AccountToken is the only thing it signs, and that is unchanged. If it *does* reject the
key, the symptom is `isTicketAvailable` staying false — a clean no-op — and the `.knowngood` restore
returns the exact prior file. Voice/SMS cannot regress from this unless ActivationState itself
changes, which the operator checks (step 5) and the undo reverses.

**Key assumption:** the data-ark `-ActivationTicket` directive surfaces as record key
`ActivationTicket`, exactly as `-ActivationState` surfaces as `ActivationState`. If mobileactivationd
namespaces it otherwise, isTicketAvailable just stays false (reversible no-op).

## The payload — `cc-ticket.b64`
`U1dFRVQtQ0MtVElDS0VULVBMQUNFSE9MREVSLU5PVC1SRUFMLXYx` → 39 placeholder bytes. It is a **minimal
non-empty base64 blob and carries NO real provisioning data** — expected for this first test; it only
proves the boolean gate. (A real data bearer would still need genuine cellular provisioning the
baseband/WDS reads later — research §6.)

## Nothing is built here
Route 2 is a plist edit, so there is no binary to compile (no `lb-bin` needed). `mk-data-ark.py`
runs on the PC; nothing is sent to the phone by these files. **DO NOT apply — the operator does,
after user OK.**

## Operator steps (no reboot: userland daemon restart only)
Run from the repo on the PC/WSL; `files` = `tools/flame-files.py` (the phone agent's put/get).
The live ark is in the mobileactivationd data container:
`/private/var/containers/Data/System/<UUID>/Library/internal/data_ark.plist` (0600 mobile:mobile).

    # 0. find the live path (UUID varies) and pull a copy
    ARK=$(ssh-or-agent: python3 - <<'PY'  # or run tools/sweet-root-tweaks.find_container against /
    import glob,plistlib
    for m in glob.glob('/private/var/containers/Data/System/*/.com.apple.mobile_container_manager.metadata.plist'):
        if plistlib.load(open(m,'rb')).get('MCMMetadataIdentifier')=='com.apple.mobileactivationd':
            print(m.rsplit('/',1)[0]+'/Library/internal/data_ark.plist')
    PY
    )
    files get "$ARK" data_ark.plist.live
    # 1. BACK UP (.knowngood) and record it
    cp data_ark.plist.live data_ark.plist.knowngood        # keep this; it is the undo source
    #    also: append the path + sha256 of data_ark.plist.knowngood to PC KNOWN-GOOD.txt
    # 2. build the patched copy (preserves every key + binary format)
    python3 -I phone/cc/mk-data-ark.py add data_ark.plist.live data_ark.plist.ticket
    python3 -I phone/cc/mk-data-ark.py show data_ark.plist.ticket   # sanity: ActivationTicket PRESENT
    # 3. push it back, restore owner/mode
    files put data_ark.plist.ticket "$ARK" 600
    (on phone) chown mobile:mobile "$ARK"
    # 4. restart the two daemons (no reboot)
    (on phone) psx -k mobileactivationd ; psx -k CommCenter      # launchd respawns both
    # 5. confirm voice/SMS path intact (must stay Activated)
    (on phone) read ActivationState via your usual check; expect still "Activated"

## THE CHECK (read-only)
Pull the unified log and run the reader, then look for the ticket line:

    files get /private/var/db/diagnostics ... (diagnostics + uuidtext)  -> PULLED/
    tools-claude/cc-oslog.sh PULLED/
    # PASS  = cc-key.txt shows:  isTicketAvailable = true        (today: = false source = kPlist)
    grep -i isTicketAvailable PULLED/out/cc-key.txt

If it flips true, path A is passed; read on in `cc-key.txt` for path B (research §6):
any `carrier bundle` / `DeviceManagement` / SIM-slot `vf168` / `StartNetworkInterface` /
`data ... id=` line, or a second `context is not assigned`.

## UNDO (full revert)
    files put data_ark.plist.knowngood "$ARK" 600
    (on phone) chown mobile:mobile "$ARK" ; psx -k mobileactivationd ; psx -k CommCenter
This restores the byte-exact prior ark. (If no backup was kept:
`python3 -I phone/cc/mk-data-ark.py del data_ark.plist.ticket data_ark.plist.clean` rebuilds a
ticket-free copy, but prefer the `.knowngood` restore.)

## No kernel change
Route 2 touches only a userland plist served by a userland daemon. No kernel change, no new
entitlement, no AMFI change, no `kern.sweet_*` switch needed.
