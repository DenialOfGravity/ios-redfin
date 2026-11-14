#!/bin/sh
# (redfin hm-cache) mkseed.sh: a cache-seed directory for /var/local/hmetal/cache-seed, on the PC (WSL).
#
#   mkseed.sh compile CACHE SEED   every stage the phone's manifests name (CACHE = a pulled /var/local/hmetal/cache with
#                                  its src/ and bc/), compiled again by hm-seed under this build's key into SEED: the way
#                                  to seed a phone after HAKIOS_METAL_CACHE_FORMAT or the Mesa changed
#   mkseed.sh harvest CACHE SEED   the phone's own compiled stages (CACHE/*.stage) copied into SEED as they are: exact
#                                  (the phone's compiler made them), good for the key they were made under only
# Then SEED.tgz beside SEED for `files put`; on the phone it is unpacked to /var/local/hmetal/cache-seed (read-only for
# every process: m2shim's HAKIOS_METAL_CACHE_SEED is "/var/local/hmetal/cache:/var/local/hmetal/cache-seed").
# The bitcode in CACHE/bc is the phone's (Apple's QuartzCore functions among it): keep CACHE and SEED in the PC's project
# folder, never in git.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
MODE=${1:?compile or harvest}; CACHE=${2:?CACHE}; SEED=${3:?SEED}
mkdir -p "$SEED"
case $MODE in
compile)
	"$HERE/hm-seed" build "$CACHE" "$SEED" --chip 0x06020000 ;;
harvest)
	n=0; for f in "$CACHE"/*.stage; do [ -f "$f" ] || continue; cp "$f" "$SEED/"; n=$((n + 1)); done
	echo "mkseed: $n stages harvested into $SEED" ;;
*) echo "mkseed.sh compile|harvest CACHE SEED"; exit 2 ;;
esac
(cd "$SEED" && tar czf "../$(basename "$SEED").tgz" .)
echo "mkseed: $(ls "$SEED" | grep -c '\.stage$') stages, $(du -sh "$SEED" | cut -f1); $(dirname "$SEED")/$(basename "$SEED").tgz"
