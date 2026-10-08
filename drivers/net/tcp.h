/* Minimal single-connection TCP client (for www HTTP/HTTPS connect). */
#ifndef RXOS_TCP_H
#define RXOS_TCP_H

#include "rogex.h"

void     tcp_init(void);
void     tcp_service(void);   /* retransmit / timers; call from idle */

/* Connect to dip:dport. Returns true if ESTABLISHED. */
bool     tcp_connect(const uint8_t dip[4], uint16_t dport, uint32_t timeout_ms);
bool     tcp_connected(void);
void     tcp_close(void);

/* Send application data on the open connection. */
bool     tcp_send(const void *data, uint16_t len);

/* Recv into buf; returns bytes (0 if none yet, wait up to timeout_ms). */
uint16_t tcp_recv(void *buf, uint16_t max, uint32_t timeout_ms);

/* Drain all currently buffered RX into buf (non-blocking). */
uint16_t tcp_recv_nb(void *buf, uint16_t max);

#endif /* RXOS_TCP_H */
