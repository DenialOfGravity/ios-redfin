#!/bin/sh
# venus-iommu phone check (<= 5 min): window path vs /dev/vmmu path, same firmware boot.
#   modes: win (VENUSCTL_VMMU=0), vmmu (buffers in ordinary memory), surf (vmmu + encoder inputs = IOSurfaces by id)
#   decode t720 10 frames (md5 vs 7ec42d77...) + 60 frames (speed, rusage); encode hd10.nv12 1920x1080 RC off 10 frames
#   (md5 across modes) + 300 frames looped (speed, rusage; surf: no CPU touch of the inputs after the first fill).
# usage: vmmu-run.sh TAG [MODES]   env passes through (VENUSCTL_*)
V=${V:-/var/local/tmp/vmmu/venusctl}
D=/var/local/tmp/vmmu
T=/var/local/tmp/vtest
E=/var/local/tmp/venc
TAG=$1
MODES=${2:-"win vmmu surf"}
N=${N:-300}
L=$D/run-$TAG.log
S=$D/step-$TAG.log
up() { echo "[$(date +%s)] $*"; }
{
	up "vmmu-run $TAG start ($V) modes $MODES"
	$V --step-file $S --step power on --cvp || { up "power on failed"; exit 1; }
	$V --step-file $S --step boot --keep || { up "boot failed"; $V power off --cvp; exit 1; }
	up "firmware loaded"
	$V --step-file $S --step init --phys 0xa2600000 || { up "init failed"; $V shutdown; $V power off --cvp; exit 1; }
	for m in $MODES; do
		case $m in
		win) X="VENUSCTL_VMMU=0"; F="";;
		vmmu) X="VENUSCTL_VMMU=1"; F="";;
		surf) X="VENUSCTL_VMMU=1 VENUSCTL_ENC_SURF=1 VENUSCTL_VMMU_LOG=1"; F="VENUSCTL_ENC_NOFILL=1";;
		esac
		if [ $m != surf ]; then
			up "== $m: decode 10 (md5)"
			env $X $V decode $T/t720.h264 $D/o-$TAG-$m.nv12 --frames 10; up "decode rc $?"
			toybox md5sum $D/o-$TAG-$m.nv12
			up "reference 7ec42d77fb41c1cab558686b8e454706"
			up "== $m: decode 60 (speed)"
			env VENUSCTL_RUSAGE=1 $X $V decode $T/t720.h264 $D/o60-$TAG-$m.nv12 --frames 60; up "decode60 rc $?"
			toybox md5sum $D/o60-$TAG-$m.nv12
		fi
		up "== $m: encode 1080p RC off 10 frames"
		env VENUSCTL_RUSAGE=1 $X $V encode $E/hd10.nv12 $D/e-$TAG-$m.264 1920x1080 --rc off --frames 10; up "encode rc $?"
		toybox md5sum $D/e-$TAG-$m.264
		up "== $m: encode 1080p RC off $N frames looped $F"
		env VENUSCTL_RUSAGE=1 VENUSCTL_ENC_LOOP=1 $F $X $V encode $E/hd10.nv12 $D/eN-$TAG-$m.264 1920x1080 --rc off \
			--frames $N; up "encodeN rc $?"
		ls -la $D/eN-$TAG-$m.264
	done
	$V shutdown; up "shutdown rc $?"
	$V power off --cvp; up "power off rc $?"
	up "vmmu-run $TAG end"
} > $L 2>&1
