/* The coprocessor (PC_FPU=1): found as programs look for it, the BIOS's
 * equipment bit, arithmetic, the functions, integers and BCD, comparisons,
 * FPREM's quotient bits, FXAM, the 287's (and 8087's) projective infinity,
 * FSAVE and FRSTOR, exact 64- and 80-bit moves, an integer overflow, and an
 * unmasked zero divide reaching INT 2 (the AT's IRQ 13 through INT 75h,
 * the XT's NMI). Each line: the value, what it should be, OK or BAD; at
 * the end the control word FNINIT left and the number of BADs.
 */
#include "boot.h"

#define CHECK(label, want)  movw $A(label), %si; movw $(want), %bx; call check

main:
    /* ---- there: FNINIT, FNSTSW 0, FNSTCW 037F (the 287) or 03FF (the 8087) ---- */
    movw $0xFFFF, A(w)
    fninit
    fnstsw A(w)
    movw A(w), %ax
    CHECK(t_sw, 0)
    fnstcw A(w)
    movw A(w), %ax
    movw %ax, A(cw0)
    movw $A(t_cw), %si
    call print
    call hex16
    call newline
    int $0x11                       /* the equipment: bit 1 */
    andw $2, %ax
    CHECK(t_equip, 2)

    /* ---- (1.5 + 2.25) x 4 = 15 ---- */
    flds A(f_1_5)
    fadds A(f_2_25)
    fimuls A(i_4)
    fistps A(w)
    movw A(w), %ax
    CHECK(t_arith, 15)

    /* ---- sqrt(2) x 10000, pi x 10000 ---- */
    filds A(i_2)
    fsqrt
    fimuls A(i_10000)
    fistps A(w)
    movw A(w), %ax
    CHECK(t_sqrt, 14142)
    fldpi
    fimuls A(i_10000)
    fistps A(w)
    movw A(w), %ax
    CHECK(t_pi, 31416)

    /* ---- 1 / 3 as a double: 3FD5 5555 5555 5555 ---- */
    fld1
    fidivs A(i_3)
    fstpl A(q)
    movw A(q) + 6, %ax
    CHECK(t_third, 0x3FD5)
    movw A(q), %ax
    CHECK(t_third2, 0x5555)

    /* ---- BCD: 1234567 ---- */
    fildl A(l_1234567)
    fbstp A(t)
    movw A(t), %ax
    CHECK(t_bcd, 0x4567)
    fbld A(t)
    fistpl A(l)
    movw A(l), %ax
    CHECK(t_bcd2, 0xD687)           /* (1234567 = 12 D687) */

    /* ---- tan(0.5), atan(1) x 4, 2^0.5 - 1, log2(8) ---- */
    flds A(f_0_5)
    fptan
    fdivrp %st, %st(1)              /* ST(1) / ST */
    fimuls A(i_10000)
    fistps A(w)
    movw A(w), %ax
    CHECK(t_tan, 5463)
    fld1
    fld1
    fpatan
    fimuls A(i_4)
    fimuls A(i_10000)
    fistps A(w)
    movw A(w), %ax
    CHECK(t_atan, 31416)
    flds A(f_0_5)
    f2xm1
    fimuls A(i_10000)
    fistps A(w)
    movw A(w), %ax
    CHECK(t_2xm1, 4142)
    fld1
    filds A(i_8)
    fyl2x
    fistps A(w)
    movw A(w), %ax
    CHECK(t_log, 3)

    /* ---- FPREM: 10 mod 3 = 1, quotient 3: C3 and C1 ---- */
    filds A(i_3)
    filds A(i_10)
    fprem
    fnstsw A(w)
    fistps A(l)
    fstp %st(0)
    movw A(l), %ax
    CHECK(t_prem, 1)
    movw A(w), %ax
    andw $0x4700, %ax
    CHECK(t_premcc, 0x4200)

    /* ---- 1 against 2: less (C0) ---- */
    filds A(i_2)
    fld1
    fcompp
    fnstsw A(w)
    movw A(w), %ax
    andw $0x4500, %ax
    CHECK(t_com, 0x0100)

    /* ---- +inf against -inf: equal under projective closure (the 287), ZE ---- */
    fninit
    fld1
    fldz
    fdivrp %st, %st(1)              /* 1 / 0 */
    fld %st(0)
    fchs
    fcompp
    fnstsw A(w)
    movw A(w), %ax
    andw $0x4504, %ax
    CHECK(t_inf, 0x4004)

    /* ---- FXAM of -0: zero (C3), negative (C1) ---- */
    fldz
    fchs
    fxam
    fnstsw A(w)
    fstp %st(0)
    movw A(w), %ax
    andw $0x4700, %ax
    CHECK(t_xam, 0x4200)

    /* ---- FSAVE, FINIT, FRSTOR: pi again ---- */
    fninit
    fldpi
    fnsave A(save)
    fldz                            /* (a clean stack after FSAVE's FINIT) */
    fstp %st(0)
    frstor A(save)
    fimuls A(i_10000)
    fistps A(w)
    movw A(w), %ax
    CHECK(t_save, 31416)

    /* ---- exact moves: a 64-bit integer, an 80-bit value ---- */
    fildll A(q_in)
    fistpll A(q)
    movw A(q) + 6, %ax
    CHECK(t_q, 0x1234)
    movw A(q), %ax
    CHECK(t_q2, 0xDEF1)
    fldt A(t_in)
    fstpt A(t)
    movw A(t), %ax
    CHECK(t_t, 0x1235)
    movw A(t) + 8, %ax
    CHECK(t_t2, 0x4001)

    /* ---- 40000 into 16 bits: the integer indefinite, IE ---- */
    fninit
    fildl A(l_40000)
    fistps A(w)
    fnstsw A(l)
    movw A(w), %ax
    CHECK(t_ovf, 0x8000)
    movw A(l), %ax
    andw $1, %ax
    CHECK(t_ovfie, 1)

    /* ---- an unmasked zero divide: INT 2 ---- */
    cli
    movw $A(int2), 2 * 4
    movw $0, 2 * 4 + 2
    cmpw $0x03FF, A(cw0)
    je 1f
    inb $0xA1, %al                  /* the AT: IRQ 13 (and the cascade) */
    andb $0xDF, %al
    outb %al, $0xA1
    inb $0x21, %al
    andb $0xFB, %al
    outb %al, $0x21
    jmp 2f
1:  movb $0x80, %al                 /* the XT: NMIs on */
    outb %al, $0xA0
2:  sti
    movb $0, A(nmis)
    fninit
    fldcw A(cw_ze)
    fld1
    fldz
    fdivrp %st, %st(1)
    nop
    nop
    hlt                             /* (the timer's tick at the latest) */
    fninit
    movb A(nmis), %al
    xorb %ah, %ah
    CHECK(t_int, 1)
    movw $A(t_end), %si
    call print
    movw A(cw0), %ax
    call hex16
    movw $A(t_bads), %si
    call print
    movw A(bads), %ax
    call hex16
    call newline

3:  hlt
    jmp 3b

int2:
    incb %cs:A(nmis)
    fnclex
    iret

/* SI: the label, AX: the value, BX: what it should be */
check:
    call print
    call hex16
    pushw %ax
    movb $0x0E, %ah
    movb $' ', %al
    int $0x10
    movw %bx, %ax
    call hex16
    popw %ax
    movw $A(s_ok), %si
    cmpw %bx, %ax
    je 1f
    movw $A(s_bad), %si
    incw A(bads)
1:  call print
    call newline
    ret

f_1_5:      .long 0x3FC00000
f_2_25:     .long 0x40100000
f_0_5:      .long 0x3F000000
i_2:        .word 2
i_3:        .word 3
i_4:        .word 4
i_8:        .word 8
i_10:       .word 10
i_10000:    .word 10000
l_1234567:  .long 1234567
l_40000:    .long 40000
q_in:       .quad 0x123456789ABCDEF1
t_in:       .quad 0xC000000000001235    /* 6.0000...01 (bits below a double's) */
            .word 0x4001
cw_ze:      .word 0x037B                /* ZE unmasked (and the 8087's interrupts on) */
cw0:        .word 0
nmis:       .byte 0
bads:       .word 0
w:          .word 0
l:          .long 0
q:          .quad 0
t:          .quad 0, 0
save:       .space 94

t_sw:       .asciz "FNSTSW after FNINIT: "
t_cw:       .asciz "FNSTCW (037F 287, 03FF 8087): "
t_equip:    .asciz "INT 11h coprocessor bit: "
t_arith:    .asciz "(1.5 + 2.25) x 4: "
t_sqrt:     .asciz "sqrt(2) x 10000: "
t_pi:       .asciz "pi x 10000: "
t_third:    .asciz "1/3 double, top: "
t_third2:   .asciz "1/3 double, bottom: "
t_bcd:      .asciz "FBSTP 1234567: "
t_bcd2:     .asciz "FBLD back: "
t_tan:      .asciz "tan(0.5) x 10000: "
t_atan:     .asciz "atan(1) x 4 x 10000: "
t_2xm1:     .asciz "(2^0.5 - 1) x 10000: "
t_log:      .asciz "log2(8): "
t_prem:     .asciz "10 FPREM 3: "
t_premcc:   .asciz "FPREM's C3 C1: "
t_com:      .asciz "1 FCOM 2 (C0): "
t_inf:      .asciz "+inf FCOM -inf, ZE: "
t_xam:      .asciz "FXAM -0: "
t_save:     .asciz "FSAVE, FRSTOR: "
t_q:        .asciz "FILD/FISTP qword, top: "
t_q2:       .asciz "FILD/FISTP qword, bottom: "
t_t:        .asciz "FLD/FSTP tbyte, bottom: "
t_t2:       .asciz "FLD/FSTP tbyte, exponent: "
t_ovf:      .asciz "40000 to 16 bits: "
t_ovfie:    .asciz "and IE: "
t_int:      .asciz "unmasked zero divide, INT 2s: "
t_end:      .asciz "control word "
t_bads:     .asciz ", BADs: "
s_ok:       .asciz " OK"
s_bad:      .asciz " BAD"
