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
- `pa snap [--drift]` reads the sysctls listed in pa-snap.sysctls and shows temp overrides + the last state diff counts.
- Installed as ~/bin/pa (symlink). Backup of the agent before this change: phone-agent.sh.pre-pa.
