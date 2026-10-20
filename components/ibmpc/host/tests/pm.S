/* The 286's protected mode (the AT): into it, faults with their error
 * codes, ring 3, gates, tasks, a double fault, a triple fault back to real
 * mode (the CMOS shutdown code 0Ah), then LOADALL's "unreal" segments.
 * Each result line: what ran, the values, OK or BAD.
 */
#include "boot.h"

#define CODE0   0x08
#define DATA0   0x10
#define SCR     0x18            /* the scratch area at 20000h */
#define EXT     0x20            /* extended memory: 100000h */
#define SMALL   0x28            /* 20000h, limit FFh */
#define CODE3   0x30
#define DATA3   0x38
#define GATE3   0x40            /* a call gate from ring 3: one parameter */
#define TSSA    0x48
#define TSSB    0x50
#define LDTD    0x58
#define DOWN    0x60            /* expand-down: 1000h-FFFFh */
#define RDONLY  0x68
#define NOTP    0x70
#define GATEB   0x78            /* back to ring 0 */

#define DESC(limit, base_lo, base_hi, access) .word (limit), (base_lo); .byte (base_hi), (access); .word 0
#define GATE(off, sel, count, access) .word (off), (sel); .byte (count), (access); .word 0

/* A test whose instruction may fault: the handler skips it */
#define TRY(n)  movw $(n##_e - n##_s), A(skip); movw $0xFFFF, A(last_vec); movw $0xFFFF, A(last_err); n##_s:
#define DONE(n, slot) n##_e: movw A(last_vec), %ax; movw %ax, A(slot); movw A(last_err), %ax; movw %ax, A(slot) + 2

main:
    movw $A(t_title), %si
    call print
    call newline
    movw %sp, A(saved_sp)
    movw $0x5678, 0x600                 /* (real mode: linear 600h) */
    call kbc_wait                       /* the A20 gate open */
    movb $0xD1, %al
    outb %al, $0x64
    call kbc_wait
    movb $0xDF, %al
    outb %al, $0x60
    call kbc_wait
    cli
    lgdt A(gdtr)
    lidt A(idtr)
    smsw %ax
    orw $1, %ax
    lmsw %ax
    ljmp $CODE0, $A(pm)

kbc_wait:
    inb $0x64, %al
    testb $2, %al
    jnz kbc_wait
    ret

/* ---- Protected mode, ring 0 ---- */

pm:
    movw $DATA0, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $A(stack0_top), %sp
    movw $1, A(r_pm)

    movw $EXT, %ax                      /* extended memory, not 600h again */
    movw %ax, %es
    movw $0x1234, %es:0x600
    movw %es:0x600, %ax
    movw %ax, A(r_ext)
    movw 0x600, %ax
    movw %ax, A(r_ext) + 2

    movw $SMALL, %ax                    /* a word across the limit */
    movw %ax, %es
    movb %es:0xFF, %al
    TRY(t_lim) movw %es:0xFF, %ax
    DONE(t_lim, r_lim)

    movw $NOTP, %ax                     /* not present */
    TRY(t_np) movw %ax, %es
    DONE(t_np, r_np)

    movw $0x1000, %ax                   /* beyond the GDT */
    TRY(t_gdt) movw %ax, %es
    DONE(t_gdt, r_gdt)

    movw $RDONLY, %ax                   /* written */
    movw %ax, %es
    TRY(t_ro) movw %ax, %es:0x600
    DONE(t_ro, r_ro)

    movw $DOWN, %ax                     /* expand-down: 0FFFh no, 1000h yes */
    movw %ax, %es
    TRY(t_dn1) movb %es:0x0FFF, %al
    DONE(t_dn1, r_dn1)
    TRY(t_dn2) movb %es:0x1000, %al
    DONE(t_dn2, r_dn2)

    movw $LDTD, %ax                     /* the LDT's segment 0Ch (20100h) */
    lldt %ax
    sldt %ax
    movw %ax, A(r_ldt)
    movw $0x0C, %ax
    movw %ax, %es
    movw $0xABCD, %es:0x10
    movw $SCR, %ax
    movw %ax, %es
    movw %es:0x110, %ax
    movw %ax, A(r_ldt) + 2

    movw $CODE0, %bx                    /* LAR, LSL, VERR, VERW */
    lar %bx, %ax
    movw %ax, A(r_lar)
    movw $SMALL, %bx
    lsl %bx, %ax
    movw %ax, A(r_lar) + 2
    movw $RDONLY, %bx
    verw %bx
    pushf
    popw %ax
    andw $0x40, %ax
    movw %ax, A(r_ver)
    verr %bx
    pushf
    popw %ax
    andw $0x40, %ax
    movw %ax, A(r_ver) + 2

    movw $TSSA, %ax                     /* the task register */
    ltr %ax
    str %ax
    movw %ax, A(r_str)

    /* ---- Ring 3 ---- */
    movw %sp, A(saved_sp0)
    pushw $(DATA3 | 3)
    pushw $A(stack3_top)
    pushf
    pushw $(CODE3 | 3)
    pushw $A(ring3)
    iret

ring3:
    movw $(DATA3 | 3), %ax
    movw %ax, %ds
    movw %ax, %es
    movw %cs, %ax
    andw $3, %ax
    movw %ax, A(r_cpl3)
    TRY(t_cli) cli
    DONE(t_cli, r_cli)
    TRY(t_in) inb $0x60, %al
    DONE(t_in, r_in)
    int $0x30                           /* a trap gate for ring 3 */
    TRY(t_i32) int $0x32                /* an interrupt gate for ring 0 only */
    DONE(t_i32, r_i32)
    pushw $0x4242                       /* a call gate to ring 0, a parameter */
    lcall $(GATE3 | 3), $0
    movw %sp, A(r_sp3)
    movw $DATA0, %ax                    /* ring 0's data: not from here */
    TRY(t_ds0) movw %ax, %es
    DONE(t_ds0, r_ds0)
    lcall $(GATEB | 3), $0              /* back to ring 0 */

int30:
    pushw %ax
    movw %cs, %ax
    andw $3, %ax
    movw %ax, %ss:A(r_int30)
    incw %ss:A(r_int30) + 2
    popw %ax
    iret

gate_target:                            /* IP, CS, the parameter, SP, SS */
    pushw %bp
    movw %sp, %bp
    movw 6(%bp), %ax
    movw %ax, %ss:A(r_gate)
    movw %cs, %ax
    andw $3, %ax
    movw %ax, %ss:A(r_gate) + 2
    popw %bp
    lret $2

back0:
    movw $DATA0, %ax
    movw %ax, %ds
    movw %ax, %es
    movw A(saved_sp0), %sp

    /* ---- Tasks ---- */
    ljmp $TSSB, $0                      /* B, back by its JMP */
    lcall $TSSB, $0                     /* B nested, back by its IRET */
    int $0x31                           /* B through a task gate */
    movw A(tss_b), %ax                  /* its back link */
    movw %ax, A(r_link)

    /* ---- A double fault: #GP's gate not present ---- */
    andb $0x7F, A(idt) + 13 * 8 + 5
    movw $0x1000, %ax
    TRY(t_df) movw %ax, %es
    DONE(t_df, r_df)
    orb $0x80, A(idt) + 13 * 8 + 5

    /* ---- Back to real mode: a triple fault, the shutdown code 0Ah ---- */
    movb $0x0F, %al
    outb %al, $0x70
    movb $0x0A, %al
    outb %al, $0x71
    movw $A(rm_back), 0x467
    movw $0, 0x469
    lidt A(idtr0)
    int $3
    hlt

task_b:
    movw %ax, A(r_tb)                   /* AX from its TSS */
    pushf
    popw %ax
    movw %ax, A(r_tbf)                  /* NT clear: a JMP */
    ljmp $TSSA, $0
    pushf                               /* by A's CALL: NT set */
    popw %ax
    movw %ax, A(r_tbf) + 2
    movw A(tss_b), %ax
    movw %ax, A(r_tb) + 2               /* the back link: A */
    iret
    pushf                               /* by INT 31h */
    popw %ax
    movw %ax, A(r_tbf) + 4
    iret

/* ---- Exceptions: the vector, the error code, IP; past the instruction ---- */

#define EXC(n)  e##n: pushw %ax; movb $n, %al; jmp exc_plain
#define EXCE(n) e##n: pushw %ax; movb $n, %al; jmp exc_err
EXC(0)
EXC(1)
EXC(2)
EXC(3)
EXC(4)
EXC(5)
EXC(6)
EXC(7)
EXCE(8)
EXC(9)
EXCE(10)
EXCE(11)
EXCE(12)
EXCE(13)
EXC(14)
EXC(15)
EXC(16)
EXC(17)

exc_plain:
    pushw %bp
    movw %sp, %bp
    movb %al, %ss:A(last_vec)
    movb $0, %ss:A(last_vec) + 1
    movw $0xEEEE, %ss:A(last_err)
    movw 4(%bp), %ax
    addw %ss:A(skip), %ax
    movw %ax, 4(%bp)
    popw %bp
    popw %ax
    iret

exc_err:
    pushw %bp
    movw %sp, %bp
    movb %al, %ss:A(last_vec)
    movb $0, %ss:A(last_vec) + 1
    movw 4(%bp), %ax
    movw %ax, %ss:A(last_err)
    movw 6(%bp), %ax
    addw %ss:A(skip), %ax
    movw %ax, 6(%bp)
    popw %bp
    popw %ax
    addw $2, %sp
    iret

/* ---- Real mode again ---- */

rm_back:
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw A(saved_sp), %sp
    sti

    /* LOADALL: ES's base 100000h ("unreal"), the rest as they are */
    movw $0x800, %di
    xorw %ax, %ax
    movw $0x33, %cx
    cld
    rep stosw
    movw $0xFFF0, 0x806                 /* MSW */
    movw $0x0002, 0x818                 /* FLAGS */
    movw $A(after_loadall), 0x81A       /* IP */
    movw $0x1234, 0x824                 /* ES's selector (anything) */
    movw %sp, 0x82C
    movw $0x0000, 0x836                 /* ES: 100000h, 93h, FFFFh */
    movw $0x9310, 0x838
    movw $0xFFFF, 0x83A
    movw $0x9B00, 0x83E                 /* CS: 0, 9Bh, FFFFh */
    movw $0xFFFF, 0x840
    movw $0x9300, 0x844                 /* SS */
    movw $0xFFFF, 0x846
    movw $0x9300, 0x84A                 /* DS */
    movw $0xFFFF, 0x84C
    movw $0xFFFF, 0x852                 /* GDTR's limit */
    movw $0x03FF, 0x85E                 /* IDTR's limit */
    .byte 0x0F, 0x05                    /* LOADALL */
after_loadall:
    movw %es:0x600, %ax
    movw %ax, A(r_unreal)
    movw %es, %ax
    movw %ax, A(r_unreal) + 2
    xorw %ax, %ax                       /* reloaded in real mode: 0 again */
    movw %ax, %es
    movw %es:0x600, %ax
    movw %ax, A(r_unreal) + 4

    /* ---- The results ---- */
    movw $A(checks), %bx
1:  movw (%bx), %si
    orw %si, %si
    jz 9f
    call print
    movw 2(%bx), %di                    /* the value's address */
    movw 4(%bx), %cx                    /* words */
    movw 6(%bx), %dx                    /* the expected values' address */
    movb $1, %ah                        /* (all the same so far) */
    pushw %bx
2:  movw (%di), %bx
    pushw %ax
    movw %bx, %ax
    call space
    call hex16
    popw %ax
    pushw %si
    movw %dx, %si
    cmpw (%si), %bx
    popw %si
    je 3f
    movb $0, %ah
3:  addw $2, %di
    addw $2, %dx
    loop 2b
    popw %bx
    movw $A(t_ok), %si
    orb %ah, %ah
    jnz 4f
    movw $A(t_bad), %si
4:  call print
    call newline
    addw $8, %bx
    jmp 1b
9:  hlt
    jmp 9b

space:
    pushw %ax
    pushw %bx
    movw $0x0E20, %ax
    movw $7, %bx
    int $0x10
    popw %bx
    popw %ax
    ret

/* label, value, words, expected */
#define CHECK(label, value, n, expect) .word A(label), A(value), n, A(expect)
checks:
    CHECK(t_pm, r_pm, 1, x_pm)
    CHECK(t_ext, r_ext, 2, x_ext)
    CHECK(t_lim, r_lim, 2, x_lim)
    CHECK(t_np, r_np, 2, x_np)
    CHECK(t_gdt, r_gdt, 2, x_gdt)
    CHECK(t_ro, r_ro, 2, x_ro)
    CHECK(t_dn, r_dn1, 4, x_dn)
    CHECK(t_ldt, r_ldt, 2, x_ldt)
    CHECK(t_lar, r_lar, 4, x_lar)
    CHECK(t_str, r_str, 2, x_str)
    CHECK(t_cli, r_cli, 4, x_cli)
    CHECK(t_int, r_int30, 4, x_int)
    CHECK(t_gate, r_gate, 3, x_gate)
    CHECK(t_ds0, r_ds0, 2, x_ds0)
    CHECK(t_task, r_tb, 5, x_task)
    CHECK(t_link, r_link, 1, x_link)
    CHECK(t_df, r_df, 2, x_df)
    CHECK(t_unreal, r_unreal, 3, x_unreal)
    .word 0

t_title:  .asciz "286 protected mode"
t_pm:     .asciz "protected mode on:"
t_ext:    .asciz "100600h, 600h:"
t_lim:    .asciz "word at the limit (13, 0):"
t_np:     .asciz "not present (11, 70h):"
t_gdt:    .asciz "beyond the GDT (13, 1000h):"
t_ro:     .asciz "read-only written (13, 0):"
t_dn:     .asciz "expand-down 0FFFh, 1000h:"
t_ldt:    .asciz "SLDT, LDT segment:"
t_lar:    .asciz "LAR, LSL, VERW, VERR:"
t_str:    .asciz "STR, CPL in ring 3:"
t_cli:    .asciz "ring 3: CLI, IN (13, 0):"
t_int:    .asciz "INT 30h's CPL, count, INT 32h:"
t_gate:   .asciz "call gate: parameter, CPL, SP:"
t_ds0:    .asciz "ring 3 loads ring 0 data:"
t_task:   .asciz "task B: AX, link, flags x3:"
t_link:   .asciz "B's back link after INT 31h:"
t_df:     .asciz "double fault (8, 0):"
t_unreal: .asciz "LOADALL: ES:600, ES, real again:"
t_ok:     .asciz " OK"
t_bad:    .asciz " BAD"

/* (the expected values, in the results' order) */
x_pm:     .word 1
x_ext:    .word 0x1234, 0x5678
x_lim:    .word 13, 0
x_np:     .word 11, 0x70
x_gdt:    .word 13, 0x1000
x_ro:     .word 13, 0
x_dn:     .word 13, 0, 0xFFFF, 0xFFFF
x_ldt:    .word LDTD, 0xABCD
x_lar:    .word 0x9B00, 0x00FF, 0x0000, 0x0040
x_str:    .word TSSA, 3
x_cli:    .word 13, 0, 13, 0
x_int:    .word 0, 1, 13, 0x32 * 8 + 2
x_gate:   .word 0x4242, 0, A(stack3_top)
x_ds0:    .word 13, DATA0
x_task:   .word 0xB0B0, TSSA, 0x0002, 0x4002, 0x4002
x_link:   .word TSSA
x_df:     .word 8, 0
x_unreal: .word 0x1234, 0x1234, 0x5678

/* (the results; r_cpl3 after r_str, r_i32 after r_int30, r_sp3 after r_gate) */
r_pm:     .word 0
r_ext:    .word 0, 0
r_lim:    .word 0, 0
r_np:     .word 0, 0
r_gdt:    .word 0, 0
r_ro:     .word 0, 0
r_dn1:    .word 0, 0
r_dn2:    .word 0, 0
r_ldt:    .word 0, 0
r_lar:    .word 0, 0
r_ver:    .word 0, 0
r_str:    .word 0
r_cpl3:   .word 0
r_cli:    .word 0, 0
r_in:     .word 0, 0
r_int30:  .word 0, 0
r_i32:    .word 0, 0
r_gate:   .word 0, 0
r_sp3:    .word 0
r_ds0:    .word 0, 0
r_tb:     .word 0, 0
r_tbf:    .word 0xFFFF, 0xFFFF, 0xFFFF
r_link:   .word 0
r_df:     .word 0, 0
r_unreal: .word 0, 0, 0

skip:     .word 0
last_vec: .word 0
last_err: .word 0
saved_sp: .word 0
saved_sp0: .word 0

    .balign 8
gdt:
    DESC(0, 0, 0, 0)
    DESC(0xFFFF, 0, 0, 0x9A)                    /* CODE0 */
    DESC(0xFFFF, 0, 0, 0x92)                    /* DATA0 */
    DESC(0xFFFF, 0, 0x02, 0x92)                 /* SCR */
    DESC(0xFFFF, 0, 0x10, 0x92)                 /* EXT */
    DESC(0x00FF, 0, 0x02, 0x92)                 /* SMALL */
    DESC(0xFFFF, 0, 0, 0xFA)                    /* CODE3 */
    DESC(0xFFFF, 0, 0, 0xF2)                    /* DATA3 */
    GATE(A(gate_target), CODE0, 1, 0xE4)        /* GATE3 */
    DESC(0x2B, A(tss_a), 0, 0x81)               /* TSSA */
    DESC(0x2B, A(tss_b), 0, 0x81)               /* TSSB */
    DESC(0x0F, A(ldt), 0, 0x82)                 /* LDTD */
    DESC(0x0FFF, 0, 0x02, 0x96)                 /* DOWN */
    DESC(0xFFFF, 0, 0, 0x90)                    /* RDONLY */
    DESC(0xFFFF, 0, 0, 0x12)                    /* NOTP */
    GATE(A(back0), CODE0, 0, 0xE4)              /* GATEB */
gdt_end:

ldt:
    DESC(0, 0, 0, 0)
    DESC(0x00FF, 0x0100, 0x02, 0x92)            /* 0Ch: 20100h */

idt:
    GATE(A(e0), CODE0, 0, 0x86)
    GATE(A(e1), CODE0, 0, 0x86)
    GATE(A(e2), CODE0, 0, 0x86)
    GATE(A(e3), CODE0, 0, 0x86)
    GATE(A(e4), CODE0, 0, 0x86)
    GATE(A(e5), CODE0, 0, 0x86)
    GATE(A(e6), CODE0, 0, 0x86)
    GATE(A(e7), CODE0, 0, 0x86)
    GATE(A(e8), CODE0, 0, 0x86)
    GATE(A(e9), CODE0, 0, 0x86)
    GATE(A(e10), CODE0, 0, 0x86)
    GATE(A(e11), CODE0, 0, 0x86)
    GATE(A(e12), CODE0, 0, 0x86)
    GATE(A(e13), CODE0, 0, 0x86)
    GATE(A(e14), CODE0, 0, 0x86)
    GATE(A(e15), CODE0, 0, 0x86)
    GATE(A(e16), CODE0, 0, 0x86)
    .rept 0x30 - 17
    GATE(A(e17), CODE0, 0, 0x86)
    .endr
    GATE(A(int30), CODE0, 0, 0xE7)              /* 30h: a trap gate, DPL 3 */
    GATE(0, TSSB, 0, 0xE5)                      /* 31h: a task gate, DPL 3 */
    GATE(A(int30), CODE0, 0, 0x86)              /* 32h: DPL 0 */
idt_end:

gdtr:   .word gdt_end - gdt - 1, A(gdt), 0
idtr:   .word idt_end - idt - 1, A(idt), 0
idtr0:  .word 0, 0, 0

tss_a:
    .word 0, A(stack0b_top), DATA0, 0, 0, 0, 0
    .fill 15, 2, 0
tss_b:
    .word 0, A(stack0b_top), DATA0, 0, 0, 0, 0
    .word A(task_b), 0x0002                     /* IP, FLAGS */
    .word 0xB0B0, 0, 0, 0                       /* AX CX DX BX */
    .word A(stackb_top), 0, 0, 0                /* SP BP SI DI */
    .word DATA0, CODE0, DATA0, DATA0            /* ES CS SS DS */
    .word 0                                     /* LDT */

    .balign 2
    .fill 256, 2, 0
stack0_top:
    .fill 128, 2, 0
stack0b_top:
    .fill 128, 2, 0
stack3_top:
    .fill 128, 2, 0
stackb_top:
