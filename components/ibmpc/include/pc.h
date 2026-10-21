/*
 * IBM PC, platform independent: the PC/XT (an 8088 or 8086) and the PC/AT
 * (an 80286), with a CGA, Hercules, EGA or VGA card, on the x86 core in
 * ../esp32emu. Built in BIOS (bios*.c), or a real BIOS ROM the machine
 * gives (the built-in one's disk and video parts then as option ROMs).
 *
 * The machine gives the memory (pc_config_t.ram: the first megabyte as
 * the PC lays it out, then the extended memory) and the CPU's page table;
 * pc_init() puts the BIOS ROM in and starts it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "x86.h"

typedef enum { PC_XT, PC_AT } pc_board_t;
typedef enum { PC_CGA, PC_HERCULES, PC_EGA, PC_VGA } pc_video_t;

#define PC_FLOPPIES         2
#define PC_HARD_DISKS       2

/* The CPU's page table: the XT's 1 MB, the AT's 16 MB */
#define PC_PAGES(board)     ((board) == PC_AT ? X86_PAGES : 256u)

/* The memory block the machine gives for ext_kb of extended memory */
#define PC_RAM_BYTES(ext_kb)    ((1024u + (uint32_t)(ext_kb)) * 1024u)

typedef struct {
    pc_board_t board;
    x86_model_t cpu;
    uint32_t cpu_hz;                /* 4772727 for the XT */
    pc_video_t video;
    uint16_t conv_kb;               /* conventional memory: up to 640 */
    uint32_t ext_kb;                /* above 1 MB (the AT) */
    uint8_t *ram;                   /* PC_RAM_BYTES(ext_kb), 4 KB aligned */
    uintptr_t *page_table;          /* PC_PAGES(board) entries */
    /* A real BIOS ROM instead of the built-in BIOS (NULL: built in): 8 to
     * 64 KB, at the top of the first megabyte (the AT's also at the top of
     * 16 MB). Its POST finds ours as option ROMs: the hard disks' INT 13h
     * at C8000h, the EGA's and VGA's BIOS at C0000h.
     */
    const uint8_t *bios_rom;
    uint32_t bios_size;
    /* A frame's picture is ready to copy: from the emulation task, so that
     * another calls pc_copy_picture() (NULL: it's copied right away)
     */
    void (*picture_ready)(void);
    /* Drawing from more than one task (the presenter, a screenshot, the
     * debugger): taken around each line, as the drawing keeps state
     * between lines (NULL: one task draws)
     */
    void (*render_lock)(bool take);
    /* LPT1's printer: a byte printed, from the emulation task; false if it
     * can't take it now (the port is busy till it does). NULL: no LPT1.
     */
    bool (*printer)(uint8_t byte);
    /* A coprocessor in its socket: the 8087, the AT's 287 (its errors: the
     * AT's IRQ 13, the XT's NMI)
     */
    bool fpu;
} pc_config_t;

bool pc_init(const pc_config_t *cfg);
/* Cold reset: memory cleared, the BIOS starts again */
void pc_reset(void);
/* One frame of the video card (CGA 59.92 Hz, MDA 49.82 Hz) */
void pc_run_frame(void);
/* The CPU stopped for good (a shutdown with nothing to reset it: the XT) */
bool pc_halted(void);

/* ---- The picture (the presenter's task) ---- */
void pc_screen_size(int *width, int *height);
int pc_screen_hz(void);
uint32_t pc_screen_signature(void);
void pc_render_line_rgb565(int y, uint16_t *dst);
/* Copy the frame pc_config_t.picture_ready announced (another task) */
void pc_copy_picture(void);

/* ---- Sound (the speaker's task): 16-bit stereo ---- */
#define PC_AUDIO_HZ         44100
void pc_audio_enable(bool on);
int pc_audio_pending(void);
int pc_audio_render(int16_t *dst, int frames);

/* ---- Keyboard (any task): scan code set 1, an extended key 0xE0xx ---- */
void pc_key(uint16_t code, bool down);
/* Type text ('\n' is Enter) on the keyboard */
void pc_type(const char *text);
/* The mouse on COM1 (any task): movement in mickeys (right, down), buttons
 * bit 0 left, bit 1 right
 */
void pc_mouse(int dx, int dy, uint8_t buttons);

/* ---- Disks (emulation paused) ---- */
bool pc_floppy_insert(int drive, const char *path);
void pc_floppy_eject(int drive);
const char *pc_floppy_path(int drive);
bool pc_floppy_write_protected(int drive);
/* A formatted blank floppy (FAT12) of 360, 720, 1200 or 1440 KB */
bool pc_floppy_create(const char *path, uint32_t bytes);

/* An image, or a folder: a read-only disk made from its files (folder.c) */
bool pc_hd_insert(int drive, const char *path);
void pc_hd_eject(int drive);
const char *pc_hd_path(int drive);
bool pc_hd_write_protected(int drive);  /* (a read-only file, or a folder) */
uint32_t pc_hd_size(int drive);
/* A blank hard disk: partitioned and formatted (FAT12 up to 16 MB, else
 * FAT16), cylinders of 16 heads x 63 sectors; bytes a multiple of those
 */
bool pc_hd_create(const char *path, uint32_t bytes);

/* A disk's free space and size in bytes as its FAT counts them (the data
 * area; a hard disk's FAT partitions added up), read from the image now:
 * not while pc_run_frame() runs on another task. n: 0-1 floppies, 2-3 hard
 * disks. False when empty, a folder or not FAT.
 */
bool pc_disk_space(int n, uint64_t *free_bytes, uint64_t *size);

/* ---- Status ---- */
typedef struct {
    uint32_t frames;
    uint32_t insns;                 /* the last frame's */
    uint16_t cs, ip;
    int width, height;
    const char *mode;               /* "80 x 25 text", ... */
    int light;                      /* drive used lately (0-1 floppies, 2-3 hard disks), -1 */
    uint32_t reads[PC_FLOPPIES + PC_HARD_DISKS], writes[PC_FLOPPIES + PC_HARD_DISKS];
    uint8_t cylinder[PC_FLOPPIES + PC_HARD_DISKS];
} pc_status_t;

void pc_get_status(pc_status_t *status);
/* DOS's free conventional memory and its largest free block (KB), from
 * its memory control blocks; false while there's no DOS (any task: it only
 * reads the PC's memory)
 */
bool pc_dos_memory(uint32_t *free_kb, uint32_t *largest_kb);
