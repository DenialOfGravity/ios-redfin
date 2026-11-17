#!/bin/bash
# pa-session.sh: free-form UI session recorder (called as `pa session ...`; also runs on its own).
#
#   pa session NAME [SECS] [--chunk S] [--shots S] [--pull auto|live|end] [--classes LIST] [--no-analyse]
#                          [--no-lock] [--min-free-mb N]
#   pa session-stop          stop a running session early (= Ctrl-C in its terminal)
#
# The owner uses the phone freely (taps, swipes, apps) while this records; nothing is injected, the display is not
# woken or slept, no process is restarted. SECS default 180 (max 900).
#
# What it records (everything in logs/sessions/NAME-MMDD-HHMMSS/):
#   chunks/cNNN.swtr  sweettrace, back to back: chunk N+1 starts the moment chunk N gives up kdebug (KDREMOVE, before
#                     N writes its file), so the gaps are ~0.1-0.3 s (measured: manifest.json "gaps_ms"). Each chunk
#                     is `sweettrace -t CHUNK -m 8388608 -n -c CLASSES` (CLASSES default 0x21/0xa6,0x31 as pa trace;
#                     kernel buffer 64 MB read every 100 ms). ~9-13 MB/s of trace on the phone: 180 s ~ 1.6-2.3 GB.
#   --pull auto       (default) end if /private/var has room for all chunks + --min-free-mb (default 1500) to spare,
#                     else live. end: everything stays on the phone until the session stops (no ssh copies loading the
#                     CPU while you use it), then pulled (~30 MB/s: ~60 s for 180 s). live: each chunk is pulled
#                     and deleted from the phone as soon as it is written (while the next one records).
#   shots/NNN.png    a screenshot every --shots seconds (default 3; 0 = none), the `pa shot` path (VIG0 registers ->
#                     fb-shot.py --where -> the scanout surface from /dev/pmem, read-only) + NNN.thumb.jpg; the PC
#                     times before/after each read are in shots.tsv (mapped to trace time by the analyser).
#   klog.txt          the kernel log for the window: the sweetmon copy (logs/mon/klog-*.txt) cut to the session, or
#                     the ring (`klog`) when sweetmon isn't running.
#   manifest.json     chunks (start/end mach time, events, wrapped/dropped), gaps, the phone wall clock <-> mach time
#                     (from each chunk header), the PC <-> phone clock offset (ssh probe), shots, args.
#   chain.log         the phone-side chain's log (chunk start/done lines).
# Then runs tools-claude/session-analyse.py on it (--no-analyse: skip; run it later by hand).
#
# Safety: takes the phone lock (/tmp/pa-phone.lock) for the whole session so no cycle/poke runs into it (--no-lock:
# don't); refuses while a cycle runs; checks free space first; on the phone it only writes /var/tmp/pa-sess-NAME/
# (deleted after a complete pull). Stop: `pa session-stop`, Ctrl-C, or the time running out.
set -uo pipefail
P=/mnt/d/ClaudeProjects/iOS_Pixel5
T=$P/tools-claude
PA_LIB=1 source "$T/phone-agent.sh" || { echo "session: cannot load phone-agent.sh"; exit 2; }
set +e
PT=/var/local/bin/toybox PTF=/var/local/bin/toybox-full ST=/var/local/bin/sweettrace DASH=/var/local/bin/dash
STOPF=/tmp/pa-session.stop

usage() { sed -n '3,6p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
name= secs=180 chunk=15 shots=3 pull=auto classes='0x21/0xa6,0x31' analyse=1 lock=1 minfree=1500
while [ $# -gt 0 ]; do
	case $1 in
	--chunk) chunk=$2; shift 2;; --shots) shots=$2; shift 2;; --pull) pull=$2; shift 2;;
	--classes) classes=$2; shift 2;; --no-analyse) analyse=; shift;; --no-lock) lock=; shift;;
	--min-free-mb) minfree=$2; shift 2;; -h|--help) usage;;
	-*) echo "unknown option $1"; usage;;
	*) if [ -z "$name" ]; then name=$1; else secs=$1; fi; shift;;
	esac
done
[[ $name =~ ^[A-Za-z0-9._-]+$ && $secs =~ ^[0-9]+$ && $secs -ge 5 && $secs -le 900 ]] || { echo "NAME [A-Za-z0-9._-], SECS 5..900"; usage; }
[[ $chunk =~ ^[0-9]+$ && $chunk -ge 5 && $chunk -le 30 ]] || { echo "--chunk 5..30"; exit 2; }
[[ $shots =~ ^[0-9.]+$ ]] || { echo "--shots SECONDS (0 = none)"; exit 2; }
[[ $pull =~ ^(auto|live|end)$ ]] || { echo "--pull auto|live|end"; exit 2; }
[[ $classes =~ ^[0-9a-fA-Fx/,]*$ ]] || { echo "--classes: like 0x21/0xa6,0x31"; exit 2; }

ssh_to() { local h=$1; shift; timeout "${PA_SSH_TIMEOUT:-300}" ssh -i ~/.ssh/redfin-claude -o StrictHostKeyChecking=no \
	-o UserKnownHostsFile=/dev/null -o ConnectTimeout=10 -o LogLevel=ERROR -o BatchMode=yes "root@$h" "$@"; }
now() { date +%s.%N; }
hms() { date +%T; }
[ -n "${PA_SESSION_TEST:-}" ] && source "$PA_SESSION_TEST"	# offline test: local mocks for ssh_to/run/sweettrace

cycle_running && { echo "session: a cycle is using the phone (pa cycle-status)"; exit 1; }
if [ -n "$lock" ]; then
	exec 9>>/tmp/pa-phone.lock
	flock -n 9 || { echo "session: the phone is busy: $(cat /tmp/pa-phone.owner 2>/dev/null) (wait, or --no-lock)"; exit 75; }
	echo "pid $$ 'pa session $name' since $(date +%T)" > /tmp/pa-phone.owner
	export PA_HAVE_LOCK=1
fi
H=192.168.7.2
phone_up || { echo "session: the phone does not answer on USB ($H)"; exit 1; }
ssh_to $H "test -x $ST" || { echo "session: no $ST on the phone"; exit 1; }
ssh_to $H "$PT pgrep -x sweettrace" >/dev/null 2>&1 && { echo "session: a sweettrace runs on the phone already (pa trace?)"; exit 1; }
old=$(ssh_to $H "$PT ls /var/tmp/pa-sess-$name 2>/dev/null | $PT grep -c 'swtr\$'" </dev/null | tr -d ' \r')
[ "${old:-0}" -gt 0 ] && { echo "session: /var/tmp/pa-sess-$name holds $old chunk(s) of an earlier session not pulled yet: pull them (pa get) or use another NAME"; exit 1; }

dir=$P/logs/sessions/$name-$(date +%m%d-%H%M%S)
mkdir -p "$dir/chunks" "$dir/shots"
D=/var/tmp/pa-sess-$name
rm -f "$STOPF"
log() { echo "[$(hms)] $*" | tee -a "$dir/session.log"; }

# ---- free space, pull mode
free_kb=$(ssh_to $H "$PT df -k /private/var | $PT tail -1" | awk '{print $4}')
need_mb=$(( secs * 14 ))	# 14 MB/s: above the busiest rate seen (~13 MB/s), so a busy session still fits
free_mb=$(( ${free_kb:-0} / 1024 ))
if [ "$pull" = auto ]; then
	if [ "$free_mb" -ge $(( need_mb + minfree )) ]; then pull=end; else pull=live; fi
fi
if [ "$free_mb" -lt $(( chunk * 14 * 3 + minfree )) ]; then
	log "session: only $free_mb MB free on /private/var (need $(( chunk * 14 * 3 + minfree )) MB even for live pulls)"; exit 1
fi
log "session $name: ${secs}s, chunks ${chunk}s, shots every ${shots}s, pull $pull (phone free $free_mb MB, ~$need_mb MB needed), classes $classes -> ${dir#$P/}/"

# ---- display state (iOS's own last change, from the sweetmon klog copy: no phone command)
monklog=$(ls -t "$P"/logs/mon/klog-*.txt 2>/dev/null | head -1)
dl=$(grep -aE 'SWEET FB: display (on|off)$' "$monklog" 2>/dev/null | tail -1)
case $dl in *"display on") log "display: on (klog: ${dl:0:12})";; *"display off") log "WARNING: iOS display is OFF (klog ${dl:0:12}): wake the phone yourself; the session does not";; *) log "display state unknown (no sweetmon klog line)";; esac
mon_age=$(( $(date +%s) - $(stat -c %Y "$monklog" 2>/dev/null || echo 0) ))
[ "$mon_age" -gt 30 ] && log "note: sweetmon's klog copy is ${mon_age}s old (collector not running?): the kernel ring is read at the end instead"

# ---- PC <-> phone clock (3 ssh probes, the one with the smallest round trip)
: > "$dir/clock.tsv"
for i in 1 2 3; do
	a=$(now); p=$(ssh_to $H "$PTF date +%s.%N 2>/dev/null || $PT date +%s"); b=$(now)
	echo -e "$a\t$p\t$b" >> "$dir/clock.tsv"
done

# ---- the phone-side chain (dash): chunk after chunk, each started the moment the previous one releases kdebug
cat > "$dir/chain.sh" <<EOF
D=$D; ST=$ST; PT=$PT; SEG=$chunk; TOTAL=$secs; CLS='$classes'
trap '' HUP
log() { echo "\$*" >> \$D/chain.log; }
now() { $PTF date +%s.%N 2>/dev/null || \$PT date +%s; }
sec() { \$PT date +%s; }
T_END=\$(( \$(sec) + TOTAL ))
# start chunk \$1 for \$2 s; while the previous chunk still owns kdebug, sweettrace exits at once (EBUSY): retry
start_chunk() {
	f=\$D/c\$(printf %03d \$1).swtr; n=0
	while [ \$n -lt 600 ]; do
		\$ST -t \$2 -m 8388608 -n -c \$CLS -o \$f > \$f.out 2>&1 &
		pid=\$!; k=0
		while [ \$k -lt 60 ]; do
			\$PT grep -q 'kernel buffer' \$f.out 2>/dev/null && { log "CHUNK \$1 START \$(now) pid \$pid tries \$n secs \$2"; return 0; }
			kill -0 \$pid 2>/dev/null || break
			\$PT sleep 0.005; k=\$((k+1))
		done
		kill -0 \$pid 2>/dev/null && { log "CHUNK \$1 START \$(now) pid \$pid tries \$n secs \$2 (late start line)"; return 0; }
		n=\$((n+1)); \$PT sleep 0.005
	done
	log "CHUNK \$1 FAILED \$(\$PT tr '\n' ' ' < \$f.out)"; return 1
}
done_check() {	# log DONE for the previous chunk once its process has written the file and exited
	[ -n "\$prev" ] || return 0
	kill -0 \$prev 2>/dev/null && return 0
	log "CHUNK \$prevn DONE \$(\$PT stat -c %s \$D/c\$(printf %03d \$prevn).swtr 2>/dev/null) \$(\$PT tail -1 \$D/c\$(printf %03d \$prevn).swtr.out | \$PT tr -d '\n')"
	prev=; prevn=
}
log "CHAIN BEGIN \$(now) seg \$SEG total \$TOTAL"
i=1; prev=; prevn=
dur=\$SEG; [ \$dur -gt \$TOTAL ] && dur=\$TOTAL
start_chunk 1 \$dur || { log "END \$(now) failed"; exit 1; }
cur=\$pid
while :; do
	# sleep to ~0.3 s before the current chunk ends (1 s steps: the stop file is seen within a second)
	s=0; stopped=
	while [ \$s -lt \$((dur - 1)) ]; do
		[ -f \$D/stop ] && { stopped=1; break; }
		\$PT sleep 1; s=\$((s + 1)); done_check
	done
	[ -n "\$stopped" ] || \$PT sleep 0.6
	left=\$(( T_END - \$(sec) ))
	if [ -n "\$stopped" ] || [ -f \$D/stop ] || [ \$left -le 1 ]; then
		[ -n "\$stopped" ] || [ -f \$D/stop ] && { kill -TERM \$cur 2>/dev/null; log "STOP \$(now) (stop file)"; }
		while kill -0 \$cur 2>/dev/null; do \$PT sleep 0.2; done_check; done
		while [ -n "\$prev" ]; do \$PT sleep 0.2; done_check; done
		prev=\$cur; prevn=\$i; done_check
		break
	fi
	# next chunk: tries from now on until the current one releases kdebug (~0.3 s of EBUSY retries)
	n2=\$SEG; [ \$n2 -gt \$((left - 1)) ] && n2=\$((left - 1))
	while [ -n "\$prev" ]; do \$PT sleep 0.2; done_check; done
	prev=\$cur; prevn=\$i
	i=\$((i + 1))
	start_chunk \$i \$n2 || { while [ -n "\$prev" ]; do \$PT sleep 0.2; done_check; done; log "END \$(now) failed"; exit 1; }
	cur=\$pid; dur=\$n2
	done_check
done
log "END \$(now)"
EOF

ssh_to $H "$PT rm -rf $D; $PT mkdir -p $D && $PT cat > $D/chain.sh" < "$dir/chain.sh" || { log "session: could not write the chain script"; exit 1; }
# the chain's log, streamed (one ssh session for the whole recording)
PA_SSH_TIMEOUT=$(( secs + 900 )) ssh_to $H "$PT touch $D/chain.log; $PT tail -n +1 -f $D/chain.log" </dev/null > "$dir/chain.log" 2>/dev/null &
tailpid=$!
ssh_to $H "($DASH $D/chain.sh </dev/null >$D/chain.err 2>&1 &)" </dev/null
t_launch=$(now)

stop_phone() { ssh_to $H "$PT touch $D/stop" </dev/null >/dev/null 2>&1; }
interrupted=
trap 'interrupted=1' INT TERM

# ---- wait for the first chunk
for i in $(seq 100); do grep -q '^CHUNK 1 START' "$dir/chain.log" && break; grep -qE 'FAILED|^END' "$dir/chain.log" && break; sleep 0.1; done
if ! grep -q '^CHUNK 1 START' "$dir/chain.log"; then
	log "session: the first chunk did not start:"; cat "$dir/chain.log"; ssh_to $H "$PT cat $D/c001.swtr.out $D/chain.err" 2>/dev/null | head -5
	kill $tailpid 2>/dev/null; exit 1
fi
t_start=$(now)
echo "================================================================"
log "RECORDING STARTED $(date '+%F %T') (for ${secs}s, until ~$(date -d "@$(( ${t_start%.*} + secs ))" +%T)); use the phone now. Stop early: pa session-stop"
echo "================================================================"

# ---- screenshots (background loop) + live pulls (foreground loop)
shot_loop() {
	local i=0 next step t0 t1 regs raw w kind pa_ len geo
	next=$(date +%s%3N); step=$(python3 -c "print(int($shots * 1000))")
	while [ ! -f "$dir/.recording-done" ]; do
		i=$((i + 1))
		regs=$dir/shots/.regs-$i.bin raw=$dir/shots/.raw-$i.bin
		t0=$(now)
		if run "files read /dev/pmem 0x0ae05000 64 $regs" >/dev/null 2>&1; then
			w=$(python3 "$T/fb-shot.py" --where "$regs" $monklog 2>&1)
			read -r kind pa_ len geo _ <<<"$w"
			case $kind in
			direct|blit) run "files read /dev/pmem $pa_ $len $raw" >/dev/null 2>&1 || kind=readfail;;
			esac
		else kind=regfail; fi
		t1=$(now)
		printf '%d\t%s\t%s\t%s\t%s\n' "$i" "$t0" "$t1" "$kind" "${geo:--}" >> "$dir/shots.tsv"
		if [[ $kind == direct || $kind == blit ]]; then
			( local png; png=$dir/shots/$(printf %03d $i).png
			  if [ "$kind" = direct ]; then python3 "$T/fb-shot.py" "$raw" "$png" 2 "$geo" >/dev/null 2>&1; else python3 "$T/fb-shot.py" "$raw" "$png" 2 >/dev/null 2>&1; fi
			  python3 - "$png" "${png%.png}.thumb.jpg" <<'PYEOF' 2>/dev/null
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB'); im.thumbnail((240, 520)); im.save(sys.argv[2], quality=65)
PYEOF
			  rm -f "$raw" ) &
		fi
		rm -f "$regs"
		next=$(( next + step )); [ "$next" -lt "$(date +%s%3N)" ] && next=$(date +%s%3N)
		while [ ! -f "$dir/.recording-done" ] && [ "$(date +%s%3N)" -lt "$next" ]; do sleep 0.1; done
	done
	wait
}
if [ "$shots" != 0 ]; then shot_loop & shotpid=$!; else shotpid=; fi

pulled=" "
pull_chunk() {	# N: pull cNNN.swtr (+ .out), check the size, delete it on the phone
	local n=$1 f sz got; f=c$(printf %03d "$n").swtr
	sz=$(grep -m1 "^CHUNK $n DONE" "$dir/chain.log" | awk '{print $4}')
	run "pull $D/$f ${dir#$P/}/chunks/$f" | tail -1 | sed "s/^/  chunk $n: /" | tee -a "$dir/session.log"
	got=$(stat -c %s "$dir/chunks/$f" 2>/dev/null || echo 0)
	ssh_to $H "$PT cat $D/$f.out" > "$dir/chunks/$f.out" 2>/dev/null
	if [ -n "$sz" ] && [ "$got" = "$sz" ]; then
		ssh_to $H "$PT rm -f $D/$f" </dev/null; pulled="$pulled$n "
	else log "chunk $n: pulled $got of ${sz:-?} bytes: left on the phone ($D/$f)"; fi
}

deadline=$(( ${t_start%.*} + secs + 60 ))
stop_sent=
while :; do
	if [ -z "$stop_sent" ] && { [ -f "$STOPF" ] || [ -n "$interrupted" ]; }; then
		log "stop requested: stopping the current chunk"; stop_phone; stop_sent=1
	fi
	grep -q '^END' "$dir/chain.log" && break
	[ "$(date +%s)" -gt "$deadline" ] && { log "session: no END from the phone chain 60 s after the planned end: stopping it"; stop_phone; stop_sent=1; deadline=$(( deadline + 30 )); }
	if [ "$pull" = live ]; then
		for n in $(grep '^CHUNK [0-9]* DONE' "$dir/chain.log" | awk '{print $2}'); do
			[[ $pulled == *" $n "* ]] || pull_chunk "$n"
		done
	fi
	sleep 0.5
done
t_stop=$(now)
touch "$dir/.recording-done"
echo "================================================================"
log "STOPPED $(date '+%F %T') ($(python3 -c "print(f'{$t_stop-$t_start:.0f}')") s); you can stop using the phone. Pulling and analysing..."
echo "================================================================"
grep -E 'FAILED|STOP|END' "$dir/chain.log" | sed 's/^/  phone: /'
kill $tailpid 2>/dev/null
ssh_to $H "$PT cat $D/chain.log" > "$dir/chain.log" 2>/dev/null

# ---- pull what is left
tp0=$(now)
for n in $(grep '^CHUNK [0-9]* DONE' "$dir/chain.log" | awk '{print $2}'); do
	[[ $pulled == *" $n "* ]] || pull_chunk "$n"
done
[ -n "$shotpid" ] && wait "$shotpid" 2>/dev/null
nleft=$(ssh_to $H "$PT ls $D/ | $PT grep -c 'swtr$'" | tr -d ' \r')
if [ "${nleft:-1}" = 0 ]; then ssh_to $H "$PT rm -rf $D" </dev/null; else log "left on the phone: $D ($nleft chunk(s) not pulled completely)"; fi
log "pulled in $(python3 -c "print(f'{$(now)-$tp0:.0f}')") s"
[ -n "$lock" ] && { flock -u 9; exec 9>&-; }

# ---- kernel log for the window
python3 - "$dir" "$monklog" <<'PYEOF'
import glob, os, re, struct, sys
d, mon = sys.argv[1], sys.argv[2]
HDR = struct.Struct('<8sIIIIQQQIIIIIIiIQ128s')
lo = hi = None
for f in sorted(glob.glob(os.path.join(d, 'chunks', 'c*.swtr'))):
    h = HDR.unpack_from(open(f, 'rb').read(HDR.size), 0)
    s, e = h[5] * h[3] / h[4] / 1e9, h[6] * h[3] / h[4] / 1e9
    lo = s if lo is None else min(lo, s); hi = e if hi is None else max(hi, e)
n = 0
if lo is not None and mon and os.path.exists(mon):
    rx = re.compile(rb'^\[ *([0-9]+\.[0-9]+)\]')
    with open(os.path.join(d, 'klog.txt'), 'wb') as o:
        last = None
        for line in open(mon, 'rb'):
            m = rx.match(line)
            if m:
                last = float(m.group(1))
            if last is not None and lo - 10 <= last <= hi + 10:
                o.write(line); n += 1
print(f'klog: {n} lines from {os.path.basename(mon) if mon else "-"} for {lo} .. {hi} s' if lo is not None else 'klog: no chunks')
open(os.path.join(d, '.klog-lines'), 'w').write(str(n))
PYEOF
if [ "$(cat "$dir/.klog-lines" 2>/dev/null || echo 0)" -lt 10 ]; then
	kpa=
	lb=$(cat "$P/agent/last-boot.txt" 2>/dev/null | tr -d '\r\n ')
	kd=$(sed -n 's/^kernel *\(builds\/xnu\/[^/ ]*\)\/.*/\1/p' "$P/$lb/README.txt" 2>/dev/null | head -1)
	[ -n "$kd" ] && kpa=$(grep -o -m1 '0x[0-9a-fA-F]*' "$P/$kd/KLOG_PA.txt" 2>/dev/null | head -1)
	if [ -n "$kpa" ] && run "klog $kpa" >/dev/null 2>&1; then cp "$P/logs/klog.txt" "$dir/klog.txt"; log "klog: the kernel ring ($(wc -l < "$dir/klog.txt") lines)"
	else log "klog: none (no sweetmon copy and no KLOG_PA)"; fi
fi
rm -f "$dir/.klog-lines" "$dir/.recording-done" "$STOPF"

# ---- manifest
python3 - "$dir" "$name" "$secs" "$chunk" "$shots" "$pull" "$classes" "$t_launch" "$t_start" "$t_stop" "$free_mb" <<'PYEOF'
import glob, json, os, struct, sys
d, name, secs, chunk, shots, pull, classes, t_launch, t_start, t_stop, free_mb = sys.argv[1:12]
HDR = struct.Struct('<8sIIIIQQQIIIIIIiIQ128s')
chunks = []
for f in sorted(glob.glob(os.path.join(d, 'chunks', 'c*.swtr'))):
    h = HDR.unpack_from(open(f, 'rb').read(HDR.size), 0)
    numer, denom = h[3], h[4]
    chunks.append({'file': os.path.relpath(f, d), 'bytes': os.path.getsize(f), 'start_abs': h[5], 'end_abs': h[6],
                   'start_s': h[5] * numer / denom / 1e9, 'end_s': h[6] * numer / denom / 1e9, 'events': h[7],
                   'threads': h[8], 'wrapped': bool(h[10] & 8), 'dropped': h[13], 'wall_ns': h[16],
                   'numer': numer, 'denom': denom, 'args': h[17].split(b'\0')[0].decode()})
gaps = [round((b['start_s'] - a['end_s']) * 1000, 1) for a, b in zip(chunks, chunks[1:])]
clock = []
for line in open(os.path.join(d, 'clock.tsv')):
    try:
        a, p, b = (float(x) for x in line.split('\t'))
        clock.append({'pc_before': a, 'phone': p, 'pc_after': b, 'rtt_ms': (b - a) * 1000, 'offset_s': p - (a + b) / 2})
    except ValueError:
        pass
best = min(clock, key=lambda c: c['rtt_ms']) if clock else None
shotl = []
if os.path.exists(os.path.join(d, 'shots.tsv')):
    for line in open(os.path.join(d, 'shots.tsv')):
        f = line.rstrip('\n').split('\t')
        if len(f) >= 5:
            png = os.path.join('shots', '%03d.png' % int(f[0]))
            shotl.append({'n': int(f[0]), 'pc_t0': float(f[1]), 'pc_t1': float(f[2]), 'kind': f[3], 'geo': f[4],
                          'png': png if os.path.exists(os.path.join(d, png)) else None})
m = {'name': name, 'secs': int(secs), 'chunk_s': int(chunk), 'shots_every_s': float(shots), 'pull': pull,
     'classes': classes, 'pc_launch': float(t_launch), 'pc_start': float(t_start), 'pc_stop': float(t_stop),
     'phone_free_mb_before': int(free_mb), 'chunks': chunks, 'gaps_ms': gaps,
     'clock': {'probes': clock, 'phone_minus_pc_s': best['offset_s'] if best and best['rtt_ms'] < 500 else None},
     'shots': shotl,
     'notes': 'trace time = mach seconds (klog [ s ] uses the same clock); phone wall = chunk wall_ns at start_abs; '
              'PC wall + phone_minus_pc_s = phone wall'}
json.dump(m, open(os.path.join(d, 'manifest.json'), 'w'), indent=1)
tot = sum(c['bytes'] for c in chunks) / 1e6
span = (chunks[-1]['end_s'] - chunks[0]['start_s']) if chunks else 0
print(f"manifest: {len(chunks)} chunks, {tot:.0f} MB, {span:.1f} s of trace, gaps ms {gaps}, "
      f"wrapped {sum(c['wrapped'] for c in chunks)}, dropped {sum(c['dropped'] for c in chunks)}, {len(shotl)} shots "
      f"({sum(1 for s in shotl if s['png'])} images), clock offset {m['clock']['phone_minus_pc_s']}")
PYEOF

if [ -n "$analyse" ]; then
	ta=$(now)
	python3 "$T/session-analyse.py" "$dir" | tail -40
	log "analysis took $(python3 -c "print(f'{$(now)-$ta:.0f}')") s -> ${dir#$P/}/report.txt, report.html"
else
	log "analyse later: python3 tools-claude/session-analyse.py ${dir#$P/}"
fi
