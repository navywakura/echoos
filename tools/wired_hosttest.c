/* Host-side unit checks for the Wired wire format / PING-PONG logic. */
#include "wired.h"                 /* pulls netif.h + rogex.h (freestanding types) */
extern int printf(const char *, ...);

/* --- kernel shims wired.c references (no libc headers -> no size_t clash) - */
void *k_memcpy(void *d, const void *s, size_t n){ unsigned char *a=d; const unsigned char *b=s; while(n--) *a++=*b++; return d; }
void *k_memset(void *d, int v, size_t n){ unsigned char *a=d; while(n--) *a++=(unsigned char)v; return d; }
size_t k_strlen(const char *s){ size_t n=0; while(s[n]) n++; return n; }

/* --- mock NIC ------------------------------------------------------------ */
static uint8_t  g_mac[6];
static uint8_t  g_tx[1600]; static uint32_t g_txlen; static int g_txn;
static uint8_t  g_rx[1600]; static uint32_t g_rxlen; static int g_rxrdy;

const uint8_t *net_mac(void){ return g_mac; }
bool net_present(void){ return 1; }
bool net_send(const void *f, uint32_t len){ k_memcpy(g_tx,f,len); g_txlen=len; g_txn++; return 1; }
uint32_t net_poll(void *out, uint32_t max){ if(!g_rxrdy) return 0; g_rxrdy=0; uint32_t n=g_rxlen<max?g_rxlen:max; k_memcpy(out,g_rx,n); return n; }
/* IRQ-mode stubs: report polling so wired_service drains unconditionally. */
bool net_irq_active(void){ return 0; }
bool net_take_pending(void){ return 1; }
int  net_irq_line(void){ return -1; }
bool net_irq_enable(void){ return 0; }
/* Stubs so wired.c can link without the full IP stack (host unit test). */
void ipstack_service(void){}
void ip_set_l2_rx(void (*fn)(const uint8_t *, uint32_t)){ (void)fn; }

static void set_mac(uint8_t a,uint8_t b,uint8_t c,uint8_t d,uint8_t e,uint8_t f){ g_mac[0]=a;g_mac[1]=b;g_mac[2]=c;g_mac[3]=d;g_mac[4]=e;g_mac[5]=f; }
static void inject(const uint8_t *f, uint32_t n){ k_memcpy(g_rx,f,n); g_rxlen=n; g_rxrdy=1; }

/* wire format (must match wired.c) */
#define OFF_MAGIC 14
#define OFF_VER   18
#define OFF_TYPE  19
#define OFF_FROM  20
#define OFF_TARG  52
#define FLEN      84
#define WT_PING 1
#define WT_PONG 2
#define ET 0x88B5

static int fails;
static void ok(const char *name, int cond){ printf("  [%s] %s\n", cond?"ok":"FAIL", name); if(!cond) fails++; }

/* Compare a 32-byte NUL-padded alias field against a C string. */
static int alias_eq(const uint8_t *p, const char *s){
    int i=0;
    for(; i<32 && s[i]; i++) if ((char)p[i] != s[i]) return 0;
    for(; i<32; i++) if (p[i] != 0) return 0;
    return 1;
}

/* Build a frame the way a peer would, to feed our node. */
static void build_frame(uint8_t *f, const uint8_t *dst, const uint8_t *src,
                        uint8_t type, const char *from, const char *targ){
    for(int i=0;i<6;i++) f[i]=dst[i];
    for(int i=0;i<6;i++) f[6+i]=src[i];
    f[12]=(ET>>8)&0xff; f[13]=ET&0xff;
    f[OFF_MAGIC]='R';f[OFF_MAGIC+1]='G';f[OFF_MAGIC+2]='X';f[OFF_MAGIC+3]='W';
    f[OFF_VER]=1; f[OFF_TYPE]=type;
    int i=0; for(;i<32 && from[i];i++) f[OFF_FROM+i]=(uint8_t)from[i]; for(;i<32;i++) f[OFF_FROM+i]=0;
    i=0;     for(;i<32 && targ[i];i++) f[OFF_TARG+i]=(uint8_t)targ[i]; for(;i<32;i++) f[OFF_TARG+i]=0;
}

int main(void){
    uint8_t bcast[6]={0xff,0xff,0xff,0xff,0xff,0xff};
    uint8_t bob_mac[6]={0x52,0x54,0x00,0xbb,0xbb,0xbb};
    uint8_t alice_mac[6]={0x52,0x54,0x00,0xaa,0xaa,0xaa};

    printf("Rogex Wired host protocol test\n");

    /* ---- Test 1: a node named "alice" answers a PING addressed to it ---- */
    set_mac(0x52,0x54,0x00,0xaa,0xaa,0xaa);
    wired_init("alice");
    uint8_t ping[FLEN];
    build_frame(ping, bcast, bob_mac, WT_PING, "bob", "alice");
    inject(ping, FLEN);
    g_txn=0;
    wired_service();
    ok("PING for us -> exactly one reply sent", g_txn==1);
    ok("reply ethertype 0x88B5", g_tx[12]==0x88 && g_tx[13]==0xB5);
    ok("reply is a PONG", g_tx[OFF_TYPE]==WT_PONG);
    ok("reply from = alice", alias_eq(&g_tx[OFF_FROM], "alice"));
    ok("reply target echoes requester = bob", alias_eq(&g_tx[OFF_TARG], "bob"));
    { int u=1; for(int i=0;i<6;i++) if(g_tx[i]!=bob_mac[i]) u=0; ok("reply is UNICAST to bob's MAC", u); }
    { int s=1; for(int i=0;i<6;i++) if(g_tx[6+i]!=alice_mac[i]) s=0; ok("reply src = our MAC", s); }

    /* ---- Test 2: a PING NOT addressed to us is ignored ------------------ */
    build_frame(ping, bcast, bob_mac, WT_PING, "bob", "carol");
    inject(ping, FLEN); g_txn=0; wired_service();
    ok("PING for someone else -> no reply", g_txn==0);

    /* ---- Test 3: bob pings alice, then records alice's PONG ------------- */
    set_mac(0x52,0x54,0x00,0xbb,0xbb,0xbb);
    wired_init("bob");
    g_txn=0;
    wired_send_ping("alice");
    ok("send_ping emits one frame", g_txn==1);
    ok("ping is broadcast", (g_tx[0]&g_tx[1]&g_tx[2]&g_tx[3]&g_tx[4]&g_tx[5])==0xff);
    ok("ping type = PING", g_tx[OFF_TYPE]==WT_PING);
    ok("ping from = bob", alias_eq(&g_tx[OFF_FROM],"bob"));
    ok("ping target = alice", alias_eq(&g_tx[OFF_TARG],"alice"));
    uint8_t pong[FLEN];
    build_frame(pong, bob_mac, alice_mac, WT_PONG, "alice", "bob");
    inject(pong, FLEN);
    wired_service();
    ok("PONG recorded one peer", wired_reply_count()==1);
    ok("peer alias = alice", wired_reply_count()==1 && alias_eq((const uint8_t*)wired_reply_alias(0),"alice"));

    /* ---- Test 4: anonymity — the frame carries ONLY protocol + aliases -- */
    /* the fixed 84-byte layout has no room for host metadata; assert the
       only non-zero, non-alias payload bytes are the known constants. */
    build_frame(ping, bcast, bob_mac, WT_PING, "bob", "alice");
    set_mac(0x52,0x54,0x00,0xaa,0xaa,0xaa); wired_init("alice");
    inject(ping,FLEN); g_txn=0; wired_service();
    int clean = (g_txlen==FLEN)
        && g_tx[OFF_MAGIC]=='R' && g_tx[OFF_MAGIC+1]=='G'
        && g_tx[OFF_MAGIC+2]=='X' && g_tx[OFF_MAGIC+3]=='W'
        && g_tx[OFF_VER]==1;
    ok("frame is exactly 84 bytes, only header+aliases (no host metadata)", clean);

    printf(fails? "\nWIRED HOST TEST FAIL (%d)\n" : "\nWIRED HOST TEST PASS\n", fails);
    return fails ? 1 : 0;
}
