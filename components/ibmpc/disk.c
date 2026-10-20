/*
 * Disk images: two floppy drives (A:, B:) and two hard disks (C:, D:)
 *
 * Images are read and written in place on the SD card (PSRAM is better
 * spent on the PC's memory). Floppies (.img, .ima, .vfd: raw sectors): the
 * geometry from the size (160 KB to 2.88 MB) or the boot sector's BPB.
 * Hard disks (.img, .hdd raw, .vhd fixed): from the VHD's footer, the
 * partition table's (the first partition's last head and sector), or 16
 * heads of 63 sectors. A folder is a hard disk too (folder.c).
 *
 * Blank images are formatted: floppies as DOS's FORMAT makes them (FAT12),
 * hard disks with one active partition (FAT12 up to 16 MB, else FAT16) and
 * a master boot record that starts it, so SYS C: (or FORMAT C: /S) makes
 * them bootable; the boot sectors' own code says the disk isn't a system
 * disk. The rest of the file is zeros.
 */
#include <stdio.h>
#include <sys/stat.h>
#include "pc_internal.h"

disk_t disks[PC_FLOPPIES + PC_HARD_DISKS];

/* ---- Geometry ---- */

typedef struct {
    uint32_t bytes;
    uint16_t cylinders;
    uint8_t heads, sectors;
} floppy_geometry_t;

static const floppy_geometry_t s_floppies[] = {
    { 163840, 40, 1, 8 },   { 184320, 40, 1, 9 },   { 327680, 40, 2, 8 },   { 368640, 40, 2, 9 },
    { 737280, 80, 2, 9 },   { 1228800, 80, 2, 15 }, { 1474560, 80, 2, 18 }, { 1720320, 80, 2, 21 },
    { 1763328, 82, 2, 21 }, { 2949120, 80, 2, 36 },
};

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

static bool floppy_geometry(disk_t *d, uint32_t bytes, const uint8_t *b)
{
    for (size_t i = 0; i < sizeof(s_floppies) / sizeof(s_floppies[0]); i++) {
        if (s_floppies[i].bytes == bytes) {
            d->cylinders = s_floppies[i].cylinders;
            d->heads = s_floppies[i].heads;
            d->sectors = s_floppies[i].sectors;
            return true;
        }
    }
    /* Another size: the boot sector's BPB */
    uint16_t spt = le16(b + 24), heads = le16(b + 26), total = le16(b + 19);
    if (le16(b + 11) == 512 && spt >= 8 && spt <= 63 && heads >= 1 && heads <= 2 && total) {
        d->sectors = (uint8_t)spt;
        d->heads = (uint8_t)heads;
        d->cylinders = (uint16_t)((bytes / 512 + spt * heads - 1) / (spt * heads));
        return true;
    }
    return false;
}

/* ---- Floppies ---- */

static void close_disk(disk_t *d)
{
    if (d->file) {
        fclose((FILE *)d->file);
    }
    folder_close(d->folder);
    free(d->data);
    bool changed = d->present;
    memset(d, 0, sizeof(*d));
    d->changed = changed;
}

bool pc_floppy_insert(int drive, const char *path)
{
    if (drive < 0 || drive >= PC_FLOPPIES) {
        return false;
    }
    disk_t *d = &disks[drive];
    close_disk(d);
    fdc_disk_changed(drive);
    bool readonly = false;
    FILE *f = fopen(path, "r+b");
    if (!f) {
        f = fopen(path, "rb");
        readonly = true;
    }
    if (!f) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long bytes = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (bytes < 160 * 1024 || bytes > 2880 * 1024) {
        fclose(f);
        return false;
    }
    uint8_t boot[512];
    if (fread(boot, 1, sizeof(boot), f) != sizeof(boot) || !floppy_geometry(d, (uint32_t)bytes, boot)) {
        fclose(f);
        return false;
    }
    d->file = f;
    d->readonly = readonly;
    d->total = (uint32_t)bytes / 512;
    d->present = true;
    d->changed = true;
    snprintf(d->path, sizeof(d->path), "%s", path);
    PC_LOG("%c: %s, %u KB (%u/%u/%u)%s", 'A' + drive, path, (unsigned)(bytes / 1024), d->cylinders, d->heads,
           d->sectors, readonly ? ", read only" : "");
    fdc_disk_changed(drive);
    return true;
}

void pc_floppy_eject(int drive)
{
    if (drive >= 0 && drive < PC_FLOPPIES) {
        close_disk(&disks[drive]);
        fdc_disk_changed(drive);
    }
}

const char *pc_floppy_path(int drive)
{
    return drive >= 0 && drive < PC_FLOPPIES && disks[drive].present ? disks[drive].path : NULL;
}

bool pc_floppy_write_protected(int drive)
{
    return drive >= 0 && drive < PC_FLOPPIES && disks[drive].readonly;
}

/* ---- Hard disks ---- */

bool pc_hd_insert(int drive, const char *path)
{
    if (drive < 0 || drive >= PC_HARD_DISKS) {
        return false;
    }
    disk_t *d = &disks[PC_FLOPPIES + drive];
    close_disk(d);
    struct stat dir;
    if (stat(path, &dir) == 0 && S_ISDIR(dir.st_mode)) {
        /* a folder: a disk made from its files */
        d->folder = folder_open(path, d);
        if (d->folder == NULL) {
            return false;
        }
        d->readonly = true;
        d->present = true;
        d->changed = true;
        snprintf(d->path, sizeof(d->path), "%s", path);
        PC_LOG("%c: %s, %u MB (%u/%u/%u), read only", 'C' + drive, path, (unsigned)(d->total >> 11),
               d->cylinders, d->heads, d->sectors);
        return true;
    }
    bool readonly = false;
    FILE *f = fopen(path, "r+b");
    if (!f) {
        f = fopen(path, "rb");
        readonly = true;
    }
    if (!f) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long bytes = ftell(f);
    if (bytes < 1024 * 1024) {
        fclose(f);
        return false;
    }
    uint8_t s[512];
    d->heads = 16;
    d->sectors = 63;
    /* A fixed VHD: its footer gives the geometry */
    fseek(f, bytes - 512, SEEK_SET);
    if (fread(s, 1, 512, f) == 512 && !memcmp(s, "conectix", 8)) {
        bytes -= 512;
        d->heads = s[0x3A];
        d->sectors = s[0x3B];
    } else {
        /* The partition table: the first partition's last head and sector */
        fseek(f, 0, SEEK_SET);
        if (fread(s, 1, 512, f) == 512 && s[510] == 0x55 && s[511] == 0xAA) {
            for (int i = 0; i < 4; i++) {
                const uint8_t *e = s + 0x1BE + i * 16;
                if (e[4] && (e[6] & 63) && e[5]) {
                    d->heads = (uint8_t)(e[5] + 1);
                    d->sectors = e[6] & 63;
                    break;
                }
            }
        }
    }
    if (!d->heads || !d->sectors) {
        d->heads = 16;
        d->sectors = 63;
    }
    d->total = (uint32_t)(bytes / 512);
    uint32_t cyl = d->total / ((uint32_t)d->heads * d->sectors);
    d->cylinders = (uint16_t)(cyl > 1024 ? 1024 : cyl);
    d->file = f;
    d->readonly = readonly;
    d->present = true;
    d->changed = true;
    snprintf(d->path, sizeof(d->path), "%s", path);
    PC_LOG("%c: %s, %u MB (%u/%u/%u)%s", 'C' + drive, path, (unsigned)(bytes >> 20), d->cylinders, d->heads,
           d->sectors, readonly ? ", read only" : "");
    return true;
}

void pc_hd_eject(int drive)
{
    if (drive >= 0 && drive < PC_HARD_DISKS) {
        close_disk(&disks[PC_FLOPPIES + drive]);
    }
}

const char *pc_hd_path(int drive)
{
    disk_t *d = &disks[PC_FLOPPIES + drive];
    return drive >= 0 && drive < PC_HARD_DISKS && d->present ? d->path : NULL;
}

bool pc_hd_write_protected(int drive)
{
    return drive >= 0 && drive < PC_HARD_DISKS && disks[PC_FLOPPIES + drive].readonly;
}

uint32_t pc_hd_size(int drive)
{
    disk_t *d = &disks[PC_FLOPPIES + drive];
    return drive >= 0 && drive < PC_HARD_DISKS && d->present ? d->total * 512u : 0;
}

/* ---- Sectors ---- */

bool disk_read(int n, uint32_t lba, uint32_t count, uint8_t *dst)
{
    disk_t *d = &disks[n];
    if (!d->present || lba + count > d->total) {
        return false;
    }
    if (d->folder) {
        folder_read(d->folder, lba, count, dst);
    } else if (d->data) {
        memcpy(dst, d->data + lba * 512, count * 512);
    } else {
        FILE *f = d->file;
        if (fseek(f, (long)lba * 512, SEEK_SET) != 0 || fread(dst, 512, count, f) != count) {
            return false;
        }
    }
    d->reads += count;
    d->last_used = pc.frames ? pc.frames : 1;
    return true;
}

bool disk_write(int n, uint32_t lba, uint32_t count, const uint8_t *src)
{
    disk_t *d = &disks[n];
    if (!d->present || d->readonly || lba + count > d->total) {
        return false;
    }
    if (d->data) {
        memcpy(d->data + lba * 512, src, count * 512);
    }
    FILE *f = d->file;
    bool ok = fseek(f, (long)lba * 512, SEEK_SET) == 0 && fwrite(src, 512, count, f) == count;
    fflush(f);
    d->writes += count;
    d->last_used = pc.frames ? pc.frames : 1;
    return ok;
}

/* ---- Blank images ---- */

/* The boot sector's code: "not a system disk", a key, INT 19h again */
static void boot_code(uint8_t *b)
{
    static const uint8_t code[] = {
        0xFA, 0x31, 0xC0, 0x8E, 0xD0, 0xBC, 0x00, 0x7C, 0xFB, 0x8E, 0xD8, 0xBE, 0x5D, 0x7C,
        0xAC, 0x08, 0xC0, 0x74, 0x06, 0xB4, 0x0E, 0xCD, 0x10, 0xEB, 0xF5,
        0x30, 0xE4, 0xCD, 0x16, 0xCD, 0x19,
    };
    static const char msg[] = "Not a system disk: press a key to try again.\r\n";
    b[0] = 0xEB;
    b[1] = 0x3C;
    b[2] = 0x90;
    memcpy(b + 3, "MSDOS5.0", 8);
    memcpy(b + 0x3E, code, sizeof(code));
    memcpy(b + 0x5D, msg, sizeof(msg));
    b[510] = 0x55;
    b[511] = 0xAA;
}

typedef struct {
    uint32_t sectors;               /* in the file system */
    uint32_t hidden;
    uint8_t per_cluster;
    uint16_t root_entries;
    uint8_t media;
    uint8_t spt, heads;
    bool fat16;
    uint8_t drive;
} fs_t;

/* FAT size (sectors) for the clusters left */
static uint16_t fat_sectors(const fs_t *fs)
{
    uint32_t root = (uint32_t)fs->root_entries * 32 / 512;
    uint32_t fat = 1;
    for (int i = 0; i < 8; i++) {
        uint32_t data = fs->sectors - 1 - 2 * fat - root;
        uint32_t clusters = data / fs->per_cluster + 2;
        uint32_t need = fs->fat16 ? (clusters * 2 + 511) / 512 : (clusters * 3 / 2 + 511) / 512;
        if (need <= fat) {
            break;
        }
        fat = need;
    }
    return (uint16_t)fat;
}

static void bpb(uint8_t *b, const fs_t *fs, uint16_t fat, const char *label)
{
    boot_code(b);
    put16(b + 11, 512);
    b[13] = fs->per_cluster;
    put16(b + 14, 1);               /* reserved: the boot sector */
    b[16] = 2;
    put16(b + 17, fs->root_entries);
    put16(b + 19, fs->sectors < 65536 ? fs->sectors : 0);
    b[21] = fs->media;
    put16(b + 22, fat);
    put16(b + 24, fs->spt);
    put16(b + 26, fs->heads);
    put32(b + 28, fs->hidden);
    put32(b + 32, fs->sectors < 65536 ? 0 : fs->sectors);
    b[36] = fs->drive;
    b[38] = 0x29;
    put32(b + 39, 0x12345678u ^ fs->sectors);
    memcpy(b + 43, label ? label : "NO NAME    ", 11);
    memcpy(b + 54, fs->fat16 ? "FAT16   " : "FAT12   ", 8);
}

/* Write `bytes` of zeros with the given sectors at their places */
typedef struct {
    uint32_t lba;
    const uint8_t *data;
} put_t;

static bool write_image(const char *path, uint32_t bytes, const put_t *puts, int n)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    const uint32_t block = 16384;
    uint8_t *buf = pc_calloc(1, block);
    bool ok = buf != NULL;
    for (uint32_t at = 0; ok && at < bytes; at += block) {
        uint32_t len = bytes - at < block ? bytes - at : block;
        memset(buf, 0, block);
        for (int i = 0; i < n; i++) {
            uint32_t off = puts[i].lba * 512;
            if (off >= at && off < at + len) {
                memcpy(buf + (off - at), puts[i].data, 512);
            }
        }
        ok = fwrite(buf, 1, len, f) == len;
    }
    free(buf);
    return fclose(f) == 0 && ok;
}

bool pc_floppy_create(const char *path, uint32_t bytes)
{
    fs_t fs = { .media = 0xF0, .heads = 2, .per_cluster = 1, .root_entries = 224 };
    switch (bytes) {
    case 368640: fs.spt = 9; fs.media = 0xFD; fs.per_cluster = 2; fs.root_entries = 112; break;
    case 737280: fs.spt = 9; fs.media = 0xF9; fs.per_cluster = 2; fs.root_entries = 112; break;
    case 1228800: fs.spt = 15; fs.media = 0xF9; break;
    case 1474560: fs.spt = 18; break;
    default: return false;
    }
    fs.sectors = bytes / 512;
    uint16_t fat = fat_sectors(&fs);
    static uint8_t boot[512], fat0[512];
    memset(boot, 0, sizeof(boot));
    bpb(boot, &fs, fat, NULL);
    memset(fat0, 0, sizeof(fat0));
    fat0[0] = fs.media;
    fat0[1] = fat0[2] = 0xFF;
    put_t puts[] = { { 0, boot }, { 1, fat0 }, { 1u + fat, fat0 } };
    return write_image(path, bytes, puts, 3);
}

/* The master boot record: one active partition from cylinder 0, head 1
 * (sector 63) on, 16 heads of 63 sectors; its code starts the partition's
 * boot sector
 */
void disk_make_mbr(uint8_t *mbr, uint32_t fs_sectors, uint8_t type)
{
    static const uint8_t mbr_code[] = {
        0xFA, 0x31, 0xC0, 0x8E, 0xD0, 0xBC, 0x00, 0x7C, 0x8E, 0xD8, 0x8E, 0xC0, 0xFB,
        0xBE, 0x00, 0x7C, 0xBF, 0x00, 0x06, 0xB9, 0x00, 0x01, 0xFC, 0xF3, 0xA5,
        0xEA, 0x1E, 0x06, 0x00, 0x00,
        0xBE, 0xBE, 0x07, 0xB9, 0x04, 0x00,
        0x80, 0x3C, 0x80, 0x74, 0x0B, 0x83, 0xC6, 0x10, 0xE2, 0xF6,
        0xCD, 0x18, 0xEB, 0xFE, 0x90, 0x90,
        0x8A, 0x74, 0x01, 0x8B, 0x4C, 0x02, 0xB2, 0x80, 0xBB, 0x00, 0x7C, 0xB8, 0x01, 0x02, 0xCD, 0x13,
        0x72, 0xE8, 0x81, 0x3E, 0xFE, 0x7D, 0x55, 0xAA, 0x75, 0xE0,
        0xEA, 0x00, 0x7C, 0x00, 0x00,
    };
    memset(mbr, 0, 512);
    memcpy(mbr, mbr_code, sizeof(mbr_code));
    uint8_t *e = mbr + 0x1BE;
    uint32_t end = 63 + fs_sectors - 1;  /* its last sector: cylinder, head, sector */
    uint32_t c = end / (16 * 63), h = end / 63 % 16, s = end % 63 + 1;
    if (c > 1023) {
        c = 1023;
        h = 15;
        s = 63;
    }
    e[0] = 0x80;                        /* active */
    e[1] = 1;                           /* from cylinder 0, head 1, sector 1 */
    e[2] = 1;
    e[3] = 0;
    e[4] = type;
    e[5] = (uint8_t)h;
    e[6] = (uint8_t)(s | ((c >> 8) << 6));
    e[7] = (uint8_t)c;
    put32(e + 8, 63);
    put32(e + 12, fs_sectors);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
}

/* A hard disk partition's boot sector (FAT16, 512 root entries) */
void disk_make_boot(uint8_t *b, uint32_t fs_sectors, uint8_t per_cluster, uint16_t fat, const char *label)
{
    fs_t fs = { .sectors = fs_sectors, .hidden = 63, .per_cluster = per_cluster, .root_entries = 512,
                .media = 0xF8, .spt = 63, .heads = 16, .fat16 = true, .drive = 0x80 };
    memset(b, 0, 512);
    bpb(b, &fs, fat, label);
}

bool pc_hd_create(const char *path, uint32_t bytes)
{
    const uint32_t cylinder = 16 * 63;
    uint32_t total = bytes / 512;
    uint32_t cyls = total / cylinder;
    if (cyls < 2 || cyls > 1024 || total % cylinder) {
        return false;
    }
    fs_t fs = { .hidden = 63, .root_entries = 512, .media = 0xF8, .spt = 63, .heads = 16, .drive = 0x80 };
    fs.sectors = total - 63;
    fs.fat16 = fs.sectors > 32768;      /* above 16 MB */
    fs.per_cluster = !fs.fat16 ? 8 : fs.sectors <= 131072 ? 4 : 8;
    uint16_t fat = fat_sectors(&fs);

    static uint8_t mbr[512], boot[512], fat0[512];
    disk_make_mbr(mbr, fs.sectors, !fs.fat16 ? 0x01 : fs.sectors < 65536 ? 0x04 : 0x06);
    memset(boot, 0, sizeof(boot));
    bpb(boot, &fs, fat, NULL);
    memset(fat0, 0, sizeof(fat0));
    fat0[0] = 0xF8;
    fat0[1] = fat0[2] = 0xFF;
    if (fs.fat16) {
        fat0[3] = 0xFF;
    }
    put_t puts[] = { { 0, mbr }, { 63, boot }, { 64, fat0 }, { 64u + fat, fat0 } };
    return write_image(path, bytes, puts, 4);
}
