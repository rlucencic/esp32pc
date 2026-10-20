/* INT 15h AH=89h, the BIOS's way into protected mode: IBM's GDT layout,
 * the segment registers as it leaves them (CS 30h, DS 18h, ES 20h, SS 28h),
 * AH 0, interrupts off; "PM" written through ES (the CGA's memory), then
 * back by a triple fault (shutdown code 0Ah) and the results printed.
 */
#include "boot.h"

#define DESC(limit, base_lo, base_hi, access) .word (limit), (base_lo); .byte (base_hi), (access); .word 0

main:
    movw $A(t_title), %si
    call print
    call newline
    movw %sp, A(saved_sp)
    movw $A(gdt), %si                   /* ES:SI the GDT */
    movb $0x89, %ah
    movw $0x2028, %bx                   /* IRQ 0-7 at 20h, 8-15 at 28h */
    int $0x15
    movw %cs, A(r_seg)                  /* (CS, DS, ES, SS) */
    movw %ds, A(r_seg) + 2
    movw %es, A(r_seg) + 4
    movw %ss, A(r_seg) + 6
    movb %ah, %al
    xorb %ah, %ah
    movw %ax, A(r_ah)
    pushf
    popw %ax
    andw $0x0200, %ax
    movw %ax, A(r_ah) + 2
    movw $0x0750, %es:1600              /* "PM" on row 10 */
    movw $0x074D, %es:1602
    movb $0x0F, %al                     /* back: a triple fault */
    outb %al, $0x70
    movb $0x0A, %al
    outb %al, $0x71
    movw $A(rm_back), 0x467
    movw $0, 0x469
    lidt A(idtr0)
    int $3
    hlt

rm_back:
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw A(saved_sp), %sp
    movb $0x11, %al                     /* the interrupt controllers back at 08h, 70h */
    outb %al, $0x20
    movb $0x08, %al
    outb %al, $0x21
    movb $0x04, %al
    outb %al, $0x21
    movb $0x01, %al
    outb %al, $0x21
    movb $0x11, %al
    outb %al, $0xA0
    movb $0x70, %al
    outb %al, $0xA1
    movb $0x02, %al
    outb %al, $0xA1
    movb $0x01, %al
    outb %al, $0xA1
    sti
    movw $A(t_seg), %si
    call print
    movw $A(r_seg), %di
    movw $A(x_seg), %bx
    movw $6, %cx
    movb $1, %dl
1:  movw (%di), %ax
    call space
    call hex16
    cmpw (%bx), %ax
    je 2f
    movb $0, %dl
2:  addw $2, %di
    addw $2, %bx
    loop 1b
    movw $A(t_ok), %si
    orb %dl, %dl
    jnz 3f
    movw $A(t_bad), %si
3:  call print
    call newline
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

t_title: .asciz "INT 15h AH=89h"
t_seg:   .asciz "CS DS ES SS, AH, IF:"
t_ok:    .asciz " OK"
t_bad:   .asciz " BAD"
x_seg:   .word 0x30, 0x18, 0x20, 0x28, 0, 0
r_seg:   .word 0, 0, 0, 0
r_ah:    .word 0xFFFF, 0xFFFF
saved_sp: .word 0
idtr0:   .word 0, 0, 0

    .balign 8
gdt:
    DESC(0, 0, 0, 0)
    DESC(gdt_end - gdt - 1, A(gdt), 0, 0x92)    /* 08: the GDT */
    DESC(0, 0, 0, 0x92)                         /* 10: the IDT (none) */
    DESC(0xFFFF, 0, 0, 0x92)                    /* 18: DS */
    DESC(0x0FFF, 0x8000, 0x0B, 0x92)            /* 20: ES, the CGA's memory */
    DESC(0xFFFF, 0, 0, 0x92)                    /* 28: SS */
    DESC(0xFFFF, 0, 0, 0x9A)                    /* 30: CS */
    DESC(0, 0, 0, 0)                            /* 38: the BIOS's */
gdt_end:
