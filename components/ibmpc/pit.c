/*
 * 8253 timer (8254 on the AT: the read-back command), at 1.193182 MHz
 *
 * Channel 0 is the system tick (its output's rising edges are IRQ 0),
 * channel 1 the DRAM refresh, channel 2 the speaker (gated by port 61h
 * bit 0, its output on port 62h/61h bit 5). The counters aren't counted
 * down: each channel keeps the tick its count started from, and its value
 * and output are worked out from the time when they're read; channel 0's
 * next rising edge is the frame loop's event.
 */
#include "pc_internal.h"

typedef struct {
    uint8_t mode;                   /* 0-5 */
    uint8_t rw;                     /* 1 LSB, 2 MSB, 3 LSB then MSB */
    bool bcd;
    bool counting;                  /* a count is loaded (and, modes 1 and 5, triggered) */
    bool null;                      /* written but not loaded yet (the status bit) */
    uint32_t reload;                /* 1-65536 */
    uint64_t start;                 /* the tick the count started from */
    bool gate;
    bool frozen;                    /* modes 0 and 4 with the gate low: the count holds */
    uint32_t frozen_at;             /* its elapsed ticks then */
    bool write_msb, read_msb;
    uint8_t lsb;
    bool latched;
    uint16_t latch;
    bool status_latched;
    uint8_t status;
    uint64_t edge;                  /* channel 0: the next rising edge (tick), NEVER */
} chan_t;

static chan_t s_ch[3];

static uint64_t now_tick(void)
{
    return pc_ticks(pc_now());
}

static uint32_t elapsed(const chan_t *c, uint64_t t)
{
    if (c->frozen) {
        return c->frozen_at;
    }
    if (t < c->start) {
        return 0;
    }
    uint64_t e = t - c->start;
    return e > 0x7FFFFFFF ? 0x7FFFFFFF : (uint32_t)e;
}

static uint16_t value_at(const chan_t *c, uint64_t t)
{
    if (!c->counting) {
        return (uint16_t)c->reload;
    }
    uint32_t e = elapsed(c, t), r = c->reload;
    switch (c->mode) {
    case 2: {
        uint32_t p = e % r;
        return (uint16_t)(r - p);
    }
    case 3: {
        uint32_t p = e % r, half = (r + 1) / 2;
        uint32_t v = r - 2 * (p < half ? p : p - half);
        return (uint16_t)(v & 0xFFFE);
    }
    default:                        /* 0, 1, 4, 5: down, wrapping at 0 */
        return (uint16_t)(r - e);
    }
}

static bool out_at(const chan_t *c, uint64_t t)
{
    uint32_t e = elapsed(c, t), r = c->reload;
    switch (c->mode) {
    case 0:
        return c->counting && e >= r;
    case 1:
        return !c->counting || e >= r;
    case 2:
        return !c->counting || !c->gate || e % r != r - 1;
    case 3:
        return !c->counting || !c->gate || e % r < (r + 1) / 2;
    default:                        /* 4, 5: low for one tick at the end */
        return !c->counting || e != r;
    }
}

/* Channel 0's next rising edge after tick t */
static void plan_edge(uint64_t t)
{
    chan_t *c = &s_ch[0];
    c->edge = NEVER;
    if (!c->counting || c->frozen) {
        return;
    }
    uint32_t r = c->reload;
    switch (c->mode) {
    case 2:
    case 3:
        if (t < c->start) {
            c->edge = c->start + r;
        } else {
            uint64_t e = t - c->start;
            c->edge = c->start + (e / r + 1) * r;
        }
        break;
    case 0:
        if (c->start + r > t) {
            c->edge = c->start + r;
        }
        break;
    case 4:
        if (c->start + r + 1 > t) {
            c->edge = c->start + r + 1;
        }
        break;
    default:
        break;
    }
    if (c->edge != NEVER) {
        pc_schedule(pc_clock_of(c->edge));
    }
}

void pit_reset(void)
{
    memset(s_ch, 0, sizeof(s_ch));
    for (int i = 0; i < 3; i++) {
        s_ch[i].reload = 65536;
        s_ch[i].rw = 3;
        s_ch[i].gate = i != 2;
        s_ch[i].edge = NEVER;
    }
}

uint64_t pit_next_event(void)
{
    return s_ch[0].edge == NEVER ? NEVER : pc_clock_of(s_ch[0].edge);
}

void pit_event(uint64_t now)
{
    chan_t *c = &s_ch[0];
    if (c->edge == NEVER) {
        return;
    }
    uint64_t t = pc_ticks(now);
    if (t < c->edge) {
        return;
    }
    pic_irq(0, true);
    pic_irq(0, false);
    if (c->mode == 2 || c->mode == 3) {
        /* the periodic modes start again from the last edge: small numbers */
        uint64_t e = t - c->start;
        c->start += e / c->reload * c->reload;
    }
    plan_edge(t);
}

void pit_rebase(void)
{
    uint64_t t = now_tick();
    for (int i = 0; i < 3; i++) {
        chan_t *c = &s_ch[i];
        if (c->counting && !c->frozen && (c->mode == 2 || c->mode == 3) && t > c->start) {
            c->start += (t - c->start) / c->reload * c->reload;
        }
    }
}

static void load(chan_t *c, int n, uint32_t count)
{
    uint64_t t = now_tick();
    c->reload = count ? count : (c->bcd ? 10000 : 65536);
    if (c->mode == 2 || c->mode == 3) {
        if (c->reload < 2) {
            c->reload = 2;
        }
    }
    c->null = false;
    c->frozen = false;
    if (c->mode == 1 || c->mode == 5) {
        c->counting = false;        /* waits for the gate's rising edge */
    } else {
        c->counting = true;
        c->start = t + 1;
        if (!c->gate && (c->mode == 0 || c->mode == 4)) {
            c->frozen = true;
            c->frozen_at = 0;
        }
    }
    if (n == 0) {
        plan_edge(t);
    }
}

uint8_t pit_read(int port)
{
    if (port == 3) {
        return 0xFF;
    }
    chan_t *c = &s_ch[port];
    if (c->status_latched) {
        c->status_latched = false;
        return c->status;
    }
    uint16_t v;
    if (c->latched) {
        v = c->latch;
    } else {
        v = value_at(c, now_tick());
    }
    uint8_t b;
    switch (c->rw) {
    case 1:
        b = (uint8_t)v;
        c->latched = false;
        break;
    case 2:
        b = (uint8_t)(v >> 8);
        c->latched = false;
        break;
    default:
        b = c->read_msb ? (uint8_t)(v >> 8) : (uint8_t)v;
        if (c->read_msb) {
            c->latched = false;
        }
        c->read_msb = !c->read_msb;
        break;
    }
    return b;
}

static void latch(chan_t *c)
{
    if (!c->latched) {
        c->latch = value_at(c, now_tick());
        c->latched = true;
        c->read_msb = false;
    }
}

void pit_write(int port, uint8_t v)
{
    if ((port == 3 && (v >> 6) == 2) || port == 2) {
        speaker_run();              /* the sound so far with the old setting */
    }
    if (port == 3) {
        int n = v >> 6;
        if (n == 3) {               /* 8254 read-back */
            if (pc.cfg.board != PC_AT) {
                return;
            }
            uint64_t t = now_tick();
            for (int i = 0; i < 3; i++) {
                if (!(v & (2 << i))) {
                    continue;
                }
                chan_t *c = &s_ch[i];
                if (!(v & 0x20)) {
                    latch(c);
                }
                if (!(v & 0x10) && !c->status_latched) {
                    c->status = (uint8_t)((out_at(c, t) ? 0x80 : 0) | (c->null ? 0x40 : 0) | c->rw << 4 |
                                          c->mode << 1 | (c->bcd ? 1 : 0));
                    c->status_latched = true;
                }
            }
            return;
        }
        chan_t *c = &s_ch[n];
        int rw = (v >> 4) & 3;
        if (rw == 0) {
            latch(c);
            return;
        }
        c->rw = (uint8_t)rw;
        c->mode = (v >> 1) & 7;
        if (c->mode > 5) {
            c->mode -= 4;
        }
        c->bcd = v & 1;
        c->counting = false;
        c->frozen = false;
        c->null = true;
        c->write_msb = false;
        c->read_msb = false;
        c->latched = false;
        if (n == 0) {
            c->edge = NEVER;
        }
        return;
    }
    chan_t *c = &s_ch[port];
    switch (c->rw) {
    case 1:
        load(c, port, v);
        break;
    case 2:
        load(c, port, (uint32_t)v << 8);
        break;
    default:
        if (!c->write_msb) {
            c->lsb = v;
            c->write_msb = true;
        } else {
            c->write_msb = false;
            load(c, port, c->lsb | (uint32_t)v << 8);
        }
        break;
    }
}

void pit_gate2(bool on)
{
    chan_t *c = &s_ch[2];
    if (on == c->gate) {
        return;
    }
    speaker_run();
    uint64_t t = now_tick();
    c->gate = on;
    switch (c->mode) {
    case 0:
    case 4:
        if (!on && c->counting) {
            c->frozen_at = elapsed(c, t);
            c->frozen = true;
        } else if (on && c->frozen) {
            c->frozen = false;
            c->start = t - c->frozen_at;
        }
        break;
    case 1:
    case 5:
    case 2:
    case 3:
        if (on && !c->null) {       /* a rising edge starts the count again */
            c->counting = true;
            c->start = t + 1;
        }
        break;
    default:
        break;
    }
}

/* Channel 1's periods (the XT's refresh requests to DMA channel 0)
 * since tick `from`: how many, and from where the next is counted
 */
uint64_t pit_periods1(uint64_t from, uint64_t *next_from)
{
    chan_t *c = &s_ch[1];
    uint64_t now = now_tick();
    *next_from = now;
    if (!c->counting || (c->mode != 2 && c->mode != 3) || now <= from) {
        return 0;
    }
    uint64_t n = (now - from) / c->reload;
    *next_from = from + n * c->reload;
    return n;
}

bool pit_out2(void)
{
    return out_at(&s_ch[2], now_tick());
}

/* Ticks in [a, b) with channel 2's output high (the speaker's integral):
 * the square wave worked out, the other modes a tick at a time
 */
uint32_t pit_high2(uint64_t a, uint64_t b)
{
    const chan_t *c = &s_ch[2];
    if (b <= a) {
        return 0;
    }
    if (c->mode == 3 && c->counting && c->gate && !c->frozen) {
        uint32_t r = c->reload, half = (r + 1) / 2;
        /* high ticks from the count's start up to t */
        uint64_t sa = a > c->start ? a - c->start : 0, sb = b > c->start ? b - c->start : 0;
        uint64_t ha = sa / r * half + ((sa % r) < half ? sa % r : half);
        uint64_t hb = sb / r * half + ((sb % r) < half ? sb % r : half);
        uint64_t before = (a < c->start ? (c->start < b ? c->start : b) - a : 0);  /* high before it starts */
        return (uint32_t)(hb - ha + before);
    }
    uint32_t n = 0;
    for (uint64_t t = a; t < b; t++) {
        n += out_at(c, t);
    }
    return n;
}
