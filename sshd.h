/*
 * MacShell SSH server: lets another computer log in to MacShell with an
 * SSH key. One connection at a time; each opens a MacShell tab.
 */
#ifndef MACSHELL_SSHD_H
#define MACSHELL_SSHD_H

/* sshd start [port] / stop / status (messages go to the given tab) */
int sshd_start(int idx, unsigned short port);
void sshd_stop(int idx);
void sshd_status(int idx);
int sshd_running(void);

/* called from the main event loop: opens tabs, feeds typed input to the
   shell, runs requested commands and closes finished tabs */
void sshd_tick(void);

/* shell output for a tab that belongs to a connection */
void sshd_output(int idx, const char* data, long len);

/* from shell_init: a new tab may belong to a waiting connection */
void sshd_claim_session(int idx);

/* from close_session: the tab is going away */
void sshd_session_closing(int idx);

#endif
