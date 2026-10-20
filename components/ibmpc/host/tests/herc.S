/* Hercules graphics: the card set up by hand (as programs do: the BIOS
 * has no Hercules graphics mode), a border, a diagonal, a filled box and
 * stripes drawn through the four banks; PAGE1 draws and shows B800's page.
 */
#include "boot.h"

#ifdef PAGE1
#define SEG     0xB800
#define MODE    0x8A
#else
#define SEG     0xB000
#define MODE    0x0A
#endif

main:
    movw $0x3BF, %dx                /* configuration: graphics allowed, both pages */
    movb $3, %al
    outb %al, %dx
    movw $0x3B8, %dx                /* graphics, video off while it's set up */
    movb $0x02, %al
    outb %al, %dx
    movw $A(crtc), %si
    xorb %bl, %bl
1:  movw $0x3B4, %dx
    movb %bl, %al
    outb %al, %dx
    incw %dx
    lodsb
    outb %al, %dx
    incb %bl
    cmpb $12, %bl
    jb 1b
    movw $SEG, %ax
    movw %ax, %es
    xorw %di, %di
    movw $0x4000, %cx
    xorw %ax, %ax
    rep stosw
    xorw %cx, %cx                   /* top and bottom edges */
2:  xorw %dx, %dx
    call plot
    movw $347, %dx
    call plot
    incw %cx
    cmpw $720, %cx
    jb 2b
    xorw %dx, %dx                   /* left and right edges, the diagonal */
3:  xorw %cx, %cx
    call plot
    movw $719, %cx
    call plot
    movw %dx, %cx
    call plot
    incw %dx
    cmpw $348, %dx
    jb 3b
    movw $100, %dx                  /* a box: 400-599 x 100-199 */
4:  movw $400, %cx
5:  call plot
    incw %cx
    cmpw $600, %cx
    jb 5b
    incw %dx
    cmpw $200, %dx
    jb 4b
    movw $250, %dx                  /* stripes: every other line, 400-699 x 250-299 */
6:  movw $400, %cx
7:  call plot
    incw %cx
    cmpw $700, %cx
    jb 7b
    addw $2, %dx
    cmpw $300, %dx
    jb 6b
    movw $0x3B8, %dx
    movb $MODE, %al
    outb %al, %dx
8:  hlt
    jmp 8b

/* the pixel at CX, DX: bank y & 3, 90 bytes a line in it */
plot:
    pushw %ax
    pushw %bx
    pushw %cx
    pushw %dx
    pushw %di
    movw %cx, %bx                   /* x */
    movw %dx, %ax                   /* y */
    movw %ax, %di
    andw $3, %di
    movb $13, %cl
    shlw %cl, %di                   /* (y & 3) * 2000h */
    shrw $1, %ax
    shrw $1, %ax
    movw $90, %cx
    mulw %cx
    addw %ax, %di                   /* + (y / 4) * 90 */
    movw %bx, %ax
    shrw $1, %ax
    shrw $1, %ax
    shrw $1, %ax
    addw %ax, %di                   /* + x / 8 */
    movb %bl, %cl
    andb $7, %cl
    movb $0x80, %al
    shrb %cl, %al
    orb %al, %es:(%di)
    popw %di
    popw %dx
    popw %cx
    popw %bx
    popw %ax
    ret

crtc: .byte 0x35, 0x2D, 0x2E, 0x07, 0x5B, 0x02, 0x57, 0x57, 0x02, 0x03, 0x00, 0x00
