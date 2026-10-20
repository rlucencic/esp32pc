/* The EGA's modes (and the VGA's, the same), a key between each:
 *   1. mode 10h (640 x 350): 16 bands in write mode 2, a diagonal through
 *      INT 10h, the band read back at x = 300 (7)
 *   2. mode 0Dh (320 x 200): 16 bands through set/reset (write mode 0),
 *      text in colour 14
 *   3. mode 3 with the 8x8 font: 43 rows on the EGA (50 on the VGA), a
 *      blinking line (CE: yellow on red), a key, then bright backgrounds
 *      (AX=1003h BL=0) and a second line
 */
#include "boot.h"

main:
    /* ---- 1: mode 10h ---- */
    movw $0x0010, %ax
    int $0x10
    movw $0x3CE, %dx
    movw $0x0205, %ax                   /* write mode 2 */
    outw %ax, %dx
    movw $0xA000, %ax
    movw %ax, %es
    xorw %di, %di
    movw $350, %bx
1:  xorw %si, %si
2:  movw %si, %ax
    movb $5, %cl
    divb %cl
    movb %es:(%di), %ah
    stosb
    incw %si
    cmpw $80, %si
    jb 2b
    decw %bx
    jnz 1b
    movw $0x0005, %ax
    outw %ax, %dx
    xorw %ax, %ax
    movw %ax, %es
    movw $A(t_10h), %si
    movb $24, %dh
    call title
    xorw %cx, %cx
1:  movw %cx, %dx
    movw $0x0C0F, %ax
    xorb %bh, %bh
    int $0x10
    incw %cx
    cmpw $350, %cx
    jb 1b
    movw $300, %cx
    movw $10, %dx
    movb $0x0D, %ah
    xorb %bh, %bh
    int $0x10
    xorb %ah, %ah
    call hex16
    call waitkey

    /* ---- 2: mode 0Dh ---- */
    movw $0x000D, %ax
    int $0x10
    movw $0x3CE, %dx
    movw $0x0F01, %ax                   /* set/reset on all planes */
    outw %ax, %dx
    movw $0xA000, %ax
    movw %ax, %es
    xorw %bx, %bx                       /* the band: 20 pixels, 2.5 bytes: do it by bit mask */
1:  movb %bl, %ah                       /* set/reset: the colour */
    xorb %al, %al
    movw $0x3CE, %dx
    outw %ax, %dx
    movw %bx, %ax                       /* x from 20 * band to 20 * band + 19 */
    movb $20, %cl
    mulb %cl
    movw %ax, %bp
    movw $20, %si
2:  movw %bp, %cx                       /* the bit mask for x */
    andb $7, %cl
    movb $0x80, %ah
    shrb %cl, %ah
    movb $8, %al
    movw $0x3CE, %dx
    outw %ax, %dx
    movw %bp, %di
    movb $3, %cl
    shrw %cl, %di
    movw $200, %cx
3:  movb %es:(%di), %al                 /* the latches, then a write */
    movb %al, %es:(%di)
    addw $40, %di
    loop 3b
    incw %bp
    decw %si
    jnz 2b
    incw %bx
    cmpw $16, %bx
    jb 1b
    movw $0x3CE, %dx
    movw $0x0001, %ax                   /* the defaults back */
    outw %ax, %dx
    movw $0xFF08, %ax
    outw %ax, %dx
    xorw %ax, %ax
    movw %ax, %es
    movb $0x02, %ah
    xorb %bh, %bh
    movw $0x1800, %dx
    int $0x10
    movw $A(t_0dh), %si
1:  lodsb
    orb %al, %al
    jz 2f
    movb $0x0E, %ah
    movw $0x000E, %bx
    int $0x10
    jmp 1b
2:  call waitkey

    /* ---- 3: 43 (50) rows ---- */
    movw $0x0003, %ax
    int $0x10
    movw $0x1112, %ax
    xorb %bl, %bl
    int $0x10
    movw $A(t_text), %si
    call print
    movb $0x0F, %ah                     /* rows: from 40:84 */
    int $0x10
    pushw %ds
    movw $0x40, %ax
    movw %ax, %ds
    movb 0x84, %al
    popw %ds
    xorb %ah, %ah
    incw %ax
    call hex16
    call newline
    movw $0x09B1, %ax                   /* blinking: bit 7 */
    movw $0x00CE, %bx
    movw $20, %cx
    int $0x10
    movb $0x02, %ah
    xorb %bh, %bh
    movw $0x0200, %dx
    int $0x10
    call waitkey
    movw $0x1003, %ax                   /* bright backgrounds (both lines) */
    xorb %bl, %bl
    int $0x10
    movw $0x0941, %ax
    movw $0x00CE, %bx
    movw $20, %cx
    int $0x10
9:  hlt
    jmp 9b

title:
    pushw %dx
    movb $0x02, %ah
    xorb %bh, %bh
    xorb %dl, %dl
    int $0x10
    popw %dx
    call print
    ret

waitkey:
    xorb %ah, %ah
    int $0x16
    ret

t_10h:  .asciz "Mode 10h, (300,10): "
t_0dh:  .asciz "Mode 0Dh"
t_text: .asciz "8x8 font, rows: "
