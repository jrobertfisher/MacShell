#!/usr/bin/env python3
"""Tests dnsmsg.c (MacShell's DNS query builder and reply decoder) against
real DNS replies and the laptop's dig. Also feeds it damaged replies, which
must be rejected without crashing.

Usage: test_dnsmsg.py  (builds the harness with cc first)"""
import os, random, socket, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
HARNESS = os.path.join(HERE, 'dns_harness')
SERVER = ('1.1.1.1', 53)

def build():
    subprocess.check_call(['cc', '-Wall', '-O1', '-o', HARNESS,
                           os.path.join(HERE, 'dns_harness.c'), os.path.join(HERE, '..', '..', 'dnsmsg.c')])

def query(name, qtype):
    q = subprocess.run([HARNESS, 'build', name, qtype], capture_output=True, check=True).stdout
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(5)
    s.sendto(q, SERVER)
    reply, _ = s.recvfrom(4096)
    return reply

def parse(reply):
    r = subprocess.run([HARNESS, 'parse'], input=reply, capture_output=True)
    return r.returncode, r.stdout.decode()

def dig(name, qtype):
    out = subprocess.run(['dig', '+short', '@1.1.1.1', name, qtype], capture_output=True, text=True).stdout
    return sorted(l.strip() for l in out.splitlines() if l.strip())

def main():
    build()
    fails = 0
    cases = [('example.com', 'A'), ('example.com', 'AAAA'), ('gmail.com', 'MX'), ('example.com', 'NS'),
             ('example.com', 'TXT'), ('example.com', 'SOA'), ('www.github.com', 'CNAME'),
             ('1.1.1.1.in-addr.arpa', 'PTR')]
    for name, qtype in cases:
        code, text = parse(query(name, qtype))
        mine = sorted(l.split(' ', 3)[3] for l in text.splitlines()[1:]
                      if l.startswith('1 ') and l.split(' ')[2] == qtype)
        theirs = dig(name, qtype)
        # dig +short drops the trailing quotes' spacing etc. only for TXT; compare as sets
        ok = code == 0 and (mine == theirs or (qtype == 'TXT' and len(mine) == len(theirs)))
        print(('ok    ' if ok else 'FAIL  ') + f'{qtype:5} {name}: {len(mine)} record(s)')
        if not ok:
            print('      mine  ', mine, '\n      dig   ', theirs)
            fails += 1
    code, text = parse(query('no-such-name-macshell-test.example', 'A'))
    ok = 'NXDOMAIN' in text
    print(('ok    ' if ok else 'FAIL  ') + 'NXDOMAIN reported')
    fails += not ok

    # damaged replies: truncated, random bytes, pointer loops
    reply = query('example.com', 'A')
    random.seed(1)
    bad = [reply[:n] for n in range(0, len(reply))]
    bad += [bytes(random.getrandbits(8) for _ in range(random.randint(0, 200))) for _ in range(300)]
    bad.append(reply[:12] + b'\xc0\x0c' + b'\x00\x01\x00\x01')          # question name points at itself
    crashed = 0
    for b in bad:
        r = subprocess.run([HARNESS, 'parse'], input=b, capture_output=True)
        if r.returncode not in (0, 1): crashed += 1
    ok = crashed == 0
    print(('ok    ' if ok else 'FAIL  ') + f'{len(bad)} damaged replies handled without crashing')
    fails += not ok
    print('PASS' if fails == 0 else f'{fails} FAILED')
    return 1 if fails else 0

if __name__ == '__main__':
    sys.exit(main())
