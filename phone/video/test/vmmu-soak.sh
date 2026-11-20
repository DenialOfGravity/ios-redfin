#!/bin/sh
# venus-iommu short stability check (~3 min, real load) + 4K: one firmware boot; ROUNDS x (1080p zero-copy encode
# 300 frames from IOSurfaces, 1080p zero-copy decode 300 frames into IOSurfaces, 720p vmmu decode 10 frames md5,
# suspend 1 s resume); then 4K (3840x2160) encode RC off 60 frames looped from IOSurfaces.
# usage: vmmu-soak.sh TAG [ROUNDS]
V=${V:-/var/local/tmp/vmmu/venusctl}
D=/var/local/tmp/vmmu
T=/var/local/tmp/vtest
E=/var/local/tmp/venc
TAG=$1
R=${2:-6}
L=$D/run-$TAG.log
up() { echo "[$(date +%s)] $*"; }
{
	up "vmmu-soak $TAG start ($V) rounds $R"
	$V power on --cvp || { up "power on failed"; exit 1; }
	$V boot --keep || { up "boot failed"; $V power off --cvp; exit 1; }
	$V init --phys 0xa2600000 || { up "init failed"; $V shutdown; $V power off --cvp; exit 1; }
	i=1
	while [ $i -le $R ]; do
		env VENUSCTL_RUSAGE=1 VENUSCTL_VMMU=1 VENUSCTL_ENC_SURF=1 VENUSCTL_ENC_NOFILL=1 VENUSCTL_ENC_LOOP=1 \
			$V encode $E/hd10.nv12 $D/s-$TAG.264 1920x1080 --rc off --frames 300; up "round $i encode rc $?"
		env VENUSCTL_RUSAGE=1 VENUSCTL_VMMU=1 VENUSCTL_DEC_SURF=1 VENUSCTL_NOCOPY=1 \
			$V decode $T/t1080.h264 $D/sd-$TAG.nv12 --frames 300; up "round $i decode rc $?"
		env VENUSCTL_VMMU=1 $V decode $T/t720.h264 $D/s7-$TAG.nv12 --frames 10; up "round $i decode720 rc $?"
		toybox md5sum $D/s7-$TAG.nv12
		$V suspend; up "round $i suspend rc $?"
		sleep 1
		$V resume; up "round $i resume rc $?"
		i=$((i + 1))
	done
	up "== 4K encode"
	env VENUSCTL_RUSAGE=1 VENUSCTL_VMMU=1 VENUSCTL_ENC_SURF=1 VENUSCTL_ENC_NOFILL=1 VENUSCTL_ENC_LOOP=1 \
		$V encode $E/uhd2.nv12 $D/u-$TAG.264 3840x2160 --rc off --bitrate 40000000 --frames 60; up "4K encode rc $?"
	ls -la $D/u-$TAG.264
	$V shutdown; up "shutdown rc $?"
	$V power off --cvp; up "power off rc $?"
	up "vmmu-soak $TAG end"
} > $L 2>&1
