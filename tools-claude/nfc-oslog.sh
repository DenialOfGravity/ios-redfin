#!/bin/bash
# nfc-oslog.sh PULLED_DIR [OUT_DIR]
# Parse an iOS unified log pulled from the phone and list the NFC (nfcd / com.apple.nfc) lines with timestamps.
#   PULLED_DIR must contain the phone's /private/var/db/diagnostics and /private/var/db/uuidtext, either as
#   PULLED_DIR/diagnostics + PULLED_DIR/uuidtext or as PULLED_DIR/private/var/db/{diagnostics,uuidtext}.
#   OUT_DIR (default PULLED_DIR/out) gets all.csv (every entry), nfc.csv (NFC entries) and nfc-key.txt (the
#   handler strings from logs/research/nfc-ese-2026-11-16.md, Follow-up 4).
# Needs Mandiant macos-UnifiedLogs' unifiedlog_iterator, built in ~/ulog:
#   cd ~/ulog/macos-UnifiedLogs && cargo build --release --manifest-path examples/Cargo.toml -p unifiedlog_iterator
# Read-only on PULLED_DIR (the logarchive view is a directory of symlinks under OUT_DIR).
set -euo pipefail
IN=${1:?usage: nfc-oslog.sh PULLED_DIR [OUT_DIR]}
OUT=${2:-$IN/out}
BIN=${ULOG_BIN:-$HOME/ulog/macos-UnifiedLogs/examples/target/release/unifiedlog_iterator}
[ -x "$BIN" ] || BIN=$HOME/ulog/macos-UnifiedLogs/target/release/unifiedlog_iterator
[ -x "$BIN" ] || { echo "unifiedlog_iterator not built (see header)"; exit 1; }
for base in "$IN" "$IN/private/var/db" "$IN/var/db"; do
	if [ -d "$base/diagnostics" ] && [ -d "$base/uuidtext" ]; then DB=$base; break; fi
done
[ -n "${DB:-}" ] || { echo "no diagnostics/ + uuidtext/ under $IN"; exit 1; }
mkdir -p "$OUT/archive"
# logarchive layout = contents of uuidtext and diagnostics side by side
for d in "$DB/uuidtext" "$DB/diagnostics"; do
	for e in "$d"/*; do ln -sfn "$(readlink -f "$e")" "$OUT/archive/$(basename "$e")"; done
done
echo "parsing $DB -> $OUT/all.csv"
"$BIN" --mode log-archive --input "$OUT/archive" --format csv --output "$OUT/all.csv" 2> "$OUT/parse-warnings.txt" || true
python3 -I - "$OUT/all.csv" "$OUT" <<'EOF'
import csv, sys, re
src, out = sys.argv[1], sys.argv[2]
csv.field_size_limit(1 << 30)
# CSV columns (unifiedlog_iterator write_record): 0 time, 1 event, 2 log_type, 3 subsystem, 4 tid, 5 pid, 6 euid,
# 7 persona, 8 library, 9 lib uuid, 10 activity, 11 parent, 12 category, 13 process, 14 proc uuid, 15 message, ...
keys = [
    "NFCEE Mode Set process Success", "Set Mode Notification Timer Created", "Set Mode ntf timer started",
    "Received IceFall Power OFF notification", "Invoking upper layer call back", "NO upper layer call back",
    "Set Mode notification TIMEOUT", "NfceeDisableProc", "Nfcee ModeSet Sequence failed", "Disable Nfcee mode failed",
    "DisableNfceeDisc", "SetMode Skipped", "Timer restart count reached limit", "HCI Child Device Init Failed",
    "Set Session ID Failed", "Received System End of operation", "RXD eSE EOP", "EoOp Ntf is dropped",
    "Initialization in Progress", "Hci network initialization", "eSE ReInit Sequence", "VerifySEStatus",
    "NFCC Re-Init failed", "Static feature config", "DelayForSeNtf", "DownloadRequest", "firmware",
]
kre = re.compile("|".join(re.escape(k) for k in keys), re.I)
n = m = k = 0
with open(src, newline="", errors="replace") as f, open(out + "/nfc.csv", "w", newline="") as fo, \
        open(out + "/nfc-key.txt", "w") as fk:
    w = csv.writer(fo)
    for row in csv.reader(f):
        n += 1
        if len(row) < 16:
            continue
        sub, proc, lib, msg = row[3], row[13], row[8], row[15]
        if not ("com.apple.nfc" in sub or "nfcd" in proc or "nfrestore" in proc or "NFC" in lib or "PN548" in lib):
            continue
        m += 1
        w.writerow([row[0], row[2], sub, row[12], proc, row[5], msg])
        if kre.search(msg):
            k += 1
            fk.write(f"{row[0]} {proc}[{row[5]}] {sub}/{row[12]} {row[2]}: {msg}\n")
print(f"{n} entries, {m} NFC entries -> {out}/nfc.csv, {k} key lines -> {out}/nfc-key.txt")
EOF
echo "--- key lines (first 80) ---"
head -80 "$OUT/nfc-key.txt"
