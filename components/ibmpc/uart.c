/*
 * COM1 (an 8250/16450 at 3F8h, IRQ 4) with a Microsoft mouse on it
 *
 * The UART: the divisor latch, the line and modem control (OUT2 gates the
 * interrupt, loopback), line and modem status, the scratch register, the
 * four interrupt sources in their priority. Bytes arrive at the line's
 * speed (its divisor and frame).
 *
 * The mouse, powered by DTR and RTS as drivers wake it: 'M' when they come
 * on, then three bytes for each movement or button change (1200 baud, 7N1,
 * as the drivers set the line): 01LRYYXX, 00XXXXXX, 00YYYYYY. pc_mouse()
 * (any task) adds movement and sets the buttons; the frame's end turns it
 * into packets while the line is free.
 */
#include "pc_internal.h"

#define QUEUE   16

static struct {
    uint8_t ier, lcr, mcr, lsr, msr, scr, dll, dlm;
    uint8_t rbr;
    bool thre_pending;              /* the transmitter empty interrupt, not yet seen */
    uint8_t queue[QUEUE];           /* the mouse's bytes, waiting for the line */
    int head, count;
    uint64_t next;                  /* the next byte's arrival (NEVER: none on the way) */
    uint8_t buttons;                /* as last sent */
} u;

/* From other tasks (s_pressed: buttons pressed since the last packet, so
 * that a click between two of them is seen)
 */
static int s_dx, s_dy;
static uint8_t s_buttons, s_pressed;

static bool powered(void)
{
    return (u.mcr & 0x03) == 0x03 && !(u.mcr & 0x10);
}

/* The interrupt pending, as IIR reports it (1: none) */
static uint8_t pending(void)
{
    if ((u.ier & 0x04) && (u.lsr & 0x1E)) {
        return 0x06;                /* line status */
    }
    if ((u.ier & 0x01) && (u.lsr & 0x01)) {
        return 0x04;                /* data received */
    }
    if ((u.ier & 0x02) && u.thre_pending) {
        return 0x02;                /* transmitter empty */
    }
    if ((u.ier & 0x08) && (u.msr & 0x0F)) {
        return 0x00;                /* modem status */
    }
    return 0x01;
}

static void update_irq(void)
{
    pic_irq(4, (u.mcr & 0x08) && pending() != 0x01);
}

/* A byte's time on the line, in CPU clocks */
static uint64_t byte_clocks(void)
{
    uint32_t divisor = (uint32_t)u.dlm << 8 | u.dll;
    if (divisor == 0) {
        divisor = 96;               /* (not set: 1200 baud) */
    }
    uint32_t bits = 1 + 5 + (u.lcr & 3) + ((u.lcr & 0x08) ? 1 : 0) + ((u.lcr & 0x04) ? 2 : 1);
    return (uint64_t)bits * divisor * pc.cfg.cpu_hz / 115200;
}

static void receive(uint8_t v)
{
    if (u.lsr & 0x01) {
        u.lsr |= 0x02;              /* overrun */
    }
    u.rbr = v;
    u.lsr |= 0x01;
    update_irq();
}

static void send_later(void)
{
    if (u.count && u.next == NEVER) {
        u.next = pc_now() + byte_clocks();
        pc_schedule(u.next);
    }
}

static void queue(uint8_t v)
{
    if (u.count < QUEUE) {
        u.queue[(u.head + u.count++) % QUEUE] = v;
    }
    send_later();
}

uint64_t uart_next_event(void)
{
    return u.next;
}

void uart_event(uint64_t now)
{
    if (now < u.next) {
        return;
    }
    u.next = NEVER;
    if (u.count) {
        receive(u.queue[u.head]);
        u.head = (u.head + 1) % QUEUE;
        u.count--;
        send_later();
    }
}

/* The frame's end: the mouse's movement into a packet, when the line is free */
void uart_frame(void)
{
    if (!powered() || u.count) {
        return;
    }
    uint8_t buttons = (uint8_t)(__atomic_load_n(&s_buttons, __ATOMIC_ACQUIRE) |
                                __atomic_exchange_n(&s_pressed, 0, __ATOMIC_ACQ_REL));
    int dx = __atomic_exchange_n(&s_dx, 0, __ATOMIC_ACQ_REL);
    int dy = __atomic_exchange_n(&s_dy, 0, __ATOMIC_ACQ_REL);
    int sx = dx < -128 ? -128 : dx > 127 ? 127 : dx;
    int sy = dy < -128 ? -128 : dy > 127 ? 127 : dy;
    if (sx != dx) {
        __atomic_fetch_add(&s_dx, dx - sx, __ATOMIC_ACQ_REL);     /* the rest next time */
    }
    if (sy != dy) {
        __atomic_fetch_add(&s_dy, dy - sy, __ATOMIC_ACQ_REL);
    }
    if (sx == 0 && sy == 0 && buttons == u.buttons) {
        return;
    }
    u.buttons = buttons;
    uint8_t x = (uint8_t)sx, y = (uint8_t)sy;
    queue((uint8_t)(0x40 | ((buttons & 1) ? 0x20 : 0) | ((buttons & 2) ? 0x10 : 0) | ((y >> 4) & 0x0C) | (x >> 6)));
    queue((uint8_t)(x & 0x3F));
    queue((uint8_t)(y & 0x3F));
}

void pc_mouse(int dx, int dy, uint8_t buttons)
{
    __atomic_fetch_add(&s_dx, dx, __ATOMIC_ACQ_REL);
    __atomic_fetch_add(&s_dy, dy, __ATOMIC_ACQ_REL);
    __atomic_store_n(&s_buttons, buttons, __ATOMIC_RELEASE);
    __atomic_fetch_or(&s_pressed, buttons, __ATOMIC_ACQ_REL);
}

static void set_mcr(uint8_t v)
{
    bool was = powered();
    u.mcr = v & 0x1F;
    if (u.mcr & 0x10) {
        /* loopback: the outputs come back as the inputs */
        uint8_t m = (uint8_t)(((v & 0x02) ? 0x10 : 0) | ((v & 0x01) ? 0x20 : 0) |
                              ((v & 0x04) ? 0x40 : 0) | ((v & 0x08) ? 0x80 : 0));
        u.msr = (uint8_t)(m | ((m ^ u.msr) >> 4 & 0x0F) | (u.msr & 0x0F));
    } else {
        u.msr &= 0x0F;
    }
    if (powered() && !was) {
        /* the mouse comes on: its reset, then 'M' */
        u.count = 0;
        u.next = NEVER;
        u.buttons = 0;
        __atomic_store_n(&s_dx, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&s_dy, 0, __ATOMIC_RELEASE);
        queue('M');
    }
    update_irq();
}

bool uart_in(uint16_t port, uint8_t *v)
{
    if (port < 0x3F8 || port > 0x3FF) {
        return false;
    }
    bool dlab = u.lcr & 0x80;
    switch (port & 7) {
    case 0:
        if (dlab) {
            *v = u.dll;
        } else {
            *v = u.rbr;
            u.lsr &= ~0x01;
        }
        break;
    case 1: *v = dlab ? u.dlm : u.ier; break;
    case 2: {
        uint8_t id = pending();
        if (id == 0x02) {
            u.thre_pending = false; /* (reading it says so) */
        }
        *v = id;
        break;
    }
    case 3: *v = u.lcr; break;
    case 4: *v = u.mcr; break;
    case 5:
        *v = u.lsr;
        u.lsr &= ~0x1E;             /* the errors: once */
        break;
    case 6:
        *v = u.msr;
        u.msr &= 0xF0;              /* the deltas: once */
        break;
    default: *v = u.scr; break;
    }
    update_irq();
    return true;
}

bool uart_out(uint16_t port, uint8_t v)
{
    if (port < 0x3F8 || port > 0x3FF) {
        return false;
    }
    bool dlab = u.lcr & 0x80;
    switch (port & 7) {
    case 0:
        if (dlab) {
            u.dll = v;
        } else {
            if (u.mcr & 0x10) {
                receive(v);         /* loopback */
            }
            u.thre_pending = true;  /* (sent at once: the mouse takes nothing) */
        }
        break;
    case 1:
        if (dlab) {
            u.dlm = v;
        } else {
            if ((v & 0x02) && !(u.ier & 0x02)) {
                u.thre_pending = true;
            }
            u.ier = v & 0x0F;
        }
        break;
    case 3: u.lcr = v; break;
    case 4: set_mcr(v); break;
    case 7: u.scr = v; break;
    default: break;
    }
    update_irq();
    return true;
}

void uart_reset(void)
{
    memset(&u, 0, sizeof(u));
    u.lsr = 0x60;                   /* the transmitter empty */
    u.next = NEVER;
    u.dll = 0x0C;                   /* 9600 baud */
    pic_irq(4, false);
}
