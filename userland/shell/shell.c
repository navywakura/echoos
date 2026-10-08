/*
 * Desktop shell: left nav, views, command bar.
 * Still "a browser with an account" on rgx:// — local resolution for now.
 */
#include "shell.h"
#include "arch.h"
#include "rogex_core.h"
#include "desktop.h"
#include "wallpaper_pack.h"
#include "rgxsite.h"
#include "roxenite.h"
#include "framebuffer.h"
#include "mouse.h"
#include "edit.h"
#include "rxfs.h"
#include "vfs.h"
#include "commands.h"
#include "rgx_protocol.h"
#include "disk.h"
#include "wired.h"
#include "www.h"
#include "netif.h"
#include "arch.h"
#include "window.h"
#include "rx_event.h"
#include "env.h"
#include "power.h"
#include "update.h"
#include "browse.h"
#include "gui.h"
#include "setup.h"
#include "i18n.h"
#include "clock.h"
#include "cli.h"
#include "bootmode.h"
#include "wizard.h"
#include "version.h"
#include "layout.h"
#include "epk.h"

#define PSEUDONYM_MAX 32

#define PASSWORD_MAX 48

static struct {
    char    pseudonym[PSEUDONYM_MAX + 1];
    char    email[48];
    char    pass_hash[65];   /* hex SHA3-256 (64) + NUL */
    int     theme;
    uint8_t uid[ROGEX_UID_BYTES];
    uint8_t binding[ROGEX_UID_BYTES];
    bool    logged_in;
    bool    has_password;
    char    lang[8];
    char    locale[16];
    char    timezone[40];
    int     tzoff;
} g_account;

static const hwid_t *g_hw;

enum { V_HOME, V_PAGE, V_YOU, V_SETTINGS, V_EDITOR, V_APP, V_TEXT, V_WIRED };
static int  g_view = V_HOME;
static char g_page_url[64] = "rgx://home";
static char g_edit_file[RXFS_NAME] = "myapp.rxc";
static char g_app_file[RXFS_NAME];        /* rxfs RX-C page being served (V_APP) */
static char g_term[8192];                 /* terminal view: last command output  */
static char g_status[80];
static bool g_halt;

/* ------------------------------ small helpers ---------------------------- */
static bool streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}
static void split(char *line, char **arg) {
    char *p = line;
    while (*p && *p != ' ') p++;
    if (*p == ' ') { *p = '\0'; p++; while (*p == ' ') p++; }
    *arg = p;
}
static void cpn(char *d, const char *s, uint32_t max) {
    uint32_t i = 0; for (; s[i] && i < max - 1; i++) d[i] = s[i]; d[i] = '\0';
}
static void set_status(const char *s) { cpn(g_status, s, sizeof(g_status)); }
static char *apps(char *d, const char *s, char *e) { while (*s && d < e - 1) *d++ = *s++; *d = '\0'; return d; }
/* uint64 -> decimal string (out must hold >= 21 bytes). */
static void u64s(char *out, uint64_t v) {
    char t[21]; int i = 0;
    if (!v) t[i++] = '0';
    while (v) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
    int j = 0; while (i) out[j++] = t[--i];
    out[j] = '\0';
}
static char *apphex(char *d, const uint8_t *b, int n, char *e) {
    static const char H[] = "0123456789abcdef";
    for (int i = 0; i < n && d < e - 2; i++) { *d++ = H[b[i] >> 4]; *d++ = H[b[i] & 0xf]; }
    *d = '\0'; return d;
}

/* --------------------------- built-in rgx:// pages ----------------------- */
static const rogex_page PAGES[] = {
    { "rgx://laws", "The System of Laws",
      "1. One immutable pseudonym per hardware UID (TPM 2.0 + CPU + MAC).\n"
      "2. Moderation by consensus: Proof-of-Report. A terminal is banned from\n"
      "   the mesh only after multiple distinct users report it.\n"
      "3. The blacklist is distributed; banned terminals lose mesh access.\n\n"
      "Use the command bar: report <name>   |   bans" },
    { "rgx://about", "User Guide",
      "EchOS 1.0 ECLIPSE - Eclipse Shell, Nova browser, optional WWW.\n"
      "By Rogex Laboratories. Experimental research OS - not production.\n\n"
      "Left nav: Home, Social, Laws, You, Editor, Settings, Halt - all clickable.\n"
      "Home has guide buttons. Editor writes RX-C apps and saves them to RXFS.\n"
      "Settings changes the theme, your email, and detects hardware.\n\n"
      "Command bar (bottom): help | status | ls | cat | write | run | devices\n"
      "                      go rgx://... | rx demo | report <name>" },
    { "rgx://guide", "How to build a page and an app",
      "Everything on rgx:// is written in Roxenite (RX-C). A page IS an app.\n"
      " \n"
      "1. Open the Editor (left nav, or the Home button).\n"
      "2. Click a line, then type. No arrow keys - click to place the caret.\n"
      "   Try this minimal page:\n"
      " \n"
      "      app \"My Page\"\n"
      "      page home:\n"
      "        title \"Hello rgx://\"\n"
      "        text \"My first page on the mesh.\"\n"
      "        button \"Open Laws\" opens \"rgx://laws\"\n"
      " \n"
      "3. Click Save. Give it a name like  mypage.rxc  (files end in .rxc).\n"
      "4. Serve it: in the command bar type   go rgx://mypage\n"
      "   EchOS runs your Roxenite source as a live page - that is the backend.\n"
      " \n"
      "Make it an app (state + actions):\n"
      "      state counter is 0\n"
      "      value counter          (shows the value)\n"
      "      button \"Add\":\n"
      "        add 1 to counter     (indent actions under the button)\n"
      " \n"
      "Actions: add <n> to <state> | set <state> to <value> | open \"rgx://..\"\n"
      "Your app is sandboxed: UI + navigation + its own state only." },
};
#define NPAGES (sizeof(PAGES) / sizeof(PAGES[0]))
static const rogex_page *lookup_page(const char *url) {
    for (size_t i = 0; i < NPAGES; i++) if (streq(PAGES[i].url, url)) return &PAGES[i];
    return NULL;
}

/* Public hook so Nova (ui/window.c via browse.c) can render rgx:// pages. */
const char *rgx_local_page(const char *url, char *out, size_t max) {
    const rogex_page *p = lookup_page(url);
    size_t n = 0;
    if (!p) return NULL;
    if (out) {
        const char *s = p->title;
        while (*s && n < max - 2) out[n++] = *s++;
        out[n++] = '\n';
        for (s = p->body; *s && n < max - 1; ) out[n++] = *s++;
        out[n] = '\0';
    }
    return p->title;
}

/* If an EchOS-formatted disk is attached, snapshot the whole filesystem so a
   change (profile, app state) survives reboots without a manual `save`. It is
   one image, so this persists the entire current FS. */
static void disk_sync(void) {
    if (disk_present() && rxfs_disk_has_image((struct blockdev *)disk_dev()))
        rxfs_save((struct blockdev *)disk_dev());
}

/* --------------------------------- profile ------------------------------- */
/* SHA3-256(password || uid) as lowercase hex into out (65 bytes). */
static void hash_password(const char *pass, char out[65]) {
    uint8_t material[PASSWORD_MAX + ROGEX_UID_BYTES];
    uint32_t plen = (uint32_t)k_strlen(pass);
    if (plen > PASSWORD_MAX) plen = PASSWORD_MAX;
    k_memcpy(material, pass, plen);
    k_memcpy(material + plen, g_account.uid, ROGEX_UID_BYTES);
    uint8_t dig[32];
    rogex_sha3_256(material, plen + ROGEX_UID_BYTES, dig);
    static const char H[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2]     = H[dig[i] >> 4];
        out[i * 2 + 1] = H[dig[i] & 0xf];
    }
    out[64] = '\0';
}

static bool password_ok(const char *pass) {
    if (!g_account.has_password) return true;
    char h[65];
    hash_password(pass, h);
    return streq(h, g_account.pass_hash);
}

/* Read a line with '*' echo (password). Serial + keyboard. Returns length. */
static size_t read_secret(char *buf, size_t max) {
    size_t n = 0;
    if (max == 0) return 0;
    buf[0] = '\0';
    for (;;) {
        int ch = -1;
        /* Serial first so headless tests can type over COM1. Then the unified
           input path (IRQ ring or polled drivers) — never kbd_poll_char while
           IRQ1 owns the port. */
        int sc = console_serial_getc_nb();
        if (sc >= 0) ch = sc;
        else {
            int raw = 0, dx = 0, dy = 0, btn = 0;
            int kind = ui_next_raw(&raw, &dx, &dy, &btn);
            if (kind == 1) ch = raw;
            else continue;
        }
        if (ch == '\n' || ch == '\r') {
            console_putc('\n');
            buf[n] = '\0';
            return n;
        }
        if (ch == 0x7f || ch == '\b') {
            if (n > 0) {
                n--;
                buf[n] = '\0';
                console_write("\b \b");
            }
            continue;
        }
        if (ch == 3) { /* Ctrl-C: empty */
            buf[0] = '\0';
            console_putc('\n');
            return 0;
        }
        if (ch < 32 || ch == 0x7f) continue;
        if (n + 1 >= max) continue;
        buf[n++] = (char)ch;
        buf[n] = '\0';
        console_putc('*');
    }
}

static void profile_save(void) {
    char buf[512]; char *d = buf, *e = buf + sizeof(buf);
    d = apps(d, "pseudonym=", e); d = apps(d, g_account.pseudonym, e);
    d = apps(d, "\nemail=", e);   d = apps(d, g_account.email[0] ? g_account.email : "(none)", e);
    d = apps(d, "\ntheme=", e);   d = apps(d, fb_theme_name(), e);
    if (g_account.has_password && g_account.pass_hash[0]) {
        d = apps(d, "\npasshash=", e); d = apps(d, g_account.pass_hash, e);
    }
    if (g_account.lang[0]) {
        d = apps(d, "\nlang=", e); d = apps(d, g_account.lang, e);
    }
    if (g_account.locale[0]) {
        d = apps(d, "\nlocale=", e); d = apps(d, g_account.locale, e);
    }
    if (g_account.timezone[0]) {
        d = apps(d, "\ntimezone=", e); d = apps(d, g_account.timezone, e);
    }
    if (g_account.tzoff) {
        char n[8]; int v = g_account.tzoff, neg = 0, i = 0, t = 0;
        char tmp[8];
        if (v < 0) { neg = 1; v = -v; }
        if (!v) tmp[i++] = '0';
        while (v) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
        if (neg) n[t++] = '-';
        while (i) n[t++] = tmp[--i];
        n[t] = '\0';
        d = apps(d, "\ntzoff=", e); d = apps(d, n, e);
    }
    d = apps(d, "\n", e);
    vfs_write("/etc/profile", buf, (uint32_t)(d - buf));
    /* Legacy root path for older images. */
    vfs_write("profile", buf, (uint32_t)(d - buf));
    disk_sync();
}

/* State sidecar name for an app file: "<app>.rxstate" (in `out`). */
static void state_file(const char *app, char *out, uint32_t max) {
    char *d = out, *e = out + max;
    d = apps(d, app, e); apps(d, ".rxstate", e);
}

/* --------------------------------- views --------------------------------- */
static void periph_lines(void) {
    char b[96];
    { char *d = b, *e = b + sizeof(b); d = apps(d, "CPU: ", e); d = apps(d, g_hw->cpu_vendor, e); render_line(b); }
    { char *d = b, *e = b + sizeof(b);
      uint32_t w, h; fb_dims(&w, &h);
      d = apps(d, "Display: ", e);
      char n[8]; int t = 0, v = (int)w; char tmp[8]; int i = 0; if(!v)tmp[i++]='0'; while(v){tmp[i++]='0'+v%10;v/=10;} while(i)n[t++]=tmp[--i]; n[t]=0;
      d = apps(d, n, e); d = apps(d, "x", e);
      t = 0; v = (int)h; i = 0; if(!v)tmp[i++]='0'; while(v){tmp[i++]='0'+v%10;v/=10;} while(i)n[t++]=tmp[--i]; n[t]=0;
      d = apps(d, n, e); d = apps(d, " (framebuffer)", e); render_line(b); }
    render_line(mouse_present() ? "Mouse: PS/2 present" : "Mouse: not detected");
    { char *d = b, *e = b + sizeof(b); d = apps(d, "Keyboard: ", e); d = apps(d, kbd_layout_name(), e); render_line(b); }
    render_line(g_hw->tpm_present ? "TPM 2.0: present" : "TPM 2.0: absent");
}

static void render_home(void) {
    render_set_nav(NAV_HOME);
    render_begin_page("Home", "rgx://home");
    if (g_status[0]) { render_line(g_status); render_line(""); }
    char hi[80]; char *d = hi, *e = hi + sizeof(hi);
    d = apps(d, "Welcome, ", e); d = apps(d, g_account.pseudonym, e); apps(d, ".", e);
    render_line(hi);
    render_line("The OS is a browser with an account on the rgx:// mesh. Pick a tile:");
    render_line("");
    render_button("Social Club",        "meet the mesh  -  rgx://social", 1);
    render_button("The System of Laws", "how moderation works  -  rgx://laws", 2);
    render_button("Your Account",       "identity + stats  -  rgx://you", 3);
    render_button("Write an app",       "the Editor (a nano, improved)", 4);
    render_button("Settings",           "theme, email, peripherals", 5);
    render_button("Wired  (rgx:// mesh)", "ping peers by alias  -  L2 network", 8);
    render_button("Desktop", "wallpaper, icons, taskbar, apps", 9);
    render_button("Build a page / app", "step-by-step guide  -  rgx://guide", 6);
    render_button("User Guide",         "what EchOS is  -  rgx://about", 7);
    render_end_page();
}

static void render_you(void) {
    render_set_nav(NAV_YOU);
    render_begin_page("Your Account", "rgx://you");
    if (g_status[0]) { render_line(g_status); render_line(""); }
    char b[96], *d, *e;
    d = b; e = b + sizeof(b); d = apps(d, "Pseudonym: ", e); apps(d, g_account.pseudonym, e); render_line(b);
    d = b; e = b + sizeof(b); d = apps(d, "UID: ", e); d = apphex(d, g_account.uid, 12, e); apps(d, "...", e); render_line(b);
    d = b; e = b + sizeof(b); d = apps(d, "Email: ", e); apps(d, g_account.email[0] ? g_account.email : "(not set)", e); render_line(b);
    d = b; e = b + sizeof(b); d = apps(d, "Theme: ", e); apps(d, fb_theme_name(), e); render_line(b);
    d = b; e = b + sizeof(b); d = apps(d, "Blacklist size: ", e);
    { int v = rogex_blacklist_count(); char n[8]; int t=0,i=0; char tmp[8]; if(!v)tmp[i++]='0'; while(v){tmp[i++]='0'+v%10;v/=10;} while(i)n[t++]=tmp[--i]; n[t]=0; apps(d,n,e);} render_line(b);
    render_line("");
    render_line("Hardware:");
    periph_lines();
    render_line("");
    render_button("Settings", "", 1);
    render_button("Edit my profile file", "", 2);
    render_end_page();
}

static void render_settings(void) {
    render_set_nav(NAV_SETTINGS);
    render_begin_page("Settings", "rgx://settings");
    if (g_status[0]) { render_line(g_status); render_line(""); }
    render_line("Personalize your terminal and account.");
    render_line("");
    char b[64], *d, *e;
    d = b; e = b + sizeof(b); d = apps(d, "Theme: ", e); apps(d, fb_theme_name(), e);
    render_button(b, "click to cycle", 1);
    render_button("Set email", g_account.email[0] ? g_account.email : "not set", 2);
    d = b; e = b + sizeof(b); d = apps(d, "Keyboard: ", e); apps(d, kbd_layout() == 1 ? "ES" : "US", e);
    render_button(b, "click to toggle US/ES", 3);
    render_button("Detect peripherals", "", 4);
    d = b; e = b + sizeof(b); d = apps(d, "Wallpaper: ", e); apps(d, gui_wallpaper_name(), e);
    render_button(b, "next packed JPEG", 7);
    render_button("Change password", "", 8);
    render_button("Check for updates", "rx update check", 6);
    render_button("Back to Home", "", 5);
    render_line("");
    render_line("Detected peripherals:");
    periph_lines();
    render_end_page();
}

/* Terminal view: output of the last system command (commands.c), rendered as
   a page so it survives the shell's redraw loop and mirrors to serial. */
static void render_term(void) {
    render_set_nav(-1);
    render_begin_page("EchOS terminal", "rgx://term");
    const char *s = g_term;
    char line[132];
    int rows = 0;
    while (*s && rows < 26) {
        int n = 0;
        while (*s && *s != '\n' && n < (int)sizeof(line) - 1) line[n++] = *s++;
        line[n] = '\0';
        if (*s == '\n') s++;
        render_line(line);
        rows++;
    }
    if (*s) render_line("... (output truncated - the view fits 26 lines)");
    render_line("");
    render_button("Back to Home", "", 1);
    render_end_page();
}

/* Wired panel: our mesh identity (alias + MAC), link state, and the aliases
   that answered the last ping — pseudonyms only, never IPs. */
static void render_wired(void) {
    render_set_nav(-1);
    render_begin_page("Wired  -  rgx:// mesh", "rgx://wired");
    if (g_status[0]) { render_line(g_status); render_line(""); }
    char b[96], *d, *e;
    d = b; e = b + sizeof(b); d = apps(d, "Alias:   ", e); apps(d, g_account.pseudonym, e); render_line(b);
    if (net_present()) {
        char mac[24]; wired_mac_str(mac, sizeof(mac));
        d = b; e = b + sizeof(b); d = apps(d, "MAC:     ", e); apps(d, mac, e); render_line(b);
        d = b; e = b + sizeof(b);
        d = apps(d, "Link:    up  (", e); d = apps(d, net_driver_name(), e);
        apps(d, ", EtherType 0x88B5, no IP/DNS)", e);
        render_line(b);
        {   /* live event-fabric telemetry: proves the drain path is running */
            uint64_t evs, hints, ddl, spikes; uint32_t drop;
            net_actor_stats(&evs, &hints, &ddl, &spikes, &drop);
            char n[24];
            d = b; e = b + sizeof(b);
            d = apps(d, "Fabric:  ", e); u64s(n, evs);    d = apps(d, n, e);
            d = apps(d, " ev (",  e);    u64s(n, hints);  d = apps(d, n, e);
            d = apps(d, " hint / ", e);  u64s(n, ddl);    d = apps(d, n, e);
            d = apps(d, " deadline), ", e); u64s(n, spikes); d = apps(d, n, e);
            d = apps(d, " spikes, ", e); u64s(n, (uint64_t)drop); d = apps(d, n, e);
            apps(d, " drop", e);
            render_line(b);
        }
    } else {
        render_line("MAC:     -");
        render_line("Link:    DOWN  (attach a NIC — `nics` lists PCI chips)");
    }
    render_line("");
    render_button("Mesh", "ping / chat / ACK", 2);
    render_button("Browse", "simple HTML", 3);
    render_button("Trace", "alias or IP", 4);
    render_line("");
    d = b; e = b + sizeof(b); d = apps(d, "Peers who answered (", e);
    { int v=wired_reply_count(); char n[8]; int t=0,i=0; char tmp[8]; if(!v)tmp[i++]='0'; while(v){tmp[i++]='0'+v%10;v/=10;} while(i)n[t++]=tmp[--i]; n[t]=0; d=apps(d,n,e);} apps(d, "):", e);
    render_line(b);
    if (wired_reply_count() == 0) render_line("  (none yet - try:  ping <alias>)");
    for (int i = 0; i < wired_reply_count(); i++) {
        d = b; e = b + sizeof(b); d = apps(d, "  * ", e);
        d = apps(d, wired_reply_alias(i), e);
        d = apps(d, "  ACK ", e);
        { char n[12]; u64s(n, (uint64_t)wired_reply_rtt_ms(i)); d = apps(d, n, e); }
        apps(d, " ms", e);
        render_line(b);
    }
    if (wired_chat_acked()) {
        d = b; e = b + sizeof(b);
        d = apps(d, "Last chat ACK: ", e); apps(d, wired_chat_ack_from(), e);
        render_line(b);
    }
    render_line("");
    render_line("last page:");
    render_line(browse_last_url());
    render_line("");
    render_button("Back to Home", "", 1);
    render_end_page();
}

/* ------------------- neuro: interactive LIF neuron demo ------------------- */
/* The verifiable neuromorphic demo (docs/event_fabric.md): a real rx_actor_t
   whose stimuli are YOUR keystrokes. Type slowly and the leak wins (the
   membrane decays below threshold between keys — suppressed). Type fast and
   the potential integrates across events until it crosses threshold: SPIKE,
   reset, refractory. The synapse weight evolves by STDP underneath. All in
   Q16.16 on the same fabric the network drain runs on. */
static rx_actor_t g_demo_neuron;
static int        g_demo_spikes;
static bool demo_neuron_fire(rx_actor_t *self, const rx_event_t *ev) {
    (void)self; (void)ev;
    g_demo_spikes++;
    return true;
}

/* Render a Q16.16 value as "i.ff" (two decimals) into out. */
static void q16s(char *out, int32_t q) {
    char n[24];
    u64s(n, (uint64_t)(q >> 16));
    char *d = out, *e = out + 16;
    d = apps(d, n, e); d = apps(d, ".", e);
    uint32_t frac = ((uint32_t)(q & 0xffff) * 100u) >> 16;
    n[0] = (char)('0' + frac / 10); n[1] = (char)('0' + frac % 10); n[2] = 0;
    apps(d, n, e);
}

static void neuro_render(void) {
    uint64_t now = arch_timer_ticks();
    int32_t v = rx_actor_peek_potential(&g_demo_neuron, now);
    char b[96], n[24], *d, *e;

    render_set_nav(-1);
    render_begin_page("Neuro  -  live LIF neuron (event fabric)", "rgx://neuro");
    render_line("Your keystrokes are the stimuli (+0.35 each).");
    render_line("Type SLOWLY: the leak wins, no spike. Type FAST: it fires.");
    render_line("");

    /* membrane bar: 40 cells up to threshold */
    int cells = (int)(((int64_t)v * 40) / g_demo_neuron.threshold_q16);
    if (cells > 40) cells = 40;
    d = b; e = b + sizeof(b);
    d = apps(d, "membrane [", e);
    for (int i = 0; i < 40; i++) d = apps(d, i < cells ? "#" : ".", e);
    d = apps(d, "] ", e); q16s(n, v); apps(d, n, e);
    render_line(b);
    d = b; d = apps(d, "threshold 1.00   tau ", e);
    u64s(n, g_demo_neuron.tau_ticks); d = apps(d, n, e);
    d = apps(d, " ticks   refractory ", e);
    u64s(n, g_demo_neuron.refractory_ticks); d = apps(d, n, e);
    apps(d, " ticks", e);
    render_line(b);
    render_line("");

    d = b; d = apps(d, "SPIKES: ", e); u64s(n, (uint64_t)g_demo_spikes);
    d = apps(d, n, e);
    d = apps(d, "    suppressed: ", e);
    u64s(n, g_demo_neuron.stats.suppressed); d = apps(d, n, e);
    d = apps(d, "    events: ", e);
    u64s(n, g_demo_neuron.stats.received); apps(d, n, e);
    render_line(b);

    d = b; d = apps(d, "STDP synapse weight: ", e);
    q16s(n, g_demo_neuron.synapse.weight_q16); d = apps(d, n, e);
    apps(d, "  (moves on every event/spike pair)", e);
    render_line(b);
    render_line("");
    render_line("[any key] stimulate      [Esc or q] back to the shell");
    render_end_page();
}

static void do_neuro(void) {
    rx_actor_init(&g_demo_neuron, "demo_lif", demo_neuron_fire,
                  /* threshold  */ Q16_ONE,
                  /* reset      */ 0,
                  /* tau        */ 100,   /* 1 s leak at PIT 100 Hz          */
                  /* refractory */ 30);   /* 300 ms                          */
    g_demo_spikes = 0;
    neuro_render();
    uint64_t last_draw = arch_timer_ticks();
    for (;;) {
        char ch = 0; int click = 0;
        int evk = ui_poll(&ch, &click);
        uint64_t now = arch_timer_ticks();
        if (evk == 1) {
            if (ch == 27 || ch == 'q') return;
            rx_actor_stimulate(&g_demo_neuron, RX_EVENT_SELFTEST,
                               (Q16_ONE * 35) / 100, now);
            neuro_render(); last_draw = now;
        } else if (now - last_draw >= 20) {     /* ~5 Hz decay redraw        */
            neuro_render(); last_draw = now;
        }
    }
}

/* Broadcast a PING for `target` and service the mesh briefly to collect
   PONG replies (the idle hook also services it, but we spin here so results
   are ready when we render). */
static void do_ping(const char *target) {
    if (!wired_up()) { set_status("network down. Attach a NIC (virtio-net or e1000)."); g_view = V_WIRED; return; }
    if (!target[0])  { set_status("usage: ping <alias>"); g_view = V_WIRED; return; }
    wired_clear_replies();
    if (!wired_send_ping(target)) { set_status("ping: send failed."); g_view = V_WIRED; return; }
    uint64_t start = arch_timer_ticks();
    while (arch_timer_ticks() - start < 150) {              /* ~1.5 s at 100 Hz     */
        wired_service();
        arch_idle_wait();                            /* wake on the next tick */
    }
    set_status(wired_reply_count() ? "ping complete - replies below." : "ping sent - no reply (is the peer up?).");
    g_view = V_WIRED;
}

static void render_view(void) {
    switch (g_view) {
    case V_YOU:      render_you(); break;
    case V_SETTINGS: render_settings(); break;
    case V_TEXT:     render_term(); break;
    case V_WIRED:    render_wired(); break;
    case V_HOME:     render_home(); break;
    case V_PAGE:
    default: {
        /* set nav highlight for known pages */
        if (streq(g_page_url, "rgx://social")) render_set_nav(NAV_SOCIAL);
        else if (streq(g_page_url, "rgx://laws")) render_set_nav(NAV_LAWS);
        else render_set_nav(-1);
        if (!rgxsite_try(g_page_url)) {
            const rogex_page *p = lookup_page(g_page_url);
            if (p) render_page(p);
            else { render_begin_page("Not found", g_page_url);
                   render_line("rgx:// resolve failed (DHT resolution is roadmap).");
                   render_end_page(); }
        }
        if (g_status[0]) { console_write("\n  "); console_write(g_status); console_putc('\n'); }
        break;
    }
    }
}

/* ---------------------------- navigation glue ---------------------------- */
/* The Roxenite backend: an rgx:// URL can be served by an RX-C source stored
   in RXFS. Resolution lives in userland/runtime/rgx_protocol.c: write a page
   in the Editor, Save it, then `go rgx://<name>` serves it live. */
static void goto_page(const char *url) {
    char file[RXFS_NAME];
    if (rgx_resolve(url, file, sizeof(file)) == RGX_ROUTE_APP) {
        cpn(g_app_file, file, sizeof(g_app_file));
        cpn(g_page_url, url, sizeof(g_page_url));
        g_view = V_APP;
        return;
    }
    cpn(g_page_url, url, sizeof(g_page_url));
    g_view = V_PAGE;
}

static void apply_nav(int tab) {
    set_status("");
    switch (tab) {
    case NAV_HOME:     g_view = V_HOME; break;
    case NAV_SOCIAL:   goto_page("rgx://social"); break;
    case NAV_LAWS:     goto_page("rgx://laws"); break;
    case NAV_YOU:      g_view = V_YOU; break;
    case NAV_EDITOR:   g_view = V_EDITOR; break;
    case NAV_SETTINGS: g_view = V_SETTINGS; break;
    case NAV_HALT:     g_halt = true; break;
    }
}

static void set_email(void) {
    console_write("  email> ");
    char buf[48];
    ui_line(buf, sizeof(buf), false);
    if (buf[0]) { cpn(g_account.email, buf, sizeof(g_account.email)); profile_save(); set_status("email saved to your profile."); }
}

static void enter_desktop(void);

static void content_click(int id) {
    set_status("");
    if (g_view == V_HOME) {
        switch (id) {
        case 1: goto_page("rgx://social"); break;
        case 2: goto_page("rgx://laws"); break;
        case 3: g_view = V_YOU; break;
        case 4: g_view = V_EDITOR; break;
        case 5: g_view = V_SETTINGS; break;
        case 6: goto_page("rgx://guide"); break;
        case 7: goto_page("rgx://about"); break;
        case 8: set_status(""); g_view = V_WIRED; break;
        case 9: enter_desktop(); break;
        }
    } else if (g_view == V_YOU) {
        if (id == 1) g_view = V_SETTINGS;
        else if (id == 2) { cpn(g_edit_file, "profile", sizeof(g_edit_file)); g_view = V_EDITOR; }
    } else if (g_view == V_SETTINGS) {
        switch (id) {
        case 1: fb_set_theme((fb_theme() + 1) % RGX_THEME_COUNT); g_account.theme = fb_theme(); profile_save(); set_status("theme changed."); break;
        case 2: set_email(); break;
        case 3: kbd_set_layout(kbd_layout() == 1 ? 0 : 1); set_status(kbd_layout() == 1 ? "keyboard: ES (accents on)" : "keyboard: US"); break;
        case 4: set_status("peripherals refreshed."); break;
        case 5: g_view = V_HOME; break;
        case 6:
            update_cmd("update", "check", "", g_term, sizeof(g_term));
            g_view = V_TEXT;
            break;
        case 7: {
            int n = gui_wp_count();
            if (n > 0) {
                static int wp_i;
                wp_i = (wp_i + 1) % n;
                gui_wallpaper_load_index(wp_i);
                vfs_write("/etc/wallpaper", gui_wp_name_at(wp_i),
                          (uint32_t)k_strlen(gui_wp_name_at(wp_i)));
                disk_sync();
                set_status(gui_wallpaper_name());
            }
            break;
        }
        case 8: {
            console_write("  new password> ");
            char p1[48], p2[48];
            ui_line(p1, sizeof(p1), false);
            console_write("  again> ");
            ui_line(p2, sizeof(p2), false);
            if (!p1[0] || !streq(p1, p2)) set_status("password not changed.");
            else {
                hash_password(p1, g_account.pass_hash);
                g_account.has_password = true;
                profile_save();
                set_status("password updated.");
            }
            break;
        }
        }
    } else if (g_view == V_WIRED) {
        if (id == 1) g_view = V_HOME;
        else if (id == 2) { set_status("tab Mesh — ping <alias> / chat / say"); }
        else if (id == 3) {
            browse_cmd("", g_term, sizeof(g_term));
            g_view = V_TEXT;
        }
        else if (id == 4) {
            trace_cmd("", g_term, sizeof(g_term));
            g_view = V_TEXT;
        }
    } else if (g_view == V_TEXT) {
        if (id == 1) g_view = V_HOME;
    }
}

/* ------------------------------ the desktop -------------------------------
   One way in and one way out. The desktop can ask for a reboot, a soft-off or
   a halt from its Start menu, and it deliberately does none of those itself:
   it returns, and this decides, so the shell has unwound and the screen is in
   a known state before the machine goes away. */
static void enter_desktop(void) {
    wm_run();
    switch (wm_last_action()) {
    case WM_ACTION_REBOOT:
        system_reboot();                       /* does not return           */
        break;
    case WM_ACTION_OFF:
        console_clear();
        console_write("\n[echOS] ACPI soft-off...\n");
        power_soft_off();
        /* Still here: the firmware did not honour S5. Say so rather than
           leaving a machine that looks hung with no explanation. */
        console_write("[echOS] the firmware did not power off (");
        console_write(power_soft_off_status());
        console_write("). Halting instead.\n");
        g_halt = true;
        break;
    case WM_ACTION_HALT:
        g_halt = true;
        break;
    case WM_ACTION_SHELL:
    default:
        set_status("classic shell. `wm` returns to the desktop.");
        break;
    }
}

/* ----------------------------- command bar ------------------------------- */
static void run_rx_source(const char *src) {
    static rxc_program prog;
    rxc_compile(src, &prog);
    char open_url[64];
    if (rxc_run(&prog, open_url, sizeof(open_url))) goto_page(open_url);
}

static void handle_command(char *line) {
    set_status("");

    /* System commands first (help/status/mem/ls/cat/write/run/devices/...).
       They tokenize their input, so hand them a scratch copy — GUI commands
       below still need the original line intact. */
    {
        char sys[96], runfile[RXFS_NAME];
        cpn(sys, line, sizeof(sys));
        switch (commands_dispatch(sys, runfile, sizeof(runfile), g_term, sizeof(g_term))) {
        case CMD_TEXT:     g_view = V_TEXT; return;
        case CMD_RUN_FILE: cpn(g_app_file, runfile, sizeof(g_app_file));
                           cpn(g_page_url, "rgx://run", sizeof(g_page_url));
                           g_view = V_APP; return;
        case CMD_CLEAR:
            g_term[0] = '\0';
            g_view = V_HOME;
            console_clear();
            set_status("terminal cleared.");
            return;
        case CMD_HALT:     g_halt = true; return;
        case CMD_NOT_HANDLED:
            if (echos2_cli_only()) {
                char name[48];
                int i = 0;
                const char *pkg;
                while (line[i] && line[i] != ' ' && i < 47) { name[i] = line[i]; i++; }
                name[i] = '\0';
                pkg = epk_pkg_for_command(name);
                g_term[0] = '\0';
                if (pkg && !epk_is_installed(pkg)) {
                    char *d = g_term;
                    const char *p = name;
                    while (*p && d < g_term + sizeof(g_term) - 1) *d++ = *p++;
                    p = ": not installed. Try:  epk install ";
                    while (*p && d < g_term + sizeof(g_term) - 1) *d++ = *p++;
                    p = pkg;
                    while (*p && d < g_term + sizeof(g_term) - 1) *d++ = *p++;
                    *d++ = '\n'; *d = '\0';
                } else {
                    char *d = g_term;
                    const char *p = name;
                    while (*p && d < g_term + sizeof(g_term) - 1) *d++ = *p++;
                    p = ": command not found\n";
                    while (*p && d < g_term + sizeof(g_term) - 1) *d++ = *p++;
                    *d = '\0';
                }
                g_view = V_TEXT;
                return;
            }
            break;
        }
    }

    char *arg; split(line, &arg);
    if (streq(line, "home")) g_view = V_HOME;
    else if (streq(line, "go") || streq(line, "open")) goto_page(arg);
    else if (streq(line, "social")) goto_page("rgx://social");
    else if (streq(line, "laws")) goto_page("rgx://laws");
    else if (streq(line, "guide")) goto_page("rgx://guide");
    else if (streq(line, "ping")) do_ping(arg);
    else if (streq(line, "wired") || streq(line, "mesh")) { set_status(""); g_view = V_WIRED; }
    else if (streq(line, "wm") || streq(line, "desktop")) enter_desktop();
    else if (streq(line, "neuro")) { do_neuro(); set_status("back from the LIF neuron demo."); }
    else if (streq(line, "you") || streq(line, "account") || streq(line, "id") || streq(line, "whoami")) g_view = V_YOU;
    else if (streq(line, "settings")) g_view = V_SETTINGS;
    else if (streq(line, "editor") || streq(line, "edit")) { if (arg[0]) cpn(g_edit_file, arg, sizeof(g_edit_file)); g_view = V_EDITOR; }
    else if (streq(line, "theme")) { fb_set_theme((fb_theme() + 1) % RGX_THEME_COUNT); g_account.theme = fb_theme(); profile_save(); set_status("theme changed."); }
    else if (streq(line, "kbd")) {
        if (streq(arg, "es")) kbd_set_layout(1);
        else if (streq(arg, "us")) kbd_set_layout(0);
        else if (streq(arg, "mac") || streq(arg, "us-mac")) kbd_set_layout(2);
        set_status(kbd_layout_name());
    }
    else if (streq(line, "rx")) {
        /* Handled by commands_dispatch above when it knows `rx`; if we land
           here the dispatch did not claim it — keep demo shortcut. */
        char *sub; split(arg, &sub);
        if (streq(arg, "demo") || streq(arg, "run")) {
            const char *s = rxc_demo_source(streq(arg, "demo") ? "demo" : sub);
            if (s) run_rx_source(s); else set_status("no such built-in app.");
        } else set_status("try: rx app help | rx app add hellopkg");
    }
    else if (streq(line, "report")) {
        if (!arg[0]) { set_status("usage: report <name>"); }
        else { uint8_t z[ROGEX_UID_BYTES] = {0}, subj[ROGEX_UID_BYTES];
               rogex_pseudonym_bind(z, (const uint8_t *)arg, (uint32_t)k_strlen(arg), subj);
               int r = rogex_report(g_account.uid, subj);
               set_status(r == 2 ? "consensus: terminal banned" : r == 1 ? "vote accepted" : r == 0 ? "already reported by you" : "already banned"); }
    }
    else if (streq(line, "bans")) { char b[40]; char *d = apps(b, "blacklist size: ", b + sizeof(b)); int v = rogex_blacklist_count(); char n[8]; int t=0,i=0; char tmp[8]; if(!v)tmp[i++]='0'; while(v){tmp[i++]='0'+v%10;v/=10;} while(i)n[t++]=tmp[--i]; n[t]=0; apps(d,n,b+sizeof(b)); set_status(b); }
    else if (streq(line, "halt") || streq(line, "exit")) g_halt = true;
    else set_status("unknown command - try the buttons, or: go <url> | rx demo | help");
}

/* --------------------------- first-run setup ----------------------------- */
/* Extract "key=value" (line-based) from the profile text into out. */
static bool profile_get(const char *text, uint32_t len, const char *key,
                        char *out, uint32_t max) {
    uint32_t klen = (uint32_t)k_strlen(key);
    uint32_t i = 0;
    while (i < len) {
        uint32_t start = i;
        while (i < len && text[i] != '\n') i++;
        if (i - start > klen && text[start + klen] == '=' ) {
            bool match = true;
            for (uint32_t k = 0; k < klen; k++)
                if (text[start + k] != key[k]) { match = false; break; }
            if (match) {
                uint32_t n = 0, v = start + klen + 1;
                while (v < i && n + 1 < max) out[n++] = text[v++];
                out[n] = '\0';
                return n > 0;
            }
        }
        if (i < len) i++;                       /* skip '\n' */
    }
    return false;
}

static void lock_screen(void) {
    setup_lock_screen(g_account.pseudonym, password_ok);
    g_account.logged_in = true;
}

/* Load profile from disk if present. Does NOT unlock — caller shows lock. */
static bool load_profile(void) {
    uint32_t len = 0;
    const char *p = vfs_read("/etc/profile", &len);
    if (!p) p = vfs_read("profile", &len);
    if (!p) return false;
    char name[PSEUDONYM_MAX + 1];
    if (!profile_get(p, len, "pseudonym", name, sizeof(name))) return false;

    cpn(g_account.pseudonym, name, sizeof(g_account.pseudonym));
    char email[48];
    if (profile_get(p, len, "email", email, sizeof(email)) && !streq(email, "(none)"))
        cpn(g_account.email, email, sizeof(g_account.email));
    char theme[16];
    if (profile_get(p, len, "theme", theme, sizeof(theme)))
        for (int t = 0; t < RGX_THEME_COUNT; t++) {
            fb_set_theme(t);
            if (streq(fb_theme_name(), theme)) break;
        }
    char ph[65];
    if (profile_get(p, len, "passhash", ph, sizeof(ph))) {
        cpn(g_account.pass_hash, ph, sizeof(g_account.pass_hash));
        g_account.has_password = true;
    } else {
        g_account.has_password = false;
        g_account.pass_hash[0] = '\0';
    }
    char lang[8], loc[16], tz[40], off[8];
    if (profile_get(p, len, "lang", lang, sizeof(lang)))
        cpn(g_account.lang, lang, sizeof(g_account.lang));
    if (profile_get(p, len, "locale", loc, sizeof(loc)))
        cpn(g_account.locale, loc, sizeof(g_account.locale));
    if (profile_get(p, len, "timezone", tz, sizeof(tz)))
        cpn(g_account.timezone, tz, sizeof(g_account.timezone));
    if (profile_get(p, len, "tzoff", off, sizeof(off))) {
        int v = 0, sig = 1; const char *q = off;
        if (*q == '-') { sig = -1; q++; }
        while (*q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); q++; }
        g_account.tzoff = v * sig;
    }
    setup_apply_locale(g_account.lang[0] ? g_account.lang : "en",
                       g_account.locale, g_account.timezone, g_account.tzoff);
    rogex_pseudonym_bind(g_account.uid, (const uint8_t *)g_account.pseudonym,
                         (uint32_t)k_strlen(g_account.pseudonym), g_account.binding);
    return true;
}

void account_create(const char *user, const char *pass,
                    const char *lang, const char *locale,
                    const char *tz, int tzoff) {
    if (user && user[0]) cpn(g_account.pseudonym, user, sizeof(g_account.pseudonym));
    else cpn(g_account.pseudonym, "RogexUser", sizeof(g_account.pseudonym));
    if (pass && pass[0]) {
        hash_password(pass, g_account.pass_hash);
        g_account.has_password = true;
    }
    if (lang) cpn(g_account.lang, lang, sizeof(g_account.lang));
    if (locale) cpn(g_account.locale, locale, sizeof(g_account.locale));
    if (tz) cpn(g_account.timezone, tz, sizeof(g_account.timezone));
    g_account.tzoff = tzoff;
    setup_apply_locale(g_account.lang, g_account.locale, g_account.timezone, tzoff);
    rogex_pseudonym_bind(g_account.uid, (const uint8_t *)g_account.pseudonym,
                         (uint32_t)k_strlen(g_account.pseudonym), g_account.binding);
    g_account.logged_in = true;
#if ECHOS_CLI_ONLY
    echos2_users_seed(g_account.pseudonym, lang);
#else
    {
        char home[RXFS_NAME];
        char *d = home, *e = home + sizeof(home);
        d = apps(d, "/home/", e);
        apps(d, g_account.pseudonym, e);
        vfs_mkdir(home);
        vfs_chdir(home);
    }
#endif
    profile_save();
    console_write("\n   Welcome, "); console_write(g_account.pseudonym);
    console_write(". Account saved.\n");
}

/* First-run registration: username + password, then save profile. */
static void register_account(void) {
    console_write(
        "\n  ================================================================\n"
        "   EchOS 1.0  -  Rogex Laboratories  -  initial registration\n"
        "  ================================================================\n"
        "   Your hardware UID: ");
    { char b[40]; apphex(b, g_account.uid, 8, b + sizeof(b)); console_write(b); }
    console_write(
        "...\n\n"
        "   Create your local account. The password unlocks the desktop\n"
        "   every boot (lock screen). Bound to this hardware UID.\n\n"
        "   username (max 32)> ");

    char name[PSEUDONYM_MAX + 1];
    size_t n = console_readline(name, sizeof(name));
    if (n == 0) cpn(name, "RogexUser", sizeof(name));
    cpn(g_account.pseudonym, name, sizeof(g_account.pseudonym));

    char pass[PASSWORD_MAX + 1], pass2[PASSWORD_MAX + 1];
    for (;;) {
        console_write("   password> ");
        if (read_secret(pass, sizeof(pass)) == 0) {
            console_write("   ! password cannot be empty\n");
            continue;
        }
        console_write("   confirm password> ");
        read_secret(pass2, sizeof(pass2));
        if (!streq(pass, pass2)) {
            console_write("   ! passwords do not match — try again\n");
            continue;
        }
        break;
    }
    hash_password(pass, g_account.pass_hash);
    g_account.has_password = true;

    rogex_pseudonym_bind(g_account.uid, (const uint8_t *)g_account.pseudonym,
                         (uint32_t)k_strlen(g_account.pseudonym), g_account.binding);
    g_account.logged_in = true;
    /* Home directory tree for the new user. */
#if ECHOS_CLI_ONLY
    echos2_users_seed(g_account.pseudonym, g_account.lang);
#else
    {
        char home[RXFS_NAME];
        char *d = home, *e = home + sizeof(home);
        d = apps(d, "/home/", e);
        apps(d, g_account.pseudonym, e);
        vfs_mkdir(home);
        vfs_chdir(home);
        {
            static const char *const SUBS[] = {
                "Images", "Videos", "Documents", "Downloads", "Desktop",
            };
            for (int i = 0; i < (int)(sizeof(SUBS) / sizeof(SUBS[0])); i++) {
                char sub[RXFS_NAME];
                char *sd = sub, *se = sub + sizeof(sub);
                sd = apps(sd, home, se);
                apps(sd, "/", se);
                apps(sd, SUBS[i], se);
                if (!vfs_mkdir(sub)) {
                    console_write("  [seed] mkdir failed: ");
                    console_write(sub);
                    console_write("\n");
                }
            }
        }
        {
            int n = wp_count();
            for (int i = 0; i < n; i++) {
                uint32_t len = 0;
                const uint8_t *data = wp_data_at(i, &len);
                if (!data || !len || len > 60000) continue;
                char img[RXFS_NAME];
                char *id = img, *ie = img + sizeof(img);
                id = apps(id, home, ie);
                apps(id, "/Images/", ie);
                apps(id, wp_name_at(i), ie);
                apps(id, ".jpg", ie);
                vfs_write(img, (const char *)data, len);
            }
        }
        {
            char note[RXFS_NAME];
            char *nd = note, *ne = note + sizeof(note);
            nd = apps(nd, home, ne);
            apps(nd, "/Desktop/readme.txt", ne);
            vfs_write(note,
                      "Los iconos del escritorio viviran aqui en una proxima"
                      " version.\n", 96);
        }
    }
#endif
    profile_save();
    console_write("\n   Welcome, "); console_write(g_account.pseudonym);
    console_write(". Account saved. Loading your desktop...\n");
}

static void setup_account(const hwid_t *hw) {
    k_memcpy(g_account.uid, hw->uid, ROGEX_UID_BYTES);
    g_account.email[0] = '\0';
    g_account.has_password = false;
    g_account.pass_hash[0] = '\0';
    g_account.logged_in = false;

    g_account.lang[0] = g_account.locale[0] = g_account.timezone[0] = '\0';
    g_account.tzoff = 0;

    if (bootmode_get() == BOOT_MEMTEST) return;

    if (load_profile()) {
        console_write("\n   Account found: ");
        console_write(g_account.pseudonym);
        console_write("\n");
        /* 2.0 LIVE / CLI: no lock screen — the TUI wizard already ran. */
        if (echos2_cli_only() || vfs_path_exists("/etc/echos-live")) {
            g_account.logged_in = true;
            console_write("   LIVE / CLI session. Type `echos-install` to install.\n");
            return;
        }
        if (g_account.has_password) {
            lock_screen();
        } else {
            /* Legacy profile without password — force a one-time set. */
            console_write("   No password on file. Set one now for the lock screen.\n");
            char pass[PASSWORD_MAX + 1], pass2[PASSWORD_MAX + 1];
            for (;;) {
                console_write("   password> ");
                if (read_secret(pass, sizeof(pass)) == 0) continue;
                console_write("   confirm > ");
                read_secret(pass2, sizeof(pass2));
                if (streq(pass, pass2)) break;
                console_write("   ! mismatch\n");
            }
            hash_password(pass, g_account.pass_hash);
            g_account.has_password = true;
            profile_save();
            g_account.logged_in = true;
        }
        console_write("   Welcome back, ");
        console_write(g_account.pseudonym);
        console_write(". Loading your desktop...\n");
        return;
    }
    /* LIVE with a writable disk and no serial script: GUI/text installer.
       Headless smoke tests (serial bytes already waiting, or no disk) keep
       the classic username/password registration so they do not hang. */
    if (setup_should_run()) {
        if (setup_run()) return;
    }
    register_account();
}

/* --------------------------------- loop ---------------------------------- */
/* Built-in apps are seeded by userland/init/init.c from the .rxc sources in
   userland/apps/ before the shell starts; the shell only assumes a mounted
   VFS. */
void wired_run(const hwid_t *hw) {
    g_hw = hw;
    render_init();
    echos2_boot();
    setup_account(hw);

    /* $USER is the pseudonym, so the ROSH prompt reads `[you @ rxos] #` and
       not `[anon @ rxos] #`. This has to happen after setup_account: before
       it, the pseudonym is either empty or last boot's, and the environment
       would be seeded with a name that is about to change. */
    env_init(g_account.pseudonym);

    /* Rogex Wired L2 + optional WWW stack (OFF until `www on`). */
    www_init();
    wired_init(g_account.pseudonym);
    wired_attach_ipstack();   /* demux 0x88B5 + IPv4 through one RX drain */
    /* Network RX through the kernel event fabric (wake-hint + deadline). */
    net_actor_online();

    /* Boot lands on the desktop, not on the browser shell's home page. The
       classic shell is still all of itself — Esc from the desktop, or the
       Start menu's "Classic shell" — and the desktop's own Terminal runs the
       same commands. What changed is only which of the two you see first.

       Serial-only boots (no framebuffer, or the automated tests driving the
       machine down COM1) skip it: wm_run() refuses without a framebuffer, and
       a test that expects a prompt should get a prompt. */
    if (!echos2_cli_only() && fb_active()) enter_desktop();

    char cmd[96];
    for (;;) {
        if (echos2_cli_only() && !g_halt) {
            const char *user = g_account.pseudonym[0] ? g_account.pseudonym : "live";
            const char *cwd = vfs_getcwd();
            console_write_prompt(user, "echos", cwd ? cwd : "/");
            if (console_readline(cmd, sizeof(cmd)) && cmd[0]) {
                commands_hist_add(cmd);
                handle_command(cmd);
                if (g_term[0]) {
                    console_write(g_term);
                    { size_t n = k_strlen(g_term);
                      if (n && g_term[n - 1] != '\n') console_write("\n"); }
                }
            }
            continue;
        }
        if (g_halt) {
            render_begin_page("Halted", "rgx://home");
            if (disk_present() && rxfs_disk_has_image((struct blockdev *)disk_dev()))
                render_line("EchOS halted. Files saved to disk persist; RAM state is gone.");
            else
                render_line("EchOS halted. Nothing was persisted (amnesic).");
            render_end_page();
            return;
        }
        if (g_view == V_EDITOR) {
            int nav = edit_run(g_edit_file);
            cpn(g_edit_file, "myapp.rxc", sizeof(g_edit_file));   /* reset default */
            if (nav < 0) g_view = V_HOME; else apply_nav(nav);
            continue;
        }
        if (g_view == V_APP) {
            uint32_t len = 0;
            const char *data = vfs_read(g_app_file, &len);
            if (!data) { set_status("page not found in filesystem."); g_view = V_HOME; continue; }
            static char src[RXFS_SIZE];
            uint32_t n = len < sizeof(src) - 1 ? len : sizeof(src) - 1;
            k_memcpy(src, data, n); src[n] = '\0';
            static rxc_program prog;
            rxc_compile(src, &prog);

            /* Roxenite backend: re-apply this app's saved state so counters
               and other state resume where they left off. */
            char sfile[RXFS_NAME]; state_file(g_app_file, sfile, sizeof(sfile));
            uint32_t slen = 0; const char *sdata = vfs_read(sfile, &slen);
            if (sdata) rxc_state_apply(&prog, sdata, slen);

            render_set_nav(-1);          /* a served app isn't a fixed nav tab */
            char open_url[64];
            bool navigated = rxc_run(&prog, open_url, sizeof(open_url));

            /* Persist the (possibly mutated) state back to the sidecar, then
               snapshot to disk so it survives a reboot too. */
            if (prog.ok && prog.nstates > 0) {
                static char sbuf[RXC_MAX_STATES * (RXC_STR + 8)];
                uint32_t w = rxc_state_save(&prog, sbuf, sizeof(sbuf));
                if (vfs_write(sfile, sbuf, w)) disk_sync();
            }

            if (navigated) goto_page(open_url);
            else g_view = V_HOME;
            continue;
        }
        render_view();
        console_write("  > ");
        int ev = ui_line(cmd, sizeof(cmd), true);
        if (ev >= RENDER_NAV_BASE && ev < RENDER_NAV_BASE + NAV_COUNT) apply_nav(ev - RENDER_NAV_BASE);
        else if (ev > 0) content_click(ev);
        else if (cmd[0]) handle_command(cmd);
    }
}
