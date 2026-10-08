/* Rogex Wired L2 protocol — no IP/DNS. Ping + chat by pseudonym. */
#ifndef RXOS_WIRED_H
#define RXOS_WIRED_H

#include "rogex.h"

#define WIRED_ETHERTYPE 0x88B5
#define WIRED_ALIAS_MAX 32
#define WIRED_MAX_PEERS 16
#define WIRED_CHAT_MAX  200
#define WIRED_CHAT_LOG  8

/* Store our own alias and reset the peer table. Call after account setup. */
void wired_init(const char *my_pseudonym);
bool wired_up(void);                       /* NIC present + initialized      */

/* Broadcast a PING searching for `target` (a pseudonym). */
bool wired_send_ping(const char *target);

/* L2 chat: unicast if we know peer MAC from a prior PONG, else broadcast.
   Message is truncated to WIRED_CHAT_MAX. Real Ethernet frames (EtherType 0x88B5). */
bool wired_send_chat(const char *target_alias, const char *msg);

/* Service the network once (or use www_service / ipstack which demuxes L2). */
void wired_service(void);

/* Live results of the last ping round (responders' aliases). */
void wired_clear_replies(void);
int  wired_reply_count(void);
const char *wired_reply_alias(int i);
uint32_t    wired_reply_rtt_ms(int i);   /* 0 if unknown */

/* Chat delivery ACK (WT_ACK). Set after the peer confirms our last say/chat. */
bool        wired_chat_acked(void);
const char *wired_chat_ack_from(void);

/* Recent chat inbox (ring). */
int  wired_chat_count(void);
const char *wired_chat_from(int i);
const char *wired_chat_text(int i);
void wired_chat_clear(void);

/* Our identity for the Wired panel. */
const char *wired_my_alias(void);
void        wired_mac_str(char *out, uint32_t max);   /* "52:54:00:12:34:56" */

/* Register L2 demux with the IP stack (call once at boot after net_init). */
void wired_attach_ipstack(void);

#endif /* RXOS_WIRED_H */
