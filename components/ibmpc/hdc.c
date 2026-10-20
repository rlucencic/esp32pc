/*
 * The AT's hard disk controller (the WD1003's task file, as IDE drives
 * answer it): 1F0h-1F7h and 3F6h, IRQ 14, the two hard disks' images
 *
 * For systems that drive the disks themselves (Minix, Xenix, OS/2, ...);
 * the BIOS's INT 13h reads the images directly. Commands: recalibrate,
 * read and write sectors (and READ/WRITE MULTIPLE), verify, format track
 * (the data stays), seek, diagnostics, initialize drive parameters (the
 * geometry the CHS addresses use from then on), identify, set multiple,
 * set features and the power ones (accepted). CHS or LBA addresses. Each
 * sector (or seek) takes a short time, then the interrupt.
 */
#include "pc_internal.h"

#define SECTOR      512
#define BLOCK_MAX   16              /* READ/WRITE MULTIPLE's largest block */

enum {
    ST_ERR = 0x01, ST_DRQ = 0x08, ST_DSC = 0x10, ST_DRDY = 0x40, ST_BSY = 0x80,
    ER_ABRT = 0x04, ER_IDNF = 0x10, ER_UNC = 0x40,
};

static struct {
    uint8_t error, count, sector, cyl_lo, cyl_hi, sdh, status, control;
    uint8_t command;
    bool irq;
    uint8_t *buf;                   /* SECTOR * BLOCK_MAX (PSRAM: the S3's internal RAM is short) */
    int pos, len;                   /* the data transfer through 1F0h: bytes */
    uint64_t when;                  /* the command's next step (NEVER: none) */
    int left;                       /* sectors left in the command */
    int multiple;                   /* SET MULTIPLE's block (0: one sector a DRQ) */
    struct { uint16_t heads, sectors; } geo[PC_HARD_DISKS];    /* (0: the image's) */
} h;

static int drive(void)
{
    return (h.sdh >> 4) & 1;
}

static disk_t *disk(void)
{
    disk_t *d = &disks[PC_FLOPPIES + drive()];
    return d->present ? d : NULL;
}

static bool any_disk(void)
{
    return disks[PC_FLOPPIES].present || disks[PC_FLOPPIES + 1].present;
}

static void set_irq(bool on)
{
    h.irq = on;
    pic_irq(14, on && !(h.control & 0x02));
}

static void later(uint32_t us)
{
    h.when = pc_now() + (uint64_t)us * (pc.cfg.cpu_hz / 1000) / 1000;
    pc_schedule(h.when);
}

static int sectors_per_track(const disk_t *d)
{
    return h.geo[drive()].sectors ? h.geo[drive()].sectors : d->sectors;
}

static int heads(const disk_t *d)
{
    return h.geo[drive()].heads ? h.geo[drive()].heads : d->heads;
}

/* The task file's address as an LBA, -1 if it isn't on the disk */
static int64_t address(const disk_t *d)
{
    if (h.sdh & 0x40) {
        uint32_t lba = (uint32_t)(h.sdh & 0x0F) << 24 | (uint32_t)h.cyl_hi << 16 | (uint32_t)h.cyl_lo << 8 | h.sector;
        return lba < d->total ? (int64_t)lba : -1;
    }
    int spt = sectors_per_track(d), hd = heads(d);
    uint32_t cyl = (uint32_t)h.cyl_hi << 8 | h.cyl_lo, head = h.sdh & 0x0F;
    if (h.sector == 0 || h.sector > spt || head >= (uint32_t)hd) {
        return -1;
    }
    uint32_t lba = (cyl * (uint32_t)hd + head) * (uint32_t)spt + h.sector - 1;
    return lba < d->total ? (int64_t)lba : -1;
}

/* The task file on to the next sector, as the drive leaves it */
static void advance(const disk_t *d)
{
    if (h.sdh & 0x40) {
        uint32_t lba = ((uint32_t)(h.sdh & 0x0F) << 24 | (uint32_t)h.cyl_hi << 16 | (uint32_t)h.cyl_lo << 8 | h.sector) + 1;
        h.sector = (uint8_t)lba;
        h.cyl_lo = (uint8_t)(lba >> 8);
        h.cyl_hi = (uint8_t)(lba >> 16);
        h.sdh = (uint8_t)((h.sdh & 0xF0) | ((lba >> 24) & 0x0F));
    } else if (++h.sector > sectors_per_track(d)) {
        h.sector = 1;
        uint8_t head = (uint8_t)((h.sdh & 0x0F) + 1);
        if (head >= heads(d)) {
            head = 0;
            uint16_t cyl = (uint16_t)((h.cyl_hi << 8 | h.cyl_lo) + 1);
            h.cyl_lo = (uint8_t)cyl;
            h.cyl_hi = (uint8_t)(cyl >> 8);
        }
        h.sdh = (uint8_t)((h.sdh & 0xF0) | head);
    }
    h.count--;
}

static void done(uint8_t error)
{
    h.error = error;
    h.status = (uint8_t)(ST_DRDY | ST_DSC | (error ? ST_ERR : 0));
    h.left = 0;
    h.len = h.pos = 0;
    set_irq(true);
}

static int block(void)
{
    bool multi = h.command == 0xC4 || h.command == 0xC5;
    int n = multi && h.multiple ? h.multiple : 1;
    return n < h.left ? n : h.left;
}

/* The next sectors read into the buffer: DRQ and the interrupt */
static void read_block(void)
{
    disk_t *d = disk();
    int n = block();
    for (int i = 0; i < n; i++) {
        int64_t lba = d ? address(d) : -1;
        if (lba < 0) {
            done(ER_IDNF);
            return;
        }
        if (!disk_read(PC_FLOPPIES + drive(), (uint32_t)lba, 1, h.buf + i * SECTOR)) {
            done(ER_UNC);
            return;
        }
        advance(d);
    }
    h.left -= n;
    h.pos = 0;
    h.len = n * SECTOR;
    h.status = ST_DRDY | ST_DSC | ST_DRQ;
    set_irq(true);
}

/* The buffer's sectors written: the next block's DRQ, or the end */
static void write_block(void)
{
    disk_t *d = disk();
    int n = h.len / SECTOR;
    for (int i = 0; i < n; i++) {
        int64_t lba = d ? address(d) : -1;
        if (lba < 0) {
            done(ER_IDNF);
            return;
        }
        if (!disk_write(PC_FLOPPIES + drive(), (uint32_t)lba, 1, h.buf + i * SECTOR)) {
            done(ER_ABRT);
            return;
        }
        advance(d);
    }
    h.left -= n;
    if (h.left <= 0) {
        done(0);
        return;
    }
    h.pos = 0;
    h.len = block() * SECTOR;
    h.status = ST_DRDY | ST_DSC | ST_DRQ;
    set_irq(true);
}

static void put_string(uint8_t *w, int words, const char *s)
{
    for (int i = 0; i < words * 2; i++) {
        char ch = *s ? *s++ : ' ';
        w[i ^ 1] = (uint8_t)ch;     /* (ATA's strings: two characters a word, the first high) */
    }
}

static void identify(const disk_t *d)
{
    uint8_t *b = h.buf;
    memset(b, 0, SECTOR);
#define W(i, v) do { b[(i) * 2] = (uint8_t)(v); b[(i) * 2 + 1] = (uint8_t)((v) >> 8); } while (0)
    W(0, 0x0040);                   /* fixed */
    W(1, d->cylinders);
    W(3, d->heads);
    W(4, d->sectors * SECTOR);
    W(5, SECTOR);
    W(6, d->sectors);
    put_string(b + 20, 10, drive() ? "ESP32PC-HD1" : "ESP32PC-HD0");
    W(20, 3);
    W(21, 16);
    put_string(b + 46, 4, "1.0");
    put_string(b + 54, 20, "esp32pc hard disk");
    W(47, 0x8000 | BLOCK_MAX);
    W(49, 0x0200);                  /* LBA */
    W(51, 0x0200);
    W(53, 0x0001);
    W(54, d->cylinders);
    W(55, heads(d));
    W(56, sectors_per_track(d));
    uint32_t cap = (uint32_t)d->cylinders * heads(d) * sectors_per_track(d);
    W(57, cap);
    W(58, cap >> 16);
    W(59, h.multiple ? 0x0100 | h.multiple : 0);
    W(60, d->total);
    W(61, d->total >> 16);
#undef W
    h.pos = 0;
    h.len = SECTOR;
    h.status = ST_DRDY | ST_DSC | ST_DRQ;
    set_irq(true);
}

static void command(uint8_t cmd)
{
    disk_t *d = disk();
    if (h.buf == NULL) {
        return;
    }
    if (pc_log_int13) {
        PC_LOG("HDC %02X: drive/head %02X, count %u, sector %u, cylinder %u", cmd, h.sdh, h.count, h.sector,
               h.cyl_hi << 8 | h.cyl_lo);
    }
    h.command = cmd;
    h.error = 0;
    set_irq(false);
    if (d == NULL && cmd != 0x90) {
        return;                     /* (no drive answers) */
    }
    h.left = h.count ? h.count : 256;
    h.len = h.pos = 0;
    switch (cmd) {
    case 0x30: case 0x31: case 0xC5:            /* WRITE: the first block's data now */
        if ((cmd == 0xC5 && !h.multiple) || d->readonly) {
            done(ER_ABRT);
            return;
        }
        h.len = block() * SECTOR;
        h.status = ST_DRDY | ST_DSC | ST_DRQ;
        return;
    case 0x50:                                  /* FORMAT TRACK: the interleave table */
        h.len = SECTOR;
        h.status = ST_DRDY | ST_DSC | ST_DRQ;
        return;
    case 0xC4:
        if (!h.multiple) {
            done(ER_ABRT);
            return;
        }
        break;
    default:
        break;
    }
    h.status = ST_BSY;
    later(cmd >= 0x70 && cmd <= 0x7F ? 500 : 100);
}

/* A command's step after its time */
static void step(void)
{
    disk_t *d = disk();
    uint8_t cmd = h.command;
    if ((cmd >= 0x20 && cmd <= 0x23) || cmd == 0xC4) {
        read_block();
    } else if ((cmd >= 0x30 && cmd <= 0x33) || cmd == 0xC5) {
        write_block();
    } else if (cmd == 0x40 || cmd == 0x41) {    /* VERIFY */
        while (h.left > 0) {
            if (address(d) < 0) {
                done(ER_IDNF);
                return;
            }
            advance(d);
            h.left--;
        }
        done(0);
    } else if (cmd >= 0x10 && cmd <= 0x1F) {    /* RECALIBRATE */
        h.cyl_lo = h.cyl_hi = 0;
        done(0);
    } else if (cmd >= 0x70 && cmd <= 0x7F) {    /* SEEK */
        done(0);
    } else if (cmd == 0x50) {
        done(0);
    } else if (cmd == 0x90) {                   /* DIAGNOSTICS: no error */
        done(0);
        h.error = 0x01;
    } else if (cmd == 0x91) {                   /* INITIALIZE DRIVE PARAMETERS */
        h.geo[drive()].heads = (uint16_t)((h.sdh & 0x0F) + 1);
        h.geo[drive()].sectors = h.count;
        done(0);
    } else if (cmd == 0xEC) {
        identify(d);
    } else if (cmd == 0xC6) {                   /* SET MULTIPLE */
        if (h.count > BLOCK_MAX || (h.count & (h.count - 1))) {
            done(ER_ABRT);
        } else {
            h.multiple = h.count;
            done(0);
        }
    } else if (cmd == 0xEF || (cmd >= 0xE0 && cmd <= 0xE5) || cmd == 0x94 || cmd == 0x95 ||
               cmd == 0x96 || cmd == 0x97 || cmd == 0x98 || cmd == 0x99) {
        done(0);                                /* set features, power: as asked */
    } else {
        done(ER_ABRT);
    }
}

/* The data port's bytes (the buffer's): the end of a block moves on */
static void data_done(void)
{
    if (h.pos < h.len) {
        return;
    }
    uint8_t cmd = h.command;
    if ((cmd >= 0x30 && cmd <= 0x33) || cmd == 0xC5 || cmd == 0x50) {
        h.status = ST_BSY;          /* written: the drive's turn */
        later(100);
    } else if (h.left > 0) {
        h.status = ST_BSY;          /* read: the next block */
        h.len = 0;
        later(100);
    } else {
        h.status = ST_DRDY | ST_DSC;
        h.len = h.pos = 0;
    }
}

uint16_t hdc_read_data(void)
{
    if (!(h.status & ST_DRQ) || h.pos >= h.len) {
        return 0xFFFF;
    }
    uint16_t v = (uint16_t)(h.buf[h.pos] | h.buf[h.pos + 1] << 8);
    h.pos += 2;
    data_done();
    return v;
}

void hdc_write_data(uint16_t v)
{
    if (!(h.status & ST_DRQ) || h.pos >= h.len) {
        return;
    }
    h.buf[h.pos] = (uint8_t)v;
    h.buf[h.pos + 1] = (uint8_t)(v >> 8);
    h.pos += 2;
    data_done();
}

bool hdc_in(uint16_t port, uint8_t *v)
{
    if (pc.cfg.board != PC_AT || !((port >= 0x1F0 && port <= 0x1F7) || port == 0x3F6)) {
        return false;
    }
    if (!any_disk()) {
        *v = 0xFF;                  /* no controller */
        return true;
    }
    bool present = disk() != NULL;
    switch (port) {
    case 0x1F0: {
        uint16_t w = hdc_read_data();   /* (a byte read takes a word's low byte) */
        *v = (uint8_t)w;
        break;
    }
    case 0x1F1: *v = h.error; break;
    case 0x1F2: *v = h.count; break;
    case 0x1F3: *v = h.sector; break;
    case 0x1F4: *v = h.cyl_lo; break;
    case 0x1F5: *v = h.cyl_hi; break;
    case 0x1F6: *v = (uint8_t)(h.sdh | 0xA0); break;
    case 0x1F7:
        *v = present ? h.status : 0x00;
        set_irq(false);
        break;
    default:                        /* 3F6: the alternate status */
        *v = present ? h.status : 0x00;
        break;
    }
    return true;
}

bool hdc_out(uint16_t port, uint8_t v)
{
    if (pc.cfg.board != PC_AT || !((port >= 0x1F0 && port <= 0x1F7) || port == 0x3F6)) {
        return false;
    }
    switch (port) {
    case 0x1F0: hdc_write_data((uint16_t)(v | v << 8)); break;
    case 0x1F1: break;              /* write precompensation, features */
    case 0x1F2: h.count = v; break;
    case 0x1F3: h.sector = v; break;
    case 0x1F4: h.cyl_lo = v; break;
    case 0x1F5: h.cyl_hi = v; break;
    case 0x1F6: h.sdh = v; break;
    case 0x1F7:
        if (!(h.status & ST_BSY) || v == 0x08) {
            command(v);
        }
        break;
    default:                        /* 3F6: the device control (reset, nIEN) */
        if ((h.control & 0x04) && !(v & 0x04)) {
            hdc_reset();            /* SRST's end */
        }
        h.control = v;
        pic_irq(14, h.irq && !(v & 0x02));
        break;
    }
    return true;
}

uint64_t hdc_next_event(void)
{
    return h.when;
}

void hdc_event(uint64_t now)
{
    if (now >= h.when) {
        h.when = NEVER;
        step();
    }
}

void hdc_reset(void)
{
    uint8_t control = h.control;
    uint8_t *buf = h.buf ? h.buf : pc_calloc(BLOCK_MAX, SECTOR);
    memset(&h, 0, sizeof(h));
    h.buf = buf;
    h.control = control;
    h.when = NEVER;
    h.status = ST_DRDY | ST_DSC;
    h.error = 0x01;                 /* (the diagnostic code: no error) */
    h.count = 1;
    h.sector = 1;
    pic_irq(14, false);
}
