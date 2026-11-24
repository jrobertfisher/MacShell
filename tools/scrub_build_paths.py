#!/usr/bin/env python3
"""Removes the build computer's home folder from paths that the toolchain's
C library leaves inside the application (in its error messages), so a
published MacShell.bin doesn't carry the builder's user name. The path is
replaced by one of the same length, so nothing else in the file moves.

Usage: scrub_build_paths.py file ...   (uses $HOME)
"""
import os, sys

home = os.path.expanduser('~').rstrip('/').encode()
if len(home) < 3:
    sys.exit(0)
blank = b'/' + b'x' * (len(home) - 1)
for path in sys.argv[1:]:
    data = open(path, 'rb').read()
    n = data.count(home + b'/')
    if n:
        open(path, 'wb').write(data.replace(home + b'/', blank + b'/'))
    print(f'{path}: {n} build path(s) scrubbed')
