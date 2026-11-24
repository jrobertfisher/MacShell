#!/usr/bin/env python3
"""Removes the build computer's home folder from paths that the toolchain's
C library leaves inside the application (in its error messages), so a
published MacShell.bin doesn't carry the builder's user name. Each occurrence
is replaced by a string of the same length, so nothing else in the file moves.

Every occurrence of the home folder is replaced, with or without a "/" after
it, and the file is checked again afterwards: if any trace of the home folder
or the user name in a path is left, the script fails (exit 1) so the build
stops instead of publishing it.

Usage: scrub_build_paths.py file ...   (uses $HOME)
"""
import os, sys

home = os.path.realpath(os.path.expanduser('~')).rstrip('/').encode()
if len(home) < 3:
    sys.exit(0)
user = os.path.basename(home)
# what must not survive: the home folder (also as written through a
# symlink), and the user name as a path component
needles = {home, os.path.expanduser('~').rstrip('/').encode()}
blank = lambda s: b'/' + b'x' * (len(s) - 1)

failed = False
for path in sys.argv[1:]:
    data = open(path, 'rb').read()
    n = 0
    for s in sorted(needles, key=len, reverse=True):
        n += data.count(s)
        data = data.replace(s, blank(s))
    left = [s for s in needles if s in data]
    if user and len(user) >= 3:
        for form in (b'/' + user + b'/', b'/' + user + b'\0'):
            if form in data:
                left.append(form)
    open(path, 'wb').write(data)
    print(f'{path}: {n} build path(s) scrubbed')
    if left:
        print(f'{path}: ERROR: still contains {left!r}', file=sys.stderr)
        failed = True
sys.exit(1 if failed else 0)
