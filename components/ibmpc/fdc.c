/*
 * The floppy controller as the PC wires it: a uPD765 (../esp32emu's
 * upd765) behind the digital output register, with DMA channel 2 and IRQ 6
 *
 * Ports: 3F2 the digital output register (bits 0-1 the drive selected, 2
 * the controller out of reset, 3 DMA and interrupts on, 4-7 the motors),
 * 3F4 the main status, 3F5 the data; on the AT 3F7 the data rate (written)
 * and the disk change line (bit 7, read). The images are disk.c's, sector
 * by sector through upd765_disk_attach(). Programs that drive the
 * controller themselves use this; the BIOS's INT 13h reads the images
 * directly. The interrupt comes a millisecond after the controller raises
 * it (the work itself takes no time); the change line clears with a step.
 */
#include "pc_internal.h"
#include "upd765.h"

static uint8_t s_dor;
static uint8_t s_rate;
static bool s_line;                 /* the controller's INT */
static uint64_t s_irq_at = NEVER;   /* when IRQ 6 follows it */
static bool s_changed[PC_FLOPPIES];

static uint32_t lba(int drive, int c, int h, int r)
{
    const disk_t *d = &disks[drive];
    return ((uint32_t)c * d->heads + (uint32_t)h) * d->sectors + (uint32_t)(r - 1);
}

static bool raw_read(int drive, int c, int h, int r, uint8_t *buf)
{
    return disk_read(drive, lba(drive, c, h, r), 1, buf);
}

static bool raw_write(int drive, int c, int h, int r, const uint8_t *buf)
{
    return disk_write(drive, lba(drive, c, h, r), 1, buf);
}

static const upd765_raw_t s_raw = { raw_read, raw_write };

static int dma(bool to_memory, uint8_t *buf, int len, bool *tc)
{
    if (!(s_dor & 0x08)) {
        *tc = false;
        return 0;
    }
    return dma_transfer(2, buf, len, to_memory, tc);
}

static void update_irq(void)
{
    if (!s_line || !(s_dor & 0x08)) {
        s_irq_at = NEVER;
        pic_irq(6, false);
    } else if (s_irq_at == NEVER) {
        s_irq_at = pc_now() + pc.cfg.cpu_hz / 1000;
        pc_schedule(s_irq_at);
    }
}

static void irq(bool level)
{
    s_line = level;
    update_irq();
}

static void seek(int drive)
{
    if (drive < PC_FLOPPIES) {
        s_changed[drive] = false;
    }
}

uint64_t fdc_next_event(void)
{
    return s_irq_at;
}

void fdc_event(uint64_t now)
{
    if (now >= s_irq_at) {
        s_irq_at = NEVER;
        pic_irq(6, true);
    }
}

void fdc_disk_changed(int drive)
{
    const disk_t *d = &disks[drive];
    s_changed[drive] = true;
    if (d->present) {
        upd765_disk_attach(drive, &s_raw, d->cylinders, d->heads, d->sectors, d->readonly, d->path);
    } else {
        upd765_disk_eject(drive);
    }
}

void fdc_reset(void)
{
    static const upd765_wiring_t wiring = {
        .dma = dma, .irq = irq, .seek = seek, .four_units = true, .multitrack = true,
    };
    upd765_wire(&wiring);
    s_dor = 0;
    s_rate = 0;
    s_line = false;
    s_irq_at = NEVER;
    pic_irq(6, false);
    upd765_reset();
    upd765_motor(false);
    for (int d = 0; d < PC_FLOPPIES; d++) {
        if (disks[d].present) {
            upd765_disk_attach(d, &s_raw, disks[d].cylinders, disks[d].heads, disks[d].sectors,
                               disks[d].readonly, disks[d].path);
        }
    }
}

bool fdc_in(uint16_t port, uint8_t *v)
{
    switch (port) {
    case 0x3F2:
        *v = s_dor;                 /* (not readable on the IBM's card: what was written) */
        return true;
    case 0x3F4:
        *v = upd765_status();
        return true;
    case 0x3F5:
        *v = upd765_read();
        return true;
    case 0x3F7:
        if (pc.cfg.board != PC_AT) {
            return false;
        }
        *v = (uint8_t)(((s_dor & 3) < PC_FLOPPIES && s_changed[s_dor & 3]) ? 0x80 : 0x00);
        return true;
    default:
        return false;
    }
}

bool fdc_out(uint16_t port, uint8_t v)
{
    switch (port) {
    case 0x3F2: {
        uint8_t was = s_dor;
        s_dor = v;
        if ((was & 0x04) && !(v & 0x04)) {
            s_line = false;         /* held in reset */
        } else if (!(was & 0x04) && (v & 0x04)) {
            upd765_reset();         /* out of reset: the ready-change interrupts */
        }
        upd765_motor((v & 0x30) != 0);
        update_irq();
        return true;
    }
    case 0x3F5:
        upd765_write(v);
        return true;
    case 0x3F7:
        if (pc.cfg.board != PC_AT) {
            return false;
        }
        s_rate = v & 3;             /* noted: any rate reads any of the images */
        return true;
    default:
        return false;
    }
}
