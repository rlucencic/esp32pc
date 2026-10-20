/* The 8088's and 8086's prefetch queues against code that changes the
 * bytes ahead: a REP STOSB writes INC DX over a NOP 1, 4 and 7 bytes past
 * its end; DX says whether the new byte ran (1) or the queued old one (0).
 * An 8088 (4 bytes) shows 0 1 1, an 8086 (6 bytes) 0 0 1.
 */
#include "boot.h"

#define PROBE(n, k) \
    xorw %dx, %dx; \
    movw $A(patch##n), %di; \
    movb $0x42, %al; \
    movw $1, %cx; \
    cld; \
    rep stosb; \
    .fill k, 1, 0x90; \
patch##n: nop; \
    movw %dx, %ax; \
    call hex16; \
    movb $' ', %al; \
    movb $0x0E, %ah; \
    movw $7, %bx; \
    int $0x10

main:
    movw $A(t_queue), %si
    call print
    cli
    PROBE(1, 1)
    PROBE(4, 4)
    PROBE(7, 7)
    sti
    call newline
1:  hlt
    jmp 1b

t_queue: .asciz "queue probes 1 4 7: "
