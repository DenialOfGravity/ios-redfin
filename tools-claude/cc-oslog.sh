#!/bin/bash
# cc-oslog.sh PULLED_DIR [OUT_DIR]
# Parse an iOS unified log pulled from the phone and surface the CommCenter cellular
# activation-ticket / data-context lines, for the Route-2 ticket test (phone/cc/README.md).
#   PULLED_DIR must contain the phone's /private/var/db/diagnostics and uuidtext, either as
#   PULLED_DIR/{diagnostics,uuidtext} or PULLED_DIR/private/var/db/{diagnostics,uuidtext}.
#   OUT_DIR (default PULLED_DIR/out) gets all.csv (every entry), cc.csv (CommCenter /
#   mobileactivationd entries) and cc-key.txt (the gate lines, chronological).
# THE ONE CHECK: after you add the ticket and restart CommCenter, cc-key.txt should show
#   `isTicketAvailable = true` (today every run shows `= false source = kPlist`).
# Then read on in cc-key.txt for what CommCenter does next (path B, research section 6):
#   `carrier bundle` / `DeviceManagement` / SIM-slot / `StartNetworkInterface` / `data ... id=`
#   or a second `context is not assigned`.
# Needs Mandiant macos-UnifiedLogs' unifiedlog_iterator, built in ~/ulog (same as nfc-oslog.sh):
#   cd ~/ulog/macos-UnifiedLogs && cargo build --release --manifest-path examples/Cargo.toml -p unifiedlog_iterator
# Read-only on PULLED_DIR (the logarchive view is a directory of symlinks under OUT_DIR).
set -euo pipefail
IN=${1:?usage: cc-oslog.sh PULLED_DIR [OUT_DIR]}
OUT=${2:-$IN/out}
BIN=${ULOG_BIN:-$HOME/ulog/macos-UnifiedLogs/examples/target/release/unifiedlog_iterator}
[ -x "$BIN" ] || BIN=$HOME/ulog/macos-UnifiedLogs/target/release/unifiedlog_iterator
[ -x "$BIN" ] || { echo "unifiedlog_iterator not built (see header)"; exit 1; }
for base in "$IN" "$IN/private/var/db" "$IN/var/db"; do
	if [ -d "$base/diagnostics" ] && [ -d "$base/uuidtext" ]; then DB=$base; break; fi
done
[ -n "${DB:-}" ] || { echo "no diagnostics/ + uuidtext/ under $IN"; exit 1; }
mkdir -p "$OUT/archive"
for d in "$DB/uuidtext" "$DB/diagnostics"; do
	for e in "$d"/*; do ln -sfn "$(readlink -f "$e")" "$OUT/archive/$(basename "$e")"; done
done
echo "parsing $DB -> $OUT/all.csv"
"$BIN" --mode log-archive --input "$OUT/archive" --format csv --output "$OUT/all.csv" 2> "$OUT/parse-warnings.txt" || true
python3 -I - "$OUT/all.csv" "$OUT" <<'EOF'
import csv, sys, re
src, out = sys.argv[1], sys.argv[2]
csv.field_size_limit(1 << 30)
# CSV columns (unifiedlog_iterator write_record): 0 time, 2 log_type, 3 subsystem, 5 pid,
# 8 library, 12 category, 13 process, 15 message, ...
# Gate strings, newest-at-bottom. "isTicketAvailable"/"source =" is THE check; the rest is path B.
keys = [
    "isTicketAvailable", "postponement ticket", "ActivationTicket", "is_postponement_ticket_available",
    "source = kPlist", "source =", "Setting postponement ticket", "kPostponementTicketSet",
    "context is not assigned", "Incompatible Data Mode", "NESession",
    "carrier bundle", "CarrierBundle", "DeviceManagement", "fActivationEnabled",
    "StartNetworkInterface", "canActivate", "data context", "ActivationState",
]
kre = re.compile("|".join(re.escape(k) for k in keys), re.I)
idre = re.compile(r"\bid\s*=\s*-?\d+")   # "data ... id=" context ids
n = m = k = 0
with open(src, newline="", errors="replace") as f, open(out + "/cc.csv", "w", newline="") as fo, \
        open(out + "/cc-key.txt", "w") as fk:
    w = csv.writer(fo)
    for row in csv.reader(f):
        n += 1
        if len(row) < 16:
            continue
        sub, proc, lib, msg = row[3], row[13], row[8], row[15]
        if not ("CommCenter" in sub or "CommCenter" in proc or "mobileactivation" in sub
                or "mobileactivation" in proc or "CommCenter" in lib or "CommCenterBase" in lib):
            continue
        m += 1
        w.writerow([row[0], row[2], sub, row[12], proc, row[5], msg])
        if kre.search(msg) or idre.search(msg):
            k += 1
            fk.write(f"{row[0]} {proc}[{row[5]}] {sub}/{row[12]} {row[2]}: {msg}\n")
print(f"{n} entries, {m} CommCenter entries -> {out}/cc.csv, {k} gate lines -> {out}/cc-key.txt")
EOF
echo "--- isTicketAvailable lines (THE check) ---"
grep -i "isTicketAvailable" "$OUT/cc-key.txt" || echo "  (none seen)"
echo "--- gate lines (first 80) ---"
head -80 "$OUT/cc-key.txt"
