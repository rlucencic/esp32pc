/*
 * The built-in BIOS: a ROM at F0000 made at each reset, its services in C
 *
 * The ROM has IBM's entry points (F000:E05B, the interrupt handlers'
 * addresses programs call directly), the tables (disk and video
 * parameters, the 8x8 font at FA6E), the model byte; each handler is a few
 * instructions around a trap (FE 38 nn, which the CPU core hands to
 * bios_trap() only inside this ROM: the ST's cartridge idea). The
 * services work on the emulated hardware's registers through its ports,
 * so what programs read back is what the BIOS set.
 *
 *   bios.c         POST, the boot, the time, the small services
 *   bios_video.c   INT 10h (MDA, CGA)
 *   bios_disk.c    INT 13h (floppies and hard disks from their images)
 *   bios_kbd.c     INT 09h and 16h
 */
#include <stdio.h>
#include <time.h>
#include "pc_internal.h"

#define TRAP(n)     0xFE, 0x38, (n)
#define ROM(off)    (pc.ram + 0xF0000 + (off))

/* Where the handlers' bodies go: past the IBM entry points' jumps */
static uint16_t s_body;
static bool s_told_no_boot;
static uint16_t s_at_irq, s_at_int70, s_at_int71, s_at_int75;     /* the AT's handlers */
static bool s_waiting;              /* INT 15h AH=86h */
static uint64_t s_wait_until;

static void put(uint16_t off, const uint8_t *bytes, size_t n)
{
    memcpy(ROM(off), bytes, n);
}

/* An entry point at IBM's address: a jump to a body of its own */
static void entry(uint16_t at, const uint8_t *body, size_t n)
{
    uint16_t rel = (uint16_t)(s_body - (at + 3));
    uint8_t jmp[3] = { 0xE9, (uint8_t)rel, (uint8_t)(rel >> 8) };
    put(at, jmp, 3);
    put(s_body, body, n);
    s_body = (uint16_t)(s_body + n);
}

static void set_vector(int n, uint16_t seg, uint16_t off)
{
    uint8_t *v = pc.ram + n * 4;
    v[0] = (uint8_t)off;
    v[1] = (uint8_t)(off >> 8);
    v[2] = (uint8_t)seg;
    v[3] = (uint8_t)(seg >> 8);
}

/* The ROM's fixed addresses */
enum {
    A_POST = 0xE05B,
    A_NMI = 0xE2C3,
    A_HD_PARAMS = 0xE401,           /* the fixed disks' parameter tables (16 bytes each) */
    A_INT19 = 0xE6F2,
    A_CONFIG = 0xE6F5,
    A_INT14 = 0xE739,
    A_INT16 = 0xE82E,
    A_INT09 = 0xE987,
    A_INT13 = 0xEC59,
    A_INT0E = 0xEF57,
    A_DISK_PARAMS = 0xEFC7,
    A_INT17 = 0xEFD2,
    A_INT10 = 0xF065,
    A_VIDEO_PARAMS = 0xF0A4,
    A_INT12 = 0xF841,
    A_INT11 = 0xF84D,
    A_INT15 = 0xF859,
    A_FONT = 0xFA6E,
    A_INT1A = 0xFE6E,
    A_INT08 = 0xFEA5,
    A_IRQ = 0xFF23,
    A_IRET = 0xFF53,
    A_INT05 = 0xFF54,
    A_RESET = 0xFFF0,
    A_BASIC = 0x6000,               /* F600:0000, where IBM's cassette BASIC was */
};

/* ---- A real BIOS ROM (pc_config_t.bios_rom) ----
 *
 * At the top of the first megabyte, F0000h-FFFFFh otherwise FFh; ours as
 * option ROMs its POST finds: the hard disks' INT 13h at C8000h (as an
 * XT's controller card brings its own; the floppies stay the BIOS's,
 * through INT 40h), the EGA's and VGA's BIOS at C0000h.
 */
void bios_rom_checksum(uint8_t *rom, uint32_t size)
{
    uint8_t sum = 0;
    for (uint32_t i = 0; i + 1 < size; i++) {
        sum = (uint8_t)(sum + rom[i]);
    }
    rom[size - 1] = (uint8_t)-sum;
}

static void real_bios(void)
{
    uint32_t size = pc.cfg.bios_size > 0x10000 ? 0x10000 : pc.cfg.bios_size;
    memset(ROM(0), 0xFF, 0x10000);
    memcpy(pc.ram + 0x100000 - size, pc.cfg.bios_rom + (pc.cfg.bios_size - size), size);
    uint8_t *r = pc.ram + HD_ROM;
    memset(r, 0, 0x2000);
    static const uint8_t rom[] = {
        0x55, 0xAA, 0x10,                   /* 8 KB */
        TRAP(0x61), 0xCB,                   /* 0003: its initialisation (hd_rom_init()) */
        0x80, 0xFA, 0x80,                   /* 0007: INT 13h: CMP DL, 80h */
        0x72, 0x05,                         /* JB the floppies */
        0xFB, TRAP(0x13), 0xCF,             /* the hard disks: ours */
        0xEA, 0, 0, 0, 0,                   /* 0011: the floppies: the BIOS's INT 13h */
        TRAP(0x62),                         /* 0016: INT 19h: C: if A: is empty (hd_boot()) */
        0xEA, 0, 0, 0, 0,                   /* 0019: else the BIOS's bootstrap */
    };
    memcpy(r, rom, sizeof(rom));
    bios_rom_checksum(r, 0x2000);
}

#define HD_ROM_INT13    0x0007
#define HD_ROM_OLD13    0x0012
#define HD_ROM_INT19    0x0016
#define HD_ROM_OLD19    0x001A

static uint32_t vector(int n)
{
    return (uint32_t)(pc.ram[n * 4] | pc.ram[n * 4 + 1] << 8) |
           (uint32_t)(pc.ram[n * 4 + 2] | pc.ram[n * 4 + 3] << 8) << 16;
}

/* The hard disks' option ROM, as the real BIOS's POST calls it: INT 13h
 * for them (the XT's floppies' handler to INT 40h first, where the AT's
 * BIOS has it already), their tables at INT 41h and 46h, how many at 40:75
 */
static void hd_rom_init(x86_t *c)
{
    (void)c;
    bios_disk_init();
    if (*bda(0x75) == 0) {
        return;
    }
    uint8_t *r = pc.ram + HD_ROM;
    uint32_t v13 = vector(0x13), v19 = vector(0x19);
    memcpy(r + HD_ROM_OLD13, &(uint8_t[4]){ (uint8_t)v13, (uint8_t)(v13 >> 8), (uint8_t)(v13 >> 16), (uint8_t)(v13 >> 24) }, 4);
    memcpy(r + HD_ROM_OLD19, &(uint8_t[4]){ (uint8_t)v19, (uint8_t)(v19 >> 8), (uint8_t)(v19 >> 16), (uint8_t)(v19 >> 24) }, 4);
    bios_rom_checksum(r, 0x2000);
    if (pc.cfg.board != PC_AT) {
        set_vector(0x40, (uint16_t)(v13 >> 16), (uint16_t)v13);
    }
    set_vector(0x13, HD_ROM >> 4, HD_ROM_INT13);
    set_vector(0x19, HD_ROM >> 4, HD_ROM_INT19);
    set_vector(0x41, HD_ROM >> 4, HD_ROM_PARAMS);
    set_vector(0x46, HD_ROM >> 4, HD_ROM_PARAMS + 16);
}

/* INT 19h through the hard disks' ROM: with A: empty, C:'s boot sector;
 * else back for the BIOS's own bootstrap (the far jump after the trap)
 */
static void hd_boot(x86_t *c)
{
    if (disks[0].present) {
        return;
    }
    for (int i = 0; i < PC_HARD_DISKS; i++) {
        if (disks[PC_FLOPPIES + i].present && disk_read(PC_FLOPPIES + i, 0, 1, pc.ram + 0x7C00) &&
            pc.ram[0x7DFE] == 0x55 && pc.ram[0x7DFF] == 0xAA) {
            c->r.b[4] = 0x80;       /* DL */
            x86_load_seg(c, X86_CS, 0x0000);
            c->ip = 0x7C00;
            c->flags |= X86_IF;
            return;
        }
    }
}

void bios_init(void)
{
    if (pc.cfg.video == PC_EGA || pc.cfg.video == PC_VGA) {
        bios_vga_rom();
    }
    if (pc.cfg.bios_rom) {
        real_bios();
        return;
    }
    memset(ROM(0), 0xFF, 0x10000);
    s_body = 0xC000;
    bool at = pc.cfg.board == PC_AT;

    static const uint8_t post[] = { 0xFA, TRAP(0x00), 0xFB, 0xCD, 0x19, 0xF4, 0xEB, 0xFD };
    put(A_POST, post, sizeof(post));
    static const uint8_t iret[] = { 0xCF };
    put(A_NMI, iret, 1);
    put(A_IRET, iret, 1);

    /* INT 08: the tick, then the program's INT 1C, then the EOI */
    static const uint8_t int08[] = {
        0xFB, 0x50, TRAP(0x08), 0xCD, 0x1C, 0xFA, 0xB0, 0x20, 0xE6, 0x20, 0x58, 0xCF,
    };
    put(A_INT08, int08, sizeof(int08));
    /* INT 09: the scan code; the XT's keyboard is cleared through port 61h */
    static const uint8_t int09_xt[] = {
        0xFB, 0x50, 0xE4, 0x60, 0x50, 0xE4, 0x61, 0x0C, 0x80, 0xE6, 0x61, 0x24, 0x7F, 0xE6, 0x61, 0x58,
        TRAP(0x09), 0xFA, 0xB0, 0x20, 0xE6, 0x20, 0x58, 0xCF,
    };
    /* The AT's: INT 15h AH=4Fh may take the key (CF clear) first */
    static const uint8_t int09_at[] = {
        0xFB, 0x50, 0xE4, 0x60, 0xB4, 0x4F, 0xF9, 0xCD, 0x15, 0x73, 0x03, TRAP(0x09),
        0xFA, 0xB0, 0x20, 0xE6, 0x20, 0x58, 0xCF,
    };
    if (at) {
        entry(A_INT09, int09_at, sizeof(int09_at));
    } else {
        entry(A_INT09, int09_xt, sizeof(int09_xt));
    }
    static const uint8_t int10[] = { 0xFB, TRAP(0x10), 0xCF };
    entry(A_INT10, int10, sizeof(int10));
    static const uint8_t int11[] = { TRAP(0x11), 0xCF };
    entry(A_INT11, int11, sizeof(int11));
    static const uint8_t int12[] = { TRAP(0x12), 0xCF };
    entry(A_INT12, int12, sizeof(int12));
    static const uint8_t int13[] = { 0xFB, TRAP(0x13), 0xCF };
    entry(A_INT13, int13, sizeof(int13));
    static const uint8_t int14[] = { 0xFB, TRAP(0x14), 0xCF };
    entry(A_INT14, int14, sizeof(int14));
    /* INT 15h: AH=86h's wait goes round again while CF is set */
    static const uint8_t int15[] = { 0xFB, TRAP(0x15), 0x73, 0x02, 0xEB, 0xF9, 0xCF };
    entry(A_INT15, int15, sizeof(int15));
    /* INT 16: waiting for a key sleeps (HLT) until the next interrupt */
    static const uint8_t int16[] = { 0xFB, TRAP(0x16), 0x73, 0x03, 0xF4, 0xEB, 0xF7, 0xCF };
    entry(A_INT16, int16, sizeof(int16));
    static const uint8_t int17[] = { 0xFB, TRAP(0x17), 0xCF };
    entry(A_INT17, int17, sizeof(int17));
    /* INT 19: boot; without a disk, wait and try again (a disk may come) */
    static const uint8_t int19[] = { TRAP(0x19), 0xFB, 0xF4, 0xEB, 0xF9 };
    entry(A_INT19, int19, sizeof(int19));
    static const uint8_t int1a[] = { 0xFB, TRAP(0x1A), 0xCF };
    entry(A_INT1A, int1a, sizeof(int1a));
    static const uint8_t int05[] = { TRAP(0x05), 0xCF };
    put(A_INT05, int05, sizeof(int05));
    static const uint8_t irq[] = { 0x50, 0xB0, 0x20, 0xE6, 0x20, 0x58, 0xCF };
    put(A_IRQ, irq, sizeof(irq));
    entry(A_INT0E, irq, sizeof(irq));
    if (at) {
        /* The second 8259's: an EOI to both; the clock (reads register C);
         * IRQ 9 as IRQ 2 (INT 0Ah); the coprocessor's error as an NMI
         */
        static const uint8_t irq_slave[] = { 0x50, 0xB0, 0x20, 0xE6, 0xA0, 0xE6, 0x20, 0x58, 0xCF };
        static const uint8_t int70[] = {
            0x50, 0xB0, 0x0C, 0xE6, 0x70, 0xE4, 0x71, 0xB0, 0x20, 0xE6, 0xA0, 0xE6, 0x20, 0x58, 0xCF,
        };
        static const uint8_t int71[] = { 0x50, 0xB0, 0x20, 0xE6, 0xA0, 0x58, 0xCD, 0x0A, 0xCF };
        static const uint8_t int75[] = {
            0x50, 0x30, 0xC0, 0xE6, 0xF0, 0xB0, 0x20, 0xE6, 0xA0, 0xE6, 0x20, 0x58, 0xCD, 0x02, 0xCF,
        };
        s_at_irq = s_body;
        put(s_body, irq_slave, sizeof(irq_slave));
        s_body = (uint16_t)(s_body + sizeof(irq_slave));
        s_at_int70 = s_body;
        put(s_body, int70, sizeof(int70));
        s_body = (uint16_t)(s_body + sizeof(int70));
        s_at_int71 = s_body;
        put(s_body, int71, sizeof(int71));
        s_body = (uint16_t)(s_body + sizeof(int71));
        s_at_int75 = s_body;
        put(s_body, int75, sizeof(int75));
        s_body = (uint16_t)(s_body + sizeof(int75));
    }
    /* No BASIC: a word, a key, the boot again */
    static const uint8_t basic[] = { TRAP(0x18), 0xB4, 0x00, 0xCD, 0x16, 0xCD, 0x19 };
    put(A_BASIC, basic, sizeof(basic));

    /* The diskette parameters (1.44 MB) */
    static const uint8_t disk_params[] = { 0xDF, 0x02, 0x25, 0x02, 0x12, 0x1B, 0xFF, 0x6C, 0xF6, 0x0F, 0x08 };
    put(A_DISK_PARAMS, disk_params, sizeof(disk_params));
    /* The video parameters: the 6845's registers for 40 x 25, 80 x 25,
     * graphics and the MDA, the page sizes, columns, mode registers
     */
    static const uint8_t video_params[] = {
        0x38, 0x28, 0x2D, 0x0A, 0x1F, 0x06, 0x19, 0x1C, 0x02, 0x07, 0x06, 0x07, 0x00, 0x00, 0x00, 0x00,
        0x71, 0x50, 0x5A, 0x0A, 0x1F, 0x06, 0x19, 0x1C, 0x02, 0x07, 0x06, 0x07, 0x00, 0x00, 0x00, 0x00,
        0x38, 0x28, 0x2D, 0x0A, 0x7F, 0x06, 0x64, 0x70, 0x02, 0x01, 0x06, 0x07, 0x00, 0x00, 0x00, 0x00,
        0x61, 0x50, 0x52, 0x0F, 0x19, 0x06, 0x19, 0x19, 0x02, 0x0D, 0x0B, 0x0C, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x08, 0x00, 0x10, 0x00, 0x40, 0x00, 0x40,
        0x28, 0x28, 0x50, 0x50, 0x28, 0x28, 0x50, 0x50,
        0x2C, 0x28, 0x2D, 0x29, 0x2A, 0x2E, 0x1E, 0x29,
    };
    put(A_VIDEO_PARAMS, video_params, sizeof(video_params));
    put(A_FONT, pc_font_bios8, 128 * 8);
    /* The configuration (INT 15h AH=C0h on the AT) */
    static const uint8_t config[] = { 0x08, 0x00, 0xFC, 0x01, 0x00, 0x70, 0x00, 0x00, 0x00, 0x00 };
    if (at) {
        put(A_CONFIG, config, sizeof(config));
    }

    static const uint8_t reset[] = { 0xEA, 0x5B, 0xE0, 0x00, 0xF0 };
    put(A_RESET, reset, sizeof(reset));
    memcpy(ROM(0xFFF5), at ? "11/15/85" : "01/10/86", 8);
    *ROM(0xFFFE) = at ? 0xFC : 0xFE;
    *ROM(0xFFFF) = 0x00;
}

/* ---- Helpers ---- */

static void flags_word(x86_t *c, uint16_t bit, bool on)
{
    uint32_t a = c->seg[X86_SS].base + (uint16_t)(c->r.w[X86_SP] + 4);
    uint16_t f = x86_read16(c, a);
    x86_write16(c, a, on ? (uint16_t)(f | bit) : (uint16_t)(f & ~bit));
}

void bios_return_cf(x86_t *c, bool cf)
{
    flags_word(c, X86_CF, cf);
}

void bios_return_zf(x86_t *c, bool zf)
{
    flags_word(c, X86_ZF, zf);
}

/* CF as the next instruction sees it (the stubs' loops) */
static void live_cf(x86_t *c, bool cf)
{
    uint16_t f = x86_get_flags(c);
    x86_set_flags(c, cf ? (uint16_t)(f | X86_CF) : (uint16_t)(f & ~X86_CF));
}

static void print(const char *s)
{
    for (; *s; s++) {
        bios_teletype((uint8_t)*s, 7);
    }
}

static uint8_t bcd(int v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

/* The time of day from the board's clock (local), if it was set */
static bool clock_now(struct tm *tm)
{
    time_t now = time(NULL);
    localtime_r(&now, tm);
    return tm->tm_year + 1900 >= 2024;
}

/* ---- POST ---- */

static void post(x86_t *c)
{
    bool warm = bda16(0x72) == 0x1234;
    bool at = pc.cfg.board == PC_AT;

    /* The AT's reset from a program (the 8042, a triple fault): the CMOS
     * shutdown byte says whether to go on where it says (40:67), as
     * programs coming back from protected mode ask
     */
    if (at) {
        uint8_t code = rtc_shutdown_code();
        rtc_set_shutdown_code(0);
        if (code == 0x05 || code == 0x0A) {
            if (code == 0x05) {
                pic_write(1, 0, 0x20);
                pic_write(0, 0, 0x20);
            }
            x86_load_seg(c, X86_CS, bda16(0x69));
            c->ip = bda16(0x67);
            return;
        }
    }

    /* The interrupt vectors */
    for (int i = 0; i < 0x20; i++) {
        set_vector(i, 0xF000, A_IRET);
    }
    set_vector(0x02, 0xF000, A_NMI);
    set_vector(0x05, 0xF000, A_INT05);
    set_vector(0x08, 0xF000, A_INT08);
    set_vector(0x09, 0xF000, A_INT09);
    for (int i = 0x0A; i <= 0x0F; i++) {
        set_vector(i, 0xF000, A_IRQ);
    }
    set_vector(0x0E, 0xF000, A_INT0E);
    set_vector(0x10, 0xF000, A_INT10);
    set_vector(0x11, 0xF000, A_INT11);
    set_vector(0x12, 0xF000, A_INT12);
    set_vector(0x13, 0xF000, A_INT13);
    set_vector(0x14, 0xF000, A_INT14);
    set_vector(0x15, 0xF000, A_INT15);
    set_vector(0x16, 0xF000, A_INT16);
    set_vector(0x17, 0xF000, A_INT17);
    set_vector(0x18, 0xF600, 0x0000);
    set_vector(0x19, 0xF000, A_INT19);
    set_vector(0x1A, 0xF000, A_INT1A);
    set_vector(0x1D, 0xF000, A_VIDEO_PARAMS);
    set_vector(0x1E, 0xF000, A_DISK_PARAMS);
    set_vector(0x1F, 0, 0);
    if (at) {
        set_vector(0x70, 0xF000, s_at_int70);
        set_vector(0x71, 0xF000, s_at_int71);
        set_vector(0x72, 0xF000, s_at_irq);
        set_vector(0x73, 0xF000, s_at_irq);
        set_vector(0x74, 0xF000, s_at_irq);
        set_vector(0x75, 0xF000, s_at_int75);
        set_vector(0x76, 0xF000, s_at_irq);
        set_vector(0x77, 0xF000, s_at_irq);
    }
    set_vector(0x40, 0xF000, A_INT13);      /* the diskette service, as a hard disk BIOS moves it */
    set_vector(0x41, 0xF000, A_HD_PARAMS);
    set_vector(0x46, 0xF000, A_HD_PARAMS + 16);

    /* The BIOS data area */
    memset(bda(0), 0, 0x100);
    uint16_t equipment = 0x0001 | ((PC_FLOPPIES - 1) << 6);    /* diskette drives */
    equipment |= pc.cfg.fpu ? 0x0002 : 0;   /* the coprocessor */
    /* MDA, CGA 80 x 25, the EGA and VGA (their own BIOS: 00) */
    equipment |= pc.cfg.video == PC_HERCULES ? 0x30 : pc.cfg.video == PC_CGA ? 0x20 : 0x00;
    equipment |= 0x0200;                    /* one serial port: COM1 */
    if (lpt_present()) {
        equipment |= 0x4000;                /* one printer: LPT1 */
        set_bda16(0x08, 0x378);
        *bda(0x78) = 20;                    /* its time-out */
    }
    set_bda16(0x10, equipment);
    set_bda16(0x13, pc.cfg.conv_kb);
    set_bda16(0x00, 0x3F8);                 /* COM1 */
    set_bda16(0x1A, 0x1E);                  /* the keyboard's buffer */
    set_bda16(0x1C, 0x1E);
    set_bda16(0x80, 0x1E);
    set_bda16(0x82, 0x3E);
    *bda(0x17) = 0x00;                      /* Num Lock off, as IBM's */
    *bda(0x96) = at ? 0x10 : 0x00;          /* a 101-key keyboard (the AT's) */
    struct tm tm;
    uint32_t ticks = 0;
    if (clock_now(&tm)) {
        uint32_t s = (uint32_t)(tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec);
        ticks = (uint32_t)((uint64_t)s * PIT_HZ / 65536);
    }
    set_bda16(0x6C, (uint16_t)ticks);
    set_bda16(0x6E, (uint16_t)(ticks >> 16));
    bios_disk_init();

    /* The interrupt controller, the timer, the keyboard */
    if (at) {
        pic_write(0, 0, 0x11);              /* edge triggered, cascaded, ICW4 */
        pic_write(0, 1, 0x08);              /* vectors 08-0F */
        pic_write(0, 1, 0x04);              /* the second on IRQ 2 */
        pic_write(0, 1, 0x01);
        pic_write(1, 0, 0x11);
        pic_write(1, 1, 0x70);              /* vectors 70-77 */
        pic_write(1, 1, 0x02);
        pic_write(1, 1, 0x01);
        pic_write(0, 1, 0xB8);              /* IRQ 0, 1, 2 (the second), 6 */
        pic_write(1, 1, 0xFF);
    } else {
        pic_write(0, 0, 0x13);              /* edge triggered, single, ICW4 */
        pic_write(0, 1, 0x08);              /* vectors 08-0F */
        pic_write(0, 1, 0x09);              /* buffered, 8086 */
        pic_write(0, 1, 0xBC);              /* IRQ 0, 1 and 6 */
    }
    pit_write(3, 0x36);                     /* the tick: 18.2 Hz */
    pit_write(0, 0x00);
    pit_write(0, 0x00);
    pit_write(3, 0x54);                     /* refresh */
    pit_write(1, 18);
    pit_write(3, 0xB6);                     /* the speaker: 896 Hz */
    pit_write(2, 0x33);
    pit_write(2, 0x05);
    if (!at) {
        ppi_write(3, 0x99);
        ppi_write(1, 0x4C);                 /* keyboard on, speaker off */
    } else {
        kbc_write(4, 0xD1);                 /* the A20 gate closed, as the AT's POST leaves it */
        kbc_write(0, 0xDD);
    }

    bios_keyboard_init();
    bios_video_mode(pc.cfg.video == PC_HERCULES ? 7 : 3);

    /* Its stack, as IBM's */
    x86_load_seg(c, X86_SS, 0x0030);
    c->r.w[X86_SP] = 0x0100;
    x86_load_seg(c, X86_DS, 0x0040);
    x86_load_seg(c, X86_ES, 0x0000);

    if (!warm) {
        char line[120], ext[40] = "";
        if (at && pc.cfg.ext_kb) {
            snprintf(ext, sizeof(ext), " + %u KB extended", (unsigned)pc.cfg.ext_kb);
        }
        snprintf(line, sizeof(line), "esp32pc BIOS   %s, %s at %u.%02u MHz, %u KB%s\r\n\r\n",
                 at ? "PC/AT" : "PC/XT",
                 pc.cfg.cpu == X86_80286 ? "80286" : pc.cfg.cpu == X86_8086 ? "8086" : "8088",
                 (unsigned)(pc.cfg.cpu_hz / 1000000), (unsigned)((pc.cfg.cpu_hz + 5000) / 10000 % 100),
                 pc.cfg.conv_kb, ext);
        print(line);
    }
    set_bda16(0x72, 0);
    s_told_no_boot = false;
}

/* ---- INT 19h: the boot sector to 0000:7C00 ---- */

static void boot(x86_t *c)
{
    int drive = -1;
    if (disks[0].present && disk_read(0, 0, 1, pc.ram + 0x7C00)) {
        drive = 0x00;               /* a diskette's (no signature on DOS 1's) */
    } else if (disks[2].present && disk_read(2, 0, 1, pc.ram + 0x7C00) &&
               pc.ram[0x7DFE] == 0x55 && pc.ram[0x7DFF] == 0xAA) {
        drive = 0x80;
    }
    if (drive < 0) {
        if (!s_told_no_boot) {
            print("No boot disk: put one in A: or C: on the setup screen or the web page.\r\n");
            s_told_no_boot = true;
        }
        return;                     /* the stub waits for an interrupt and tries again */
    }
    s_told_no_boot = false;
    c->r.b[4] = (uint8_t)drive;     /* DL */
    x86_load_seg(c, X86_CS, 0x0000);
    c->ip = 0x7C00;
    c->flags |= X86_IF;             /* as IBM's bootstrap (it starts with STI) */
}

/* ---- INT 1Ah: the time ---- */

static void time_of_day(x86_t *c)
{
    uint8_t ah = c->r.b[1];
    struct tm tm;
    bool ok;
    switch (ah) {
    case 0x00:                      /* the ticks, and midnight's flag */
        c->r.w[X86_CX] = bda16(0x6E);
        c->r.w[X86_DX] = bda16(0x6C);
        c->r.b[0] = *bda(0x70);
        *bda(0x70) = 0;
        break;
    case 0x01:
        set_bda16(0x6E, c->r.w[X86_CX]);
        set_bda16(0x6C, c->r.w[X86_DX]);
        *bda(0x70) = 0;
        break;
    case 0x02:                      /* the clock (the board's), as the AT's or a clock card */
        ok = clock_now(&tm);
        if (ok) {
            c->r.b[3] = bcd(tm.tm_hour);    /* CH */
            c->r.b[2] = bcd(tm.tm_min);     /* CL */
            c->r.b[5] = bcd(tm.tm_sec);     /* DH */
            c->r.b[4] = 0;                  /* DL */
        }
        bios_return_cf(c, !ok);
        break;
    case 0x04:
        ok = clock_now(&tm);
        if (ok) {
            int year = tm.tm_year + 1900;
            c->r.b[3] = bcd(year / 100);    /* CH: century */
            c->r.b[2] = bcd(year % 100);    /* CL */
            c->r.b[5] = bcd(tm.tm_mon + 1); /* DH */
            c->r.b[4] = bcd(tm.tm_mday);    /* DL */
        }
        bios_return_cf(c, !ok);
        break;
    case 0x03:
    case 0x05:                      /* setting it: the board's clock stays */
        bios_return_cf(c, false);
        break;
    default:
        bios_return_cf(c, true);
        break;
    }
}

/* ---- INT 14h: COM1 through its UART (the mouse's port) ---- */

/* INT 17h: LPT1. The status as IBM's BIOS gives it (the port's, acknowledge
 * and error turned round): 90h ready, 10h + 01h busy (a time-out)
 */
static void int17(x86_t *c)
{
    if (c->r.w[X86_DX] != 0 || !lpt_present()) {
        R_AH(c) = 0x30;             /* none: out of paper, selected (as with no adapter) */
        return;
    }
    switch (R_AH(c)) {
    case 0x00:                      /* print AL */
        R_AH(c) = lpt_print(R_AL(c)) ? 0x90 : 0x11;
        break;
    case 0x01:                      /* initialise */
        lpt_reset();
        __attribute__((fallthrough));
    default:                        /* status */
        R_AH(c) = lpt_ready() ? 0x90 : 0x10;
        break;
    }
}

/* INT 05h: the text on the screen to the printer, a line at a time (the
 * cursor and the registers kept). 50:00 as IBM's: 1 printing, 0 done, FFh
 * failed.
 */
static void print_screen(x86_t *c)
{
    if (!lpt_present() || pc.ram[0x500] == 1) {
        return;
    }
    pc.ram[0x500] = 1;
    typeof(c->r) saved = c->r;
    R_AH(c) = 0x0F;                 /* the columns, the page */
    bios_int10(c);
    int cols = R_AH(c), page = c->r.b[7];
    int rows = pc.cfg.video == PC_EGA || pc.cfg.video == PC_VGA ? *bda(0x84) + 1 : 25;
    R_AH(c) = 0x03;
    c->r.b[7] = (uint8_t)page;
    bios_int10(c);
    uint16_t cursor = c->r.w[X86_DX];
    bool ok = lpt_print('\r') && lpt_print('\n');
    for (int y = 0; ok && y < rows; y++) {
        char line[132];
        int n = 0;
        for (int x = 0; x < cols && x < (int)sizeof(line); x++) {
            R_AH(c) = 0x02;
            c->r.b[7] = (uint8_t)page;
            c->r.w[X86_DX] = (uint16_t)(y << 8 | x);
            bios_int10(c);
            R_AH(c) = 0x08;
            bios_int10(c);
            line[x] = R_AL(c) ? (char)R_AL(c) : ' ';
            n = line[x] != ' ' ? x + 1 : n;
        }
        for (int x = 0; ok && x < n; x++) {
            ok = lpt_print((uint8_t)line[x]);
        }
        ok = ok && lpt_print('\r') && lpt_print('\n');
    }
    R_AH(c) = 0x02;
    c->r.b[7] = (uint8_t)page;
    c->r.w[X86_DX] = cursor;
    bios_int10(c);
    c->r = saved;
    pc.ram[0x500] = ok ? 0x00 : 0xFF;
}

static void int14(x86_t *c)
{
    uint8_t v;
    if (c->r.w[X86_DX] != 0) {
        R_AH(c) = 0x80;             /* no COM2-4: time out */
        return;
    }
    switch (R_AH(c)) {
    case 0x00: {                    /* the line: speed, parity, stop bits, word length */
        static const uint16_t divisor[8] = { 1047, 768, 384, 192, 96, 48, 24, 12 };
        uint16_t d = divisor[R_AL(c) >> 5];
        uart_out(0x3FB, 0x80);
        uart_out(0x3F8, (uint8_t)d);
        uart_out(0x3F9, (uint8_t)(d >> 8));
        uart_out(0x3FB, R_AL(c) & 0x1F);
        uart_out(0x3F9, 0x00);
    }
        __attribute__((fallthrough));
    case 0x03:                      /* status */
        uart_in(0x3FD, &v);
        R_AH(c) = v;
        uart_in(0x3FE, &v);
        R_AL(c) = v;
        break;
    case 0x01:                      /* send AL */
        uart_out(0x3F8, R_AL(c));
        uart_in(0x3FD, &v);
        R_AH(c) = v;
        break;
    case 0x02:                      /* receive: what's there, else a time-out */
        uart_in(0x3FD, &v);
        if (v & 0x01) {
            uint8_t b;
            uart_in(0x3F8, &b);
            R_AL(c) = b;
            R_AH(c) = v & 0x1E;
        } else {
            R_AH(c) = 0x80;
        }
        break;
    default:
        break;
    }
}

/* ---- INT 15h: the XT's nothing, the AT's services ---- */

/* AH=87h: words between two linear addresses (the descriptors at ES:SI +
 * 12h and 1Ah), with the A20 gate open meanwhile, as the AT's BIOS does
 */
static void block_move(x86_t *c)
{
    uint32_t gdt = c->seg[X86_ES].base + c->r.w[X86_SI];
    uint32_t src = x86_read16(c, gdt + 0x12) | (uint32_t)x86_read8(c, gdt + 0x14) << 16;
    uint32_t dst = x86_read16(c, gdt + 0x1A) | (uint32_t)x86_read8(c, gdt + 0x1C) << 16;
    uint32_t n = (uint32_t)c->r.w[X86_CX] * 2;
    if (c->r.w[X86_CX] > 0x8000) {
        R_AH(c) = 0x01;
        bios_return_cf(c, true);
        return;
    }
    uint32_t mask = c->addr_mask;
    c->addr_mask = 0xFFFFFF;
    for (uint32_t i = 0; i < n; i++) {
        x86_write8(c, dst + i, x86_read8(c, src + i));
    }
    c->addr_mask = mask;
    R_AH(c) = 0;
    bios_return_cf(c, false);
    bios_return_zf(c, true);
}

/* AH=89h: into protected mode as IBM's BIOS goes: the caller's GDT at
 * ES:SI (08h itself, 10h the IDT, 18h DS, 20h ES, 28h SS, 30h CS; 38h the
 * BIOS's code, filled in here), BH and BL the interrupt controllers' new
 * vectors, the A20 gate open; back at the caller's IP with CS 30h,
 * interrupts off, AH 0
 */
static void enter_protected(x86_t *c)
{
    uint32_t gdt = c->seg[X86_ES].base + c->r.w[X86_SI];
    x86_write16(c, gdt + 0x38, 0xFFFF);             /* F0000h, 64 KB, code */
    x86_write16(c, gdt + 0x3A, 0x0000);
    x86_write8(c, gdt + 0x3C, 0x0F);
    x86_write8(c, gdt + 0x3D, 0x9B);
    x86_write16(c, gdt + 0x3E, 0);
    kbc_set_a20(true);
    uint8_t mask0 = pic_read(0, 1), mask1 = pic_read(1, 1);
    pic_write(0, 0, 0x11);
    pic_write(0, 1, R_BH(c));
    pic_write(0, 1, 0x04);
    pic_write(0, 1, 0x01);
    pic_write(1, 0, 0x11);
    pic_write(1, 1, R_BL(c));
    pic_write(1, 1, 0x02);
    pic_write(1, 1, 0x01);
    pic_write(0, 1, mask0);
    pic_write(1, 1, mask1);
    c->gdtr.limit = x86_read16(c, gdt + 0x08);
    c->gdtr.base = x86_read16(c, gdt + 0x0A) | (uint32_t)x86_read8(c, gdt + 0x0C) << 16;
    c->idtr.limit = x86_read16(c, gdt + 0x10);
    c->idtr.base = x86_read16(c, gdt + 0x12) | (uint32_t)x86_read8(c, gdt + 0x14) << 16;
    uint16_t ip = x86_read16(c, c->seg[X86_SS].base + c->r.w[X86_SP]);    /* INT 15h's frame */
    c->r.w[X86_SP] += 6;
    c->msw |= X86_MSW_PE;
    x86_load_seg(c, X86_DS, 0x18);
    x86_load_seg(c, X86_ES, 0x20);
    x86_load_seg(c, X86_SS, 0x28);
    x86_load_seg(c, X86_CS, 0x30);
    c->ip = ip;
    x86_set_flags(c, (uint16_t)(x86_get_flags(c) & ~(X86_IF | X86_CF)));
    R_AH(c) = 0;
}

static void int15(x86_t *c)
{
    uint8_t ah = R_AH(c);
    bios_wait_result(c, false);
    if (pc.cfg.board != PC_AT) {
        if (ah != 0x4F) {
            R_AH(c) = 0x86;         /* (the cassette's) */
        }
        bios_return_cf(c, true);
        return;
    }
    switch (ah) {
    case 0x4F:                      /* the keyboard intercept: the key goes on */
        bios_return_cf(c, true);
        break;
    case 0x85:                      /* SysReq */
    case 0x90:                      /* device busy */
    case 0x91:                      /* interrupt complete */
        R_AH(c) = 0;
        bios_return_cf(c, false);
        break;
    case 0x86: {                    /* wait CX:DX microseconds */
        if (!s_waiting) {
            uint32_t us = (uint32_t)c->r.w[X86_CX] << 16 | c->r.w[X86_DX];
            s_wait_until = pc_now() + (uint64_t)us * (pc.cfg.cpu_hz / 1000) / 1000;
            s_waiting = true;
        }
        if (pc_now() < s_wait_until) {
            bios_wait_result(c, true);  /* round again */
            return;
        }
        s_waiting = false;
        R_AH(c) = 0;
        bios_return_cf(c, false);
        break;
    }
    case 0x87:
        block_move(c);
        break;
    case 0x88:                      /* extended memory, KB */
        c->r.w[X86_AX] = (uint16_t)(pc.cfg.ext_kb > 0xFFFF ? 0xFFFF : pc.cfg.ext_kb);
        bios_return_cf(c, false);
        break;
    case 0xC0:                      /* the configuration table */
        x86_load_seg(c, X86_ES, 0xF000);
        c->r.w[X86_BX] = A_CONFIG;
        R_AH(c) = 0;
        bios_return_cf(c, false);
        break;
    case 0x89:
        enter_protected(c);
        break;
    default:
        R_AH(c) = 0x86;
        bios_return_cf(c, true);
        break;
    }
}

/* INT 08h's part: the tick count, midnight, the diskette motor's time */
static void tick(void)
{
    uint32_t t = (uint32_t)bda16(0x6C) | (uint32_t)bda16(0x6E) << 16;
    if (++t >= 0x1800B0) {
        t = 0;
        *bda(0x70) = 1;
    }
    set_bda16(0x6C, (uint16_t)t);
    set_bda16(0x6E, (uint16_t)(t >> 16));
    if (*bda(0x40) && --*bda(0x40) == 0) {
        *bda(0x3F) &= 0xF0;         /* the motors off */
    }
}

/* ---- The trap ---- */

void bios_trap(void *ctx, x86_t *c, uint8_t n)
{
    (void)ctx;
    switch (n) {
    case 0x00: post(c); break;
    case 0x05: print_screen(c); break;
    case 0x08: tick(); break;
    case 0x09: bios_int09(c); break;
    case 0x10: bios_int10(c); break;
    case 0x11: c->r.w[X86_AX] = bda16(0x10); break;
    case 0x12: c->r.w[X86_AX] = bda16(0x13); break;
    case 0x13: bios_int13(c); break;
    case 0x14: int14(c); break;
    case 0x15: int15(c); break;
    case 0x16: bios_int16(c); break;
    case 0x17: int17(c); break;
    case 0x18: print("\r\nNo ROM BASIC: press a key to start again.\r\n"); break;
    case 0x19: boot(c); break;
    case 0x1A: time_of_day(c); break;
    case 0x60: bios_vga_init(c); break;     /* the option ROMs' (a real BIOS) */
    case 0x61: hd_rom_init(c); break;
    case 0x62: hd_boot(c); break;
    default: break;
    }
}

void bios_wait_result(x86_t *c, bool wait)
{
    live_cf(c, wait);
}
