#!/bin/sh
# venus-iommu run 3 (<= 5 min): decode into IOSurfaces (VENUSCTL_DEC_SURF=1), 1080p decode speed per path, and
# power collapse (suspend/resume) between vmmu sessions; md5 equal across paths = bit exact.
# usage: vmmu-run3.sh TAG
V=${V:-/var/local/tmp/vmmu/venusctl}
D=/var/local/tmp/vmmu
T=/var/local/tmp/vtest
TAG=$1
L=$D/run-$TAG.log
S=$D/step-$TAG.log
up() { echo "[$(date +%s)] $*"; }
dec() {	# MODE FILE FRAMES OUT [extra env]
	m=$1; f=$2; n=$3; o=$4; shift 4
	case $m in
	win) X="VENUSCTL_VMMU=0";;
	vmmu) X="VENUSCTL_VMMU=1";;
	surf) X="VENUSCTL_VMMU=1 VENUSCTL_DEC_SURF=1";;
	esac
	env VENUSCTL_RUSAGE=1 $X "$@" $V decode $f $o --frames $n; up "decode $m $f $n rc $?"
	[ -f $o ] && toybox md5sum $o
}
{
	up "vmmu-run3 $TAG start ($V)"
	$V --step-file $S --step power on --cvp || { up "power on failed"; exit 1; }
	$V --step-file $S --step boot --keep || { up "boot failed"; $V power off --cvp; exit 1; }
	$V --step-file $S --step init --phys 0xa2600000 || { up "init failed"; $V shutdown; $V power off --cvp; exit 1; }
	up "== 720p 10 frames into IOSurfaces (ref 7ec42d77fb41c1cab558686b8e454706)"
	dec surf $T/t720.h264 10 $D/o-$TAG-surf720.nv12
	up "== 1080p 120 frames per path"
	for m in win vmmu surf; do dec $m $T/t1080.h264 120 $D/o-$TAG-$m-1080.nv12; done
	up "== 1080p 300 frames into IOSurfaces, no CPU read (zero-copy speed)"
	dec surf $T/t1080.h264 300 $D/o-$TAG-surfnc-1080.nv12 VENUSCTL_NOCOPY=1
	up "== power collapse between vmmu sessions"
	for i in 1 2 3; do
		$V suspend; up "suspend $i rc $?"
		sleep 2
		$V resume; up "resume $i rc $?"
		dec vmmu $T/t720.h264 10 $D/o-$TAG-pc$i.nv12
	done
	$V shutdown; up "shutdown rc $?"
	$V power off --cvp; up "power off rc $?"
	up "vmmu-run3 $TAG end"
} > $L 2>&1
