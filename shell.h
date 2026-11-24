/*
 * MacShell (based on SevenTTY by LXXero and ssheven by cy384)
 *
 * Copyright (c) 2020 by cy384 <cy384@cy384.com>
 * See LICENSE file for details
 */

#pragma once

void shell_init(int session_idx);
void shell_input(int session_idx, unsigned char c, int modifiers, unsigned char vkeycode);
void shell_prompt(int idx);

/* Documents:.ssh on the startup volume (where ssh-keygen saves keys).
   Creates the folders if create is set. Returns 1 on success. */
int ssh_key_folder(short* vRefNum, long* dirID, int create);

/* for the SSH server */
void shell_run_line(int idx, char* line);
int shell_is_busy(int idx);
void shell_tick(void);
void shell_more_free(int idx);
void shell_free_session(int idx);
void shell_note_output(int idx, const char* t);
void shell_set_status(int idx, int status);

/* dns.c */
void cmd_dig(int idx, int argc, char* argv[]);
void cmd_nslookup(int idx, int argc, char* argv[]);
int dns_resolve(int idx, const char* cmd, const char* name, InetHost* addr);
