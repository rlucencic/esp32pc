/*
 * Hercules Graphics Card: the MDA's text and its 720 x 348 graphics
 *
 * 64 KB: B0000-B7FFF, and B8000-BFFFF once the configuration switch (3BF
 * bit 1) allows the second page. The 6845 at 3B4/3B5, the mode register
 * 3B8 (bit 1 graphics, if 3BF bit 0 allows it; 3 video on; 5 blinking; 7
 * the page shown: B0000 or B8000), the status 3BA: bit 0 the horizontal
 * retrace, bit 3 the video signal, bit 7 low during the vertical retrace
 * (an MDA's doesn't change: how programs tell the cards apart).
 * Text: 80 x 25 in 9 x 14 cells (the MDA's character ROM, the 9th column
 * repeating the 8th for C0-DF), the MDA's attributes: underline,
 * intensity, reverse, blinking, no display. Graphics: a character clock
 * is 16 dots (two bytes), each row of the 6845 four scan lines from the
 * four 8 KB banks (line y at bank y & 3, row y / 4: 90 bytes a line).
 */
#include "video.h"

#define NORMAL      0x2604          /* green, RGB565 */
#define BRIGHT      0x87F0

/* Graphics shown: the mode register asks, the configuration switch lets it */
static bool graphics(const adapter_t *a)
{
    return (a->mode & 0x02) && (a->config & 0x01);
}

static void map(void)
{
    pc_map(0xB0000, 0x8000, pc.ram + 0xB0000, 0);
    pc_map(0xB8000, 0x8000, NULL, 0);
}

static bool in(uint16_t port, uint8_t *v)
{
    if (port < 0x3B0 || port > 0x3BF) {
        return false;
    }
    if (port <= 0x3B7) {
        *v = (port & 1) ? crtc_read() : 0xFF;
        return true;
    }
    if (port == 0x3BA) {
        int line, dot, lines, dots;
        video_beam(&line, &dot, &lines, &dots);
        const uint8_t *r = adapter.crtc;
        int rh = (r[9] & 0x1F) + 1;
        int cw = graphics(&adapter) ? 16 : 9;
        int hs = r[2] * cw;
        bool hsync = dot >= hs && dot < hs + (r[3] & 0x0F) * cw;
        int vs = (r[7] & 0x7F) * rh;
        bool vsync = line >= vs && line < vs + 16;
        bool video = !hsync && line < (r[6] & 0x7F) * rh && (dot & 4);
        *v = (uint8_t)(0x70 | (hsync ? 0x01 : 0) | (video ? 0x08 : 0) | (vsync ? 0 : 0x80));
        return true;
    }
    *v = 0xFF;
    return true;
}

static bool out(uint16_t port, uint8_t v)
{
    if (port < 0x3B0 || port > 0x3BF) {
        return false;
    }
    if (port <= 0x3B7) {
        if (port & 1) {
            crtc_write(v);
        } else {
            adapter.index = v & 0x1F;
        }
        return true;
    }
    switch (port) {
    case 0x3B8:
        if ((adapter.mode ^ v) & 0x02) {
            video_timing_changed();
        }
        adapter.mode = v;
        video_changed(0, 0, v);
        break;
    case 0x3BF:                     /* configuration: bit 0 lets graphics in, bit 1 maps the second page */
        if ((adapter.config ^ v) & 0x01) {
            video_timing_changed();
        }
        adapter.config = v;
        pc_map(0xB8000, 0x8000, (v & 0x02) ? pc.ram + 0xB8000 : NULL, 0);
        break;
    default:
        break;
    }
    return true;
}

static void geometry(const adapter_t *a, geometry_t *g)
{
    const uint8_t *r = a->crtc;
    int rh = (r[9] & 0x1F) + 1;
    g->dot_hz = 16257000;
    g->char_w = graphics(a) ? 16 : 9;
    g->dots_line = (r[0] + 1) * g->char_w;
    g->lines = ((r[4] & 0x7F) + 1) * rh + (r[5] & 0x1F);
    if (g->dots_line < 700 || g->dots_line > 1100 || g->lines < 300 || g->lines > 420) {
        g->dots_line = 882;         /* not set up (yet): the MDA's 18.4 kHz, 370 lines */
        g->lines = 370;
    }
    int w = r[1] * g->char_w, h = (r[6] & 0x7F) * rh;
    g->width = w >= 360 && w <= 800 ? w : 720;
    g->height = h >= 200 && h <= 400 ? h : graphics(a) ? 348 : 350;
}

static void render_graphics(const picture_t *p, const uint8_t *vram, int y, uint16_t *dst)
{
    const adapter_t *a = &p->regs;
    const uint8_t *r = a->crtc;
    int rh = (r[9] & 0x1F) + 1;
    int row = y / rh, ra = y % rh;
    int cols = p->width / 16;
    uint32_t page = (a->mode & 0x80) ? 0x8000 : 0;
    uint32_t bank = page | (uint32_t)(ra & 3) << 13;
    uint32_t ma = (uint32_t)((r[12] << 8) | r[13]) + (uint32_t)row * r[1];
    uint16_t *o = dst;
    for (int c = 0; c < cols; c++) {
        uint32_t addr = ((ma + (uint32_t)c) * 2) & 0x1FFF;
        for (int k = 0; k < 2; k++) {
            uint8_t b = vram[bank | ((addr + (uint32_t)k) & 0x1FFF)];
            for (int i = 7; i >= 0; i--) {
                *o++ = (b >> i) & 1 ? NORMAL : 0;     /* (no intensity in graphics) */
            }
        }
    }
}

static void render(const picture_t *p, const uint8_t *vram, int y, uint16_t *dst)
{
    const adapter_t *a = &p->regs;
    const uint8_t *r = a->crtc;
    int w = p->width;
    if (!(a->mode & 0x08)) {
        memset(dst, 0, (size_t)w * sizeof(uint16_t));
        return;
    }
    if (graphics(a)) {
        render_graphics(p, vram, y, dst);
        return;
    }
    int rh = (r[9] & 0x1F) + 1;
    int row = y / rh, ra = y % rh;
    int cols = w / 9;
    uint32_t ma = (uint32_t)((r[12] << 8) | r[13]) + (uint32_t)row * r[1];
    uint32_t cursor = (uint32_t)((r[14] << 8) | r[15]) & 0x3FFF;
    int cs = r[10] & 0x1F, ce = r[11] & 0x1F;
    bool cursor_on = ((r[10] >> 5) & 3) != 1 && (p->frame & 8);
    bool in_cursor = cs <= ce ? (ra >= cs && ra <= ce) : (ra >= cs || ra <= ce);
    bool blink_off = (a->mode & 0x20) && (p->frame & 16);
    uint16_t *o = dst;
    for (int c = 0; c < cols; c++) {
        uint32_t m = (ma + (uint32_t)c) & 0x3FFF;
        uint32_t addr = (m * 2) & 0x7FFF;
        uint8_t ch = vram[addr], at = vram[addr + 1];
        uint32_t bits = ra < 14 ? pc_font_mda14[ch * 14 + ra] : 0;
        bits = bits << 1 | ((ch & 0xE0) == 0xC0 ? (bits & 1) : 0);     /* 9 columns */
        uint16_t fg = (at & 0x08) ? BRIGHT : NORMAL, bg = 0;
        if ((at & 0x77) == 0x00) {
            bits = 0;               /* not displayed */
        } else if ((at & 0x77) == 0x70) {
            fg = 0;                 /* reverse */
            bg = NORMAL;
        } else if ((at & 0x07) == 0x01 && ra == 12) {
            bits = 0x1FF;           /* underline */
        }
        if ((at & 0x80) && blink_off) {
            bits = 0;
        }
        if (cursor_on && in_cursor && m == cursor) {
            bits = 0x1FF;
            if (fg == 0) {
                fg = NORMAL;
                bg = 0;
            }
        }
        for (int i = 8; i >= 0; i--) {
            *o++ = (bits >> i) & 1 ? fg : bg;
        }
    }
}

static void apply(picture_t *p, const change_t *c)
{
    p->regs.mode = c->value;
}

static const char *mode_name(const adapter_t *a)
{
    return graphics(a) ? "720 x 348 graphics" : "80 x 25 text (MDA)";
}

const card_t herc_card = {
    .vram = 0xB0000,
    .vram_size = 0x10000,
    .map = map,
    .in = in,
    .out = out,
    .geometry = geometry,
    .render = render,
    .mode_name = mode_name,
    .apply = apply,
};
