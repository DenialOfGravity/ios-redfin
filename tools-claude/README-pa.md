# pa (tools-claude/pa)

The phone agent as a direct command. `pa help` lists everything; `pa help ACTION` shows one action.
Full docs: project doc claude/redfin-pa-2026-11-16.md.

    pa status | pa snap | pa sh CMD | pa put LOCAL REMOTE [MODE] [--kg NOTE] | pa get REMOTE LOCAL
    pa klog [--tail N] [--grep RE] [--count RE] | pa lock | pa q ACTION | pa <any phone-agent action>

- Same code as phone-agent.sh (loaded with PA_LIB=1). Screen/boot actions share /tmp/pa-phone.lock with the agent;
  the rest run in parallel.
- cycle, exp, mon-start/stop, reload, attach, net, net-use, build-apt go through the running agent's queue.
- `pa put` = `state put --temp`: recorded test install (.st-new, sha check, old -> .st-prev once, mv), managed roots
  only. `pa revert file:PATH` undoes it, `pa keep KEY --manifest FILE` makes it permanent. `pa state ...` = state.py.
- Locks: /tmp/pa-phone.lock (screen/boot actions), /tmp/pa-state.lock (state apply/set/revert/put).
- The desktop relay drops a wsl__run reply after ~80 s: keep calls short or use start_job.
- `pa wake` (final 2026-11-19): no input within 15 s of a backboardd start (its pid's line in the hm starts file;
  PA_WAKE_BBD_QUIET=N changes it), since an early touch leaves SpringBoard hanging the screen; after the wake it checks
  kern.sweet_fb_frames moves within 3 s of a no-op swipe (an idle screen draws nothing), else one real power off/on (key 0, pause, key 0) and checks again;
  "WAKE FAILED: frames still not moving" = the screen is stuck.
- `pa shot` reads through /var/local/bin/pmemsnap when it is installed (re-reads the black runs a ~1 s /dev/pmem read
  of a slot redrawn every frame produces; PA_SHOT_PLAIN=1 = the plain file-server read).
- `pa snap [--drift]` reads the sysctls listed in pa-snap.sysctls and shows temp overrides + the last state diff counts.
  Its first lines are the Metal driver per process: `FALLBACK: NAME -> SweetMetal2 for AGE: REASON` for each process
  the hakios-metal dispatcher tripped, or `metal: all on hm`.
- `pa fallback [clear PROC...|clear all|test PROC]` (ws-hm-fallback, 2026-11-19): the dispatcher's trip markers with
  their reasons (MARKER.why, /var/local/hmetal/trips/NAME.why), the hm build (via KNOWN-GOOD.txt), fallback.log.
  `clear` deletes the trip marker (the owner allows clearing to return to the newest driver) and restarts the process
  once on purpose. `test PROC` arms /var/local/hmetal/trip-test: the next start trips with reason "test".
  Deliberate restarts (`sweetctl kill NAME`, render, respring) are not counted toward a trip: sweetctl notes them in
  /var/local/hmetal/intent/ first (backboardd and SpringBoard also intent/_ui, for the processes that go down with them).
- Installed as ~/bin/pa (symlink). Backup of the agent before this change: phone-agent.sh.pre-pa.

## pa session: free-form UI session recorder + slow-moment finder (2026-11-17)

    pa session NAME [SECS=180] [--chunk 15] [--shots 3] [--pull auto|live|end] [--no-analyse] [--no-lock] [--no-rotate]
    pa session-stop                                   (or Ctrl-C in the session's terminal)
    python3 tools-claude/session-analyse.py DIR [--gap-ms 50] [--resp-ms 100] [--launch-ms 600] [--worst 15]

- Run it in a WSL terminal (it lasts SECS + pull + analysis; through wsl__run use start_job). Wake the phone yourself
  first: the session injects nothing, never wakes/sleeps the display, restarts nothing. It holds the phone lock and
  refuses while a cycle or another sweettrace runs. Use the phone between "RECORDING STARTED" and "STOPPED".
- Trace (tools-claude/pa-session.sh writes a small dash script to /var/tmp/pa-sess-NAME/; session.log and
  manifest.json "trace_mode" say which mode ran):
  - rotate (the phone's sweettrace says "rotate" in `sweettrace -V`, sweettrace 2 from branch ws-tools-sweettrace-rotate):
    ONE `sweettrace -t SECS -s CHUNK` for the whole session: one kdebug session, a new file every CHUNK s, no gap at all
    (file N ends at the time stamp where N+1 starts; the analyser reads them as one trace).
  - chunks (older sweettrace, or --no-rotate): one sweettrace per chunk; chunk N+1 is retried from 1 s before chunk N
    ends (absolute phone clock) and gets kdebug when N releases it, before N writes its file. The first loop (hands1,
    2026-11-17) lost 0.97-2.02 s per boundary: it counted 1 s sleeps whose process starts added ~70-140 ms each
    under trace load, so it started the next chunk after the previous one had ended ("tries 0" every time). Gaps are
    listed in manifest.json "gaps_ms".
  Same classes as pa trace (0x21/0xa6, 0x31), -m 8M events, ~9-22 MB/s (hands1: ~20 MB/s while busy). --pull auto = keep everything on the
  phone and pull after the stop when /private/var has room (no ssh copies during the session), else pull + delete each
  chunk while the next records. Pull ~30 MB/s (~60 s for 180 s).
- Screenshots: every --shots s the `pa shot` path (64 bytes of VIG0 registers, fb-shot.py --where, the scanout surface
  from /dev/pmem; read-only) -> shots/NNN.png + .thumb.jpg; PC times in shots.tsv, mapped to trace time through the
  ssh clock probe and each chunk's wall clock. UBWC-compressed scanout (xnu 1111 path) gives no image ("ubwc" rows).
- Kernel log: the sweetmon copy (logs/mon/klog-*.txt) cut to the window; the ring (`klog`) if sweetmon isn't running.
- Output logs/sessions/NAME-MMDD-HHMMSS/: chunks/, shots/, klog.txt, manifest.json, chain.log, session.log, then
  report.txt, session.json, report.html (frame-interval timeline per 30 s with finger-down bands, slow moments,
  thumbnails; worst-15 and gesture tables).
- Analysis: ~1.4 s per 100 MB of trace (180 s session ~25-35 s, ~1 GB RAM). Also works on one pa trace folder
  (writes to its session/ subfolder; --klog FILE for the boot's klog; a klog of another boot is detected and dropped).
- Reading the report: worst list = frame gaps > 50 ms inside a running animation (or while the finger drags) and
  touch -> first frame > 100 ms from a still screen (a tap with no frame while down counts from the up) and launches
  (the gesture right before the klog exec: up -> the first frame after the app's first CONTENT commit, a >= 4 KB
  layer tree; plus "ready", an ESTIMATE of launch -> interactable: the first 500 ms after that frame with the app's
  main thread >= 85 % idle and no >= 2 KB commit) > 600 ms, each with one cause + evidence (app main thread on CPU per cluster /
  runnable / blocked + wakers, commits, backboardd display thread, GPU busy and frame GPU ms, SwapWait, shader builds,
  CPU load, GOV/THERMAL, CRASH/exec). "app idle until a timer fired" and low-rate app animations (commits every 2+
  vsyncs: Apple's 20-30 fps fades) are listed separately and not counted; so are frame gaps across a trace gap
  (between chunk files: no data) and idle gaps (render server + app idle, no app commits, big cores < 30 %, the app
  not blocked on another thread: nothing was animating). Gaps that end with a new touch > 100 ms into them are
  skipped (that touch's response is measured instead).
