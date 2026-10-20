/* CGA mid-frame palette change: mode 4 in vertical stripes of the four
 * colours; each frame, from the vertical retrace's end, 100 lines counted
 * through 3DA bit 0 (display enable), then the other palette (3D9 bit 5),
 * and back at the next retrace: the top half in green, red, brown, the
 * bottom in cyan, magenta, white.
 */
#include "boot.h"

main:
    movw $0x0004, %ax               /* 320 x 200, four colours */
    int $0x10
    movw $0xB800, %ax
    movw %ax, %es
    xorw %di, %di
    movw $0x4000, %cx               /* both banks: bytes 00 55 AA FF in 20-pixel runs */
1:  movw %di, %ax
    andw $0x1FFF, %ax
    xorw %dx, %dx
    movw $80, %bx
    divw %bx                        /* DX: the byte within its line */
    movw %dx, %ax
    xorw %dx, %dx
    movw $5, %bx
    divw %bx                        /* AX: 20-pixel run */
    andb $3, %al
    movw $A(fills), %bx
    xlat
    stosb
    loop 1b
    movw $0x3DA, %dx
2:  inb %dx, %al                    /* wait for the vertical retrace */
    testb $8, %al
    jz 2b
3:  inb %dx, %al                    /* and its end: the picture's first line */
    testb $8, %al
    jnz 3b
    movb $0x00, %al                 /* palette 0 for the top */
    movw $0x3D9, %dx
    outb %al, %dx
    movw $0x3DA, %dx
    movw $100, %cx
4:  inb %dx, %al                    /* a line: display enable goes off, then on */
    testb $1, %al
    jz 4b
5:  inb %dx, %al
    testb $1, %al
    jnz 5b
    loop 4b
    movb $0x20, %al                 /* palette 1 from here */
    movw $0x3D9, %dx
    outb %al, %dx
    movw $0x3DA, %dx
    jmp 2b

fills: .byte 0x00, 0x55, 0xAA, 0xFF
