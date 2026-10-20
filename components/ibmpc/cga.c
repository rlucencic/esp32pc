/*
 * IBM Color Graphics Adapter
 *
 * 16 KB at B8000 (again at BC000), the 6845 at 3D4/3D5, the mode register
 * 3D8, the colour select 3D9, the status 3DA. Text in 40 and 80 columns
 * (its 8x8 character ROM), 320 x 200 in four colours (both palettes, their
 * intense halves, the third one with the colour burst off), 640 x 200 in
 * two; the graphics modes' even and odd lines in the two 8 KB halves.
 * Drawn from the 6845's registers, so programs' own timings (the 160 x 100
 * mode made of 2-line characters) show as on the card. Composite colour
 * would be another way of drawing the same copy.
 */
#include "video.h"

static void map(void)
{
    pc_map(0xB8000, 0x4000, pc.ram + 0xB8000, 0);
    pc_map(0xBC000, 0x4000, pc.ram + 0xB8000, 0);
}

static bool in(uint16_t port, uint8_t *v)
{
    if (port < 0x3D0 || port > 0x3DF) {
        return false;
    }
    if (port <= 0x3D7) {
        *v = (port & 1) ? crtc_read() : 0xFF;
        return true;
    }
    if (port == 0x3DA) {
        int line, dot, lines, dots;
        video_beam(&line, &dot, &lines, &dots);
        const uint8_t *r = adapter.crtc;
        int cw = (adapter.mode & 1) ? 8 : 16;
        int rh = (r[9] & 0x1F) + 1;
        bool display = dot < r[1] * cw && line < (r[6] & 0x7F) * rh;
        int vs = (r[7] & 0x7F) * rh;
        bool vsync = line >= vs && line < vs + 16;
        *v = (uint8_t)(0xF0 | (display ? 0 : 0x01) | (vsync ? 0x08 : 0));
        return true;
    }
    *v = 0xFF;
    return true;
}

static bool out(uint16_t port, uint8_t v)
{
    if (port < 0x3D0 || port > 0x3DF) {
        return false;
    }
    if (port <= 0x3D7) {
        if (port & 1) {
            crtc_write(v);
        } else {
            adapter.index = v & 0x1F;
        }
        return true;
    }
    switch (port) {
    case 0x3D8:
        if ((adapter.mode ^ v) & 0x13) {
            video_timing_changed();
        }
        adapter.mode = v;
        video_changed(0, 0, v);
        break;
    case 0x3D9:
        adapter.color = v;
        video_changed(1, 0, v);
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
    g->dot_hz = 14318180;
    g->char_w = (a->mode & 1) ? 8 : 16;
    g->dots_line = (r[0] + 1) * g->char_w;
    g->lines = ((r[4] & 0x7F) + 1) * rh + (r[5] & 0x1F);
    if (g->dots_line < 600 || g->dots_line > 1200 || g->lines < 200 || g->lines > 400) {
        g->dots_line = 912;         /* not set up (yet): the usual 15.7 kHz, 262 lines */
        g->lines = 262;
    }
    int w = r[1] * g->char_w, h = (r[6] & 0x7F) * rh;
    g->width = w >= 320 && w <= 800 ? w : 640;
    g->height = h >= 100 && h <= 300 ? h : 200;
}

static void render(const picture_t *p, const uint8_t *vram, int y, uint16_t *dst)
{
    const adapter_t *a = &p->regs;
    const uint8_t *r = a->crtc;
    int w = p->width;
    if (!(a->mode & 0x08)) {
        memset(dst, 0, (size_t)w * sizeof(uint16_t));      /* video off */
        return;
    }
    int rh = (r[9] & 0x1F) + 1;
    int row = y / rh, ra = y % rh;
    int cw = (a->mode & 1) ? 8 : 16;
    int cols = w / cw;
    uint32_t ma = (uint32_t)((r[12] << 8) | r[13]) + (uint32_t)row * r[1];
    uint16_t *o = dst;

    if (a->mode & 0x02) {
        uint32_t bank = (uint32_t)(ra & 1) << 13;
        if (a->mode & 0x10) {       /* 640 x 200: one bit a pixel */
            uint16_t fg = cga_rgb565[a->color & 15];
            for (int c = 0; c < cols; c++) {
                uint32_t addr = ((ma + (uint32_t)c) * 2) & 0x1FFF;
                for (int k = 0; k < 2; k++) {
                    uint8_t b = vram[bank | ((addr + (uint32_t)k) & 0x1FFF)];
                    for (int i = 7; i >= 0; i--) {
                        *o++ = (b >> i) & 1 ? fg : 0;
                    }
                }
            }
        } else {                    /* 320 x 200: two bits a pixel, each two wide */
            static const uint8_t pals[3][3] = { { 2, 4, 6 }, { 3, 5, 7 }, { 3, 4, 7 } };
            const uint8_t *pal = pals[(a->mode & 0x04) ? 2 : (a->color >> 5) & 1];
            uint8_t bright = (a->color & 0x10) ? 8 : 0;
            uint16_t colours[4] = {
                cga_rgb565[a->color & 15],
                cga_rgb565[pal[0] | bright], cga_rgb565[pal[1] | bright], cga_rgb565[pal[2] | bright],
            };
            for (int c = 0; c < cols; c++) {
                uint32_t addr = ((ma + (uint32_t)c) * 2) & 0x1FFF;
                for (int k = 0; k < 2; k++) {
                    uint8_t b = vram[bank | ((addr + (uint32_t)k) & 0x1FFF)];
                    for (int i = 6; i >= 0; i -= 2) {
                        uint16_t px = colours[(b >> i) & 3];
                        *o++ = px;
                        *o++ = px;
                    }
                }
            }
        }
        return;
    }

    /* Text */
    uint32_t cursor = (uint32_t)((r[14] << 8) | r[15]) & 0x3FFF;
    int cs = r[10] & 0x1F, ce = r[11] & 0x1F;
    bool cursor_on = ((r[10] >> 5) & 3) != 1 && (p->frame & 8);
    bool in_cursor = cs <= ce ? (ra >= cs && ra <= ce) : (ra >= cs || ra <= ce);
    bool blink_off = (a->mode & 0x20) && (p->frame & 16);
    for (int c = 0; c < cols; c++) {
        uint32_t m = (ma + (uint32_t)c) & 0x3FFF;
        uint32_t addr = (m * 2) & 0x3FFF;
        uint8_t ch = vram[addr], at = vram[addr + 1];
        uint8_t bits = pc_font_cga8[ch * 8 + (ra & 7)];
        uint8_t fg = at & 15, bg = at >> 4;
        if (a->mode & 0x20) {       /* bit 7 blinks rather than brightens the background */
            if ((bg & 8) && blink_off) {
                bits = 0;
            }
            bg &= 7;
        }
        if (cursor_on && in_cursor && m == cursor) {
            bits = 0xFF;
        }
        uint16_t f = cga_rgb565[fg], b = cga_rgb565[bg];
        if (cw == 8) {
            for (int i = 7; i >= 0; i--) {
                *o++ = (bits >> i) & 1 ? f : b;
            }
        } else {
            for (int i = 7; i >= 0; i--) {
                uint16_t px = (bits >> i) & 1 ? f : b;
                *o++ = px;
                *o++ = px;
            }
        }
    }
}

static void apply(picture_t *p, const change_t *c)
{
    if (c->reg == 0) {
        p->regs.mode = c->value;
    } else {
        p->regs.color = c->value;
    }
}

static const char *mode_name(const adapter_t *a)
{
    if (a->mode & 0x02) {
        return (a->mode & 0x10) ? "640 x 200, 2 colours" : "320 x 200, 4 colours";
    }
    return (a->mode & 0x01) ? "80 x 25 text" : "40 x 25 text";
}

const card_t cga_card = {
    .vram = 0xB8000,
    .vram_size = 0x4000,
    .map = map,
    .in = in,
    .out = out,
    .geometry = geometry,
    .render = render,
    .mode_name = mode_name,
    .apply = apply,
};
