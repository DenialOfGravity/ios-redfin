#!/bin/bash
# hmv7 (throwaway): compile CA's fixed_frag_lph_cpf A2Xghfc variant for the A620 on the host, with ir3's listing
P=/mnt/d/ClaudeProjects/iOS_Pixel5; O=$P/logs/hmetal/hmv7; BR=$HOME/redfin-build
exec > $O/sweep.log 2>&1
set -x
set -a; . $BR/work/hmesa/mesa/hmesa.env; set +a
D=$BR/hmv7
REF=${1:-ws-hm-var7}
if [ ! -d $D/.git ]; then git clone -q --depth 1 --filter=blob:none --no-checkout -b $REF https://github.com/DenialOfGravity/ios-redfin.git $D && git -C $D sparse-checkout set phone/hmetal && git -C $D checkout -q $REF; fi
git -C $D fetch -q --depth 1 origin $REF && git -C $D checkout -q -f FETCH_HEAD
git -C $D log --oneline -1
cd $D/phone/hmetal/upstream
make -s MESA=1 MESA_SRC=$MESA_SRC MESA_BUILD=$MESA_BUILD CXX=g++ out/host-mesa/sweep || { echo "result FAILED make"; exit 1; }
for f in A2Xghfc; do
  IR3_SHADER_DEBUG=disasm timeout 600 out/host-mesa/sweep -v --chip 0x06020000 $P/apple-ref/16.7-x/$f.metallib > $O/$f-a620.txt 2>&1; echo "$f rc $?"
done
echo "result ok"
