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
