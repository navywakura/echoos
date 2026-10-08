/* EchOS Eclipse Shell — global right-click context menu. */
#ifndef ECHOS_CTXMENU_H
#define ECHOS_CTXMENU_H

#include "rogex.h"

typedef enum {
    CTX_FILE = 0, CTX_FOLDER, CTX_APP, CTX_DESKTOP,
} ctxmenu_kind;

enum {
    /* ctxmenu_action results (>= 0) */
    CTX_NONE       = -1,   /* clicked outside: menu closed, nothing chosen */
    CTX_DISMISS    = -2,   /* clicked inside but on no item                */
    CTX_OPEN       = 0,
    CTX_RENAME     = 1,
    CTX_DELETE     = 2,
    CTX_PROPERTIES = 3,
    CTX_PIN_DOCK   = 4,
    /* custom surface items start here: ctxmenu_add_item("...", CTX_CUSTOM+n) */
    CTX_CUSTOM     = 100,
};

void        ctxmenu_open(uint32_t x, uint32_t y, ctxmenu_kind kind,
                         const char *target, void *ctx);
void        ctxmenu_close(void);
bool        ctxmenu_is_open(void);
bool        ctxmenu_add_item(const char *label, int action);
void        ctxmenu_set_pinned(bool pinned);
uint32_t    ctxmenu_width(void);
uint32_t    ctxmenu_height(void);
int         ctxmenu_hit_test(uint32_t x, uint32_t y);
int         ctxmenu_click(uint32_t x, uint32_t y);   /* consume: returns action */
const char *ctxmenu_target(void);
void        ctxmenu_render(void);

#endif /* ECHOS_CTXMENU_H */
