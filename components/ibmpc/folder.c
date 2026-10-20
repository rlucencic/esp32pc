/*
 * A folder on the SD card as a hard disk, read only: a FAT16 file system
 * made up from its files and subfolders when it goes in.
 *
 * The folder is read once (names, sizes, dates, into PSRAM); the sectors are
 * worked out as they're read: the partition table, the boot sector, the FATs
 * and the directories from that list, the files' contents straight from the
 * card. Each folder and file has a run of clusters of its own, in the order
 * they were found, so a cluster's owner is a binary search away.
 *
 * Names become 8.3 the way Windows makes them: upper case, spaces gone,
 * characters DOS doesn't take as '_', and if that changed the name or it was
 * too long, its first six characters and ~1 (~2, ...). Files added on the
 * card show the next time the folder goes in. Writes fail as on a
 * write-protected disk.
 *
 * At least 4,200 clusters (DOS takes fewer for FAT12), 2 KB each and bigger
 * as needed for up to 65,000; partitions up to 32 MB suit DOS 3.3, bigger
 * ones (type 06) DOS 4 and later; at most 1,024 cylinders (504 MB).
 */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include "pc_internal.h"

#define MAX_ENTRIES     8192
#define ROOT_ENTRIES    512
#define CYLINDER        (16 * 63)
#define FIRST_FAT       64              /* the MBR, 62 hidden sectors, the boot sector */

typedef struct {
    char name[11];                  /* 8.3, padded with spaces */
    uint8_t attr;                   /* 10h folder, 20h file; 0: left out (no room) */
    uint16_t time, date;
    uint32_t size;                  /* files */
    uint32_t first, clusters;       /* its run (first 0: none) */
    int32_t parent;                 /* the folder it's in, -1: the root */
    uint32_t card_name;             /* its name on the card: an offset in the pool */
    uint32_t child_first, child_count;     /* folders: their entries */
} entry_t;

struct folder {
    char path[192];                 /* the folder on the card */
    entry_t *e;
    uint32_t count, size;
    char *pool;
    uint32_t pool_used, pool_size;
    uint32_t root_count;            /* the root's entries: e[0 ...] */
    char label[11];
    uint8_t spc;                    /* sectors per cluster */
    uint16_t fat;                   /* sectors per FAT */
    uint32_t fs_sectors, clusters, cylinders;
    uint32_t root_lba, data_lba;
    uint32_t *runs;                 /* entries with clusters, in cluster order */
    uint32_t run_count;
    FILE *file;                     /* the file last read */
    int32_t file_entry;
};

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

static void *grow(void *p, uint32_t used, uint32_t *size, uint32_t item, uint32_t want)
{
    if (want <= *size) {
        return p;
    }
    uint32_t n = *size ? *size * 2 : 64;
    while (n < want) {
        n *= 2;
    }
    void *q = pc_calloc(n, item);
    if (q == NULL) {
        return NULL;
    }
    if (p) {
        memcpy(q, p, (size_t)used * item);
        free(p);
    }
    *size = n;
    return q;
}

/* ---- Names ---- */

static bool dos_char(unsigned char c)
{
    return isalnum(c) || (c && strchr("!#$%&'()-@^_`{}~", c)) || c >= 0x80;
}

/* The 8.3 name for a card name: n = 0 as it is (false if it can't be),
 * else with ~n
 */
static bool short_name(const char *name, int n, char out[11])
{
    const char *dot = strrchr(name, '.');
    if (dot == name) {
        dot = NULL;
    }
    size_t blen = dot ? (size_t)(dot - name) : strlen(name);
    char base[64], ext[4];
    size_t b = 0, x = 0;
    bool lossy = false;
    for (size_t i = 0; i < blen; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c == ' ' || c == '.') {
            lossy = true;
            continue;
        }
        if (b < sizeof(base) - 1) {
            base[b++] = dos_char(c) ? (char)toupper(c) : (lossy = true, '_');
        }
    }
    if (dot) {
        for (const char *p = dot + 1; *p; p++) {
            unsigned char c = (unsigned char)*p;
            if (c == ' ') {
                lossy = true;
                continue;
            }
            if (x == 3) {
                lossy = true;
                break;
            }
            ext[x++] = dos_char(c) ? (char)toupper(c) : (lossy = true, '_');
        }
    }
    if (b > 8 || b == 0) {
        lossy = true;
    }
    if (n == 0 && lossy) {
        return false;
    }
    memset(out, ' ', 11);
    if (n > 0) {
        char tail[8];
        int t = snprintf(tail, sizeof(tail), "~%d", n);
        size_t keep = b < (size_t)(8 - t) ? b : (size_t)(8 - t);
        memcpy(out, base, keep);
        memcpy(out + keep, tail, (size_t)t);
    } else {
        memcpy(out, base, b);
    }
    memcpy(out + 8, ext, x);
    if ((uint8_t)out[0] == 0xE5) {
        out[0] = 0x05;              /* (E5h marks a deleted entry) */
    }
    return true;
}

static bool name_taken(const struct folder *fd, uint32_t from, uint32_t to, const char name[11])
{
    for (uint32_t i = from; i < to; i++) {
        if (!memcmp(fd->e[i].name, name, 11)) {
            return true;
        }
    }
    return false;
}

static void dos_time(time_t t, uint16_t *time_out, uint16_t *date_out)
{
    struct tm tm;
    localtime_r(&t, &tm);
    if (tm.tm_year < 80) {
        *time_out = 0;
        *date_out = 1 << 5 | 1;     /* 1 January 1980 */
        return;
    }
    if (tm.tm_year > 207) {
        tm.tm_year = 207;
    }
    *time_out = (uint16_t)(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
    *date_out = (uint16_t)((tm.tm_year - 80) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
}

/* ---- Reading the folder ---- */

static int path_of(const struct folder *fd, int32_t i, char *buf, size_t len)
{
    if (i < 0) {
        return snprintf(buf, len, "%s", fd->path);
    }
    int n = path_of(fd, fd->e[i].parent, buf, len);
    if (n < 0 || (size_t)n >= len) {
        return -1;
    }
    int m = snprintf(buf + n, len - (size_t)n, "/%s", fd->pool + fd->e[i].card_name);
    return m < 0 || (size_t)(n + m) >= len ? -1 : n + m;
}

/* The entries of one folder (parent: -1 the root), after the others */
static bool scan(struct folder *fd, int32_t parent, uint32_t limit)
{
    char path[320];
    if (path_of(fd, parent, path, sizeof(path)) < 0) {
        return true;                /* (too deep: left empty) */
    }
    DIR *dir = opendir(path);
    if (dir == NULL) {
        return true;
    }
    uint32_t first = fd->count;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL && fd->count - first < limit) {
        const char *name = de->d_name;
        if (name[0] == '.' || fd->count >= MAX_ENTRIES) {
            continue;
        }
        char full[320];
        struct stat st;
        if (snprintf(full, sizeof(full), "%s/%s", path, name) >= (int)sizeof(full) || stat(full, &st) != 0) {
            continue;
        }
        bool is_dir = S_ISDIR(st.st_mode);
        if (!is_dir && !S_ISREG(st.st_mode)) {
            continue;
        }
        /* room for one more entry and its name */
        uint32_t nlen = (uint32_t)strlen(name) + 1;
        entry_t *ne = grow(fd->e, fd->count, &fd->size, sizeof(entry_t), fd->count + 1);
        fd->e = ne ? ne : fd->e;
        char *np = ne ? grow(fd->pool, fd->pool_used, &fd->pool_size, 1, fd->pool_used + nlen) : NULL;
        fd->pool = np ? np : fd->pool;
        if (np == NULL) {
            closedir(dir);
            return false;
        }
        entry_t *x = &fd->e[fd->count];
        memset(x, 0, sizeof(*x));
        x->attr = is_dir ? 0x10 : 0x20;
        uint64_t bytes = is_dir ? 0 : (uint64_t)st.st_size;
        x->size = bytes > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)bytes;
        x->parent = parent;
        x->card_name = fd->pool_used;
        memcpy(fd->pool + fd->pool_used, name, nlen);
        fd->pool_used += nlen;
        dos_time(st.st_mtime, &x->time, &x->date);
        /* its 8.3 name, unlike the others' in its folder */
        char sn[11];
        bool ok = short_name(name, 0, sn) && !name_taken(fd, first, fd->count, sn);
        for (int n = 1; !ok && n < 100000; n++) {
            ok = short_name(name, n, sn) && !name_taken(fd, first, fd->count, sn);
        }
        memcpy(x->name, sn, 11);
        fd->count++;
    }
    closedir(dir);
    if (parent >= 0) {
        fd->e[parent].child_first = first;
        fd->e[parent].child_count = fd->count - first;
    } else {
        fd->root_count = fd->count - first;
    }
    return true;
}

/* ---- The layout ---- */

static uint32_t entry_clusters(const entry_t *e, uint32_t bytes)
{
    if (e->attr & 0x10) {
        return ((2 + e->child_count) * 32 + bytes - 1) / bytes;
    }
    return (uint32_t)(((uint64_t)e->size + bytes - 1) / bytes);
}

/* The sizes for spc sectors a cluster on that many cylinders */
static void lay_out_cylinders(struct folder *fd, uint8_t spc, uint32_t cyls)
{
    uint32_t fs = cyls * CYLINDER - 63, fat = 1, clusters;
    bool capped;
    for (;;) {
        clusters = (fs - 1 - 2 * fat - ROOT_ENTRIES / 16) / spc;
        capped = clusters > 65000;
        if (capped) {
            clusters = 65000;
        }
        uint32_t need_fat = ((clusters + 2) * 2 + 511) / 512;
        if (need_fat <= fat) {
            break;
        }
        fat = need_fat;
    }
    if (capped) {
        /* at most 65,000 clusters: the partition ends with them (DOS works
         * them out from its size)
         */
        fs = 1 + 2 * fat + ROOT_ENTRIES / 16 + clusters * spc;
    }
    fd->spc = spc;
    fd->fat = (uint16_t)fat;
    fd->fs_sectors = fs;
    fd->clusters = clusters;
    fd->cylinders = cyls;
    fd->root_lba = FIRST_FAT + 2 * fat;
    fd->data_lba = fd->root_lba + ROOT_ENTRIES / 16;
}

/* The sizes for spc sectors a cluster and that many clusters: false if too
 * big (whole cylinders: the rest is clusters too)
 */
static bool fit(struct folder *fd, uint8_t spc, uint32_t need)
{
    uint32_t clusters = need + 64;      /* (a little room shown free) */
    if (clusters < 4200) {
        clusters = 4200;
    }
    if (clusters > 65000) {
        return false;
    }
    uint32_t fat = ((clusters + 2) * 2 + 511) / 512;
    uint32_t fs = 1 + 2 * fat + ROOT_ENTRIES / 16 + clusters * spc;
    uint32_t cyls = (63 + fs + CYLINDER - 1) / CYLINDER;
    if (cyls > 1024) {
        return false;
    }
    lay_out_cylinders(fd, spc, cyls);
    return true;
}

static void lay_out(struct folder *fd)
{
    static const uint8_t sizes[] = { 4, 8, 16, 32, 64 };
    uint8_t spc = 0;
    for (size_t i = 0; i < sizeof(sizes) && !spc; i++) {
        uint64_t need = 0;
        for (uint32_t k = 0; k < fd->count; k++) {
            need += entry_clusters(&fd->e[k], sizes[i] * 512u);
        }
        if (need < 65000 && fit(fd, sizes[i], (uint32_t)need)) {
            spc = sizes[i];
        }
    }
    if (!spc) {
        lay_out_cylinders(fd, 16, 1024);    /* too much: what fits, in 8 KB clusters */
    }
    /* the runs, in the order of the entries; what doesn't fit is left out */
    uint32_t bytes = fd->spc * 512u, next = 2, n = 0;
    for (uint32_t k = 0; k < fd->count; k++) {
        entry_t *e = &fd->e[k];
        uint32_t c = entry_clusters(e, bytes);
        bool parent_out = e->parent >= 0 && fd->e[e->parent].attr == 0;
        if (parent_out || next + c > fd->clusters + 2) {
            e->attr = 0;
            continue;
        }
        e->first = c ? next : 0;
        e->clusters = c;
        next += c;
        if (c) {
            fd->runs[n++] = k;
        }
    }
    fd->run_count = n;
}

/* The entry whose run has cluster c (-1: none) */
static int32_t owner(const struct folder *fd, uint32_t c)
{
    uint32_t lo = 0, hi = fd->run_count;
    while (lo < hi) {
        uint32_t m = (lo + hi) / 2;
        const entry_t *e = &fd->e[fd->runs[m]];
        if (c < e->first) {
            hi = m;
        } else if (c >= e->first + e->clusters) {
            lo = m + 1;
        } else {
            return (int32_t)fd->runs[m];
        }
    }
    return -1;
}

/* ---- Sectors ---- */

static void dir_entry(uint8_t *p, const char name[11], uint8_t attr, uint16_t time, uint16_t date,
                      uint32_t first, uint32_t size)
{
    memcpy(p, name, 11);
    p[11] = attr;
    put16(p + 22, time);
    put16(p + 24, date);
    put16(p + 26, first);
    put32(p + 28, size);
}

static void entry_at(const struct folder *fd, const entry_t *e, uint8_t *p)
{
    if (e->attr == 0) {
        p[0] = 0xE5;                /* left out: a deleted entry */
        return;
    }
    dir_entry(p, e->name, e->attr, e->time, e->date, e->first, e->attr & 0x10 ? 0 : e->size);
}

static void fat_sector(const struct folder *fd, uint32_t k, uint8_t *dst)
{
    uint32_t c = k * 256;
    for (uint32_t i = 0; i < 256; i++, c++) {
        uint32_t v = 0;
        if (c < 2) {
            v = c == 0 ? 0xFFF8 : 0xFFFF;
        } else if (c < fd->clusters + 2) {
            int32_t o = owner(fd, c);
            if (o >= 0) {
                const entry_t *e = &fd->e[o];
                v = c + 1 < e->first + e->clusters ? c + 1 : 0xFFFF;
            }
        }
        put16(dst + i * 2, v);
    }
}

static void folder_sector(const struct folder *fd, int32_t i, uint32_t index, uint8_t *dst)
{
    const entry_t *e = &fd->e[i];
    for (uint32_t k = 0; k < 16; k++, index++) {
        uint8_t *p = dst + k * 32;
        if (index == 0) {
            dir_entry(p, ".          ", 0x10, e->time, e->date, e->first, 0);
        } else if (index == 1) {
            uint32_t up = e->parent >= 0 ? fd->e[e->parent].first : 0;
            dir_entry(p, "..         ", 0x10, e->time, e->date, up, 0);
        } else if (index - 2 < e->child_count) {
            entry_at(fd, &fd->e[e->child_first + index - 2], p);
        }
    }
}

/* Up to `count` sectors of entry i's file from sector s of it: how many */
static uint32_t file_sectors(struct folder *fd, int32_t i, uint32_t s, uint32_t count, uint8_t *dst)
{
    const entry_t *e = &fd->e[i];
    uint32_t left = e->clusters * fd->spc - s;
    uint32_t n = count < left ? count : left;
    memset(dst, 0, (size_t)n * 512);
    uint64_t at = (uint64_t)s * 512;
    if (at >= e->size) {
        return n;
    }
    if (fd->file_entry != i) {
        if (fd->file) {
            fclose(fd->file);
        }
        char path[320];
        fd->file = path_of(fd, i, path, sizeof(path)) >= 0 ? fopen(path, "rb") : NULL;
        fd->file_entry = i;
    }
    uint64_t bytes = (uint64_t)n * 512;
    if (at + bytes > e->size) {
        bytes = e->size - at;
    }
    if (fd->file && fseek(fd->file, (long)at, SEEK_SET) == 0) {
        size_t got = fread(dst, 1, (size_t)bytes, fd->file);
        (void)got;
    }
    return n;
}

bool folder_read(struct folder *fd, uint32_t lba, uint32_t count, uint8_t *dst)
{
    while (count) {
        uint32_t done = 1;
        memset(dst, 0, 512);
        if (lba == 0) {
            disk_make_mbr(dst, fd->fs_sectors, fd->fs_sectors < 65536 ? 0x04 : 0x06);
        } else if (lba == 63) {
            disk_make_boot(dst, fd->fs_sectors, fd->spc, fd->fat, fd->label);
        } else if (lba >= FIRST_FAT && lba < fd->root_lba) {
            fat_sector(fd, (lba - FIRST_FAT) % fd->fat, dst);
        } else if (lba >= fd->root_lba && lba < fd->data_lba) {
            uint32_t index = (lba - fd->root_lba) * 16;
            for (uint32_t k = 0; k < 16; k++, index++) {
                uint8_t *p = dst + k * 32;
                if (index == 0) {
                    dir_entry(p, fd->label, 0x08, 0, 1 << 5 | 1, 0, 0);
                } else if (index - 1 < fd->root_count) {
                    entry_at(fd, &fd->e[index - 1], p);
                }
            }
        } else if (lba >= fd->data_lba) {
            uint32_t rel = lba - fd->data_lba;
            uint32_t c = 2 + rel / fd->spc;
            int32_t i = owner(fd, c);
            if (i >= 0) {
                uint32_t s = (c - fd->e[i].first) * fd->spc + rel % fd->spc;
                if (fd->e[i].attr & 0x10) {
                    folder_sector(fd, i, s * 16, dst);
                } else {
                    done = file_sectors(fd, i, s, count, dst);
                }
            }
        }
        lba += done;
        count -= done;
        dst += done * 512;
    }
    return true;
}

/* ---- In and out ---- */

void folder_close(struct folder *fd)
{
    if (fd) {
        if (fd->file) {
            fclose(fd->file);
        }
        free(fd->e);
        free(fd->pool);
        free(fd->runs);
        free(fd);
    }
}

struct folder *folder_open(const char *path, disk_t *d)
{
    struct folder *fd = pc_calloc(1, sizeof(*fd));
    if (fd == NULL) {
        return NULL;
    }
    snprintf(fd->path, sizeof(fd->path), "%s", path);
    size_t n = strlen(fd->path);
    while (n > 1 && fd->path[n - 1] == '/') {
        fd->path[--n] = 0;
    }
    fd->file_entry = -1;
    /* the root (at most its directory's entries, less the label), then
     * each folder found, in order
     */
    bool ok = scan(fd, -1, ROOT_ENTRIES - 1);
    for (uint32_t i = 0; ok && i < fd->count; i++) {
        if (fd->e[i].attr & 0x10) {
            ok = scan(fd, (int32_t)i, 65534);
        }
    }
    fd->runs = pc_calloc(fd->count ? fd->count : 1, sizeof(uint32_t));
    if (!ok || fd->runs == NULL) {
        folder_close(fd);
        return NULL;
    }
    /* the volume label: the folder's name */
    const char *base = strrchr(fd->path, '/');
    base = base ? base + 1 : fd->path;
    memset(fd->label, ' ', sizeof(fd->label));
    for (size_t i = 0, k = 0; base[i] && k < sizeof(fd->label); i++) {
        unsigned char c = (unsigned char)base[i];
        if (c != '.') {
            fd->label[k++] = dos_char(c) || c == ' ' ? (char)toupper(c) : '_';
        }
    }
    lay_out(fd);

    uint32_t files = 0, folders = 0, out = 0;
    for (uint32_t i = 0; i < fd->count; i++) {
        const entry_t *e = &fd->e[i];
        out += e->attr == 0;
        files += (e->attr & 0x20) != 0;
        folders += (e->attr & 0x10) != 0;
    }
    d->heads = 16;
    d->sectors = 63;
    d->cylinders = (uint16_t)fd->cylinders;
    d->total = fd->cylinders * CYLINDER;
    PC_LOG("folder %s: %u files, %u folders%s, %u KB clusters", fd->path, (unsigned)files, (unsigned)folders,
           out ? ", some left out (no room)" : "", (unsigned)fd->spc / 2);
    return fd;
}
