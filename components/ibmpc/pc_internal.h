/*
 * The IBM PC's parts, between themselves
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "pc.h"
#include "x86.h"

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#define PC_LOG(fmt, ...)    ESP_LOGI("pc", fmt, ##__VA_ARGS__)
/* Drawing (the presenter's task on core 0) in internal RAM on the S3: from
 * PSRAM its code took the shared instruction cache from the emulation
 */
#if CONFIG_IDF_TARGET_ESP32S3
#define PC_DRAW_HOT         IRAM_ATTR
#else
#define PC_DRAW_HOT
#endif
#define pc_calloc(n, s)     heap_caps_calloc((n), (s), MALLOC_CAP_SPIRAM)
/* Memory the CPU's page table maps (its entries' host addresses: 4 KB aligned) */
#define pc_calloc_pages(s)  heap_caps_aligned_calloc(4096, 1, (s), MALLOC_CAP_SPIRAM)
#else
#include <stdio.h>
#include <stdlib.h>
#define PC_LOG(fmt, ...)    fprintf(stderr, "pc: " fmt "\n", ##__VA_ARGS__)
#define PC_DRAW_HOT
#define pc_calloc(n, s)     calloc((n), (s))
static inline void *pc_calloc_pages(size_t s)
{
    void *p = NULL;
    if (posix_memalign(&p, 4096, s) == 0) {
        memset(p, 0, s);
    }
    return p;
}
#endif

#define PIT_HZ              1193182u    /* 14.31818 MHz / 12 */
#define NEVER               UINT64_MAX

typedef struct {
    pc_config_t cfg;
    x86_t cpu;
    uint8_t *ram;
    uint32_t frames;
    uint32_t frame_insns;           /* the last frame's instructions */
    uint64_t frame_start;           /* CPU clocks */
    uint64_t run_until;             /* where the current x86_run() ends */
    /* CPU clocks against PIT ticks: ticks = base_tick + (clk - base_clk) * mul
     * (0.32 fixed point); clk = base_clk + (tick - base_tick) * inv (16.16).
     * The bases move on each frame, which keeps the products in 64 bits.
     */
    uint32_t pit_mul, pit_inv;
    uint64_t base_clk, base_tick;
    uint32_t base_frac;             /* the tick's fraction at base_clk (0.32) */
    bool nmi_mask;                  /* the XT's port A0 */
} pc_t;

extern pc_t pc;

/* Host builds' tracing (host_main.c sets them) */
extern bool pc_log_int13;

static inline uint64_t pc_now(void)
{
    return x86_now(&pc.cpu);
}

/* PIT ticks at CPU clock `clk` (not before the frame's start) */
static inline uint64_t pc_ticks(uint64_t clk)
{
    return pc.base_tick + (((clk - pc.base_clk) * (uint64_t)pc.pit_mul + pc.base_frac) >> 32);
}

/* The first CPU clock at which PIT tick `tick` has happened */
static inline uint64_t pc_clock_of(uint64_t tick)
{
    if (tick <= pc.base_tick) {
        return pc.base_clk;
    }
    return pc.base_clk + (((tick - pc.base_tick) * (uint64_t)pc.pit_inv + 0xFFFF) >> 16) + 1;
}

/* Something will happen at CPU clock `when`: the run in progress ends
 * before it, so that the frame loop looks at the events again
 */
static inline void pc_schedule(uint64_t when)
{
    if (when < pc.run_until) {
        x86_end_run(&pc.cpu);
    }
}

/* The memory map: [addr, addr + size) to host memory with X86_NO_READ /
 * X86_NO_WRITE bits (4 KB pages); NULL host: unmapped (reads FF)
 */
void pc_map(uint32_t addr, uint32_t size, uint8_t *host, uintptr_t bits);

/* ---- 8259 interrupt controller ---- */
void pic_reset(void);
/* An interrupt line's level; a rising edge requests the interrupt */
void pic_irq(int n, bool level);
uint8_t pic_read(int chip, int port);
void pic_write(int chip, int port, uint8_t v);
uint8_t pic_inta(void);

/* ---- 8253 timer ---- */
void pit_reset(void);
uint8_t pit_read(int port);
void pit_write(int port, uint8_t v);
void pit_gate2(bool on);
bool pit_out2(void);
uint64_t pit_periods1(uint64_t from, uint64_t *next_from);
uint32_t pit_high2(uint64_t a, uint64_t b);
uint64_t pit_next_event(void);
void pit_event(uint64_t now);
void pit_rebase(void);

/* ---- The speaker ---- */
void speaker_reset(void);
/* Catch the sound up to now (before the timer or port 61h change) */
void speaker_run(void);
/* A beep from the BIOS (the bell): `frames` long */
void speaker_beep(int frames);
void speaker_frame(void);
/* The sound ring's next sample (after catching up), and the CPU clock by
 * which `samples` more will have been made
 */
uint32_t speaker_position(void);
uint64_t speaker_clock_after(uint32_t samples);

/* ---- The AdLib and the Sound Blaster (sound.c) ---- */
void sound_reset(void);
bool sound_in(uint16_t port, uint8_t *v);
bool sound_out(uint16_t port, uint8_t v);
uint64_t sound_next_event(void);
void sound_event(uint64_t now);
/* The Sound Blaster's output for the next 44.1 kHz sample (speaker.c's loop) */
int32_t sound_sample(void);
/* The speaker's task: the AdLib's sound added to n frames from ring sample `first` */
void sound_mix(int16_t *dst, int n, uint32_t first);

/* ---- The AT: 8042 keyboard controller, port 61h, the clock and CMOS ---- */
void kbc_reset(void);
uint8_t kbc_read(int port);
void kbc_write(int port, uint8_t v);
uint64_t kbc_next_event(void);
void kbc_event(uint64_t now);
/* The A20 gate as the 8042's output port has it (the BIOS's INT 15h) */
void kbc_set_a20(bool open);
uint8_t port61_read(void);
/* Port 61h's speaker bits (the XT's through its 8255) */
void port61_write(uint8_t v);
void rtc_reset(void);
uint8_t rtc_read(int port);
void rtc_write(int port, uint8_t v);
uint64_t rtc_next_event(void);
void rtc_event(uint64_t now);
uint8_t rtc_shutdown_code(void);
void rtc_set_shutdown_code(uint8_t v);
/* The AT's reset line: the CPU starts again, memory and chips as they are */
void pc_cpu_reset(void);

/* ---- 8237 DMA controllers, the floppy controller ---- */
void dma_reset(void);
uint8_t dma_read(uint16_t port);
void dma_write(uint16_t port, uint8_t v);
/* A device's block through channel n (to memory or from it): the bytes
 * moved, *tc when the channel's count ran out
 */
int dma_transfer(int n, uint8_t *buf, int len, bool to_memory, bool *tc);
void fdc_reset(void);
bool fdc_in(uint16_t port, uint8_t *v);
bool fdc_out(uint16_t port, uint8_t v);
uint64_t fdc_next_event(void);
void fdc_event(uint64_t now);
/* A floppy went in or out of drive n (disk.c) */
void fdc_disk_changed(int drive);
/* COM1 and its mouse (uart.c) */
void lpt_reset(void);
bool lpt_present(void);
bool lpt_print(uint8_t v);          /* false: busy (the byte not taken) */
bool lpt_ready(void);
bool lpt_in(uint16_t port, uint8_t *v);
bool lpt_out(uint16_t port, uint8_t v);
void uart_reset(void);
bool uart_in(uint16_t port, uint8_t *v);
bool uart_out(uint16_t port, uint8_t v);
uint64_t uart_next_event(void);
void uart_event(uint64_t now);
void uart_frame(void);
/* The AT's hard disk controller (1F0h-1F7h, 3F6h, IRQ 14) */
void hdc_reset(void);
bool hdc_in(uint16_t port, uint8_t *v);
bool hdc_out(uint16_t port, uint8_t v);
uint16_t hdc_read_data(void);
void hdc_write_data(uint16_t v);
uint64_t hdc_next_event(void);
void hdc_event(uint64_t now);

/* ---- 8255 and the XT keyboard ---- */
void ppi_reset(void);
uint8_t ppi_read(int port);
void ppi_write(int port, uint8_t v);
uint64_t kbd_next_event(void);
void kbd_event(uint64_t now);
void kbd_frame(void);
/* The port 61h bits the timer and speaker share (PPI port B) */
extern uint8_t ppi_portb;

/* ---- Video ---- */
void video_reset(void);
bool video_in(uint16_t port, uint8_t *v);
bool video_out(uint16_t port, uint8_t v);
/* This frame's CPU clocks (with the fractions carried) */
uint32_t video_frame_clocks(void);
void video_frame_end(void);
const char *video_mode_name(void);

/* The adapters: their registers (video.c keeps the picture copies) */
typedef struct {
    uint8_t crtc[18];
    uint8_t index;                  /* the 6845's register select */
    uint8_t mode, color;            /* CGA 3D8/3D9, MDA/Hercules 3B8 (and 3BF in color) */
    uint8_t config;                 /* Hercules 3BF */
} adapter_t;

extern adapter_t adapter;

/* Where the beam is: line within the frame, and whether it's in the
 * displayed part (for the status registers)
 */
void video_beam(int *line, int *dot, int *lines, int *dots);

/* ---- BIOS ---- */

/* The byte registers (x86_t.r.b holds AL AH CL CH DL DH BL BH) */
#define R_AL(c)     ((c)->r.b[0])
#define R_AH(c)     ((c)->r.b[1])
#define R_CL(c)     ((c)->r.b[2])
#define R_CH(c)     ((c)->r.b[3])
#define R_DL(c)     ((c)->r.b[4])
#define R_DH(c)     ((c)->r.b[5])
#define R_BL(c)     ((c)->r.b[6])
#define R_BH(c)     ((c)->r.b[7])

void bios_init(void);
void bios_trap(void *ctx, x86_t *c, uint8_t n);
/* The BIOS's own fonts (font.c) */
extern const uint8_t pc_font_bios8[128 * 8];
extern const uint8_t pc_font_cga8[256 * 8];
extern const uint8_t pc_font_mda14[256 * 14];
extern const uint8_t pc_font_ega14[256 * 14];

/* BIOS data area at 0040:0000 */
static inline uint8_t *bda(uint32_t off)
{
    return pc.ram + 0x400 + off;
}

static inline uint16_t bda16(uint32_t off)
{
    return (uint16_t)(bda(off)[0] | bda(off)[1] << 8);
}

static inline void set_bda16(uint32_t off, uint16_t v)
{
    bda(off)[0] = (uint8_t)v;
    bda(off)[1] = (uint8_t)(v >> 8);
}

/* The keyboard for the BIOS's INT 9 (port 60h, then acknowledge) */
void bios_video_init(void);
void bios_int10(x86_t *c);
void bios_video_mode(int mode);
void bios_teletype(uint8_t ch, uint8_t color);
/* The EGA's and VGA's (bios_vga.c): the video ROM at C0000, INT 10h */
void bios_vga_rom(void);
void bios_vga_int10(x86_t *c);
void bios_vga_mode(int mode);
void bios_vga_teletype(uint8_t ch, uint8_t color);
extern const uint8_t pc_font_vga16[256 * 16];
void bios_int13(x86_t *c);
void bios_int09(x86_t *c);
void bios_int16(x86_t *c);
void bios_disk_init(void);
void bios_keyboard_init(void);

/* INT 16h's wait: CF as the stub's loop sees it (set: no key yet) */
void bios_wait_result(x86_t *c, bool wait);
/* The ROM's fixed-disk parameter tables (INT 41h, 46h); with a real BIOS
 * in the hard disks' option ROM at C8000h
 */
#define BIOS_HD_PARAMS      0xE401
#define HD_ROM              0xC8000
#define HD_ROM_PARAMS       0x0100
/* An option ROM's checksum byte (its last): the bytes add up to 0 */
void bios_rom_checksum(uint8_t *rom, uint32_t size);
void bios_vga_init(x86_t *c);

/* Set or clear CF (and ZF) in the FLAGS an interrupt pushed (IRET restores them) */
void bios_return_cf(x86_t *c, bool cf);
void bios_return_zf(x86_t *c, bool zf);

/* ---- Disk images ---- */
typedef struct {
    char path[192];
    bool present;
    bool readonly;
    bool changed;                   /* inserted or ejected since the BIOS last looked */
    uint16_t cylinders;
    uint8_t heads, sectors;
    uint32_t total;                 /* sectors */
    uint8_t *data;                  /* floppies: the whole image in memory */
    void *file;                     /* hard disks: the open file (FILE *) */
    struct folder *folder;          /* a hard disk made from a folder (folder.c) */
    uint32_t reads, writes;
    uint8_t cylinder;               /* the last one used */
    uint32_t last_used;             /* the frame */
} disk_t;

/* 0-1 floppies A: B:, 2-3 hard disks (80h, 81h) */
extern disk_t disks[PC_FLOPPIES + PC_HARD_DISKS];
bool disk_read(int d, uint32_t lba, uint32_t count, uint8_t *dst);
bool disk_write(int d, uint32_t lba, uint32_t count, const uint8_t *src);
/* A hard disk's first sectors: the MBR (one partition of fs_sectors from
 * sector 63, 16 heads of 63 sectors), its FAT16 boot sector
 */
void disk_make_mbr(uint8_t *mbr, uint32_t fs_sectors, uint8_t type);
void disk_make_boot(uint8_t *b, uint32_t fs_sectors, uint8_t per_cluster, uint16_t fat, const char *label);

/* A folder as a read-only hard disk (folder.c): its geometry into d */
struct folder *folder_open(const char *path, disk_t *d);
bool folder_read(struct folder *fd, uint32_t lba, uint32_t count, uint8_t *dst);
void folder_close(struct folder *fd);
