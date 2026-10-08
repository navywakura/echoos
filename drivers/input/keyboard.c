/*
 * PS/2 keyboard (i8042, scancode set 1).
 * Prefer IRQ1 → event fabric SPSC ring; polling if IRQ setup fails.
 *
 * Layouts:
 *   0 = US Windows QWERTY
 *   1 = ES Spanish Windows QWERTY (ISO) — AltGr+2 = @, ñ, dead keys
 *   2 = US Mac QWERTY — Option (AltGr) specials
 * Latin-1 out. Delete acts as backspace.
 * CJK needs a real IME + font — stubbed on purpose, not faked.
 */
#include "rogex.h"
#include "irq.h"
#include "rx_event.h"
#include "seb.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

static inline uint8_t inb(uint16_t port) {
    uint8_t r; __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(port)); return r;
}
static inline void outb_(uint16_t port, uint8_t v) {
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}

/* Layouts. */
enum { KL_US = 0, KL_ES = 1, KL_US_MAC = 2, KL_JP = 3, KL_CN = 4 };
static int g_layout = KL_US;

/* Dead-key markers returned by the layout mapper (out of char range). */
#define DK_ACUTE 0x101
#define DK_DIAER 0x102
#define DK_GRAVE 0x103
#define DK_CIRC  0x104
#define DK_TILDE 0x105

/* Base US scancode->ASCII (set 1), unshifted / shifted. 0 = no character. */
static const unsigned char US_L[128] = {
    0,27,'1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,'a','s',
    'd','f','g','h','j','k','l',';','\'','`',0,'\\','z','x','c','v',
    'b','n','m',',','.','/',0,'*',0,' ',0,0,0,0,0,0,
};
static const unsigned char US_U[128] = {
    0,27,'!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,'A','S',
    'D','F','G','H','J','K','L',':','"','~',0,'|','Z','X','C','V',
    'B','N','M','<','>','?',0,'*',0,' ',0,0,0,0,0,0,
};

static bool g_shift, g_caps, g_ext, g_ctrl, g_altgr, g_alt;
static int  g_dead;   /* pending dead-key accent, or 0 */

/* Spanish Windows QWERTY (ISO 105) with AltGr level.
   Returns Latin-1 / dead marker, or -1 to fall through to US letter table. */
static int es_map(uint8_t sc, bool shift, bool altgr) {
    if (altgr) {
        switch (sc) {
        case 0x02: return '|';            /* AltGr+1 */
        case 0x03: return '@';            /* AltGr+2  ← the missing @ */
        case 0x04: return '#';            /* AltGr+3 */
        case 0x05: return '~';            /* AltGr+4  (often tilde) */
        case 0x06: return 0x80;          /* € as Latin-1 placeholder (0x80) */
        case 0x07: return 0xAC;          /* ¬ */
        case 0x0C: return '\\';           /* AltGr+' */
        case 0x1A: return '[';            /* AltGr+` */
        case 0x1B: return ']';            /* AltGr++ */
        case 0x28: return '{';            /* AltGr+´ */
        case 0x2B: return '}';            /* AltGr+ç */
        case 0x08: return '{';            /* some boards: AltGr+7 */
        case 0x09: return '[';            /* AltGr+8 */
        case 0x0A: return ']';            /* AltGr+9 */
        case 0x0B: return '}';            /* AltGr+0 */
        case 0x35: return '\\';           /* AltGr+- on some ES boards */
        default:   return -1;
        }
    }
    switch (sc) {
    case 0x03: return shift ? '"'  : '2';
    case 0x04: return shift ? 0xB7 : '3';    /* · */
    case 0x05: return shift ? '$'  : '4';
    case 0x06: return shift ? '%'  : '5';
    case 0x07: return shift ? '&'  : '6';
    case 0x08: return shift ? '/'  : '7';
    case 0x09: return shift ? '('  : '8';
    case 0x0A: return shift ? ')'  : '9';
    case 0x0B: return shift ? '='  : '0';
    case 0x0C: return shift ? '?'  : '\'';
    case 0x0D: return shift ? 0xBF : 0xA1;   /* ¿ / ¡ */
    case 0x1A: return shift ? DK_CIRC : DK_GRAVE;   /* ^ ` (dead) */
    case 0x1B: return shift ? '*'  : '+';
    case 0x27: return shift ? 0xD1 : 0xF1;   /* Ñ / ñ */
    case 0x28: return shift ? DK_DIAER : DK_ACUTE;  /* ¨ ´ (dead) */
    case 0x29: return shift ? 0xAA : 0xBA;   /* ª / º */
    case 0x2B: return shift ? 0xC7 : 0xE7;   /* Ç / ç */
    case 0x56: return shift ? '>'  : '<';    /* 102-key ISO extra key */
    case 0x33: return shift ? ';'  : ',';
    case 0x34: return shift ? ':'  : '.';
    case 0x35: return shift ? '_'  : '-';
    default:   return -1;                    /* fall through to US letters */
    }
}

/* US Mac QWERTY: base like US Windows; Option (AltGr) produces Mac specials.
   Shift+2 stays @. Option+2 = ™ is less useful here; map common symbols. */
static int us_mac_map(uint8_t sc, bool shift, bool altgr) {
    if (altgr) {
        switch (sc) {
        case 0x02: return 0xA1;   /* Option+1 → ¡ */
        case 0x03: return 0x84;   /* Option+2 → ™ (use latin-1-ish) */
        case 0x04: return 0xA3;   /* Option+3 → £ */
        case 0x05: return 0xA2;   /* Option+4 → ¢ */
        case 0x06: return 0x88;   /* Option+5 → ∞ placeholder */
        case 0x07: return 0xA7;   /* Option+6 → § */
        case 0x08: return 0xB6;   /* Option+7 → ¶ */
        case 0x09: return 0x80;   /* Option+8 → • */
        case 0x0A: return 0xAA;   /* Option+9 → ª */
        case 0x0B: return 0xBA;   /* Option+0 → º */
        case 0x0C: return 0x96;   /* Option+- → – */
        case 0x0D: return 0x89;   /* Option+= → ≠ */
        case 0x10: return 0x9C;   /* Option+q → œ */
        case 0x11: return 0x91;   /* Option+w → ∑ */
        case 0x12: return DK_ACUTE; /* Option+e dead acute */
        case 0x13: return 0xAE;   /* Option+r → ® */
        case 0x14: return 0x86;   /* Option+t → † */
        case 0x15: return 0xA5;   /* Option+y → ¥ */
        case 0x18: return 0xF8;   /* Option+o → ø */
        case 0x19: return 0x80;   /* Option+p → π placeholder */
        case 0x1A: return '"';     /* Option+[ → “ */
        case 0x1B: return '\'';    /* Option+] → ‘ */
        case 0x1E: return 0xE5;   /* Option+a → å */
        case 0x1F: return 0xDF;   /* Option+s → ß */
        case 0x20: return 0xF0;   /* Option+d → ð */
        case 0x21: return 'f';    /* Option+f — wide chars not in Latin-1 */
        case 0x22: return 0xA9;   /* Option+g → © */
        case 0x26: return 0xAC;   /* Option+l → ¬ */
        case 0x27: return 0x85;   /* Option+; → … */
        case 0x28: return 0xE6;   /* Option+' → æ */
        case 0x2C: return 0xA9;   /* Option+z placeholder */
        case 0x2D: return 0x89;   /* Option+x */
        case 0x2E: return 0xE7;   /* Option+c → ç */
        case 0x2F: return 0x88;   /* Option+v */
        case 0x30: return 0xB0;   /* Option+b → ° */
        case 0x31: return DK_TILDE; /* Option+n dead tilde */
        case 0x32: return 0xB5;   /* Option+m → µ */
        case 0x33: return 0xAB;   /* Option+, → « */
        case 0x34: return 0xBB;   /* Option+. → » */
        case 0x35: return 0xF7;   /* Option+/ → ÷ */
        default:
            /* Shift+Option for a few common ones */
            if (shift) {
                switch (sc) {
                case 0x03: return 0x80; /* € approx */
                case 0x10: return 0x8C; /* Œ */
                case 0x1E: return 0xC5; /* Å */
                case 0x2E: return 0xC7; /* Ç */
                default: break;
                }
            }
            return -1;
        }
    }
    /* Base level identical to US Windows. */
    return -1;
}

/* Combine a dead-key accent with a base letter -> Latin-1 char (or base). */
static int combine(int dead, int base) {
    switch (dead) {
    case DK_ACUTE:
        switch (base) { case 'a':return 0xE1; case 'e':return 0xE9; case 'i':return 0xED;
            case 'o':return 0xF3; case 'u':return 0xFA;
            case 'A':return 0xC1; case 'E':return 0xC9; case 'I':return 0xCD;
            case 'O':return 0xD3; case 'U':return 0xDA; } break;
    case DK_DIAER:
        switch (base) { case 'u':return 0xFC; case 'U':return 0xDC; case 'a':return 0xE4;
            case 'o':return 0xF6; case 'e':return 0xEB; case 'i':return 0xEF;
            case 'A':return 0xC4; case 'O':return 0xD6; case 'E':return 0xCB; case 'I':return 0xCF; } break;
    case DK_GRAVE:
        switch (base) { case 'a':return 0xE0; case 'e':return 0xE8; case 'i':return 0xEC;
            case 'o':return 0xF2; case 'u':return 0xF9;
            case 'A':return 0xC0; case 'E':return 0xC8; case 'I':return 0xCC;
            case 'O':return 0xD2; case 'U':return 0xD9; } break;
    case DK_CIRC:
        switch (base) { case 'a':return 0xE2; case 'e':return 0xEA; case 'i':return 0xEE;
            case 'o':return 0xF4; case 'u':return 0xFB;
            case 'A':return 0xC2; case 'E':return 0xCA; case 'I':return 0xCE;
            case 'O':return 0xD4; case 'U':return 0xDB; } break;
    case DK_TILDE:
        switch (base) { case 'n':return 0xF1; case 'N':return 0xD1;
            case 'a':return 0xE3; case 'A':return 0xC3;
            case 'o':return 0xF5; case 'O':return 0xD5; } break;
    }
    return base;   /* not combinable: emit the base letter */
}

/* Only claim a byte when it is keyboard data (status bit 5 clear); mouse bytes
   (bit 5 set) are left for mouse.c so the two devices share the i8042. */
bool kbd_has_input(void) {
    uint8_t s = inb(PS2_STATUS);
    return (s & 0x01) && !(s & 0x20);
}

void kbd_set_layout(int layout) {
    if (layout < 0) layout = 0;
    if (layout > 4) layout = 0;
    g_layout = layout;
    g_dead = 0;
}
int  kbd_layout(void) { return g_layout; }

const char *kbd_layout_name(void) {
    switch (g_layout) {
    case KL_ES:     return "es";
    case KL_US_MAC: return "mac";
    case KL_JP:     return "jp";
    case KL_CN:     return "cn";
    default:        return "us";
    }
}

/* Translate one raw scancode through the layout/shift/dead-key state machine.
   Returns a Latin-1 char, or -1 for "no character yet". */
static int kbd_translate(uint8_t sc) {
    if (sc == 0xE0) { g_ext = true; return -1; }
    if (g_ext) {
        g_ext = false;
        if (sc == 0x53) return 0x7f;    /* Delete/Supr -> backspace behavior */
        switch (sc) {
        case 0x48: return (g_ctrl || g_shift) ? KEY_PGUP : KEY_UP;
        case 0x50: return (g_ctrl || g_shift) ? KEY_PGDN : KEY_DOWN;
        case 0x49: return KEY_PGUP;     /* Page Up                          */
        case 0x51: return KEY_PGDN;     /* Page Down                        */
        case 0x4B: return KEY_LEFT;
        case 0x4D: return KEY_RIGHT;
        case 0x47: return KEY_HOME;
        case 0x4F: return KEY_END;
        /* Right Ctrl */
        case 0x1D: g_ctrl = true;  return -1;
        case 0x9D: g_ctrl = false; return -1;
        /* Right Alt = AltGr on Windows ES / Option on Mac layouts */
        case 0x38: g_altgr = true;  return -1;
        case 0xB8: g_altgr = false; return -1;
        default:   return -1;
        }
    }
    if (sc & 0x80) {                     /* release */
        uint8_t c = sc & 0x7f;
        if (c == 0x2a || c == 0x36) g_shift = false;
        if (c == 0x1d)              g_ctrl  = false;
        if (c == 0x38)              g_alt   = false;
        /* Left Alt alone is not AltGr; release clears nothing for AltGr
           (Right Alt is extended). */
        return -1;
    }
    if (sc == 0x2a || sc == 0x36) { g_shift = true; return -1; }
    if (sc == 0x1d) { g_ctrl = true; return -1; }
    if (sc == 0x38) { g_alt = true;  return -1; }   /* Left Alt */
    if (sc == 0x3a) { g_caps = !g_caps; return -1; }
    /* Alt+Tab / Ctrl+Tab — window switching (consumed by the shell). */
    if (sc == 0x0f && (g_alt || g_ctrl)) return KEY_ALTTAB;
    if (sc == 0x58) return KEY_F12;   /* F12 — screen capture */
    if (sc >= 128) return -1;

    /* Ctrl+letter -> C0 control code. */
    if (g_ctrl && !g_altgr) {
        int base = US_L[sc];
        if (base >= 'a' && base <= 'z') {
            int code = base - 'a' + 1;
            if (code == 3 || code == 22) return code;   /* Ctrl+C / Ctrl+V */
            if (code >= 0x11 && code <= 0x16) return -1;
            return code;
        }
        return -1;
    }

    int ch = -1;
    if (g_layout == KL_ES) {
        ch = es_map(sc, g_shift, g_altgr);
        if (ch == -1) ch = g_shift ? US_U[sc] : US_L[sc];
    } else if (g_layout == KL_US_MAC) {
        ch = us_mac_map(sc, g_shift, g_altgr);
        if (ch == -1) ch = g_shift ? US_U[sc] : US_L[sc];
        /* Guard: us_mac_map may return values > 255 for skipped wide chars. */
        if (ch > 255 && ch < DK_ACUTE) ch = -1;
    } else {
        /* US Windows: AltGr often used for @ on some non-US hosts; still
           honour Shift level. AltGr+2 = @ as a courtesy. */
        if (g_altgr && sc == 0x03) ch = '@';
        else ch = g_shift ? US_U[sc] : US_L[sc];
    }
    if (ch <= 0) return -1;

    /* Dead keys: remember the accent, combine with the next letter. */
    if (ch == DK_ACUTE || ch == DK_DIAER || ch == DK_GRAVE ||
        ch == DK_CIRC  || ch == DK_TILDE) {
        g_dead = ch;
        return -1;
    }
    if (g_dead) {
        int base = ch;
        int combined = combine(g_dead, base);
        g_dead = 0;
        /* accent + non-combinable letter: emit the base letter — a dead key
           must never EAT a keystroke ("algunas letras no escriben"). */
        if (combined > 0) ch = combined;
    }

    /* Caps lock toggles case for ASCII letters only. */
    if (g_caps) {
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        else if (ch >= 'A' && ch <= 'Z') ch += 32;
    }
    /* Final clamp to Latin-1 / control range for the event fabric. */
    if (ch > 255) return -1;
    return ch;
}

/* Polling path (fallback; also used before IRQ setup). */
int kbd_poll_char(void) {
    uint8_t st = inb(PS2_STATUS);
    if (!(st & 0x01) || (st & 0x20)) return -1;   /* no data, or mouse data */
    return kbd_translate(inb(PS2_DATA));
}

/* IRQ1 path: translate in interrupt context, post the char as a typed event. */
static void kbd_irq(void) {
    uint8_t st = inb(PS2_STATUS);
    if (!(st & 0x01) || (st & 0x20)) return;
    uint8_t sc = inb(PS2_DATA);
    seb_emit_spike(SEB_CH_KEYBOARD, 250, sc, &sc, 1);
    int ch = kbd_translate(sc);
    if (ch >= 0) rx_input_post_key((char)ch);
}

bool kbd_enable_irq(void) {
    for (int i = 0; i < 100000; i++) if (!(inb(PS2_STATUS) & 0x02)) break;
    outb_(PS2_CMD, 0x20);
    for (int i = 0; i < 100000; i++) if (inb(PS2_STATUS) & 0x01) break;
    uint8_t cfg = inb(PS2_DATA);
    cfg |= 0x01;
    cfg &= (uint8_t)~0x10;
    cfg |= 0x40;   /* bit6: translate port-1 Set 2 → Set 1. Some boards/VMs
                      boot with translation off — raw Set 2 then types as
                      scrambled letters / wrong symbols. Force it on. */
    for (int i = 0; i < 100000; i++) if (!(inb(PS2_STATUS) & 0x02)) break;
    outb_(PS2_CMD, 0x60);
    for (int i = 0; i < 100000; i++) if (!(inb(PS2_STATUS) & 0x02)) break;
    outb_(PS2_DATA, cfg);

    while (inb(PS2_STATUS) & 0x01) (void)inb(PS2_DATA);

    for (int i = 0; i < 100000; i++) if (!(inb(PS2_STATUS) & 0x02)) break;
    outb_(PS2_DATA, 0xF3);
    for (int i = 0; i < 100000; i++) if (inb(PS2_STATUS) & 0x01) break;
    (void)inb(PS2_DATA);
    for (int i = 0; i < 100000; i++) if (!(inb(PS2_STATUS) & 0x02)) break;
    outb_(PS2_DATA, 0x00);
    for (int i = 0; i < 100000; i++) if (inb(PS2_STATUS) & 0x01) break;
    (void)inb(PS2_DATA);
    while (inb(PS2_STATUS) & 0x01) (void)inb(PS2_DATA);

    irq_register(1, kbd_irq);
    return true;
}

/* Is there an i8042 at all? An absent controller reads back 0xFF on the
   status port. Asked by the boot banner, which has no business touching a
   port itself. */
bool kbd_controller_present(void) { return inb(0x64) != 0xff; }
