/*
 * The BIOS's INT 13h: the floppy drives (00h, 01h) and hard disks (80h,
 * 81h) from their images, at once (no controller timing)
 *
 * Read, write, verify, format (a track filled), the drive parameters (the
 * floppies as 1.44 MB drives that read any of the formats, the hard
 * disks' geometry), the disk type, the change line (DOS's way of knowing
 * a floppy was swapped on the setup screen or the web page). The
 * controller and DMA themselves come with the programs that use them.
 */
#include "pc_internal.h"

static int hard_disks(void)
{
    int n = 0;
    for (int i = 0; i < PC_HARD_DISKS; i++) {
        n += disks[PC_FLOPPIES + i].present;
    }
    return n;
}

/* The fixed disk parameter tables (INT 41h, 46h) from the images */
void bios_disk_init(void)
{
    for (int i = 0; i < PC_HARD_DISKS; i++) {
        const disk_t *d = &disks[PC_FLOPPIES + i];
        uint8_t *t = pc.ram + (pc.cfg.bios_rom ? HD_ROM + HD_ROM_PARAMS : 0xF0000 + BIOS_HD_PARAMS) + i * 16;
        memset(t, 0, 16);
        t[0] = (uint8_t)d->cylinders;
        t[1] = (uint8_t)(d->cylinders >> 8);
        t[2] = d->heads;
        t[5] = 0xFF;                /* no write precompensation */
        t[6] = 0xFF;
        t[8] = d->heads > 8 ? 0x08 : 0x00;
        t[12] = t[0];               /* landing zone */
        t[13] = t[1];
        t[14] = d->sectors;
    }
    *bda(0x75) = (uint8_t)hard_disks();
}

/* The disk for a drive number, -1 none */
static int disk_of(uint8_t dl)
{
    if (dl < PC_FLOPPIES) {
        return dl;
    }
    if (dl >= 0x80 && dl < 0x80 + PC_HARD_DISKS) {
        /* the hard disks are numbered as they're there (C: then D:) */
        int n = dl - 0x80;
        for (int i = 0; i < PC_HARD_DISKS; i++) {
            if (disks[PC_FLOPPIES + i].present && n-- == 0) {
                return PC_FLOPPIES + i;
            }
        }
    }
    return -1;
}

static void finish(x86_t *c, uint8_t dl, uint8_t status)
{
    R_AH(c) = status;
    *bda(dl & 0x80 ? 0x74 : 0x41) = status;
    bios_return_cf(c, status != 0);
}

/* The sectors CH/CL/DH address, from the start of the disk */
static bool chs(const disk_t *d, x86_t *c, uint32_t *lba)
{
    uint32_t cyl = R_CH(c) | (uint32_t)(R_CL(c) & 0xC0) << 2;
    uint32_t sector = R_CL(c) & 0x3F, head = R_DH(c);
    if (sector == 0 || sector > d->sectors || head >= d->heads || cyl >= d->cylinders) {
        return false;
    }
    *lba = (cyl * d->heads + head) * d->sectors + sector - 1;
    return true;
}

/* Sectors to and from ES:BX (as the CPU sees memory above conventional) */
static bool transfer(x86_t *c, int n, uint32_t lba, uint32_t count, bool write)
{
    uint32_t addr = c->seg[X86_ES].base + c->r.w[X86_BX];
    uint32_t bytes = count * 512;
    disk_t *d = &disks[n];
    d->cylinder = (uint8_t)(lba / ((uint32_t)d->heads * d->sectors));
    if (addr + bytes <= (uint32_t)pc.cfg.conv_kb * 1024u) {
        return write ? disk_write(n, lba, count, pc.ram + addr) : disk_read(n, lba, count, pc.ram + addr);
    }
    uint8_t sector[512];
    for (uint32_t i = 0; i < count; i++) {
        if (write) {
            for (int k = 0; k < 512; k++) {
                sector[k] = x86_read8(c, addr + i * 512 + (uint32_t)k);
            }
            if (!disk_write(n, lba + i, 1, sector)) {
                return false;
            }
        } else {
            if (!disk_read(n, lba + i, 1, sector)) {
                return false;
            }
            for (int k = 0; k < 512; k++) {
                x86_write8(c, addr + i * 512 + (uint32_t)k, sector[k]);
            }
        }
    }
    return true;
}

bool pc_log_int13;

void bios_int13(x86_t *c)
{
    uint8_t ah = R_AH(c), dl = R_DL(c);
    if (pc_log_int13) {
        PC_LOG("INT 13h AX=%04X CX=%04X DX=%04X ES:BX=%04X:%04X", c->r.w[X86_AX], c->r.w[X86_CX],
               c->r.w[X86_DX], c->seg[X86_ES].sel, c->r.w[X86_BX]);
    }
    bool hard = dl & 0x80;
    int n = disk_of(dl);
    disk_t *d = n >= 0 ? &disks[n] : NULL;
    uint32_t lba;

    switch (ah) {
    case 0x00:                      /* reset */
    case 0x09:                      /* the fixed disk's parameters */
    case 0x0C:                      /* seek */
    case 0x0D:                      /* reset */
    case 0x11:                      /* recalibrate */
    case 0x14:                      /* diagnostics */
        if (hard && !d) {
            finish(c, dl, 0x01);
        } else {
            finish(c, dl, 0x00);
        }
        return;
    case 0x01:                      /* the last status */
        R_AH(c) = *bda(hard ? 0x74 : 0x41);
        bios_return_cf(c, R_AH(c) != 0);
        return;
    case 0x02:                      /* read */
    case 0x03:                      /* write */
    case 0x04: {                    /* verify */
        uint32_t count = R_AL(c);
        if (!d || !d->present) {
            R_AL(c) = 0;
            finish(c, dl, hard ? 0x01 : 0x80);
            return;
        }
        if (count == 0 || !chs(d, c, &lba) || lba + count > d->total) {
            R_AL(c) = 0;
            finish(c, dl, 0x04);    /* sector not found */
            return;
        }
        if (ah == 0x03 && d->readonly) {
            R_AL(c) = 0;
            finish(c, dl, 0x03);    /* write protected */
            return;
        }
        if (ah != 0x04 && !transfer(c, n, lba, count, ah == 0x03)) {
            R_AL(c) = 0;
            finish(c, dl, hard ? 0xBB : 0x04);
            return;
        }
        d->changed = false;
        finish(c, dl, 0x00);
        return;
    }
    case 0x05: {                    /* format a track: its sectors filled */
        if (!d || !d->present) {
            finish(c, dl, hard ? 0x01 : 0x80);
            return;
        }
        if (d->readonly) {
            finish(c, dl, 0x03);
            return;
        }
        uint32_t cyl = R_CH(c) | (uint32_t)(R_CL(c) & 0xC0) << 2, head = R_DH(c);
        if (cyl >= d->cylinders || head >= d->heads) {
            finish(c, dl, 0x04);
            return;
        }
        uint8_t fill[512];
        memset(fill, hard ? 0x00 : 0xF6, sizeof(fill));
        lba = (cyl * d->heads + head) * d->sectors;
        for (uint32_t s = 0; s < d->sectors; s++) {
            disk_write(n, lba + s, 1, fill);
        }
        finish(c, dl, 0x00);
        return;
    }
    case 0x08:                      /* the drive's parameters */
        if (hard) {
            if (!d) {
                finish(c, dl, 0x01);
                return;
            }
            uint32_t cyl = d->cylinders - 1u;
            R_CH(c) = (uint8_t)cyl;
            R_CL(c) = (uint8_t)(d->sectors | ((cyl >> 2) & 0xC0));
            R_DH(c) = (uint8_t)(d->heads - 1);
            R_DL(c) = (uint8_t)hard_disks();
        } else {
            if (dl >= PC_FLOPPIES) {
                finish(c, dl, 0x01);
                return;
            }
            uint16_t cyl = 79;
            uint8_t spt = 18, type = 4;
            if (d->present) {
                cyl = (uint16_t)(d->cylinders - 1);
                spt = d->sectors;
                type = d->cylinders <= 40 ? 1 : spt == 9 ? 3 : spt == 15 ? 2 : spt >= 36 ? 5 : 4;
            }
            R_BL(c) = type;
            R_CH(c) = (uint8_t)cyl;
            R_CL(c) = spt;
            R_DH(c) = 1;
            R_DL(c) = PC_FLOPPIES;
            x86_load_seg(c, X86_ES, 0xF000);
            c->r.w[X86_DI] = 0xEFC7;
            R_AL(c) = 0;
        }
        finish(c, dl, 0x00);
        return;
    case 0x10:                      /* ready? */
        finish(c, dl, d && d->present ? 0x00 : 0xAA);
        return;
    case 0x15:                      /* the disk's type */
        if (hard) {
            if (d) {
                R_AH(c) = 0x03;
                c->r.w[X86_CX] = (uint16_t)(d->total >> 16);
                c->r.w[X86_DX] = (uint16_t)d->total;
            } else {
                R_AH(c) = 0x00;
            }
        } else {
            R_AH(c) = dl < PC_FLOPPIES ? 0x02 : 0x00;  /* a drive with a change line */
        }
        bios_return_cf(c, false);
        return;
    case 0x16:                      /* changed since? */
        if (!hard && d && d->changed) {
            d->changed = false;
            finish(c, dl, 0x06);
        } else {
            finish(c, dl, 0x00);
        }
        return;
    case 0x17:                      /* the type for formatting */
    case 0x18:
        if (ah == 0x18) {
            x86_load_seg(c, X86_ES, 0xF000);
            c->r.w[X86_DI] = 0xEFC7;
        }
        finish(c, dl, 0x00);
        return;
    default:
        finish(c, dl, 0x01);        /* not supported (the extensions) */
        return;
    }
}
