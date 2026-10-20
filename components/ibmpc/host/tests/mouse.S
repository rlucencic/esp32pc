/* COM1 and its mouse as a driver sees them: the BIOS's port address and
 * equipment word, the UART's loopback, then 1200 baud 7N1, DTR and RTS on
 * (the mouse's 'M'), and its packets through IRQ 4, each line: the three
 * bytes, then X, Y and the buttons (L, R) decoded.
 */
#include "boot.h"

main:
    movw $A(t_bios), %si                /* 40:00 and the equipment word's serial ports */
    call print
    movw 0x400, %ax
    call hex16
    call space
    movw 0x410, %ax
    movb $9, %cl
    shrw %cl, %ax
    andw $7, %ax
    call hex16
    call newline

    movw $A(t_loop), %si                /* loopback: a byte back */
    call print
    movw $0x3FC, %dx
    movb $0x10, %al
    outb %al, %dx
    movw $0x3F8, %dx
    movb $0x5A, %al
    outb %al, %dx
    movw $0x3FD, %dx
    inb %dx, %al
    xorb %ah, %ah
    call hex16
    call space
    movw $0x3F8, %dx
    inb %dx, %al
    xorb %ah, %ah
    call hex16
    call newline

    cli
    movw $A(irq4), 0x0C * 4
    movw $0, 0x0C * 4 + 2
    movw $0x3FB, %dx                    /* 1200 baud, 7N1 */
    movb $0x80, %al
    outb %al, %dx
    movw $0x3F8, %dx
    movb $0x60, %al
    outb %al, %dx
    incw %dx
    movb $0x00, %al
    outb %al, %dx
    movw $0x3FB, %dx
    movb $0x02, %al
    outb %al, %dx
    movw $0x3FC, %dx                    /* the mouse off, then on */
    movb $0x00, %al
    outb %al, %dx
    movw $0x3F8, %dx
    inb %dx, %al
    movw $0x3F9, %dx                    /* the received data's interrupt */
    movb $0x01, %al
    outb %al, %dx
    inb $0x21, %al
    andb $0xEF, %al
    outb %al, $0x21
    movw $0x3FC, %dx
    movb $0x0B, %al                     /* DTR, RTS, OUT2 */
    outb %al, %dx
    sti

    movw $A(t_id), %si                  /* its identification */
    call print
    call get
    xorb %ah, %ah
    call hex16
    call newline

1:  call get                            /* packets: the first byte has bit 6 */
    testb $0x40, %al
    jz 1b
    movb %al, A(pkt)
    call get
    movb %al, A(pkt) + 1
    call get
    movb %al, A(pkt) + 2
    movw $A(pkt), %si
    movw $3, %cx
2:  lodsb
    xorb %ah, %ah
    call hex16
    call space
    loop 2b
    movb A(pkt), %al                    /* X: bits 1-0 of the first, then the second's 6 */
    movb $6, %cl
    shlb %cl, %al
    orb A(pkt) + 1, %al
    cbw
    call hex16
    call space
    movb A(pkt), %al                    /* Y: bits 3-2 */
    shlb $1, %al
    shlb $1, %al
    shlb $1, %al
    shlb $1, %al
    andb $0xC0, %al
    orb A(pkt) + 2, %al
    cbw
    call hex16
    call space
    movb A(pkt), %al                    /* L, R */
    movb $4, %cl
    shrb %cl, %al
    andb $3, %al
    xorb %ah, %ah
    call hex16
    call newline
    jmp 1b

/* the next byte from the interrupt's ring */
get:
    sti
1:  movb A(tail), %bl
    cmpb A(head), %bl
    je 1b
    xorb %bh, %bh
    movb A(ring)(%bx), %al
    incb %bl
    andb $15, %bl
    movb %bl, A(tail)
    ret

irq4:
    pushw %ax
    pushw %bx
    pushw %dx
    movw $0x3F8, %dx
    inb %dx, %al
    movb %cs:A(head), %bl
    xorb %bh, %bh
    movb %al, %cs:A(ring)(%bx)
    incb %bl
    andb $15, %bl
    movb %bl, %cs:A(head)
    movb $0x20, %al
    outb %al, $0x20
    popw %dx
    popw %bx
    popw %ax
    iret

space:
    pushw %ax
    pushw %bx
    movw $0x0E20, %ax
    movw $7, %bx
    int $0x10
    popw %bx
    popw %ax
    ret

head:   .byte 0
tail:   .byte 0
ring:   .fill 16, 1, 0
pkt:    .fill 3, 1, 0
t_bios: .asciz "COM1, serial ports: "
t_loop: .asciz "loopback: LSR, byte: "
t_id:   .asciz "mouse: "
