/* The VGA's (and EGA's) modes, a key between each:
 *   1. mode 13h: a 16 x 16 grid of the 256 colours, a diagonal through
 *      INT 10h's pixel write, a pixel read back (grid colour 45h)
 *   2. mode 12h: 16 bands written in write mode 2, a yellow diagonal, the
 *      band read back at x = 300 (7)
 *   3. mode X (unchained 320 x 240): the grid again, plane by plane
 *   4. mode 13h's grid split: the top from row 96 panned 2 pixels left,
 *      below the line compare (scan line 200) the grid from row 0, not panned
 *   5. the CGA's mode 4: stripes in palette 0 (green, red, brown)
 *   6. mode 3 with the 8x8 font (50 rows), DAC 1 made orange, the card's
 *      answers to AH=12h BL=10h and AH=1Ah
 * On the EGA modes 13h and X fall back to text.
 */
#include "boot.h"

main:
    /* ---- 1: mode 13h ---- */
    movw $0x0013, %ax
    int $0x10
    call grid13
    movw $A(t_13h), %si
    movb $24, %dh
    call title
    xorw %cx, %cx                       /* a diagonal through INT 10h */
1:  movw %cx, %dx
    movw $0x0C0F, %ax
    xorb %bh, %bh
    int $0x10
    incw %cx
    cmpw $192, %cx
    jb 1b
    movw $100, %cx                      /* the grid at (100, 50): 5 + 4 * 16 */
    movw $50, %dx
    movb $0x0D, %ah
    xorb %bh, %bh
    int $0x10
    xorb %ah, %ah
    call hex16
    call waitkey

    /* ---- 2: mode 12h ---- */
    movw $0x0012, %ax
    int $0x10
    movw $0x3CE, %dx
    movw $0x0205, %ax                   /* write mode 2 */
    outw %ax, %dx
    movw $0xA000, %ax
    movw %ax, %es
    xorw %di, %di
    movw $480, %bx
1:  xorw %si, %si
2:  movw %si, %ax
    movb $5, %cl
    divb %cl                            /* AL: the band (a byte's 8 pixels each) */
    movb %es:(%di), %ah                 /* the latches */
    stosb
    incw %si
    cmpw $80, %si
    jb 2b
    decw %bx
    jnz 1b
    movw $0x0005, %ax                   /* write mode 0 */
    outw %ax, %dx
    xorw %ax, %ax
    movw %ax, %es
    movw $A(t_12h), %si
    movb $29, %dh
    call title
    xorw %cx, %cx
1:  movw %cx, %dx
    movw $0x0C0E, %ax
    xorb %bh, %bh
    int $0x10
    incw %cx
    cmpw $480, %cx
    jb 1b
    movw $300, %cx
    movw $10, %dx
    movb $0x0D, %ah
    xorb %bh, %bh
    int $0x10
    xorb %ah, %ah
    call hex16
    call waitkey

    /* ---- 3: mode X ---- */
    movw $0x0013, %ax
    int $0x10
    movw $0x3C4, %dx
    movw $0x0604, %ax                   /* chain-4 off */
    outw %ax, %dx
    movw $0x0100, %ax                   /* the sequencer held in reset */
    outw %ax, %dx
    movw $0x3C2, %dx
    movb $0xE3, %al                     /* 480 lines' sync */
    outb %al, %dx
    movw $0x3C4, %dx
    movw $0x0300, %ax
    outw %ax, %dx
    movw $0x3D4, %dx
    movw $0x0E11, %ax                   /* unprotected */
    outw %ax, %dx
    movw $A(modex), %si
    movw $10, %cx
1:  lodsw
    outw %ax, %dx
    loop 1b
    movw $0x3C4, %dx
    movw $0x0F02, %ax                   /* all planes: clear */
    outw %ax, %dx
    movw $0xA000, %ax
    movw %ax, %es
    xorw %di, %di
    xorw %ax, %ax
    movw $0x8000, %cx
    cld
    rep stosw
    xorw %bp, %bp                       /* the plane */
1:  movw $0x3C4, %dx
    movb $1, %ah
    movw %bp, %cx
    shlb %cl, %ah
    movb $2, %al
    outw %ax, %dx
    xorw %di, %di
    xorw %dx, %dx                       /* y */
2:  movw %dx, %ax
    movb $15, %cl
    divb %cl
    movb $4, %cl
    shlb %cl, %al
    movb %al, %bh                       /* the grid's row */
    xorw %si, %si
3:  movw %si, %ax
    shlw $1, %ax
    shlw $1, %ax
    addw %bp, %ax                       /* x */
    movb $20, %cl
    divb %cl
    addb %bh, %al
    stosb
    incw %si
    cmpw $80, %si
    jb 3b
    incw %dx
    cmpw $240, %dx
    jb 2b
    incw %bp
    cmpw $4, %bp
    jb 1b
    xorw %ax, %ax
    movw %ax, %es
    call waitkey

    /* ---- 4: split screen and panning ---- */
    movw $0x0013, %ax
    int $0x10
    call grid13
    movw $0x3D4, %dx
    movw $0x1E0C, %ax                   /* the start: row 96 (320 * 96 / 4) */
    outw %ax, %dx
    movw $0x000D, %ax
    outw %ax, %dx
    movw $0xC718, %ax                   /* the line compare: 199 */
    outw %ax, %dx
    movw $0x0F07, %ax                   /* (its bit 8 clear: writable when protected) */
    outw %ax, %dx
    movw $0x0109, %ax                   /* and bit 9 (mode 13h sets it) */
    outw %ax, %dx
    movw $0x3DA, %dx
    inb %dx, %al
    movw $0x3C0, %dx
    movb $0x30, %al                     /* the mode: panning stops at the split */
    outb %al, %dx
    movb $0x61, %al
    outb %al, %dx
    movb $0x33, %al                     /* panning: 2 pixels */
    outb %al, %dx
    movb $0x04, %al
    outb %al, %dx
    call waitkey

    /* ---- 5: the CGA's mode 4 ---- */
    movw $0x0004, %ax
    int $0x10
    movw $0xB800, %ax
    movw %ax, %es
    xorw %di, %di
    movw $0x4000, %cx
1:  movw %di, %ax
    andw $0x1FFF, %ax
    xorw %dx, %dx
    movw $80, %bx
    divw %bx
    movw %dx, %ax
    xorw %dx, %dx
    movw $5, %bx
    divw %bx
    andb $3, %al
    movw $A(fills), %bx
    xlat
    stosb
    loop 1b
    xorw %ax, %ax
    movw %ax, %es
    movw $0x0B00, %ax                   /* palette 0 */
    movw $0x0100, %bx
    int $0x10
    movw $A(t_cga), %si
    movb $24, %dh
    call title
    call waitkey

    /* ---- 6: 80 x 50 text, the DAC ---- */
    movw $0x0003, %ax
    int $0x10
    movw $0x1112, %ax                   /* the 8x8 font: 50 rows */
    xorb %bl, %bl
    int $0x10
    movw $0x3C8, %dx                    /* DAC 1: orange */
    movb $1, %al
    outb %al, %dx
    incw %dx
    movb $63, %al
    outb %al, %dx
    movb $32, %al
    outb %al, %dx
    movb $0, %al
    outb %al, %dx
    movw $A(t_text), %si
    call print
    call newline
    movb $0x12, %ah                     /* the EGA's information */
    movb $0x10, %bl
    int $0x10
    movw %bx, %ax
    call hex16
    movb $' ', %al
    call putc
    movw %cx, %ax
    call hex16
    movb $' ', %al
    call putc
    movw $0x1A00, %ax                   /* the display combination (VGA) */
    xorw %bx, %bx
    int $0x10
    call hex16
    movb $' ', %al
    call putc
    movw %bx, %ax
    call hex16
    call newline
    movw $0x0920, %ax                   /* a row in white on blue (orange now) */
    movw $0x001F, %bx
    movw $40, %cx
    int $0x10
    movw $0x0300, %ax                   /* the rows: the cursor to the last */
    xorb %bh, %bh
    movw $0x3100, %dx
    movb $0x02, %ah
    int $0x10
    movw $A(t_last), %si
    call print
9:  hlt
    jmp 9b

/* ---- Helpers ---- */

/* mode 13h's grid: colour x / 20 + y / 12 * 16, lines 0-191 */
grid13:
    movw $0xA000, %ax
    movw %ax, %es
    xorw %di, %di
    xorw %dx, %dx
1:  movw %dx, %ax
    movb $12, %cl
    divb %cl
    movb $4, %cl
    shlb %cl, %al
    movb %al, %bh
    xorw %si, %si
2:  movw %si, %ax
    movb $20, %cl
    divb %cl
    addb %bh, %al
    stosb
    incw %si
    cmpw $320, %si
    jb 2b
    incw %dx
    cmpw $192, %dx
    jb 1b
    xorw %ax, %ax
    movw %ax, %es
    ret

/* SI's string at row DH, column 0 */
title:
    pushw %dx
    movb $0x02, %ah
    xorb %bh, %bh
    xorb %dl, %dl
    int $0x10
    popw %dx
    call print
    ret

putc:
    pushw %ax
    pushw %bx
    movb $0x0E, %ah
    movw $0x0007, %bx
    int $0x10
    popw %bx
    popw %ax
    ret

waitkey:
    xorb %ah, %ah
    int $0x16
    ret

modex:  .word 0x0D06, 0x3E07, 0x4109, 0xEA10, 0xAC11, 0xDF12, 0x0014, 0xE715, 0x0616, 0xE317
fills:  .byte 0x00, 0x55, 0xAA, 0xFF
t_13h:  .asciz "Mode 13h, (100,50): "
t_12h:  .asciz "Mode 12h, (300,10): "
t_cga:  .asciz "Mode 4, palette 0"
t_text: .asciz "80 x 50 text, DAC 1 orange. 12h/10h BX CX, 1Ah AX BX:"
t_last: .asciz "the last row (49)"
