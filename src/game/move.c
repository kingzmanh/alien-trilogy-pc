/* Player speed: the same speed in every direction, Shift walks.
 *
 * The player has one speed (4b0a64) and one move direction (4b0a5a).  Each
 * logic step a held move key runs a speed routine:
 *   43d2b8  forward:            speed += accel (4b0b48), up to max (4b0b46)
 *   43d340  back, strafe, diagonals: speed += accel/2, up to max/2
 * (both halve again on floor tiles 5 and 9 unless 4b0abc is set).  So
 * backing up and strafing ran at half speed.  Here 43d340 runs the forward
 * routine, and while Shift is held the speed is capped at half (walking).
 * Shift was the game's run key (44d8ce sets the run bit: top speed 90h);
 * tools/subst_trilogy.txt takes that out, so normal speed is the top one. */
#include "rt.h"

#define RET(n) (cpu.r[R_ESP] += 4 + (n))
extern void (*rt_wrap_orig(const char *native, u32 *addr))(void);

#define KEYDOWN 0x7e9e18   /* the game's key-down table, by scancode */

static void guest_call(u32 addr)
{
    extern void rt_call(u32 target, u32 at);
    cpu.r[R_ESP] -= 4;
    WR32(cpu.r[R_ESP], addr);
    rt_call(addr, addr);
}

static void walk_cap(void)
{
    if (!RD8(KEYDOWN + 0x2a) && !RD8(KEYDOWN + 0x36)) return;   /* left / right Shift */
    int cap = (s16)RD16(0x4b0b46);
    u8 tile = RD8(RD32(0x4b0ab0) + 0xa);
    if (!RD16(0x4b0abc) && (tile == 5 || tile == 9)) cap >>= 1;   /* the game's slow floors */
    cap >>= 1;
    if ((s16)RD16(0x4b0a64) > cap) WR16(0x4b0a64, (u16)cap);
}

/* 43d2b8 (wrap): forward speed step */
void N_move_fwd(void)
{
    rt_wrap_orig("N_move_fwd", NULL)();
    walk_cap();
}

/* 43d340: back / strafe speed step - the forward one */
void N_move_side(void)
{
    guest_call(0x43d2b8);
    RET(0);
}
