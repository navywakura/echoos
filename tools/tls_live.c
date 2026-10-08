/* Live TLS 1.2 smoke against a real HTTPS host.
   gcc -std=c17 -O2 -DRXOS_HOSTTEST -Idrivers/net \
       -o /tmp/tls_live tools/tls_live.c drivers/net/tls_crypto.c drivers/net/tls_min.c
*/
#define _POSIX_C_SOURCE 200809L
#define RXOS_HOSTTEST 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>

#include "tls_min.h"
#include "tls_crypto.h"

static int g_fd = -1;
static int g_up;

uint64_t pit_ticks(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 100ull + (uint64_t)(ts.tv_nsec / 10000000ull);
}

int host_tcp_send(const void *p, uint16_t n) {
    const uint8_t *b = p;
    uint16_t off = 0;
    if (g_fd < 0) return 0;
    while (off < n) {
        ssize_t w = send(g_fd, b + off, (size_t)(n - off), 0);
        if (w <= 0) return -1;
        off = (uint16_t)(off + w);
    }
    return (int)n;
}

uint16_t host_tcp_recv(void *p, uint16_t n, uint32_t ms) {
    struct pollfd pfd;
    ssize_t r;
    if (g_fd < 0) return 0;
    pfd.fd = g_fd; pfd.events = POLLIN;
    if (poll(&pfd, 1, (int)ms) <= 0) return 0;
    r = recv(g_fd, p, n, 0);
    if (r <= 0) { g_up = 0; return 0; }
    return (uint16_t)r;
}

int host_tcp_connected(void) { return g_up && g_fd >= 0; }

static int dial(const char *host, const char *port) {
    struct addrinfo hints, *res, *rp;
    int fd = -1;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_INET;
    if (getaddrinfo(host, port ? port : "443", &hints, &res) != 0) return -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

int main(int argc, char **argv) {
    const char *host = argc > 1 ? argv[1] : "www.rogexlaboratories.com";
    const char *port = argc > 2 ? argv[2] : "443";
    char why[128], req[256], body[4096];
    int n, got = 0;
    tls_crypto_reseed((uint32_t)time(NULL) ^ (uint32_t)getpid());
    g_fd = dial(host, port);
    if (g_fd < 0) { fprintf(stderr, "dial %s:%s failed\n", host, port); return 2; }
    g_up = 1;
    printf("connected to %s:443, handshake...\n", host);
    fflush(stdout);
    if (!tls_handshake(host, why, (int)sizeof why)) {
        fprintf(stderr, "handshake FAIL: %s\n", why[0] ? why : "(empty)");
        close(g_fd);
        return 1;
    }
    printf("handshake OK [%s]\n", tls_version_name());
    fflush(stdout);
    snprintf(req, sizeof req,
             "GET / HTTP/1.1\r\nHost: %s\r\nUser-Agent: EchOS/2.0\r\n"
             "Accept-Encoding: identity\r\nConnection: close\r\n\r\n",
             host);
    if (!tls_write(req, (uint16_t)strlen(req))) {
        fprintf(stderr, "tls_write fail\n");
        tls_close(); close(g_fd); return 1;
    }
    while ((n = tls_read(body + got, (uint16_t)(sizeof body - 1 - got), 4000)) > 0) {
        got += n;
        if (got >= (int)sizeof body - 1) break;
    }
    body[got] = 0;
    tls_close();
    close(g_fd);
    printf("read %d bytes\n", got);
    fwrite(body, 1, got > 400 ? 400 : (size_t)got, stdout);
    printf("\n");
    if (got >= 12 && memcmp(body, "HTTP/", 5) == 0) {
        printf("LIVE OK\n");
        return 0;
    }
    printf("LIVE FAIL (no HTTP status)\n");
    return 1;
}
