/* Window manager. */
#ifndef RXOS_WINDOW_H
#define RXOS_WINDOW_H

#include "rogex.h"

#define WM_MAX_WINDOWS 8
#define WM_WORKSPACES  3
#define WM_BAR_H       24        /* top status bar height (slim Smoke Aero) */
#define WM_TASK_H      0         /* bottom taskbar removed — the dock owns the bottom edge */
#define WM_TITLE_H     26        /* per-window glass title bar              */
#define WM_GAP         8         /* gap between tiles                       */
#define WM_CORNER      8         /* rounded window radius                   */

/* WK_TERMINAL stays last: `n` cycles the kinds below it, and the terminal is
   opened with `t`. One window, up to two ROSH panes (split view). */
typedef enum { WK_FILES, WK_MONITOR, WK_ABOUT, WK_NEURO, WK_CALC,
               WK_DISKS, WK_PHOTOS, WK_SETTINGS, WK_BROWSER, WK_TERMINAL } wm_kind;
#define WM_KIND_COUNT 10

typedef struct wm_window {
    int      id;
    char     title[28];
    int      ws;                 /* workspace 0..WM_WORKSPACES-1 */
    bool     used;
    bool     floating;
    bool     hidden;             /* omitted from the next compose (drag cache) */
    bool     maximized;          /* fills the free desktop (bars stay) */
    int      x, y, w, h;         /* outer geometry (incl. title bar) */
    int      rest_x, rest_y, rest_w, rest_h; /* geometry before maximize */
    wm_kind  kind;
    int      sel;                /* per-window selection (file manager) */

    /* Resize state */
    bool     resizing;
    int      resize_edge;        /* 0=none, 1=N, 2=S, 3=E, 4=W, 5=NE, 6=NW, 7=SE, 8=SW */
    int      orig_w, orig_h;

    /* Text selection (terminal) — clipboard itself is global (Ctrl+C/V). */
    bool     selecting;
    int      sel_start_x, sel_start_y;
    int      sel_end_x, sel_end_y;

    /* Gradient colors for this window */
    uint32_t grad_top;
    uint32_t grad_bottom;
} wm_window;

/* Why the desktop handed control back. The window manager never reboots or
   powers the machine off from inside its own event loop — it returns and says
   what was asked for, so the shell can unwind, flush and print first. */
typedef enum { WM_ACTION_SHELL, WM_ACTION_REBOOT, WM_ACTION_OFF, WM_ACTION_HALT } wm_action;

/* Enter the desktop. Returns when the user drops back to the classic shell.
   State survives across calls: leaving and coming back finds the same windows
   in the same places, which is the difference between a desktop and a screen
   that gets rebuilt every time you glance away. */
void wm_run(void);

/* Nova — the EchOS browser. Opens (or focuses) the browser window pointed
   at `url` ("rgx://home" for the start page). Callable from hotkeys and
   from the shell (`nova <url>`). */
void wm_open_nova(const char *url);
bool wm_active(void);                 /* desktop running (GUI spawns ok) */

/* What the last wm_run() was asked to do on the way out. */
wm_action wm_last_action(void);

/* True once wm_run() has built its windows at least once, so the shell can
   tell "first boot into the desktop" from "back again". */
bool wm_started(void);

#endif /* RXOS_WINDOW_H */
