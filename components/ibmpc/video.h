/*
 * The video cards, between video.c (timing, picture copies, drawing's
 * entry) and each card (cga.c, herc.c, vga.c: the EGA and the VGA)
 */
#pragma once

#include "pc_internal.h"

/* A register written while the beam drew the picture (palettes, modes):
 * from that line on
 */
typedef struct {
    uint16_t line;
    uint8_t reg, index, value;      /* the card's own numbering (card_t.apply) */
} change_t;

#define VIDEO_CHANGES   64

/* The EGA's and VGA's registers (vga.c) */
typedef struct {
    uint8_t misc;
    uint8_t seq[5];
    uint8_t gc[9];
    uint8_t crtc[25];
    uint8_t attr[21];
    uint8_t attr_index;             /* bit 5: the palette's address source (the picture on) */
    uint8_t pel_mask;
    uint8_t dac[256][3];            /* the VGA's, 6 bits a component */
} vga_regs_t;

typedef struct {
    bool valid;
    pc_video_t type;
    adapter_t regs;                 /* as the frame began */
    vga_regs_t vga;                 /* (the EGA and VGA) */
    uint32_t frame;                 /* the frame counter (blinking) */
    int width, height;
    uint32_t signature;
    int changes;
    int applied;                    /* the changes applied to this copy (drawing's own copies) */
    change_t change[VIDEO_CHANGES];
} picture_t;

typedef struct {
    uint32_t dot_hz;                /* the dot clock */
    int dots_line, lines;           /* a line's dots, the frame's lines */
    int char_w;                     /* dots a character clock */
    int width, height;              /* the picture drawn */
} geometry_t;

typedef struct {
    uint32_t vram;                  /* where its memory is (in pc.ram) and how much the picture uses */
    uint32_t vram_size;
    void (*map)(void);
    bool (*in)(uint16_t port, uint8_t *v);
    bool (*out)(uint16_t port, uint8_t v);
    void (*geometry)(const adapter_t *a, geometry_t *g);
    void (*render)(const picture_t *p, const uint8_t *vram, int y, uint16_t *dst);
    const char *(*mode_name)(const adapter_t *a);
    /* A logged change applied to a picture's copy of the registers (NULL: none logged) */
    void (*apply)(picture_t *p, const change_t *c);
    /* The card's own memory, not in pc.ram (NULL: vram in pc.ram): what a picture copies */
    const uint8_t *(*memory)(void);
    /* Memory the page table doesn't map (A0000-BFFFF): the card's (NULL: none),
     * as the CPU's bus callbacks (ctx unused; FF and nothing outside its window)
     */
    uint8_t (*mem_read)(void *ctx, uint32_t addr);
    void (*mem_write)(void *ctx, uint32_t addr, uint8_t v);
    /* Its registers for a picture (the EGA's and VGA's; NULL: adapter_t's) */
    void (*save)(vga_regs_t *r);
    /* The part of its memory a picture shows, [from, to) bytes (NULL: all) */
    void (*span)(const picture_t *p, uint32_t *from, uint32_t *to);
} card_t;

extern const card_t cga_card, herc_card, ega_card, vga_card;

/* The registers changed what the frame's timing is */
void video_timing_changed(void);

/* A register the picture shows written (after the card changed its
 * registers): logged if the beam is in the picture, so the lines below
 * show it
 */
void video_changed(uint8_t reg, uint8_t index, uint8_t value);

/* The card's memory as the CPU sees it, where the page table doesn't map it
 * (the pointers: the card's own callbacks, NULL if none; pc.c makes them the
 * CPU's bus callbacks)
 */
uint8_t video_mem_read(uint32_t addr);
void video_mem_write(uint32_t addr, uint8_t v);
extern uint8_t (*video_read_fn)(void *ctx, uint32_t addr);
extern void (*video_write_fn)(void *ctx, uint32_t addr, uint8_t v);

/* The card in use (the BIOS asks it) */
const card_t *video_card(void);

/* CRTC registers the 6845 lets be read (14-17), the others 0 */
uint8_t crtc_read(void);
void crtc_write(uint8_t v);

/* The IBM colours (RGBI, with the CGA monitor's brown) as RGB565 */
extern const uint16_t cga_rgb565[16];
