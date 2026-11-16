#!/usr/bin/env python3
"""Route-2 cellular-activation-ticket test: add a non-empty ActivationTicket to
mobileactivationd's data ark so CommCenter's isTicketAvailable gate flips true.

PC-side (WSL) generator. It never touches the phone. The operator pulls the live
data_ark.plist, runs this to produce a patched copy, backs up the original, then
`files put`s the patched copy and restarts mobileactivationd + CommCenter.

  # add the ticket (preserves every existing key + the binary plist format):
  python3 -I phone/cc/mk-data-ark.py add  data_ark.plist  data_ark.plist.ticket
  # inspect what a pulled ark currently holds:
  python3 -I phone/cc/mk-data-ark.py show data_ark.plist
  # build the undo copy (strip the key again, in case no .knowngood was kept):
  python3 -I phone/cc/mk-data-ark.py del  data_ark.plist.ticket data_ark.plist.clean

The payload (phone/cc/cc-ticket.b64) is a MINIMAL non-empty base64 string. It
decodes to 39 placeholder bytes and carries NO real provisioning data -- that is
expected for this first test. CommCenter's gate (sub_100842344) only base64-
decodes it and checks non-empty; it verifies no signature, no SEP, no keybag.

Key assumption (stated in phone/cc/README.md): the data-ark "-ActivationTicket"
directive surfaces as the record key "ActivationTicket" that CommCenter reads,
exactly as "-ActivationState" surfaces as "ActivationState". If mobileactivationd
namespaces it differently the symptom is simply isTicketAvailable staying false
-- a clean no-op, fully reversible, with no effect on the voice/SMS path (which
does not pass through this DataServiceController gate).
"""
import plistlib, sys

KEY = '-ActivationTicket'
# base64 placeholder, kept in one place next to this script so both match.
import os
TICKET = open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           'cc-ticket.b64')).read().strip()


def load(path):
    with open(path, 'rb') as f:
        return plistlib.load(f), _fmt(path)


def _fmt(path):
    with open(path, 'rb') as f:
        head = f.read(8)
    return plistlib.FMT_BINARY if head[:6] == b'bplist' else plistlib.FMT_XML


def dump(path, data, fmt):
    with open(path, 'wb') as f:
        plistlib.dump(data, f, fmt=fmt)


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    op, inp = argv[1], argv[2]
    data, fmt = load(inp)
    if op == 'show':
        for k in sorted(data):
            v = data[k]
            print(f'  {k} = {v!r}')
        print(f'  [format={"binary" if fmt==plistlib.FMT_BINARY else "xml"}, '
              f'ActivationTicket {"PRESENT" if KEY in data else "absent"}]')
        return 0
    out = argv[3]
    if op == 'add':
        data[KEY] = TICKET
        dump(out, data, fmt)
        print(f'wrote {out}: {KEY} = {TICKET!r} (+{len(data)} keys, '
              f'{"binary" if fmt==plistlib.FMT_BINARY else "xml"})')
        print('remember: chown mobile:mobile, chmod 600 after files put')
        return 0
    if op == 'del':
        data.pop(KEY, None)
        dump(out, data, fmt)
        print(f'wrote {out}: {KEY} removed ({len(data)} keys)')
        return 0
    print(f'unknown op {op!r}')
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
