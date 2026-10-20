#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "pc.h"

/* The machine, kept in NVS (namespace "pc") */
typedef struct {
    uint8_t machine;        /* MACHINE_XT, MACHINE_AT, MACHINE_CUSTOM */
    uint8_t cpu;            /* x86_model_t */
    uint16_t khz;           /* its clock: 4772, 6000, 8000, 12000 */
    uint8_t fpu;            /* 1: the coprocessor beside it (the 8087, the 287) */
    uint8_t video;          /* pc_video_t: CGA, Hercules, EGA, VGA */
    uint16_t ram_kb;        /* conventional: 256, 512, 640 */
    uint16_t ext_kb;        /* extended (the 286): 0, 1024, ... */
    uint8_t bios;           /* 0 the built-in BIOS, 1 a ROM file (pcxt.rom, pcat.rom with the images) */
} settings_t;

enum { MACHINE_XT, MACHINE_AT, MACHINE_CUSTOM };

extern settings_t settings;

/* The processors each machine offers, and the memory each processor */
typedef struct {
    uint8_t cpu;
    uint16_t khz;
    bool fpu;
    const char *name;       /* the setup screen's button */
} processor_t;

typedef struct {
    uint16_t ram_kb, ext_kb;
    const char *name;
} memory_t;

const processor_t *settings_processors(int machine, int *count);
const memory_t *settings_memories(int cpu, int *count);

/* The machine's defaults (choosing a machine on the setup screen) */
void settings_defaults(int machine);

void settings_load(void);
void settings_save(void);
