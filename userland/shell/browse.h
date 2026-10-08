/* Wired browser: simple HTML pages, traceroute, NIC inventory. */
#ifndef RXOS_BROWSE_H
#define RXOS_BROWSE_H

#include "rogex.h"

void browse_cmd(const char *url, char *out, size_t outmax);
void trace_cmd(const char *target, char *out, size_t outmax);
void nics_cmd(char *out, size_t outmax);
void nmap_cmd(const char *host, char *out, size_t outmax);

/* Last page/trace text for the Wired app tabs (NUL-terminated). */
const char *browse_last_page(void);
const void *browse_lines(void);       /* nova_line[] — styled HTML render  */
int         browse_line_count(void);
uint32_t    browse_page_bg(void);
const char *browse_last_trace(void);
const char *browse_last_url(void);

#endif /* RXOS_BROWSE_H */
