MacShell tests
==============

**On the Mac, over SSH** (`tests/remote/run_tests.py`): runs every command
in MacShell on a real or emulated Mac and checks its output and exit status,
mostly against Linux's own tools on the same files (checksums, `wc`, `grep`,
`cut`, `xxd`, `dig`...). Covers paths, text and byte tools, Mac file commands,
`;`/`&&` and exit codes, ftp/wget/curl, ping, dig and nslookup.

1. In MacShell: `sshd start`; this computer's test key must be in
   `~/.ssh/authorized_keys` on the Mac.
2. `python3 tests/remote/run_tests.py` (or `-k word` for some tests).
   Settings such as the ssh command and the FTP share are described at the
   top of the script.

**On this computer** (`tests/host/`): code that doesn't need a Mac, built
with the system compiler.

- `test_dnsmsg.py`: dig/nslookup's DNS message code against real DNS replies
  and the system's `dig`, plus hundreds of damaged replies.
