/*
 * The video card's common side: the frame's timing (the 6845's totals at
 * the card's dot clock), where the beam is, and the pictures the presenter
 * draws
 *
 * At each frame's end the registers are noted and the card's memory is
 * copied on another task (pc_copy_picture(): the presenter draws some
 * frames later, by when a program may have changed the memory), with a
 * hash of what's drawn as the signature. Three copies, as esp32arc's: the
 * presenter's, the last one copied, the next one.
 */
#include <stdlib.h>
#include "video.h"

adapter_t adapter;

static const card_t *s_card;
static geometry_t s_geo;
static bool s_geo_dirty;
static uint32_t s_frame_whole, s_frame_frac, s_frame_acc;
static uint32_t s_dots_per_clk;     /* 16.16 */

typedef struct {
    picture_t pic;
    uint8_t *data;
} frame_t;

/* The pictures (in PSRAM: with the EGA's and VGA's registers they're too
 * big for the S3's internal RAM): this frame's, the three copies, drawing's
 * own with the changes applied
 */
static struct {
    picture_t now;
    frame_t frames[3];
    picture_t at;
} *s_pics;
#define s_now       (s_pics->now)
#define s_frames    (s_pics->frames)
static adapter_t s_base;            /* the registers as this frame began */
static vga_regs_t s_base_vga;
static uint32_t s_frame_bytes;      /* each copy's room */
static int s_changes;
static change_t s_change[VIDEO_CHANGES];
#define LATEST(st)      ((st) & 3u)
#define PINNED(st)      (((st) >> 2) & 3u)
#define COPYING         0x10u
static uint32_t s_state;
static int s_copying;
static uint32_t s_taken;
static uint32_t s_since_taken, s_interval = 2 << 4;
static bool s_copied;

const uint16_t cga_rgb565[16] = {
    0x0000, 0x0015, 0x0540, 0x0555, 0xA800, 0xA815, 0xAAA0, 0xAD55,
    0x52AA, 0x52BF, 0x57EA, 0x57FF, 0xFAAA, 0xFABF, 0xFFEA, 0xFFFF,
};

/* ---- Timing ---- */

static void work_out_timing(void)
{
    s_card->geometry(&adapter, &s_geo);
    uint64_t num = (uint64_t)pc.cfg.cpu_hz * (uint32_t)s_geo.dots_line * (uint32_t)s_geo.lines;
    s_frame_whole = (uint32_t)(num / s_geo.dot_hz);
    s_frame_frac = (uint32_t)(((num % s_geo.dot_hz) << 32) / s_geo.dot_hz);
    s_dots_per_clk = (uint32_t)(((uint64_t)s_geo.dot_hz << 16) / pc.cfg.cpu_hz);
    s_geo_dirty = false;
}

void video_timing_changed(void)
{
    s_geo_dirty = true;
}

void video_changed(uint8_t reg, uint8_t index, uint8_t value)
{
    int line, dot, lines, dots;
    video_beam(&line, &dot, &lines, &dots);
    if (line < s_geo.height && s_changes < VIDEO_CHANGES) {
        s_change[s_changes].line = (uint16_t)line;
        s_change[s_changes].reg = reg;
        s_change[s_changes].index = index;
        s_change[s_changes].value = value;
        s_changes++;
    }
}

uint32_t video_frame_clocks(void)
{
    if (s_geo_dirty) {
        work_out_timing();
    }
    uint32_t before = s_frame_acc;
    s_frame_acc += s_frame_frac;
    return s_frame_whole + (s_frame_acc < before ? 1 : 0);
}

void video_beam(int *line, int *dot, int *lines, int *dots)
{
    if (s_geo_dirty) {
        work_out_timing();
    }
    uint64_t into = pc_now() - pc.frame_start;
    uint32_t d = (uint32_t)((into * s_dots_per_clk) >> 16);
    *line = (int)(d / (uint32_t)s_geo.dots_line);
    *dot = (int)(d % (uint32_t)s_geo.dots_line);
    *lines = s_geo.lines;
    *dots = s_geo.dots_line;
}

int pc_screen_hz(void)
{
    if (s_card == NULL || s_geo.lines == 0) {
        return 60;
    }
    uint32_t per_frame = (uint32_t)s_geo.dots_line * (uint32_t)s_geo.lines;
    int hz = (int)((s_geo.dot_hz + per_frame / 2) / per_frame);
    return hz < 40 ? 40 : hz > 80 ? 80 : hz;
}

/* ---- The 6845 ---- */

uint8_t crtc_read(void)
{
    if (adapter.index >= 14 && adapter.index <= 17) {
        return adapter.crtc[adapter.index];
    }
    return 0;
}

void crtc_write(uint8_t v)
{
    if (adapter.index < 16) {
        adapter.crtc[adapter.index] = v;
        if (adapter.index <= 9) {
            video_timing_changed();
        }
    }
}

/* ---- Ports ---- */

bool video_in(uint16_t port, uint8_t *v)
{
    return s_card->in(port, v);
}

/* The card's memory callbacks, for pc.c's bus (NULL: none) */
uint8_t (*video_read_fn)(void *ctx, uint32_t addr);
void (*video_write_fn)(void *ctx, uint32_t addr, uint8_t v);

uint8_t video_mem_read(uint32_t addr)
{
    return video_read_fn ? video_read_fn(NULL, addr) : 0xFF;
}

void video_mem_write(uint32_t addr, uint8_t v)
{
    if (video_write_fn) {
        video_write_fn(NULL, addr, v);
    }
}

const card_t *video_card(void)
{
    return s_card;
}

static const card_t *card_of(pc_video_t type)
{
    switch (type) {
    case PC_HERCULES: return &herc_card;
    case PC_EGA: return &ega_card;
    case PC_VGA: return &vga_card;
    default: return &cga_card;
    }
}

bool video_out(uint16_t port, uint8_t v)
{
    return s_card->out(port, v);
}

const char *video_mode_name(void)
{
    return s_card ? s_card->mode_name(&adapter) : "-";     /* (not started yet) */
}

void video_reset(void)
{
    if (s_pics == NULL) {
        s_pics = pc_calloc(1, sizeof(*s_pics));
    }
    s_card = card_of(pc.cfg.video);
    video_read_fn = s_card->mem_read;
    video_write_fn = s_card->mem_write;
    memset(&adapter, 0, sizeof(adapter));
    s_base = adapter;
    s_changes = 0;
    s_card->map();
    if (s_card->save) {
        s_card->save(&s_base_vga);
    }
    s_geo_dirty = true;
    work_out_timing();
    s_frame_acc = 0;
    uint32_t bytes = s_card->vram_size < 64 * 1024 ? 64 * 1024 : s_card->vram_size;
    for (int i = 0; i < 3; i++) {
        if (s_frames[i].data && s_frame_bytes < bytes) {
            free(s_frames[i].data);
            s_frames[i].data = NULL;
        }
        if (s_frames[i].data == NULL) {
            s_frames[i].data = pc_calloc(1, bytes);
        }
        s_frames[i].pic.valid = false;
    }
    s_frame_bytes = bytes;
    memset(&s_now, 0, sizeof(s_now));
    __atomic_store_n(&s_state, 0, __ATOMIC_RELEASE);
}

/* ---- Pictures ---- */

static void queue_copy(void)
{
    /* Only the frame before the presenter's next picture (copying each
     * would take the memory from the emulation for nothing)
     */
    if (pc.cfg.picture_ready) {
        s_since_taken++;
        if (__atomic_exchange_n(&s_taken, 0, __ATOMIC_ACQUIRE)) {
            s_interval = (s_interval * 3 + (s_since_taken << 4)) / 4;
            s_since_taken = 0;
            s_copied = false;
        }
        if (s_copied || (s_since_taken + 1) << 4 < s_interval) {
            return;
        }
    }
    uint32_t st = __atomic_load_n(&s_state, __ATOMIC_ACQUIRE);
    if (st & COPYING) {
        return;
    }
    int f = 0;
    while ((uint32_t)f == LATEST(st) || (uint32_t)f == PINNED(st)) {
        f++;
    }
    s_frames[f].pic = s_now;
    s_copying = f;
    s_copied = true;
    __atomic_fetch_or(&s_state, COPYING, __ATOMIC_RELEASE);
    if (pc.cfg.picture_ready) {
        pc.cfg.picture_ready();
    } else {
        pc_copy_picture();
    }
}

void video_frame_end(void)
{
    if (s_geo_dirty) {
        work_out_timing();
    }
    s_now.valid = true;
    s_now.type = pc.cfg.video;
    s_now.regs = s_base;
    s_now.changes = s_changes;
    s_now.applied = 0;
    memcpy(s_now.change, s_change, (size_t)s_changes * sizeof(change_t));
    s_base = adapter;
    if (s_card->save) {
        s_now.vga = s_base_vga;
        s_card->save(&s_base_vga);
    }
    s_changes = 0;
    s_now.frame = pc.frames;
    s_now.width = s_geo.width;
    s_now.height = s_geo.height;
    queue_copy();
}

static uint32_t hash(const uint8_t *p, uint32_t n, uint32_t h)
{
    const uint32_t *w = (const uint32_t *)p;
    for (uint32_t i = 0; i < n / 4; i++) {
        h = (h ^ w[i]) * 0x01000193u;
    }
    return h;
}

/* The card's memory copied and hashed in one pass: reading it again for the
 * hash took as long as the copy (on the S3 its cache is shared with the
 * emulation's core, and the copy doesn't stay in it)
 */
static uint32_t copy_and_hash(uint8_t *dst, const uint8_t *src, uint32_t n, uint32_t h)
{
    uint32_t *d = (uint32_t *)(void *)dst;
    const uint32_t *s = (const uint32_t *)(const void *)src;
    uint32_t i = 0;
    for (; i + 4 <= n / 4; i += 4) {
        uint32_t a = s[i], b = s[i + 1], c = s[i + 2], e = s[i + 3];
        d[i] = a;
        d[i + 1] = b;
        d[i + 2] = c;
        d[i + 3] = e;
        h = (h ^ a) * 0x01000193u;
        h = (h ^ b) * 0x01000193u;
        h = (h ^ c) * 0x01000193u;
        h = (h ^ e) * 0x01000193u;
    }
    for (; i < n / 4; i++) {
        d[i] = s[i];
        h = (h ^ s[i]) * 0x01000193u;
    }
    return h;
}

void pc_copy_picture(void)
{
    uint32_t st = __atomic_load_n(&s_state, __ATOMIC_ACQUIRE);
    if (!(st & COPYING)) {
        return;
    }
    frame_t *fr = &s_frames[s_copying];
    picture_t *p = &fr->pic;
    if (fr->data) {
        uint32_t from = 0, to = s_card->vram_size;
        if (s_card->span) {
            s_card->span(p, &from, &to);
        }
        const uint8_t *mem = s_card->memory ? s_card->memory() : pc.ram + s_card->vram;
        uint32_t h = copy_and_hash(fr->data + from, mem + from, to - from, 0x811C9DC5u);
        h = (h ^ from) * 0x01000193u;
        const uint8_t *r = (const uint8_t *)&p->regs;
        for (uint32_t i = 0; i < sizeof(p->regs); i++) {
            h = (h ^ r[i]) * 0x01000193u;
        }
        if (s_card->save) {
            h = hash((const uint8_t *)&p->vga, sizeof(p->vga) & ~3u, h);
        }
        r = (const uint8_t *)p->change;
        for (uint32_t i = 0; i < (uint32_t)p->changes * sizeof(change_t); i++) {
            h = (h ^ r[i]) * 0x01000193u;
        }
        h = (h ^ (uint32_t)p->width) * 0x01000193u;
        h = (h ^ (uint32_t)p->height) * 0x01000193u;
        /* blinking: the cursor every 8 frames, characters every 16 (text) */
        h = (h ^ ((p->frame >> 3) & 3)) * 0x01000193u;
        p->signature = h;
    } else {
        p->valid = false;
    }
    uint32_t want;
    do {
        want = (st & ~(COPYING | 3u)) | (uint32_t)s_copying;
    } while (!__atomic_compare_exchange_n(&s_state, &st, want, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
}

static const frame_t *shown(void)
{
    return &s_frames[PINNED(__atomic_load_n(&s_state, __ATOMIC_ACQUIRE))];
}

void pc_screen_size(int *width, int *height)
{
    const picture_t *p = s_pics ? &shown()->pic : NULL;     /* (none before the first reset) */
    if (p && p->valid) {
        *width = p->width;
        *height = p->height;
    } else {
        static const int w[4] = { 640, 720, 640, 720 }, h[4] = { 200, 350, 350, 400 };
        *width = w[pc.cfg.video & 3];
        *height = h[pc.cfg.video & 3];
    }
}

uint32_t pc_screen_signature(void)
{
    if (s_pics == NULL) {
        return 0;
    }
    uint32_t st = __atomic_load_n(&s_state, __ATOMIC_ACQUIRE), want;
    do {
        want = (st & ~(3u << 2)) | LATEST(st) << 2;
    } while (!__atomic_compare_exchange_n(&s_state, &st, want, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    __atomic_store_n(&s_taken, 1, __ATOMIC_RELEASE);
    return s_frames[LATEST(st)].pic.signature;
}

static void render_line(int y, uint16_t *dst);

PC_DRAW_HOT void pc_render_line_rgb565(int y, uint16_t *dst)
{
    if (pc.cfg.render_lock) {
        pc.cfg.render_lock(true);
        render_line(y, dst);
        pc.cfg.render_lock(false);
    } else {
        render_line(y, dst);
    }
}

static PC_DRAW_HOT void render_line(int y, uint16_t *dst)
{
    if (s_pics == NULL) {
        memset(dst, 0, 640 * sizeof(uint16_t));
        return;
    }
    const frame_t *fr = shown();
    const picture_t *p = &fr->pic;
    if (!p->valid || !fr->data || y < 0 || y >= p->height) {
        int w, h;
        pc_screen_size(&w, &h);
        memset(dst, 0, (size_t)w * sizeof(uint16_t));
        return;
    }
    const card_t *card = card_of(p->type);
    if (p->changes && card->apply && p->change[0].line <= y) {
        /* the registers as the beam drew this line: the lines come in
         * order, so the changes are applied as they're reached
         */
        picture_t *at = &s_pics->at;
        static const picture_t *at_of;
        static uint32_t at_frame, at_signature;
        static int at_y;
        if (at_of != p || at_frame != p->frame || at_signature != p->signature || y < at_y) {
            *at = *p;
            at->applied = 0;
            at_of = p;
            at_frame = p->frame;
            at_signature = p->signature;
        }
        at_y = y;
        while (at->applied < p->changes && p->change[at->applied].line <= y) {
            card->apply(at, &p->change[at->applied]);
            at->applied++;
        }
        card->render(at, fr->data, y, dst);
        return;
    }
    card->render(p, fr->data, y, dst);
}
