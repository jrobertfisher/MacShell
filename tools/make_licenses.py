#!/usr/bin/env python3
"""Collects the licences of MacShell and of everything built into it, from
the original files, into docs/LICENSES.txt (shipped with releases) and
man/license.txt (shown by 'man license' in MacShell).

Usage: make_licenses.py   (run from the repository's top folder)
"""
import os, re, textwrap

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NEWLIB = os.path.expanduser('~/Retro68/gcc/newlib/libc')


def read(*p):
    return open(os.path.join(*p), encoding='utf-8').read().strip('\n')


def c_comment(path, first_line, last_line):
    """The licence block of a C file, without comment markers."""
    lines = open(path, encoding='utf-8', errors='replace').read().split('\n')
    out = []
    for l in lines[first_line - 1:last_line]:
        l = re.sub(r'^\s*/?\*+/?', '', l).rstrip()
        out.append(l[1:] if l.startswith(' ') else l)
    return '\n'.join(out).strip('\n')


def wrap(text, width=78):
    """Re-wrap paragraphs so they fit an 80-column terminal."""
    out = []
    for para in text.split('\n\n'):
        lines = para.split('\n')
        if all(len(l) <= width for l in lines):
            out.append(para)
        else:
            indent = re.match(r'\s*', lines[0]).group(0)
            out.append(textwrap.fill(' '.join(l.strip() for l in lines), width,
                                     initial_indent=indent, subsequent_indent=indent))
    return '\n\n'.join(out)


def section(title, body):
    return '%s\n%s\n\n%s\n' % (title, '-' * len(title), wrap(body))


mbedtls = read(ROOT, 'opentransport-mbedtls', 'LICENSE')
apache = mbedtls[mbedtls.index('Apache License'):mbedtls.index('END OF TERMS AND CONDITIONS') + len('END OF TERMS AND CONDITIONS')]
apache = '\n'.join(l.strip() if l.strip() in ('Apache License', 'Version 2.0, January 2004',
                                              'http://www.apache.org/licenses/') else l for l in apache.split('\n'))

parts = [
    'MacShell includes software from several projects. Their licences and\n'
    'notices follow, as those licences require.\n',
    section('MacShell (BSD 2-Clause License)', read(ROOT, 'LICENSE')),
    section('libssh2 (BSD 3-Clause License)', read(ROOT, 'opentransport-libssh2', 'COPYING')),
    section('Mbed TLS (used under the Apache License 2.0)',
            'Copyright The Mbed TLS Contributors.\n\nMbed TLS is dual-licensed under Apache-2.0 or '
            'GPL-2.0-or-later; MacShell uses it under the Apache License 2.0, whose text follows. '
            'MacShell includes a modified copy (cooperative yields for classic Mac OS).\n\n' + apache),
    section('libvterm (MIT License)', read(ROOT, 'libvterm', 'LICENSE')),
    section('newlib C library (parts of)',
            c_comment(os.path.join(NEWLIB, 'stdlib', 'mprec.c'), 3, 16) + '\n\n' +
            c_comment(os.path.join(NEWLIB, 'stdio', 'vfprintf.c'), 2, 29)),
    section('Retro68 and GCC runtime',
            'MacShell is built with Retro68 (https://github.com/autc04/Retro68). Its runtime '
            'library and the GCC runtime libraries are covered by the GCC Runtime Library '
            'Exception 3.1, which allows them in programs under any licence.'),
]
text = '\n'.join(parts)
os.makedirs(os.path.join(ROOT, 'docs'), exist_ok=True)
with open(os.path.join(ROOT, 'docs', 'LICENSES.txt'), 'w') as f:
    f.write(text)

page = ('NAME\n    license - the licences of MacShell and the software in it\n\nSYNOPSIS\n'
        '    man license\n\nDESCRIPTION\n' + text)
with open(os.path.join(ROOT, 'man', 'license.txt'), 'w') as f:
    f.write(page)
print('docs/LICENSES.txt: %d bytes, longest line %d' % (len(text), max(len(l) for l in text.split('\n'))))
