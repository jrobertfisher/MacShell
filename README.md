MacShell
========
An SSH client and Unix-style shell for classic Mac OS 7, 8 and 9 on 68k Macs.

MacShell is a fork of [SevenTTY](https://github.com/LXXero/SevenTTY) by LXXero, which is itself a
fork of [ssheven](https://github.com/cy384/ssheven) by cy384. It aims to be a clean, plain terminal
with current security libraries and the everyday command-line tools you know from Linux and macOS.

![MacShell icons](docs/icon-preview.png)

What's different from SevenTTY
------------------------------
* **Current security libraries**: libssh2 from upstream (November 2026, includes the
  Terrapin "strict key exchange" fix) and mbedTLS 3.6.7 LTS, built with unused features removed.
* **Real randomness for SSH and TLS**: classic Mac OS has no random number source, so MacShell
  gathers its own (see [Randomness](#randomness)) and refuses to make a secure connection until it
  has at least 256 bits.
* **A clean look**: white text on black by default, a plain prompt, plain `ls`. Colour text from
  programs (for example `ls --color` on a server) still works.
* **An interactive FTP client** with active and passive modes.
* **`curl`** alongside `wget`.
* **A new icon**, an original design.

Features
--------
* **SSH client**: password and public key login, known hosts checking.
* **Local shell** with almost 90 commands, tab completion and history (up/down arrows):
  * Files: `ls`, `cd`, `pwd`, `cat`, `less`, `cp`, `mv`, `rm`, `mkdir`, `rmdir`, `touch`, `ln`, `head`, `tail`, `grep`, `wc`, `cut`, `file`, `strings`, `hexdump`, `xxd`
  * Text: `dos2unix`, `unix2mac`, `mac2unix`, `fold`, `nl`, `rev`, `rot13`, `seq`, `echo`
  * Checksums: `md5sum`, `sha1sum`, `sha256sum`, `sha512sum`, `crc32`, `cmp`
  * System: `ps`, `free`, `df`, `uname`, `uptime`, `date`, `cal`, `hostname`, `ifconfig`, `entropy`
  * Mac-specific: `getinfo`, `settype`, `setcreator`, `fixtype`, `chattr`, `chmod`, `chown`, `label`, `open`
  * Network: `ssh`, `telnet`, `nc`, `ping`, `host`, `ftp`, `scp`, `wget`, `curl`
* **Windows and tabs**: Cmd+N opens a window; Cmd+T opens a shell tab, Cmd+S an SSH tab.
* **Terminal**: xterm-256color with 256 colours and true colour, bold, italic, underline, reverse
  video, a symbol font for box drawing, 100 lines of scrollback per tab, copy and paste.

Requirements
------------
* **CPU**: 68020 or later (a 68030 or faster is recommended for SSH)
* **RAM**: 2 MB for MacShell
* **System**: 7.1 or later (before 7.5, the Thread Manager extension is needed)
* **Network**: Open Transport for SSH, FTP, wget and curl. The local shell works without it.

MacShell has been tested on a Macintosh LC with a 68030 accelerator running System 7.5.3, and in
emulated Mac OS 9. The source still has SevenTTY's PowerPC build, but it hasn't been built or
tested since the changes above.

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
* Transfers are binary and copy the data fork only.

wget and curl
-------------
* `wget <url>` saves to a file named from the URL (`index.html` for a site address) and never
  overwrites a file: it saves as `name.1`, `name.2` and so on instead. `-O file` chooses the name;
  `-O -` shows the download on screen. wget follows redirects.
* `curl <url>` shows the page on screen. `-o file` saves to a file, `-O` saves with the name from
  the URL, `-I` shows only the headers and `-L` follows redirects. curl stops if the data is binary
  and suggests `-o` instead.
* Downloads with a MacBinary header, or a known file extension, get the right Mac type and creator.

Security notes
--------------
* **HTTPS certificates are not checked.** Classic Mac OS has no certificate store, so `wget` and
  `curl` encrypt the connection but can't confirm who is on the other end. They print a warning
  each time. Don't rely on them for anything sensitive.
* **FTP and Telnet are not encrypted.** Passwords and files cross the network as plain text. Use
  them only on a network you trust; use SSH or `scp` otherwise.
* SSH checks the server's host key against MacShell's known hosts file.

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

License
-------
BSD 2-Clause; see `LICENSE`.

Based on [SevenTTY](https://github.com/LXXero/SevenTTY) by LXXero and
[ssheven](https://github.com/cy384/ssheven) by cy384.
