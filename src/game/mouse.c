/* Mouse options (in the game's Options panel and in trilogy_pc.ini [game]):
 *   Mouse=1        mouse moves the player (0: buttons only)
 *   MouseSpeed=3   1..5: x0.5, x0.71, x1 (the speed so far), x1.41, x2
 *   MouseLook=1    mouse up/down looks up/down like a modern FPS (the game's
 *                  own pitch, same limits as its look keys); 0 = the original:
 *                  mouse up/down walks forward/back.  With it on, the game's
 *                  automatic view tilt on stairs/slopes is off (43f318: floor
 *                  tile 2dh-38h tilts the pitch by facing; tools/subst_trilogy.txt)
 *
 * 460320 reads the mouse once per logic step (int 33h 0Bh): it leaves the
 * turn in 804656 (mickeys x / 2, added to the yaw 4b0a52) and the walk in
 * 804658 (mickeys y).  The wrapper scales / clears them and turns y into
 * pitch: 4b0a50 = view pitch (12-bit angle, -128 look up .. +128 look down),
 * 4b0b42 = the pitch the view returns to when no look key is held (set per
 * level, 48) - written together so the view stays where the mouse put it. */
#include <math.h>
#include <string.h>
#include "rt.h"

#define RET(n) (cpu.r[R_ESP] += 4 + (n))
extern int settings_int(const char *sec, const char *key, int def, int lo, int hi);

static const double speed[6] = {1.0, 0.5, 0.7071, 1.0, 1.4142, 2.0};
static double accx, accy;
static int look_pitch, look_rest = 0x7fffffff, look_written = 0x7fffffff;

int mouse_opt_on(void) { return settings_int("game", "Mouse", 1, 0, 1); }
int mouse_opt_speed(void) { return settings_int("game", "MouseSpeed", 3, 1, 5); }
int mouse_opt_look(void) { return settings_int("game", "MouseLook", 1, 0, 1); }

void N_mouse_read(void)
{
    rt_wrap_orig("N_mouse_read", NULL)();
    /* freelook: no automatic view tilt on stairs/slopes (substituted check at 43f320) */
    rt_game_var[1] = mouse_opt_on() && mouse_opt_look();
    s16 dx = (s16)RD16(0x804656), dy = (s16)RD16(0x804658);
    if (!mouse_opt_on()) { WR16(0x804656, 0); WR16(0x804658, 0); accx = accy = 0; return; }
    double s = speed[mouse_opt_speed()];
    accx += dx * s;
    int ix = (int)accx;
    accx -= ix;
    WR16(0x804656, (u16)(s16)ix);
    s16 rest = (s16)RD16(0x4b0b42);
    if (!mouse_opt_look()) {
        if (look_written != 0x7fffffff && rest == look_written && look_rest != 0x7fffffff) {
            WR16(0x4b0b42, (u16)(s16)look_rest);   /* mouse look switched off: the level's own resting pitch again */
            look_written = 0x7fffffff;
        }
        accy += dy * s;
        int iy = (int)accy;
        accy -= iy;
        WR16(0x804658, (u16)(s16)iy);
        return;
    }
    /* a new level (or the game itself) set the resting pitch: start from it */
    if (rest != look_written) { look_rest = rest; look_pitch = rest; }
    accy += dy * s * 0.5;                    /* same scale as turning (mickeys / 2) */
    int iy = (int)accy;
    accy -= iy;
    look_pitch += iy;
    if (look_pitch < -128) look_pitch = -128;
    if (look_pitch > 128) look_pitch = 128;
    if (iy) WR16(0x4b0a50, (u16)(s16)look_pitch);   /* moved: the view follows at once (keys still work) */
    WR16(0x4b0b42, (u16)(s16)look_pitch);
    look_written = look_pitch;
    WR16(0x804658, 0);                       /* no walking from mouse up/down */
}

/* ---- the three rows in the game's in-game Options column ----
   4443dc draws the column (SFX VOLUME, its bar, MUSIC VOLUME, its bar, EXIT
   GAME: rows from y 32 in 40-pixel steps, text with 443604 (eax string,
   edx x, ebx y, ecx brightness), box with 443660 (eax x-2, edx y-2, ebx
   brightness), bars as strings of '*' (two per block)).  Selection 649870
   (0-2, state 64985c >= 3 while in the column) wraps at rt_game_var[0]
   (tools/subst_trilogy.txt), Enter goes to 444dc0.  Rows 3-5 are ours:
   MOUSE ON/OFF, MOUSE SPEED + bar, FREELOOK ON/OFF (mouse look) - Enter changes them. */
extern void settings_set_int(const char *sec, const char *key, int v);
#define MENU_STR 0x32100000u    /* our row texts in guest memory */

static u32 put_str(int slot, const char *s)
{
    u32 a = MENU_STR + (u32)slot * 32;
    for (int i = 0; s[i] && i < 31; i++) WR8(a + i, (u8)s[i]);
    WR8(a + (u32)strlen(s), 0);
    return a;
}

static void gcall(u32 fn, u32 eax, u32 edx, u32 ebx, u32 ecx)
{
    extern void rt_call(u32 target, u32 at);
    cpu.r[R_EAX] = eax; cpu.r[R_EDX] = edx; cpu.r[R_EBX] = ebx; cpu.r[R_ECX] = ecx;
    cpu.r[R_ESP] -= 4;
    WR32(cpu.r[R_ESP], fn);
    rt_call(fn, fn);
}

static void draw_row(int row, int y, const char *text, int slot)
{
    int sel = RD16(0x64985c) >= 3 && RD16(0x649870) == row && ((RD32(0x4b0bb8) >> 3) & 1);
    int on = RD16(0x4ad464) != 0;   /* lit like the game's own rows (SFX VOLUME's record) */
    gcall(0x443604, put_str(slot, text), 0xa2, (u32)y, sel ? 0xff : on ? 0x46 : 0x2e);
    gcall(0x443660, 0xa2 - 2, (u32)(y - 2), on ? 0x80 : 0x20, 0);
}

void N_menu_draw(void)
{
    rt_game_var[0] = 5;
    rt_wrap_orig("N_menu_draw", NULL)();
    u32 save[8];
    memcpy(save, cpu.r, sizeof save);
    draw_row(3, 152, mouse_opt_on() ? "MOUSE ON" : "MOUSE OFF", 0);
    draw_row(4, 184, "MOUSE SPEED", 1);
    {   /* its bar: two '*' per block, like the volumes */
        char bar[16];
        int n = mouse_opt_speed();          /* one '*' = one block: 1-5, 3 = the old speed */
        for (int i = 0; i < n; i++) bar[i] = '*';
        bar[n] = 0;
        gcall(0x443604, put_str(2, bar), 0xa2, 204, RD16(0x4ad464) && mouse_opt_on() ? 0x80 : 0x40);
    }
    draw_row(5, 224, mouse_opt_look() ? "FREELOOK ON" : "FREELOOK OFF", 3);
    memcpy(cpu.r, save, sizeof save);
}

void N_menu_enter(void)
{
    int row = RD16(0x649870);
    if (row < 3) { rt_wrap_orig("N_menu_enter", NULL)(); return; }
    if (row == 3) settings_set_int("game", "Mouse", !mouse_opt_on());
    if (row == 4) settings_set_int("game", "MouseSpeed", mouse_opt_speed() % 5 + 1);
    if (row == 5) settings_set_int("game", "MouseLook", !mouse_opt_look());
    RET(0);
}
