/* The AdLib and the Sound Blaster as games find and use them: the OPL2's
 * timer detection, an A (440 Hz) for a second; the DSP's reset (AAh), its
 * version, a single-cycle DMA block of a 1 kHz square wave at 8 kHz with its
 * interrupt (about 9 ticks), auto-init (its interrupts in a second: ~4,
 * then DAh stops it), the forced interrupt (F2h). With PC_WAV the sound
 * shows the 440 Hz, then the 1 kHz.
 */
#include "boot.h"

#define BUF     0x9000                  /* the DMA buffer: 4000 bytes, below 64 KB */

main:
    cli
    movw $A(irq7), 0x0F * 4             /* IRQ 7: vector 0Fh */
    movw $0, 0x0F * 4 + 2
    inb $0x21, %al
    andb $0x7F, %al
    outb %al, $0x21
    sti

    /* ---- the AdLib: its timer 1 ---- */
    movw $A(t_adlib), %si
    call print
    movw $0x0460, %ax                   /* reg 4: both timers masked */
    call opl
    movw $0x0480, %ax                   /* the IRQ reset */
    call opl
    movw $0x388, %dx
    inb %dx, %al
    andb $0xE0, %al
    movb %al, %bl
    movw $0x02FF, %ax                   /* timer 1: 255, one step (80 us) */
    call opl
    movw $0x0421, %ax                   /* start it, timer 2 masked */
    call opl
    movw $200, %cx                      /* (well over 80 us) */
1:  inb %dx, %al
    loop 1b
    inb %dx, %al
    andb $0xE0, %al
    movb %al, %bh
    movw $0x0460, %ax
    call opl
    movw $0x0480, %ax
    call opl
    movw %bx, %ax
    call hex16
    cmpw $0xC000, %bx
    call okbad

    /* ---- an A on channel 0 for a second ---- */
    movw $A(t_note), %si
    call print
    movw $A(note), %si
2:  lodsw
    orw %ax, %ax
    jz 3f
    call opl
    jmp 2b
3:  movw $18, %cx
    call ticks
    movw $0xB012, %ax                   /* key off */
    call opl
    movw $4, %cx
    call ticks
    call newline

    /* ---- the DSP: reset, version ---- */
    movw $A(t_dsp), %si
    call print
    movw $0x226, %dx
    movb $1, %al
    outb %al, %dx
    movw $10, %cx
4:  inb %dx, %al
    loop 4b
    movb $0, %al
    outb %al, %dx
    call dsp_read
    xorb %ah, %ah
    movw %ax, %bx
    call hex16
    movb $0xE1, %al
    call dsp_write
    call dsp_read
    movb %al, %ah
    call dsp_read
    xchgb %al, %ah
    xchgb %al, %ah
    movw %ax, %di
    call space
    call hex16
    cmpw $0x00AA, %bx
    jne 5f
    cmpw $0x0201, %di
5:  call okbad

    /* ---- a 1 kHz square wave at 8 kHz, one DMA block of 4000 bytes ---- */
    movw $BUF, %di
    movw $500, %cx
6:  movw $0xC0C0, %ax
    stosw
    stosw
    movw $0x4040, %ax
    stosw
    stosw
    loop 6b
    movw $A(t_single), %si
    call print
    movb $0x49, %bl                     /* single, increment, memory to the card, channel 1 */
    call dma
    movb $0xD1, %al                     /* the speaker on */
    call dsp_write
    movb $0x40, %al                     /* 8 kHz: 256 - 125 */
    call dsp_write
    movb $131, %al
    call dsp_write
    movb $0, A(irqs)
    movw 0x46C, %ax
    movw %ax, A(t0)
    movb $0x14, %al                     /* 8-bit single-cycle: 4000 bytes */
    call dsp_write
    movb $0x9F, %al
    call dsp_write
    movb $0x0F, %al
    call dsp_write
7:  cmpb $0, A(irqs)                    /* (a timeout: 3 s) */
    jne 8f
    movw 0x46C, %ax
    subw A(t0), %ax
    cmpw $54, %ax
    jb 7b
8:  movw 0x46C, %ax
    subw A(t0), %ax
    call hex16
    cmpb $1, A(irqs)
    call okbad

    /* ---- auto-init: blocks of 2000, a second's interrupts, then DAh ---- */
    movw $A(t_auto), %si
    call print
    movb $0x59, %bl                     /* auto-init DMA */
    call dma
    movb $0x48, %al
    call dsp_write
    movb $0xCF, %al                     /* 1999 */
    call dsp_write
    movb $0x07, %al
    call dsp_write
    movb $0, A(irqs)
    movb $0x1C, %al
    call dsp_write
    movw $18, %cx
    call ticks
    movb $0xDA, %al
    call dsp_write
    movw $9, %cx
    call ticks
    movb A(irqs), %al
    xorb %ah, %ah
    call hex16
    cmpb $4, %al
    jb 9f
    cmpb $6, %al
    ja 9f
    xorb %al, %al                       /* (ZF) */
9:  call okbad

    /* ---- the forced interrupt ---- */
    movw $A(t_force), %si
    call print
    movb $0, A(irqs)
    movb $0xF2, %al
    call dsp_write
    movw $2, %cx
    call ticks
    movb A(irqs), %al
    xorb %ah, %ah
    call hex16
    cmpb $1, %al
    call okbad
10: hlt
    jmp 10b

/* ---- helpers ---- */

irq7:
    pushw %ax
    pushw %dx
    incb %cs:A(irqs)
    movw $0x22E, %dx                    /* the DSP's acknowledge */
    inb %dx, %al
    movb $0x20, %al
    outb %al, $0x20
    popw %dx
    popw %ax
    iret

/* AH: the OPL2's register, AL: the value */
opl:
    pushw %dx
    pushw %ax
    movw $0x388, %dx
    movb %ah, %al
    outb %al, %dx
    popw %ax
    incw %dx
    outb %al, %dx
    popw %dx
    ret

dsp_write:
    pushw %dx
    pushw %ax
    movw $0x22C, %dx
1:  inb %dx, %al
    testb $0x80, %al
    jnz 1b
    popw %ax
    outb %al, %dx
    popw %dx
    ret

dsp_read:
    pushw %dx
    pushw %cx
    movw $0x22E, %dx
    movw $1000, %cx
1:  inb %dx, %al
    testb $0x80, %al
    jnz 2f
    loop 1b
2:  movw $0x22A, %dx
    inb %dx, %al
    popw %cx
    popw %dx
    ret

/* DMA channel 1: BL the mode, 4000 bytes at BUF */
dma:
    movb $0x05, %al                     /* mask channel 1 */
    outb %al, $0x0A
    outb %al, $0x0C
    movb %bl, %al
    outb %al, $0x0B
    movb $(BUF & 0xFF), %al
    outb %al, $0x02
    movb $(BUF >> 8), %al
    outb %al, $0x02
    movb $0, %al
    outb %al, $0x83                     /* page 0 */
    movb $0x9F, %al                     /* 3999 */
    outb %al, $0x03
    movb $0x0F, %al
    outb %al, $0x03
    movb $0x01, %al
    outb %al, $0x0A
    ret

/* CX ticks of the BIOS's clock */
ticks:
    pushw %ax
    movw 0x46C, %ax
    addw %cx, %ax
1:  sti
    hlt
    cmpw 0x46C, %ax
    jne 1b
    popw %ax
    ret

space:
    pushw %ax
    pushw %bx
    movw $0x0E20, %ax
    movw $7, %bx
    int $0x10
    popw %bx
    popw %ax
    ret

okbad:
    pushw %si
    movw $A(t_ok), %si
    je 1f
    movw $A(t_bad), %si
1:  call print
    call newline
    popw %si
    ret

irqs:   .byte 0
t0:     .word 0
/* the A: modulator and carrier of channel 0, F-number 244h in block 4 */
note:   .word 0x2001, 0x4010, 0x60F0, 0x8077, 0x2301, 0x4300, 0x63F0, 0x8377
        .word 0xA044, 0xB032, 0
t_adlib:  .asciz "AdLib timer (00, C0): "
t_note:   .asciz "A 440 Hz for a second"
t_dsp:    .asciz "DSP reset, version (AA 0201): "
t_single: .asciz "single DMA block, ticks (~9): "
t_auto:   .asciz "auto-init IRQs in 1 s (4-6): "
t_force:  .asciz "forced IRQ (1): "
t_ok:     .asciz " OK"
t_bad:    .asciz " BAD"
