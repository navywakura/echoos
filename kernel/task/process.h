/* Process API (stub). */
#ifndef RXOS_PROCESS_H
#define RXOS_PROCESS_H

#include "rogex.h"

typedef struct {
    int         pid;
    const char *name;
    const char *state;       /* "running" — only the kernel flow exists */
} process_t;

void             process_init(void);
int              process_count(void);      /* always 1 */
const process_t *process_at(int i);        /* i == 0 -> the kernel entry */

#endif /* RXOS_PROCESS_H */
