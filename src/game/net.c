/* Multiplayer: HMI NetNOW's IPX backend replaced by UDP (Windows sockets).
 *
 * The game talks to NetNOW through a table of 13 functions (804a88-804abc,
 * filled by 462e1d with the IPX backend at 46fxxx, or a NetBIOS one).  A
 * player is a 16-byte address (IPX: network 4 bytes, node 6 bytes, rest 0);
 * the node list (805224, 16 entries, count 805998, own index 805944, own
 * address 805204) is plain data and its functions (add, delete, find, sort,
 * count, own index, own address) stay the game's own.  The functions that
 * did IPX are native here:
 *   46f0c2 init(max nodes, socket)  0 ok, 2 no network, 3 socket in use
 *   46f2c5 uninit()
 *   46fbc7 post listens()           (here: take what has arrived)
 *   46f79b send(hdr, hsize, data, dsize, node index)
 *   46f9ca send to(hdr, hsize, data, dsize, address*)   (node FF..FF = all)
 *   46f614 get(hdr, hsize, data, dsize)        1 = a packet was there
 *   46f6df get(hdr, hsize, &data)              (data pointer into our buffer)
 * Both sends put a counter in the header's first dword; get takes the queued
 * packet with the lowest one first, as the IPX backend does.
 *
 * On the wire each UDP datagram is "ATN1", the sender's node (6 bytes), the
 * receiver's node (FF..FF = everyone), then header + data.  A receiver learns
 * the sender's UDP address from the datagram, so players behind routers (NAT)
 * reach each other once one side knows the other: packets for everyone go to
 * the LAN broadcast, the host address the player gave ([net] Connect) and
 * every player heard from.  [net] Port (17733) is the UDP port; the host of
 * an internet game forwards it in the router.
 * Test switches: AT_NETPORT=<port>, AT_NETCONNECT=<host[:port],...>, AT_NETLOG=1. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#define RET(n) (cpu.r[R_ESP] += 4 + (n))
#define ARG(i) RD32(cpu.r[R_ESP] + 4 + 4 * (i))

extern void settings_str(const char *sec, const char *key, const char *def, char *out, int n);
extern int settings_int(const char *sec, const char *key, int def, int lo, int hi);

#define OWN_ADDR   0x805204u
#define NODE_LIST  0x805224u
#define NODE_COUNT 0x805998u
#define SEQ        0x805954u
#define MAX_NODES  0x80594cu
#define RX_BUF     0x32200000u     /* guest buffer for get-by-pointer */
#define NET_PORT   17733

static SOCKET sock = INVALID_SOCKET;
static u8 own[6];
static int port, logon;

typedef struct { u8 node[6]; struct sockaddr_in a; } Peer;
static Peer peers[32];
static int npeers;
static struct sockaddr_in hosts[8];   /* [net] Connect */
static int nhosts;

typedef struct { u32 seq; int len; u8 d[600]; } Pkt;
static Pkt q[64];
static int nq;

static void nlog(const char *f, ...)
{
    if (!logon) return;
    va_list a;
    va_start(a, f);
    char t[300];
    vsnprintf(t, sizeof t, f, a);
    va_end(a);
    rt_log("net: %s\n", t);
}

static int parse_addr(const char *s, struct sockaddr_in *a)
{
    char h[256];
    int p = port;
    snprintf(h, sizeof h, "%s", s);
    char *c = strrchr(h, ':');
    if (c) { *c = 0; p = atoi(c + 1); }
    while (*h == ' ') memmove(h, h + 1, strlen(h));
    if (!*h) return 0;
    struct addrinfo hint = {0}, *r = NULL;
    hint.ai_family = AF_INET;
    hint.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(h, NULL, &hint, &r) || !r) return 0;
    *a = *(struct sockaddr_in *)r->ai_addr;
    a->sin_port = htons((u16)p);
    freeaddrinfo(r);
    return 1;
}

static void load_hosts(void)
{
    char s[512];
    const char *e = getenv(ENV("NETCONNECT"));
    if (e) snprintf(s, sizeof s, "%s", e);
    else settings_str("net", "Connect", "", s, sizeof s);
    nhosts = 0;
    for (char *t = strtok(s, ",; "); t && nhosts < 8; t = strtok(NULL, ",; "))
        if (parse_addr(t, &hosts[nhosts])) { nlog("host %s", t); nhosts++; }
}

static int same(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static void send_one(const u8 *pkt, int len, const struct sockaddr_in *a)
{
    sendto(sock, (const char *)pkt, len, 0, (const struct sockaddr *)a, sizeof *a);
}

static void enqueue(const u8 *d, int len)
{
    if (len <= 0 || len > (int)sizeof q[0].d) return;
    if (nq == 64) { nlog("queue full, packet dropped"); return; }
    q[nq].len = len;
    q[nq].seq = len >= 4 ? (u32)d[0] | (u32)d[1] << 8 | (u32)d[2] << 16 | (u32)d[3] << 24 : 0;
    memcpy(q[nq].d, d, len);
    nq++;
}

/* everything that has arrived goes to the queue */
static void poll_net(void)
{
    if (sock == INVALID_SOCKET) return;
    for (;;) {
        u8 b[700];
        struct sockaddr_in from;
        int fl = sizeof from;
        int n = recvfrom(sock, (char *)b, sizeof b, 0, (struct sockaddr *)&from, &fl);
        if (n == SOCKET_ERROR) { if (WSAGetLastError() == WSAECONNRESET) continue; break; }
        if (n < 16 || memcmp(b, "ATN1", 4)) continue;
        const u8 *src = b + 4, *dst = b + 10;
        if (!memcmp(src, own, 6)) continue;                       /* our own broadcast */
        int k;
        for (k = 0; k < npeers; k++) if (!memcmp(peers[k].node, src, 6)) break;
        if (k == npeers && npeers < 32) { memcpy(peers[npeers].node, src, 6); npeers++; nlog("new player %02x%02x%02x%02x%02x%02x at %s:%d", src[0], src[1], src[2], src[3], src[4], src[5], inet_ntoa(from.sin_addr), ntohs(from.sin_port)); }
        if (k < 32) peers[k].a = from;                            /* (moves with the NAT) */
        static const u8 all[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
        if (memcmp(dst, own, 6) && memcmp(dst, all, 6)) continue;
        { static int k; if (k < 400) { k++; nlog("recv %d from %02x%02x%02x%02x%02x%02x type %08x", n - 16, src[0], src[1], src[2], src[3], src[4], src[5], n >= 24 ? *(u32 *)(b + 20) : 0); } }
        enqueue(b + 16, n - 16);
    }
}

/* send header + data to node dst (6 bytes) */
static int net_send(u32 hdr, u32 hs, u32 data, u32 ds, const u8 *dst)
{
    if (sock == INVALID_SOCKET || hs + ds > 590) return 0;
    WR32(hdr, RD32(SEQ));                 /* the packet counter, as the IPX backend */
    WR32(SEQ, RD32(SEQ) + 1);
    u8 p[16 + 600];
    memcpy(p, "ATN1", 4);
    memcpy(p + 4, own, 6);
    memcpy(p + 10, dst, 6);
    memcpy(p + 16, MEM + hdr, hs);
    if (ds) memcpy(p + 16 + hs, MEM + data, ds);
    int len = 16 + (int)(hs + ds);
    {   /* (AT_NETLOG: the first packets of each kind) */
        static int k;
        if (k < 400) { k++; nlog("send %u+%u to %02x%02x%02x%02x%02x%02x type %08x", hs, ds, dst[0], dst[1], dst[2], dst[3], dst[4], dst[5], RD32(hdr + 4)); }
    }
    if (!memcmp(dst, own, 6)) { enqueue(p + 16, len - 16); return 1; }
    static const u8 all[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    int bc = !memcmp(dst, all, 6);
    if (!bc) for (int k = 0; k < npeers; k++)
        if (!memcmp(peers[k].node, dst, 6)) { send_one(p, len, &peers[k].a); return 1; }
    /* everyone, or a player not heard from yet: LAN, the host, all players */
    struct sockaddr_in b = {0};
    b.sin_family = AF_INET;
    b.sin_port = htons((u16)port);
    b.sin_addr.s_addr = INADDR_BROADCAST;
    send_one(p, len, &b);
    for (int i = 0; i < nhosts; i++) send_one(p, len, &hosts[i]);
    for (int k = 0; k < npeers; k++) {
        int dup = 0;
        for (int i = 0; i < nhosts; i++) dup |= same(&hosts[i], &peers[k].a);
        if (!dup) send_one(p, len, &peers[k].a);
    }
    if (bc) enqueue(p + 16, len - 16);    /* IPX hears its own broadcast too */
    return 1;
}

/* 46f0c2: init(max nodes, socket) */
void N_net_init(void)
{
    u32 maxn = ARG(0);
    logon = getenv(ENV("NETLOG")) != NULL;
    if (sock != INVALID_SOCKET) { closesocket(sock); sock = INVALID_SOCKET; }
    static int wsa;
    if (!wsa) { WSADATA w; if (WSAStartup(MAKEWORD(2, 2), &w)) { cpu.r[R_EAX] = 2; RET(0); return; } wsa = 1; }
    const char *e = getenv(ENV("NETPORT"));
    port = e ? atoi(e) : settings_int("net", "Port", NET_PORT, 1024, 65535);
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) { cpu.r[R_EAX] = 2; RET(0); return; }
    BOOL on = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof on);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((u16)port);
    a.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, (struct sockaddr *)&a, sizeof a)) {
        rt_log("net: UDP port %d is in use\n", port);
        closesocket(sock); sock = INVALID_SOCKET;
        cpu.r[R_EAX] = 3; RET(0); return;
    }
    u_long nb = 1;
    ioctlsocket(sock, FIONBIO, &nb);
    /* a node of our own: unique per running game (two on one PC too) */
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    u32 r1 = (u32)t.QuadPart ^ GetCurrentProcessId() * 2654435761u, r2 = (u32)(t.QuadPart >> 32) ^ (u32)port << 8 ^ GetTickCount();
    own[0] = 0x02; own[1] = (u8)r2; own[2] = (u8)(r1 >> 24); own[3] = (u8)(r1 >> 16); own[4] = (u8)(r1 >> 8); own[5] = (u8)r1;
    npeers = 0; nq = 0;
    load_hosts();
    /* as the IPX backend: no players, our address, ourselves in the list */
    WR32(NODE_COUNT, 0);
    for (int i = 0; i < 16; i++) WR8(OWN_ADDR + i, 0);
    WR32(OWN_ADDR, 0x01000000);           /* "network" 00 00 00 01 */
    for (int i = 0; i < 6; i++) WR8(OWN_ADDR + 4 + i, own[i]);
    memcpy(MEM + 0x805214, MEM + OWN_ADDR, 16);
    {   /* AddNode(805214) - the game's own routine (46f472) */
        extern void rt_call(u32 target, u32 at);
        u32 save[8];
        memcpy(save, cpu.r, sizeof save);
        cpu.r[R_ESP] -= 4; WR32(cpu.r[R_ESP], 0x805214);
        cpu.r[R_ESP] -= 4; WR32(cpu.r[R_ESP], 0x46f472);
        rt_call(0x46f472, 0x46f472);
        memcpy(cpu.r, save, sizeof save);
    }
    WR32(SEQ, 0);
    WR32(MAX_NODES, maxn);
    for (int i = 0; i < 6; i++) { WR8(0x805338 + i, 0xff); WR8(0x80533e + i, 0xff); }   /* the broadcast address */
    WR32(0x805334, 0);
    rt_log("net: UDP port %d, node %02x%02x%02x%02x%02x%02x, %d host address(es)\n", port, own[0], own[1], own[2], own[3], own[4], own[5], nhosts);
    cpu.r[R_EAX] = 0;
    RET(0);
}

/* 46f2c5: uninit */
void N_net_uninit(void)
{
    if (sock != INVALID_SOCKET) { closesocket(sock); sock = INVALID_SOCKET; }
    nq = 0; npeers = 0;
    nlog("closed");
    cpu.r[R_EAX] = 1;
    RET(0);
}

/* 46fbc7: post a listen - IPX: 1 = one more receive buffer posted, 0 = all
   are listening (callers loop until 0).  Here: take in what has arrived. */
void N_net_listen(void)
{
    poll_net();
    cpu.r[R_EAX] = 0;
    RET(0);
}

/* 46f79b: send(hdr, hsize, data, dsize, node) */
void N_net_send(void)
{
    u32 node = ARG(4);   /* (0x11: the broadcast address kept after the list, 805334) */
    { static int k; if (k < 50) { k++; nlog("send(node %u) count %u sock %d", node, RD32(NODE_COUNT), sock != INVALID_SOCKET); } }
    if ((node >= RD32(NODE_COUNT) || node > 16) && node != 0x11) { cpu.r[R_EAX] = 0; RET(0); return; }
    cpu.r[R_EAX] = (u32)net_send(ARG(0), ARG(1), ARG(2), ARG(3), MEM + NODE_LIST + node * 16 + 4);
    RET(0);
}

/* 46f9ca: send to(hdr, hsize, data, dsize, address) */
void N_net_sendto(void)
{
    cpu.r[R_EAX] = (u32)net_send(ARG(0), ARG(1), ARG(2), ARG(3), MEM + ARG(4) + 4);
    RET(0);
}

/* the queued packet with the lowest counter, removed from the queue */
static int take(Pkt *out)
{
    poll_net();
    if (!nq) return 0;
    int b = 0;
    for (int i = 1; i < nq; i++) if (q[i].seq < q[b].seq) b = i;
    *out = q[b];
    memmove(&q[b], &q[b + 1], (size_t)(nq - b - 1) * sizeof q[0]);
    nq--;
    return 1;
}

/* 46f614: get(hdr, hsize, data, dsize) */
void N_net_get(void)
{
    u32 hdr = ARG(0), hs = ARG(1), data = ARG(2), ds = ARG(3);
    Pkt p;
    if (!take(&p)) { cpu.r[R_EAX] = 0; RET(0); return; }
    memset(MEM + hdr, 0, hs);
    memcpy(MEM + hdr, p.d, (u32)p.len < hs ? (u32)p.len : hs);
    if (ds) {
        memset(MEM + data, 0, ds);
        if ((u32)p.len > hs) memcpy(MEM + data, p.d + hs, (u32)p.len - hs < ds ? (u32)p.len - hs : ds);
    }
    cpu.r[R_EAX] = 1;
    RET(0);
}

/* 46f6df: get(hdr, hsize, &data) - data stays in our buffer until the next get */
void N_net_getp(void)
{
    u32 hdr = ARG(0), hs = ARG(1), pdata = ARG(2);
    Pkt p;
    if (!take(&p)) { cpu.r[R_EAX] = 0; RET(0); return; }
    memset(MEM + RX_BUF, 0, 0x300);
    memcpy(MEM + RX_BUF, p.d, p.len);
    memcpy(MEM + hdr, MEM + RX_BUF, hs);
    WR32(pdata, RX_BUF + hs);
    cpu.r[R_EAX] = 1;
    RET(0);
}

/* ---- the join address (Input menu) ---- */
static wchar_t prompt_buf[256];
static int prompt_done;
static HWND prompt_edit;

static LRESULT CALLBACK prompt_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_COMMAND && (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL)) {
        if (LOWORD(w) == IDOK) GetWindowTextW(prompt_edit, prompt_buf, 256);
        prompt_done = LOWORD(w) == IDOK ? 1 : 2;
        return 0;
    }
    if (m == WM_CLOSE) { prompt_done = 2; return 0; }
    return DefWindowProcW(h, m, w, l);
}

/* ask for the host's address; 1 = given (saved in [net] Connect) */
int net_ask_address(void *parent)
{
    static int reg;
    HINSTANCE hi = GetModuleHandleW(NULL);
    if (!reg) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = prompt_proc;
        wc.hInstance = hi;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = L"ATNetPrompt";
        RegisterClassW(&wc);
        reg = 1;
    }
    char cur[256];
    settings_str("net", "Connect", "", cur, sizeof cur);
    RECT pr = {0, 0, 800, 600};
    if (parent) GetWindowRect((HWND)parent, &pr);
    int W = 470, H = 254;
    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"ATNetPrompt", L"Join a multiplayer game",
                             WS_POPUP | WS_CAPTION | WS_SYSMENU, (pr.left + pr.right - W) / 2, (pr.top + pr.bottom - H) / 2, W, H,
                             (HWND)parent, NULL, hi, NULL);
    HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HWND t = CreateWindowExW(0, L"STATIC",
                             L"The host's address (IP or name, :port if not 17733).\n"
                             L"On the same network (LAN) leave it empty - games are found by themselves.\n"
                             L"Over the internet the host forwards UDP port 17733 in the router.\n\n"
                             L"Tip - easiest over the internet: everyone installs Radmin VPN (free),\n"
                             L"joins the same Radmin network, and types the host's Radmin IP\n"
                             L"(26.x.x.x) here. No router setup needed.",
                             WS_CHILD | WS_VISIBLE, 12, 10, W - 30, 124, h, NULL, hi, NULL);
    prompt_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                  12, 140, W - 30, 24, h, NULL, hi, NULL);
    HWND ok = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, W - 200, 176, 80, 28, h, (HMENU)IDOK, hi, NULL);
    HWND ca = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, W - 110, 176, 80, 28, h, (HMENU)IDCANCEL, hi, NULL);
    SendMessageW(t, WM_SETFONT, (WPARAM)f, 0);
    SendMessageW(prompt_edit, WM_SETFONT, (WPARAM)f, 0);
    SendMessageW(ok, WM_SETFONT, (WPARAM)f, 0);
    SendMessageW(ca, WM_SETFONT, (WPARAM)f, 0);
    {
        wchar_t wc[256];
        MultiByteToWideChar(CP_UTF8, 0, cur, -1, wc, 256);
        SetWindowTextW(prompt_edit, wc);
        SendMessageW(prompt_edit, EM_SETSEL, 0, -1);
    }
    if (parent) EnableWindow((HWND)parent, FALSE);
    ShowWindow(h, SW_SHOW);
    SetFocus(prompt_edit);
    prompt_done = 0;
    MSG msg;
    while (!prompt_done && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) { prompt_done = 0; SendMessageW(h, WM_COMMAND, IDOK, 0); continue; }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) { prompt_done = 2; continue; }
        if (!IsDialogMessageW(h, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    if (parent) EnableWindow((HWND)parent, TRUE);
    DestroyWindow(h);
    if (parent) SetForegroundWindow((HWND)parent);
    if (prompt_done != 1) return 0;
    char s[256];
    WideCharToMultiByte(CP_UTF8, 0, prompt_buf, -1, s, sizeof s, NULL, NULL);
    char *p = s;
    while (*p == ' ') p++;
    for (char *e = p + strlen(p); e > p && e[-1] == ' '; ) *--e = 0;
    extern void settings_set_str(const char *sec, const char *key, const char *v);
    settings_set_str("net", "Connect", p);
    if (!port) port = settings_int("net", "Port", NET_PORT, 1024, 65535);
    load_hosts();
    rt_log("net: join address now '%s'\n", p);
    return 1;
}

/* the address for the menu text ("" = LAN only) */
void net_address(char *out, int n) { settings_str("net", "Connect", "", out, n); }

/* the join address changed in the menu: use it now */
void net_reload(void)
{
    if (!port) port = settings_int("net", "Port", NET_PORT, 1024, 65535);
    load_hosts();
}
