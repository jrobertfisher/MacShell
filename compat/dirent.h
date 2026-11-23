/*
 * Minimal <dirent.h> for Retro68 (classic Mac OS has no POSIX directories).
 * Only mbedTLS's mbedtls_x509_crt_parse_path() uses it, and MacShell never
 * calls that function: opendir() always fails, so it returns an error cleanly.
 */
#ifndef MACSHELL_COMPAT_DIRENT_H
#define MACSHELL_COMPAT_DIRENT_H

#include <stddef.h>

typedef struct macshell_dir DIR;

struct dirent {
    char d_name[256];
};

static inline DIR *opendir(const char *path) { (void)path; return NULL; }
static inline struct dirent *readdir(DIR *dir) { (void)dir; return NULL; }
static inline int closedir(DIR *dir) { (void)dir; return 0; }

#endif
