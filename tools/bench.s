/* The speed benchmark (tools/bench.sh makes a floppy of it): a boot
 * sector that runs a mix of real mode code forever, 8088 instructions
 * only, and counts its rounds in hex at the top left of the text screen:
 * checksums (LODSW, ADD, ADC), a sort pass ([BX+SI], CMP, Jcc, XCHG),
 * recursive calls with a BP frame, REP MOVSW, shifts and a MUL, LOOPs.
 * Loaded at 0000:7C00; data at 0000:0600 and 2000:0000.
 *
 * LLVM's assembler makes 32-bit CALLs and RETs in .code16 with the Intel
 * syntax: they're written as bytes (call16, 0xC3).
 */
.code16
.intel_syntax noprefix
.text
.macro call16 target
    .byte 0xE8
    .word \target - . - 2
.endm
.globl start
start:
    cli
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    mov ax, 0x2000
    mov es, ax
    xor di, di                  /* fill 2000:0000, 4 KB, with a pattern */
    mov cx, 2048
    mov ax, 0x1234
fill:
    stosw
    add ax, 0x3579
    push cx
    mov cl, 3
    rol ax, cl
    pop cx
    loop fill
    mov word ptr [0x600], 0
    mov word ptr [0x602], 0

outer:
    /* checksum of 1 KB at 2000:0000 */
    push ds
    mov ax, 0x2000
    mov ds, ax
    xor si, si
    mov cx, 512
    xor bx, bx
    xor dx, dx
    cld
sum:
    lodsw
    add bx, ax
    adc dx, 0
    loop sum
    pop ds
    mov [0x604], bx

    /* a sort pass over 64 words at 2000:0400 */
    push ds
    mov ax, 0x2000
    mov ds, ax
    mov bx, 0x400
    xor si, si
    mov cx, 63
pass:
    mov ax, [bx + si]
    cmp ax, [bx + si + 2]
    jle inorder
    xchg ax, [bx + si + 2]
    mov [bx + si], ax
inorder:
    add si, 2
    loop pass
    pop ds

    /* fib(9), recursive, with a BP frame */
    mov ax, 9
    push ax
    call16 fib
    add sp, 2
    mov [0x606], ax

    /* REP MOVSW: 256 words 2000:0000 -> 2000:0800 */
    push ds
    mov ax, 0x2000
    mov ds, ax
    mov es, ax
    xor si, si
    mov di, 0x800
    mov cx, 256
    rep movsw
    pop ds

    /* arithmetic: shifts, a multiply, byte operations */
    mov cx, 32
    mov ax, 0x1357
arith:
    mov dx, ax
    shl dx, 1
    xor ax, dx
    mov bl, al
    and bl, 0x0F
    add al, bl
    mov dx, 13
    mul dx
    inc ax
    loop arith

    /* the counter, in hex at the top left */
    add word ptr [0x600], 1
    adc word ptr [0x602], 0
    mov ax, 0xB800
    mov es, ax
    xor di, di
    mov dx, [0x602]
    call16 hex4
    mov dx, [0x600]
    call16 hex4
    jmp outer

/* fib(n) on the stack: AX */
fib:
    push bp
    mov bp, sp
    mov ax, [bp + 4]
    cmp ax, 2
    jb fib_done
    dec ax
    push ax
    call16 fib
    add sp, 2
    push ax
    mov ax, [bp + 4]
    sub ax, 2
    push ax
    call16 fib
    add sp, 2
    pop dx
    add ax, dx
fib_done:
    pop bp
    .byte 0xC3

/* DX in hex at ES:DI (attribute 07h) */
hex4:
    mov cx, 4
hex_digit:
    push cx
    mov cl, 4
    rol dx, cl
    pop cx
    mov al, dl
    and al, 0x0F
    add al, '0'
    cmp al, '9'
    jbe hex_put
    add al, 7
hex_put:
    mov ah, 0x07
    stosw
    loop hex_digit
    .byte 0xC3

count:  .word 0, 0
result: .word 0, 0

    .org 510
    .byte 0x55, 0xAA
