MacShell
========
A Unix-style shell for classic Mac OS 7, 8 and 9 on 68k Macs, with an SSH client and server,
file transfer, network tools and the everyday command-line tools you know from Linux and macOS.

![MacShell running ifconfig and nslookup in Mac OS 9](docs/screenshot.png)

Requirements
------------
* **CPU**: 68020 or later (a 68030 or faster is recommended for SSH)
* **RAM**: 3 MB for MacShell
* **System**: 7.1 or later (before 7.5, the Thread Manager extension is needed)
* **Network**: Open Transport for SSH, FTP, wget and curl. The local shell works without it.

MacShell is developed and tested in emulated Mac OS 9 (QEMU), where the test suite passes. It is
meant for a Macintosh LC with a 68030 accelerator running System 7.5.3, but hasn't been tested on
that hardware yet. The source also has a PowerPC build, which hasn't been built or
tested yet.

Download
--------
Download `MacShell.bin` from the [latest release](https://github.com/jrobertfisher/MacShell/releases/latest).
It is a MacBinary file, which keeps the application in one piece on the way: Mac FTP and web
programs (Fetch, Transmit) and StuffIt Expander turn it into the MacShell application, and so does
MacShell's own `ftp get` or `wget`. Give MacShell at least 3 MB of memory (the default).

What it does
------------
* **A real shell.** 95 commands with Unix-style paths (`/Macintosh HD/Documents`, `~`, `..`,
  `cd -`, `/` for the list of disks), tab completion, history, wildcards, `;` and `&&`, `>` and
  `>>`, and an exit status for every command. Every command has a manual page (`man ls`,
  `ls --help`, `man -k word`).
* **SSH client and server.** Log in to other computers with `ssh user@host`, using a key made on the Mac
  with `ssh-keygen`, or a password. Copy files with `scp`. Run `sshd start` and other computers
  can log in to the Mac with a key, for an interactive shell or a single command.
* **Modern encryption.** SSH uses elliptic-curve (ECDH P-256, P-384, P-521) or Diffie-Hellman
  key exchange, AES-GCM or AES-CTR encryption with HMAC-SHA-256/512 (encrypt-then-MAC), ECDSA and
  RSA (SHA-2) keys, and strict key exchange against the Terrapin attack (the server uses ECDH
  P-256, AES-128-CTR and HMAC-SHA-256). HTTPS uses TLS 1.2. It is
  built on current libssh2 and mbedTLS 3.6 LTS. The keys' randomness comes from the Mac's sound
  input and clock jitter, checked by health tests, plus a seed saved from the last run; if that
  isn't enough, MacShell asks you to move the mouse and type a few keys (see
  [Randomness](#randomness)).
* **File transfer that keeps Mac files whole.** An interactive FTP client (`ftp host`, passive or
  active), `wget` and `curl` for the web, and `scp`. Files with a resource fork, such as
  applications, are sent as MacBinary, and MacBinary downloads are unpacked.
* **Standard network tools.** ICMP `ping` and `traceroute`, `dig` and `nslookup` that send their own
  DNS queries, `host`, `hostname`, `ifconfig`, `nc` and `telnet`.
* **Mac file tools.** View and change type and creator codes, Finder labels, locked and invisible
  flags, and aliases; convert between Mac, Unix and DOS line endings.
* **A clean terminal.** White on black by default, with other colour schemes; 256 colours and true
  colour for programs that use them; windows and tabs; scrollback; copy and paste.

Commands
--------
* **Files:** `ls`, `cd`, `pwd`, `cat`, `less`, `cp`, `mv`, `rm`, `mkdir`, `rmdir`, `touch`, `ln`,
  `readlink`, `realpath`, `basename`, `dirname`, `which`, `head`, `tail` (including `-f`), `file`
* **Text:** `grep`, `wc`, `cut`, `nl`, `fold`, `rev`, `rot13`, `strings`, `echo`, `seq`,
  `dos2unix`, `unix2dos`, `mac2unix`, `unix2mac`
* **Bytes and checksums:** `hexdump`, `xxd`, `cmp`, `md5sum`, `sha1sum`, `sha256sum`, `sha512sum`,
  `crc32`
* **Mac files:** `getinfo`, `settype`, `setcreator`, `chown`, `fixtype`, `chmod`, `chattr`,
  `label`, `open`
* **System:** `ps`, `free`, `df`, `uname`, `uptime`, `date`, `cal`, `hostname`, `history`,
  `sleep`, `clear`, `colors`, `entropy`
* **Network:** `ssh`, `ssh-keygen`, `sshd`, `scp`, `ftp`, `wget`, `curl`, `telnet`, `nc`, `ping`,
  `traceroute`, `host`, `dig`, `nslookup`, `ifconfig`
* **Help:** `help`, `man`

Windows and tabs: Cmd+N opens a window, Cmd+T a shell tab and Cmd+S an SSH tab.

Randomness
----------
SSH and HTTPS keys are only as strong as the random numbers behind them, and classic Mac OS has
no random number source. MacShell gathers randomness from:

1. **Sound input noise**: the lowest bits of the microphone or line input, which carry analogue
   noise even with nothing plugged in.
2. **Clock jitter**: tiny timing variations between the processor and the Mac's VIA timer.

Each source must pass health tests based on NIST SP 800-90B (repetition, proportion and prediction
tests) and is credited conservatively; silence, hum and repeating patterns count for nothing. The
results are mixed with SHA-256 and saved to a seed file ("MacShell Random Seed" in the Preferences
folder), which is renewed on each launch.

If the sources and seed together give less than 256 bits, MacShell asks you to move the mouse and
type a few keys. Type `entropy` to see where the randomness came from.

Paths
-----
MacShell uses Unix-style paths:

* `/` lists the mounted disks; `/Macintosh HD/Documents` is a folder on the disk "Macintosh HD".
* `~` is the Documents folder on the startup disk (the disk with the active System Folder),
  created if missing. MacShell starts there, and `cd` on its own returns there.
* `cd -` goes back to the previous folder (and again to return).
* `.` and `..` work as usual, including `..` from a disk's top level back to `/`.
* Names with spaces need quotes or a backslash: `cd "/Macintosh HD/System Folder"`.
* A `/` inside a Mac file name is shown and typed as `:` (as macOS does).
* An old-style Mac path such as `Macintosh HD:Documents:notes` still works.

Interactive FTP
---------------
```
$ ftp 192.168.0.10
Username: (enter for anonymous)
ftp> ls
ftp> cd "To Mac"
ftp> get file.sit
ftp> put notes.txt
ftp> bye
```
* Press Return at the Username prompt to log in anonymously; you won't be asked for a password.
  Passwords for other accounts aren't shown as you type.
* Remote commands: `ls`/`dir`, `cd`, `cdup`, `pwd`, `get`, `put`, `mkdir`, `rmdir`, `delete`,
  `rename`, `size`. Local: `lcd`, `lpwd`, `lls`. Session: `passive`, `active`, `status`, `help`, `bye`.
* Passive mode is the default. `ftp -A host` (or `active` in a session) uses active mode, where the
  server connects back to the Mac; this needs the Mac to be reachable from the server.
* One-shot transfers also work: `ftp get user@host:/path/file`, `ftp put file user@host:/dir/`,
  `ftp ls user@host:/dir/`.
* Transfers are binary. A Mac file with a resource fork (an application, for example) is uploaded
  as MacBinary with `.bin` added to its name, so nothing is lost on a non-Mac server; a MacBinary
  download is unpacked into a working Mac file, and the `.bin` is kept.

wget and curl
-------------
* `wget <url>` saves to a file named from the URL (`index.html` for a site address) and never
  overwrites a file: it saves as `name.1`, `name.2` and so on instead. `-O file` chooses the name;
  `-O -` shows the download on screen. wget follows redirects.
* `curl <url>` shows the page on screen. `-o file` saves to a file, `-O` saves with the name from
  the URL, `-I` shows only the headers and `-L` follows redirects. curl stops if the data is binary
  and suggests `-o` instead.
* MacBinary downloads are unpacked (the `.bin` is kept); other files get a Mac type and creator
  from their extension.

SSH keys and the SSH server
---------------------------
* `ssh-keygen` makes an ECDSA P-256 key pair on the Mac, in `~/.ssh` (`id_ecdsa` and
  `id_ecdsa.pub`), optionally protected by a passphrase. Put the public key in
  `~/.ssh/authorized_keys` on the servers you use; `ssh user@host` and `scp` then use it without
  asking for anything.
* `sshd start` runs MacShell's own SSH server in its own tab, so another computer can log in with
  `ssh user@mac` (an interactive shell in a new tab) or run one command with `ssh user@mac
  'command'`, which returns the command's exit status. It accepts keys only, from
  `~/.ssh/authorized_keys` on the Mac (ECDSA P-256, or RSA of 2048 bits or more), uses ECDH
  P-256, AES-128-CTR with HMAC-SHA-256 and strict key exchange, and logs every connection and
  rejected key in `~/.sshd/logs/sshd.log` (never more than 512 KB). See `man sshd`.

DNS and network tools
---------------------
* `ping` sends real ICMP echo requests (until Ctrl+C, or `-c N`); `ping host port` tests a TCP
  port instead. `traceroute` shows the routers on the way to a host.
* `dig` and `nslookup` send their own DNS queries over UDP (A, AAAA, MX, NS, TXT, CNAME, SOA,
  PTR) and show the answers in their usual formats. `host` does simple lookups.
* `ifconfig` shows the address, netmask, gateway, DNS server, hardware address and MTU.

Security notes
--------------
* **HTTPS certificates are not checked.** Classic Mac OS has no certificate store, so `wget` and
  `curl` encrypt the connection but can't confirm who is on the other end. They print a warning
  each time. Don't rely on them for anything sensitive.
* **FTP and Telnet are not encrypted.** Passwords and files cross the network as plain text. Use
  them only on a network you trust; use SSH or `scp` otherwise.
* SSH checks the server's host key against MacShell's known hosts file.
* The SSH server is off until you start it, and accepts keys only, never passwords.

Colour schemes
--------------
**File → Preferences** has a **Color scheme** menu: White on Black (the default), Black on White,
Green on Black, Amber on Black, Solarized Dark and Solarized Light. **Theme File...** loads a
`.sttheme` file, and the Background and Text colour menus override a scheme's colours.

To convert an iTerm2 colour scheme to a `.sttheme` file:

```bash
python3 tools/itermcolors2sttheme.py MyTheme.itermcolors > MyTheme.sttheme
```

Keyboard shortcuts
------------------
| Shortcut | Action |
|----------|--------|
| Cmd+N | New window |
| Cmd+T | New shell tab |
| Cmd+S | New SSH tab |
| Cmd+W | Close tab or window |
| Cmd+D | Disconnect SSH, close tab or quit |
| Cmd+Q | Quit |
| Cmd+K | Connect (SSH) |
| Cmd+1 to Cmd+8 | Switch tabs |
| Cmd+C, Cmd+V | Copy, paste |
| Shift+Page Up/Down | Scroll back |
| Ctrl+C | Cancel the current command or transfer |

Tests
-----
`tests/remote/run_tests.py` runs every MacShell command on a Mac (real or emulated) over SSH and
checks its output and exit status, mostly against what Linux's own tools give for the same files:
over 100 tests, from checksums and text tools to FTP, `dig` and `traceroute`.
`tests/host/test_dnsmsg.py` tests the DNS code on the build computer against real DNS replies.
See `tests/README.md`.

Building
--------
Requires [Retro68](https://github.com/autc04/Retro68/) and cmake. MacShell's patched libssh2,
mbedTLS and libvterm are git submodules and are built automatically.

```bash
mkdir build-m68k && cd build-m68k
cmake .. -DCMAKE_TOOLCHAIN_FILE=/path/to/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
cmake --build . --parallel $(nproc)
```

The result is `MacShell.bin` (MacBinary). The build sets the Finder's "has bundle" flag so the
icon appears. The 68k code uses no floating-point instructions, so MacShell runs on Macs without
an FPU.

To change the icon, edit and run `tools/make_icons.py icons.r docs/icon-preview.png`.

License and credits
-------------------
BSD 2-Clause; see `LICENSE`.

MacShell grew out of [SevenTTY](https://github.com/LXXero/SevenTTY) by LXXero, which is based on
[ssheven](https://github.com/cy384/ssheven) by cy384.
