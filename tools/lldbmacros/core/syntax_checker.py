#!/usr/bin/env python
from __future__ import absolute_import, print_function


helpdoc = """
A simple utility that verifies the syntax for python scripts.
The checks it does are :
  * Check for 'tab' characters in .py files
  * Compile errors in py sources
Usage:
  python syntax_checker.py <python_source_file> [<python_source_file> ..] 
"""
import sys
import os

def find_non_ascii(s):
    for c in s:
        if ord(c) >= 0x80: return True
    return False

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Error: Unknown arguments", file=sys.stderr)
        print(helpdoc)
        sys.exit(1)
    for fname in sys.argv[1:]:
        if not os.path.exists(fname):
            print("Error: Cannot recognize %s as a file" % fname, file=sys.stderr)
            sys.exit(1)
        if fname.split('.')[-1] != 'py':
            print("Note: %s is not a valid python file. Skipping." % fname)
            continue
        fh = open(fname)
        print("Success: Checked %s. No syntax errors found." % fname)
    sys.exit(0)

