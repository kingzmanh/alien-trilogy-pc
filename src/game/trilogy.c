/* Alien Trilogy natives (tools/native_trilogy.txt).
 *
 * 44d2c8 - start of a game-logic frame ("speed compensation").  The original
 * waits until its 69 Hz timer count (80415f) is even, stores ticks+1 in
 * 4ad5d0 and clears the count; 412590 then runs the object list
 * (4ad5d0+1)/2 times, at least once.  The count only covers the time since the
 * frame copy, and on a PC far faster than 1996 machines it is 0 every frame:
 * one logic step per frame at several hundred frames a second - the game,
 * and the attract-mode demos (one recorded input per step), ran at many times
 * their speed.  Here the steps come from a fixed clock: one step per two
 * ticks of the game's own timer (34.5 Hz), waiting when the PC is early and
 * running up to 4 steps when it is late.  [game] GameSpeed=<percent>
 * scales the clock (100 = the game's rate). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <SDL2/SDL.h>
#include "rt.h"

#define RET(n) (cpu.r[R_ESP] += 4 + (n))

extern void irq_poll(void);
extern u32 timer_divisor(void);
extern int settings_int(const char *sec, const char *key, int def, int lo, int hi);

void N_logic_start(void)
{
    if (RD8(0x4ae128)) {
        static u64 next;
        u64 f = SDL_GetPerformanceFrequency();
        u32 div = timer_divisor();
        if (div < 1000 || div > 65536) div = 17290;          /* the game's 69 Hz */
        int pct = settings_int("game", "GameSpeed", 100, 25, 400);
        u64 per = f * 2 * div / 1193182 * 100 / (u64)pct;
        u64 now = SDL_GetPerformanceCounter();
        if (!next || now > next + per * 8 || next > now + per * 2) next = now;   /* start, loading pauses */
        while ((now = SDL_GetPerformanceCounter()) < next) {
            u64 ms = (next - now) * 1000 / f;
            if (ms > 1) SDL_Delay((Uint32)(ms - 1));
            irq_poll();
        }
        u32 n = 1 + (u32)((now - next) / per);
        if (n > 4) n = 4;
        next += per * n;
        WR32(0x4ad5d0, 2 * n - 1);     /* 412590 runs (2n-1+1)/2 = n steps */
        WR8(0x80415f, 0);
    }
    RET(0);
}
