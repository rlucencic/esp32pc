/*
 * The AT's MC146818 clock and CMOS RAM (ports 70h, 71h; IRQ 8)
 *
 * 70h selects a register (bit 7 masks the NMI), 71h reads or writes it.
 * The time and date (00h-09h, 32h the century) come from the board's
 * clock, BCD or binary and 12 or 24 hours as register B says; setting them
 * leaves the board's clock alone. Register A's rate and B's periodic
 * interrupt give IRQ 8 (C says which, and reading it lets the next one
 * through), A's update-in-progress bit shows before each second's update,
 * D says the battery is good. The rest is the CMOS RAM, filled
 * at each reset from the machine: the floppy drives (1.44 MB), the hard
 * disks (type 47, user defined), the equipment, the memory sizes, the
 * checksum; 0Fh is the shutdown byte (how a program comes back from
 * protected mode through a reset).
 */
#include <time.h>
#include "pc_internal.h"

static uint8_t s_ram[128];
static uint8_t s_index;
static time_t s_epoch;              /* the board's clock at the reset */
static uint64_t s_epoch_clk;        /* the CPU's clocks then */

/* The clock's time: the board's, or the reset's plus the emulated seconds
 * when the machine runs faster than real time (the host build), so that it
 * ticks with the machine's own time
 */
static time_t rtc_time(void)
{
    time_t board = time(NULL);
    time_t machine = s_epoch + (time_t)((pc_now() - s_epoch_clk) / pc.cfg.cpu_hz);
    return machine > board ? machine : board;
}
static uint64_t s_next;             /* the next periodic interrupt (CPU clocks), NEVER */

static uint8_t to_bcd(int v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static uint8_t clock_value(int reg, const struct tm *tm)
{
    bool binary = s_ram[0x0B] & 0x04, h24 = s_ram[0x0B] & 0x02;
    int v;
    switch (reg) {
    case 0x00: v = tm->tm_sec; break;
    case 0x02: v = tm->tm_min; break;
    case 0x04:
        v = tm->tm_hour;
        if (!h24) {
            int h = v % 12 ? v % 12 : 12;
            uint8_t b = binary ? (uint8_t)h : to_bcd(h);
            return (uint8_t)(b | (v >= 12 ? 0x80 : 0));
        }
        break;
    case 0x06: v = tm->tm_wday + 1; break;
    case 0x07: v = tm->tm_mday; break;
    case 0x08: v = tm->tm_mon + 1; break;
    case 0x09: v = tm->tm_year % 100; break;
    case 0x32: v = (tm->tm_year + 1900) / 100; break;
    default: return s_ram[reg];
    }
    return binary ? (uint8_t)v : to_bcd(v);
}

static uint32_t period(void)
{
    int rate = s_ram[0x0A] & 0x0F;
    if (!(s_ram[0x0B] & 0x40) || rate == 0) {
        return 0;
    }
    if (rate < 3) {
        rate += 7;
    }
    uint32_t hz = 32768u >> (rate - 1);
    return pc.cfg.cpu_hz / hz;
}

static void plan(void)
{
    uint32_t p = period();
    s_next = p ? pc_now() + p : NEVER;
    if (p) {
        pc_schedule(s_next);
    }
}

static void checksum(void)
{
    uint16_t sum = 0;
    for (int i = 0x10; i <= 0x2D; i++) {
        sum = (uint16_t)(sum + s_ram[i]);
    }
    s_ram[0x2E] = (uint8_t)(sum >> 8);
    s_ram[0x2F] = (uint8_t)sum;
}

void rtc_reset(void)
{
    s_epoch = time(NULL);
    s_epoch_clk = pc_now();
    uint8_t shutdown = s_ram[0x0F];
    memset(s_ram, 0, sizeof(s_ram));
    s_ram[0x0F] = shutdown;         /* a reset keeps it: the POST reads it */
    s_ram[0x0A] = 0x26;             /* 32 kHz, 1024 Hz periodic rate */
    s_ram[0x0B] = 0x02;             /* 24 hours, BCD */
    s_ram[0x0D] = 0x80;             /* the battery is good */
    s_ram[0x10] = 0x44;             /* A: and B: 1.44 MB */
    uint8_t hd = 0;
    if (pc.cfg.bios_rom) {
        /* a real BIOS: the hard disks are the option ROM's */
    } else if (disks[PC_FLOPPIES].present) {
        hd |= 0xF0;
        s_ram[0x19] = 47;
    }
    if (disks[PC_FLOPPIES + 1].present && !pc.cfg.bios_rom) {
        hd |= 0x0F;
        s_ram[0x1A] = 47;
    }
    s_ram[0x12] = hd;
    uint8_t display = pc.cfg.video == PC_HERCULES ? 3 : pc.cfg.video == PC_CGA ? 2 : 0;
    s_ram[0x14] = (uint8_t)(0x01 | (pc.cfg.fpu ? 0x02 : 0) | (display << 4) | ((PC_FLOPPIES - 1) << 6));
    s_ram[0x15] = (uint8_t)pc.cfg.conv_kb;
    s_ram[0x16] = (uint8_t)(pc.cfg.conv_kb >> 8);
    uint32_t ext = pc.cfg.ext_kb > 0xFFFF ? 0xFFFF : pc.cfg.ext_kb;
    s_ram[0x17] = s_ram[0x30] = (uint8_t)ext;
    s_ram[0x18] = s_ram[0x31] = (uint8_t)(ext >> 8);
    s_ram[0x32] = 0x20;
    checksum();
    s_index = 0;
    s_next = NEVER;
}

uint8_t rtc_shutdown_code(void)
{
    return s_ram[0x0F];
}

void rtc_set_shutdown_code(uint8_t v)
{
    s_ram[0x0F] = v;
}

uint64_t rtc_next_event(void)
{
    return s_next;
}

void rtc_event(uint64_t now)
{
    if (s_next == NEVER || now < s_next) {
        return;
    }
    if (!(s_ram[0x0C] & 0x80)) {    /* the last one was read: the next goes through */
        s_ram[0x0C] |= 0xC0;        /* IRQF, PF */
        pic_irq(8, true);
        pic_irq(8, false);
    }
    uint32_t p = period();
    s_next = p ? s_next + p : NEVER;
    if (p && s_next <= now) {
        s_next = now + p;
    }
}

uint8_t rtc_read(int port)
{
    if (port == 0) {
        return 0xFF;
    }
    int reg = s_index & 0x7F;
    if (reg < 0x0A || reg == 0x32) {
        time_t t = rtc_time();
        struct tm tm;
        localtime_r(&t, &tm);
        return clock_value(reg, &tm);
    }
    uint8_t v = s_ram[reg];
    if (reg == 0x0A) {
        /* update in progress: the last 244 us before each second's update
         * (programs wait for it before they read the time), while the
         * divider runs and SET is off
         */
        uint64_t into = (pc_now() - s_epoch_clk) % pc.cfg.cpu_hz;
        if ((v & 0x70) == 0x20 && !(s_ram[0x0B] & 0x80) && into >= pc.cfg.cpu_hz - pc.cfg.cpu_hz / 4096) {
            v |= 0x80;
        }
    } else if (reg == 0x0C) {
        s_ram[0x0C] = 0;            /* read: cleared */
    }
    return v;
}

void rtc_write(int port, uint8_t v)
{
    if (port == 0) {
        s_index = v;
        pc.nmi_mask = (v & 0x80) != 0;
        return;
    }
    int reg = s_index & 0x7F;
    if (reg < 0x0A || reg == 0x0C || reg == 0x0D) {
        return;                     /* the time is the board's; C and D read only */
    }
    s_ram[reg] = v;
    if (reg == 0x0A || reg == 0x0B) {
        plan();
    }
}
