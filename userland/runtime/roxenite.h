/* Roxenite / RX-C runtime. */
#ifndef ROGEX_RXC_H
#define ROGEX_RXC_H

#include "rogex.h"

/* --- RXIR: the bytecode model (target of v0.2 `.rxbc`). The v0.1 interpreter
   walks the AST directly; these ops define the eventual serialized form. --- */
typedef enum {
    RX_OP_APP,           /* app "name"                         */
    RX_OP_PAGE,          /* page <name>:                       */
    RX_OP_TITLE,         /* title "..."                        */
    RX_OP_TEXT,          /* text  "..."                        */
    RX_OP_VALUE,         /* value <state>                      */
    RX_OP_BUTTON_OPEN,   /* button "label" opens "rgx://..."   */
    RX_OP_BUTTON_ACTION, /* button "label": <actions>          */
    RX_OP_STATE_INT,     /* state <name> is <int>              */
    RX_OP_STATE_STRING,  /* state <name> is "..."              */
    RX_OP_ADD_INT,       /* add <n> to <state>                 */
    RX_OP_SET_INT,       /* set <state> to <int>               */
    RX_OP_SET_STRING,    /* set <state> to "..."               */
    RX_OP_OPEN_URL,      /* open "rgx://..."                   */
    RX_OP_PRINT,         /* print "..."                        */
    RX_OP_CLEAR,         /* clear                              */
    RX_OP_HALT,
} rx_op;

/* --- sizes (fixed, no allocation) --- */
#define RXC_STR          64
#define RXC_MAX_STATES   16
#define RXC_MAX_PAGES    8
#define RXC_MAX_NODES    96
#define RXC_MAX_BUTTONS  32
#define RXC_MAX_ACTIONS  64
#define RXC_ERR_LEN      128

typedef enum { RXV_INT, RXV_STR } rxc_vtype;
typedef struct {
    char      name[RXC_STR];
    rxc_vtype type;
    long      ival;
    char      sval[RXC_STR];
} rxc_state;

typedef enum { RXN_TITLE, RXN_TEXT, RXN_VALUE, RXN_BUTTON } rxc_nodekind;
typedef struct {
    rxc_nodekind kind;
    char text[RXC_STR];   /* literal (title/text) or state name (value) or label */
    int  button;          /* index into buttons[] when kind==RXN_BUTTON */
} rxc_node;

typedef enum { RXA_OPEN, RXA_PRINT, RXA_CLEAR, RXA_ADD, RXA_SET_INT, RXA_SET_STR } rxc_akind;
typedef struct {
    rxc_akind kind;
    char arg[RXC_STR];    /* url / print text / target state name */
    long ival;            /* add amount / set-int value */
    char sval[RXC_STR];   /* set-string value */
} rxc_action;

typedef struct {
    char label[RXC_STR];
    char open_url[RXC_STR];   /* non-empty => "opens" button */
    int  action_start, action_count;
} rxc_button;

typedef struct {
    char name[RXC_STR];
    int  node_start, node_count;
} rxc_page;

typedef struct {
    char       app_name[RXC_STR];
    rxc_state  states[RXC_MAX_STATES];   int nstates;
    rxc_page   pages[RXC_MAX_PAGES];     int npages;
    rxc_node   nodes[RXC_MAX_NODES];     int nnodes;
    rxc_button buttons[RXC_MAX_BUTTONS]; int nbuttons;
    rxc_action actions[RXC_MAX_ACTIONS]; int nactions;
    bool       ok;
    char       error[RXC_ERR_LEN];
} rxc_program;

/* Compile RX-C source into the program. On failure prog->ok is false and
   prog->error holds a human-readable message. Never crashes on bad input. */
void rxc_compile(const char *src, rxc_program *prog);

/* Render pages + handle button presses (state actions / page switches) via
   render.c + console input, until the user types 'back'. If a button opens an
   external rgx:// page, that URL is written to `open_out` and the function
   returns true so the shell can navigate the OS to it. */
bool rxc_run(rxc_program *prog, char *open_out, size_t max);

/* --- app state persistence (the Roxenite "backend") -----------------------
   A running app mutates prog->states (e.g. `add 1 to visits`). To make that
   survive closing/reopening the app — and reboots, once persisted to disk —
   the shell serializes the states to a sidecar file "<app>.rxstate" after a
   run and re-applies them after the next compile. Format: one `name=value`
   per line (strings are quoted). */
uint32_t rxc_state_save(const rxc_program *prog, char *out, uint32_t max);
void     rxc_state_apply(rxc_program *prog, const char *data, uint32_t len);

/* Built-in demo sources (no filesystem yet). name: "demo" | "hello". */
const char *rxc_demo_source(const char *name);
void        rxc_help(void);
void        rxc_list(void);

#endif /* ROGEX_RXC_H */
