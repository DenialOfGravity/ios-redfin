#!/bin/sh
# simalert-check [MINUTES] [LINES]: why the SIM PIN alert is (not) up, in one call.
#   1. lockwatch: what SpringBoard reports now (sb_lock = CommCenter's "coversheet active", mkb = "display locked")
#   2. CommCenter's alert decisions from the persisted unified log of the last MINUTES (default 15), last LINES (80)
# Read-only. Install: pa put out/simalert-check.sh /var/local/bin/simalert-check 755 --kg "..."
# Gate (CommCenter 0x100b0d62c): alert shown only if coversheet inactive, or passcode-locked (then on the lock screen).
M=${1:-15}
N=${2:-80}
now=$(date +%s)
from=$((now - M * 60))
echo "== lockwatch"
/var/local/bin/lockwatch
echo "== CommCenter/SpringBoard log, last $M min"
/var/local/bin/oslogshow "$from" "$now" CommCenter SpringBoard 2>/dev/null |
	grep -E 'SIM unlock|unlock alert|coversheet|Coversheet active|display status|Display on|Display locked|buddy state|pending alert|pin locked|SpringBoardFinishedStartup|kSimStateLocked|kSimStateReady|RemoteAlertHandle|SIMSetup|user prefers' |
	tail -n "$N"
