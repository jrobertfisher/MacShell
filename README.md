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
* **Network**: Open Transport for the network commands (ssh, sshd, ftp, wget, curl, ping and the
  rest). The local shell works without it.

MacShell is developed and tested in emulated Mac OS 9.2.1 on PowerPC (QEMU), where all of the
test suite passes, and runs on a Macintosh LC with a 68030 accelerator and System 7.5.3, where a
shorter set of the tests has been run. MacShell holds both the 68k and the PowerPC code.

Download
--------
Download `MacShell_v1.0.2_installer.bin` from the
[latest release](https://github.com/jrobertfisher/MacShell/releases/latest). It is a MacBinary
file, which keeps the program in one piece on the way: Mac FTP and web programs (Fetch,
Transmit), StuffIt Expander, and MacShell's own `ftp get` and `wget` turn it into the
MacShell Installer application.

The installer shows the licenses, installs MacShell in a MacShell folder on the disk you choose,
sets up `~/.ssh` (Documents:.ssh on the startup disk) with an empty `authorized_keys` file for
`sshd`, and can gather the randomness MacShell needs for SSH and HTTPS (a few seconds of moving
the mouse). Give MacShell at least 3 MB of memory (the default).

What it does
------------
* **A real shell.** 97 commands with Unix-style paths (`/Macintosh HD/Documents`, `~`, `..`,
  `cd -`, `/` for the list of disks), tab completion, history, wildcards, `;` and `&&`, `>` and
  `>>`, and an exit status for every command. Every command has a manual page (`man ls`,
  `ls --help`, `man -k word`).
* **SSH client and server.** Log in to other computers with `ssh user@host`, using a key made on the Mac
  with `ssh-keygen`, or a password. Copy files with `scp`. Run `sshd start` and other computers
  can log in to the Mac with a key, for an interactive shell or a single command.
* **Modern encryption.** SSH uses elliptic-curve (ECDH P-256, P-384, P-521) or Diffie-Hellman
  key exchange; ChaCha20-Poly1305, AES-GCM, AES-CTR or (for old servers) AES-CBC encryption with
  HMAC-SHA-256/512; ECDSA and RSA (SHA-2) keys; and strict key exchange against the Terrapin
  attack. The server uses ECDH P-256, AES-128-CTR and HMAC-SHA-256 (encrypt-then-MAC). HTTPS
  uses TLS 1.2. It is built on current libssh2 and mbedTLS 3.6 LTS. The keys' randomness comes
  from mouse movement and keystrokes the first time, then from a seed saved by each run, with
  sound input noise and clock jitter mixed in (see [Randomness](#randomness)).
* **File transfer that keeps Mac files whole.** An interactive FTP client (`ftp host`, passive or
  active), `wget` and `curl` for the web, and `scp`. Files with a resource fork, such as
  applications, are sent as MacBinary, and MacBinary downloads are unpacked.
* **Standard network tools.** ICMP `ping` and `traceroute`, `dig` and `nslookup` that send their own
  DNS queries, `host`, `hostname`, `ifconfig`, `nc` and `telnet`.
* **Mac file tools.** View and change type and creator codes, Finder labels, locked and invisible
  flags, and aliases; convert between Mac, Unix and DOS line endings.
* **A clean terminal.** White on black by default, with other color schemes; 256 colors and true
  color for programs that use them; windows and tabs; scrollback; copy and paste.

Commands
--------
* **Files:** `ls`, `cd`, `pwd`, `cat`, `less`, `cp`, `mv`, `rm`, `mkdir`, `rmdir`, `touch`, `ln`,
  `readlink`, `realpath`, `basename`, `dirname`, `which` (finds applications by name or
  creator), `head`, `tail` (including `-f`), `file`
* **Text:** `grep`, `wc`, `cut`, `nl`, `fold`, `rev`, `rot13`, `strings`, `echo`, `seq`,
  `dos2unix`, `unix2dos`, `mac2unix`, `unix2mac`
* **Bytes and checksums:** `hexdump`, `xxd`, `cmp`, `md5sum`, `sha1sum`, `sha256sum`, `sha512sum`,
  `crc32`
* **Mac files:** `getinfo`, `settype`, `setcreator`, `chown`, `fixtype`, `chmod`, `chattr`,
  `label`, `open` (documents open in their application, as double-clicking does; applications by
  name from any folder), `apps` (every application and what the Desktop Database knows)
* **System:** `ps`, `kill`, `free`, `df`, `uname`, `uptime`, `date`, `cal`, `hostname`, `history`,
  `sleep`, `clear`, `colors`, `entropy`, `exit`
* **Network:** `ssh`, `ssh-keygen`, `sshd`, `scp`, `ftp`, `wget`, `curl`, `telnet`, `nc`, `ping`,
  `traceroute`, `host`, `dig`, `nslookup`, `ifconfig`
* **Help:** `help`, `man`

Windows and tabs: Cmd+N opens a window, Cmd+T a shell tab and Cmd+S an SSH tab.

Randomness
----------
SSH and HTTPS keys are only as strong as the random numbers behind them, and classic Mac OS has
no random number source. MacShell gathers randomness from:

1. **You**: the first time, MacShell asks you to move the mouse and type a few keys (about five
   seconds of movement). The installer can do this for you.
2. **A seed file** ("MacShell Random Seed" in the Preferences folder), written at the end of each
   run from everything gathered. It is used up when read, so no two runs start from the same
   seed; after the first time you aren't asked again.
3. **Sound input noise and clock jitter**: the lowest bits of the microphone or line input, and
   timing variations between the processor and the Mac's VIA timer. These are always mixed in and
   checked by health tests based on NIST SP 800-90B, but they don't count toward the 256 bits
   needed, since an idle input can carry mains hum that looks random to those tests.

Everything is mixed with SHA-256. Type `entropy` to see where the randomness came from.

If you copy a disk image to set up another Mac, delete "MacShell Random Seed" and "MacShell SSH
Host Key" from its Preferences folder, so the two Macs don't share them.

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
* `sshd start` runs MacShell's own SSH server (port 22, or `sshd start <port>`) in its own tab, so another computer can log in with
  `ssh user@mac` (an interactive shell in a new tab) or run one command with `ssh user@mac
  'command'`, which returns the command's exit status. It accepts keys only, from
  `~/.ssh/authorized_keys` on the Mac (ECDSA P-256, or RSA of 2048 bits or more; a line starting
  with `#` is off, and OpenSSH key options such as `restrict` or `from=` aren't supported, so a key
  on such a line is refused). It serves one connection at a time, uses ECDH
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

Color schemes
-------------
**File → Preferences** has a **Color scheme** menu: White on Black (the default), Black on White,
Green on Black, Amber on Black, Solarized Dark and Solarized Light. **Theme File...** loads a
`.sttheme` file, and the Background and Text color menus override a scheme's colors.

To convert an iTerm2 color scheme to a `.sttheme` file:

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
BSD 2-Clause; see `LICENSE`. MacShell also includes libssh2 (BSD 3-Clause), Mbed TLS (Apache 2.0),
libvterm (MIT) and parts of the newlib C library; all their licenses and notices are in
`docs/LICENSES.txt`, and in MacShell itself with `man license`.

MacShell grew out of [SevenTTY](https://github.com/LXXero/SevenTTY) by LXXero, which is based on
[ssheven](https://github.com/cy384/ssheven) by cy384.
