/* Skippable start-up movies.
 *
 * TRILOGY.EXE runs the movie player as "DUKDOS <n>".  DUKDOS lets a movie be
 * stopped only when n > 100 (then n - 100 is the movie), and then only with
 * Esc (3010280: kbhit + getch == 1Bh).  The start-up chain (n = 14: Fox,
 * Acclaim, Probe logos, intro) comes as 14 - nothing stops it.  Here:
 *   - the start-up chain is started as 114;
 *   - 3010280 (native): any key stops the current movie, Esc also skips the
 *     rest of the chain (3010010, "play one movie", returns at once). */
#include <string.h>
#include "rt.h"

#define RET(n) (cpu.r[R_ESP] += 4 + (n))
extern void (*rt_wrap_orig(const char *native, u32 *addr))(void);

static int skip_rest;

const char *game_exec_args(const char *name, const char *args)
{
    if (!_stricmp(name, "DUKDOS")) {
        skip_rest = 0;
        if (!strcmp(args, "14")) return "114";
    }
    return args;
}

/* call a DUKDOS routine from native code (it pops the return address we push) */
static u32 guest_call(u32 addr)
{
    extern void rt_call(u32 target, u32 at);
    cpu.r[R_ESP] -= 4;
    WR32(cpu.r[R_ESP], addr);
    rt_call(addr, addr);
    return cpu.r[R_EAX];
}

/* 3010280: has a key been pressed that stops the movie?  (1 = stop) */
void N_movie_key(void)
{
    u32 save[8];
    memcpy(save, cpu.r, sizeof save);
    u32 stop = 0;
    if (guest_call(0x3012f98)) {                 /* kbhit */
        u32 k = guest_call(0x3011b81);           /* getch */
        rt_log("movie: key %08x\n", k);
        if (k == 0x1b) skip_rest = 1;
        stop = 1;
    }
    memcpy(cpu.r, save, sizeof save);
    cpu.r[R_EAX] = stop;
    RET(0);
}

/* 3010010: play one movie of the chain - not after Esc */
void N_movie_play(void)
{
    if (skip_rest) { cpu.r[R_EAX] = 0; RET(0); return; }
    rt_wrap_orig("N_movie_play", NULL)();
}
