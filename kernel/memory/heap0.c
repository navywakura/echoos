/*
 * Heap-0 Static Memory Layout Implementation
 */
#include "heap0.h"
#include "seb.h"

/* Pre-allocated static buffers in BSS (zero fragmentation) */
static uint8_t g_heap0_wsp_pool[HEAP0_WSP_POOL_SIZE] __attribute__((aligned(64)));

static heap0_region_t g_regions[] = {
    { "VRAM Backbuffer", 0, 1280 * 720 * 4, "Direct 32bpp double-buffering" },
    { "RogexWSP Pool",   0, HEAP0_WSP_POOL_SIZE, "Static 16-byte frame queues" },
    { "SEB Ring Buffer", 0, sizeof(seb_spike_t) * SEB_RING_CAPACITY, "Spiking Event Bus pulse ring" },
    { "Device Registry", 0, sizeof(void*) * HEAP0_DEVMGR_MAX, "Static driver descriptors" },
    { "Icon Static Pool",0, 64 * 1024, "Static RGBA icon cache" }
};

#define REGION_COUNT (sizeof(g_regions) / sizeof(g_regions[0]))

void heap0_init(void) {
    /* Bind runtime addresses to descriptors for introspection */
    g_regions[1].base_addr = (uint64_t)(uintptr_t)g_heap0_wsp_pool;
}

bool heap0_ready(void) {
    return g_regions[1].base_addr == (uint64_t)(uintptr_t)g_heap0_wsp_pool
        && g_regions[1].base_addr != 0;
}

size_t heap0_total_bytes(void) {
    size_t total = 0;
    for (size_t i = 0; i < REGION_COUNT; i++) {
        total += g_regions[i].size_bytes;
    }
    return total;
}

size_t heap0_reserved_bytes(void) {
    return heap0_total_bytes();
}

uint32_t heap0_region_count(void) {
    return (uint32_t)REGION_COUNT;
}

const heap0_region_t *heap0_get_region(uint32_t idx) {
    if (idx >= REGION_COUNT) return NULL;
    return &g_regions[idx];
}

void *heap0_get_wsp_pool(size_t *out_size) {
    if (out_size) *out_size = sizeof(g_heap0_wsp_pool);
    return g_heap0_wsp_pool;
}
