#include "wallpaper_pack.h"

int wp_count(void) {
    return (int)rxos_wp_count;
}
const char *wp_name_at(int i) {
    if (i < 0 || (uint64_t)i >= rxos_wp_count) return "";
    return rxos_wp_table[i].name;
}
const uint8_t *wp_data_at(int i, uint32_t *len) {
    if (i < 0 || (uint64_t)i >= rxos_wp_count) { if (len) *len = 0; return NULL; }
    if (len) *len = (uint32_t)rxos_wp_table[i].len;
    return rxos_wp_table[i].data;
}
