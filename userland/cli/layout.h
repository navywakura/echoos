#ifndef ECHOS_LAYOUT_H
#define ECHOS_LAYOUT_H

#include "rogex.h"

/* Seed the echOS sovereign tree. Idempotent. */
void echos2_layout_seed(void);
/* /users/<name>/{Desktop,Documents,...} — names follow lang (es/en). */
void echos2_users_seed(const char *user, const char *lang);

#endif
