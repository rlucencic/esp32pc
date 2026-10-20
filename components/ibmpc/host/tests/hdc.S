/* The AT's hard disk controller (1F0h-1F7h, IRQ 14) against the BIOS's
 * INT 13h on the same disk (C:): reset, IDENTIFY's geometry, a sector read
 * by CHS, one written by LBA and read back through INT 13h, READ MULTIPLE,
 * a read past the end (IDNF). Each line: what ran, the values, OK or BAD.
 */
#include "boot.h"

#define BUF1    0x9000
#define BUF2    0xA000

main:
    cli
    movw $A(irq14), 0x76 * 4            /* IRQ 14 (vector 76h) */
    movw $0, 0x76 * 4 + 2
    inb $0xA1, %al
    andb $0xBF, %al
    outb %al, $0xA1
    sti

    /* reset (SRST), then the status and the diagnostic code */
    movw $A(t_reset), %si
    call print
    movw $0x3F6, %dx
    movb $0x04, %al
    outb %al, %dx
    movb $0x00, %al
    outb %al, %dx
    call not_busy
    movw $0x1F7, %dx
    inb %dx, %al
    movb %al, %bl
    movw $0x1F1, %dx
    inb %dx, %al
    movb %al, %bh
    movw %bx, %ax
    call hex16
    cmpw $0x0150, %bx
    call okbad

    /* the BIOS's geometry */
    movb $0x08, %ah
    movb $0x80, %dl
    int $0x13
    movb %dh, A(heads)
    incb A(heads)
    movb %cl, %al
    andb $0x3F, %al
    movb %al, A(spt)

    /* IDENTIFY: heads and sectors the same */
    movw $A(t_ident), %si
    call print
    movb $0xA0, %al
    movw $0x1F6, %dx
    outb %al, %dx
    movb $0xEC, %al
    call cmd
    movw $BUF1, %di
    call read_words
    movw BUF1 + 6, %ax                  /* word 3: heads */
    call hex16
    movw BUF1 + 12, %ax                 /* word 6: sectors */
    call hex16
    movb BUF1 + 55, %al                 /* the model's first character ("e") */
    xorb %ah, %ah
    call hex16
    movw BUF1 + 6, %ax
    cmpb A(heads), %al
    jne 1f
    movw BUF1 + 12, %ax
    cmpb A(spt), %al
    jne 1f
    cmpb $'e', BUF1 + 55
1:  call okbad

    /* sector 1 of cylinder 0, head 0 (CHS), against INT 13h's */
    movw $A(t_read), %si
    call print
    movw $0x0201, %ax
    movw $0x0001, %cx
    movw $0x0080, %dx
    movw $BUF2, %bx
    int $0x13
    call chs_0_0_1
    movb $1, %al
    movw $0x1F2, %dx
    outb %al, %dx
    movb $0x20, %al
    call cmd
    movw $BUF1, %di
    call read_words
    movw BUF1 + 510, %ax
    call hex16
    movw $BUF1, %si
    movw $BUF2, %di
    movw $256, %cx
    call same
    call okbad

    /* the last sector written by LBA, read back through INT 13h */
    movw $A(t_write), %si
    call print
    movw $BUF1, %di                     /* 00 01 02 ... */
    xorb %al, %al
    movw $512, %cx
2:  movb %al, (%di)
    incw %di
    incb %al
    loop 2b
    movb $0x00, %ah                     /* the last: cylinder count - 1 from AH=08h */
    movb $0x08, %ah
    movb $0x80, %dl
    int $0x13
    movw %cx, A(c08)                    /* (CX, DH as AH=08h gives them) */
    movb %dh, A(c08) + 2
    /* LBA = (cyl * heads + head) * spt + sector - 1 of that last sector */
    movb %ch, %al                       /* cylinder: CH, CL's top two bits */
    movb %cl, %ah
    movb $6, %cl
    shrb %cl, %ah
    movw %ax, A(cyl)
    movb A(heads), %bl
    xorb %bh, %bh
    mulw %bx                            /* DX:AX = cyl * heads */
    movb A(c08) + 2, %bl                /* + last head */
    addw %bx, %ax
    adcw $0, %dx
    movb A(spt), %bl
    movw %dx, %si
    mulw %bx                            /* low word * spt */
    pushw %ax
    pushw %dx
    movw %si, %ax
    mulw %bx                            /* high word * spt */
    popw %dx
    addw %ax, %dx
    popw %ax
    xorb %bh, %bh
    movb A(spt), %bl
    decw %bx
    addw %bx, %ax                       /* + sector - 1 */
    adcw $0, %dx
    movw %ax, A(lba)
    movw %dx, A(lba) + 2
    movw $0x1F3, %dx                    /* LBA 0-7, 8-15, 16-23, 24-27 */
    movb A(lba), %al
    outb %al, %dx
    incw %dx
    movb A(lba) + 1, %al
    outb %al, %dx
    incw %dx
    movb A(lba) + 2, %al
    outb %al, %dx
    incw %dx
    movb A(lba) + 3, %al
    andb $0x0F, %al
    orb $0xE0, %al
    outb %al, %dx
    movb $1, %al
    movw $0x1F2, %dx
    outb %al, %dx
    movb $0x30, %al
    movw $0x1F7, %dx
    movb $0, A(irq)
    outb %al, %dx
    call drq
    movw $BUF1, %si
    movw $0x1F0, %dx
    movw $256, %cx
    cld
    rep outsw
    call wait_irq
    movw $0x1F7, %dx
    inb %dx, %al
    xorb %ah, %ah
    call hex16
    movw A(c08), %cx                    /* INT 13h: the same sector */
    movb A(c08) + 2, %dh
    movb $0x80, %dl
    andb $0xC0, %cl
    orb A(spt), %cl
    movw $0x0201, %ax
    movw $BUF2, %bx
    int $0x13
    movw $BUF1, %si
    movw $BUF2, %di
    movw $256, %cx
    call same
    call okbad

    /* READ MULTIPLE: 4 sectors from CHS 0/0/1 in one block */
    movw $A(t_multi), %si
    call print
    movb $4, %al
    movw $0x1F2, %dx
    outb %al, %dx
    movb $0xA0, %al
    movw $0x1F6, %dx
    outb %al, %dx
    movb $0xC6, %al                     /* SET MULTIPLE 4 */
    call cmd
    movw $0x1F7, %dx
    inb %dx, %al
    xorb %ah, %ah
    call hex16
    call chs_0_0_1
    movb $4, %al
    movw $0x1F2, %dx
    outb %al, %dx
    movb $0xC4, %al
    call cmd
    movw $BUF1, %di
    movw $1024, %cx                     /* (one interrupt: the whole block) */
    movw $0x1F0, %dx
    rep insw
    movw $0x0204, %ax
    movw $0x0001, %cx
    movw $0x0080, %dx
    movw $BUF2, %bx
    int $0x13
    movw $BUF1, %si
    movw $BUF2, %di
    movw $1024, %cx
    call same
    call okbad

    /* past the end: an error, IDNF */
    movw $A(t_end), %si
    call print
    movw $0x1F3, %dx
    movb $0xFF, %al
    outb %al, %dx
    incw %dx
    outb %al, %dx
    incw %dx
    outb %al, %dx
    incw %dx
    movb $0xEF, %al
    outb %al, %dx
    movb $1, %al
    movw $0x1F2, %dx
    outb %al, %dx
    movb $0x20, %al
    call cmd
    movw $0x1F7, %dx
    inb %dx, %al
    movb %al, %bl
    movw $0x1F1, %dx
    inb %dx, %al
    movb %al, %bh
    movw %bx, %ax
    call hex16
    cmpw $0x1051, %bx
    call okbad
9:  hlt
    jmp 9b

/* ---- Helpers ---- */

irq14:
    pushw %ax
    movb $1, %cs:A(irq)
    movb $0x20, %al
    outb %al, $0xA0
    outb %al, $0x20
    popw %ax
    iret

/* a command (AL) to 1F7h: back after its interrupt */
cmd:
    pushw %dx
    movb $0, A(irq)
    movw $0x1F7, %dx
    outb %al, %dx
    call wait_irq
    popw %dx
    ret

wait_irq:
    sti
1:  cmpb $0, A(irq)
    je 1b
    ret

not_busy:
    pushw %ax
    pushw %dx
    movw $0x3F6, %dx
1:  inb %dx, %al
    testb $0x80, %al
    jnz 1b
    popw %dx
    popw %ax
    ret

drq:
    pushw %ax
    pushw %dx
    movw $0x3F6, %dx
1:  inb %dx, %al
    testb $0x08, %al
    jz 1b
    popw %dx
    popw %ax
    ret

/* 256 words from the data port to DI */
read_words:
    pushw %cx
    pushw %dx
    movw $256, %cx
    movw $0x1F0, %dx
    cld
    rep insw
    popw %dx
    popw %cx
    ret

/* the task file at CHS 0/0/1, drive 0 */
chs_0_0_1:
    pushw %ax
    pushw %dx
    movb $1, %al
    movw $0x1F3, %dx
    outb %al, %dx
    xorb %al, %al
    incw %dx
    outb %al, %dx
    incw %dx
    outb %al, %dx
    movb $0xA0, %al
    incw %dx
    outb %al, %dx
    popw %dx
    popw %ax
    ret

/* CX words at SI and DI the same: ZF */
same:
    pushw %si
    pushw %di
    pushw %cx
    cld
    repe cmpsw
    popw %cx
    popw %di
    popw %si
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

irq:    .byte 0
heads:  .byte 0
spt:    .byte 0
c08:    .word 0, 0
cyl:    .word 0
lba:    .word 0, 0
t_reset: .asciz "reset: status, error (50 01): "
t_ident: .asciz "IDENTIFY heads, sectors, model: "
t_read: .asciz "read CHS 0/0/1 (55AA?): "
t_write: .asciz "write the last sector by LBA: "
t_multi: .asciz "SET MULTIPLE 4, READ MULTIPLE: "
t_end:  .asciz "read past the end (51 10): "
t_ok:   .asciz " OK"
t_bad:  .asciz " BAD"
