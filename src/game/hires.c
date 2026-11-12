/* Internal Resolution for Alien Trilogy (Video > Internal Resolution, ini
 * RenderScale).
 *
 * The PC version draws through a software copy of the PlayStation GPU: the
 * game builds packets (POLY_FT4, POLY_F3/G3, TILE, ...) into an ordering
 * table, 4493cc walks it once per frame and calls each packet's handler
 * ([packet+4], eax = packet), and the handlers rasterise into a 320x240
 * 15-bit buffer (64c288 / 671a88, double-buffered by 4ad574) through
 * self-modifying span drawers.  459010 then copies that buffer to the VESA
 * screen (640x480, every pixel doubled).
 *
 * At N x (like a PlayStation emulator's internal resolution) the whole frame
 * is rasterised into an N x buffer instead:
 *   - the viewport block (base 4ad58c, pitch 800198, size 800294 x 80029c,
 *     clip 4ad548 / 4ad54c) describes the N x buffer: 447324 (full view) and
 *     447044 (view shrunk with keypad -/+) are native here, the per-frame
 *     copy of the full view inside 4493cc and the drawers' hard-coded 640-byte
 *     row steps and 320/240 clip constants go through tools/subst_trilogy.txt
 *     (rt_hr_n = N during the game, 1 = original);
 *   - packet coordinates are 320x240 values: 4473d0 (the game's own "scale
 *     to the shrunk view" step, called by the quad handler 44a444) always
 *     scales by the view size, and the handlers that take raw coordinates
 *     (F3 4497dc, flat/gouraud 4498dc/449dd0 in their raw mode, TILE 44c904)
 *     get wrappers that scale the packet while they run;
 *   - 459010 (frame copy) first reduces the N x frame into the game's 1x
 *     buffer (so everything that reads it, and the VESA copy, still work) and
 *     hands the N x frame to the presenter (hires_frame16).
 * Texture coordinates are interpolated over the spans, so textures come out
 * at N x detail by themselves. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <SDL2/SDL.h>
#include "rt.h"

#define RET(n) (cpu.r[R_ESP] += 4 + (n))

u32 rt_hr_n = 1, rt_hr_k, rt_hr_rowtab;   /* (cpu.h) the frame's scale, 1 = original */
u32 rt_hr_w = 320, rt_hr_on;              /* (cpu.h) the frame's width at 1x (widescreen: wider), N x / wide pass on */
static u32 hr_off;                        /* widescreen: (width - 320) / 2, where the 320-wide picture (HUD, menus) sits */

#define HR_BUF0 0x30000000u                /* N x frames (15-bit), up to 6 x */
#define HR_BUF1 0x31000000u                /* (up to 576 x 240 at 6 x) */
#define BUF0    0x64c288u
#define BUF1    0x671a88u

extern int opt_scale;
extern void (*rt_wrap_orig(const char *native, u32 *addr))(void);

static int hr_ready;              /* a whole N x frame was drawn since the last mode change */
static int hr_drawn;              /* the frame being copied was drawn by the N x frame pass */
static u32 hr_shown_buf;          /* the N x buffer of the frame being presented */
static u32 hr_shown_n = 1, hr_shown_w = 320;

static u32 hr_frame_n = 1;        /* the scale of the last frame pass */
static void hr_leave(void);

static u32 cur_buf(void) { return RD8(0x4ad574) ? (rt_hr_on ? HR_BUF1 : BUF1) : (rt_hr_on ? HR_BUF0 : BUF0); }

static void probed(void (*f)(void), const char *what);
static int cmp_log;            /* AT_HRCMP pass: log every primitive (hrcmp.txt) */
static const char *tri_path;
static FILE *cmp_f;

/* ---- widescreen: a wider view, not a stretched one ----
   The game projects a vertex to x = X*H/Z + OFX (448490: OFX = word 4ad614,
   set by 44e4d4).  With [game] WideView=1 the frame is W x 240 (W from the
   window's shape, 428 at 16:9) and OFX moves right by (W - 320) / 2, so the
   same 3D view gets wider on both sides.  The 2D picture (HUD, menus, text:
   the packets in raw 320x240 coordinates) stays 320 wide, in the middle. */
extern int settings_int(const char *sec, const char *key, int def, int lo, int hi);
static u32 wide_width(void)
{
    const char *e = getenv(ENV("WIDE"));       /* AT_WIDE=<width>: tests (no window) */
    if (e) { u32 w = (u32)atoi(e) & ~1u; return w < 320 ? 320 : w > 576 ? 576 : w; }
    if (!settings_int("game", "WideView", 1, 0, 1)) return 320;
    extern double video_client_aspect(void);
    double a = video_client_aspect();
    if (a < 4.0 / 3.0 + 0.01) return 320;
    u32 w = (u32)(240.0 * a + 0.5) & ~1u;
    return w < 320 ? 320 : w > 576 ? 576 : w;
}

static s32 game_ofx = -1;                  /* the centre the game asked for */
static u32 geom_off;                       /* what is added to it now */
static void geom_apply(u32 off)
{
    if (game_ofx < 0) game_ofx = (s16)RD16(0x4ad614) - (s32)geom_off;
    geom_off = off;
    WR16(0x4ad614, (u16)(game_ofx + (s32)off));
    rt_game_var[2] = off ? 320 + 2 * off : 0;   /* the builders' right-edge culls (tools/subst_trilogy.txt) */
    {   /* the map cells' angle windows (427440) grow by the extra half-angle of the view */
        double h = (s32)RD32(0x800168) > 0 ? (double)(s32)RD32(0x800168) : 256.0;
        double d = off ? (atan2(160.0 + off, h) - atan2(160.0, h)) * 4096.0 / (2 * 3.14159265358979) + 8 : 0;
        rt_game_var[3] = (u32)d;
    }
}

/* 44e4d4 (wrap): set the projection centre (ax = x, dx = y) */
void N_geom_offset(void)
{
    s32 x = (s16)cpu.r[R_EAX];
    rt_wrap_orig("N_geom_offset", NULL)();
    game_ofx = x;
    u32 keep = geom_off;
    geom_off = 0;
    geom_apply(keep);                     /* (the frame pass picks the width) */
}

static u32 hr_frame_w = 320;

/* The projectors (448128, 448490, 448924 ...) divide by each corner's depth;
   a polygon with a corner behind the camera (but not all of them) gets that
   corner thrown across the screen and the triangle rejects drop it.  At 4:3
   such walls are off screen; the wide view sees them at its sides (holes at
   the edges).  With widescreen on, a corner nearer than the game's near
   distance (20h) is projected as if on it (tools/subst_trilogy.txt). */
#define NEARZ 0x20
#define NEARZ_SLOT 0x32300000u
u32 rt_nearz(u32 a)
{
    s32 z = (s32)RD32(a);
    if (!rt_game_var[2] || z >= NEARZ) return a;
    WR32(NEARZ_SLOT, NEARZ);
    return NEARZ_SLOT;
}

/* ---- widescreen HUD: panels to the screen edges ----
   In play (the gun was drawn since the last frame and the Options panel is
   closed) each 2D element moves by its own place in the 320-wide picture:
   left-side ones stay at the left edge, right-side ones go to the right edge,
   the rest stay in the middle.  Small glyphs (text, panel digits) are joined
   into rows first (a scan of the ordering table), so a line moves as one.
   Menus and other screens stay in the middle as a whole. */
static int hud_on, gun_drawn;
typedef struct { s32 y, x0, x1; } Row;
static Row rows[96];
static int nrows;

/* the element's box in 320x240 units (0 = not a 2D packet) */
static int pk_box(u32 pk, s32 *x0, s32 *y0, s32 *x1, s32 *y1)
{
    static const u8 quad[] = {0x0c, 0x14, 0x1c, 0x24}, g3[] = {0x0c, 0x10, 0x14, 0x18};
    const u8 *v = NULL;
    int nv = 0;
    switch (RD32(pk + 4)) {
    case 0x44a444: case 0x44b234: case 0x44b400:   /* textured quads: 2D when +22 is set */
        if (!RD16(pk + 0x22)) return 0; v = quad; nv = 4; break;
    case 0x44b5cc: case 0x44b710: case 0x44b818: v = quad; nv = 3; break;
    case 0x4498dc: case 0x449dd0: if (!RD8(pk + 0x0b)) return 0; v = g3; nv = 4; break;
    case 0x4497dc: v = g3; nv = 3; break;
    case 0x44960c: *x0 = (s16)RD16(pk + 0x0c); *y0 = (s16)RD16(pk + 0x0e); *x1 = (s16)RD16(pk + 0x14); *y1 = (s16)RD16(pk + 0x16); break;
    case 0x44cc90: *x0 = (s16)RD16(pk + 0x0c); *y0 = (s16)RD16(pk + 0x0e); *x1 = *x0 + (s16)RD16(pk + 0x14); *y1 = *y0 + (s16)RD16(pk + 0x16); break;
    case 0x44e53c: *x0 = (s16)RD16(pk + 0x0c); *y0 = (s16)RD16(pk + 0x0e); *x1 = (s16)RD16(pk + 0x14); *y1 = (s16)RD16(pk + 0x1e); break;
    case 0x44c904: *x0 = (s16)RD16(pk + 0x0c); *y0 = (s16)RD16(pk + 0x0e); *x1 = *x0 + (s16)RD16(pk + 0x10); *y1 = *y0 + (s16)RD16(pk + 0x12); break;
    default: return 0;
    }
    if (v) {
        *x0 = *y0 = 1 << 30; *x1 = *y1 = -(1 << 30);
        for (int i = 0; i < nv; i++) {
            s32 x = (s16)RD16(pk + v[i]), y = (s16)RD16(pk + v[i] + 2);
            if (x < *x0) *x0 = x; if (x > *x1) *x1 = x;
            if (y < *y0) *y0 = y; if (y > *y1) *y1 = y;
        }
    }
    if (*x1 < *x0) { s32 t = *x0; *x0 = *x1; *x1 = t; }
    if (*y1 < *y0) { s32 t = *y0; *y0 = *y1; *y1 = t; }
    return 1;
}

static int is_glyph(s32 x0, s32 y0, s32 x1, s32 y1) { return x1 - x0 < 16 && y1 - y0 < 16; }

static void hud_scan(u32 ot)
{
    nrows = 0;
    int k = 0;
    for (u32 pk = ot; pk && k < 6000; pk = RD32(pk), k++) {
        s32 x0, y0, x1, y1;
        if (!pk_box(pk, &x0, &y0, &x1, &y1) || !is_glyph(x0, y0, x1, y1)) continue;
        int r;
        for (r = 0; r < nrows; r++)
            if (abs(rows[r].y - y0) <= 2 && x0 <= rows[r].x1 + 16 && x1 >= rows[r].x0 - 16) break;
        if (r == nrows) { if (nrows == 96) continue; rows[nrows].y = y0; rows[nrows].x0 = x0; rows[nrows].x1 = x1; nrows++; }
        else { if (x0 < rows[r].x0) rows[r].x0 = x0; if (x1 > rows[r].x1) rows[r].x1 = x1; }
    }
    for (int again = 1; again; ) {   /* rows that grew into each other */
        again = 0;
        for (int a = 0; a < nrows; a++)
            for (int b = a + 1; b < nrows; b++)
                if (abs(rows[a].y - rows[b].y) <= 2 && rows[b].x0 <= rows[a].x1 + 16 && rows[b].x1 >= rows[a].x0 - 16) {
                    if (rows[b].x0 < rows[a].x0) rows[a].x0 = rows[b].x0;
                    if (rows[b].x1 > rows[a].x1) rows[a].x1 = rows[b].x1;
                    rows[b] = rows[--nrows];
                    again = 1;
                }
    }
}

/* how far right an element with this box goes in the wide frame (1x pixels) */
static s32 hud_shift(s32 x0, s32 y0, s32 x1, s32 y1)
{
    s32 off = (s32)hr_off;
    if (!off || !hud_on) return off;
    if (is_glyph(x0, y0, x1, y1))
        for (int r = 0; r < nrows; r++)
            if (abs(rows[r].y - y0) <= 2 && x0 <= rows[r].x1 + 16 && x1 >= rows[r].x0 - 16) { x0 = rows[r].x0; x1 = rows[r].x1; break; }
    if (x0 < 40) return 0;
    if (x1 > 280) return 2 * off;
    if (x1 <= 168) return 0;
    if (x0 >= 152) return 2 * off;
    return off;
}
static s32 hud_shift_pk(u32 pk)
{
    s32 x0, y0, x1, y1;
    int ok = pk_box(pk, &x0, &y0, &x1, &y1);
    s32 s = ok ? hud_shift(x0, y0, x1, y1) : (s32)hr_off;
    {   /* AT_HUDLOG=ms: the decisions of one frame */
        static int at = -1;
        static u64 until;
        if (at < 0) { const char *e = getenv(ENV("HUDLOG")); at = e ? atoi(e) : 0; }
        u64 now = SDL_GetTicks64();
        if (at > 0 && now >= (u64)at) { until = now + 25; at = 0; }
        if (until && now < until)
            rt_log("hud %08x h %08x box %d,%d-%d,%d shift %d on %d\n", pk, RD32(pk + 4), ok ? x0 : -1, ok ? y0 : -1, ok ? x1 : -1, ok ? y1 : -1, s, hud_on);
    }
    return s;
}

/* 428c54 (wrap): the gun (the only user: 43e93c) - a quad in view space at
   fixed 320x240 places: to the middle of the wide view */
void N_gun_sprite(void)
{
    u32 pk = RD32(0x63e2e0);
    rt_wrap_orig("N_gun_sprite", NULL)();
    gun_drawn = 1;
    s32 off = rt_game_var[2] ? ((s32)rt_game_var[2] - 320) / 2 : 0;
    if (off) for (int i = 0; i < 4; i++) WR16(pk + 0x0c + 8 * i, (u16)((s16)RD16(pk + 0x0c + 8 * i) + off));
}

/* 4493cc: draw the ordering table (the whole frame) - the scale is chosen here */
void N_hr_frame(void)
{
    u32 n = (u32)opt_scale;
    { const char *e = getenv(ENV("HRSCALE")); if (e) n = (u32)atoi(e); }
    if (n < 1) n = 1;
    if (n > 6) n = 6;
    /* wide only in play (the gun drawn lately, the Options panel closed): menus,
       the panel and the briefings keep their 4:3 picture.  This frame's
       vertices were made for the current shift (geom_off): it is drawn at that
       width, the next one gets the wanted width. */
    static int since_gun = 1000;
    since_gun = gun_drawn ? 0 : since_gun + 1;
    int play = since_gun < 3;   /* (no gun behind the Options panel, menus, briefings) */
    u32 want = play ? wide_width() : 320;
    u32 w = 320 + 2 * geom_off;
    if (n != hr_frame_n || w != hr_frame_w) { hr_frame_n = n; hr_frame_w = w; hr_ready = 0; }
    rt_hr_n = n;                             /* N x / wide for this frame pass only (hr_leave) */
    rt_hr_w = w;
    rt_hr_on = n > 1 || w != 320;
    hr_off = (w - 320) / 2;
    hud_on = hr_off && play;              /* in play: HUD to the edges */
    gun_drawn = 0;
    if (hud_on) hud_scan(cpu.r[R_EAX]);
    if (geom_off != (want - 320) / 2) geom_apply((want - 320) / 2);   /* the next frame's vertices */
    WR32(0x800198, 2 * rt_hr_w * rt_hr_n);  /* row pitch (set once by the game, at 1x) */
    /* the whole N x frame starts black, as the game's own clear (45e769) does at 1x */
    if (rt_hr_on) memset(MEM + (RD8(0x4ad574) ? HR_BUF1 : HR_BUF0), 0, 2 * rt_hr_w * rt_hr_n * 240 * rt_hr_n);
    {   /* AT_OTLOG=ms: log the ordering table of one frame (handler + packet words) */
        static int at = -1;
        if (at < 0) { const char *e = getenv(ENV("OTLOG")); at = e ? atoi(e) : 0; }
        if (at > 0 && SDL_GetTicks64() >= (u64)at) {
            at = 0;
            int k = 0;
            for (u32 pk = cpu.r[R_EAX]; pk && k < 3000; pk = RD32(pk), k++)
                rt_log("ot %08x h %08x: %08x %08x %08x %08x %08x %08x %08x\n", pk, RD32(pk + 4), RD32(pk + 8), RD32(pk + 12),
                       RD32(pk + 16), RD32(pk + 20), RD32(pk + 24), RD32(pk + 28), RD32(pk + 32));
        }
    }
    {   /* AT_HRCMP=ms: draw that frame's ordering table at N x (hr.raw) and
           again at 1x (lo1.raw) - the same packets, for comparing */
        static int at = -1;
        if (at < 0) { const char *e = getenv(ENV("HRCMP")); at = e ? atoi(e) : 0; }
        if (at > 0 && rt_hr_n > 1 && rt_hr_w == 320 && SDL_GetTicks64() >= (u64)at) {
            at = 0;
            u32 regs[8], nn = rt_hr_n, a574 = RD8(0x4ad574), ra;
            memcpy(regs, cpu.r, sizeof regs);
            void (*f)(void) = rt_wrap_orig("N_hr_frame", &ra);
            cmp_f = fopen("hrcmp_hr.txt", "w"); cmp_log = 1;
            cpu.r[R_ESP] -= 4; WR32(cpu.r[R_ESP], ra);
            f();                                            /* N x pass (copies, toggles 4ad574) */
            fclose(cmp_f); cmp_f = fopen("hrcmp_lo.txt", "w");
            FILE *fo = fopen("hr.raw", "wb");
            if (fo) { fwrite(MEM + (a574 ? HR_BUF1 : HR_BUF0), 2, 320 * nn * 240 * nn, fo); fclose(fo); }
            memcpy(cpu.r, regs, sizeof regs);
            WR8(0x4ad574, a574);
            rt_hr_n = 1;
            rt_hr_on = 0;
            WR32(0x800198, 0x280);
            cpu.r[R_ESP] -= 4; WR32(cpu.r[R_ESP], ra);
            f();                                            /* 1x pass of the same table */
            fclose(cmp_f); cmp_f = NULL; cmp_log = 0;
            fo = fopen("lo1.raw", "wb");
            if (fo) { fwrite(MEM + (a574 ? BUF1 : BUF0), 2, 320 * 240, fo); fclose(fo); }
            rt_hr_n = nn;
            rt_hr_on = 1;
            WR32(0x800198, 0x280u * nn);
            hr_ready = 0;
            rt_log("hrcmp: %ux frame written\n", nn);
            RET(0);
            return;
        }
    }
    hr_drawn = rt_hr_on;
    rt_wrap_orig("N_hr_frame", NULL)();
    hr_drawn = 0;
    hr_leave();
}

/* N x only inside the frame pass: everything the game draws elsewhere (fades,
   screens around the movies and level loads) goes to its 1x buffers with the
   1x pitch and view - with the N x pitch left in place such a draw ran past
   the 1x buffer into the shade table behind it (6972c8: blue dark colours
   for the rest of the session) */
static void hr_leave(void)
{
    if (!rt_hr_on) return;
    u32 n = rt_hr_n, k = RD8(0x4ad550);
    rt_hr_n = 1;
    rt_hr_w = 320;
    rt_hr_on = 0;
    WR32(0x800198, 0x280);
    if (k > 4) k = 4;
    u32 shrunk = RD8(0x8002d3) != 0, w = shrunk ? 320 - 40 * k : 320, h = shrunk ? 240 - 30 * k : 240;
    WR32(0x4ad54c, h - 1);
    WR32(0x4ad548, w - 1);
    WR32(0x800294, w);
    WR32(0x80029c, h);
    WR32(0x4ad58c, cur_buf() + (shrunk ? 15 * k * 640 + 20 * k * 2 : 0));
    hr_frame_n = n;
}

/* 447324: full view */
void N_vp_full(void)
{
    u32 n = rt_hr_n;
    WR8(0x8002d3, 0);
    WR32(0x4ad54c, 240 * n - 1);
    WR32(0x4ad548, rt_hr_w * n - 1);
    WR32(0x800294, rt_hr_w * n);
    WR32(0x80029c, 240 * n);
    WR32(0x4ad58c, cur_buf());
    RET(0);
}

/* 447044: the view shrunk with keypad -/+: level k (4ad550, 0-4) leaves a
   border of 20k x 15k pixels (320x240 space) */
void N_vp_shrink(void)
{
    u32 n = rt_hr_n, k = RD8(0x4ad550);
    if (k > 4) k = 4;
    u32 w = (rt_hr_w - 40 * k) * n, h = (240 - 30 * k) * n;
    WR8(0x8002d3, 1);
    WR32(0x4ad54c, h - 1);
    WR32(0x4ad548, w - 1);
    WR32(0x800294, w);
    WR32(0x80029c, h);
    WR32(0x4ad58c, cur_buf() + 15 * k * n * RD32(0x800198) + 20 * k * n * 2);
    RET(0);
}

/* 4473d0: packet coordinates (320x240 space, in 800210-21c / 800220-22c) to
   the view: the game does it only for a shrunk view; at N x always */
/* x coordinates beyond +-32767 N x pixels overflow when the quad routine
   turns them into 16.16 (vertices far off screen, near the camera): the
   exact values are kept here by their overflowed 16.16 form, and the
   triangle wrapper takes them back and clips the triangle itself */
static struct { u32 key; s32 x; } bigx[16];
static int nbigx;
static int big_x(u32 fx, double *x)
{
    for (int i = 0; i < nbigx; i++) if (bigx[i].key == fx) { *x = bigx[i].x; return 1; }
    return 0;
}

void N_vp_scale(void)
{
    if (RD16(0x4ad550) != 0 || rt_hr_on) {
        s32 w = (s32)RD32(0x800294), h = (s32)RD32(0x80029c);
        nbigx = 0;
        for (int i = 0; i < 4; i++) {
            s32 x = (s32)RD32(0x800210 + i * 4) * w / (s32)rt_hr_w;   /* (wide: the game's x is in W x 240 space) */
            WR32(0x800210 + i * 4, (u32)x);
            WR32(0x800220 + i * 4, (u32)((s32)RD32(0x800220 + i * 4) * h / 0xf0));
            if ((x > 32767 || x < -32768) && nbigx < 16) { bigx[nbigx].key = (u32)x << 16; bigx[nbigx].x = x; nbigx++; }
        }
    }
    RET(0);
}

/* scale n packet shorts at the given offsets (x, y pairs) while the handler
   runs; widescreen: the x of these 320x240 packets moves to the middle */
static void scaled_call(const char *name, const u8 *offs, int cnt)
{
    u32 pk = cpu.r[R_EAX];
    u16 save[8];
    int on = rt_hr_on;
    s32 sh = on ? hud_shift_pk(pk) : 0;
    if (on) for (int i = 0; i < cnt; i++) {
        save[i] = (u16)RD16(pk + offs[i]);
        WR16(pk + offs[i], (u16)(((s16)save[i] + (i & 1 ? 0 : sh)) * (s32)rt_hr_n));
    }
    rt_wrap_orig(name, NULL)();
    if (on) for (int i = 0; i < cnt; i++) WR16(pk + offs[i], save[i]);
}

/* 4497dc: POLY_F3, raw coordinates x/y at +0c..+16 */
static void N_pk_f3_impl(void)
{
    static const u8 o[] = {0x0c, 0x0e, 0x10, 0x12, 0x14, 0x16};
    scaled_call("N_pk_f3", o, 6);
}

/* 4498dc / 449dd0: three-point packets; byte +0b = 0: coordinates in 320x240
   space scaled to the view by the handler itself, else raw screen pixels */
static const u8 o_g3[] = {0x0c, 0x0e, 0x10, 0x12, 0x14, 0x16, 0x18, 0x1a};
/* the raw-pixel ones are HUD bars (gradient quads): at N x the rasterizer ran
   their flat bottom row across the whole screen (the green line under the
   HUD) - drawn at 1x, blocks into the N x frame, as the other bars */
static void lowres_call(const char *name, s32 x, s32 y, s32 w, s32 h);
static void raw_g3(const char *name)
{
    s32 x0, y0, x1, y1;
    if (!rt_hr_on || !pk_box(cpu.r[R_EAX], &x0, &y0, &x1, &y1)) { scaled_call(name, o_g3, 8); return; }
    lowres_call(name, x0, y0, x1 - x0 + 1, y1 - y0 + 1);
}
static void N_pk_4498dc_impl(void)
{
    if (getenv(ENV("PKLOG")) && cpu.r[R_EAX] == 0x4b2668) {
        static int k;
        u32 pk = cpu.r[R_EAX];
        if (k++ < 3) rt_log("pk %08x: %08x %08x %08x %08x %08x %08x 294 %u 29c %u 548 %u 54c %u 58c %08x\n", pk, RD32(pk + 8), RD32(pk + 12), RD32(pk + 16),
                            RD32(pk + 20), RD32(pk + 24), RD32(pk + 28), RD32(0x800294), RD32(0x80029c), RD32(0x4ad548), RD32(0x4ad54c), RD32(0x4ad58c));
    }
    if (RD8(cpu.r[R_EAX] + 0x0b)) raw_g3("N_pk_4498dc");
    else rt_wrap_orig("N_pk_4498dc", NULL)();
}
static void N_pk_449dd0_impl(void)
{
    if (RD8(cpu.r[R_EAX] + 0x0b)) raw_g3("N_pk_449dd0");
    else rt_wrap_orig("N_pk_449dd0", NULL)();
}

/* 44c904: TILE (solid rectangle x,y,w,h at +0c, colour word at +08) on the
   whole screen */
static void N_pk_tile_impl(void)
{
    if (!rt_hr_on) { rt_wrap_orig("N_pk_tile", NULL)(); return; }
    u32 pk = cpu.r[R_EAX], n = rt_hr_n, W = rt_hr_w;
    s32 x = (s16)RD16(pk + 0x0c), y = (s16)RD16(pk + 0x0e), w = (s16)RD16(pk + 0x10), h = (s16)RD16(pk + 0x12);
    u16 c = (u16)RD16(pk + 8);
    if (x + w <= 0 || x >= 320 || y + h <= 0 || y >= 240) { RET(0); return; }
    if (x <= 0 && x + w >= 320) { x = 0; w = (s32)W; }   /* across the whole screen: the whole wide frame */
    else x += hud_shift_pk(pk);
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (s32)W) w = (s32)W - x;
    if (y + h > 240) h = 240 - y;
    u32 base = RD8(0x4ad574) ? HR_BUF1 : HR_BUF0, pitch = 2 * W * n;
    for (u32 r = (u32)y * n; r < (u32)(y + h) * n; r++) {
        u16 *p = (u16 *)(MEM + base + r * pitch) + x * n;
        for (u32 i = 0; i < (u32)w * n; i++) p[i] = c;
    }
    RET(0);
}

/* 459010: copy the frame to the screen - first reduce the N x frame into the
   game's own 1x buffer (centre sample of each N x N block) */
void N_hr_copy(void)
{
    /* a copy outside the frame pass (fades, screens the game builds itself at
       1x): show the game's own picture */
    if (rt_hr_on && hr_drawn) {
        u32 n = rt_hr_n, src = RD32(0x4ad590) == BUF1 ? HR_BUF1 : HR_BUF0, dst = RD32(0x4ad590);
        u32 pitch = 2 * rt_hr_w * n, c = n / 2;
        for (u32 y = 0; y < 240; y++) {   /* (widescreen: the middle 320 columns) */
            const u16 *s = (const u16 *)(MEM + src + (y * n + c) * pitch) + hr_off * n + c;
            u16 *d = (u16 *)(MEM + dst + y * 640);
            for (u32 x = 0; x < 320; x++) d[x] = s[x * n];
        }
        hr_shown_buf = src;
        hr_shown_n = n;
        hr_shown_w = rt_hr_w;
        hr_ready = 1;
        {   /* AT_HRDUMP=ms[,ms...]: write the N x frame (raw 15-bit) and the 1x one
               to hr.raw / lo.raw (AT_HRDUMPFILE=<name>: <name>0.raw, <name>1.raw ...) */
            static int at = -1, k;
            static const char *list;
            if (at < 0) { list = getenv(ENV("HRDUMP")); at = list ? atoi(list) : 0; }
            if (at > 0 && SDL_GetTicks64() >= (u64)at) {
                const char *nx = list ? strchr(list, ',') : NULL;
                list = nx ? nx + 1 : NULL;
                at = list ? atoi(list) : 0;
                char fn[300] = "hr.raw";
                const char *pre = getenv(ENV("HRDUMPFILE"));
                if (pre) snprintf(fn, sizeof fn, "%s%d.raw", pre, k++);
                FILE *f = fopen(fn, "wb");
                if (f) { fwrite(MEM + src, 2, rt_hr_w * n * 240 * n, f); fclose(f); }
                f = fopen("lo.raw", "wb");
                if (f) { fwrite(MEM + dst, 2, 320 * 240, f); fclose(f); }
                rt_log("hrdump: %ux width %u\n", n, rt_hr_w * n);
            }
        }
    } else hr_ready = 0;
    rt_wrap_orig("N_hr_copy", NULL)();
}

/* presenter (video.c): the N x frame of the last copy, 15-bit x:1 r:5 g:5 b:5 */
int hires_frame16(const u16 **pix, int *w, int *h)
{
    if (!hr_ready || (hr_shown_n < 2 && hr_shown_w == 320)) return 0;
    *pix = (const u16 *)(MEM + hr_shown_buf);
    *w = (int)(hr_shown_w * hr_shown_n);
    *h = 240 * (int)hr_shown_n;
    return 1;
}

/* the Internal Resolution menu is offered (video.c checks for this hook) */
int hires3d_frame(const u8 **pix, const u8 **mask, int *w, int *h) { (void)pix; (void)mask; (void)w; (void)h; return 0; }
int hires3d_scales(void) { return 0x7e; }

/* 44a444: textured quad.  Packets with the halfword at +22 set are screen
   overlays (HUD, menus, legal text): drawn on the whole screen at their raw
   320x240 coordinates (+0c/+0e, +14/+16, +1c/+1e, +24/+26) without the view
   scaling - scale them here */
static void N_pk_quad_impl(void)
{
    static const u8 o[] = {0x0c, 0x0e, 0x14, 0x16, 0x1c, 0x1e, 0x24, 0x26};
    if (RD16(cpu.r[R_EAX] + 0x22)) scaled_call("N_pk_quad", o, 8);
    else rt_wrap_orig("N_pk_quad", NULL)();
}

/* 2D packets that draw texels 1:1 (sprites, bitmaps, gradient bars): at N x
   the packet is drawn at 1x into a scratch copy of the frame (pre-filled with
   the current picture, so blending sees the right background), and every
   pixel it changed becomes an N x N block of the N x frame */
#define HR_TMP 0x32000000u
static void lowres_call(const char *name, s32 x, s32 y, s32 w, s32 h)
{
    if (!rt_hr_on) { rt_wrap_orig(name, NULL)(); return; }
    /* the scratch frame is the 320x240 picture; in the wide frame it sits at
       column hr_off (o) */
    u32 n = rt_hr_n, W = rt_hr_w, o = (u32)hud_shift_pk(cpu.r[R_EAX]), hb = RD8(0x4ad574) ? HR_BUF1 : HR_BUF0, hp = 2 * W * n, c = n / 2;
    {   /* drawn outside the frame pass (target not the N x frame): plain 1x */
        u32 base = RD32(0x4ad58c);
        if (base < hb || base >= hb + hp * 240 * n) {
            rt_hr_n = 1; rt_hr_w = 320; rt_hr_on = 0;
            rt_wrap_orig(name, NULL)();
            rt_hr_n = n; rt_hr_w = W; rt_hr_on = 1;
            return;
        }
    }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > 320) w = 320 - x;
    if (y + h > 240) h = 240 - y;
    if (w <= 0 || h <= 0) { RET(0); return; }
    for (s32 r = y; r < y + h; r++) {
        const u16 *s = (const u16 *)(MEM + hb + (r * n + c) * hp) + o * n + c;
        u16 *d = (u16 *)(MEM + HR_TMP + r * 640);
        for (s32 i = x; i < x + w; i++) d[i] = s[i * n];
    }
    static u16 before[320 * 240];
    for (s32 r = y; r < y + h; r++) memcpy(before + r * 320 + x, MEM + HR_TMP + r * 640 + x * 2, (size_t)w * 2);
    /* the view block at 1x on the scratch frame (the shrunk view keeps its offset) */
    u32 s58c = RD32(0x4ad58c), s198 = RD32(0x800198), s548 = RD32(0x4ad548), s54c = RD32(0x4ad54c),
        s294 = RD32(0x800294), s29c = RD32(0x80029c);
    u32 off = s58c - hb, orow = off / hp, ocol = (off % hp) / 2 / n;
    ocol = ocol > o ? ocol - o : 0;   /* (the view's left edge in the 320-wide picture) */
    u32 vw = s294 / n;
    if (vw > 320 - ocol) vw = 320 - ocol;
    WR32(0x4ad58c, HR_TMP + orow / n * 640 + ocol * 2);
    WR32(0x800198, 640);
    WR32(0x4ad548, vw - 1);
    WR32(0x4ad54c, (s54c + 1) / n - 1);
    WR32(0x800294, vw);
    WR32(0x80029c, s29c / n);
    rt_hr_n = 1; rt_hr_w = 320; rt_hr_on = 0;
    rt_hr_k = HR_TMP;                 /* handlers that aim at the 1x buffer constant draw here too */
    rt_wrap_orig(name, NULL)();
    rt_hr_k = 0;
    rt_hr_n = n; rt_hr_w = W; rt_hr_on = 1;
    WR32(0x4ad58c, s58c); WR32(0x800198, s198); WR32(0x4ad548, s548); WR32(0x4ad54c, s54c);
    WR32(0x800294, s294); WR32(0x80029c, s29c);
    for (s32 r = y; r < y + h; r++) {
        const u16 *t = (const u16 *)(MEM + HR_TMP + r * 640);
        for (s32 i = x; i < x + w; i++) {
            if (t[i] == before[r * 320 + i]) continue;
            for (u32 a = 0; a < n; a++) {
                u16 *d = (u16 *)(MEM + hb + (r * n + a) * hp) + (i + o) * n;
                for (u32 b = 0; b < n; b++) d[b] = t[i];
            }
        }
    }
}

/* 44cc90: sprite / bitmap: x,y at +0c/+0e, w,h at +14/+16 */
static void N_pk_sprt_impl(void)
{
    u32 pk = cpu.r[R_EAX];
    lowres_call("N_pk_sprt", (s16)RD16(pk + 0x0c), (s16)RD16(pk + 0x0e), (s16)RD16(pk + 0x14), (s16)RD16(pk + 0x16));
}

/* 44e53c: gradient bar: corners +0c/+0e .. +14 / +1e */
static void N_pk_bar_impl(void)
{
    u32 pk = cpu.r[R_EAX];
    s32 x0 = (s16)RD16(pk + 0x0c), y0 = (s16)RD16(pk + 0x0e), x1 = (s16)RD16(pk + 0x14), y1 = (s16)RD16(pk + 0x1e);
    lowres_call("N_pk_bar", x0, y0, x1 - x0 + 1, y1 - y0 + 1);
}

/* ---- spans longer than the drawers can take ----
   The span drawers count a span's pixels in an 8-bit register: the game keeps
   its polygons narrower than 256 pixels at 320x240, at N x they are N times
   wider.  Triangles (44b934) wider than SLICE N x pixels are clipped into
   vertical slices; x, y, u, v are linear over a triangle (affine texturing),
   so the slices draw exactly the original.  Axis-aligned rectangles (44abb4,
   billboards) that are too wide go the same way as two triangles. */
static int slice_w(void) { static int w; if (!w) { const char *e = getenv(ENV("SLICE")); w = e ? atoi(e) : 240; } return w; }
#define SLICE slice_w()
typedef struct { double x, y, u, v; } TV;   /* x in pixels, u/v in texels */
static u32 slice_regs[8];                  /* the rasterizer's register inputs (eax = texture source ...) */

static void tri_draw(const TV *a, const TV *b, const TV *c)
{
    const TV *t[3] = {a, b, c};
    /* the rasterizer takes its vertices top to bottom (y0 <= y1 <= y2), as the
       game's quads deliver them; compare the rounded y it will see */
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2 - i; j++)
            if (lround(t[j]->y) > lround(t[j + 1]->y)) { const TV *x = t[j]; t[j] = t[j + 1]; t[j + 1] = x; }
    /* the rasterizer divides by the triangle's width at the middle vertex's row
       (whole pixels): slivers under a pixel there are skipped */
    {
        long y0 = lround(t[0]->y), y1 = lround(t[1]->y), y2 = lround(t[2]->y);
        double w;
        if (y0 == y2) return;
        if (y0 == y1) w = t[1]->x - t[0]->x;
        else if (y1 == y2) w = t[2]->x - t[1]->x;
        else w = t[0]->x + (t[2]->x - t[0]->x) * (double)(y1 - y0) / (double)(y2 - y0) - t[1]->x;
        if (fabs(w) < 1.0) return;
        if (getenv(ENV("PIECELOG")) && RD8(0x8002d0) == 5) {
            static int k;
            if (k++ < 40) rt_log("piece x %.2f %.2f %.2f y %ld %ld %ld\n", t[0]->x, t[1]->x, t[2]->x, y0, y1, y2);
        }
    }
    for (int i = 0; i < 3; i++) {
        WR32(0x800210 + i * 4, (u32)(s32)lround(t[i]->x * 65536.0));
        WR32(0x800220 + i * 4, (u32)(s32)lround(t[i]->y));
        WR32(0x800238 + i * 4, (u32)(s32)lround(t[i]->u * 65536.0));
        WR32(0x800200 + i * 4, (u32)(s32)lround(t[i]->v * 65536.0));
    }
    u32 at;
    void (*f)(void) = rt_wrap_orig("N_tri", &at);
    for (int i = 0; i < 8; i++) if (i != R_ESP) cpu.r[i] = slice_regs[i];
    cpu.r[R_ESP] -= 4;                 /* a nested call: the original pops its return address */
    WR32(cpu.r[R_ESP], at);
    f();
}

static int clip_x(const TV *in, int n, TV *out, double lim, int keep_ge)
{
    int m = 0;
    for (int i = 0; i < n; i++) {
        const TV *p = &in[i], *q = &in[(i + 1) % n];
        int pin = keep_ge ? p->x >= lim : p->x <= lim, qin = keep_ge ? q->x >= lim : q->x <= lim;
        if (pin) out[m++] = *p;
        if (pin != qin) {
            double t = (lim - p->x) / (q->x - p->x);
            TV r = {lim, p->y + (q->y - p->y) * t, p->u + (q->u - p->u) * t, p->v + (q->v - p->v) * t};
            out[m++] = r;
        }
    }
    return m;
}

static int clip_y(const TV *in, int n, TV *out, double lim, int keep_ge)
{
    int m = 0;
    for (int i = 0; i < n; i++) {
        const TV *p = &in[i], *q = &in[(i + 1) % n];
        int pin = keep_ge ? p->y >= lim : p->y <= lim, qin = keep_ge ? q->y >= lim : q->y <= lim;
        if (pin) out[m++] = *p;
        if (pin != qin) {
            double t = (lim - p->y) / (q->y - p->y);
            TV r = {p->x + (q->x - p->x) * t, lim, p->u + (q->u - p->u) * t, p->v + (q->v - p->v) * t};
            out[m++] = r;
        }
    }
    return m;
}

/* the drawers take shapes up to the 320x240 screen's size: wider or taller
   triangles (and any crossing the left edge, where their own clip goes wrong
   at N x) are cut into tiles of SLICE x SLICE */
static int tri_fits(const TV v[3])
{
    double xl = v[0].x, xh = v[0].x, yl = v[0].y, yh = v[0].y;
    for (int i = 1; i < 3; i++) {
        if (v[i].x < xl) xl = v[i].x; if (v[i].x > xh) xh = v[i].x;
        if (v[i].y < yl) yl = v[i].y; if (v[i].y > yh) yh = v[i].y;
    }
    /* inside one cell of a fixed grid: neighbours that share an edge cut it at
       the same points (rounded y), so no seams open between them */
    return xl >= 0 && floor(xl / SLICE) == floor((xh - 1e-6) / SLICE) && floor(yl / SLICE) == floor((yh - 1e-6) / SLICE);
}

static void tri_sliced(TV v[3])
{
    if (tri_fits(v)) { tri_draw(&v[0], &v[1], &v[2]); return; }
    double xl = v[0].x, xh = v[0].x, yl = v[0].y, yh = v[0].y;
    for (int i = 1; i < 3; i++) {
        if (v[i].x < xl) xl = v[i].x; if (v[i].x > xh) xh = v[i].x;
        if (v[i].y < yl) yl = v[i].y; if (v[i].y > yh) yh = v[i].y;
    }
    double w = (s32)RD32(0x4ad548) + 1, h = (s32)RD32(0x4ad54c) + 1;
    if (xl < 0) xl = 0;
    if (xh > w) xh = w;
    if (yl < 0) yl = 0;
    if (yh > h) yh = h;
    for (double y0 = floor(yl / SLICE) * SLICE; y0 < yh; y0 += SLICE) {
        TV a[10], b[10];
        int n = clip_y(v, 3, a, y0, 1);
        n = clip_y(a, n, b, y0 + SLICE, 0);
        if (n < 3) continue;
        for (double x0 = floor(xl / SLICE) * SLICE; x0 < xh; x0 += SLICE) {
            TV c[12], d[14];
            int m = clip_x(b, n, c, x0, 1);
            m = clip_x(c, m, d, x0 + SLICE, 0);
            for (int i = 1; i + 1 < m; i++) tri_draw(&d[0], &d[i], &d[i + 1]);
        }
    }
}

/* 44b934: triangle, vertices x 800210.. (16.16), y 800220.., u 800238.. and
   v 800200.. (16.16); eax = packet (kept) */
static void probe(const char *what)
{   /* AT_PROBE=x,y: log the rasterizer calls whose box holds that N x pixel */
    static int px = -2, py;
    if (px == -2) { const char *e = getenv(ENV("PROBE")); px = -1; if (e) sscanf(e, "%d,%d", &px, &py); }
    if (px < 0) return;
    s32 x0 = 1 << 30, x1 = -(1 << 30), y0 = 1 << 30, y1 = -(1 << 30);
    int rect = what[0] == 'r';
    for (int i = 0; i < 3; i++) {
        s32 x = (s32)RD32(0x800210 + i * 4), y = (s32)RD32(0x800220 + i * 4);
        if (!rect) x >>= 16;
        if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
    }
    if (px < x0 || px > x1 || py < y0 || py > y1) return;
    static int k;
    if (k++ > 60) return;
    rt_log("probe %s mode %u eax %08x c6 %02x 230 %08x 234 %08x 1a0 %08x 1e0 %08x 26c %08x 248 %08x 268 %08x 27c %08x x %d..%d y %d..%d\n", what,
           RD8(0x8002d0), cpu.r[R_EAX], RD8(0x8002c6), RD32(0x800230), RD32(0x800234), RD32(0x8001a0), RD32(0x8001e0),
           RD32(0x80026c), RD32(0x800248), RD32(0x800268), RD32(0x80027c), x0, x1, y0, y1);
}

static void tri_impl(void)
{
    if (getenv(ENV("M5LOG")) && RD8(0x8002d0) == 5) {
        static int k;
        if (k++ < 12) rt_log("m5 x %d %d %d y %d %d %d c6 %02x\n", (s32)RD32(0x800210) >> 16, (s32)RD32(0x800214) >> 16, (s32)RD32(0x800218) >> 16,
                             (s32)RD32(0x800220), (s32)RD32(0x800224), (s32)RD32(0x800228), RD8(0x8002c6));
    }
    if (!rt_hr_on) { rt_wrap_orig("N_tri", NULL)(); return; }
    TV v[3];
    int big = 0;
    for (int i = 0; i < 3; i++) {
        v[i].x = (s32)RD32(0x800210 + i * 4) / 65536.0;
        if (nbigx && big_x(RD32(0x800210 + i * 4), &v[i].x)) big = 1;
        v[i].y = (s32)RD32(0x800220 + i * 4);
        v[i].u = (s32)RD32(0x800238 + i * 4) / 65536.0;
        v[i].v = (s32)RD32(0x800200 + i * 4) / 65536.0;
    }
    if (!big && tri_fits(v)) { rt_wrap_orig("N_tri", NULL)(); return; }
    {   /* 44b934's own rejects, which the pieces would no longer hit: a triangle
           reaching past both the left and right edge, or from above -64 to
           below 281 rows (320x240 units; behind-the-camera geometry) */
        double wl = (double)rt_hr_w * rt_hr_n - 1;
        int r = 0, l = 0, t = 0, b2 = 0;
        for (int i = 0; i < 3; i++) {
            if (v[i].x > wl) r = 1;
            if (v[i].x < 0) l = 1;
            if (v[i].y < -64.0 * rt_hr_n) t = 1;
            if (v[i].y > 281.0 * rt_hr_n) b2 = 1;
        }
        if (((r && l) || (t && b2)) && !getenv(ENV("NOCULL"))) { tri_path = "cull"; RET(0); return; }
    }
    memcpy(slice_regs, cpu.r, sizeof slice_regs);
    tri_path = big ? "slice-big" : "slice";
    if (getenv(ENV("SLICELOG"))) {
        static int k;
        if (k++ < 40) {
            rt_log("slice mode %u eax %08x |", RD8(0x8002d0), cpu.r[R_EAX]);
            for (u32 a = 0x800190; a < 0x800250; a += 4) rt_log(" %08x", RD32(a));
            rt_log("\n");
        }
    }
    tri_sliced(v);
    for (int i = 0; i < 8; i++) if (i != R_ESP) cpu.r[i] = slice_regs[i];
    RET(0);
}

/* 44abb4: axis-aligned rectangle x 800210..800214, y 800220..800228 (pixels),
   u 800238..80023c across, v 800200..800208 down (texels) */
static void rect_impl(void)
{
    s32 x0 = (s32)RD32(0x800210), x1 = (s32)RD32(0x800214), y0 = (s32)RD32(0x800220), y1 = (s32)RD32(0x800228);
    if (!rt_hr_on || (x1 - x0 <= SLICE && y1 - y0 <= SLICE && x0 >= 0)) { rt_wrap_orig("N_rect", NULL)(); return; }
    /* 44abb4's own rejects */
    if (y1 < 0 || y0 > (s32)RD32(0x4ad54c) || x0 > (s32)RD32(0x4ad548) || x1 < 0 || x0 == x1) { RET(0); return; }
    double u0 = (s32)RD32(0x800238), u1 = (s32)RD32(0x80023c), v0 = (s32)RD32(0x800200), v1 = (s32)RD32(0x800208);
    memcpy(slice_regs, cpu.r, sizeof slice_regs);
    TV a[3] = {{x0, y0, u0, v0}, {x1, y0, u1, v0}, {x0, y1, u0, v1}};
    TV b[3] = {{x1, y0, u1, v0}, {x1, y1, u1, v1}, {x0, y1, u0, v1}};
    tri_sliced(a);
    tri_sliced(b);
    for (int i = 0; i < 8; i++) if (i != R_ESP) cpu.r[i] = slice_regs[i];
    RET(0);
}

/* AT_PROBE=x,y (N x pixel): log every triangle / rectangle that changes it */
static u16 *probe_px(void)
{
    static int px = -2, py;
    if (px == -2) { const char *e = getenv(ENV("PROBE")); px = -1; if (e) sscanf(e, "%d,%d", &px, &py); }
    if (px < 0) return NULL;
    u32 n = rt_hr_n, b = rt_hr_on ? (RD8(0x4ad574) ? HR_BUF1 : HR_BUF0) : (RD8(0x4ad574) ? BUF1 : BUF0);
    return (u16 *)(MEM + b + (u32)py * 2 * rt_hr_w * n + (u32)px * 2);
}
static void probed(void (*f)(void), const char *what)
{
    if (cmp_log && cmp_f) {
        static int idx;
        s32 x[3], y[3];
        int raw = what[0] == 'r';
        for (int i = 0; i < 3; i++) { x[i] = (s32)RD32(0x800210 + i * 4); y[i] = (s32)RD32(0x800220 + i * 4); if (!raw && what[0] == 't') x[i] >>= 16; }
        tri_path = "orig";
        f();
        fprintf(cmp_f, "%s mode %u eax %08x n %u x %d %d %d y %d %d %d path %s\n", what, RD8(0x8002d0), cpu.r[R_EAX], rt_hr_n,
                x[0], x[1], x[2], y[0], y[1], y[2], tri_path);
        (void)idx;
        return;
    }
    u16 *pp = probe_px();
    { static int at = -1; if (at < 0) { const char *e = getenv(ENV("PROBE_AT")); at = e ? atoi(e) : 0; }
      if (pp && SDL_GetTicks64() < (u64)at) pp = NULL; }
    if (!pp) { f(); return; }
    u16 before = *pp;
    u32 mode = RD8(0x8002d0), eax = cpu.r[R_EAX];
    s32 x[3], y[3];
    for (int i = 0; i < 3; i++) { x[i] = (s32)RD32(0x800210 + i * 4); y[i] = (s32)RD32(0x800220 + i * 4); }
    f();
    if (*pp != before) {
        static int k;
        if (k++ < 80) rt_log("probe %s %04x->%04x mode %u eax %08x x %d %d %d y %d %d %d\n", what, before, *pp, mode, eax,
                             x[0], x[1], x[2], y[0], y[1], y[2]);
    }
}
void N_tri(void) { probed(tri_impl, "tri"); }
void N_rect(void) { probed(rect_impl, "rect"); }
void N_pk_sprt(void) { probed(N_pk_sprt_impl, "sprt"); }
void N_pk_bar(void) { probed(N_pk_bar_impl, "bar"); }
void N_pk_tile(void) { probed(N_pk_tile_impl, "tile"); }
void N_pk_quad(void) { probed(N_pk_quad_impl, "quad"); }
void N_pk_4498dc(void) { probed(N_pk_4498dc_impl, "4498dc"); }
void N_pk_449dd0(void) { probed(N_pk_449dd0_impl, "449dd0"); }
void N_pk_f3(void) { probed(N_pk_f3_impl, "f3"); }

/* 44960c: line (radar, HUD): it steps rows by a computed y*640, so at N x it
   is drawn at 1x and its pixels become N x N blocks */
static void N_pk_line_impl(void) { lowres_call("N_pk_line", 0, 0, 320, 240); }
void N_pk_line(void) { probed(N_pk_line_impl, "line"); }

/* 44b818: sprite (x,y at +0c/+0e, size from its texture header): drawn at 1x,
   blocks into the N x frame */
static void N_pk_sprite2_impl(void) { lowres_call("N_pk_sprite2", 0, 0, 320, 240); }
void N_pk_sprite2(void) { probed(N_pk_sprite2_impl, "sprite2"); }
