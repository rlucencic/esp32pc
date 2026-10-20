/*
 * 8259A interrupt controller: one on the XT (IRQ 0-7), two cascaded on the
 * AT (the second on the first's IRQ 2)
 *
 * The initialization words (edge or level triggered, the vector base,
 * auto EOI), the mask, fixed priority (IRQ 0 first) with specific and
 * non-specific EOI and rotation, reading IRR or ISR, the poll command.
 */
#include "pc_internal.h"

typedef struct {
    uint8_t irr, isr, imr;
    uint8_t lines;                  /* the inputs' levels */
    uint8_t base;                   /* ICW2: the vectors' base */
    uint8_t icw1, icw4;
    uint8_t init;                   /* initialization word expected next (0: none) */
    uint8_t lowest;                 /* lowest priority IRQ (rotation) */
    bool read_isr;                  /* OCW3: reads give ISR */
    bool poll;
    bool auto_rotate;
} pic_t;

static pic_t s_pic[2];

static int chips(void)
{
    return pc.cfg.board == PC_AT ? 2 : 1;
}

/* The bit with the highest priority in `bits` (-1: none) */
static int highest(const pic_t *p, uint8_t bits)
{
    for (int i = 1; i <= 8; i++) {
        int n = (p->lowest + i) & 7;
        if (bits & (1u << n)) {
            return n;
        }
    }
    return -1;
}

/* The request this chip would pass on: higher priority than any in service */
static int pending(const pic_t *p)
{
    int r = highest(p, p->irr & ~p->imr);
    if (r < 0) {
        return -1;
    }
    int s = highest(p, p->isr);
    if (s >= 0) {
        /* in service at the same or higher priority blocks it */
        int pr = (r - p->lowest - 1) & 7, ps = (s - p->lowest - 1) & 7;
        if (ps <= pr) {
            return -1;
        }
    }
    return r;
}

static void update(void)
{
    if (chips() == 2) {
        bool slave = pending(&s_pic[1]) >= 0;
        uint8_t was = s_pic[0].lines;
        s_pic[0].lines = slave ? (uint8_t)(was | 4) : (uint8_t)(was & ~4);
        if (slave && !(was & 4)) {
            s_pic[0].irr |= 4;
        } else if (!slave && (s_pic[0].icw1 & 0x08)) {
            s_pic[0].irr &= (uint8_t)~4;
        }
    }
    x86_set_intr(&pc.cpu, pending(&s_pic[0]) >= 0);
}

void pic_reset(void)
{
    memset(s_pic, 0, sizeof(s_pic));
    for (int i = 0; i < 2; i++) {
        s_pic[i].imr = 0xFF;
        s_pic[i].lowest = 7;
        s_pic[i].base = i ? 0x70 : 0x08;
    }
    update();
}

void pic_irq(int n, bool level)
{
    pic_t *p = &s_pic[n >> 3];
    if (n >= 8 && chips() < 2) {
        return;
    }
    uint8_t bit = (uint8_t)(1u << (n & 7));
    bool was = p->lines & bit;
    if (level) {
        p->lines |= bit;
        if (!was || (p->icw1 & 0x08)) {     /* edge, or level triggered */
            p->irr |= bit;
        }
    } else {
        p->lines &= (uint8_t)~bit;
        if (p->icw1 & 0x08) {
            p->irr &= (uint8_t)~bit;
        }
    }
    update();
}

static void eoi(pic_t *p, int n)
{
    if (n < 0) {
        n = highest(p, p->isr);
    }
    if (n >= 0) {
        p->isr &= (uint8_t)~(1u << n);
    }
}

/* The interrupt's acknowledgement: its vector, the request in service */
static uint8_t ack(pic_t *p, bool *cascade)
{
    int n = pending(p);
    *cascade = false;
    if (n < 0) {
        return (uint8_t)(p->base | 7);      /* spurious */
    }
    if (!(p->icw1 & 0x08)) {
        p->irr &= (uint8_t)~(1u << n);
    }
    if (p->icw4 & 0x02) {
        if (p->auto_rotate) {
            p->lowest = (uint8_t)n;
        }
    } else {
        p->isr |= (uint8_t)(1u << n);
    }
    *cascade = chips() == 2 && p == &s_pic[0] && n == 2;
    return (uint8_t)(p->base + n);
}

uint8_t pic_inta(void)
{
    bool cascade;
    uint8_t v = ack(&s_pic[0], &cascade);
    if (cascade) {
        v = ack(&s_pic[1], &cascade);
    }
    update();
    return v;
}

uint8_t pic_read(int chip, int port)
{
    pic_t *p = &s_pic[chip];
    if (p->poll) {
        p->poll = false;
        int n = pending(p);
        if (n < 0) {
            return 0;
        }
        bool cascade;
        ack(p, &cascade);
        update();
        return (uint8_t)(0x80 | n);
    }
    if (port) {
        return p->imr;
    }
    return p->read_isr ? p->isr : p->irr;
}

void pic_write(int chip, int port, uint8_t v)
{
    pic_t *p = &s_pic[chip];
    if (port == 0) {
        if (v & 0x10) {             /* ICW1 */
            p->icw1 = v;
            p->imr = 0;
            p->isr = 0;
            p->irr = 0;
            p->lowest = 7;
            p->read_isr = false;
            p->icw4 = 0;
            p->init = 2;
        } else if (v & 0x08) {      /* OCW3 */
            if (v & 0x02) {
                p->read_isr = v & 0x01;
            }
            p->poll = (v & 0x04) != 0;
        } else {                    /* OCW2 */
            int n = v & 7;
            switch (v >> 5) {
            case 1: eoi(p, -1); break;                                  /* non-specific EOI */
            case 3: eoi(p, n); break;                                   /* specific EOI */
            case 5: {                                                   /* rotate on EOI */
                int s = highest(p, p->isr);
                eoi(p, s);
                if (s >= 0) {
                    p->lowest = (uint8_t)s;
                }
                break;
            }
            case 4: p->auto_rotate = true; break;
            case 0: p->auto_rotate = false; break;
            case 6: p->lowest = (uint8_t)n; break;                     /* set priority */
            case 7: eoi(p, n); p->lowest = (uint8_t)n; break;          /* rotate on specific EOI */
            default: break;
            }
        }
    } else {
        switch (p->init) {
        case 2:                     /* ICW2: the vectors */
            p->base = v & 0xF8;
            p->init = (p->icw1 & 0x02) ? ((p->icw1 & 0x01) ? 4 : 0) : 3;
            break;
        case 3:                     /* ICW3: the cascade */
            p->init = (p->icw1 & 0x01) ? 4 : 0;
            break;
        case 4:                     /* ICW4 */
            p->icw4 = v;
            p->init = 0;
            break;
        default:                    /* OCW1: the mask */
            p->imr = v;
            break;
        }
    }
    update();
}
