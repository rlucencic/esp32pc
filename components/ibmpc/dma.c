/*
 * The 8237 DMA controllers and their page registers
 *
 * The XT has one (channels 0-3, ports 00-0F: channel 0 the memory refresh,
 * 2 the floppy), the AT a second for words (channels 4-7, ports C0-DF at
 * even addresses; 4 the cascade). Page registers 80-8F: 87 81 82 83 for
 * channels 0-3, 8F 8B 89 8A for 4-7 (the 8237 doesn't carry into them: a
 * transfer wraps within its 64 KB, or 128 KB for words), the others kept
 * for reading back (80: POST codes).
 *
 * A device moves a block at once (dma_transfer()), as far as the channel's
 * count goes: the channel's mode decides the direction (a mismatched one
 * moves nothing, as verify does), the address steps up or down, and at the
 * count's end the terminal count shows in the status and the channel
 * reloads (autoinit) or masks itself. Memory as the page table maps it,
 * without the CPU's A20 gate.
 */
#include "pc_internal.h"

typedef struct {
    uint16_t base_addr, base_count;
    uint16_t addr, count;
    uint8_t mode;
} channel_t;

typedef struct {
    channel_t ch[4];
    uint8_t mask;                   /* bits 0-3 */
    uint8_t status;                 /* bits 0-3 terminal count reached, 4-7 requests */
    uint8_t command;
    bool flipflop;                  /* the next byte of a 16-bit register is the high one */
} controller_t;

static controller_t s_dma[2];
static uint8_t s_page[16];          /* ports 80-8F */
static uint64_t s_refresh_from;     /* PIT ticks counted into channel 0 */

/* Each channel's page register (index into s_page) */
static const uint8_t s_page_port[8] = { 7, 3, 1, 2, 15, 11, 9, 10 };

/* The XT's memory refresh: a cycle on channel 0 at each of the timer's
 * channel 1 periods (when it's unmasked), as the BIOS's POST checks it
 * counts (its terminal count); worked out when its registers are read
 */
static void refresh(void)
{
    uint64_t n = pit_periods1(s_refresh_from, &s_refresh_from);
    controller_t *d = &s_dma[0];
    channel_t *ch = &d->ch[0];
    if (n == 0 || (d->mask & 1) || (d->command & 0x04) || pc.cfg.board == PC_AT) {
        return;
    }
    int16_t step = (ch->mode & 0x20) ? -1 : 1;
    while (n) {
        uint64_t k = n <= ch->count ? n : (uint64_t)ch->count + 1;  /* to the end of the count */
        ch->addr = (uint16_t)(ch->addr + step * (int32_t)k);
        if (k <= ch->count) {
            ch->count = (uint16_t)(ch->count - k);
            break;
        }
        d->status |= 0x01;          /* terminal count */
        if (!(ch->mode & 0x10)) {
            ch->count = 0xFFFF;
            d->mask |= 1;
            break;
        }
        ch->addr = ch->base_addr;   /* autoinit */
        ch->count = ch->base_count;
        n -= k;
        n %= (uint64_t)ch->base_count + 1;
    }
}

void dma_reset(void)
{
    s_refresh_from = 0;
    memset(s_dma, 0, sizeof(s_dma));
    memset(s_page, 0, sizeof(s_page));
    s_dma[0].mask = s_dma[1].mask = 0x0F;
}

static uint8_t reg_read(controller_t *d, int reg)
{
    if (reg < 8) {
        channel_t *ch = &d->ch[reg >> 1];
        uint16_t v = (reg & 1) ? ch->count : ch->addr;
        bool high = d->flipflop;
        d->flipflop = !d->flipflop;
        return (uint8_t)(high ? v >> 8 : v);
    }
    switch (reg) {
    case 8: {                       /* status: reading clears the terminal counts */
        uint8_t v = d->status;
        d->status &= 0xF0;
        return v;
    }
    case 15:
        return (uint8_t)(0xF0 | d->mask);
    default:
        return 0xFF;
    }
}

static void reg_write(controller_t *d, int reg, uint8_t v)
{
    if (reg < 8) {
        channel_t *ch = &d->ch[reg >> 1];
        uint16_t *base = (reg & 1) ? &ch->base_count : &ch->base_addr;
        uint16_t *cur = (reg & 1) ? &ch->count : &ch->addr;
        if (d->flipflop) {
            *base = (uint16_t)((*base & 0x00FF) | v << 8);
        } else {
            *base = (uint16_t)((*base & 0xFF00) | v);
        }
        *cur = *base;
        d->flipflop = !d->flipflop;
        return;
    }
    switch (reg) {
    case 8: d->command = v; break;
    case 9:                         /* request (software): a request bit */
        if (v & 4) {
            d->status |= (uint8_t)(0x10 << (v & 3));
        } else {
            d->status &= (uint8_t)~(0x10 << (v & 3));
        }
        break;
    case 10:                        /* single mask bit */
        if (v & 4) {
            d->mask |= (uint8_t)(1 << (v & 3));
        } else {
            d->mask &= (uint8_t)~(1 << (v & 3));
        }
        break;
    case 11: d->ch[v & 3].mode = v; break;
    case 12: d->flipflop = false; break;
    case 13:                        /* master clear */
        d->command = 0;
        d->status = 0;
        d->mask = 0x0F;
        d->flipflop = false;
        break;
    case 14: d->mask = 0; break;
    case 15: d->mask = v & 0x0F; break;
    default: break;
    }
}

uint8_t dma_read(uint16_t port)
{
    if (port < 0x10) {
        if (port < 2 || port == 8) {
            refresh();
        }
        if (port == 2 || port == 3 || port == 8) {
            speaker_run();          /* the Sound Blaster's channel as far as now */
        }
        return reg_read(&s_dma[0], port);
    }
    if (port >= 0x80 && port < 0x90) {
        return s_page[port & 15];
    }
    if (port >= 0xC0 && port < 0xE0 && pc.cfg.board == PC_AT) {
        return reg_read(&s_dma[1], (port - 0xC0) >> 1);
    }
    return 0xFF;
}

void dma_write(uint16_t port, uint8_t v)
{
    if (port < 0x10) {
        reg_write(&s_dma[0], port, v);
    } else if (port >= 0x80 && port < 0x90) {
        s_page[port & 15] = v;
    } else if (port >= 0xC0 && port < 0xE0 && pc.cfg.board == PC_AT) {
        reg_write(&s_dma[1], (port - 0xC0) >> 1, v);
    }
}

static uint8_t *host(uint32_t addr, bool write)
{
    uint32_t page = addr >> X86_PAGE_SHIFT;
    if (page >= PC_PAGES(pc.cfg.board)) {
        return NULL;
    }
    uintptr_t e = pc.cfg.page_table[page];
    if (e & (write ? X86_NO_WRITE : X86_NO_READ)) {
        return NULL;
    }
    return (uint8_t *)(e & ~(uintptr_t)0xFFF) + (addr & 0xFFF);
}

int dma_transfer(int n, uint8_t *buf, int len, bool to_memory, bool *tc)
{
    *tc = false;
    controller_t *d = &s_dma[n >> 2];
    channel_t *ch = &d->ch[n & 3];
    if (n >= 4 && pc.cfg.board != PC_AT) {
        return 0;
    }
    if ((d->mask >> (n & 3)) & 1 || (d->command & 0x04)) {
        return 0;                   /* masked, or the controller off */
    }
    int type = (ch->mode >> 2) & 3; /* 0 verify, 1 to memory, 2 from memory */
    bool move = (type == 1 && to_memory) || (type == 2 && !to_memory);
    bool words = n >= 4;
    int step = (ch->mode & 0x20) ? -1 : 1;
    uint32_t page = s_page[s_page_port[n]];
    int done = 0;
    while (done < len) {
        uint32_t addr = words ? ((page & 0xFE) << 16) | ((uint32_t)ch->addr << 1) : (page << 16) | ch->addr;
        for (int k = 0; k < (words ? 2 : 1) && done < len; k++, done++) {
            if (move) {
                uint8_t *p = host(addr + (uint32_t)k, to_memory);
                if (to_memory) {
                    if (p) {
                        *p = buf[done];
                    }
                } else {
                    buf[done] = p ? *p : 0xFF;
                }
            }
        }
        ch->addr = (uint16_t)(ch->addr + step);
        if (ch->count-- == 0) {
            *tc = true;
            d->status |= (uint8_t)(1 << (n & 3));
            if (ch->mode & 0x10) {
                ch->addr = ch->base_addr;
                ch->count = ch->base_count;
            } else {
                d->mask |= (uint8_t)(1 << (n & 3));
            }
            break;
        }
    }
    return done;
}
