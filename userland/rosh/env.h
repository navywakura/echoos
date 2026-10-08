/* ROSH environment variables. */
#ifndef RXOS_ENV_H
#define RXOS_ENV_H

#include "rogex.h"

void        env_init(const char *user);
bool        env_set(const char *key, const char *val);
const char *env_get(const char *key);          /* NULL when unset            */
int         env_count(void);
bool        env_at(int idx, const char **key, const char **val);

#endif /* RXOS_ENV_H */
