/*
 * MacShell's mbedTLS settings (MBEDTLS_USER_CONFIG_FILE), applied on top of
 * mbedTLS's default configuration, so mbedTLS's own files stay unmodified.
 */
#ifndef MACSHELL_MBEDTLS_CONFIG_H
#define MACSHELL_MBEDTLS_CONFIG_H

/* --- classic Mac OS has no time API usable here, no BSD sockets, no files for PSA --- */
#undef MBEDTLS_HAVE_TIME
#undef MBEDTLS_HAVE_TIME_DATE
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_PSA_ITS_FILE_C

/* --- x86/ARM accelerators don't exist on 68k/PPC --- */
#undef MBEDTLS_AESNI_C
#undef MBEDTLS_AESCE_C
#undef MBEDTLS_PADLOCK_C

/* --- randomness comes from macentropy.c (sound input + clock jitter, health-tested) --- */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_ENTROPY_FORCE_SHA256

/* --- cooperative multitasking: yield during long public-key operations --- */
#define MBEDTLS_PLATFORM_YIELD_HOOK

/* --- not needed: smaller code and less attack surface --- */
#undef MBEDTLS_SELF_TEST
/* wget is a TLS 1.2 client only */
#undef MBEDTLS_SSL_SRV_C
#undef MBEDTLS_SSL_PROTO_TLS1_3
#undef MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_ENABLED
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL_ENABLED
#undef MBEDTLS_SSL_EARLY_DATA
#undef MBEDTLS_SSL_RECORD_SIZE_LIMIT
#undef MBEDTLS_SSL_PROTO_DTLS
#undef MBEDTLS_SSL_DTLS_ANTI_REPLAY
#undef MBEDTLS_SSL_DTLS_HELLO_VERIFY
#undef MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE
#undef MBEDTLS_SSL_DTLS_CONNECTION_ID
#undef MBEDTLS_SSL_DTLS_SRTP
#undef MBEDTLS_SSL_COOKIE_C
#undef MBEDTLS_SSL_TICKET_C
#undef MBEDTLS_SSL_CACHE_C
#undef MBEDTLS_SSL_SESSION_TICKETS

#endif
