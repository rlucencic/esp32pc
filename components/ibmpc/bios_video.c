/*
 * The BIOS's INT 10h for the MDA (and Hercules) and the CGA: modes 0-7
 *
 * Setting a mode programs the card through its ports from the ROM's video
 * parameters (F000:F0A4, INT 1Dh), as IBM's BIOS does; the BIOS data area
 * keeps the mode, the columns, the page, the cursors. Text goes straight
 * into the card's memory; in the graphics modes characters are drawn from
 * the ROM's 8x8 font (128-255 from INT 1Fh's table, as GRAFTABL sets it).
 * The EGA's and VGA's functions answer as a card without them does (the
 * EGA and the VGA themselves: bios_vga.c).
 */
#include "pc_internal.h"

#define ROWS        25

static int mode(void)
{
    return *bda(0x49);
}

static int columns(void)
{
    return bda16(0x4A);
}

static bool text_mode(void)
{
    return mode() <= 3 || mode() == 7;
}

static uint16_t crtc_port(void)
{
    return pc.cfg.video == PC_HERCULES ? 0x3B4 : 0x3D4;
}

static void crtc(int reg, uint8_t v)
{
    video_out(crtc_port(), (uint8_t)reg);
    video_out((uint16_t)(crtc_port() + 1), v);
}

/* A character cell of a text page (wrapping in the card's memory) */
static uint8_t *cell(int page, int row, int col)
{
    uint32_t base = mode() == 7 ? 0xB0000 : 0xB8000;
    uint32_t mask = mode() == 7 ? 0x7FFF : 0x3FFF;
    uint32_t off = ((uint32_t)page * bda16(0x4C) + (uint32_t)(row * columns() + col) * 2) & mask;
    return pc.ram + base + off;
}

/* A graphics line (the CGA's even and odd banks) */
static uint8_t *gline(int y)
{
    return pc.ram + 0xB8000 + ((uint32_t)(y & 1) << 13) + (uint32_t)(y >> 1) * 80;
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

static bool ega_vga(void)
{
    return pc.cfg.video == PC_EGA || pc.cfg.video == PC_VGA;
}

void bios_video_mode(int m)
{
    if (ega_vga()) {
        bios_vga_mode(m);
        return;
    }
    bool herc = pc.cfg.video == PC_HERCULES;
    bool keep = m & 0x80;
    m &= 0x7F;
    if (herc) {
        m = 7;
    } else if (m > 6) {
        m = 3;
    }
    const uint8_t *vp = pc.ram + 0xF0000 + 0xF0A4;
    int set = m <= 1 ? 0 : m <= 3 ? 1 : m <= 6 ? 2 : 3;
    uint8_t modereg = vp[80 + m];
    uint16_t port = crtc_port();
    video_out((uint16_t)(port + 4), (uint8_t)(modereg & ~0x08));   /* the video off meanwhile */
    for (int r = 0; r < 16; r++) {
        crtc(r, vp[set * 16 + r]);
    }
    if (!keep) {
        uint32_t base = herc ? 0xB0000 : 0xB8000, size = herc ? 0x8000 : 0x4000;
        if (m <= 3 || m == 7) {
            for (uint32_t i = 0; i < size; i += 2) {
                pc.ram[base + i] = 0x20;
                pc.ram[base + i + 1] = 0x07;
            }
        } else {
            memset(pc.ram + base, 0, size);
        }
    }
    video_out((uint16_t)(port + 4), modereg);
    uint8_t palette = m == 6 ? 0x3F : 0x30;
    if (!herc) {
        video_out(0x3D9, palette);
    }
    *bda(0x49) = (uint8_t)m;
    set_bda16(0x4A, vp[72 + m]);
    static const uint16_t page_size[4] = { 0x0800, 0x1000, 0x4000, 0x1000 };
    set_bda16(0x4C, page_size[set]);
    set_bda16(0x4E, 0);
    memset(bda(0x50), 0, 16);
    set_bda16(0x60, herc ? 0x0B0C : 0x0607);
    *bda(0x62) = 0;
    set_bda16(0x63, port);
    *bda(0x65) = modereg;
    *bda(0x66) = palette;
    *bda(0x84) = ROWS - 1;
    set_bda16(0x85, herc ? 14 : 8);
    update_cursor(0);
}

/* ---- Graphics characters ---- */

static const uint8_t *glyph(uint8_t ch)
{
    static const uint8_t blank[8];
    if (ch < 128) {
        return pc.ram + 0xF0000 + 0xFA6E + ch * 8;
    }
    uint16_t off = (uint16_t)(pc.ram[0x7C] | pc.ram[0x7D] << 8);
    uint16_t seg = (uint16_t)(pc.ram[0x7E] | pc.ram[0x7F] << 8);
    if (!off && !seg) {
        return blank;
    }
    uint32_t a = ((uint32_t)seg << 4) + off + (uint32_t)(ch - 128) * 8;
    return a + 8 <= 0x100000 ? pc.ram + a : blank;
}

static void gfx_char(int row, int col, uint8_t ch, uint8_t color)
{
    const uint8_t *g = glyph(ch);
    bool xor = color & 0x80;
    for (int i = 0; i < 8; i++) {
        uint8_t *line = gline(row * 8 + i);
        uint8_t bits = g[i];
        if (mode() == 6) {
            uint8_t v = (color & 1) ? bits : 0;
            line[col] = xor ? (uint8_t)(line[col] ^ v) : v;
        } else {
            uint16_t v = 0;
            for (int b = 7; b >= 0; b--) {
                v = (uint16_t)(v << 2 | ((bits >> b) & 1 ? (color & 3) : 0));
            }
            uint8_t hi = (uint8_t)(v >> 8), lo = (uint8_t)v;
            line[col * 2] = xor ? (uint8_t)(line[col * 2] ^ hi) : hi;
            line[col * 2 + 1] = xor ? (uint8_t)(line[col * 2 + 1] ^ lo) : lo;
        }
    }
}

static uint8_t gfx_read_char(int row, int col)
{
    uint8_t rows[8];
    for (int i = 0; i < 8; i++) {
        uint8_t *line = gline(row * 8 + i);
        if (mode() == 6) {
            rows[i] = line[col];
        } else {
            uint16_t v = (uint16_t)(line[col * 2] << 8 | line[col * 2 + 1]);
            uint8_t b = 0;
            for (int k = 7; k >= 0; k--) {
                b = (uint8_t)(b << 1 | (((v >> (k * 2)) & 3) ? 1 : 0));
            }
            rows[i] = b;
        }
    }
    for (int ch = 0; ch < 256; ch++) {
        if (!memcmp(glyph((uint8_t)ch), rows, 8)) {
            return (uint8_t)ch;
        }
    }
    return 0;
}

/* ---- Scrolling ---- */

static void scroll(bool up, int lines, uint8_t attr, int r0, int c0, int r1, int c1)
{
    int cols = text_mode() ? columns() : (mode() == 6 ? 80 : 40);
    if (r1 >= ROWS) {
        r1 = ROWS - 1;
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
                    d[1] = attr;
                }
            }
        } else {
            int bpc = mode() == 6 ? 1 : 2;     /* bytes a character column */
            for (int k = 0; k < 8; k++) {
                uint8_t *d = gline(r * 8 + k) + c0 * bpc;
                size_t n = (size_t)(c1 - c0 + 1) * (size_t)bpc;
                if (from) {
                    memmove(d, gline(src * 8 + k) + c0 * bpc, n);
                } else {
                    memset(d, attr, n);
                }
            }
        }
    }
}

/* ---- Characters ---- */

static void write_chars(int page, uint8_t ch, uint8_t attr, bool with_attr, int count)
{
    uint16_t pos = bda16(0x50 + page * 2);
    int row = pos >> 8, col = pos & 0xFF;
    int cols = text_mode() ? columns() : (mode() == 6 ? 80 : 40);
    for (int i = 0; i < count; i++) {
        int c = col + i, r = row + c / cols;
        c %= cols;
        if (r >= ROWS) {
            break;
        }
        if (text_mode()) {
            uint8_t *p = cell(page, r, c);
            p[0] = ch;
            if (with_attr) {
                p[1] = attr;
            }
        } else {
            gfx_char(r, c, ch, attr);
        }
    }
}

/* The teletype's step: a control character's effect, or the character
 * (unless just moving on: write false), then the cursor on, scrolling
 */
static void tty(uint8_t ch, uint8_t color, bool write)
{
    int page = *bda(0x62);
    uint16_t pos = bda16(0x50 + page * 2);
    int row = pos >> 8, col = pos & 0xFF;
    int cols = text_mode() ? columns() : (mode() == 6 ? 80 : 40);
    switch (write ? ch : 0xFF) {
    case 0x07:
        speaker_beep(pc_screen_hz() / 4);   /* the bell: a quarter second at 896 Hz */
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
                gfx_char(row, col, ch, color);
            }
        }
        if (++col >= cols) {
            col = 0;
            row++;
        }
        break;
    }
    if (row >= ROWS) {
        row = ROWS - 1;
        uint8_t attr = text_mode() ? cell(page, row, col)[1] : 0;
        scroll(true, 1, attr, 0, 0, ROWS - 1, cols - 1);
    }
    set_cursor(page, row, col);
}

void bios_teletype(uint8_t ch, uint8_t color)
{
    if (ega_vga()) {
        bios_vga_teletype(ch, color);
        return;
    }
    tty(ch, color, true);
}

static void write_string(x86_t *c)
{
    uint8_t flags = c->r.b[0], page = c->r.b[7], attr = c->r.b[6];
    uint16_t count = c->r.w[X86_CX];
    uint16_t saved = bda16(0x50 + page * 2);
    uint32_t src = c->seg[X86_ES].base + c->r.w[X86_BP];
    set_cursor(page, c->r.b[5], c->r.b[4]);
    int active = *bda(0x62);
    *bda(0x62) = page;
    for (uint16_t i = 0; i < count; i++) {
        uint8_t ch = x86_read8(c, src++);
        if (flags & 2) {
            attr = x86_read8(c, src++);
        }
        if (ch == 0x07 || ch == 0x08 || ch == 0x0A || ch == 0x0D) {
            bios_teletype(ch, attr);
            continue;
        }
        write_chars(page, ch, attr, true, 1);
        tty(ch, attr, false);
    }
    *bda(0x62) = (uint8_t)active;
    if (!(flags & 1)) {
        set_bda16(0x50 + page * 2, saved);
        update_cursor(page);
    }
}

/* ---- Pixels ---- */

static void pixel_write(int x, int y, uint8_t color)
{
    if (y < 0 || y >= 200) {
        return;
    }
    uint8_t *line = gline(y);
    if (mode() == 6) {
        if (x < 0 || x >= 640) {
            return;
        }
        uint8_t bit = (uint8_t)(0x80 >> (x & 7));
        if (color & 0x80) {
            line[x >> 3] ^= (color & 1) ? bit : 0;
        } else {
            line[x >> 3] = (uint8_t)((line[x >> 3] & ~bit) | ((color & 1) ? bit : 0));
        }
    } else {
        if (x < 0 || x >= 320) {
            return;
        }
        int sh = (3 - (x & 3)) * 2;
        uint8_t v = (uint8_t)((color & 3) << sh);
        if (color & 0x80) {
            line[x >> 2] ^= v;
        } else {
            line[x >> 2] = (uint8_t)((line[x >> 2] & ~(3 << sh)) | v);
        }
    }
}

static uint8_t pixel_read(int x, int y)
{
    if (y < 0 || y >= 200) {
        return 0;
    }
    uint8_t *line = gline(y);
    if (mode() == 6) {
        return x >= 0 && x < 640 ? (line[x >> 3] >> (7 - (x & 7))) & 1 : 0;
    }
    return x >= 0 && x < 320 ? (line[x >> 2] >> ((3 - (x & 3)) * 2)) & 3 : 0;
}

/* ---- INT 10h ---- */

void bios_int10(x86_t *c)
{
    if (ega_vga()) {
        bios_vga_int10(c);
        return;
    }
    uint8_t ah = c->r.b[1], al = c->r.b[0], bh = c->r.b[7], bl = c->r.b[6];
    int page = bh & 7;
    switch (ah) {
    case 0x00:
        bios_video_mode(al);
        break;
    case 0x01:                      /* the cursor's lines: CH start, CL end */
        set_bda16(0x60, c->r.w[X86_CX]);
        crtc(10, c->r.b[3]);
        crtc(11, c->r.b[2]);
        break;
    case 0x02:
        set_cursor(page, c->r.b[5], c->r.b[4]);
        break;
    case 0x03:
        c->r.w[X86_DX] = bda16(0x50 + page * 2);
        c->r.w[X86_CX] = bda16(0x60);
        break;
    case 0x04:                      /* light pen: not triggered */
        c->r.b[1] = 0;
        break;
    case 0x05: {                    /* the active page */
        int pages = mode() == 7 ? 1 : mode() <= 1 ? 8 : mode() <= 3 ? 4 : 1;
        if (al < pages) {
            *bda(0x62) = al;
            uint16_t start = (uint16_t)(al * bda16(0x4C));
            set_bda16(0x4E, start);
            crtc(12, (uint8_t)(start >> 9));
            crtc(13, (uint8_t)(start >> 1));
            update_cursor(al);
        }
        break;
    }
    case 0x06:
    case 0x07:
        /* AL lines, BH the blank's attribute, CH/CL to DH/DL */
        scroll(ah == 0x06, al, bh, c->r.b[3], c->r.b[2], c->r.b[5], c->r.b[4]);
        break;
    case 0x08:
        if (text_mode()) {
            uint16_t pos = bda16(0x50 + page * 2);
            const uint8_t *p = cell(page, pos >> 8, pos & 0xFF);
            c->r.b[0] = p[0];
            c->r.b[1] = p[1];
        } else {
            uint16_t pos = bda16(0x50);
            c->r.b[0] = gfx_read_char(pos >> 8, pos & 0xFF);
            c->r.b[1] = 0;
        }
        break;
    case 0x09:
    case 0x0A:
        write_chars(text_mode() ? page : 0, al, bl, ah == 0x09 || !text_mode(), c->r.w[X86_CX]);
        break;
    case 0x0B:                      /* the CGA's colour select */
        if (pc.cfg.video == PC_CGA) {
            uint8_t v = *bda(0x66);
            if (bh == 0) {
                v = (uint8_t)((v & 0xE0) | (bl & 0x1F));
            } else {
                v = (uint8_t)((v & ~0x20) | ((bl & 1) ? 0x20 : 0));
            }
            *bda(0x66) = v;
            video_out(0x3D9, v);
        }
        break;
    case 0x0C:
        if (!text_mode()) {
            pixel_write(c->r.w[X86_CX], c->r.w[X86_DX], al);
        }
        break;
    case 0x0D:
        c->r.b[0] = text_mode() ? 0 : pixel_read(c->r.w[X86_CX], c->r.w[X86_DX]);
        break;
    case 0x0E:
        bios_teletype(al, bl);
        break;
    case 0x0F:
        c->r.b[0] = (uint8_t)mode();
        c->r.b[1] = (uint8_t)columns();
        c->r.b[7] = *bda(0x62);
        break;
    case 0x13:
        write_string(c);
        break;
    default:
        /* 10h-12h, 1Ah-1Ch (the EGA's and VGA's): registers as they were,
         * which is how programs tell there's no EGA or VGA
         */
        break;
    }
}
