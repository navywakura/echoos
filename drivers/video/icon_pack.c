#include "icon_pack.h"

int icon_count(void) {
    return (int)rxos_icon_count;
}
int icon_dim(void) {
    return (int)rxos_icon_dim[0];
}
const char *icon_name_at(int i) {
    if (i < 0 || (uint64_t)i >= rxos_icon_count) return "";
    return rxos_icon_table[i].name;
}
const uint32_t *icon_rgba_at(int i) {
    if (i < 0 || (uint64_t)i >= rxos_icon_count) return 0;
    if (rxos_icon_table[i].len < 16) return 0;
    return (const uint32_t *)rxos_icon_table[i].data;
}
int icon_index_by_name(const char *name) {
    int i;
    if (!name) return -1;
    for (i = 0; (uint64_t)i < rxos_icon_count; i++) {
        const char *a = rxos_icon_table[i].name, *b = name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == 0 && *b == 0) return i;
    }
    return -1;
}
