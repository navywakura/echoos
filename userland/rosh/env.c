/* Shell environment (key=value). Small and fixed-size. */
#include "env.h"
#include "version.h"

#define ENV_MAX      12
#define ENV_KEY_MAX  16
#define ENV_VAL_MAX  64

typedef struct { char key[ENV_KEY_MAX]; char val[ENV_VAL_MAX]; bool used; } env_t;
static env_t g_env[ENV_MAX];

static void cpy(char *d, const char *s, int max) {
    int i = 0;
    for (; s[i] && i < max - 1; i++) d[i] = s[i];
    d[i] = '\0';
}
static bool eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}

void env_init(const char *user) {
    for (int i = 0; i < ENV_MAX; i++) g_env[i].used = false;
    env_set("USER", user && user[0] ? user : "anon");
#if ECHOS_CLI_ONLY
    {
        char home[ENV_VAL_MAX];
        char *d = home;
        const char *p = "/users/";
        const char *u = user && user[0] ? user : "live";
        while (*p) *d++ = *p++;
        p = u; while (*p && d < home + sizeof(home) - 1) *d++ = *p++;
        *d = '\0';
        env_set("HOME", home);
    }
#else
    env_set("HOME", "/home");
#endif
    /* Not a search path: there is no exec. Named so a future app loader has a
       place to look, and worded so nobody mistakes it for one today. */
    env_set("PATH", "/bin (built-in commands; no exec yet)");
    env_set("SHELL", "rosh");
    env_set("TERM", "echos");
    env_set("OS", ECHOS_VERSION_NAME);
}

bool env_set(const char *key, const char *val) {
    if (!key || !key[0] || !val) return false;
    for (int i = 0; i < ENV_MAX; i++)
        if (g_env[i].used && eq(g_env[i].key, key)) {
            cpy(g_env[i].val, val, ENV_VAL_MAX);
            return true;
        }
    for (int i = 0; i < ENV_MAX; i++)
        if (!g_env[i].used) {
            g_env[i].used = true;
            cpy(g_env[i].key, key, ENV_KEY_MAX);
            cpy(g_env[i].val, val, ENV_VAL_MAX);
            return true;
        }
    return false;                    /* table full: say no, do not silently drop */
}

const char *env_get(const char *key) {
    for (int i = 0; i < ENV_MAX; i++)
        if (g_env[i].used && eq(g_env[i].key, key)) return g_env[i].val;
    return NULL;
}

int env_count(void) {
    int n = 0;
    for (int i = 0; i < ENV_MAX; i++) if (g_env[i].used) n++;
    return n;
}

bool env_at(int idx, const char **key, const char **val) {
    int n = 0;
    for (int i = 0; i < ENV_MAX; i++) {
        if (!g_env[i].used) continue;
        if (n == idx) { if (key) *key = g_env[i].key; if (val) *val = g_env[i].val; return true; }
        n++;
    }
    return false;
}
