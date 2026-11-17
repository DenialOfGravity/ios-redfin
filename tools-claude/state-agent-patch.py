#!/usr/bin/env python3
"""state-agent-patch.py: add the `state ...` action (tools/state.py) to the phone agent.

  python3 tools-claude/state-agent-patch.py            write tools-claude/phone-agent.sh.state-new from the live
                                                       phone-agent.sh (the live file is not touched)
  python3 tools-claude/state-agent-patch.py --install  the same, then phone-agent.sh -> phone-agent.sh.pre-state and
                                                       the new version in its place; then send the agent `reload`
                                                       (only when no other agent is mid-request: check agent/inbox)
Idempotent: a phone-agent.sh that already has the action is left as it is.
"""
import os, shutil, subprocess, sys

D = os.path.dirname(os.path.abspath(__file__))
LIVE, NEW = os.path.join(D, 'phone-agent.sh'), os.path.join(D, 'phone-agent.sh.state-new')
DOC_ANCHOR = "#   cycle-stop                 stop at the next stage boundary (never inside fastboot boot); cycle-kill: at once\n"
DOC = """#
# Phone state (ws-tools-state: tools/state.py + state/redfin.toml in git; copy in tools-claude/state.py):
#   state diff [--json]        the phone vs the manifest: drift report (expected last line "state diff: no drift ...")
#   state apply [--dry-run] [--retire]   install exactly the drifted files/flags, set sysctls, restart only the named
#                              processes, re-dump and verify; never reboots or flashes (refused while a cycle runs)
#   state snapshot [LABEL]     dated dump + diff -> state/snapshots/ (+ the line for the status doc)
#   state dump PATH            one raw dump (phone/tools/sweetstate.sh over ssh stdin) -> PATH in the project folder
#   state set KEY VALUE --temp | state revert KEY|all   experiment overrides, recorded in state/temp-overrides.json
#   state summary | state index | state adopt --out PATH
#                              (manifest: state/manifest-ref.txt, e.g. git:origin/main:state/redfin.toml; --manifest X)
"""
CASE_ANCHOR = "\tcycle-status) cy_brief;;\n"
CASE = """\tstate)
\t\t[[ $rest =~ ^[A-Za-z0-9._=,:/@\\ +-]*$ && $rest != *..* ]] || { echo "bad arguments"; return 1; }
\t\tcase $rest in apply*|set*|revert*) cycle_running && { echo "a cycle is using the phone (cycle-status / cycle-stop)"; return 1; };; esac
\t\t# shellcheck disable=SC2086
\t\t(cd "$P" && PHONE_AGENT_INSIDE=1 PF=$P REPO=$REPO timeout 900 python3 -I "$P/tools-claude/state.py" $rest);;
"""

s = open(LIVE).read()
if '\tstate)\n' in s:
    print('phone-agent.sh already has the state action: nothing to do')
    sys.exit(0)
for a in (DOC_ANCHOR, CASE_ANCHOR):
    if a not in s:
        sys.exit(f'anchor not found in phone-agent.sh: {a.strip()!r} (merge by hand)')
s = s.replace(DOC_ANCHOR, DOC_ANCHOR + DOC, 1).replace(CASE_ANCHOR, CASE_ANCHOR + CASE, 1)
open(NEW, 'w').write(s)
if subprocess.run(['bash', '-n', NEW]).returncode:
    sys.exit('bash -n failed on the new version: not installed')
print(f'wrote {NEW}')
if '--install' in sys.argv:
    shutil.copy2(LIVE, LIVE + '.pre-state')
    shutil.copy2(NEW, LIVE)
    print('installed (previous: phone-agent.sh.pre-state); now send the agent a `reload` request')
