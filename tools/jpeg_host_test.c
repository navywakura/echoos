/* Host-side check that jpeg.c decodes a packed wallpaper. */
#define JPEG_HOST 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

void *k_memset(void *d, int v, size_t n) { return memset(d, v, n); }
void *k_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }

static uint8_t arena[32 * 1024 * 1024];
static size_t arena_off;
uint64_t pmm_alloc_contig(uint32_t pages) {
    size_t n = (size_t)pages * 4096;
    if (arena_off + n > sizeof(arena)) return 0;
    uint64_t p = (uint64_t)(uintptr_t)(arena + arena_off);
    arena_off += n;
    return p;
}

#include "../drivers/video/jpeg.c"

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "assets/wallpaper/pack/paper29.jpg";
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) return 1;
    fclose(f);
    jpeg_info_t inf;
    if (!jpeg_info(buf, (uint32_t)n, &inf)) {
        fprintf(stderr, "jpeg_info failed\n");
        return 2;
    }
    printf("info %ux%u\n", inf.width, inf.height);
    uint32_t *pix = malloc((size_t)inf.width * inf.height * 4);
    uint32_t got = jpeg_decode(buf, (uint32_t)n, pix, inf.width * inf.height, &inf);
    printf("decoded %u pixels\n", got);
    if (got != inf.width * inf.height) return 3;
    /* sample centre should not be all black or all white if decode worked */
    uint32_t c = pix[(inf.height / 2) * inf.width + inf.width / 2];
    printf("center 0x%06x\n", c & 0xffffff);
    FILE *o = fopen("/tmp/rxos_jpeg_test.ppm", "wb");
    if (o) {
        fprintf(o, "P6\n%u %u\n255\n", inf.width, inf.height);
        for (uint32_t i = 0; i < got; i++) {
            uint8_t rgb[3] = { (uint8_t)(pix[i] >> 16), (uint8_t)(pix[i] >> 8), (uint8_t)pix[i] };
            fwrite(rgb, 1, 3, o);
        }
        fclose(o);
        printf("wrote /tmp/rxos_jpeg_test.ppm\n");
    }
    return 0;
}
