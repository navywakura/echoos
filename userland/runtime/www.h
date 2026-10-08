/*
 * WWW mode — optional World Wide Web stack for EchOS.
 *
 * OFF by default. Enable with `www on` (requires a NIC). Provides:
 *   - IPv4 (static or QEMU defaults / DHCP attempt)
 *   - DNS A lookup
 *   - TCP client
 *   - HTTP/1.0 GET/POST
 *   - HTTPS via TLS 1.3 AES_128_GCM_SHA256 (fallback TLS 1.2 ECDHE-RSA-AES128-GCM) + SNI
 *   - package channel fetch for https://www.rogexlaboratories.com/rx-os/packages
 */
#ifndef RXOS_WWW_H
#define RXOS_WWW_H

#include "rogex.h"

#define WWW_BODY_MAX  (192 * 1024)

void www_init(void);
bool www_enabled(void);
bool www_ready(void);          /* enabled + IP configured + NIC */
const char *www_last_err(void);

/* Enable/disable. enable() brings up IP defaults when none configured. */
bool www_enable(void);
void www_disable(void);

/* Static config: "10.0.2.15" "255.255.255.0" "10.0.2.2" "10.0.2.3" */
bool www_config(const char *ip, const char *mask, const char *gw, const char *dns);

/* DNS A → ipv4. */
bool www_dns(const char *host, uint8_t ip[4], uint32_t timeout_ms);

/* HTTP/1.0 GET. url must be http://host/path (or https:// — see www_fetch).
   Writes body into out (NUL-terminated if space). Returns body length or 0. */
uint32_t www_http_get(const char *url, char *out, uint32_t outmax,
                      int *http_status);

/* HTTP/1.0 GET that follows Location (HTTP only, max 4 hops). */
uint32_t www_http_get_follow(const char *url, char *out, uint32_t outmax,
                             int *http_status);

/* HTTP/1.0 POST. body may be NULL. ctype default: application/x-www-form-urlencoded */
uint32_t www_http_post(const char *url, const char *body, const char *ctype,
                       char *out, uint32_t outmax, int *http_status);

/* Generic HTTP/1.0 method (GET/POST/HEAD/PUT). Follows 301/302/303/307/308. */
uint32_t www_http_request(const char *method, const char *url,
                          const char *body, uint32_t bodylen,
                          const char *content_type,
                          char *out, uint32_t outmax, int *http_status);

/* Same as www_http_request; follow=0 stops at the first response (curl without -L). */
uint32_t www_http_request_ex(const char *method, const char *url,
                             const char *body, uint32_t bodylen,
                             const char *content_type,
                             char *out, uint32_t outmax, int *http_status,
                             int follow);

/* Extra request headers (each call appends "Line\\r\\n"). Cleared per request. */
void        www_clear_extra_hdr(void);
void        www_add_extra_hdr(const char *line);
void        www_set_read_ms(uint32_t ms);          /* recv idle window; 0 = default */

/* Last HTTP response: header block (through the blank line), Set-Cookie jar,
   and Location. Valid until the next request. */
uint32_t    www_last_headers(char *out, uint32_t max);
uint32_t    www_last_set_cookies(char *out, uint32_t max);
const char *www_last_location(void);

/* HTML → printable ASCII (drop script/style/tags). Returns bytes written. */
int www_html_text(const char *in, int nin, char *out, int omax);

/* application/x-www-form-urlencoded. Returns bytes written (no NUL in count). */
int www_url_encode(const char *in, char *out, int omax);

/* Google HTML search over HTTP (gbv=1). Writes numbered titles into out. */
int www_google_search(const char *query, char *out, int omax, int *http_status);

/* Wikipedia REST extract (es, then en). ASCII-folded. */
int www_wiki_summary(const char *title, char *out, int omax, int *http_status);

/* DuckDuckGo instant answer + lite titles. */
int www_ddg_search(const char *query, char *out, int omax, int *http_status);

/* Wiki → DDG → Google. Human extract, no invention. */
int www_web_search(const char *query, char *out, int omax, int *http_status);

int         www_last_status(void);
const char *www_last_url(void);

/* Fetch a file from the official package channel (or override base).
   path like "INDEX.json" or "hellopkg.rxc". Uses live HTTP when possible;
   for the default HTTPS channel attempts live path then signals via *live. */
uint32_t www_channel_get(const char *path, char *out, uint32_t outmax, bool *live);

const char *www_channel_base(void);   /* default PKG channel URL */
void        www_set_channel_base(const char *url); /* e.g. http://mirror/... */

/* Status text for `www status` (writes into out). */
void www_status_text(char *out, uint32_t outmax);

/* Idle pump: IP + TCP. Safe when www disabled (no-op beyond L2 demux if hooked). */
void www_service(void);

#endif /* RXOS_WWW_H */
