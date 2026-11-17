#!/usr/bin/env python3
"""launchd-add-job.py: add (or replace) one LaunchDaemons job in a copy of the phone's launchd.plist.

  tools/launchd-add-job.py IN.plist JOB.plist OUT.plist [--remove-hook]

IN is a copy of /System/Library/xpc/launchd.plist pulled from the phone (Apple's file: never commit it or OUT to
git; keep them in the project folder, e.g. PF/state/files/). JOB is a job dictionary from phone/boot/ (XML plist,
key = its Label). The job is stored under the key /System/Library/LaunchDaemons/<Label>.plist like the other
org.iosonpixel jobs. Prints the sha256 of OUT for the manifest (state/redfin.toml [[file]] launchd.plist).
"""
import hashlib, plistlib, sys


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    src, job, dst = sys.argv[1:4]
    plist = plistlib.load(open(src, 'rb'))
    j = plistlib.load(open(job, 'rb'))
    label = j['Label']
    key = f'/System/Library/LaunchDaemons/{label}.plist'
    old = plist['LaunchDaemons'].get(key)
    plist['LaunchDaemons'][key] = j
    data = plistlib.dumps(plist, fmt=plistlib.FMT_BINARY)
    open(dst, 'wb').write(data)
    print(f'{"replaced" if old else "added"} {label} ({len(plist["LaunchDaemons"])} jobs) -> {dst}')
    print(f'sha256 {hashlib.sha256(data).hexdigest()}')


if __name__ == '__main__':
    main()
