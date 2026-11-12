/* Native replacements for game / library functions that talk to PC hardware
 * or modify their own code.  Each one stands in for the whole original
 * function (listed in tools/native_<module>.txt as "<hex addr> <C name>",
 * or "<hex addr> <C name> wrap" to keep the original callable through
 * rt_wrap_orig) and works on the guest register file like the original:
 * Watcom register convention (eax, edx, ebx, ecx), result in eax, and it pops
 * its own return address.  See libs/examples for real ones. */
#include <stdlib.h>
#include "rt.h"

#define RET(n) (cpu.r[R_ESP] += 4 + (n))   /* return, popping n bytes of arguments */

/* Watcom _chain_intr(far handler in dx:eax) - in every Watcom program that
   chains an interrupt (find it by its body: mov ecx,eax; mov eax,edx;
   mov esp,ebp; xchg [ebp+28h],ecx; xchg [ebp+2ch],eax; pop gs/fs/es/ds ...
   retf).  Called from inside an interrupt handler whose frame starts at ebp;
   the original rewinds to that frame and retf's into the previous handler.
   Here the previous handler runs first and control returns to the caller,
   whose epilogue (pop segs, popad, iretd) unwinds the same frame. */
extern void irq_magic(u32 n);
extern void irq_call_handler(u32 h);
void N_chain_intr(void)
{
    u32 target = cpu.r[R_EAX];
    cpu.r[R_ESP] = cpu.r[R_EBP];
    if (target >= MAGIC_IRET && target < MAGIC_IRET + 0x100) irq_magic(target - MAGIC_IRET);
    else irq_call_handler(target);
    cpu.r[R_ESP] = cpu.r[R_EBP];
}
