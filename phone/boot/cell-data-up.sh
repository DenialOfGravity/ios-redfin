#!/bin/sh
# cell-data-up.sh -- per-boot cellular-data bring-up for redfin (native CommCenter).
#
# Kernel ws-int-k25 (ws-int-k24 + patch 1156) gives data-capable CommCenter, but three
# things must happen in order, every boot, AFTER the SIM is unlocked:
#   1. kern.sweet_cc_datacap=1  -> patch 1156 page-patches CommCenter's vf438 (data-capable)
#                                  at its NEXT launch (hook fires on shared_region_check_np).
#                                  The cmdline is full, so there is no boot-arg room: the
#                                  sysctl must be set and CommCenter restarted for it to take.
#   2. kern.sweet_data_fwd=1 / kern.sweet_qmi_rules=1 -> patch 1080 lets CommCenter own data.
#   3. CommCenter re-registers and writes a WDS 0x0027 data profile; patch 1080 HOLDS that
#      write until the modem-profile backup is acknowledged. The ack is a state machine:
#      profile write sets pb_state 0->3 ("ready", bak exposed); writing
#      kern.sweet_wds_profiles_ack=1 THEN advances 3->4, releases the writes, and the modem
#      brings up pdp_ip0 (StartNetworkInterface). A pre-set ack is useless (EBUSY unless
#      pb_state==3) -- ack must be written AFTER "profile backup ready".
#
# Tools: /var/local/bin/sctl (read: "NAME: VALUE"; write: "NAME: OLD -> NEW"),
#        /var/local/bin/psx -k NAME (kill every proc named exactly NAME).
# POSIX sh only -- toybox has no tr/awk. Idempotent and safe to re-run.
#
# Expected SWEET klog around the restart:
#   "SWEET cc: vf438 patched (datacap)"  then  "SWEET qmux: profile backup ready" (pb_state 3)
#   then on ack  "SWEET qmux: profile writes released" + "SWEET ipa: dp ... StartNetworkInterface".

SCTL=/var/local/bin/sctl
PSX=/var/local/bin/psx
BAK_TMO=90          # seconds to wait for the profile backup to be ready
IP_TMO=45           # seconds to wait for a data bearer after ack

say() { echo "cell-data-up: $*"; }

# read one sysctl, strip the "NAME: " prefix that sctl prints -> just the value
scget() {
	line=$("$SCTL" "$1" 2>/dev/null)
	echo "${line#*: }"
}

# success == kernel IPA data path at stage 2 with a non-zero assigned IP
data_up() {
	stats=$(scget kern.sweet_ipa_stats)
	case "$stats" in
		*"dp stage 2"*)
			case "$stats" in
				*"ip 0.0.0.0"*) return 1 ;;   # stage 2 but no IP yet
				*) return 0 ;;
			esac ;;
		*) return 1 ;;
	esac
}

# 1. race-free sysctls (also boot-defaulted in the manifest; set again here to be sure)
"$SCTL" kern.sweet_cc_datacap 1 >/dev/null 2>&1
"$SCTL" kern.sweet_data_fwd  1 >/dev/null 2>&1
"$SCTL" kern.sweet_qmi_rules 1 >/dev/null 2>&1
say "sysctls set: cc_datacap=$(scget kern.sweet_cc_datacap) data_fwd=$(scget kern.sweet_data_fwd) qmi_rules=$(scget kern.sweet_qmi_rules)"

# idempotent: if data is already up, do nothing (do NOT restart CommCenter and tear it down)
if data_up; then
	say "data already up (ipa_stats: $(scget kern.sweet_ipa_stats)); nothing to do"
	exit 0
fi

# 2. restart CommCenter so the kernel hook patches vf438 (datacap) at relaunch; launchd respawns it
say "restarting CommCenter (psx -k CommCenter)"
"$PSX" -k CommCenter || say "note: CommCenter was not running (will be launched by launchd)"

# 3. wait for the WDS profile backup to be ready (pb_state==3). Until then sctl returns
#    "state N (...)"; when ready it returns the raw hex TLV backup (no "state " prefix).
say "waiting up to ${BAK_TMO}s for profile backup ready (pb_state 3)..."
ready=0
i=0
while [ "$i" -lt "$BAK_TMO" ]; do
	i=$((i+1))
	bak=$(scget kern.sweet_wds_profiles_bak)
	case "$bak" in
		"state 4 "*)                      # already acknowledged (e.g. fast re-register)
			say "pb_state already 4 (acknowledged)"; ready=4; break ;;
		"state "*|"")                     # 0 none / 1 listing / 2 reading, or empty: keep waiting
			sleep 1; continue ;;
		*)                                # non-"state" payload == the exposed backup, pb_state 3
			ready=3; break ;;
	esac
done

if [ "$ready" = 3 ]; then
	# 4. save the one-shot backup (volatile per manifest: /var/local/modem-profiles-*.bak), then ack
	bakfile=/var/local/modem-profiles-$(date +%s).bak
	printf '%s' "$bak" > "$bakfile"
	say "profile backup ready; saved to $bakfile ($(printf '%s' "$bak" | wc -c) bytes)"
	res=$("$SCTL" kern.sweet_wds_profiles_ack 1 2>&1)
	say "ack: $res"
elif [ "$ready" = 4 ]; then
	say "backup already acknowledged this boot; not re-acking"
else
	say "TIMEOUT: profile backup not ready after ${BAK_TMO}s (bak=$(scget kern.sweet_wds_profiles_bak)); data will not come up"
	exit 1
fi

# 5. wait for the bearer and report
say "waiting up to ${IP_TMO}s for data bearer..."
i=0
while [ "$i" -lt "$IP_TMO" ]; do
	i=$((i+1))
	if data_up; then
		say "SUCCESS: data up -- $(scget kern.sweet_ipa_stats)"
		exit 0
	fi
	sleep 1
done
say "FAILURE: no data bearer after ${IP_TMO}s -- $(scget kern.sweet_ipa_stats)"
exit 1
