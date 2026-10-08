/* Freestanding types + the core kernel API surface. Include this almost everywhere. */
#ifndef ROGEX_H
#define ROGEX_H

/* Freestanding: no libc. Provide the fixed-width integer + bool basics. */
typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long long  uint64_t;
typedef signed char         int8_t;
typedef short               int16_t;
typedef int                 int32_t;
typedef long long           int64_t;
#ifdef ROGEX_HOST_TEST               /* host toolchain already has these */
#include <stddef.h>
#else
typedef uint64_t            size_t;
typedef uint64_t            uintptr_t;
#endif

#define NULL   ((void *)0)
#define true   1
#define false  0
typedef int bool;

#define ROGEX_PACKED   __attribute__((packed))
#define ROGEX_NORETURN __attribute__((noreturn))
#define ROGEX_UNUSED   __attribute__((unused))

/* --- drivers/video/terminal.c : VGA text + framebuffer console + COM1 ---- */
void     console_init(void);
void     console_putc(char c);
void     console_write(const char *s);
void     console_clear(void);
void     console_write_hex(uint64_t v);
void     console_write_dec(uint64_t v);
bool     console_has_input(void);
char     console_getc(void);
void     console_serial_write(const char *s);   /* serial-only mirror */
bool     console_serial_has_input(void);
int      console_serial_getc_nb(void);          /* non-blocking, -1 = none */
/* CLI palettes: night | matrix | ocean | amber | paper */
int      console_set_theme(const char *name);
const char *console_theme_name(void);
void     console_write_prompt(const char *user, const char *host, const char *cwd);
/* Terminal history view. +rows = older, −rows = newer. 0 = follow live. */
void     console_scroll(int rows);
void     console_scroll_follow(void);
int      console_scroll_offset(void);
/* Geometry of the framebuffer console, in cells, and how far back the
   scrollback goes. The cell size comes from the baked face. */
int      console_scroll_max(void);
int      console_cols(void);
int      console_rows(void);

/* Split console. One shell writes to the focused pane; the others keep what
   they have, except a monitor pane, which the idle hook keeps redrawing.
   dir: 1 = side by side, 2 = stacked. */
#define CONSOLE_PANE_SHELL   0
#define CONSOLE_PANE_MONITOR 1
int      console_pane_count(void);
int      console_pane_focused(void);
int      console_pane_kind(int id);
void     console_pane_geometry(int id, int *cols, int *rows);
int      console_pane_split(int dir);
bool     console_pane_close(void);
void     console_pane_next(void);
void     console_pane_set_kind(int kind);
void     console_pane_write(int id, const char *s);
void     console_pane_clear(int id);
int      console_pane_first_of_kind(int kind);
void     console_caret_sync(int on);   /* place blinking block at the live prompt */
void     console_page(const char *text); /* -- more -- pager (space/q/PgUp/PgDn) */
void     console_clipboard_set(const char *s);
const char *console_clipboard(void);
void     console_sel_pointer(int px, int py, int buttons); /* drag-select */
void     console_sel_copy(void);

/* --- ui/events.c : unified UI input event loop --------------------------- */
size_t   console_readline(char *buf, size_t max);
/* Event loop: 0 = line in buf; >0 = clicked button id (when allow_click). */
int      ui_line(char *buf, size_t max, bool allow_click);
/* Non-blocking poll: 0 none, 1 char in *ch, 2 click id in *click. Moves cursor. */
int      ui_poll(char *ch, int *click);
void     ui_cursor_xy(uint32_t *x, uint32_t *y);
/* Register a hook run on every input-wait iteration (background servicing). */
void     events_set_idle_hook(void (*h)(void));
/* Raw non-blocking input (window manager): 0 none / 1 char / 2 mouse deltas. */
int      ui_next_raw(int *ch, int *dx, int *dy, int *buttons);
/* Pop a pending key without consuming a mouse event (leaves mouse pushed). */
int      ui_next_key_nb(int *ch);

/* --- kernel/log.c : kernel logging ---------------------------------------- */
void     klog_info(const char *msg);
void     klog_warn(const char *msg);
void     klog_error(const char *msg);
/* Boot-banner style status line: "<label>: OK|FAIL|STUB|..." */
void     klog_status(const char *label, const char *status);
void     klog_dec(uint64_t v);

/* --- drivers/input/keyboard.c : PS/2 keyboard (IRQ1 or polled) -----------
   Cursor keys arrive as C0 control codes. They are real characters on the
   input path so the event fabric carries them like any other keystroke, and
   code that only cares about printable text ignores them for free. */
#define KEY_UP     0x11
#define KEY_DOWN   0x12
#define KEY_LEFT   0x13
#define KEY_RIGHT  0x14
#define KEY_HOME   0x15
#define KEY_END    0x1C   /* not 0x16: that byte is Ctrl+V (paste) */
#define KEY_F12    0x18   /* capture; scancode 0x58 */
#define KEY_PGUP   0x19   /* PageUp / Ctrl+Up   — scroll lists              */
#define KEY_PGDN   0x1A   /* PageDown / Ctrl+Down                           */
#define KEY_ALTTAB 0x1B   /* Alt+Tab / Ctrl+Tab — cycle windows             */

bool     kbd_has_input(void);
int      kbd_poll_char(void);   /* Latin-1 char, or -1 for none/non-char */
bool     kbd_enable_irq(void);  /* switch to IRQ1 -> IPC queue events    */
void     kbd_set_layout(int layout);   /* 0=us 1=es 2=mac 3=jp 4=cn */
int      kbd_layout(void);
const char *kbd_layout_name(void);     /* "us" | "es" | "mac" | "jp" | "cn" */

/* --- ui/events.c : input event source selection --------------------------- */
void     events_use_irq(bool on);   /* true: consume the IPC queue + hlt idle */
bool     events_irq_active(void);

/* --- kernel.c ------------------------------------------------------------ */
ROGEX_NORETURN void panic(const char *msg);

/* --- idt.c --------------------------------------------------------------- */
void idt_install(void);          /* called from boot.asm's load_idt */

/* --- mem.c : physical frame allocator + 4-level paging ------------------- */
#define PAGE_SIZE 4096ULL
struct boot_handoff;
void     pmm_init(const struct boot_handoff *bh);
uint64_t pmm_alloc_frame(void);              /* -> physical addr, or 0 */
uint64_t pmm_alloc_contig(uint32_t pages);  /* one flat run of frames, or 0  */
uint64_t pmm_total_bytes(void);   /* all usable RAM the firmware reports     */
uint64_t pmm_usable_bytes(void);  /* the part inside the identity map (4 GiB) */
uint64_t pmm_free_bytes(void);
uint64_t pmm_used_bytes(void);    /* frames actually handed out              */
bool     pmm_memtest(uint64_t max_bytes, uint64_t *out_tested, uint64_t *out_errors);

/* The firmware memory map as reported, region by region. Multiboot2 types:
   1 = available, 3 = ACPI reclaimable, 4 = ACPI NVS, 5 = bad, else reserved. */
typedef struct { uint64_t base, len; uint32_t type; } pmm_region_t;
int                 pmm_region_count(void);      /* stored (capped)          */
int                 pmm_region_total_seen(void); /* reported by the firmware */
const pmm_region_t *pmm_region(int i);
/* Map one 4 KiB page virt->phys in the active address space. flags: PTE bits */
bool     vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);

#define PTE_PRESENT  (1ULL << 0)
#define PTE_WRITABLE (1ULL << 1)
#define PTE_USER     (1ULL << 2)
#define PTE_NX       (1ULL << 63)

/* --- tiny freestanding mem helpers (kernel.c) ---------------------------- */
void    *k_memset(void *dst, int v, size_t n);
void    *k_memcpy(void *dst, const void *src, size_t n);
size_t   k_strlen(const char *s);
char    *k_strncat(char *dest, const char *src, size_t n);
int      k_snprintf(char *buf, size_t n, const char *fmt, ...);

#endif /* ROGEX_H */
