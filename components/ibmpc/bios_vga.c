/*
 * The BIOS's INT 10h for the EGA and VGA, and their video ROM
 *
 * The video ROM at C0000 (made at each reset): the option ROM's header,
 * "IBM" where programs look for it (C000:001E), the fonts (8x8, 8x14, the
 * VGA's 8x16), the video parameter table in the cards' 64-byte format (the
 * VGA's 29 entries, the EGA's first 23) that 40:A8's save pointer table
 * points to. Setting a mode programs the card from that table, as the
 * cards' BIOSes do (so programs that read it see the BIOS's own values),
 * loads the DAC (the VGA), clears the memory, and puts the text font into
 * plane 2.
 *
 * Text goes into planes 0 and 1, graphics characters are drawn from the
 * font INT 43h points to; the planar modes (0Dh-12h), the VGA's mode 13h
 * and the CGA's modes 4-6 (through the card's own logic) each their way.
 * The EGA's and VGA's own functions: the palette and the DAC (AH=10h), the
 * character generator (11h), alternate select (12h: the card's
 * information, the text modes' scan lines), the display combination
 * (1Ah), the state (1Bh), save and restore (1Ch).
 */
#include "pc_internal.h"
#include "video.h"

#define ROM         0xC0000
#define ROM_INT10   0x0008          /* INT 10h's entry (with a real BIOS) */
#define ROM_PARAMS  0x0400          /* the video parameter table: 29 entries of 64 bytes */
#define ROM_SAVE    0x0200          /* the save pointer table */
#define ROM_STATIC  0x0240          /* AH=1Bh's static functionality table */
#define ROM_FONT8   0x1000
#define ROM_FONT14  0x1800
#define ROM_FONT16  0x2600
#define ROM_ALT     0x3600          /* the 9-dot alternates: none (0) */

uint8_t *vga_planes(void);
void vga_set_dac(int i, uint8_t red, uint8_t green, uint8_t blue);
const vga_regs_t *vga_regs(void);

static bool is_vga(void)
{
    return pc.cfg.video == PC_VGA;
}

/* ---- The video parameter table ---- */

typedef struct {
    uint8_t entry;
    uint8_t cols, rows1, height;
    uint16_t page;
    uint8_t seq[4], misc;
    uint8_t crtc[25];
    uint8_t attr[20];
    uint8_t gc[9];
} params_t;

#define TEXT_ATTR   { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F }
#define GC_TEXT     { 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF }
#define GC_PLANAR   { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x0F, 0xFF }
#define ATTR16_200  { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, \
                      0x01, 0x00, 0x0F, 0x00 }
#define ATTR16      { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, \
                      0x01, 0x00, 0x0F, 0x00 }
#define ATTR_CGA4   { 0x00, 0x13, 0x15, 0x17, 0x02, 0x04, 0x06, 0x07, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, \
                      0x01, 0x00, 0x03, 0x00 }
#define ATTR_CGA2   { 0x00, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, \
                      0x01, 0x00, 0x01, 0x00 }

static const params_t s_vga_params[] = {
    { 0x00, 40, 24, 8, 0x0800, { 0x09, 0x03, 0x00, 0x02 }, 0x63,
      { 0x2D, 0x27, 0x28, 0x90, 0x2B, 0xA0, 0xBF, 0x1F, 0x00, 0xC7, 0x06, 0x07, 0x00, 0x00, 0x00, 0x00,
        0x9C, 0x8E, 0x8F, 0x14, 0x1F, 0x96, 0xB9, 0xA3, 0xFF },
      { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x0C, 0x00, 0x0F, 0x08 }, GC_TEXT },
    { 0x02, 80, 24, 8, 0x1000, { 0x01, 0x03, 0x00, 0x02 }, 0x63,
      { 0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F, 0x00, 0xC7, 0x06, 0x07, 0x00, 0x00, 0x00, 0x00,
        0x9C, 0x8E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3, 0xFF },
      { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x0C, 0x00, 0x0F, 0x08 }, GC_TEXT },
    { 0x04, 40, 24, 8, 0x4000, { 0x09, 0x03, 0x00, 0x02 }, 0x63,
      { 0x2D, 0x27, 0x28, 0x90, 0x2B, 0x80, 0xBF, 0x1F, 0x00, 0xC1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x9C, 0x8E, 0x8F, 0x14, 0x00, 0x96, 0xB9, 0xA2, 0xFF },
      ATTR_CGA4, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x0F, 0x00, 0xFF } },
    { 0x06, 80, 24, 8, 0x4000, { 0x01, 0x01, 0x00, 0x06 }, 0x63,
      { 0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, 0x00, 0xC1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x9C, 0x8E, 0x8F, 0x28, 0x00, 0x96, 0xB9, 0xC2, 0xFF },
      ATTR_CGA2, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0D, 0x00, 0xFF } },
    { 0x0D, 40, 24, 8, 0x2000, { 0x09, 0x0F, 0x00, 0x06 }, 0x63,
      { 0x2D, 0x27, 0x28, 0x90, 0x2B, 0x80, 0xBF, 0x1F, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x9C, 0x8E, 0x8F, 0x14, 0x00, 0x96, 0xB9, 0xE3, 0xFF },
      ATTR16_200, GC_PLANAR },
    { 0x0E, 80, 24, 8, 0x4000, { 0x01, 0x0F, 0x00, 0x06 }, 0x63,
      { 0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x9C, 0x8E, 0x8F, 0x28, 0x00, 0x96, 0xB9, 0xE3, 0xFF },
      ATTR16_200, GC_PLANAR },
    { 0x12, 80, 24, 14, 0x8000, { 0x01, 0x0F, 0x00, 0x06 }, 0xA3,
      { 0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x83, 0x85, 0x5D, 0x28, 0x0F, 0x63, 0xBA, 0xE3, 0xFF },
      ATTR16, GC_PLANAR },
    { 0x13, 40, 24, 14, 0x0800, { 0x09, 0x03, 0x00, 0x02 }, 0xA3,
      { 0x2D, 0x27, 0x28, 0x90, 0x2B, 0xA0, 0xBF, 0x1F, 0x00, 0x4D, 0x0B, 0x0C, 0x00, 0x00, 0x00, 0x00,
        0x83, 0x85, 0x5D, 0x14, 0x1F, 0x63, 0xBA, 0xA3, 0xFF },
      { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
        0x08, 0x00, 0x0F, 0x00 }, GC_TEXT },
    { 0x15, 80, 24, 14, 0x1000, { 0x01, 0x03, 0x00, 0x02 }, 0xA3,
      { 0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F, 0x00, 0x4D, 0x0B, 0x0C, 0x00, 0x00, 0x00, 0x00,
        0x83, 0x85, 0x5D, 0x28, 0x1F, 0x63, 0xBA, 0xA3, 0xFF },
      { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
        0x08, 0x00, 0x0F, 0x00 }, GC_TEXT },
    { 0x17, 40, 24, 16, 0x0800, { 0x08, 0x03, 0x00, 0x02 }, 0x67,
      { 0x2D, 0x27, 0x28, 0x90, 0x2B, 0xA0, 0xBF, 0x1F, 0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x00,
        0x9C, 0x8E, 0x8F, 0x14, 0x1F, 0x96, 0xB9, 0xA3, 0xFF },
      { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
        0x0C, 0x00, 0x0F, 0x08 }, GC_TEXT },
    { 0x18, 80, 24, 16, 0x1000, { 0x00, 0x03, 0x00, 0x02 }, 0x67,
      { 0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F, 0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x00,
        0x9C, 0x8E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3, 0xFF },
      { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
        0x0C, 0x00, 0x0F, 0x08 }, GC_TEXT },
    { 0x19, 80, 24, 16, 0x1000, { 0x00, 0x03, 0x00, 0x02 }, 0x66,
      { 0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F, 0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x00,
        0x9C, 0x8E, 0x8F, 0x28, 0x0F, 0x96, 0xB9, 0xA3, 0xFF },
      { 0x00, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x10, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
        0x0E, 0x00, 0x0F, 0x08 }, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0A, 0x00, 0xFF } },
    { 0x1A, 80, 29, 16, 0xA000, { 0x01, 0x0F, 0x00, 0x06 }, 0xE3,
      { 0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0x0B, 0x3E, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xEA, 0x8C, 0xDF, 0x28, 0x00, 0xE7, 0x04, 0xC3, 0xFF },
      { 0x00, 0x3F, 0x00, 0x3F, 0x00, 0x3F, 0x00, 0x3F, 0x00, 0x3F, 0x00, 0x3F, 0x00, 0x3F, 0x00, 0x3F,
        0x01, 0x00, 0x0F, 0x00 }, GC_PLANAR },
    { 0x1B, 80, 29, 16, 0xA000, { 0x01, 0x0F, 0x00, 0x06 }, 0xE3,
      { 0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0x0B, 0x3E, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xEA, 0x8C, 0xDF, 0x28, 0x00, 0xE7, 0x04, 0xE3, 0xFF },
      ATTR16, GC_PLANAR },
    { 0x1C, 40, 24, 8, 0x0000, { 0x01, 0x0F, 0x00, 0x0E }, 0x63,
      { 0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, 0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x9C, 0x8E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3, 0xFF },
      { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
        0x41, 0x00, 0x0F, 0x00 }, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF } },
};

/* The EGA with the Enhanced Color Display: 350-line text */
static const params_t s_ega_params[] = {
    { 0x04, 40, 24, 8, 0x4000, { 0x0B, 0x03, 0x00, 0x02 }, 0x23,
      { 0x37, 0x27, 0x2D, 0x37, 0x30, 0x14, 0x04, 0x11, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xE1, 0x24, 0xC7, 0x14, 0x00, 0xE0, 0xF0, 0xA2, 0xFF },
      ATTR_CGA4, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x0F, 0x00, 0xFF } },
    { 0x06, 80, 24, 8, 0x4000, { 0x01, 0x01, 0x00, 0x06 }, 0x23,
      { 0x70, 0x4F, 0x59, 0x2D, 0x5E, 0x06, 0x04, 0x11, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xE0, 0x23, 0xC7, 0x28, 0x00, 0xDF, 0xEF, 0xC2, 0xFF },
      ATTR_CGA2, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0D, 0x00, 0xFF } },
    { 0x0D, 40, 24, 8, 0x2000, { 0x0B, 0x0F, 0x00, 0x06 }, 0x23,
      { 0x37, 0x27, 0x2D, 0x37, 0x31, 0x15, 0x04, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xE1, 0x24, 0xC7, 0x14, 0x00, 0xE0, 0xF0, 0xE3, 0xFF },
      ATTR16_200, GC_PLANAR },
    { 0x0E, 80, 24, 8, 0x4000, { 0x01, 0x0F, 0x00, 0x06 }, 0x23,
      { 0x70, 0x4F, 0x59, 0x2D, 0x5E, 0x06, 0x04, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xE0, 0x23, 0xC7, 0x28, 0x00, 0xDF, 0xEF, 0xE3, 0xFF },
      ATTR16_200, GC_PLANAR },
    { 0x12, 80, 24, 14, 0x8000, { 0x01, 0x0F, 0x00, 0x06 }, 0xA7,
      { 0x5B, 0x4F, 0x53, 0x37, 0x52, 0x00, 0x6C, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x5E, 0x2B, 0x5D, 0x28, 0x0F, 0x5F, 0x0A, 0xE3, 0xFF },
      ATTR16, GC_PLANAR },
    { 0x13, 40, 24, 14, 0x0800, { 0x0B, 0x03, 0x00, 0x03 }, 0xA7,
      { 0x2D, 0x27, 0x2B, 0x2D, 0x28, 0x6D, 0x6C, 0x1F, 0x00, 0x0D, 0x0B, 0x0C, 0x00, 0x00, 0x00, 0x00,
        0x5E, 0x2B, 0x5D, 0x14, 0x0F, 0x5E, 0x0A, 0xA3, 0xFF },
      { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
        0x08, 0x00, 0x0F, 0x00 }, GC_TEXT },
    { 0x15, 80, 24, 14, 0x1000, { 0x01, 0x03, 0x00, 0x03 }, 0xA7,
      { 0x5B, 0x4F, 0x53, 0x37, 0x51, 0x5B, 0x6C, 0x1F, 0x00, 0x0D, 0x0B, 0x0C, 0x00, 0x00, 0x00, 0x00,
        0x5E, 0x2B, 0x5D, 0x28, 0x0F, 0x5E, 0x0A, 0xA3, 0xFF },
      { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
        0x08, 0x00, 0x0F, 0x00 }, GC_TEXT },
};

#define COUNT(a)    ((int)(sizeof(a) / sizeof((a)[0])))

/* An entry as the ROM's table has it (the 64-byte format) */
static void put_params(uint8_t *t, const params_t *p)
{
    t[0] = p->cols;
    t[1] = p->rows1;
    t[2] = p->height;
    t[3] = (uint8_t)p->page;
    t[4] = (uint8_t)(p->page >> 8);
    memcpy(t + 5, p->seq, 4);
    t[9] = p->misc;
    memcpy(t + 10, p->crtc, 25);
    memcpy(t + 35, p->attr, 20);
    memcpy(t + 55, p->gc, 9);
}

/* Entries for the same layout under more than one number */
static void put_table(uint8_t *table)
{
    const params_t *list = is_vga() ? s_vga_params : s_ega_params;
    int n = is_vga() ? COUNT(s_vga_params) : COUNT(s_ega_params);
    for (int i = 0; i < n; i++) {
        put_params(table + list[i].entry * 64, &list[i]);
        uint8_t twin = list[i].entry;
        /* 0/1, 2/3, 4/5, 0*-1*, 2*-3*: the second of a pair the same */
        if (twin == 0x00 || twin == 0x02 || twin == 0x04 || twin == 0x13 || twin == 0x15) {
            put_params(table + (twin + 1) * 64, &list[i]);
        }
    }
}

static const uint8_t *params(int entry)
{
    return pc.ram + ROM + ROM_PARAMS + entry * 64;
}

/* ---- The ROM ---- */

static void put_vector(int n, uint16_t seg, uint16_t off)
{
    pc.ram[n * 4] = (uint8_t)off;
    pc.ram[n * 4 + 1] = (uint8_t)(off >> 8);
    pc.ram[n * 4 + 2] = (uint8_t)seg;
    pc.ram[n * 4 + 3] = (uint8_t)(seg >> 8);
}

void bios_vga_rom(void)
{
    uint8_t *r = pc.ram + ROM;
    memset(r, 0, 0x8000);
    static const uint8_t code[] = {
        0x55, 0xAA, 0x40,                   /* 32 KB */
        0xFE, 0x38, 0x60, 0xCB,             /* 0003: its initialisation (bios_vga_init()) */
        0x90,
        0xFB, 0xFE, 0x38, 0x10, 0xCF,       /* 0008: INT 10h */
    };
    memcpy(r, code, sizeof(code));
    memcpy(r + 0x1E, "IBM", 3);
    static const char name[] = "esp32pc EGA/VGA BIOS (built in)";
    memcpy(r + 0x30, name, sizeof(name));
    put_table(r + ROM_PARAMS);
    /* the save pointer table: the parameter table, the rest none */
    r[ROM_SAVE] = (uint8_t)ROM_PARAMS;
    r[ROM_SAVE + 1] = (uint8_t)(ROM_PARAMS >> 8);
    r[ROM_SAVE + 2] = 0x00;
    r[ROM_SAVE + 3] = 0xC0;
    memcpy(r + ROM_FONT8, pc_font_cga8, sizeof(pc_font_cga8));
    memcpy(r + ROM_FONT14, pc_font_ega14, sizeof(pc_font_ega14));
    if (is_vga()) {
        memcpy(r + ROM_FONT16, pc_font_vga16, sizeof(pc_font_vga16));
    }
    /* AH=1Bh's static table: modes 0-7, 0D-0F, 10-13; 200/350/400 lines */
    static const uint8_t statics[16] = { 0xFF, 0xE0, 0x0F, 0, 0, 0, 0, 0x07, 0x08, 0x02, 0xFF, 0x0E, 0, 0, 0x3F, 0 };
    memcpy(r + ROM_STATIC, statics, sizeof(statics));
    if (!is_vga()) {
        r[ROM_STATIC + 2] = 0x03;   /* (no 11h-13h) */
        r[ROM_STATIC + 7] = 0x03;
        r[ROM_STATIC + 8] = 0x04;
    }
    bios_rom_checksum(r, 0x8000);
}

/* A real BIOS's POST found the ROM: INT 10h ours (its own to INT 42h, as
 * the cards' BIOSes leave it), the equipment word's display as 80 x 25
 * colour (as they set it), mode 3
 */
void bios_vga_init(x86_t *c)
{
    (void)c;
    memcpy(pc.ram + 0x42 * 4, pc.ram + 0x10 * 4, 4);
    put_vector(0x10, ROM >> 4, ROM_INT10);
    *bda(0x10) = (uint8_t)((*bda(0x10) & ~0x30) | 0x20);
    bios_vga_mode(3);
}

/* ---- The card's registers ---- */

static uint16_t crtc_port(void)
{
    return (vga_regs()->misc & 1) ? 0x3D4 : 0x3B4;
}

static void crtc(int reg, uint8_t v)
{
    video_out(crtc_port(), (uint8_t)reg);
    video_out((uint16_t)(crtc_port() + 1), v);
}

static void seq(int reg, uint8_t v)
{
    video_out(0x3C4, (uint8_t)reg);
    video_out(0x3C5, v);
}

static void gc(int reg, uint8_t v)
{
    video_out(0x3CE, (uint8_t)reg);
    video_out(0x3CF, v);
}

static void attr(int reg, uint8_t v)
{
    uint8_t dummy;
    video_in((uint16_t)(crtc_port() + 6), &dummy);  /* the flip-flop to the index */
    video_out(0x3C0, (uint8_t)reg);
    video_out(0x3C0, v);
    video_out(0x3C0, 0x20);         /* the picture on again */
}

/* ---- Modes ---- */

enum { K_TEXT, K_CGA4, K_CGA2, K_PLANAR, K_PLANAR1, K_256 };

static int mode(void)
{
    return *bda(0x49);
}

static int kind(void)
{
    switch (mode()) {
    case 4: case 5: return K_CGA4;
    case 6: return K_CGA2;
    case 0x0D: case 0x0E: case 0x10: case 0x12: return K_PLANAR;
    case 0x0F: case 0x11: return K_PLANAR1;
    case 0x13: return K_256;
    default: return K_TEXT;
    }
}

static int columns(void)
{
    return bda16(0x4A);
}

static int rows(void)
{
    return *bda(0x84) + 1;
}

static int char_height(void)
{
    return bda16(0x85);
}

/* The text modes' scan lines for the next mode set (AH=12h BL=30h): 0 200, 1 350, 2 400 */
static int s_scan_lines = 2;

static int entry_of(int m)
{
    if (is_vga()) {
        switch (m) {
        case 0: case 1: return s_scan_lines == 0 ? 0x00 : s_scan_lines == 1 ? 0x13 : 0x17;
        case 2: case 3: return s_scan_lines == 0 ? 0x02 : s_scan_lines == 1 ? 0x15 : 0x18;
        case 4: case 5: case 6: return m;
        case 7: return 0x19;
        case 0x0D: case 0x0E: return m;
        case 0x10: return 0x12;
        case 0x11: return 0x1A;
        case 0x12: return 0x1B;
        case 0x13: return 0x1C;
        default: return -1;
        }
    }
    switch (m) {
    case 0: case 1: return 0x13;
    case 2: case 3: return 0x15;
    case 4: case 5: case 6: return m;
    case 0x0D: case 0x0E: return m;
    case 0x10: return 0x12;
    default: return -1;
    }
}

/* The DAC as the VGA's BIOS loads it: mode 13h's 256 colours, the
 * monochrome greys, the 200-line modes' 16 CGA colours (bit 4 the
 * intensity, as the EGA reads its attributes there), else the EGA's 64
 */
static void load_dac(int m, int entry)
{
    if (!is_vga()) {
        return;
    }
    if (m == 0x13) {
        /* 0-15 the EGA's colours, 16-31 greys, then three rings of 24 hues
         * (bright, medium, dark; each saturated, half, a quarter), black
         */
        static const uint8_t ega16[16] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
                                          0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F };
        for (int i = 0; i < 16; i++) {
            int e = ega16[i];
            vga_set_dac(i, (uint8_t)(((e >> 2) & 1) * 0x2A + ((e >> 5) & 1) * 0x15),
                        (uint8_t)(((e >> 1) & 1) * 0x2A + ((e >> 4) & 1) * 0x15),
                        (uint8_t)((e & 1) * 0x2A + ((e >> 3) & 1) * 0x15));
        }
        static const uint8_t grey[16] = { 0, 5, 8, 11, 14, 17, 20, 24, 28, 32, 36, 40, 45, 50, 56, 63 };
        for (int i = 0; i < 16; i++) {
            vga_set_dac(16 + i, grey[i], grey[i], grey[i]);
        }
        static const uint8_t top[3] = { 63, 28, 16 };
        int n = 32;
        for (int level = 0; level < 3; level++) {
            for (int sat = 0; sat < 3; sat++) {
                int hi = top[level], lo = hi * (sat == 0 ? 0 : sat == 1 ? 2 : 3) / 4;
                for (int h = 0; h < 24; h++) {
                    int part = h % 4, sector = h / 4;
                    int up = lo + (hi - lo) * part / 4, down = hi - (hi - lo) * part / 4;
                    int rgb[3];
                    switch (sector) {
                    case 0: rgb[0] = up; rgb[1] = lo; rgb[2] = hi; break;      /* blue to magenta */
                    case 1: rgb[0] = hi; rgb[1] = lo; rgb[2] = down; break;    /* to red */
                    case 2: rgb[0] = hi; rgb[1] = up; rgb[2] = lo; break;      /* to yellow */
                    case 3: rgb[0] = down; rgb[1] = hi; rgb[2] = lo; break;    /* to green */
                    case 4: rgb[0] = lo; rgb[1] = hi; rgb[2] = up; break;      /* to cyan */
                    default: rgb[0] = lo; rgb[1] = down; rgb[2] = hi; break;   /* to blue */
                    }
                    vga_set_dac(n++, (uint8_t)rgb[0], (uint8_t)rgb[1], (uint8_t)rgb[2]);
                }
            }
        }
        while (n < 256) {
            vga_set_dac(n++, 0, 0, 0);
        }
        return;
    }
    if (m == 7) {
        for (int i = 0; i < 64; i++) {
            uint8_t g = (i & 0x10) ? ((i & 0x08) ? 0x3F : 0x2A) : ((i & 0x08) ? 0x2A : 0);
            vga_set_dac(i, g, g, g);
        }
        return;
    }
    if (entry <= 0x06 || entry == 0x0D || entry == 0x0E) {
        for (int i = 0; i < 64; i++) {
            int k = (i & 7) | ((i & 0x10) ? 8 : 0);
            uint8_t lo = (k & 8) ? 0x15 : 0;
            vga_set_dac(i, (uint8_t)(((k >> 2) & 1) * 0x2A + lo),
                        (uint8_t)(((k >> 1) & 1) * (k == 6 ? 0x15 : 0x2A) + (k == 6 ? 0 : lo)),
                        (uint8_t)((k & 1) * 0x2A + lo));
        }
        return;
    }
    /* the EGA's 64 colours (rgbRGB), as the 16-colour modes' palettes pick them */
    for (int i = 0; i < 64; i++) {
        vga_set_dac(i, (uint8_t)(((i >> 2) & 1) * 0x2A + ((i >> 5) & 1) * 0x15),
                    (uint8_t)(((i >> 1) & 1) * 0x2A + ((i >> 4) & 1) * 0x15),
                    (uint8_t)((i & 1) * 0x2A + ((i >> 3) & 1) * 0x15));
    }
}

/* A font into plane 2: count characters from `first`, `height` rows each,
 * in block `block`
 */
static void load_font(const uint8_t *font, int first, int count, int height, int block)
{
    uint8_t *v = vga_planes();
    uint32_t base = is_vga() ? (block < 4 ? (uint32_t)block * 0x4000 : (uint32_t)(block - 4) * 0x4000 + 0x2000)
                             : (uint32_t)(block & 3) * 0x4000;
    for (int ch = 0; ch < count; ch++) {
        for (int row = 0; row < 32; row++) {
            uint32_t o = (base + (uint32_t)(first + ch) * 32 + (uint32_t)row) & 0xFFFF;
            v[o * 4 + 2] = row < height ? font[ch * height + row] : 0;
        }
    }
}

static const uint8_t *rom_font(int height)
{
    return pc.ram + ROM + (height == 16 && is_vga() ? ROM_FONT16 : height == 14 ? ROM_FONT14 : ROM_FONT8);
}

/* The graphics characters' font for the mode (INT 43h; INT 1Fh the 8x8's upper half) */
static void graphics_font(int height)
{
    uint16_t off = height == 16 && is_vga() ? ROM_FONT16 : height == 14 ? ROM_FONT14 : ROM_FONT8;
    put_vector(0x43, 0xC000, off);
    put_vector(0x1F, 0xC000, ROM_FONT8 + 128 * 8);
}

void bios_vga_mode(int m)
{
    bool keep = m & 0x80;
    m &= 0x7F;
    int e = entry_of(m);
    if (e < 0) {
        m = 3;
        e = entry_of(3);
    }
    const uint8_t *t = params(e);
    seq(1, (uint8_t)(t[5] | 0x20));             /* the picture off meanwhile */
    video_out(0x3C2, t[9]);
    seq(0, 0x01);
    for (int i = 0; i < 4; i++) {
        seq(1 + i, t[5 + i]);
    }
    seq(0, 0x03);
    crtc(0x11, 0x00);                           /* unprotected */
    for (int i = 0; i < 25; i++) {
        crtc(i, t[10 + i]);
    }
    for (int i = 0; i < 20; i++) {
        attr(i, t[35 + i]);
    }
    if (is_vga()) {
        attr(0x14, 0x00);
    }
    for (int i = 0; i < 9; i++) {
        gc(i, t[55 + i]);
    }
    load_dac(m, e);
    video_out(0x3C6, 0xFF);

    uint8_t *v = vga_planes();
    bool text = m <= 3 || m == 7;
    if (!keep && v) {
        if (text) {
            for (uint32_t o = 0; o < 0x8000; o += 2) {
                v[o * 4] = 0x20;
                v[o * 4 + 1] = 0x07;
            }
        } else {
            memset(v, 0, 0x40000);
        }
    }
    int height = t[2];
    if (text && v) {
        load_font(rom_font(height), 0, 256, height, 0);
    }
    graphics_font(text ? 8 : height);

    *bda(0x49) = (uint8_t)m;
    set_bda16(0x4A, t[0]);
    set_bda16(0x4C, (uint16_t)(t[3] | t[4] << 8));
    set_bda16(0x4E, 0);
    memset(bda(0x50), 0, 16);
    uint8_t cs = (uint8_t)(height - 3), ce = (uint8_t)(height - 2);
    set_bda16(0x60, text ? (uint16_t)(cs << 8 | ce) : 0);
    *bda(0x62) = 0;
    set_bda16(0x63, crtc_port());
    static const uint8_t cga_mode[8] = { 0x2C, 0x28, 0x2D, 0x29, 0x2A, 0x2E, 0x1E, 0x29 };
    *bda(0x65) = m < 8 ? cga_mode[m] : 0x29;
    *bda(0x66) = m == 6 ? 0x3F : 0x30;
    *bda(0x84) = t[1];
    set_bda16(0x85, (uint16_t)height);
    *bda(0x87) = 0x60;              /* 256 KB, the card active, colour */
    *bda(0x88) = 0x09;              /* the Enhanced Color Display's switches */
    *bda(0x89) = (uint8_t)(is_vga() ? (0x01 | (s_scan_lines == 2 ? 0x10 : 0) | (s_scan_lines == 0 ? 0x80 : 0)) : 0);
    *bda(0x8A) = is_vga() ? 0x08 : 0;
    set_bda16(0xA8, ROM_SAVE);
    set_bda16(0xAA, 0xC000);
    if (text) {
        crtc(10, cs);
        crtc(11, ce);
    }
    crtc(14, 0);
    crtc(15, 0);
}

/* ---- Text ---- */

static bool text_mode(void)
{
    return kind() == K_TEXT;
}

/* A text cell's character (plane 0) and attribute (plane 1) */
static uint8_t *cell(int page, int row, int col)
{
    uint32_t off = ((uint32_t)page * bda16(0x4C) + (uint32_t)(row * columns() + col) * 2) & 0x7FFE;
    return vga_planes() + off * 4;
}

static void update_cursor(int page)
{
    if (page != *bda(0x62)) {
        return;
    }
    uint16_t pos = bda16(0x50 + page * 2);
    uint32_t at = bda16(0x4E) / 2 + (uint32_t)((pos >> 8) * columns() + (pos & 0xFF));
    crtc(14, (uint8_t)(at >> 8));
    crtc(15, (uint8_t)at);
}

static void set_cursor(int page, int row, int col)
{
    set_bda16(0x50 + page * 2, (uint16_t)(row << 8 | col));
    update_cursor(page);
}

/* ---- Graphics ---- */

static int bytes_per_line(void)
{
    switch (kind()) {
    case K_256: return 320;
    case K_CGA4: case K_CGA2: return 80;
    default: return columns();      /* 8 pixels a byte, a byte a character column */
    }
}

static uint32_t page_base(int page)
{
    return (uint32_t)page * bda16(0x4C);
}

/* The CGA modes' bytes, through the card (B8000, its odd/even) */
static uint8_t cga_get(uint32_t off)
{
    return video_mem_read(0xB8000 + (off & 0x3FFF));
}

static void cga_put(uint32_t off, uint8_t b)
{
    video_mem_write(0xB8000 + (off & 0x3FFF), b);
}

static uint32_t cga_line(int y)
{
    return ((uint32_t)(y & 1) << 13) + (uint32_t)(y >> 1) * 80;
}

static void pixel_write(int page, int x, int y, uint8_t color)
{
    uint8_t *v = vga_planes();
    bool xor = color & 0x80;
    switch (kind()) {
    case K_256: {
        if (x < 0 || x >= 320 || y < 0 || y >= 200) {
            return;
        }
        uint32_t a = (uint32_t)y * 320 + (uint32_t)x;
        v[a] = xor ? (uint8_t)(v[a] ^ color) : color;
        break;
    }
    case K_CGA4: {
        if (x < 0 || x >= 320 || y < 0 || y >= 200) {
            return;
        }
        uint32_t o = cga_line(y) + (uint32_t)(x >> 2);
        int sh = (3 - (x & 3)) * 2;
        uint8_t b = cga_get(o), px = (uint8_t)((color & 3) << sh);
        cga_put(o, xor ? (uint8_t)(b ^ px) : (uint8_t)((b & ~(3 << sh)) | px));
        break;
    }
    case K_CGA2: {
        if (x < 0 || x >= 640 || y < 0 || y >= 200) {
            return;
        }
        uint32_t o = cga_line(y) + (uint32_t)(x >> 3);
        uint8_t bit = (uint8_t)(0x80 >> (x & 7)), b = cga_get(o);
        cga_put(o, xor ? (uint8_t)(b ^ ((color & 1) ? bit : 0)) : (uint8_t)((b & ~bit) | ((color & 1) ? bit : 0)));
        break;
    }
    default: {
        int width = columns() * 8, height = rows() * char_height();
        if (x < 0 || x >= width || y < 0 || y >= height) {
            return;
        }
        uint32_t o = (page_base(page) + (uint32_t)y * (uint32_t)bytes_per_line() + (uint32_t)(x >> 3)) & 0xFFFF;
        uint8_t bit = (uint8_t)(0x80 >> (x & 7));
        int planes = kind() == K_PLANAR1 ? 1 : 4;
        for (int p = 0; p < planes; p++) {
            uint8_t *b = &v[o * 4 + p];
            bool on = (color >> p) & 1;
            if (xor) {
                *b ^= on ? bit : 0;
            } else {
                *b = (uint8_t)(on ? (*b | bit) : (*b & ~bit));
            }
        }
        break;
    }
    }
}

static uint8_t pixel_read(int page, int x, int y)
{
    uint8_t *v = vga_planes();
    switch (kind()) {
    case K_256:
        return x >= 0 && x < 320 && y >= 0 && y < 200 ? v[(uint32_t)y * 320 + (uint32_t)x] : 0;
    case K_CGA4:
        return x >= 0 && x < 320 && y >= 0 && y < 200
                   ? (uint8_t)((cga_get(cga_line(y) + (uint32_t)(x >> 2)) >> ((3 - (x & 3)) * 2)) & 3) : 0;
    case K_CGA2:
        return x >= 0 && x < 640 && y >= 0 && y < 200
                   ? (uint8_t)((cga_get(cga_line(y) + (uint32_t)(x >> 3)) >> (7 - (x & 7))) & 1) : 0;
    default: {
        uint32_t o = (page_base(page) + (uint32_t)y * (uint32_t)bytes_per_line() + (uint32_t)(x >> 3)) & 0xFFFF;
        uint8_t c = 0;
        int planes = kind() == K_PLANAR1 ? 1 : 4;
        for (int p = 0; p < planes; p++) {
            c |= (uint8_t)(((v[o * 4 + p] >> (7 - (x & 7))) & 1) << p);
        }
        return c;
    }
    }
}

/* The graphics characters' font (INT 43h) */
static const uint8_t *glyph(uint8_t ch, int height)
{
    uint16_t off = (uint16_t)(pc.ram[0x43 * 4] | pc.ram[0x43 * 4 + 1] << 8);
    uint16_t seg = (uint16_t)(pc.ram[0x43 * 4 + 2] | pc.ram[0x43 * 4 + 3] << 8);
    uint32_t a = ((uint32_t)seg << 4) + off + (uint32_t)ch * (uint32_t)height;
    return a + (uint32_t)height <= 0x100000 ? pc.ram + a : pc.ram + ROM + ROM_FONT8;
}

static void gfx_char(int page, int row, int col, uint8_t ch, uint8_t color)
{
    int h = char_height();
    int cw = 8;
    const uint8_t *g = glyph(ch, h);
    bool xor = color & 0x80;
    if (kind() == K_PLANAR || kind() == K_PLANAR1) {
        /* a byte a column: each plane its colour bit's glyph */
        uint8_t *v = vga_planes();
        int planes = kind() == K_PLANAR1 ? 1 : 4;
        for (int i = 0; i < h; i++) {
            uint32_t o = (page_base(page) + (uint32_t)(row * h + i) * (uint32_t)bytes_per_line() + (uint32_t)col)
                         & 0xFFFF;
            for (int p = 0; p < planes; p++) {
                uint8_t bits = ((color >> p) & 1) ? g[i] : 0;
                v[o * 4 + p] = xor ? (uint8_t)(v[o * 4 + p] ^ bits) : bits;
            }
        }
        return;
    }
    for (int i = 0; i < h; i++) {
        for (int k = 0; k < cw; k++) {
            int x = col * cw + k, y = row * h + i;
            bool on = (g[i] >> (7 - k)) & 1;
            if (xor) {
                if (on) {
                    pixel_write(page, x, y, color);
                }
            } else {
                pixel_write(page, x, y, on ? color : (kind() == K_256 ? 0 : 0));
            }
        }
    }
}

static uint8_t gfx_read_char(int page, int row, int col)
{
    int h = char_height();
    for (int ch = 0; ch < 256; ch++) {
        const uint8_t *g = glyph((uint8_t)ch, h);
        bool same = true;
        for (int i = 0; i < h && same; i++) {
            uint8_t bits = 0;
            for (int k = 0; k < 8; k++) {
                bits = (uint8_t)(bits << 1 | (pixel_read(page, col * 8 + k, row * h + i) ? 1 : 0));
            }
            same = bits == g[i];
        }
        if (same) {
            return (uint8_t)ch;
        }
    }
    return 0;
}

/* ---- Scrolling ---- */

static void scroll(bool up, int lines, uint8_t attr_v, int r0, int c0, int r1, int c1)
{
    int cols = text_mode() ? columns() : (kind() == K_256 ? 40 : kind() == K_CGA4 ? 40 : columns());
    if (r1 >= rows()) {
        r1 = rows() - 1;
    }
    if (c1 >= cols) {
        c1 = cols - 1;
    }
    if (r0 > r1 || c0 > c1) {
        return;
    }
    int height = r1 - r0 + 1;
    if (lines == 0 || lines > height) {
        lines = height;
    }
    int page = *bda(0x62);
    uint8_t *v = vga_planes();
    int h = char_height();
    for (int i = 0; i < height; i++) {
        int r = up ? r0 + i : r1 - i;
        int src = up ? r + lines : r - lines;
        bool from = up ? src <= r1 : src >= r0;
        if (text_mode()) {
            for (int c = c0; c <= c1; c++) {
                uint8_t *d = cell(page, r, c);
                if (from) {
                    const uint8_t *s = cell(page, src, c);
                    d[0] = s[0];
                    d[1] = s[1];
                } else {
                    d[0] = 0x20;
                    d[1] = attr_v;
                }
            }
            continue;
        }
        if (kind() == K_PLANAR || kind() == K_PLANAR1) {
            uint32_t bpl = (uint32_t)bytes_per_line();
            for (int k = 0; k < h; k++) {
                uint32_t d = (page_base(page) + (uint32_t)(r * h + k) * bpl + (uint32_t)c0) & 0xFFFF;
                uint32_t n = (uint32_t)(c1 - c0 + 1);
                if (from) {
                    uint32_t s = (page_base(page) + (uint32_t)(src * h + k) * bpl + (uint32_t)c0) & 0xFFFF;
                    memmove(v + d * 4, v + s * 4, n * 4);
                } else {
                    for (uint32_t j = 0; j < n; j++) {
                        for (int p = 0; p < 4; p++) {
                            v[(d + j) * 4 + p] = ((attr_v >> p) & 1) ? 0xFF : 0x00;
                        }
                    }
                }
            }
            continue;
        }
        if (kind() == K_256) {
            for (int k = 0; k < 8; k++) {
                uint32_t d = (uint32_t)(r * 8 + k) * 320 + (uint32_t)c0 * 8;
                uint32_t n = (uint32_t)(c1 - c0 + 1) * 8;
                if (from) {
                    memmove(v + d, v + (uint32_t)(src * 8 + k) * 320 + (uint32_t)c0 * 8, n);
                } else {
                    memset(v + d, attr_v, n);
                }
            }
            continue;
        }
        int bpc = kind() == K_CGA2 ? 1 : 2;     /* the CGA modes: through the card */
        for (int k = 0; k < 8; k++) {
            for (int j = c0 * bpc; j < (c1 + 1) * bpc; j++) {
                cga_put(cga_line(r * 8 + k) + (uint32_t)j, from ? cga_get(cga_line(src * 8 + k) + (uint32_t)j) : attr_v);
            }
        }
    }
}

/* ---- Characters ---- */

static void write_chars(int page, uint8_t ch, uint8_t attr_v, bool with_attr, int count)
{
    uint16_t pos = bda16(0x50 + page * 2);
    int row = pos >> 8, col = pos & 0xFF;
    int cols = text_mode() ? columns() : (kind() == K_256 || kind() == K_CGA4 ? 40 : columns());
    for (int i = 0; i < count; i++) {
        int c = col + i, r = row + c / cols;
        c %= cols;
        if (r >= rows()) {
            break;
        }
        if (text_mode()) {
            uint8_t *p = cell(page, r, c);
            p[0] = ch;
            if (with_attr) {
                p[1] = attr_v;
            }
        } else {
            gfx_char(page, r, c, ch, attr_v);
        }
    }
}

static void tty(uint8_t ch, uint8_t color, bool write)
{
    int page = *bda(0x62);
    uint16_t pos = bda16(0x50 + page * 2);
    int row = pos >> 8, col = pos & 0xFF;
    int cols = text_mode() ? columns() : (kind() == K_256 || kind() == K_CGA4 ? 40 : columns());
    switch (write ? ch : 0xFF) {
    case 0x07:
        speaker_beep(pc_screen_hz() / 4);
        return;
    case 0x08:
        if (col > 0) {
            col--;
        }
        break;
    case 0x0A:
        row++;
        break;
    case 0x0D:
        col = 0;
        break;
    default:
        if (write) {
            if (text_mode()) {
                cell(page, row, col)[0] = ch;
            } else {
                gfx_char(page, row, col, ch, color);
            }
        }
        if (++col >= cols) {
            col = 0;
            row++;
        }
        break;
    }
    if (row >= rows()) {
        row = rows() - 1;
        uint8_t attr_v = text_mode() ? cell(page, row, col)[1] : 0;
        scroll(true, 1, attr_v, 0, 0, rows() - 1, cols - 1);
    }
    set_cursor(page, row, col);
}

void bios_vga_teletype(uint8_t ch, uint8_t color)
{
    tty(ch, color, true);
}

static void write_string(x86_t *c)
{
    uint8_t flags = c->r.b[0], page = c->r.b[7], attr_v = c->r.b[6];
    uint16_t count = c->r.w[X86_CX];
    uint16_t saved = bda16(0x50 + page * 2);
    uint32_t src = c->seg[X86_ES].base + c->r.w[X86_BP];
    set_cursor(page, c->r.b[5], c->r.b[4]);
    int active = *bda(0x62);
    *bda(0x62) = page;
    for (uint16_t i = 0; i < count; i++) {
        uint8_t ch = x86_read8(c, src++);
        if (flags & 2) {
            attr_v = x86_read8(c, src++);
        }
        if (ch == 0x07 || ch == 0x08 || ch == 0x0A || ch == 0x0D) {
            tty(ch, attr_v, true);
            continue;
        }
        write_chars(page, ch, attr_v, true, 1);
        tty(ch, attr_v, false);
    }
    *bda(0x62) = (uint8_t)active;
    if (!(flags & 1)) {
        set_bda16(0x50 + page * 2, saved);
        update_cursor(page);
    }
}

/* ---- AH=10h: the palette and the DAC ---- */

static void palette(x86_t *c)
{
    uint8_t al = R_AL(c), bh = R_BH(c), bl = R_BL(c);
    uint32_t es = c->seg[X86_ES].base;
    const vga_regs_t *r = vga_regs();
    switch (al) {
    case 0x00:
        if (bl < 16) {
            attr(bl, bh);
        }
        break;
    case 0x01:
        attr(0x11, bh);
        break;
    case 0x02:
        for (int i = 0; i < 16; i++) {
            attr(i, x86_read8(c, es + c->r.w[X86_DX] + (uint32_t)i));
        }
        attr(0x11, x86_read8(c, es + c->r.w[X86_DX] + 16u));
        break;
    case 0x03:                      /* bit 7: blinking (1) or bright backgrounds */
        attr(0x10, (uint8_t)((r->attr[0x10] & ~0x08) | ((bl & 1) ? 0x08 : 0)));
        *bda(0x65) = (uint8_t)((*bda(0x65) & ~0x20) | ((bl & 1) ? 0x20 : 0));
        break;
    case 0x07:
        if (is_vga() && bl < 21) {
            R_BH(c) = r->attr[bl];
        }
        break;
    case 0x08:
        if (is_vga()) {
            R_BH(c) = r->attr[0x11];
        }
        break;
    case 0x09:
        if (is_vga()) {
            for (int i = 0; i < 17; i++) {
                x86_write8(c, es + c->r.w[X86_DX] + (uint32_t)i, r->attr[i < 16 ? i : 0x11]);
            }
        }
        break;
    case 0x10:
        vga_set_dac(c->r.w[X86_BX] & 0xFF, R_DH(c), R_CH(c), R_CL(c));
        break;
    case 0x12:
        for (uint16_t i = 0; i < c->r.w[X86_CX]; i++) {
            uint32_t a = es + c->r.w[X86_DX] + i * 3u;
            vga_set_dac((c->r.w[X86_BX] + i) & 0xFF, x86_read8(c, a), x86_read8(c, a + 1), x86_read8(c, a + 2));
        }
        break;
    case 0x13:                      /* the colour page: attribute mode bit 7, colour select */
        if (bl == 0) {
            attr(0x10, (uint8_t)((r->attr[0x10] & ~0x80) | ((bh & 1) ? 0x80 : 0)));
        } else {
            attr(0x14, (r->attr[0x10] & 0x80) ? (uint8_t)(bh & 0x0F) : (uint8_t)((bh & 3) << 2));
        }
        break;
    case 0x15:
        R_DH(c) = r->dac[c->r.w[X86_BX] & 0xFF][0];
        R_CH(c) = r->dac[c->r.w[X86_BX] & 0xFF][1];
        R_CL(c) = r->dac[c->r.w[X86_BX] & 0xFF][2];
        break;
    case 0x17:
        for (uint16_t i = 0; i < c->r.w[X86_CX]; i++) {
            uint32_t a = es + c->r.w[X86_DX] + i * 3u;
            const uint8_t *d = r->dac[(c->r.w[X86_BX] + i) & 0xFF];
            x86_write8(c, a, d[0]);
            x86_write8(c, a + 1, d[1]);
            x86_write8(c, a + 2, d[2]);
        }
        break;
    case 0x1A:
        R_BL(c) = (uint8_t)((r->attr[0x10] >> 7) & 1);
        R_BH(c) = (r->attr[0x10] & 0x80) ? (uint8_t)(r->attr[0x14] & 0x0F) : (uint8_t)((r->attr[0x14] >> 2) & 3);
        break;
    case 0x1B:                      /* sum to greys */
        for (uint16_t i = 0; i < c->r.w[X86_CX]; i++) {
            const uint8_t *d = r->dac[(c->r.w[X86_BX] + i) & 0xFF];
            uint8_t g = (uint8_t)((d[0] * 30 + d[1] * 59 + d[2] * 11 + 50) / 100);
            vga_set_dac((c->r.w[X86_BX] + i) & 0xFF, g, g, g);
        }
        break;
    default:
        break;
    }
}

/* ---- AH=11h: the character generator ---- */

static void character_generator(x86_t *c)
{
    uint8_t al = R_AL(c), bl = R_BL(c), bh = R_BH(c);
    bool set_mode = al >= 0x10 && al < 0x20;
    int height = 0;
    switch (al & 0xEF) {
    case 0x00: {                    /* a user font: ES:BP, CX characters from DX, BH rows, block BL */
        int count = c->r.w[X86_CX] > 256 ? 256 : c->r.w[X86_CX];
        uint32_t src = (c->seg[X86_ES].base + c->r.w[X86_BP]) & 0xFFFFF;
        int h = bh ? bh : 16;
        if (src + (uint32_t)(count * h) > 0x100000) {
            return;
        }
        load_font(pc.ram + src, c->r.w[X86_DX] & 0xFF, count, h, bl & 7);    /* (the first MB: pc.ram's) */
        height = h;
        break;
    }
    case 0x01:
        load_font(rom_font(14), 0, 256, 14, bl & 7);
        height = 14;
        break;
    case 0x02:
        load_font(rom_font(8), 0, 256, 8, bl & 7);
        height = 8;
        break;
    case 0x03:                      /* the block specifier: the character map select register */
        seq(3, bl);
        break;
    case 0x04:
        if (is_vga()) {
            load_font(rom_font(16), 0, 256, 16, bl & 7);
            height = 16;
        }
        break;
    case 0x20:                      /* INT 1Fh: ES:BP */
        put_vector(0x1F, c->seg[X86_ES].sel, c->r.w[X86_BP]);
        return;
    case 0x21: case 0x22: case 0x23: case 0x24: {   /* INT 43h: user, 8x14, 8x8, 8x16 */
        static const uint16_t offs[4] = { 0, ROM_FONT14, ROM_FONT8, ROM_FONT16 };
        if (al == 0x21) {
            put_vector(0x43, c->seg[X86_ES].sel, c->r.w[X86_BP]);
            set_bda16(0x85, c->r.w[X86_CX]);
        } else {
            put_vector(0x43, 0xC000, offs[al - 0x21]);
            set_bda16(0x85, al == 0x22 ? 14 : al == 0x23 ? 8 : 16);
        }
        int lines = bl == 1 ? 14 : bl == 2 ? 25 : bl == 3 ? 43 : R_DL(c);
        *bda(0x84) = (uint8_t)(lines - 1);
        return;
    }
    case 0x30: {                    /* the fonts' addresses: ES:BP, CX the height, DL the rows */
        uint16_t seg = 0xC000, off = 0;
        switch (bh) {
        case 0: off = (uint16_t)(pc.ram[0x7C] | pc.ram[0x7D] << 8);
                seg = (uint16_t)(pc.ram[0x7E] | pc.ram[0x7F] << 8); break;
        case 1: off = (uint16_t)(pc.ram[0x10C] | pc.ram[0x10D] << 8);
                seg = (uint16_t)(pc.ram[0x10E] | pc.ram[0x10F] << 8); break;
        case 2: off = ROM_FONT14; break;
        case 3: off = ROM_FONT8; break;
        case 4: off = ROM_FONT8 + 128 * 8; break;
        case 5: off = ROM_ALT; break;
        case 6: off = is_vga() ? ROM_FONT16 : ROM_FONT14; break;
        case 7: off = ROM_ALT; break;
        default: break;
        }
        x86_load_seg(c, X86_ES, seg);
        c->r.w[X86_BP] = off;
        c->r.w[X86_CX] = (uint16_t)char_height();
        R_DL(c) = (uint8_t)(rows() - 1);
        return;
    }
    default:
        return;
    }
    if (set_mode && height) {
        /* the text mode's rows follow the font's height */
        int lines = bda16(0x85) * rows();
        int r = lines / height;
        crtc(9, (uint8_t)((vga_regs()->crtc[9] & 0xE0) | (height - 1)));
        crtc(10, (uint8_t)(height - 2));
        crtc(11, (uint8_t)(height - 1));
        crtc(0x14, (uint8_t)(height - 1));
        set_bda16(0x85, (uint16_t)height);
        *bda(0x84) = (uint8_t)(r - 1);
        set_bda16(0x4C, (uint16_t)(((r * columns() * 2) + 0xFF) & ~0xFF));
        set_bda16(0x60, (uint16_t)((height - 2) << 8 | (height - 1)));
    }
}

/* ---- AH=1Bh: the state, AH=1Ch: save and restore ---- */

static void state(x86_t *c)
{
    uint32_t d = c->seg[X86_ES].base + c->r.w[X86_DI];
    uint8_t t[64];
    memset(t, 0, sizeof(t));
    t[0] = (uint8_t)ROM_STATIC;
    t[1] = (uint8_t)(ROM_STATIC >> 8);
    t[2] = 0x00;
    t[3] = 0xC0;
    t[4] = (uint8_t)mode();
    t[5] = (uint8_t)columns();
    t[7] = (uint8_t)bda16(0x4C);
    t[8] = (uint8_t)(bda16(0x4C) >> 8);
    t[9] = (uint8_t)bda16(0x4E);
    t[10] = (uint8_t)(bda16(0x4E) >> 8);
    memcpy(t + 11, bda(0x50), 16);
    t[27] = *bda(0x61);
    t[28] = *bda(0x60);
    t[29] = *bda(0x62);
    t[30] = (uint8_t)crtc_port();
    t[31] = (uint8_t)(crtc_port() >> 8);
    t[32] = *bda(0x65);
    t[33] = *bda(0x66);
    t[34] = (uint8_t)rows();
    t[35] = (uint8_t)char_height();
    t[37] = 0x08;                   /* VGA, analog colour */
    int colours = kind() == K_256 ? 256 : kind() == K_CGA4 ? 4 : kind() == K_CGA2 || kind() == K_PLANAR1 ? 2 : 16;
    t[39] = (uint8_t)colours;
    t[40] = (uint8_t)(colours >> 8);
    t[41] = text_mode() ? 8 : 1;
    t[42] = (uint8_t)(char_height() == 16 && text_mode() ? 2 : char_height() == 14 ? 1 : 0);
    t[49] = 0x03;                   /* 256 KB */
    for (int i = 0; i < 64; i++) {
        x86_write8(c, d + (uint32_t)i, t[i]);
    }
    R_AL(c) = 0x1B;
}

static void save_restore(x86_t *c)
{
    uint32_t b = c->seg[X86_ES].base + c->r.w[X86_BX];
    vga_regs_t r;
    switch (R_AL(c)) {
    case 0x00:
        c->r.w[X86_BX] = (uint16_t)((sizeof(vga_regs_t) + 63) / 64);
        break;
    case 0x01:
        r = *vga_regs();
        for (uint32_t i = 0; i < sizeof(r); i++) {
            x86_write8(c, b + i, ((const uint8_t *)&r)[i]);
        }
        break;
    case 0x02:
        for (uint32_t i = 0; i < sizeof(r); i++) {
            ((uint8_t *)&r)[i] = x86_read8(c, b + i);
        }
        video_out(0x3C2, r.misc);
        for (int i = 1; i < 5; i++) {
            seq(i, r.seq[i]);
        }
        crtc(0x11, 0);
        for (int i = 0; i < 25; i++) {
            crtc(i, r.crtc[i]);
        }
        for (int i = 0; i < 21; i++) {
            attr(i, r.attr[i]);
        }
        for (int i = 0; i < 9; i++) {
            gc(i, r.gc[i]);
        }
        for (int i = 0; i < 256; i++) {
            vga_set_dac(i, r.dac[i][0], r.dac[i][1], r.dac[i][2]);
        }
        video_out(0x3C6, r.pel_mask);
        break;
    default:
        return;
    }
    R_AL(c) = 0x1C;
}

/* ---- INT 10h ---- */

void bios_vga_int10(x86_t *c)
{
    uint8_t ah = R_AH(c), al = R_AL(c), bh = R_BH(c), bl = R_BL(c);
    int page = bh & 7;
    switch (ah) {
    case 0x00:
        bios_vga_mode(al);
        break;
    case 0x01:
        set_bda16(0x60, c->r.w[X86_CX]);
        crtc(10, R_CH(c));
        crtc(11, R_CL(c));
        break;
    case 0x02:
        set_cursor(page, R_DH(c), R_DL(c));
        break;
    case 0x03:
        c->r.w[X86_DX] = bda16(0x50 + page * 2);
        c->r.w[X86_CX] = bda16(0x60);
        break;
    case 0x04:
        R_AH(c) = 0;
        break;
    case 0x05: {
        int pages = text_mode() ? 8 : (int)((kind() == K_256 || !bda16(0x4C)) ? 1 : 0x10000 / bda16(0x4C));
        if (al < pages) {
            *bda(0x62) = al;
            uint16_t start = (uint16_t)(al * bda16(0x4C));
            set_bda16(0x4E, start);
            uint16_t ma = text_mode() ? (uint16_t)(start >> 1) : start;
            crtc(12, (uint8_t)(ma >> 8));
            crtc(13, (uint8_t)ma);
            update_cursor(al);
        }
        break;
    }
    case 0x06:
    case 0x07:
        scroll(ah == 0x06, al, bh, R_CH(c), R_CL(c), R_DH(c), R_DL(c));
        break;
    case 0x08:
        if (text_mode()) {
            uint16_t pos = bda16(0x50 + page * 2);
            const uint8_t *p = cell(page, pos >> 8, pos & 0xFF);
            R_AL(c) = p[0];
            R_AH(c) = p[1];
        } else {
            uint16_t pos = bda16(0x50 + page * 2);
            R_AL(c) = gfx_read_char(page, pos >> 8, pos & 0xFF);
            R_AH(c) = 0;
        }
        break;
    case 0x09:
    case 0x0A:
        write_chars(page, al, bl, ah == 0x09 || !text_mode(), c->r.w[X86_CX]);
        break;
    case 0x0B: {                    /* the CGA's colour select (its 3D9 kept in 40:66) */
        uint8_t sel = *bda(0x66);
        if (bh == 0) {
            sel = (uint8_t)((sel & 0xE0) | (bl & 0x1F));    /* background/border, the intensity */
        } else {
            sel = (uint8_t)((sel & ~0x20) | ((bl & 1) ? 0x20 : 0));
        }
        *bda(0x66) = sel;
        uint8_t back = (uint8_t)((sel & 7) | ((sel & 8) ? 0x10 : 0));
        attr(0x11, back);
        if (!text_mode()) {
            attr(0, back);
        }
        if (kind() == K_CGA4) {
            uint8_t first = (sel & 0x20) ? 3 : 2, bright = (sel & 0x10) ? 0x10 : 0;
            for (int i = 0; i < 3; i++) {
                attr(1 + i, (uint8_t)((first + i * 2) | bright));
            }
        }
        break;
    }
    case 0x0C:
        if (!text_mode()) {
            pixel_write(page, c->r.w[X86_CX], c->r.w[X86_DX], al);
        }
        break;
    case 0x0D:
        R_AL(c) = text_mode() ? 0 : pixel_read(page, c->r.w[X86_CX], c->r.w[X86_DX]);
        break;
    case 0x0E:
        tty(al, bl, true);
        break;
    case 0x0F:
        R_AL(c) = (uint8_t)(mode() | (*bda(0x87) & 0x80));
        R_AH(c) = (uint8_t)columns();
        R_BH(c) = *bda(0x62);
        break;
    case 0x10:
        palette(c);
        break;
    case 0x11:
        character_generator(c);
        break;
    case 0x12:
        switch (bl) {
        case 0x10:                  /* the card: colour, 256 KB, its switches */
            R_BH(c) = 0;
            R_BL(c) = 3;
            R_CH(c) = 0;
            R_CL(c) = 0x09;
            break;
        case 0x30:
            if (is_vga() && al <= 2) {
                s_scan_lines = al;
                R_AL(c) = 0x12;
            }
            break;
        case 0x20: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36:
            R_AL(c) = 0x12;
            break;
        default:
            break;
        }
        break;
    case 0x13:
        write_string(c);
        break;
    case 0x1A:
        if (is_vga()) {
            if (al == 0) {
                c->r.w[X86_BX] = 0x0008;    /* VGA with an analog colour display */
            }
            R_AL(c) = 0x1A;
        }
        break;
    case 0x1B:
        if (is_vga()) {
            state(c);
        }
        break;
    case 0x1C:
        if (is_vga()) {
            save_restore(c);
        }
        break;
    default:
        break;
    }
}
