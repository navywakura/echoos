/*
 * RXFS files are 4 KiB. A 1280x720x24 BMP is ~2.7 MiB, so we write a
 * nearest-neighbour 48x27 24-bit BMP (3942 bytes) plus the real size
 * in the status string. Full-res dumps stay on the host (QEMU screendump).
 */
#include "screen_capture.h"
#include "framebuffer.h"
#include "vfs.h"

#define CAP_W 48
#define CAP_H 27

static char g_path[RXFS_NAME];
static char g_stat[80];
static int  g_n;

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

const char *capture_last_path(void) { return g_path; }
const char *capture_last_status(void) { return g_stat[0] ? g_stat : "no capture yet"; }

bool capture_screen(char *path_out, size_t path_max) {
    uint32_t fw = 0, fh = 0;
    uint8_t bmp[54 + ((CAP_W * 3 + 3) & ~3) * CAP_H];
    uint32_t rowb, off, x, y, i;
    char *d, *e;

    if (!fb_active()) {
        const char *s = "capture: no framebuffer";
        char *q = g_stat;
        while (*s) *q++ = *s++;
        *q = 0;
        return false;
    }
    fb_dims(&fw, &fh);
    if (fw < 2 || fh < 2) return false;

    vfs_mkdir("/screenshots");

    g_n++;
    if (g_n > 99) g_n = 1;
    d = g_path;
    e = g_path + sizeof(g_path);
    {
        const char *p = "/screenshots/s";
        while (*p && d < e - 1) *d++ = *p++;
        *d++ = (char)('0' + g_n / 10);
        *d++ = (char)('0' + g_n % 10);
        p = ".bmp";
        while (*p && d < e - 1) *d++ = *p++;
        *d = 0;
    }

    rowb = (uint32_t)((CAP_W * 3 + 3) & ~3);
    for (i = 0; i < sizeof(bmp); i++) bmp[i] = 0;
    bmp[0] = 'B';
    bmp[1] = 'M';
    put32(bmp + 2, (uint32_t)sizeof(bmp));
    put32(bmp + 10, 54);
    put32(bmp + 14, 40);
    put32(bmp + 18, CAP_W);
    put32(bmp + 22, CAP_H);
    put16(bmp + 26, 1);
    put16(bmp + 28, 24);
    put32(bmp + 34, rowb * CAP_H);

    for (y = 0; y < CAP_H; y++) {
        uint32_t sy = (fh - 1 - y) * (fh / CAP_H);
        if (sy >= fh) sy = fh - 1;
        off = 54 + (CAP_H - 1 - y) * rowb;
        for (x = 0; x < CAP_W; x++) {
            uint32_t sx = x * (fw / CAP_W);
            uint32_t px = fb_get_pixel(sx < fw ? sx : fw - 1, sy);
            bmp[off + x * 3 + 0] = (uint8_t)(px);
            bmp[off + x * 3 + 1] = (uint8_t)(px >> 8);
            bmp[off + x * 3 + 2] = (uint8_t)(px >> 16);
        }
    }

    if (!vfs_write(g_path, (const char *)bmp, (uint32_t)sizeof(bmp))) {
        const char *s = "capture: vfs write failed";
        char *q = g_stat;
        while (*s) *q++ = *s++;
        *q = 0;
        return false;
    }
    {
        char *q = g_stat;
        const char *s = "ok ";
        while (*s) *q++ = *s++;
        s = g_path;
        while (*s) *q++ = *s++;
        s = " (48x27 thumb; RXFS 4KiB)";
        while (*s) *q++ = *s++;
        *q = 0;
    }
    if (path_out && path_max) {
        const char *s = g_path;
        d = path_out;
        e = path_out + path_max;
        while (*s && d < e - 1) *d++ = *s++;
        *d = 0;
    }
    return true;
}
