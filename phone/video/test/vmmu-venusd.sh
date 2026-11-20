#!/bin/sh
# venus-iommu: venusd (by hand, needs /var/local/venus.on) with VENUSCTL_VMMU=$VMMU, venusc decodes: 720p 10 frames over
# the socket (md5 ref 7ec42d77...), 720p into credited IOSurfaces (--surf), 1080p 300 --null and --surf (speed).
# usage: vmmu-venusd.sh TAG   (env VMMU=0|1, default 1)
D=/var/local/tmp/vmmu
T=/var/local/tmp/vtest
TAG=$1
L=$D/run-$TAG.log
up() { echo "[$(date +%s)] $*"; }
{
	up "vmmu-venusd $TAG start VMMU=${VMMU:-1}"
	env VENUSCTL_VMMU=${VMMU:-1} $D/venusd --log $D/venusd-$TAG.log &
	P=$!
	up "venusd pid $P"
	sleep 8
	$D/venusc $T/t720.h264 $D/c1-$TAG.nv12 --frames 10 | toybox tail -n 3; up "venusc socket rc $?"
	toybox md5sum $D/c1-$TAG.nv12
	$D/venusc $T/t720.h264 $D/c2-$TAG.nv12 --frames 10 --surf | toybox tail -n 3; up "venusc surf rc $?"
	toybox md5sum $D/c2-$TAG.nv12
	up "reference 7ec42d77fb41c1cab558686b8e454706"
	$D/venusc $T/t1080.h264 --null --frames 300 | toybox tail -n 3; up "venusc 1080 null rc $?"
	$D/venusc $T/t1080.h264 $D/c3-$TAG.nv12 --frames 60 --surf | toybox tail -n 3; up "venusc 1080 surf rc $?"
	toybox md5sum $D/c3-$TAG.nv12
	sleep 3
	$D/venusc $T/t720.h264 $D/c4-$TAG.nv12 --frames 10 | toybox tail -n 3; up "venusc after collapse rc $?"
	toybox md5sum $D/c4-$TAG.nv12
	kill -TERM $P
	sleep 4
	up "vmmu-venusd $TAG end"
} > $L 2>&1
