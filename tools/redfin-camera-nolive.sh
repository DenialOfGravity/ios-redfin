#!/bin/bash
# Camera.app's Live Photo off by default (and kept off), or back as it was. Live Photo (Iris) keeps mediaserverd's
# movie side running and is where its large allocations pile up; with it off a photograph is one frame.
# After Yofuness/ios-flame tools/sunfish-camera-prefs.sh (MIT): CAMUserPreferenceDesiredIrisMode 0 = off,
# CAMUserPreferencePreserveLivePhoto true so Camera keeps that choice.
#
#   tools/redfin-camera-nolive.sh status    the flag and the two prefs
#   tools/redfin-camera-nolive.sh on        Live Photo off: flag /var/local/camera-nolive (pa put, a temp override) and the
#                                           prefs written (the plist kept once as .bak-nolive); Camera and cfprefsd restarted
#   tools/redfin-camera-nolive.sh off       the prefs back from .bak-nolive, the flag reverted (pa revert)
#
# The prefs plist is outside the manifest's roots: the flag file under /var/local is the recorded state (pa snap
# lists it as a temp override); /var/local/camera-nolive says the prefs were written by this.
set -euo pipefail
P=/var/mobile/Library/Preferences/com.apple.camera.plist
FLAG=/var/local/camera-nolive
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
restart='c=$(/bin/ps ax | grep "[/]Camera.app/Camera" | sed "s/^ *//" | cut -d" " -f1); [ -n "$c" ] && kill -9 $c;
p=$(/bin/ps ax | grep "[c]fprefsd" | sed "s/^ *//" | cut -d" " -f1); for x in $p; do kill $x; done; true'

prefs() {
	pa get "$P" "$W/cam.plist" >/dev/null
	python3 -I - "$W/cam.plist" "$@" <<'PY'
import plistlib, sys
path = sys.argv[1]
p = plistlib.load(open(path, 'rb'))
if len(sys.argv) > 2:
    p['CAMUserPreferenceDesiredIrisMode'] = 0
    p['CAMUserPreferencePreserveLivePhoto'] = True
    plistlib.dump(p, open(path, 'wb'), fmt=plistlib.FMT_BINARY)
print('CAMUserPreferenceDesiredIrisMode', p.get('CAMUserPreferenceDesiredIrisMode', '(not set: Camera default, on)'),
      'CAMUserPreferencePreserveLivePhoto', p.get('CAMUserPreferencePreserveLivePhoto', '(not set)'))
PY
}

case "${1:-status}" in
status)
	pa sh "ls -la $FLAG 2>/dev/null || echo 'no $FLAG'"
	prefs
	;;
on)
	echo 1 > "$W/flag"
	pa put "$W/flag" "$FLAG" 644 --kg "camera: Live Photo off (tools/redfin-camera-nolive.sh)"
	prefs set
	pa files put "$W/cam.plist" "$P.nolive-new" >/dev/null
	pa sh "[ -e $P.bak-nolive ] || cp $P $P.bak-nolive; mv $P.nolive-new $P; chown mobile:mobile $P; chmod 600 $P; $restart"
	echo "Live Photo off (Camera reopened next time with it off)"
	;;
off)
	pa sh "[ -e $P.bak-nolive ] && cp $P.bak-nolive $P && chown mobile:mobile $P; $restart"
	pa revert "file:$FLAG" || true
	echo "Live Photo prefs back as before 'on'"
	;;
*)
	sed -n '2,13p' "$0"
	exit 2
	;;
esac
