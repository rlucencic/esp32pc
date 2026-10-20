/*
 * Test programs' boot sector: the program after it (up to 35 sectors) read
 * to 0000:7E00 through INT 13h, then `main`. GNU assembler syntax (AT&T;
 * LLVM's Intel syntax makes 32-bit CALLs in .code16), through the C
 * preprocessor; tools/mkboot.sh makes the floppy.
 *
 * No linker: addresses are label differences, A(label) the address of a
 * label with DS = 0. Helpers: print (SI: a 0-terminated string, through
 * INT 10h's teletype), hex16 (AX), newline.
 */
#define ORG     0x7C00
#define A(x)    ((x) - start + ORG)

.code16
.text
.globl start
start:
    cli
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $ORG, %sp
    sti
    movw $0x0211, %ax               /* sectors 2-18 of cylinder 0, head 0 */
    movw $0x0002, %cx
    movb $0, %dh
    movw $A(sector2), %bx
    int $0x13
    movw $0x0212, %ax               /* and head 1's 18 */
    movw $0x0001, %cx
    movb $1, %dh
    movw $(A(sector2) + 17 * 512), %bx
    int $0x13
    jmp main
    .org 510
    .word 0xAA55
sector2:

print:
    pushw %ax
    pushw %bx
1:  lodsb
    orb %al, %al
    jz 2f
    movb $0x0E, %ah
    movw $0x0007, %bx
    int $0x10
    jmp 1b
2:  popw %bx
    popw %ax
    ret

hex16:
    pushw %ax
    pushw %bx
    pushw %cx
    pushw %dx
    movw %ax, %dx
    movb $4, %ch                    /* (8088 code: shifts by CL) */
1:  movb $4, %cl
    rolw %cl, %dx
    movb %dl, %al
    andb $0x0F, %al
    addb $'0', %al
    cmpb $'9', %al
    jbe 2f
    addb $7, %al
2:  movb $0x0E, %ah
    movw $0x0007, %bx
    int $0x10
    decb %ch
    jnz 1b
    popw %dx
    popw %cx
    popw %bx
    popw %ax
    ret

newline:
    pushw %ax
    pushw %bx
    movw $0x0E0D, %ax
    movw $0x0007, %bx
    int $0x10
    movw $0x0E0A, %ax
    int $0x10
    popw %bx
    popw %ax
    ret
