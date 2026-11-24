#!/usr/bin/env python3
"""MacShell command tests, run on a Mac over SSH.

MacShell must be running with its SSH server started (sshd start), and this
computer's test key must be in ~/.ssh/authorized_keys on the Mac. Each test
runs one command line with ssh and checks its output and exit status,
mostly against what Linux's own tools give for the same files.

Settings (environment variables):
  MACSHELL_SSH    ssh command to reach the Mac
                  (default: ssh -F ~/bluescsi/ssh/ssh_config mac)
  MACSHELL_SHARE  folder on this computer that the Mac's FTP sees as /To Mac
                  (default: /srv/macshare/To Mac)
  MACSHELL_FTP    this computer's address as the Mac sees it (default 10.0.2.2)
  MACSHELL_SCP    user@host for scp tests, using the Mac's ~/.ssh key
                  (default: none, skipped)

Usage: run_tests.py [-k word] [--quick]
  -k word   run only tests whose name contains word (give -k more than once for several)
  --quick   one or two tests per area (for slow Macs: each test is a new SSH login)
"""
import hashlib, os, random, re, shlex, subprocess, sys, time, zlib

SSH = shlex.split(os.environ.get('MACSHELL_SSH', 'ssh -F %s/bluescsi/ssh/ssh_config -o LogLevel=ERROR mac'
                                 % os.path.expanduser('~')))
SHARE = os.environ.get('MACSHELL_SHARE', '/srv/macshare/To Mac')
FROM_MAC = os.path.join(os.path.dirname(SHARE), 'From Mac')
FTP = os.environ.get('MACSHELL_FTP', '10.0.2.2')
SCP = os.environ.get('MACSHELL_SCP', '')
T = 'mstest'                     # test folder on the Mac, in ~
ANSI = re.compile(r'\x1b\[[0-9;]*[A-Za-z]')

results = []
only = [sys.argv[i + 1] for i, a in enumerate(sys.argv[:-1]) if a == '-k']
quick = '--quick' in sys.argv
# --quick: one or two tests per area, for slow Macs (each test is a new SSH login)
QUICK = {
    'setup: files downloaded', 'status: error is 1', 'chain: && skips after failure',
    'mkdir -p and rmdir', 'cat: Unix lines', 'grep -c', 'sha256sum bin.dat', 'xxd -r round trip',
    'settype, setcreator, getinfo', 'entropy ready', 'ping (ICMP)',
    "dig +short matches this computer's dig", 'curl -I', 'wget https', 'ftp put (plain file)',
    'ftp put arrives intact', 'sshd status',
}


def wanted(name):
    return (not only or any(w in name for w in only)) and (not quick or name in QUICK)


def mac(cmd, timeout=180, stdin=None):
    """Run a command line in MacShell; returns (output, exit status)."""
    try:
        r = subprocess.run(SSH + ['cd ~/%s ; %s' % (T, cmd)], input=stdin, capture_output=True,
                           timeout=timeout)
    except subprocess.TimeoutExpired:
        return '<timed out>', -1
    out = ANSI.sub('', r.stdout.decode('mac_roman', 'replace')).replace('\r', '')
    return out, r.returncode


def check(name, cmd, expect=None, status=0, contains=None, regex=None, absent=None, timeout=180):
    """expect: exact output (trailing whitespace ignored); contains: list of
    substrings; regex: pattern that must match; absent: text that must not appear."""
    if not wanted(name):
        return
    out, code = mac(cmd, timeout)
    problems = []
    if status is not None and code != status:
        problems.append('exit status %s, expected %s' % (code, status))
    if expect is not None and out.rstrip() != expect.rstrip():
        problems.append('output differs')
    for c in contains or []:
        if c not in out:
            problems.append('missing %r' % c)
    if regex and not re.search(regex, out, re.M):
        problems.append('no match for /%s/' % regex)
    if absent and absent in out:
        problems.append('unexpected %r' % absent)
    ok = not problems
    results.append((name, ok))
    print(('ok    ' if ok else 'FAIL  ') + name, flush=True)
    if not ok:
        print('      command: ' + cmd)
        for p in problems:
            print('      ' + p)
        print('      output: ' + (out[:600].replace('\n', '\n              ') or '<none>'))
        if expect is not None:
            print('      expected: ' + expect[:600].replace('\n', '\n                '))


def local_files():
    d = os.path.join(SHARE, 'mstest')
    os.makedirs(d, exist_ok=True)
    files = {
        'text.txt': ''.join('%s,%s,%d\n' % (a, b, i + 1) for i, (a, b) in enumerate(zip(
            'alpha beta gamma Delta epsilon zeta eta theta iota kappa lambda mu'.split(),
            'one two three four five six seven eight nine ten eleven twelve'.split()))).encode(),
        'crlf.txt': b'line one\r\nline two\r\n',
        'mac.txt': b'mac one\rmac two\r',
        'fox.txt': b'Hello, World! The quick brown fox jumps over the lazy dog.\n',
    }
    random.seed(7)
    files['bin.dat'] = bytes(random.getrandbits(8) for _ in range(5000))
    for n, data in files.items():
        with open(os.path.join(d, n), 'wb') as f:
            f.write(data)
    return files


def main():
    files = local_files()
    text, fox, binf = files['text.txt'].decode(), files['fox.txt'].decode(), files['bin.dat']
    lines = text.splitlines()

    print('Setting up ~/%s on the Mac (downloads the test files with MacShell\'s ftp)...' % T)
    mac_setup = 'cd ~ ; mkdir -p %s ; cd %s' % (T, T) + ''.join(
        ' ; ftp get -n "anonymous@%s:/To Mac/mstest/%s"' % (FTP, n) for n in files)
    out, code = subprocess.run(SSH + [mac_setup], capture_output=True, timeout=600).stdout, 0
    check('setup: files downloaded', 'ls', contains=list(files))

    # ---- shell: status, chaining, paths ----
    check('status: success is 0', 'echo hi', 'hi')
    check('status: error is 1', 'cd /nowhere', status=1, contains=['no such directory'])
    check('status: unknown command is 127', 'frobnicate', status=127)
    check('chain: ;', 'echo a ; echo b', 'a\nb')
    check('chain: && runs after success', 'echo a && echo b', 'a\nb')
    check('chain: && skips after failure', 'cd /nowhere && echo no ; echo yes', contains=['yes'], absent='no\n')
    check('chain: quoted ; is not a separator', 'echo "a;b"', 'a;b')
    check('paths: home', 'cd ; pwd', regex=r'^/[^/]+/Documents$')
    check('paths: cd / lists disks', 'cd / ; pwd ; ls', regex=r'^/\n[^\n]+/$')
    check('paths: .. and -', 'cd /; cd ~ ; cd .. ; pwd ; cd - ; pwd', regex=r'^/[^/\n]+\n~\n/[^/\n]+/Documents$')
    check('paths: realpath', 'realpath fox.txt', regex=r'^/[^/]+/Documents/%s/fox\.txt$' % T)
    check('paths: basename, dirname', 'basename /a/b/c.txt ; dirname /a/b/c.txt', 'c.txt\n/a/b')
    check('mkdir -p and rmdir', 'mkdir -p a/b/c && cd a/b/c && pwd && cd ~/%s && rmdir a/b/c && rmdir a/b && rmdir a' % T,
          regex=r'/%s/a/b/c$' % T)
    check('rm with a wildcard in another folder', 'mkdir -p g ; cp fox.txt g/a.tmp ; cp fox.txt g/b.tmp ; '
          'cp fox.txt g/keep.txt ; rm g/*.tmp ; ls g ; rm g/* ; rmdir g', 'keep.txt')
    check('mkdir: existing folder fails', 'mkdir -p x ; mkdir x', status=1)
    check('mv: rename and move into a folder', 'mkdir -p mvdir ; cp fox.txt m.txt && mv m.txt n.txt && mv n.txt mvdir && '
          'ls mvdir && mv mvdir/n.txt back.txt && ls back.txt && rm back.txt && rmdir mvdir', 'n.txt\nback.txt')

    # ---- text tools ----
    check('cat: Unix lines', 'cat text.txt', text)
    check('cat: DOS lines', 'cat crlf.txt', 'line one\nline two')
    check('cat: Mac lines', 'cat mac.txt', 'mac one\nmac two')
    check('head -n 3', 'head -n 3 text.txt', '\n'.join(lines[:3]))
    check('head -2', 'head -2 text.txt', '\n'.join(lines[:2]))
    check('tail -n 2', 'tail -n 2 text.txt', '\n'.join(lines[-2:]))
    check('wc', 'wc text.txt', regex=r'^\s*12\s+12\s+%d text\.txt$' % len(files['text.txt']))
    check('wc -l', 'wc -l text.txt', regex=r'^\s*12 text\.txt$')
    check('grep -c', 'grep -c a text.txt', str(sum('a' in l for l in lines)))
    check('grep -i', 'grep -ic DELTA text.txt', '1')
    check('grep -v', 'grep -v a text.txt', '\n'.join(l for l in lines if 'a' not in l))
    check('grep -n', 'grep -n beta text.txt', '2:beta,two,2')
    check('grep: no match is status 1', 'grep nomatch text.txt', '', status=1)
    check('cut -f1,3', 'cut -d, -f1,3 text.txt', '\n'.join(','.join((l.split(',')[0], l.split(',')[2])) for l in lines))
    check('cut -f2-3', 'cut -d, -f2-3 text.txt', '\n'.join(','.join(l.split(',')[1:3]) for l in lines))
    check('rev', 'rev fox.txt', fox.strip()[::-1])
    import codecs
    check('rot13', 'rot13 fox.txt', codecs.encode(fox.strip(), 'rot13'))
    check('fold -w 20', 'fold -w 20 fox.txt', '\n'.join(fox.strip()[i:i + 20] for i in range(0, len(fox.strip()), 20)))
    check('nl', 'nl text.txt', '\n'.join('%6d\t%s' % (i + 1, l) for i, l in enumerate(lines)))
    check('seq', 'seq 0 10 30', '0\n10\n20\n30')
    check('echo > file has Mac line endings', 'echo hi there > out.txt ; xxd out.txt ; rm out.txt',
          contains=['6869 2074 6865 7265 0d'], absent='0d0a')
    check('echo >> appends', 'echo one > o.txt ; echo two >> o.txt ; cat o.txt ; rm o.txt', 'one\ntwo')
    check('strings -n 6', 'strings -n 6 bin.dat',
          '\n'.join(m.decode() for m in re.findall(rb'[\x20-\x7e]{6,}', binf)))

    # ---- bytes ----
    for f in ('text.txt', 'bin.dat', 'fox.txt'):
        data = files[f]
        for tool, h in (('md5sum', hashlib.md5), ('sha1sum', hashlib.sha1), ('sha256sum', hashlib.sha256),
                        ('sha512sum', hashlib.sha512)):
            check('%s %s' % (tool, f), '%s %s' % (tool, f), '%s  %s' % (h(data).hexdigest(), f))
        check('crc32 %s' % f, 'crc32 %s' % f, '%08x  %s' % (zlib.crc32(data), f))
    check('cmp: same files', 'cmp text.txt text.txt', '')
    check('cmp: different files is status 1', 'cmp text.txt fox.txt', status=1, contains=['differ'])
    check('xxd', 'xxd fox.txt', subprocess.run(['xxd', os.path.join(SHARE, 'mstest', 'fox.txt')],
                                              capture_output=True, text=True).stdout)
    check('hexdump', 'hexdump fox.txt', contains=['0000000  48 65 6c 6c 6f', '|Hello, World! Th|'])
    check('xxd -r round trip', 'xxd bin.dat > bin.hex ; xxd -r bin.hex ; md5sum bin.hex.bin ; rm bin.hex ; rm bin.hex.bin',
          contains=[hashlib.md5(binf).hexdigest()])

    # ---- Mac files ----
    check('settype, setcreator, getinfo', 'cp fox.txt f2 && settype JUNK f2 && setcreator ABCD f2 && getinfo f2',
          contains=['Type:     JUNK', 'Creator:  ABCD'])
    check('chown', 'chown TEXT:ttxt f2 && ls -l f2', contains=['TEXT/ttxt'])
    check('chmod -w: rm refuses a locked file', 'chmod -w f2 ; rm f2', status=1, contains=['locked'])
    check('chmod +w: then rm works', 'chmod +w f2 && rm f2 && ls', absent='f2')
    check('touch makes a SimpleText file', 'touch t.txt && ls -l t.txt ; rm t.txt', contains=['TEXT/ttxt'])
    check('chattr +i hides from ls, not ls -a', 'touch h.txt ; chattr +i h.txt ; ls ; echo -- ; ls -a ; rm h.txt',
          regex=r'(?s)^(?!.*h\.txt.*--).*--.*h\.txt')
    check('label', 'touch l.txt ; label 2 l.txt ; getinfo l.txt ; rm l.txt', contains=['Label:'])
    check('ln -s and readlink', 'ln -s fox.txt fl && readlink fl && cat fl ; rm fl',
          regex=r'/%s/fox\.txt\nHello, World' % T)
    check('fixtype', 'cp fox.txt q.txt && setcreator ???? q.txt && fixtype q.txt && ls -l q.txt ; rm q.txt',
          contains=['ttxt'])
    out, code = ('', 0) if not wanted('dos2unix, unix2mac, mac2unix+unix2dos') else mac('cp crlf.txt c.txt && dos2unix c.txt && xxd c.txt && unix2mac c.txt && xxd c.txt && '
                    'mac2unix c.txt && unix2dos c.txt && xxd c.txt ; rm c.txt')
    hexes = [''.join(l.split(':', 1)[1][:40].split()) for l in out.splitlines() if ':' in l]
    dumps = [hexes[i] + hexes[i + 1] for i in range(0, len(hexes) - 1, 2)]
    ok = dumps == ['6c696e65206f6e650a6c696e652074776f0a', '6c696e65206f6e650d6c696e652074776f0d',
                   '6c696e65206f6e650d0a6c696e652074776f0d0a']
    if wanted('dos2unix, unix2mac, mac2unix+unix2dos'):
        results.append(('dos2unix, unix2mac, mac2unix+unix2dos', ok))
        print(('ok    ' if ok else 'FAIL  ') + 'dos2unix, unix2mac, mac2unix+unix2dos')
        if not ok:
            print('      dumps:', dumps)

    # ---- system ----
    check('uname', 'uname', regex=r'^Mac OS \d+\.\d+')
    check('uname -a shows which code runs', 'uname -a', regex=r'MacShell [\d.]+ \((PowerPC|68k) code\)')
    check('date', 'date', regex=r'^\w{3} \w{3} +\d+ \d\d:\d\d:\d\d \d{4}$')
    check('uptime', 'uptime', regex=r'^up ')
    check('cal 2 2024', 'cal 2 2024', contains=['February 2024', ' 1  2  3', '25 26 27 28 29'])
    check('df', 'df -h', regex=r'^Disk .*\n/[^\n]+\s+\d')
    check('free', 'free', contains=['App heap'])
    check('ps', 'ps', contains=['MacShell'])
    check('hostname', 'hostname', regex=r'\S')
    check('ifconfig', 'ifconfig', regex=r'address:\s+\d+\.\d+\.\d+\.\d+')
    check('ifconfig shows the hardware address', 'ifconfig', regex=r'ether:\s+([0-9a-f]{2}:){5}[0-9a-f]{2}')
    check('entropy ready', 'entropy', contains=['ready'])
    # applications: found by name and by creator (Desktop Database), from any folder
    check('which finds SimpleText by name', 'which SimpleText', regex=r'^/.*/SimpleText$')
    check('which -c ttxt (Desktop Database)', 'which -c ttxt', regex=r'^/.*/SimpleText$')
    check('which -c MACS finds the running Finder', 'which -c MACS', contains=['Finder'])
    check('which: unknown application', 'which NoSuchApp', status=1, contains=['no application called'])
    check('apps lists SimpleText', 'apps Simple', contains=['ttxt', 'SimpleText'])
    check('open: no application for a creator',
          'echo x > nocreator.dat ; settype BINA nocreator.dat ; setcreator zzzz nocreator.dat ; '
          'open nocreator.dat ; rm nocreator.dat', status=0,
          contains=["no application found for creator 'zzzz' (type 'BINA')"])
    check('a command line that is too long is refused', 'echo ' + 'a' * 300, status=1,
          contains=['too long'])
    check('man over ssh prints the whole page (no --more--)', 'man license',
          contains=['Apache License'], absent='--more--')
    check('help lists dig, traceroute and sshd', 'help', contains=['dig', 'traceroute', 'sshd'])
    check('man ls', 'man ls', contains=['NAME', 'ls, dir - list'])
    check('man -k', 'man -k checksum', contains=['md5sum'])
    check('--help', 'ftp --help', contains=['SYNOPSIS'])
    check('man: no page is status 1', 'man nosuchthing', status=1)
    check('sshd status', 'sshd status', contains=['listening on port', 'log '])

    # ---- network ----
    check('ping (ICMP)', 'ping -c 2 %s' % FTP, contains=['2 packets transmitted, 2 received'])
    check('ping host port (TCP)', 'ping %s 21' % FTP, contains=['connected'])
    check('host', 'host example.com', regex=r'has address \d+\.\d+\.\d+\.\d+')
    check('traceroute reaches the host', 'traceroute -n -m 5 %s' % FTP, contains=[' %s ' % FTP, ' ms'])
    check('traceroute: bad usage is status 1', 'traceroute', status=1)
    want = sorted(subprocess.run(['dig', '+short', 'example.com', 'A'], capture_output=True, text=True).stdout.split())
    if wanted("dig +short matches this computer's dig"):
        out, code = mac('dig example.com +short')
        ok = code == 0 and sorted(out.split()) == want
        results.append(('dig +short matches this computer\'s dig', ok))
        print(('ok    ' if ok else 'FAIL  ') + 'dig +short matches this computer\'s dig')
        if not ok:
            print('      mac:', out.split(), 'here:', want)
    check('dig MX', 'dig gmail.com MX', contains=['ANSWER SECTION', 'IN      MX', 'status: NOERROR'])
    check('dig: NXDOMAIN is status 1', 'dig no-such-name-macshell.example', status=1, contains=['NXDOMAIN'])
    check('dig -x', 'dig -x 1.1.1.1 +short', contains=['one.one.one.one.'])
    check('nslookup', 'nslookup example.com', contains=['Server:', 'Address:', 'Name:\texample.com'])
    check('nslookup -type=mx', 'nslookup -type=mx gmail.com', contains=['mail exchanger ='])
    check('curl -I', 'curl -I http://example.com/', contains=['HTTP/1.1 200'])
    check('wget names files and never overwrites', 'wget -n http://example.com/ ; wget -n http://example.com/ ; ls ; rm index.html ; rm index.html.1',
          contains=['index.html\n', 'index.html.1'])
    check('wget https', 'wget -n -O r.txt https://www.google.com/robots.txt && head -n 1 r.txt ; rm r.txt',
          contains=['User-agent'])

    # ftp upload: data-only file as is, and the MacShell application as MacBinary
    stamp = 'mstest-%d' % int(time.time())
    check('ftp put (plain file)', 'ftp put -n bin.dat "anonymous@%s:/From Mac/%s.dat"' % (FTP, stamp))
    p = os.path.join(FROM_MAC, stamp + '.dat')
    if wanted('ftp put arrives intact'):
        ok = os.path.exists(p) and open(p, 'rb').read() == binf
        results.append(('ftp put arrives intact', ok))
        print(('ok    ' if ok else 'FAIL  ') + 'ftp put arrives intact')
    if os.path.exists(p):
        os.remove(p)

    if SCP:
        check('scp download', 'scp -n %s:/etc/hostname h.txt && cat h.txt ; rm h.txt' % SCP, regex=r'\S')

    # ---- clean up ----
    mac('rm ~/%s/* ; cd ~ ; rmdir %s' % (T, T))
    failed = [n for n, ok in results if not ok]
    print('\n%d tests, %d passed, %d failed' % (len(results), len(results) - len(failed), len(failed)))
    for n in failed:
        print('  failed: ' + n)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
