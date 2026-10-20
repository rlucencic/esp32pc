/*
 * LPT1, the parallel printer port at 378h-37Ah (the IBM printer adapter's
 * registers), when pc_config_t.printer takes the bytes.
 *
 * 378h: the data latch (reads back). 379h: the printer's status, always
 * ready unless printer() refused the last byte (then busy until it takes it
 * at a later look). 37Ah: the control lines; the strobe's rising edge (bit 0
 * written 1) prints the latch. No interrupt (IRQ 7 is the Sound Blaster's).
 */
#include "pc_internal.h"

#define STATUS_READY    0xDF        /* not busy, no acknowledge, paper, selected, no error */

static struct {
    uint8_t data, control;
    bool busy;                      /* printer() refused the byte */
    uint8_t pending;
} l;

bool lpt_present(void)
{
    return pc.cfg.printer != NULL;
}

/* Hand a byte to the printer: false while it's busy */
bool lpt_print(uint8_t v)
{
    if (!pc.cfg.printer) {
        return false;
    }
    if (l.busy) {
        l.busy = !pc.cfg.printer(l.pending);
        if (l.busy) {
            return false;
        }
    }
    l.busy = !pc.cfg.printer(v);
    l.pending = v;
    return true;
}

bool lpt_ready(void)
{
    if (l.busy) {
        l.busy = !pc.cfg.printer(l.pending);
    }
    return !l.busy;
}

void lpt_reset(void)
{
    memset(&l, 0, sizeof(l));
    l.control = 0x0C;               /* not initialising, selected */
}

bool lpt_in(uint16_t port, uint8_t *v)
{
    if (!pc.cfg.printer || port < 0x378 || port > 0x37A) {
        return false;
    }
    switch (port) {
    case 0x378: *v = l.data; break;
    case 0x379: *v = lpt_ready() ? STATUS_READY : (STATUS_READY & 0x7F); break;
    default: *v = l.control | 0xE0; break;
    }
    return true;
}

bool lpt_out(uint16_t port, uint8_t v)
{
    if (!pc.cfg.printer || port < 0x378 || port > 0x37A) {
        return false;
    }
    if (port == 0x378) {
        l.data = v;
    } else if (port == 0x37A) {
        if ((v & 0x01) && !(l.control & 0x01)) {
            lpt_print(l.data);      /* (a refused byte waits as busy) */
        }
        l.control = v & 0x1F;
    }
    return true;
}
