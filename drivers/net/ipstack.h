/* Minimal IPv4 stack (ARP, ICMP, UDP). Opt-in upper layers use this. */
#ifndef RXOS_IPSTACK_H
#define RXOS_IPSTACK_H

#include "rogex.h"

#define ETH_TYPE_IPV4  0x0800
#define ETH_TYPE_ARP   0x0806
#define ETH_TYPE_RGXW  0x88B5

typedef struct {
    uint8_t  ip[4];
    uint8_t  mask[4];
    uint8_t  gw[4];
    uint8_t  dns[4];
    bool     configured;
} ip_cfg_t;

void     ipstack_init(void);
void     ipstack_service(void);          /* drain NIC; dispatch ARP/IP/RGXW */
bool     ipstack_configured(void);
const ip_cfg_t *ipstack_cfg(void);

/* Set static addressing (also used after a successful DHCP). */
void     ipstack_set_addr(const uint8_t ip[4], const uint8_t mask[4],
                          const uint8_t gw[4], const uint8_t dns[4]);

/* QEMU user-net defaults: 10.0.2.15/24 gw 10.0.2.2 dns 10.0.2.3 */
void     ipstack_use_qemu_defaults(void);

/* ARP resolve: fills mac[6]. Blocks with service loop up to timeout_ms. */
bool     ip_arp_resolve(const uint8_t ip[4], uint8_t mac[6], uint32_t timeout_ms);

/* ICMP echo request. Returns true if an echo reply arrived; rtt_ms may be NULL. */
bool     ip_icmp_ping(const uint8_t ip[4], uint32_t timeout_ms);
bool     ip_icmp_ping_rtt(const uint8_t ip[4], uint32_t timeout_ms, uint32_t *rtt_ms);
/* Same ping, in microseconds: a local round trip is under a millisecond and
   rounding it to zero looks like a measurement that failed. */
bool     ip_icmp_ping_us(const uint8_t ip[4], uint32_t timeout_ms, uint32_t *rtt_us);

/* Send IPv4 with an explicit TTL (traceroute). */
bool     ip_send_ttl(const uint8_t dip[4], uint8_t proto, uint8_t ttl,
                     const void *payload, uint16_t plen);

#define IP_TRACE_MAX 16
typedef struct {
    uint8_t  ttl;
    uint8_t  ip[4];
    uint32_t rtt_ms;
    uint8_t  kind;     /* 0 timeout, 1 time-exceeded, 2 echo-reply */
} ip_trace_hop_t;

/* ICMP traceroute: increment TTL until the destination answers or max hops. */
int      ip_traceroute(const uint8_t dst[4], ip_trace_hop_t *hops, int maxhops,
                       uint32_t per_hop_ms);

/* UDP: send datagram; recv into buf (returns length or 0). */
bool     ip_udp_send(const uint8_t dip[4], uint16_t dport, uint16_t sport,
                     const void *data, uint16_t len);
/* Arm the one-shot UDP recv so a reply that lands during ARP/send is kept. */
void     ip_udp_listen(uint16_t sport);
uint16_t ip_udp_recv(uint16_t sport, uint8_t sip[4], uint16_t *sport_out,
                     void *buf, uint16_t max, uint32_t timeout_ms);

/* Streaming port: datagrams land in a static ring instead of a single slot,
   and the reader never blocks. For a link that talks continuously (a flight
   controller), where the one-shot above would keep only the last message of
   each burst. `ip_udp_ring_drops()` counts our own overruns — distinct from
   loss on the wire, which the protocol's own sequence numbers show. */
/* The largest datagram the streaming ring will hand back: a full UDP payload
   inside one Ethernet frame. A receive buffer smaller than this silently
   loses the tail of a packed datagram. */
#define IP_UDP_MAX_DATAGRAM 1472
void     ip_udp_bind(uint16_t port);
void     ip_udp_unbind(void);
uint16_t ip_udp_poll(uint16_t port, uint8_t sip[4], uint16_t *sport_out,
                     void *buf, uint16_t max);
uint32_t ip_udp_ring_drops(void);
/* Datagrams that reached the ring. Compared against the frames a protocol
   parser saw, it says whether a loss happened before or after this layer. */
uint32_t ip_udp_ring_accepted(void);
uint32_t ip_udp_ring_depth(void);

/* Low-level IPv4 send (protocol number: 1=ICMP, 6=TCP, 17=UDP). */
bool     ip_send(const uint8_t dip[4], uint8_t proto,
                 const void *payload, uint16_t plen);

/* DHCP / link-local: IPv4 to 255.255.255.255, dest MAC ff:ff:ff:ff:ff:ff.
   Works before ipstack_set_addr (source 0.0.0.0). */
bool     ip_send_bcast(uint8_t proto, const void *payload, uint16_t plen);

/* DHCP DISCOVER/REQUEST. Returns true and fills cfg on ACK. */
bool     ip_dhcp(uint32_t timeout_ms);

/* Feed a received IPv4 payload to the TCP layer (set by tcp_init). */
typedef void (*ip_tcp_rx_fn)(const uint8_t *sip, const uint8_t *dip,
                             const uint8_t *seg, uint16_t seglen);
void     ip_set_tcp_rx(ip_tcp_rx_fn fn);

/* Hook for Rogex Wired L2 frames (0x88B5). */
typedef void (*ip_l2_rx_fn)(const uint8_t *frame, uint32_t len);
void     ip_set_l2_rx(ip_l2_rx_fn fn);

/* Helpers */
uint16_t ip_checksum(const void *data, uint32_t len);
void     ip_fmt(const uint8_t ip[4], char *out, uint32_t max);
bool     ip_parse(const char *s, uint8_t out[4]);

#endif /* RXOS_IPSTACK_H */
