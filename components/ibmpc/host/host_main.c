/*
 * Host build of the IBM PC: runs it far faster than real time, the screen
 * to PPM files.
 *
 *   build/pc_host [frames] [out.ppm]
 *
 * Environment:
 *   PC_FLOPPY_A, PC_FLOPPY_B, PC_HD, PC_HD2   disk images (A:, B:, C:, D:; a hard
 *                                  disk can be a folder, read only)
 *   PC_DUMP_HD=<file>              write C:'s sectors to the file and stop (a folder's)
 *   PC_VIDEO=cga|herc|ega|vga   PC_CPU=8088|8086   PC_RAM=<KB>
 *   PC_BIOS=<rom file>             a real BIOS instead of the built-in one
 *   PC_MACHINE=at (an 80286 at PC_MHZ, default 6, with PC_EXT KB extended)
 *   PC_TYPE="<frame>:<text>;..."   type text from that frame (\n Enter)
 *   PC_SHOTS="<frame>,..."         a PPM at each of those frames (<out>-<frame>.ppm)
 *   PC_MOUSE="<frame>:<dx>,<dy>,<buttons>;..."   the COM1 mouse at those frames
 *   PC_INSERT="<frame>:<drive>:<image>;..."   a disk into drive 0-3 (A: B: C: D:) at
 *                                  that frame (no image: eject)
 *   PC_KEYS="<frame>:<hex>[:<frames>];..."  a key (scan code set 1, E0xx extended)
 *                                  pressed at that frame, held 2 frames (or as given)
 *   PC_NEW_HD=<path>:<MB>          make a blank hard disk (cylinders of 16 x 63 sectors)
 *   PC_NEW_FLOPPY=<path>:<KB>      make a blank floppy (360, 720, 1200, 1440)
 *   PC_TRACE=<seg>:<off>[:<n>]     disassemble n instructions (default 200) from there
 *   PC_TRACE_ALL=<frame>           every instruction from that frame on (to stderr)
 *   PC_WAV=<file.wav>              the speaker's sound
 *   PC_PRINTER=<file>              LPT1's printer: what's printed, appended
 *   PC_FPU                         a coprocessor (the 8087, the AT's 287)
 *   PC_LOG13                       log INT 13h calls and the hard disk controller's commands
 *   PC_QUIET                       no frame summary
 *   PC_DEBUG=<port>                serve the debugger (esp32emu's docs/debugger.md) and run
 *                                  in real time until stopped
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pc.h"
#include "emudbg.h"
#include "x86.h"

extern bool pc_log_int13;
bool disk_read(int d, uint32_t lba, uint32_t count, uint8_t *dst);

static pc_config_t s_cfg;
static long s_frame;
static int s_trace_left;
static uint16_t s_trace_seg, s_trace_off;
static long s_trace_all = -1;
static FILE *s_printer;

static bool printer(uint8_t byte)
{
    fputc(byte, s_printer);
    return true;
}

void x86_host_hook(x86_t *c);

void x86_host_hook(x86_t *c)
{
    uint16_t cs = c->seg[X86_CS].sel, ip = c->ip;
    if (!s_trace_left && s_trace_seg | s_trace_off && cs == s_trace_seg && ip == s_trace_off) {
        s_trace_left = s_trace_left ? s_trace_left : 200;
        char *t = getenv("PC_TRACE");
        char *p = t ? strchr(strchr(t, ':') + 1, ':') : NULL;
        if (p) {
            s_trace_left = atoi(p + 1);
        }
        s_trace_seg = s_trace_off = 0;
    }
    if (s_trace_left > 0 || (s_trace_all >= 0 && s_frame >= s_trace_all)) {
        char text[64];
        uint8_t bytes[16];          /* (from CS's base: protected mode's too) */
        for (int i = 0; i < 16; i++) {
            bytes[i] = x86_peek8(c, c->seg[X86_CS].base + ip + (uint32_t)i);
        }
        x86_disassemble_bytes(c->model, bytes, ip, text, sizeof(text));
        fprintf(stderr, "%04X:%04X  %-28s AX=%04X BX=%04X CX=%04X DX=%04X SP=%04X BP=%04X SI=%04X DI=%04X "
                "DS=%04X ES=%04X SS=%04X F=%04X\n", cs, ip, text, c->r.w[X86_AX], c->r.w[X86_BX],
                c->r.w[X86_CX], c->r.w[X86_DX], c->r.w[X86_SP], c->r.w[X86_BP], c->r.w[X86_SI],
                c->r.w[X86_DI], c->seg[X86_DS].sel, c->seg[X86_ES].sel, c->seg[X86_SS].sel,
                x86_get_flags(c));
        if (s_trace_left > 0) {
            s_trace_left--;
        }
    }
}

static void write_ppm(const char *path)
{
    int w, h;
    pc_screen_signature();
    pc_screen_size(&w, &h);
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    uint16_t *line = malloc((size_t)w * sizeof(uint16_t));
    for (int y = 0; y < h; y++) {
        pc_render_line_rgb565(y, line);
        for (int x = 0; x < w; x++) {
            uint16_t p = line[x];
            uint8_t rgb[3] = {
                (uint8_t)(((p >> 11) & 31) * 255 / 31),
                (uint8_t)(((p >> 5) & 63) * 255 / 63),
                (uint8_t)((p & 31) * 255 / 31),
            };
            fwrite(rgb, 1, 3, f);
        }
    }
    free(line);
    fclose(f);
}

/* "path:N" */
static bool path_and_number(const char *spec, char *path, size_t len, long *n)
{
    const char *colon = strrchr(spec, ':');
    if (!colon || (size_t)(colon - spec) >= len) {
        return false;
    }
    memcpy(path, spec, (size_t)(colon - spec));
    path[colon - spec] = 0;
    *n = atol(colon + 1);
    return true;
}

int main(int argc, char **argv)
{
    long frames = argc > 1 ? atol(argv[1]) : 600;
    const char *out = argc > 2 ? argv[2] : "pc.ppm";
    char path[512];
    long n;

    const char *e = getenv("PC_NEW_HD");
    if (e && path_and_number(e, path, sizeof(path), &n)) {
        uint32_t cyl = (uint32_t)(n * 1024 * 1024 / (16 * 63 * 512));
        printf("blank hard disk %s: %s\n", path, pc_hd_create(path, cyl * 16 * 63 * 512) ? "made" : "FAILED");
    }
    e = getenv("PC_NEW_FLOPPY");
    if (e && path_and_number(e, path, sizeof(path), &n)) {
        printf("blank floppy %s: %s\n", path, pc_floppy_create(path, (uint32_t)n * 1024) ? "made" : "FAILED");
    }

    s_cfg.board = PC_XT;
    s_cfg.cpu = getenv("PC_CPU") && !strcmp(getenv("PC_CPU"), "8086") ? X86_8086 : X86_8088;
    s_cfg.cpu_hz = s_cfg.cpu == X86_8086 ? 8000000 : 4772727;
    if (getenv("PC_MACHINE") && !strcmp(getenv("PC_MACHINE"), "at")) {
        s_cfg.board = PC_AT;
        s_cfg.cpu = X86_80286;
        s_cfg.cpu_hz = (uint32_t)(getenv("PC_MHZ") ? atoi(getenv("PC_MHZ")) : 6) * 1000000;
        s_cfg.ext_kb = getenv("PC_EXT") ? (uint32_t)atoi(getenv("PC_EXT")) : 1024;
    }
    const char *video = getenv("PC_VIDEO") ? getenv("PC_VIDEO") : "cga";
    s_cfg.video = !strcmp(video, "herc") ? PC_HERCULES : !strcmp(video, "ega") ? PC_EGA
                  : !strcmp(video, "vga") ? PC_VGA : PC_CGA;
    s_cfg.conv_kb = getenv("PC_RAM") ? (uint16_t)atoi(getenv("PC_RAM")) : 640;
    if (getenv("PC_BIOS")) {
        static uint8_t rom[0x10000];
        FILE *f = fopen(getenv("PC_BIOS"), "rb");
        size_t n = f ? fread(rom, 1, sizeof(rom), f) : 0;
        if (f) {
            fclose(f);
        }
        if (n < 0x2000) {
            fprintf(stderr, "%s: not a BIOS ROM (8 to 64 KB)\n", getenv("PC_BIOS"));
            return 1;
        }
        s_cfg.bios_rom = rom;
        s_cfg.bios_size = (uint32_t)n;
    }
    void *ram;
    if (posix_memalign(&ram, 4096, PC_RAM_BYTES(s_cfg.ext_kb)) != 0) {
        return 1;
    }
    s_cfg.ram = ram;
    s_cfg.fpu = getenv("PC_FPU") != NULL;
    if ((e = getenv("PC_PRINTER")) && (s_printer = fopen(e, "ab"))) {
        s_cfg.printer = printer;
    }
    s_cfg.page_table = calloc(X86_PAGES, sizeof(uintptr_t));
    if (!pc_init(&s_cfg)) {
        fprintf(stderr, "pc_init failed\n");
        return 1;
    }
    if ((e = getenv("PC_FLOPPY_A")) && !pc_floppy_insert(0, e)) {
        fprintf(stderr, "%s: not a floppy image\n", e);
    }
    if ((e = getenv("PC_FLOPPY_B")) && !pc_floppy_insert(1, e)) {
        fprintf(stderr, "%s: not a floppy image\n", e);
    }
    if ((e = getenv("PC_HD")) && !pc_hd_insert(0, e)) {
        fprintf(stderr, "%s: not a hard disk image\n", e);
    }
    if ((e = getenv("PC_HD2")) && !pc_hd_insert(1, e)) {
        fprintf(stderr, "%s: not a hard disk image\n", e);
    }
    if ((e = getenv("PC_DUMP_HD"))) {
        FILE *f = fopen(e, "wb");
        uint8_t s[64 * 512];
        uint32_t total = pc_hd_size(0) / 512;
        for (uint32_t lba = 0; f && lba < total; lba += 64) {
            uint32_t n = total - lba < 64 ? total - lba : 64;
            if (!disk_read(2, lba, n, s) || fwrite(s, 512, n, f) != n) {
                fprintf(stderr, "%s: failed at sector %u\n", e, lba);
                break;
            }
        }
        return f && fclose(f) == 0 ? 0 : 1;
    }
    if ((e = getenv("PC_TRACE"))) {
        unsigned seg = 0, off = 0;
        sscanf(e, "%x:%x", &seg, &off);
        s_trace_seg = (uint16_t)seg;
        s_trace_off = (uint16_t)off;
    }
    pc_log_int13 = getenv("PC_LOG13") != NULL;
    if ((e = getenv("PC_TRACE_ALL"))) {
        s_trace_all = atol(e);
    }

    FILE *wav = NULL;
    uint32_t wav_frames = 0;
    if ((e = getenv("PC_WAV")) && (wav = fopen(e, "wb"))) {
        uint8_t header[44] = { 0 };
        fwrite(header, 1, 44, wav);     /* filled in at the end */
        pc_audio_enable(true);
    }
    const char *type = getenv("PC_TYPE");
    const char *shots = getenv("PC_SHOTS");
    const char *mouse = getenv("PC_MOUSE");
    const char *keys = getenv("PC_KEYS");
    const char *inserts = getenv("PC_INSERT");
    char base[480];
    snprintf(base, sizeof(base), "%s", out);
    char *dot = strrchr(base, '.');
    if (dot) {
        *dot = 0;
    }
    emudbg_host_run("PC_DEBUG", pc_run_frame, pc_screen_hz());
    for (s_frame = 0; s_frame < frames; s_frame++) {
        pc_run_frame();
        if (wav) {
            int16_t buf[2048 * 2];
            int n;
            while ((n = pc_audio_render(buf, 2048)) > 0) {
                fwrite(buf, 4, (size_t)n, wav);
                wav_frames += (uint32_t)n;
            }
        }
        /* Typing: "<frame>:<text>" entries */
        for (const char *p = type; p && *p;) {
            long at = atol(p);
            const char *colon = strchr(p, ':');
            const char *end = strchr(p, ';');
            if (!colon) {
                break;
            }
            if (at == s_frame) {
                char text[256];
                int k = 0;
                for (const char *q = colon + 1; *q && q != end && k < 255; q++) {
                    if (q[0] == '\\' && q[1] == 'n') {
                        text[k++] = '\n';
                        q++;
                    } else {
                        text[k++] = *q;
                    }
                }
                text[k] = 0;
                pc_type(text);
            }
            p = end ? end + 1 : NULL;
        }
        for (const char *p = keys; p && *p;) {
            long at = atol(p);
            unsigned code = 0, hold = 2;
            const char *c = strchr(p, ':');
            if (c && sscanf(c + 1, "%x:%u", &code, &hold) >= 1) {
                if (at == s_frame) {
                    pc_key((uint16_t)code, true);
                }
                if (at + (long)hold == s_frame) {
                    pc_key((uint16_t)code, false);
                }
            }
            p = strchr(p, ';');
            p = p ? p + 1 : NULL;
        }
        for (const char *p = inserts; p && *p;) {
            int drive = -1;
            char path[400] = "";
            const char *c = strchr(p, ':');
            const char *end = strchr(p, ';');
            if (atol(p) == s_frame && c && sscanf(c + 1, "%d", &drive) == 1) {
                const char *q = strchr(c + 1, ':');
                size_t n = q ? (size_t)((end ? end : q + strlen(q)) - (q + 1)) : 0;
                if (q && n < sizeof(path)) {
                    memcpy(path, q + 1, n);
                    path[n] = 0;
                }
                bool ok = !path[0] ? (drive < PC_FLOPPIES ? (pc_floppy_eject(drive), true) : (pc_hd_eject(drive - PC_FLOPPIES), true))
                        : drive < PC_FLOPPIES ? pc_floppy_insert(drive, path) : pc_hd_insert(drive - PC_FLOPPIES, path);
                if (!ok) {
                    fprintf(stderr, "frame %ld: can't insert %s\n", s_frame, path);
                }
            }
            p = end ? end + 1 : NULL;
        }
        for (const char *p = mouse; p && *p;) {
            int dx, dy, b;
            if (atol(p) == s_frame && sscanf(strchr(p, ':') ? strchr(p, ':') + 1 : "", "%d,%d,%d", &dx, &dy, &b) == 3) {
                pc_mouse(dx, dy, (uint8_t)b);
            }
            p = strchr(p, ';');
            p = p ? p + 1 : NULL;
        }
        for (const char *p = shots; p && *p;) {
            if (atol(p) == s_frame) {
                char name[512];
                snprintf(name, sizeof(name), "%s-%ld.ppm", base, s_frame);
                write_ppm(name);
            }
            p = strchr(p, ',');
            p = p ? p + 1 : NULL;
        }
        if (pc_halted()) {
            fprintf(stderr, "the CPU stopped (protected mode) at frame %ld\n", s_frame);
            break;
        }
    }
    write_ppm(out);
    if (wav) {
        uint32_t bytes = wav_frames * 4, rate = PC_AUDIO_HZ;
        uint8_t h[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
                          16, 0, 0, 0, 1, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 16, 0,
                          'd', 'a', 't', 'a', 0, 0, 0, 0 };
        uint32_t riff = bytes + 36, brate = rate * 4;
        memcpy(h + 4, &riff, 4);
        memcpy(h + 24, &rate, 4);
        memcpy(h + 28, &brate, 4);
        memcpy(h + 40, &bytes, 4);
        fseek(wav, 0, SEEK_SET);
        fwrite(h, 1, 44, wav);
        fclose(wav);
    }
    if (!getenv("PC_QUIET")) {
        pc_status_t s;
        pc_get_status(&s);
        printf("%ld frames, %u instructions in the last, %04X:%04X, %s (%dx%d)\n", s_frame, (unsigned)s.insns,
               s.cs, s.ip, s.mode, s.width, s.height);
        pc_machine_t m;
        pc_get_machine(&m);
        printf("machine: %s A20 %s, BIOS mode %02Xh page %u cursor %u,%u, shift flags %02X, timer %u, speaker %u "
               "mode %u port 61h %02X, IRQ mask %04X, AdLib keys %03X rhythm %02X, SB %s%s %u Hz speaker %s, "
               "COM1 divisor %u LCR %02X MCR %02X, %d colours:", m.protected_mode ? "protected" : "real",
               m.a20 ? "open" : "closed", m.bios_mode, m.page, m.cursor_row, m.cursor_col, m.shift_flags,
               (unsigned)m.timer_reload, (unsigned)m.speaker_reload, m.speaker_mode, m.port61, m.irq_masked,
               m.adlib_keys, m.adlib_rhythm, m.sb_playing ? "playing" : "idle", m.sb_auto ? " auto" : "",
               (unsigned)m.sb_rate, m.sb_speaker ? "on" : "off", m.com1_divisor, m.com1_lcr, m.com1_mcr,
               m.palette_count);
        for (int i = 0; i < m.palette_count; i++) {
            printf(" %06X", (unsigned)m.palette[i]);
        }
        printf("\n");
        uint32_t free_kb, largest_kb;
        if (pc_dos_memory(&free_kb, &largest_kb)) {
            printf("DOS: %u KB free, the largest block %u KB\n", (unsigned)free_kb, (unsigned)largest_kb);
        }
        for (int d = 0; d < PC_FLOPPIES + PC_HARD_DISKS; d++) {
            uint64_t free_bytes, size;
            if (pc_disk_space(d, &free_bytes, &size)) {
                printf("%c: %llu bytes free of %llu\n", 'A' + d, (unsigned long long)free_bytes,
                       (unsigned long long)size);
            }
        }
    }
    return 0;
}
