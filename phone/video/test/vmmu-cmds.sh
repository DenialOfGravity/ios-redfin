#!/bin/sh
# venus-iommu: power on --cvp, boot --keep, init, then each argument as one venusctl command line (env words first,
# e.g. "VENUSCTL_VMMU=1 decode /var/local/tmp/vtest/t720.h264 /var/local/tmp/vmmu/o.nv12 --frames 10"), md5 of any
# /var/local/tmp/vmmu/*.nv12|*.264 it wrote, shutdown, power off --cvp. usage: vmmu-cmds.sh TAG "CMD" ...
V=${V:-/var/local/tmp/vmmu/venusctl}
D=/var/local/tmp/vmmu
TAG=$1
shift
L=$D/run-$TAG.log
S=$D/step-$TAG.log
up() { echo "[$(date +%s)] $*"; }
{
	up "vmmu-cmds $TAG start ($V)"
	$V --step-file $S --step power on --cvp || { up "power on failed"; exit 1; }
	$V --step-file $S --step boot --keep || { up "boot failed"; $V power off --cvp; exit 1; }
	$V --step-file $S --step init --phys 0xa2600000 || { up "init failed"; $V shutdown; $V power off --cvp; exit 1; }
	for c in "$@"; do
		up "== $c"
		set -- $c
		envs=""
		outs=""
		while [ $# -gt 0 ]; do case $1 in *=*) envs="$envs $1"; shift;; *) break;; esac; done
		for w in "$@"; do case $w in $D/*.nv12|$D/*.264) outs="$outs $w";; esac; done
		env VENUSCTL_RUSAGE=1 $envs $V ${STEP:+--step-file $S --step} "$@"
		up "rc $?"
		for f in $outs; do toybox md5sum $f; done
	done
	$V shutdown; up "shutdown rc $?"
	$V power off --cvp; up "power off rc $?"
	up "vmmu-cmds $TAG end"
} > $L 2>&1
