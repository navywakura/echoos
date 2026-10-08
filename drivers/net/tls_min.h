#ifndef RXOS_TLS_MIN_H
#define RXOS_TLS_MIN_H

#ifdef RXOS_HOSTTEST
#include <stdint.h>
#ifndef __cplusplus
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#ifndef bool
typedef int bool;
#define true 1
#define false 0
#endif
#endif
#endif
#else
#include "rogex.h"
#endif

/* Legacy probe (ClientHello + first record). Prefer tls_handshake. */
int tls_min_probe(char *why, int whymax);

/* Full TLS 1.2 handshake on the current tcp socket.
   Cipher: TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256 (0xC02F). SNI required.
   Does NOT verify the CA chain (leaf RSA is used only to check ServerKeyExchange).
   After success, use tls_write / tls_read for application data. */
int  tls_handshake(const char *sni, char *why, int whymax);
int  tls_ready(void);
int  tls_write(const void *data, uint16_t len);
int  tls_read(void *data, uint16_t max, uint32_t timeout_ms);
void tls_close(void);
const char *tls_version_name(void);   /* "TLS 1.3" or "TLS 1.2" */

#endif
