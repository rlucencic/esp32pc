/*
 * The XT's 8255 PPI (ports 60h-63h) and its keyboard port
 *
 * Port A (60h): the scan code the keyboard sent. Port B (61h): bit 0 the
 * speaker timer's gate, 1 the speaker, 3 which half of the switches port
 * C shows, 6 the keyboard's clock (held low resets it), 7 clears the
 * keyboard's byte (and IRQ 1). Port C (62h): the configuration switches
 * (SW1: memory on the board, the display, the floppy drives), bit 5 the
 * speaker timer's output.
 *
 * The keyboard sends a byte when the last one was cleared and its clock
 * is enabled, about a millisecond after (its serial transfer); IRQ 1 rises
 * with it.
 */
#include "pc_internal.h"
#include "keyboard.h"

uint8_t ppi_portb;
static uint8_t s_latch;             /* the byte the keyboard sent */
static bool s_full;
static uint64_t s_ready_at;         /* when it may send the next one */
static uint64_t s_clock_low_at;     /* when its clock was pulled low, NEVER */

static uint32_t ms(int n)
{
    return pc.cfg.cpu_hz / 1000 * (uint32_t)n;
}

void ppi_reset(void)
{
    ppi_portb = 0;
    s_latch = 0;
    s_full = false;
    s_ready_at = 0;
    s_clock_low_at = pc_now();      /* (port B 0: the clock held low from the start) */
    keyboard_reset();
    keyboard_take();                /* the BIOS's POST would have taken its AA */
}

static bool can_send(void)
{
    return !s_full && (ppi_portb & 0x40) && !(ppi_portb & 0x80) && keyboard_pending();
}

uint64_t kbd_next_event(void)
{
    return can_send() ? s_ready_at : NEVER;
}

void kbd_event(uint64_t now)
{
    if (can_send() && now >= s_ready_at) {
        s_latch = keyboard_take();
        s_full = true;
        pic_irq(1, true);
    }
}

void kbd_frame(void)
{
    keyboard_frame();
}

/* SW1 as port C shows it (a switch on reads 0): bits 0-3 the low half
 * (1 test loop off, 2 no coprocessor, 3-4 the memory banks on the board),
 * 4-7 the high half (5-6 the display, 7-8 the floppy drives less one)
 */
static uint8_t switches(void)
{
    uint8_t display = pc.cfg.video == PC_HERCULES ? 3 : pc.cfg.video == PC_CGA ? 2 : 0;
    uint8_t sw = 0x01 | 0x0C;       /* normal boot, 256 KB on the board */
    sw |= pc.cfg.fpu ? 0x02 : 0;    /* (switch 2 off: a coprocessor) */
    sw |= (uint8_t)(display << 4);
    sw |= (uint8_t)((PC_FLOPPIES - 1) << 6);
    return sw;
}

uint8_t ppi_read(int port)
{
    switch (port) {
    case 0:
        return s_latch;
    case 1:
        return ppi_portb;
    case 2: {
        uint8_t sw = switches();
        uint8_t v = (ppi_portb & 0x08) ? (sw >> 4) : (sw & 0x0F);
        if (pit_out2()) {
            v |= 0x20;
        }
        return v;
    }
    default:
        return 0xFF;
    }
}

void ppi_write(int port, uint8_t v)
{
    if (port != 1) {
        return;                     /* port 63h: the mode (the BIOS's 99h) */
    }
    uint8_t old = ppi_portb;
    if ((old ^ v) & 0x03) {
        speaker_run();              /* the sound so far with the old bits */
    }
    ppi_portb = v;
    pit_gate2(v & 0x01);
    uint64_t now = pc_now();
    if ((v & 0x80) && !(old & 0x80)) {
        s_full = false;             /* cleared: IRQ 1 falls, port A reads 0 */
        s_latch = 0;
        pic_irq(1, false);
    }
    if (!(v & 0x80) && (old & 0x80)) {
        s_ready_at = now + ms(1);
        pc_schedule(s_ready_at);
    }
    if (!(v & 0x40) && (old & 0x40)) {
        s_clock_low_at = now;
    }
    if ((v & 0x40) && !(old & 0x40)) {
        /* the clock held low 20 ms or more: the keyboard resets */
        if (s_clock_low_at != NEVER && now - s_clock_low_at >= ms(10)) {
            keyboard_reset();
        }
        s_clock_low_at = NEVER;
        s_ready_at = now + ms(1);
        pc_schedule(s_ready_at);
    }
}
