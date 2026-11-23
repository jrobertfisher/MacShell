/*
 * MacShell entropy: gathers randomness for SSH and TLS on classic Mac OS,
 * which has no random number source of its own.
 *
 * Sources (credited only after health tests pass):
 *   - sound input noise (the analogue input's lowest bits, even with nothing
 *     plugged in, on Macs whose input isn't digitally silent)
 *   - clock jitter between the CPU and the VIA timer that drives Microseconds()
 * A seed file in the Preferences folder carries randomness between runs.
 * If fresh sources plus a valid seed give less than 256 bits, the user is
 * asked to move the mouse and type a few keys (fallback).
 */
#ifndef MACSHELL_MACENTROPY_H
#define MACSHELL_MACENTROPY_H

#include <stddef.h>

#define MACENTROPY_REQUIRED_BITS 256

typedef struct {
    int sound_available;        /* the Mac has a sound input device */
    int sound_passed;           /* sound samples passed the health tests */
    long sound_bits;            /* bits credited from sound */
    int jitter_passed;
    long jitter_bits;
    int seed_valid;             /* a full-strength seed file was found */
    long human_bits;            /* bits credited from mouse/keyboard fallback */
    long total_bits;            /* credited total (seed counts as 256) */
    int ready;                  /* pool is ready for use */
} macentropy_stats;

/* Gathers randomness. Call once at startup, after the Toolbox is initialised.
   Returns 0 when at least MACENTROPY_REQUIRED_BITS are available (asking the
   user for mouse/keyboard input if needed), or -1 if the user cancelled. */
int macentropy_init(void);

/* Writes the gathered state to the seed file in the Preferences folder.
   Called by macentropy_init(); also useful from an installer. */
int macentropy_save_seed(void);

const macentropy_stats *macentropy_get_stats(void);

/* mbedTLS hooks (MBEDTLS_ENTROPY_HARDWARE_ALT, MBEDTLS_PLATFORM_YIELD_HOOK) */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen);
void mbedtls_platform_yield(void);

#endif
