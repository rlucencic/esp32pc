/*
 * IBM EGA and VGA (one module, the VGA's extras where it has them)
 *
 * 256 KB in four planes, kept interleaved (plane p's byte at offset o is
 * vram[o * 4 + p]), so that the VGA's chain-4 (mode 13h: CPU address A is
 * plane A & 3 at A >> 2) is plain linear memory. Registers: the sequencer
 * (3C4/3C5: clocking, map mask, character map select, memory mode), the
 * graphics controller (3CE/3CF: set/reset, rotate and logical operation,
 * read map, the read and write modes, the memory map, colour compare, bit
 * mask), the attribute controller (3C0/3C1: palette, mode, colour plane
 * enable, panning), the CRTC (3B4/3D4 by the misc register), misc output
 * (3C2), the input status registers (3C2, 3BA/3DA), the VGA's DAC
 * (3C6-3C9). The EGA's are write-only, as on the card (but the cursor's).
 *
 * The CPU's accesses to the window (A0000, B0000 or B8000 as the graphics
 * controller maps it) go through the planar logic: latches, write modes
 * 0-3, read modes 0-1, odd/even and chain-4. Chain-4 with the logic at its
 * defaults (mode 13h's way) maps A0000-AFFFF straight onto the planes.
 *
 * Pictures are drawn from the registers as the hardware generates them:
 * the CRTC's address (start, offset, byte, word or doubleword mode, its
 * MA13/MA14 substitutions for the CGA's and Hercules' banks, row scans,
 * double scanning, the split screen at the line compare, byte and pel
 * panning), the shift registers (planar, the CGA's interleaved two bits,
 * the VGA's 256 colours), the attribute controller (text: the font from
 * plane 2, blinking, the cursor, the 9th column, underlines; colour plane
 * enable, the palette), then the DAC (the EGA: 6-bit rgbRGB, read as IRGB
 * in its 200-line modes as the CGA monitor shows them). So the CGA's
 * modes, mode X and other tweaked modes show as on the cards.
 */
#include <stdio.h>
#include <stdlib.h>
#include "video.h"

#define VRAM_SIZE       0x40000

static struct {
    vga_regs_t r;
    bool vga;
    uint8_t *vram;
    uint8_t seq_index, gc_index, crtc_index;
    bool attr_data;                 /* the next 3C0 write is data */
    uint8_t latch[4];
    uint8_t dac_read, dac_write, dac_comp, dac_state;
    uint8_t dac_rgb[3];
    uint8_t feature;
    int mapped;                     /* the window as mapped (remap()) */
    /* Worked out when the registers change (remap()): the window, and the
     * plain cases' fast paths (write mode 0 with nothing rotated, combined
     * or masked: the map mask's planes take the byte; read mode 0: the
     * latches and one plane), neither chained nor odd/even
     */
    uint32_t win_base, win_size;
    bool fast_write, fast_read;
    uint32_t write_planes;          /* bytes of the four planes written (FFh each) */
    uint8_t read_plane;
} v;

/* ---- Memory ---- */

static uint32_t window_base(void)
{
    static const uint32_t base[4] = { 0xA0000, 0xA0000, 0xB0000, 0xB8000 };
    return base[(v.r.gc[6] >> 2) & 3];
}

static uint32_t window_size(void)
{
    static const uint32_t size[4] = { 0x20000, 0x10000, 0x8000, 0x8000 };
    return size[(v.r.gc[6] >> 2) & 3];
}

static bool chain4(void)
{
    return v.vga && (v.r.seq[4] & 0x08);
}

/* Chain-4 with the graphics controller at its defaults: plain memory */
static bool direct(void)
{
    const uint8_t *g = v.r.gc;
    return chain4() && window_base() == 0xA0000 && (v.r.seq[2] & 0x0F) == 0x0F && g[1] == 0 &&
           (g[3] & 0x1F) == 0 && (g[5] & 0x0B) == 0 && g[8] == 0xFF;
}

static void remap(void)
{
    const uint8_t *g = v.r.gc;
    v.win_base = window_base();
    v.win_size = window_size();
    bool linear = !chain4() && (v.r.seq[4] & 0x04);
    v.fast_write = linear && (g[5] & 3) == 0 && g[3] == 0 && g[1] == 0 && g[8] == 0xFF;
    v.fast_read = linear && !(g[5] & 0x18);
    uint8_t m = v.r.seq[2] & 0x0F;
    v.write_planes = (m & 1 ? 0xFFu : 0) | (m & 2 ? 0xFF00u : 0) | (m & 4 ? 0xFF0000u : 0) | (m & 8 ? 0xFF000000u : 0);
    v.read_plane = g[4] & 3;
    int want = direct() ? 100 : (v.r.gc[6] >> 2) & 3;
    if (want == v.mapped) {
        return;
    }
    v.mapped = want;
    pc_map(0xA0000, 0x20000, NULL, 0);          /* the card's callbacks */
    if (want == 100) {
        pc_map(0xA0000, 0x10000, v.vram, 0);    /* chain-4: CPU address A is vram[A] */
    }
}

static uint8_t mem_read(void *ctx, uint32_t addr)
{
    (void)ctx;
    uint32_t a = addr - v.win_base;
    if (a >= v.win_size) {
        return 0xFF;
    }
    if (v.fast_read) {
        const uint8_t *m = v.vram + (a & 0xFFFF) * 4;
        memcpy(v.latch, m, 4);
        return m[v.read_plane];
    }
    uint32_t o;
    int p;
    if (chain4()) {
        o = a >> 2;
        p = a & 3;
    } else if (v.r.gc[5] & 0x10) {              /* odd/even */
        o = a & ~1u;
        p = (v.r.gc[4] & 2) | (a & 1);
    } else {
        o = a;
        p = v.r.gc[4] & 3;
    }
    const uint8_t *m = v.vram + (o & 0xFFFF) * 4;
    memcpy(v.latch, m, 4);
    if (v.r.gc[5] & 0x08) {                     /* read mode 1: colour compare */
        uint8_t res = 0xFF;
        for (int i = 0; i < 4; i++) {
            if ((v.r.gc[7] >> i) & 1) {
                res &= (uint8_t)~(m[i] ^ (((v.r.gc[2] >> i) & 1) ? 0xFF : 0x00));
            }
        }
        return res;
    }
    return m[p];
}

static void mem_write(void *ctx, uint32_t addr, uint8_t val)
{
    (void)ctx;
    uint32_t a = addr - v.win_base;
    if (a >= v.win_size) {
        return;
    }
    if (v.fast_write) {
        /* the planes' four bytes at once (interleaved, so aligned) */
        uint32_t *m = (uint32_t *)(void *)(v.vram + (a & 0xFFFF) * 4);
        *m = (*m & ~v.write_planes) | (val * 0x01010101u & v.write_planes);
        return;
    }
    uint32_t o;
    uint8_t planes;
    if (chain4()) {
        o = a >> 2;
        planes = (uint8_t)((1u << (a & 3)) & v.r.seq[2]);
    } else if (!(v.r.seq[4] & 0x04)) {          /* odd/even: even addresses planes 0, 2 */
        o = a & ~1u;
        planes = v.r.seq[2] & ((a & 1) ? 0x0A : 0x05);
    } else {
        o = a;
        planes = v.r.seq[2];
    }
    planes &= 0x0F;
    if (!planes) {
        return;
    }
    uint8_t *m = v.vram + (o & 0xFFFF) * 4;
    const uint8_t *g = v.r.gc;
    int mode = g[5] & 3, rot = g[3] & 7, func = (g[3] >> 3) & 3;
    uint8_t mask = g[8];
    uint8_t d = (uint8_t)((val >> rot) | (val << ((8 - rot) & 7)));
    if (mode == 3 && v.vga) {
        mask &= d;
    }
    for (int p = 0; p < 4; p++) {
        if (!((planes >> p) & 1)) {
            continue;
        }
        uint8_t src;
        switch (mode) {
        case 1:
            m[p] = v.latch[p];
            continue;
        case 2:
            src = ((val >> p) & 1) ? 0xFF : 0x00;
            break;
        case 3:
            src = ((g[0] >> p) & 1) ? 0xFF : 0x00;
            break;
        default:
            src = ((g[1] >> p) & 1) ? (((g[0] >> p) & 1) ? 0xFF : 0x00) : d;
            break;
        }
        switch (func) {
        case 1: src &= v.latch[p]; break;
        case 2: src |= v.latch[p]; break;
        case 3: src ^= v.latch[p]; break;
        default: break;
        }
        m[p] = (uint8_t)((src & mask) | (v.latch[p] & ~mask));
    }
}

/* ---- Ports ---- */

static uint16_t crtc_base(void)
{
    return (v.r.misc & 1) ? 0x3D0 : 0x3B0;
}

/* Lines of the frame shown, the vertical retrace's start and length */
static int display_lines(const vga_regs_t *r, bool vga)
{
    const uint8_t *c = r->crtc;
    return (c[0x12] | (c[7] & 0x02) << 7 | (vga ? (c[7] & 0x40) << 3 : 0)) + 1;
}

static int char_dots(const vga_regs_t *r, bool vga)
{
    return vga && !(r->seq[1] & 1) ? 9 : 8;
}

static uint8_t status1(void)
{
    int line, dot, lines, dots;
    video_beam(&line, &dot, &lines, &dots);
    const uint8_t *c = v.r.crtc;
    int hde = (c[1] + 1) * char_dots(&v.r, v.vga);
    int vde = display_lines(&v.r, v.vga);
    int vrs = c[0x10] | (c[7] & 0x04) << 6 | (v.vga ? (c[7] & 0x80) << 2 : 0);
    int vlen = ((c[0x11] & 0x0F) - vrs) & 0x0F;
    if (vlen == 0) {
        vlen = 16;
    }
    bool retrace = line >= vrs && line < vrs + vlen;
    bool off = dot >= hde || line >= vde;
    v.attr_data = false;            /* reading it resets the attribute flip-flop */
    return (uint8_t)((off ? 0x01 : 0) | (retrace ? 0x08 : 0));
}

static bool in(uint16_t port, uint8_t *val)
{
    if (port < 0x3B0 || port > 0x3DF) {
        return false;
    }
    if ((port < 0x3C0 && crtc_base() != 0x3B0) || (port >= 0x3D0 && crtc_base() != 0x3D0)) {
        return false;               /* the other CRTC's ports: not decoded */
    }
    uint16_t cr = crtc_base();
    *val = 0xFF;
    if (port == cr + 4) {
        *val = v.vga ? v.crtc_index : 0xFF;
    } else if (port == cr + 5) {
        if (v.vga || (v.crtc_index >= 0x0C && v.crtc_index <= 0x0F)) {
            *val = v.crtc_index < 25 ? v.r.crtc[v.crtc_index] : 0xFF;
        }
    } else if (port == cr + 0x0A) {
        *val = status1();
    }
    switch (port) {
    case 0x3C0:
        if (v.vga) {
            *val = v.r.attr_index;
        }
        break;
    case 0x3C1:
        if (v.vga) {
            *val = (v.r.attr_index & 0x1F) < 21 ? v.r.attr[v.r.attr_index & 0x1F] : 0;
        }
        break;
    case 0x3C2:                     /* input status 0: the switch / the DAC's colour sense */
        *val = 0x70 | 0x10;
        break;
    case 0x3C4:
        if (v.vga) {
            *val = v.seq_index;
        }
        break;
    case 0x3C5:
        if (v.vga && v.seq_index < 5) {
            *val = v.r.seq[v.seq_index];
        }
        break;
    case 0x3C6:
        if (v.vga) {
            *val = v.r.pel_mask;
        }
        break;
    case 0x3C7:
        if (v.vga) {
            *val = v.dac_state;
        }
        break;
    case 0x3C8:
        if (v.vga) {
            *val = v.dac_write;
        }
        break;
    case 0x3C9:
        if (v.vga) {
            *val = v.r.dac[v.dac_read][v.dac_comp];
            if (++v.dac_comp == 3) {
                v.dac_comp = 0;
                v.dac_read++;
            }
        }
        break;
    case 0x3CA:
        if (v.vga) {
            *val = v.feature;
        }
        break;
    case 0x3CC:
        if (v.vga) {
            *val = v.r.misc;
        }
        break;
    case 0x3CE:
        if (v.vga) {
            *val = v.gc_index;
        }
        break;
    case 0x3CF:
        if (v.vga && v.gc_index < 9) {
            *val = v.r.gc[v.gc_index];
        }
        break;
    default:
        break;
    }
    return true;
}

static bool out(uint16_t port, uint8_t val)
{
    if (port < 0x3B0 || port > 0x3DF) {
        return false;
    }
    if ((port < 0x3C0 && crtc_base() != 0x3B0) || (port >= 0x3D0 && crtc_base() != 0x3D0)) {
        return false;
    }
    uint16_t cr = crtc_base();
    if (port == cr + 4) {
        v.crtc_index = val & 0x1F;
        return true;
    }
    if (port == cr + 5) {
        uint8_t i = v.crtc_index;
        bool protect = v.vga && (v.r.crtc[0x11] & 0x80);
        if (i < 25 && !(protect && i < 7)) {
            if (protect && i == 7) {
                val = (uint8_t)((v.r.crtc[7] & ~0x10) | (val & 0x10));     /* only LC8 */
            }
            v.r.crtc[i] = val;
            video_timing_changed();
        }
        return true;
    }
    if (port == cr + 0x0A) {
        v.feature = val;
        return true;
    }
    switch (port) {
    case 0x3C0:
        if (!v.attr_data) {
            v.r.attr_index = val & 0x3F;
        } else {
            uint8_t i = v.r.attr_index & 0x1F;
            if (i < 21) {
                v.r.attr[i] = val;
                video_changed(0x80, i, val);
                if (i == 0x10) {
                    video_timing_changed();
                }
            }
        }
        v.attr_data = !v.attr_data;
        break;
    case 0x3C2:
        v.r.misc = val;
        video_timing_changed();
        break;
    case 0x3C4:
        v.seq_index = val & 0x07;
        break;
    case 0x3C5:
        if (v.seq_index < 5) {
            v.r.seq[v.seq_index] = val;
            if (v.seq_index == 1) {
                video_timing_changed();
            }
            remap();
        }
        break;
    case 0x3C6:
        v.r.pel_mask = val;
        video_changed(0x81, 0, val);
        break;
    case 0x3C7:
        v.dac_read = val;
        v.dac_comp = 0;
        v.dac_state = 0x03;
        break;
    case 0x3C8:
        v.dac_write = val;
        v.dac_comp = 0;
        v.dac_state = 0x00;
        break;
    case 0x3C9:
        v.dac_rgb[v.dac_comp++] = val & 0x3F;
        if (v.dac_comp == 3) {
            memcpy(v.r.dac[v.dac_write], v.dac_rgb, 3);
            for (int k = 0; k < 3; k++) {
                video_changed((uint8_t)(0x90 + k), v.dac_write, v.dac_rgb[k]);
            }
            v.dac_write++;
            v.dac_comp = 0;
        }
        break;
    case 0x3CE:
        v.gc_index = val & 0x0F;
        break;
    case 0x3CF:
        if (v.gc_index < 9) {
            v.r.gc[v.gc_index] = val;
            if (v.gc_index == 5 || v.gc_index == 6) {
                video_timing_changed();
            }
            remap();
        }
        break;
    default:
        break;
    }
    return true;
}

/* ---- Timing ---- */

static void geometry(const adapter_t *a, geometry_t *g)
{
    (void)a;
    const vga_regs_t *r = &v.r;
    const uint8_t *c = r->crtc;
    static const uint32_t vga_clock[4] = { 25175000, 28322000, 25175000, 25175000 };
    static const uint32_t ega_clock[4] = { 14318180, 16257000, 14318180, 14318180 };
    uint32_t clock = (v.vga ? vga_clock : ega_clock)[(r->misc >> 2) & 3];
    if (r->seq[1] & 0x08) {
        clock /= 2;                 /* the dot clock halved: 320 and 360 wide */
    }
    int cw = char_dots(r, v.vga);
    int htotal = c[0] + (v.vga ? 5 : 2);
    int vtotal = (c[6] | (c[7] & 0x01) << 8 | (v.vga ? (c[7] & 0x20) << 4 : 0)) + (v.vga ? 2 : 1);
    g->dot_hz = clock;
    g->char_w = cw;
    g->dots_line = htotal * cw;
    g->lines = vtotal;
    if (g->dots_line < 300 || g->dots_line > 2000 || g->lines < 200 || g->lines > 700) {
        g->dots_line = 800;         /* not set up (yet): the VGA's 31.5 kHz, 70 Hz */
        g->lines = 449;
        g->dot_hz = 25175000;
    }
    int chars = c[1] + 1;
    int w = !(r->gc[6] & 1) ? chars * cw : (r->gc[5] & 0x40) ? chars * 4 : chars * 8;
    int h = display_lines(r, v.vga);
    g->width = w >= 160 && w <= 1024 ? w : 640;
    g->height = h >= 100 && h <= 700 ? h : 400;
}

/* ---- Pictures ---- */

static PC_DRAW_HOT uint16_t rgb565(uint8_t r6, uint8_t g6, uint8_t b6)
{
    return (uint16_t)((r6 >> 1) << 11 | g6 << 5 | (b6 >> 1));
}

/* An attribute's colour: the palette, then the DAC (the EGA: its 6-bit colour) */
static PC_DRAW_HOT uint16_t colour(const vga_regs_t *r, bool vga, uint8_t pal)
{
    if (!vga) {
        if (((r->misc >> 2) & 3) == 0) {
            /* 200-line modes: the CGA monitor reads IRGB (and makes yellow brown) */
            return cga_rgb565[(pal & 7) | ((pal & 0x10) ? 8 : 0)];
        }
        uint8_t red = (uint8_t)(((pal >> 2) & 1) * 2 + ((pal >> 5) & 1));
        uint8_t green = (uint8_t)(((pal >> 1) & 1) * 2 + ((pal >> 4) & 1));
        uint8_t blue = (uint8_t)((pal & 1) * 2 + ((pal >> 3) & 1));
        return rgb565((uint8_t)(red * 21), (uint8_t)(green * 21), (uint8_t)(blue * 21));
    }
    uint8_t i = pal & 0x3F;
    if (r->attr[0x10] & 0x80) {
        i = (uint8_t)((i & 0x0F) | (r->attr[0x14] & 3) << 4);
    }
    i = (uint8_t)((i | (r->attr[0x14] & 0x0C) << 4) & r->pel_mask);
    return rgb565(r->dac[i][0], r->dac[i][1], r->dac[i][2]);
}

static PC_DRAW_HOT void render(const picture_t *p, const uint8_t *vram, int y, uint16_t *dst)
{
    const vga_regs_t *r = &p->vga;
    const bool vga = p->type == PC_VGA;
    const uint8_t *c = r->crtc;
    int w = p->width;
    if ((r->seq[1] & 0x20) || !(r->attr_index & 0x20)) {
        memset(dst, 0, (size_t)w * sizeof(uint16_t));      /* the picture off */
        return;
    }
    bool graphics = r->gc[6] & 1;
    bool c256 = vga && (r->gc[5] & 0x40);
    int chars = c[1] + 1;
    int cw = graphics ? (c256 ? 4 : 8) : char_dots(r, vga);
    int msl = c[9] & 0x1F;
    bool dscan = vga && (c[9] & 0x80);
    int lc = c[0x18] | (c[7] & 0x10) << 4 | (vga ? (c[9] & 0x40) << 3 : 0);
    bool split = y > lc;
    int sl = split ? y - lc - 1 : y;
    if (dscan) {
        sl >>= 1;
    }
    if (!split) {
        sl += c[8] & 0x1F;          /* preset row scan */
    }
    int row = sl / (msl + 1), rs = sl % (msl + 1);
    uint32_t start = split ? 0 : (uint32_t)((c[0x0C] << 8 | c[0x0D]) + ((c[8] >> 5) & 3));
    uint32_t ma = start + (uint32_t)row * c[0x13] * 2;
    uint8_t mc = c[0x17];
    bool word = !(mc & 0x40), dw = vga && (c[0x14] & 0x40);
    int div = (vga && (c[0x14] & 0x20)) ? 4 : (mc & 0x08) ? 2 : 1;
    int pan = r->attr[0x13] & 0x0F;
    if (split && (r->attr[0x10] & 0x20)) {
        pan = 0;
    }
    if (!graphics && cw == 9) {
        pan = pan >= 8 ? 0 : pan + 1;
    } else if (c256) {
        pan = (pan & 7) >> 1;
    } else {
        pan &= 7;
    }

    uint16_t pal[16];
    uint8_t enable = r->attr[0x12] & 0x0F;
    for (int i = 0; i < 16; i++) {
        pal[i] = colour(r, vga, r->attr[i]);
    }
    static uint16_t line[(132 + 2) * 9];
    uint16_t *o = line;
    int clocks = chars + (pan ? 1 : 0);
    if (clocks > 132) {
        clocks = 132;
    }
    uint32_t cursor = (uint32_t)(c[0x0E] << 8 | c[0x0F]);
    int cs = c[0x0A] & 0x1F, ce = c[0x0B] & 0x1F;
    bool cursor_on = !(c[0x0A] & 0x20) && (p->frame & 8) && rs >= cs && rs <= ce;
    bool blink_off = (p->frame & 16) != 0;
    /* (the 256 colours worked out once a picture and its changes so far) */
    static uint16_t pal256[256];
    static uint32_t pal256_signature;
    static int pal256_applied = -1;
    if (c256 && (pal256_signature != p->signature || pal256_applied != p->applied)) {
        pal256_signature = p->signature;
        pal256_applied = p->applied;
        for (int i = 0; i < 256; i++) {
            uint8_t k = (uint8_t)((r->attr[i >> 4] & 0x0F) << 4 | (r->attr[i & 15] & 0x0F));
            if (!(r->attr[0x10] & 0x40)) {
                k = (uint8_t)i;
            }
            k &= r->pel_mask;
            pal256[i] = rgb565(r->dac[k][0], r->dac[k][1], r->dac[k][2]);
        }
    }

    for (int i = 0; i < clocks; i++) {
        uint32_t m = ma + (uint32_t)(i / div);
        uint32_t a;
        if (dw) {
            a = m;                  /* (chain-4's linear layout: offset A >> 2) */
        } else if (word) {
            a = m << 1 | ((m >> ((mc & 0x20) ? 15 : 13)) & 1);
        } else {
            a = m;
        }
        if (!(mc & 0x01)) {
            a = (a & ~0x2000u) | (uint32_t)(rs & 1) << 13;      /* the CGA's banks */
        }
        if (!(mc & 0x02)) {
            a = (a & ~0x4000u) | (uint32_t)(rs & 2) << 13;      /* Hercules' */
        }
        const uint8_t *pl = vram + (a & 0xFFFF) * 4;
        if (c256) {
            for (int k = 0; k < 4; k++) {
                *o++ = pal256[pl[k]];
            }
        } else if (graphics && (r->gc[5] & 0x20)) {
            /* the CGA's two bits a pixel: plane 0 then 1 (2 and 3 the high bits) */
            for (int h = 0; h < 2; h++) {
                uint8_t lo = pl[h], hi = pl[h + 2];
                for (int k = 6; k >= 0; k -= 2) {
                    int px = ((lo >> k) & 3) | ((hi >> k) & 3) << 2;
                    *o++ = pal[px & enable];
                }
            }
        } else if (graphics) {
            uint8_t b0 = pl[0], b1 = pl[1], b2 = pl[2], b3 = pl[3];
            for (int k = 7; k >= 0; k--) {
                int px = ((b0 >> k) & 1) | ((b1 >> k) & 1) << 1 | ((b2 >> k) & 1) << 2 | ((b3 >> k) & 1) << 3;
                *o++ = pal[px & enable];
            }
        } else {
            uint8_t ch = pl[0], at = pl[1];
            uint8_t cms = r->seq[3];
            int blk_b = (cms & 3) | (vga ? (cms >> 2) & 4 : 0);
            int blk_a = ((cms >> 2) & 3) | (vga ? (cms >> 3) & 4 : 0);
            int blk = (at & 8) ? blk_a : blk_b;
            uint32_t font = vga ? (blk < 4 ? (uint32_t)blk * 0x4000 : (uint32_t)(blk - 4) * 0x4000 + 0x2000)
                                : (uint32_t)blk * 0x4000;
            uint32_t bits = rs < 32 ? vram[((font + ch * 32u + (uint32_t)rs) & 0xFFFF) * 4 + 2] : 0;
            int fg = at & 0x0F, bg = at >> 4;
            if (r->attr[0x10] & 0x08) {         /* bit 7 blinks */
                if ((bg & 8) && blink_off) {
                    fg = bg & 7;
                }
                bg &= 7;
            }
            if ((r->attr[0x10] & 0x02) && (at & 0x77) == 0x01 && rs == (c[0x14] & 0x1F)) {
                bits = 0xFF;                    /* the MDA's underline */
            }
            if (cursor_on && m == cursor) {
                bits = 0xFF;
                if (cw == 9) {
                    bits = 0x1FF;
                }
            }
            if (cw == 9 && bits <= 0xFF) {
                bits = bits << 1 | (((r->attr[0x10] & 0x04) && (ch & 0xE0) == 0xC0) ? (bits & 1) : 0);
            }
            uint16_t f = pal[fg & enable], b = pal[bg & enable];
            for (int k = cw - 1; k >= 0; k--) {
                *o++ = (bits >> k) & 1 ? f : b;
            }
        }
    }
    memcpy(dst, line + pan, (size_t)w * sizeof(uint16_t));
}

/* The planes' offsets the picture reads (the CRTC's scan from the start,
 * from 0 below a split, the CGA's and Hercules' banks, the text's fonts),
 * as bytes of the interleaved memory: what a picture copies
 */
static void span(const picture_t *p, uint32_t *from, uint32_t *to)
{
    const vga_regs_t *r = &p->vga;
    const bool vga = p->type == PC_VGA;
    const uint8_t *c = r->crtc;
    int msl = (c[9] & 0x1F) + 1, scan = vga && (c[9] & 0x80) ? 2 : 1;
    uint32_t rows = (uint32_t)(p->height / (msl * scan) + 2);
    uint32_t start = (uint32_t)(c[0x0C] << 8 | c[0x0D]);
    uint32_t lo = start, hi = start + rows * c[0x13] * 2u + c[1] + 2u;
    int lc = c[0x18] | (c[7] & 0x10) << 4 | (vga ? (c[9] & 0x40) << 3 : 0);
    if (lc < p->height) {
        lo = 0;                     /* the split: from 0 */
    }
    uint8_t mc = c[0x17];
    if (!(vga && (c[0x14] & 0x40)) && !(mc & 0x40)) {
        lo <<= 1;                   /* word mode */
        hi = hi * 2 + 2;
    }
    if (!(mc & 0x01) || !(mc & 0x02)) {
        lo = 0;                     /* the banks */
        if (hi < 0x8000) {
            hi = 0x8000;
        }
    }
    if (!(r->gc[6] & 1)) {
        uint8_t cms = r->seq[3];
        int blk[2] = { (cms & 3) | (vga ? (cms >> 2) & 4 : 0), ((cms >> 2) & 3) | (vga ? (cms >> 3) & 4 : 0) };
        for (int i = 0; i < 2; i++) {
            uint32_t font = vga ? (blk[i] < 4 ? (uint32_t)blk[i] * 0x4000 : (uint32_t)(blk[i] - 4) * 0x4000 + 0x2000)
                                : (uint32_t)blk[i] * 0x4000;
            lo = font < lo ? font : lo;
            hi = font + 0x2000 > hi ? font + 0x2000 : hi;
        }
    }
    if (hi > 0x10000) {
        lo = 0;                     /* (wraps) */
        hi = 0x10000;
    }
    *from = lo * 4;
    *to = hi * 4;
}

static const char *mode_name(const adapter_t *a)
{
    (void)a;
    static char name[40];
    geometry_t g;
    geometry(NULL, &g);
    if (!(v.r.gc[6] & 1)) {
        snprintf(name, sizeof(name), "%d x %d text", v.r.crtc[1] + 1,
                 g.height / ((v.r.crtc[9] & 0x1F) + 1) / ((v.vga && (v.r.crtc[9] & 0x80)) ? 2 : 1));
    } else {
        snprintf(name, sizeof(name), "%d x %d, %d colours", g.width,
                 (v.vga && (v.r.crtc[9] & 0x80)) || (v.r.crtc[9] & 0x1F) ? g.height / 2 : g.height,
                 (v.r.gc[5] & 0x40) ? 256 : (v.r.gc[5] & 0x20) ? 4 : (v.r.attr[0x12] & 0x0F) == 1 ? 2 : 16);
    }
    return name;
}

static void apply(picture_t *p, const change_t *ch)
{
    if (ch->reg == 0x80 && ch->index < 21) {
        p->vga.attr[ch->index] = ch->value;
    } else if (ch->reg == 0x81) {
        p->vga.pel_mask = ch->value;
    } else if (ch->reg >= 0x90 && ch->reg <= 0x92) {
        p->vga.dac[ch->index][ch->reg - 0x90] = ch->value;
    }
}

static void save(vga_regs_t *r)
{
    *r = v.r;
}

static const uint8_t *memory(void)
{
    return v.vram;
}

/* ---- The BIOS's side ---- */

uint8_t *vga_planes(void)
{
    return v.vram;
}

void vga_set_dac(int i, uint8_t red, uint8_t green, uint8_t blue)
{
    v.r.dac[i & 255][0] = red & 0x3F;
    v.r.dac[i & 255][1] = green & 0x3F;
    v.r.dac[i & 255][2] = blue & 0x3F;
}

const vga_regs_t *vga_regs(void)
{
    return &v.r;
}

static void reset(bool vga)
{
    if (v.vram == NULL) {
        v.vram = pc_calloc_pages(VRAM_SIZE);     /* (mode 13h maps it in the page table) */
    }
    uint8_t *vram = v.vram;
    memset(&v, 0, sizeof(v));
    v.vram = vram;
    if (vram) {
        memset(vram, 0, VRAM_SIZE);
    }
    v.vga = vga;
    v.r.misc = 0x67;                /* colour: the CRTC at 3D4 */
    v.r.seq[2] = 0x0F;
    v.r.seq[4] = 0x02;
    v.r.gc[6] = 0x0C;               /* B8000, as the BIOS sets it up later */
    v.r.gc[8] = 0xFF;
    v.r.attr_index = 0x20;
    v.r.attr[0x12] = 0x0F;
    v.r.pel_mask = 0xFF;
    v.mapped = -1;
    remap();
}

static void ega_map(void)
{
    reset(false);
}

static void vga_map(void)
{
    reset(true);
}

const card_t ega_card = {
    .vram = 0,
    .vram_size = VRAM_SIZE,
    .map = ega_map,
    .in = in,
    .out = out,
    .geometry = geometry,
    .render = render,
    .mode_name = mode_name,
    .apply = apply,
    .memory = memory,
    .mem_read = mem_read,
    .mem_write = mem_write,
    .save = save,
    .span = span,
};

const card_t vga_card = {
    .vram = 0,
    .vram_size = VRAM_SIZE,
    .map = vga_map,
    .in = in,
    .out = out,
    .geometry = geometry,
    .render = render,
    .mode_name = mode_name,
    .apply = apply,
    .memory = memory,
    .mem_read = mem_read,
    .mem_write = mem_write,
    .save = save,
    .span = span,
};
