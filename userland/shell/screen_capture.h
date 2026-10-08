#ifndef RXOS_SCREEN_CAPTURE_H
#define RXOS_SCREEN_CAPTURE_H

#include "rogex.h"

/* Downscaled 24-bit BMP into RXFS (files are 4 KiB). Returns true on write. */
bool        capture_screen(char *path_out, size_t path_max);
const char *capture_last_path(void);
const char *capture_last_status(void);

#endif
