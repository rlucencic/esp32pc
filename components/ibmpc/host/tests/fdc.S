/* The floppy controller and DMA as programs drive them (on a 1.44 MB
 * floppy, this test's own): reset through the DOR and its four interrupts,
 * SPECIFY, RECALIBRATE, a DMA read of sectors 1-2 compared with what the
 * BIOS loaded, a multi-track read across to head 1, a DMA write to the
 * last sector read back through INT 13h, the DMA's terminal count, the
 * AT's change line. Each line: what ran, the results, OK or BAD.
 */
#include "boot.h"

#define BUF     0x9000                  /* DMA buffer (below 64 KB: page 0) */

main:
    cli
    movw $A(irq6), 0x38                 /* IRQ 6's vector (interrupt 0Eh) */
    movw $0, 0x3A
    inb $0x21, %al
    andb $0xBF, %al
    outb %al, $0x21
    sti

    movw $0x3F7, %dx                    /* the change line (AT): set, a disk went in */
    inb %dx, %al
    movb %al, A(chg0)

    /* reset: four interrupts, one SENSE INTERRUPT STATUS each */
    movb $0, A(irq_seen)
    movw $0x3F2, %dx
    movb $0x00, %al
    outb %al, %dx
    movb $0x1C, %al                     /* motor A, DMA and interrupts, drive 0 */
    outb %al, %dx
    call wait_irq
    movw $A(t_reset), %si
    call print
    movw $4, %cx
1:  movb $0x08, %al
    call cmd
    call result
    movb A(res), %al
    call hex8
    loop 1b
    cmpb $0xC3, A(res)                     /* the last: unit 3, ready changed */
    call okbad

    movb $0x03, %al                     /* SPECIFY: DMA */
    call cmd
    movb $0xDF, %al
    call cmd
    movb $0x02, %al
    call cmd

    movw $A(t_recal), %si
    call print
    movb $0x07, %al                     /* RECALIBRATE drive 0 */
    call cmd
    movb $0x00, %al
    call cmd
    call wait_irq
    movb $0x08, %al
    call cmd
    call result
    movb A(res), %al
    call hex8
    movb A(res) + 1, %al
    call hex8
    cmpb $0x20, A(res)
    call okbad

    movw $0x3F7, %dx                    /* the step cleared the change line */
    inb %dx, %al
    movb %al, A(chg1)

    /* sectors 1-2 of cylinder 0, head 0: the boot sector and the next */
    movw $A(t_read), %si
    call print
    movw $1023, %cx
    movb $0x46, %al                     /* single, increment, to memory, channel 2 */
    call dma
    movb $0, %ah                        /* head 0 */
    movb $1, %bl                        /* sector 1 */
    movb $0x66, %al                     /* READ DATA, MFM, skip */
    call rw
    call show
    movw $ORG, %si
    movw $BUF, %di
    movw $512, %cx
    call same
    jne 2f
    movw $(ORG + 512), %si
    movw $(BUF + 512), %di
    movw $512, %cx
    call same
2:  call okbad

    movw $A(t_tc), %si                  /* channel 2's terminal count in the status */
    call print
    inb $0x08, %al
    call hex8
    andb $0x04, %al
    cmpb $0x04, %al
    call okbad

    /* multi-track: sector 18 of head 0, then sector 1 of head 1 */
    movw $A(t_mt), %si
    call print
    movw $1023, %cx
    movb $0x46, %al
    call dma
    movb $0, %ah
    movb $18, %bl
    movb $0xE6, %al                     /* READ DATA, MT */
    call rw
    call show
    movw $(A(sector2) + 16 * 512), %si  /* the loader read sectors 2-18, then head 1 */
    movw $BUF, %di
    movw $1024, %cx
    call same
    call okbad

    /* write the last sector (cylinder 79, head 1, sector 18) by DMA */
    movw $A(t_write), %si
    call print
    movw $BUF, %di                      /* a pattern: 00 01 02 ... */
    xorb %al, %al
    movw $512, %cx
3:  movb %al, (%di)
    incw %di
    incb %al
    loop 3b
    movb $0x0F, %al                     /* SEEK to 79 */
    call cmd
    movb $0x04, %al                     /* head 1, drive 0 */
    call cmd
    movb $79, %al
    call cmd
    call wait_irq
    movb $0x08, %al
    call cmd
    call result
    movw $511, %cx
    movb $0x4A, %al                     /* single, increment, from memory, channel 2 */
    call dma
    movb $1, %ah
    movb $18, %bl
    movb $0x45, %al                     /* WRITE DATA, MFM */
    movb $79, A(cyl)
    call rw
    call show
    cmpb $0x04, A(res)                     /* ST0: head 1, drive 0, normal */
    jne 4f
    movw $0x0201, %ax                   /* and through the BIOS */
    movw $0x4F12, %cx
    movw $0x0100, %dx
    movw $0xA000, %bx
    int $0x13
    movw $BUF, %si
    movw $0xA000, %di
    movw $512, %cx
    call same
4:  call okbad

    movw $A(t_chg), %si                 /* the change line before and after the step */
    call print
    movb A(chg0), %al
    call hex8
    movb A(chg1), %al
    call hex8
    call newline
5:  hlt
    jmp 5b

/* ---- Helpers ---- */

irq6:
    pushw %ax
    movb $1, %cs:A(irq_seen)
    movb $0x20, %al
    outb %al, $0x20
    popw %ax
    iret

wait_irq:
    sti
1:  cmpb $0, A(irq_seen)
    je 1b
    movb $0, A(irq_seen)
    ret

/* a command or parameter byte: when the controller wants it */
cmd:
    pushw %dx
    pushw %ax
    movw $0x3F4, %dx
1:  inb %dx, %al
    andb $0xC0, %al
    cmpb $0x80, %al
    jne 1b
    popw %ax
    incw %dx
    outb %al, %dx
    popw %dx
    ret

/* the result bytes into res (up to 7) */
result:
    pushw %ax
    pushw %dx
    pushw %di
    movw $A(res), %di
1:  movw $0x3F4, %dx
2:  inb %dx, %al
    testb $0x80, %al
    jz 2b
    testb $0x40, %al
    jz 3f
    incw %dx
    inb %dx, %al
    movb %al, (%di)
    incw %di
    jmp 1b
3:  popw %di
    popw %dx
    popw %ax
    ret

/* DMA channel 2: AL the mode, CX the count, the buffer at BUF */
dma:
    pushw %ax
    movb $0x06, %al                     /* mask channel 2 */
    outb %al, $0x0A
    outb %al, $0x0C                     /* the flip-flop */
    popw %ax
    outb %al, $0x0B
    movb $(BUF & 0xFF), %al
    outb %al, $0x04
    movb $(BUF >> 8), %al
    outb %al, $0x04
    movb $0, %al
    outb %al, $0x81                     /* page 0 */
    movb %cl, %al
    outb %al, $0x05
    movb %ch, %al
    outb %al, $0x05
    movb $0x02, %al                     /* unmask */
    outb %al, $0x0A
    ret

/* a READ or WRITE DATA: AL the command, AH the head, BL the sector, cyl */
rw:
    pushw %ax
    call cmd
    movb %ah, %al
    shlb $1, %al
    shlb $1, %al                        /* head << 2 | drive 0 */
    call cmd
    movb A(cyl), %al
    call cmd
    popw %ax
    movb %ah, %al
    call cmd                            /* H */
    movb %bl, %al
    call cmd                            /* R */
    movb $2, %al
    call cmd                            /* N: 512 */
    movb $18, %al
    call cmd                            /* EOT */
    movb $0x1B, %al
    call cmd                            /* GPL */
    movb $0xFF, %al
    call cmd                            /* DTL */
    call wait_irq
    call result
    ret

/* the seven result bytes */
show:
    pushw %ax
    pushw %si
    pushw %cx
    movw $A(res), %si
    movw $7, %cx
1:  lodsb
    call hex8
    loop 1b
    popw %cx
    popw %si
    popw %ax
    ret

/* compare CX bytes at SI and DI (DS = ES = 0): ZF */
same:
    pushw %si
    pushw %di
    pushw %cx
    cld
    repe cmpsb
    popw %cx
    popw %di
    popw %si
    ret

/* " OK" if ZF, else " BAD", and a new line */
okbad:
    pushw %si
    movw $A(t_ok), %si
    je 1f
    movw $A(t_bad), %si
1:  call print
    call newline
    popw %si
    ret

hex8:
    pushw %ax
    pushw %cx
    movb %al, %ah
    movb $0, %al
    pushw %ax
    movb $' ', %al
    movb $0x0E, %ah
    pushw %bx
    movw $7, %bx
    int $0x10
    popw %bx
    popw %ax
    movb $4, %cl
    movb %ah, %al
    shrb %cl, %al
    call nibble
    movb %ah, %al
    andb $0x0F, %al
    call nibble
    popw %cx
    popw %ax
    ret

nibble:
    pushw %ax
    pushw %bx
    addb $'0', %al
    cmpb $'9', %al
    jbe 1f
    addb $7, %al
1:  movb $0x0E, %ah
    movw $7, %bx
    int $0x10
    popw %bx
    popw %ax
    ret

irq_seen: .byte 0
cyl:    .byte 0
chg0:   .byte 0
chg1:   .byte 0
res:    .fill 8, 1, 0
t_reset: .asciz "reset, sense x4:"
t_recal: .asciz "recalibrate:"
t_read: .asciz "read 0/0/1-2:"
t_tc:   .asciz "DMA status:"
t_mt:   .asciz "read 0/0/18 MT:"
t_write: .asciz "write 79/1/18:"
t_chg:  .asciz "change line before, after:"
t_ok:   .asciz " OK"
t_bad:  .asciz " BAD"
