/*
 * ui/ctxmenu.c — the GLOBAL right-click context menu.
 *
 * Directive: right-click works on ANY item, everywhere — desktop icons,
 * dock tiles, window contents, file explorer rows. This module is the one
 * shared implementation; every surface feeds it a ctxmenu_target describing
 * what was clicked and gets back a ctxmenu_action to execute.
 *
 * Standard items: Open / Rename / Delete / Properties / Pin to Dock.
 * Surface-specific items can be appended via ctxmenu_add_item().
 */
#include "ctxmenu.h"
#include "framebuffer.h"
#include "rogex.h"

#define CTX_MAX_ITEMS 10
#define CTX_ITEM_H    24
#define CTX_PAD       6

typedef struct {
    char        label[28];
    int         action;      /* ctxmenu_action or custom id >= CTX_CUSTOM */
} ctx_item;

static struct {
    bool          open;
    uint32_t      x, y;
    ctxmenu_kind  kind;
    char          target[64];
    void         *ctx;
    int           pinned;    /* -1 unknown, 0 not pinned, 1 pinned        */
    ctx_item      items[CTX_MAX_ITEMS];
    int           item_count;
    int           hover;
} g;


static bool item_enabled(int i)
{
    /* "Pin to Dock" only makes sense for apps; hide for plain desktop. */
    if (g.items[i].action == CTX_PIN_DOCK &&
        g.kind != CTX_APP && g.kind != CTX_FILE && g.kind != CTX_FOLDER)
        return false;
    return true;
}

void ctxmenu_open(uint32_t x, uint32_t y, ctxmenu_kind kind,
                  const char *target, void *ctx)
{
    g.open = true;
    g.x = x; g.y = y;
    g.kind = kind;
    g.ctx = ctx;
    g.pinned = -1;
    g.hover = -1;
    g.item_count = 0;

    size_t n = k_strlen(target);
    if (n >= sizeof(g.target)) n = sizeof(g.target) - 1;
    k_memcpy(g.target, target, n);
    g.target[n] = '\0';

    static const ctx_item defaults[] = {
        { "Open",       CTX_OPEN },
        { "Rename",     CTX_RENAME },
        { "Delete",     CTX_DELETE },
        { "Properties", CTX_PROPERTIES },
        { "Pin to Dock",CTX_PIN_DOCK },
    };
    for (int i = 0; i < (int)(sizeof(defaults) / sizeof(defaults[0])); i++)
        g.items[g.item_count++] = defaults[i];
}

void ctxmenu_close(void) { g.open = false; }
bool ctxmenu_is_open(void) { return g.open; }

bool ctxmenu_add_item(const char *label, int action)
{
    if (!g.open || g.item_count >= CTX_MAX_ITEMS) return false;
    ctx_item *it = &g.items[g.item_count++];
    size_t n = k_strlen(label);
    if (n >= sizeof(it->label)) n = sizeof(it->label) - 1;
    k_memcpy(it->label, label, n);
    it->label[n] = '\0';
    it->action = action;
    return true;
}

/* Report pin state so "Pin to Dock" becomes "Unpin from Dock". */
void ctxmenu_set_pinned(bool pinned)
{
    for (int i = 0; i < g.item_count; i++) {
        if (g.items[i].action == CTX_PIN_DOCK) {
            static const char unpin[] = "Unpin from Dock";
            k_memcpy(g.items[i].label, unpin, sizeof(unpin));
        }
    }
    g.pinned = pinned ? 1 : 0;
}

uint32_t ctxmenu_width(void)  { return 168; }
uint32_t ctxmenu_height(void) { return (uint32_t)g.item_count * CTX_ITEM_H + CTX_PAD * 2; }

int ctxmenu_hit_test(uint32_t x, uint32_t y)
{
    if (!g.open) return -1;
    if (x < g.x || x >= g.x + ctxmenu_width() ||
        y < g.y || y >= g.y + ctxmenu_height()) return -1;
    int idx = (int)((y - g.y - CTX_PAD) / CTX_ITEM_H);
    return (idx >= 0 && idx < g.item_count && item_enabled(idx)) ? idx : -1;
}

/*
 * Consume a click. Returns the chosen action, CTX_NONE when the click
 * landed outside the menu (which closes it), CTX_DISMISS on empty space
 * inside it.
 */
int ctxmenu_click(uint32_t x, uint32_t y)
{
    if (!g.open) return CTX_NONE;
    if (x < g.x || x >= g.x + ctxmenu_width() ||
        y < g.y || y >= g.y + ctxmenu_height()) {
        g.open = false;
        return CTX_NONE;
    }
    int idx = ctxmenu_hit_test(x, y);
    int action = (idx >= 0) ? g.items[idx].action : CTX_DISMISS;
    g.open = false;
    return action;
}

const char *ctxmenu_target(void) { return g.target; }

void ctxmenu_render(void)
{
    if (!g.open) return;
    uint32_t sw, sh; fb_dims(&sw, &sh);
    uint32_t w = ctxmenu_width(), h = ctxmenu_height();
    uint32_t x = g.x, y = g.y;
    if (x + w >= sw) x = (sw > w) ? sw - w - 2 : 0;   /* keep on-screen */
    if (y + h >= sh) y = (sh > h) ? sh - h - 2 : 0;

    fb_shadow_soft(x, y, w, h);
    fb_round_blend(x, y, w, h, 8, 0xFF1B1B23, 245);
    fb_rect_outline(x, y, w, h, RGX_BORDER);

    for (int i = 0; i < g.item_count; i++) {
        uint32_t iy = y + CTX_PAD + (uint32_t)i * CTX_ITEM_H;
        bool hot = (ctxmenu_hit_test(g.x, iy + CTX_ITEM_H / 2) == i && item_enabled(i));
        if (hot) fb_fill_rect(x + 4, iy, w - 8, CTX_ITEM_H, RGX_NAV_SEL);
        uint32_t fg = item_enabled(i) ? RGX_WHITE : RGX_DIM;
        fb_text(x + 12, iy + 5, g.items[i].label, fg, 0);
    }

    /* Footer: what we're acting on — small, dim, professional. */
    fb_text_trans(x + 8, y + h - FB_GLYPH_W - 3, g.target, RGX_DIM);
}
