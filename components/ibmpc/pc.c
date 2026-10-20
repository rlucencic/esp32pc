/*
 * The IBM PC: memory map, I/O ports, the frame loop
 *
 * Time is the CPU's clocks. A frame is one of the video card's (its 6845's
 * totals at its dot clock); within it the CPU runs to the next event (the
 * timer's interrupt, the keyboard's next byte) or the frame's end. The
 * status registers (retrace) work out the beam from the time when read.
 */
#include <stdio.h>
#include "video.h"
#include "emudbg.h"

pc_t pc;

/* ---- Memory ---- */

void pc_map(uint32_t addr, uint32_t size, uint8_t *host, uintptr_t bits)
{
    for (uint32_t a = addr; a < addr + size; a += 1u << X86_PAGE_SHIFT) {
        pc.cfg.page_table[a >> X86_PAGE_SHIFT] =
            host ? ((uintptr_t)(host + (a - addr)) | bits) : (X86_NO_READ | X86_NO_WRITE);
    }
    x86_flush(&pc.cpu);
}

/* What the page table doesn't map: nothing (reads FF), or with an EGA or
 * VGA its window (A0000-BFFFF through their planar logic): then the card's
 * own callbacks are the bus's (pc_reset())
 */
static uint8_t mem_read(void *ctx, uint32_t addr)
{
    (void)ctx;
    (void)addr;
    return 0xFF;
}

static void mem_write(void *ctx, uint32_t addr, uint8_t v)
{
    (void)ctx;
    (void)addr;
    (void)v;
}

static void map_memory(void)
{
    const pc_config_t *cfg = &pc.cfg;
    pc_map(0, PC_PAGES(cfg->board) << X86_PAGE_SHIFT, NULL, 0);
    pc_map(0, (uint32_t)cfg->conv_kb * 1024u, pc.ram, 0);
    if (cfg->video == PC_EGA || cfg->video == PC_VGA) {
        pc_map(0xC0000, 0x8000, pc.ram + 0xC0000, X86_NO_WRITE);   /* the video ROM */
    }
    if (cfg->bios_rom) {
        pc_map(HD_ROM, 0x2000, pc.ram + HD_ROM, X86_NO_WRITE);      /* the hard disks' ROM */
    }
    pc_map(0xF0000, 0x10000, pc.ram + 0xF0000, X86_NO_WRITE);
    if (cfg->board == PC_AT) {
        pc_map(0x100000, cfg->ext_kb * 1024u, pc.ram + 0x100000, 0);
        pc_map(0xFF0000, 0x10000, pc.ram + 0xF0000, X86_NO_WRITE);     /* the 286's reset */
    }
}

/* ---- I/O ---- */

static uint8_t io_in(void *ctx, uint16_t port)
{
    (void)ctx;
    uint8_t v;
    port &= 0x3FF;                  /* the ISA bus decodes 10 address bits */
    if (port < 0x100) {
        /* The motherboards decode 32-port blocks */
        bool at = pc.cfg.board == PC_AT;
        switch (port & 0xE0) {
        case 0x00: return dma_read(port & 0x0F);
        case 0x20: return pic_read(0, port & 1);
        case 0x40: return pit_read(port & 3);
        case 0x60:
            if (!at) {
                return ppi_read(port & 3);
            }
            if (port & 0x10) {
                return rtc_read(port & 1);          /* 70h-7Fh: the clock and CMOS */
            }
            if ((port & 0x0F) == 1) {
                return port61_read();
            }
            return (port & 0x0F) == 0 || (port & 0x0F) == 4 ? kbc_read(port & 0x0F) : 0xFF;
        case 0x80: return dma_read(0x80 | (port & 0x0F));
        case 0xA0: return at ? pic_read(1, port & 1) : 0xFF;
        case 0xC0: return dma_read(port);
        default:
            return 0xFF;
        }
    }
    if (hdc_in(port, &v) || fdc_in(port, &v) || uart_in(port, &v) || lpt_in(port, &v) || sound_in(port, &v) ||
        video_in(port, &v)) {
        return v;
    }
    return 0xFF;
}

/* Words: the hard disk's data port takes them whole, the rest as two bytes */
static uint16_t io_inw(void *ctx, uint16_t port)
{
    if ((port & 0x3FF) == 0x1F0 && pc.cfg.board == PC_AT) {
        return hdc_read_data();
    }
    return (uint16_t)(io_in(ctx, port) | io_in(ctx, (uint16_t)(port + 1)) << 8);
}

static void io_out(void *ctx, uint16_t port, uint8_t v)
{
    (void)ctx;
    port &= 0x3FF;
    if (port < 0x100) {
        bool at = pc.cfg.board == PC_AT;
        switch (port & 0xE0) {
        case 0x00: dma_write(port & 0x0F, v); break;
        case 0x20: pic_write(0, port & 1, v); break;
        case 0x40: pit_write(port & 3, v); break;
        case 0x60:
            if (!at) {
                ppi_write(port & 3, v);
            } else if (port & 0x10) {
                rtc_write(port & 1, v);             /* 70h-7Fh */
            } else if ((port & 0x0F) == 1) {
                port61_write(v);
            } else if ((port & 0x0F) == 0 || (port & 0x0F) == 4) {
                kbc_write(port & 0x0F, v);
            }
            break;
        case 0x80: dma_write(0x80 | (port & 0x0F), v); break;
        case 0xC0: dma_write(port, v); break;
        case 0xE0:
            if (at && (port & 0xFE) == 0xF0) {
                pic_irq(13, false);         /* F0h, F1h: the coprocessor's busy and error latch cleared */
            }
            break;
        case 0xA0:
            if (at) {
                pic_write(1, port & 1, v);
            } else {
                pc.nmi_mask = (v & 0x80) != 0;
            }
            break;
        default:
            break;
        }
        return;
    }
    if (!hdc_out(port, v) && !fdc_out(port, v) && !uart_out(port, v) && !lpt_out(port, v) && !sound_out(port, v)) {
        video_out(port, v);
    }
}

static void io_outw(void *ctx, uint16_t port, uint16_t v)
{
    if ((port & 0x3FF) == 0x1F0 && pc.cfg.board == PC_AT) {
        hdc_write_data(v);
        return;
    }
    io_out(ctx, port, (uint8_t)v);
    io_out(ctx, (uint16_t)(port + 1), (uint8_t)(v >> 8));
}

static uint8_t inta(void *ctx)
{
    (void)ctx;
    return pic_inta();
}

void pc_cpu_reset(void)
{
    x86_reset(&pc.cpu);
}

static void shutdown(void *ctx, x86_t *c)
{
    (void)ctx;
    (void)c;
    pc_cpu_reset();                 /* the AT's motherboard resets a shut down CPU */
}

/* The coprocessor's error line: the AT's IRQ 13 (until a write to port F0h
 * clears it), the XT's NMI (if port A0h lets it through)
 */
static void fpu_error(void *ctx, bool on)
{
    (void)ctx;
    if (pc.cfg.board == PC_AT) {
        pic_irq(13, on);
    } else if (on && pc.nmi_mask) {
        x86_nmi(&pc.cpu);
    }
}

static const x86_bus_t s_bus = {
    .read = mem_read,
    .write = mem_write,
    .in = io_in,
    .out = io_out,
    .inw = io_inw,
    .outw = io_outw,
    .inta = inta,
    .trap = bios_trap,
    .shutdown = shutdown,
    .fpu_error = fpu_error,
};

/* ---- The debugger ---- */

#if EMUDBG_ENABLED
static emudbg_cpu_t s_dbg_cpu;

static void dbg_where(char *buf, size_t len)
{
    int line, dot, lines, dots;
    video_beam(&line, &dot, &lines, &dots);
    snprintf(buf, len, "frame %u, line %d, CS:IP %04X:%04X", (unsigned)pc.frames, line,
             pc.cpu.seg[X86_CS].sel, pc.cpu.ip);
}

static const emudbg_machine_t s_dbg_machine = {
    .name = "IBM PC",
    .cpus = &s_dbg_cpu,
    .cpu_count = 1,
    .where = dbg_where,
    .reset = pc_reset,
    .screen = pc_screen_size,
    .render_line = pc_render_line_rgb565,
    .type = pc_type,
};
#endif

/* ---- Starting ---- */

bool pc_init(const pc_config_t *cfg)
{
    if (cfg->ram == NULL || cfg->page_table == NULL || cfg->conv_kb < 64 || cfg->conv_kb > 640 ||
        cfg->cpu_hz < 1000000) {
        return false;
    }
    pc.cfg = *cfg;
    pc.ram = cfg->ram;
    /* PIT ticks against CPU clocks: the XT's 4.77 MHz is exactly four */
    if (cfg->cpu_hz >= 4772720 && cfg->cpu_hz <= 4772730) {
        pc.pit_mul = 1u << 30;
        pc.pit_inv = 4u << 16;
    } else {
        pc.pit_mul = (uint32_t)(((uint64_t)PIT_HZ << 32) / cfg->cpu_hz);
        pc.pit_inv = (uint32_t)(((uint64_t)cfg->cpu_hz << 16) / PIT_HZ);
    }
    x86_bus_t bus = s_bus;
    if (cfg->board != PC_AT) {
        bus.shutdown = NULL;        /* the XT has nothing to reset it */
    }
    x86_init(&pc.cpu, cfg->cpu, &bus, cfg->page_table);
    x86_set_fpu(&pc.cpu, cfg->fpu);
    if (cfg->cpu == X86_80286) {
        x86_set_wait(&pc.cpu, 1);   /* the IBM AT's memory: one wait state */
    }
    /* the BIOS's traps: in its ROM, or in our option ROMs beside a real one */
    pc.cpu.trap_lo = cfg->bios_rom ? 0xC0000 : 0xF0000;
    pc.cpu.trap_hi = cfg->bios_rom ? 0xD0000 : 0x100000;
    pc_reset();
#if EMUDBG_ENABLED
    s_dbg_cpu = (emudbg_cpu_t){
        .name = cfg->cpu == X86_80286 ? "80286" : cfg->cpu == X86_8086 ? "8086" : "8088",
        .arch = &emudbg_arch_x86,
        .core = &pc.cpu,
    };
    emudbg_set_machine(&s_dbg_machine);
#endif
    PC_LOG("%s, %s at %u.%02u MHz, %u KB%s", cfg->board == PC_AT ? "PC/AT" : "PC/XT",
           cfg->cpu == X86_80286 ? "80286" : cfg->cpu == X86_8086 ? "8086" : "8088",
           (unsigned)(cfg->cpu_hz / 1000000), (unsigned)(cfg->cpu_hz / 10000 % 100), cfg->conv_kb,
           cfg->board == PC_AT ? " + extended" : "");
    return true;
}

void pc_reset(void)
{
    /* Memory clear as at power-on (the BIOS's ROM goes back in) */
    memset(pc.ram, 0, PC_RAM_BYTES(pc.cfg.board == PC_AT ? pc.cfg.ext_kb : 0));
    map_memory();
    dma_reset();
    fdc_reset();
    hdc_reset();
    uart_reset();
    lpt_reset();
    pc.nmi_mask = false;
    pc.base_clk = pc.cpu.cycles;
    pc.base_tick = 0;
    pc.base_frac = 0;
    pc.frame_start = pc.cpu.cycles;
    pic_reset();
    pit_reset();
    ppi_portb = 0;
    if (pc.cfg.board == PC_AT) {
        kbc_reset();
        rtc_set_shutdown_code(0);
        rtc_reset();
    } else {
        ppi_reset();
    }
    speaker_reset();
    sound_reset();
    video_reset();
    pc.cpu.bus.read = video_read_fn ? video_read_fn : mem_read;
    pc.cpu.bus.write = video_write_fn ? video_write_fn : mem_write;
    bios_init();
    x86_reset(&pc.cpu);
    emudbg_rearm();
}

bool pc_halted(void)
{
    return pc.cpu.stuck;
}

/* ---- The frame ---- */

void pc_run_frame(void)
{
    uint32_t insns = pc.cpu.insns;
    uint64_t end = pc.frame_start + video_frame_clocks();
    for (;;) {
        uint64_t now = pc_now();
        bool at = pc.cfg.board == PC_AT;
        pit_event(now);
        fdc_event(now);
        uart_event(now);
        sound_event(now);
        if (at) {
            kbc_event(now);
            rtc_event(now);
            hdc_event(now);
        } else {
            kbd_event(now);
        }
        if (now >= end) {
            break;
        }
        uint64_t until = end;
        uint64_t t = pit_next_event();
        if (t < until) {
            until = t;
        }
        t = at ? kbc_next_event() : kbd_next_event();
        if (t < until) {
            until = t;
        }
        t = at ? rtc_next_event() : NEVER;
        if (t < until) {
            until = t;
        }
        t = fdc_next_event();
        if (t < until) {
            until = t;
        }
        t = at ? hdc_next_event() : NEVER;
        if (t < until) {
            until = t;
        }
        t = uart_next_event();
        if (t < until) {
            until = t;
        }
        t = sound_next_event();
        if (t < until) {
            until = t;
        }
        if (until <= now) {
            until = now + 1;
        }
        pc.run_until = until;
        x86_run(&pc.cpu, (int32_t)(until - now));
    }
    pc.run_until = 0;
    pc.frame_start = end;
    speaker_run();
    /* The PIT's base moves on with the frames (keeps its products small) */
    uint64_t span = (pc_now() - pc.base_clk) * (uint64_t)pc.pit_mul + pc.base_frac;
    pc.base_tick += span >> 32;
    pc.base_frac = (uint32_t)span;
    pc.base_clk = pc_now();
    pit_rebase();

    pc.frames++;
    pc.frame_insns = pc.cpu.insns - insns;
    speaker_frame();
    kbd_frame();
    uart_frame();
    video_frame_end();
}

/* ---- Status ---- */

/* DOS's memory control blocks: 'M' (or 'Z' for the last), the owner (0:
 * free), the size in paragraphs, each block after the one before. The
 * chain's start (in DOS's List of Lists, which only DOS knows) is found by
 * looking for a chain that reaches the top of conventional memory, then
 * kept while it still holds.
 */
static uint32_t s_first_mcb;

static bool walk_mcbs(uint32_t seg, uint32_t *free_p, uint32_t *largest_p)
{
    const uint32_t top = (uint32_t)pc.cfg.conv_kb * 64;
    uint32_t free_p_sum = 0, largest = 0;
    for (int n = 0; n < 2000 && seg < top; n++) {
        const uint8_t *m = pc.ram + seg * 16;
        if (m[0] != 'M' && m[0] != 'Z') {
            return false;
        }
        uint32_t owner = (uint32_t)(m[1] | m[2] << 8), size = (uint32_t)(m[3] | m[4] << 8);
        uint32_t end = seg + 1 + size;
        if (owner == 0) {
            uint32_t below = end <= top ? size : top - seg - 1;     /* (what's below the top) */
            free_p_sum += below;
            largest = below > largest ? below : largest;
        }
        if (m[0] == 'Z') {
            if (end + 1 < top) {
                return false;       /* (ends short of the top: not DOS's chain) */
            }
            *free_p = free_p_sum;
            *largest_p = largest;
            return true;
        }
        seg = end;
    }
    /* past the top through an 'M': upper memory blocks follow (DOS=UMB) */
    *free_p = free_p_sum;
    *largest_p = largest;
    return seg >= top;
}

bool pc_dos_memory(uint32_t *free_kb, uint32_t *largest_kb)
{
    uint32_t f, l;
    bool found = s_first_mcb && walk_mcbs(s_first_mcb, &f, &l);
    for (uint32_t seg = 0x50; !found && seg < 0x2000; seg++) {
        if (pc.ram[seg * 16] == 'M' && walk_mcbs(seg, &f, &l)) {
            s_first_mcb = seg;
            found = true;
        }
    }
    if (!found) {
        s_first_mcb = 0;
        return false;
    }
    *free_kb = f / 64;
    *largest_kb = l / 64;
    return true;
}

void pc_get_status(pc_status_t *s)
{
    memset(s, 0, sizeof(*s));
    s->frames = pc.frames;
    s->insns = pc.frame_insns;
    s->cs = pc.cpu.seg[X86_CS].sel;
    s->ip = pc.cpu.ip;
    pc_screen_size(&s->width, &s->height);
    s->mode = video_mode_name();
    s->light = -1;
    for (int d = 0; d < PC_FLOPPIES + PC_HARD_DISKS; d++) {
        s->reads[d] = disks[d].reads;
        s->writes[d] = disks[d].writes;
        s->cylinder[d] = disks[d].cylinder;
        if (disks[d].present && disks[d].last_used && pc.frames - disks[d].last_used < 20) {
            s->light = d;
        }
    }
}
