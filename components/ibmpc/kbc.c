/*
 * The AT's 8042 keyboard controller (ports 60h, 64h) and port 61h
 *
 * 64h reads the status (output buffer full, input buffer full, system
 * flag, command or data, keyboard inhibited), takes the controller's
 * commands: the command byte (20h, 60h: IRQ 1, the system flag, keyboard
 * off, translation), self tests (AAh gives 55h, ABh 00h), keyboard off and
 * on (ADh, AEh), the input and output ports (C0h, D0h, D1h: bit 0 resets
 * the CPU, bit 1 the A20 gate), pulses (FEh: the CPU's reset). 60h reads
 * the output buffer and writes a command's data or a command for the
 * keyboard, which answers FAh (reset: FAh AAh; LEDs, typematic: FAh twice;
 * echo EEh). The keyboard's own bytes are set 1 (as the controller's
 * translation makes of the AT keyboard's set 2), a millisecond apart; IRQ 1
 * while the output buffer is full and the command byte allows it.
 *
 * Port 61h: bits 0-1 the speaker (as the XT's), 4 the DRAM refresh
 * toggling every 15 us (delay loops count it), 5 the speaker timer's output.
 */
#include "pc_internal.h"
#include "keyboard.h"

#define REPLIES     8

static uint8_t s_out;               /* the output buffer */
static bool s_full;
static uint8_t s_status_cmd;        /* bit 3 of the status: the last write was a command */
static uint8_t s_command = 0x45;    /* IRQ 1, system flag, translation */
static uint8_t s_output = 0xDF;     /* the output port: A20 open, not in reset */
static uint8_t s_pending;           /* the controller command waiting for its data (0: none) */
static uint8_t s_kbd_pending;       /* the keyboard command waiting for its data */
static bool s_kbd_enabled = true;   /* F4h / F5h */
static uint8_t s_replies[REPLIES];  /* the keyboard's (or controller's) answers, before keys */
static int s_reply_head, s_reply_count;
static uint64_t s_ready_at;

static uint32_t ms(int n)
{
    return pc.cfg.cpu_hz / 1000 * (uint32_t)n;
}

static void reply(uint8_t b)
{
    if (s_reply_count < REPLIES) {
        s_replies[(s_reply_head + s_reply_count++) % REPLIES] = b;
    }
    uint64_t at = pc_now() + ms(1) / 4;
    if (s_ready_at < at) {
        s_ready_at = at;
    }
    pc_schedule(s_ready_at);
}

static void irq(void)
{
    pic_irq(1, s_full && (s_command & 0x01));
}

static void set_output(uint8_t v)
{
    s_output = v;
    x86_set_a20(&pc.cpu, v & 0x02);
    if (!(v & 0x01)) {
        pc_cpu_reset();             /* bit 0 low: the CPU's reset line */
        s_output |= 0x01;
    }
}

void kbc_set_a20(bool open)
{
    set_output((uint8_t)((s_output & ~0x02) | (open ? 0x02 : 0)));
}

void kbc_reset(void)
{
    s_full = false;
    s_out = 0;
    s_status_cmd = 0;
    s_command = 0x45;
    s_pending = s_kbd_pending = 0;
    s_kbd_enabled = true;
    s_reply_head = s_reply_count = 0;
    s_ready_at = 0;
    set_output(0xDF);
    keyboard_reset();
    keyboard_take();                /* the POST would have taken the keyboard's AA */
    irq();
}

/* A byte waiting: the controller's or keyboard's answers first, then keys */
static bool can_send(void)
{
    if (s_full) {
        return false;
    }
    if (s_reply_count) {
        return true;
    }
    return s_kbd_enabled && !(s_command & 0x10) && keyboard_pending();
}

uint64_t kbc_next_event(void)
{
    return can_send() ? s_ready_at : NEVER;
}

void kbc_event(uint64_t now)
{
    if (!can_send() || now < s_ready_at) {
        return;
    }
    if (s_reply_count) {
        s_out = s_replies[s_reply_head];
        s_reply_head = (s_reply_head + 1) % REPLIES;
        s_reply_count--;
    } else {
        s_out = keyboard_take();
    }
    s_full = true;
    irq();
}

uint8_t kbc_read(int port)
{
    if (port == 4) {
        return (uint8_t)((s_full ? 0x01 : 0) | (s_command & 0x04) | s_status_cmd | 0x10);
    }
    uint8_t v = s_out;
    if (s_full) {
        s_full = false;
        irq();
        s_ready_at = pc_now() + ms(1);
        pc_schedule(s_ready_at);
    }
    return v;
}

static void controller_command(uint8_t v)
{
    switch (v) {
    case 0x20: reply(s_command); break;
    case 0x60: case 0xD1: s_pending = v; break;
    case 0xAA: reply(0x55); break;                  /* self test */
    case 0xAB: reply(0x00); break;                  /* interface test */
    case 0xAD: s_command |= 0x10; break;            /* keyboard off */
    case 0xAE: s_command &= (uint8_t)~0x10; break;  /* keyboard on */
    case 0xC0: reply(0xBF); break;                  /* input port: no lock, colour (bit 6) */
    case 0xD0: reply(s_output); break;
    case 0xE0: reply(0x00); break;                  /* test inputs */
    default:
        if (v >= 0xF0 && !(v & 0x01)) {
            pc_cpu_reset();                         /* pulse the reset line */
        }
        break;
    }
}

static void keyboard_command(uint8_t v)
{
    if (s_kbd_pending) {
        uint8_t cmd = s_kbd_pending;
        s_kbd_pending = 0;
        reply(0xFA);
        if (cmd == 0xF0 && v == 0) {
            reply(0x41);                            /* set 2, as translated */
        }
        return;
    }
    switch (v) {
    case 0xED: case 0xF3: case 0xF0:                /* LEDs, typematic, scan code set: a byte follows */
        s_kbd_pending = v;
        reply(0xFA);
        break;
    case 0xEE: reply(0xEE); break;                  /* echo */
    case 0xF2: reply(0xFA); reply(0xAB); reply(0x41); break;  /* the MF II keyboard's ID */
    case 0xF4: s_kbd_enabled = true; reply(0xFA); break;
    case 0xF5: s_kbd_enabled = false; reply(0xFA); break;
    case 0xFF:                                      /* reset: its self test */
        keyboard_reset();
        keyboard_take();
        s_kbd_enabled = true;
        reply(0xFA);
        reply(0xAA);
        break;
    default:
        reply(0xFA);
        break;
    }
}

void kbc_write(int port, uint8_t v)
{
    if (port == 4) {
        s_status_cmd = 0x08;
        controller_command(v);
        return;
    }
    s_status_cmd = 0;
    if (s_pending) {
        uint8_t cmd = s_pending;
        s_pending = 0;
        if (cmd == 0x60) {
            s_command = v;
            irq();
        } else {
            set_output(v);
        }
        return;
    }
    s_command &= (uint8_t)~0x10;                    /* writing to the keyboard enables it */
    keyboard_command(v);
}

/* ---- Port 61h ---- */

uint8_t port61_read(void)
{
    uint64_t t = pc_ticks(pc_now());
    uint8_t v = ppi_portb & 0x0F;
    if ((t / 18) & 1) {
        v |= 0x10;                  /* refresh: a request every 18 ticks (15 us) */
    }
    if (pit_out2()) {
        v |= 0x20;
    }
    return v;
}

void port61_write(uint8_t v)
{
    if (pc.cfg.board != PC_AT) {
        ppi_write(1, v);
        return;
    }
    if ((ppi_portb ^ v) & 0x03) {
        speaker_run();
    }
    ppi_portb = v & 0x0F;
    pit_gate2(v & 0x01);
}
