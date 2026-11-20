#!/bin/sh
# venus-cvp: one encode with the encoder persist as stock's CP non-pixel buffer (VENUSCTL_NP_BANK=1 unless the
# extra env says otherwise), firmware booted fresh, CVP powered. Step log fsynced (survives a reset); run log synced
# per line. usage: cvp-run.sh TAG IN WxH FRAMES RC BITRATE [ENV=VALUE ...]   (RC vbr|cbr|off; STEP=0: no step log
# for the encode itself)
V=${V:-/var/local/tmp/cvp/venusctl}
D=/var/local/tmp/cvp
TAG=$1; IN=$2; SZ=$3; N=$4; RC=$5; BR=$6
shift 6
L=$D/run-$TAG.log
S=$D/step-$TAG.log
O=$D/o-$TAG.264
up() { echo "[$(toybox date +%s)] $*"; sync; }
ST="--step-file $S --step"
[ "${STEP:-1}" = 0 ] && ST=""
{
	up "cvp-run $TAG start ($V) $IN $SZ $N rc $RC $BR env: $*"
	$V --step-file $S --step power on --cvp || { up "power on failed"; exit 1; }
	$V --step-file $S --step boot --keep || { up "boot failed"; $V power off --cvp; exit 1; }
	$V --step-file $S --step init --phys 0xa2600000 || { up "init failed"; $V shutdown; $V power off --cvp; exit 1; }
	up "== encode"
	env VENUSCTL_RUSAGE=1 VENUSCTL_VMMU=1 VENUSCTL_NP_BANK=1 "$@" $V $ST encode $IN $O $SZ --frames $N --rc $RC \
		--bitrate $BR --fps 30 --gop 30
	up "encode rc $?"
	toybox ls -l $O
	$V shutdown; up "shutdown rc $?"
	$V power off --cvp; up "power off rc $?"
	up "cvp-run $TAG end"
} > $L 2>&1
