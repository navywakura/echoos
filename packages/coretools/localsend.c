/* localsend — LAN file transfer for EchOS, written from scratch.
 * TCP sockets only (the OS interface; no external libraries).
 *
 *   localsend send FILE HOST [--port P]
 *   localsend receive [--port P] [-o DIR]
 *
 * Framed protocol (all integers little-endian):
 *   magic    8 bytes  "ECHOSND1"
 *   namelen  u32
 *   name     namelen bytes (UTF-8 basename, no path separators)
 *   size     u64
 *   payload  size bytes
 * Default port: 47219.
 *
 * EchOS coretools — proprietary, from scratch. C99.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define LS_DEFAULT_PORT 47219
static const char LS_MAGIC[8] = { 'E', 'C', 'H', 'O', 'S', 'N', 'D', '1' };

static void put_le32(unsigned char *p, uint32_t v)
{
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}
static void put_le64(unsigned char *p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (8 * i));
}
static uint32_t get_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t get_le64(const unsigned char *p)
{
    uint64_t v = 0;
    int i;
    for (i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static ssize_t write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len > 0) {
        ssize_t w = send(fd, p, len, 0);
        if (w <= 0) return -1;
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

static ssize_t read_all(int fd, void *buf, size_t len)
{
    char *p = buf;
    while (len > 0) {
        ssize_t r = recv(fd, p, len, 0);
        if (r <= 0) return -1;
        p += r;
        len -= (size_t)r;
    }
    return 0;
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* ---------------- sender ---------------- */

static int do_send(const char *file, const char *host, int port)
{
    FILE *f;
    struct sockaddr_in addr;
    int fd;
    unsigned char hdr[8];
    unsigned char nb[4];
    unsigned char sb[8];
    const char *name;
    uint64_t size;
    static char buf[65536];

    f = fopen(file, "rb");
    if (!f) { fprintf(stderr, "localsend: cannot open %s\n", file); return 1; }
    fseek(f, 0, SEEK_END);
    size = (uint64_t)ftell(f);
    fseek(f, 0, SEEK_SET);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("localsend: socket"); fclose(f); return 1; }

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "localsend: invalid IPv4 address '%s'\n", host);
        close(fd); fclose(f);
        return 1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        perror("localsend: connect");
        close(fd); fclose(f);
        return 1;
    }

    name = base_name(file);
    memcpy(hdr, LS_MAGIC, 8);
    if (write_all(fd, hdr, 8) != 0 ||
        strlen(name) > 255) {
        fprintf(stderr, "localsend: handshake failed\n");
        close(fd); fclose(f);
        return 1;
    }
    put_le32(nb, (uint32_t)strlen(name));
    put_le64(sb, size);
    if (write_all(fd, nb, 4) != 0 ||
        write_all(fd, name, strlen(name)) != 0 ||
        write_all(fd, sb, 8) != 0) {
        fprintf(stderr, "localsend: failed to send header\n");
        close(fd); fclose(f);
        return 1;
    }
    {
        uint64_t left = size;
        while (left > 0) {
            size_t chunk = left > sizeof buf ? sizeof buf : (size_t)left;
            if (fread(buf, 1, chunk, f) != chunk) {
                fprintf(stderr, "localsend: short read on local file\n");
                close(fd); fclose(f);
                return 1;
            }
            if (write_all(fd, buf, chunk) != 0) {
                fprintf(stderr, "localsend: connection lost\n");
                close(fd); fclose(f);
                return 1;
            }
            left -= chunk;
        }
    }
    printf("sent %s (%llu bytes) to %s:%d\n",
           name, (unsigned long long)size, host, port);
    close(fd);
    fclose(f);
    return 0;
}

/* ---------------- receiver ---------------- */

static int do_receive(int port, const char *outdir)
{
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    int yes = 1;

    if (srv < 0) { perror("localsend: socket"); return 1; }
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) != 0) {
        perror("localsend: bind");
        close(srv);
        return 1;
    }
    if (listen(srv, 4) != 0) {
        perror("localsend: listen");
        close(srv);
        return 1;
    }
    printf("localsend: listening on 0.0.0.0:%d\n", port);

    for (;;) {
        int fd = accept(srv, NULL, NULL);
        unsigned char hdr[8], nb[4], sb[8];
        uint32_t namelen;
        uint64_t size;
        char name[256];
        FILE *out;
        static char buf[65536];
        uint64_t left;
        char full[4096];
        const char *bn;

        if (fd < 0) { perror("localsend: accept"); continue; }
        if (read_all(fd, hdr, 8) != 0 || memcmp(hdr, LS_MAGIC, 8) != 0) {
            fprintf(stderr, "localsend: bad magic from peer\n");
            close(fd);
            continue;
        }
        if (read_all(fd, nb, 4) != 0) { close(fd); continue; }
        namelen = get_le32(nb);
        if (namelen == 0 || namelen >= sizeof name) {
            fprintf(stderr, "localsend: bad name length\n");
            close(fd);
            continue;
        }
        if (read_all(fd, name, namelen) != 0) { close(fd); continue; }
        name[namelen] = '\0';
        /* strip any path components — we only accept a bare filename */
        bn = base_name(name);
        if (strchr(bn, '/') || strcmp(bn, "..") == 0) {
            fprintf(stderr, "localsend: refusing unsafe name '%s'\n", name);
            close(fd);
            continue;
        }
        if (read_all(fd, sb, 8) != 0) { close(fd); continue; }
        size = get_le64(sb);

        if (outdir && *outdir)
            snprintf(full, sizeof full, "%s/%s", outdir, bn);
        else
            snprintf(full, sizeof full, "%s", bn);

        out = fopen(full, "wb");
        if (!out) {
            fprintf(stderr, "localsend: cannot create %s\n", full);
            close(fd);
            continue;
        }
        left = size;
        while (left > 0) {
            size_t want = left > sizeof buf ? sizeof buf : (size_t)left;
            if (read_all(fd, buf, want) != 0) {
                fprintf(stderr, "localsend: transfer interrupted\n");
                break;
            }
            fwrite(buf, 1, want, out);
            left -= want;
        }
        fclose(out);
        close(fd);
        if (left == 0) {
            printf("received %s (%llu bytes)\n",
                   bn, (unsigned long long)size);
        } else {
            remove(full);
        }
    }
    close(srv);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "send") == 0) {
        const char *file = NULL, *host = NULL;
        int port = LS_DEFAULT_PORT, i;
        for (i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
                port = atoi(argv[++i]);
            else if (!file) file = argv[i];
            else if (!host) host = argv[i];
        }
        if (!file || !host) {
            fprintf(stderr, "usage: localsend send FILE HOST [--port P]\n");
            return 2;
        }
        return do_send(file, host, port);
    }
    if (argc >= 2 && strcmp(argv[1], "receive") == 0) {
        int port = LS_DEFAULT_PORT, i;
        const char *outdir = NULL;
        for (i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
                port = atoi(argv[++i]);
            else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
                outdir = argv[++i];
        }
        return do_receive(port, outdir);
    }
    fprintf(stderr,
        "usage:\n"
        "  localsend send FILE HOST [--port P]\n"
        "  localsend receive [--port P] [-o DIR]\n");
    return 2;
}
