/*
 * IBM PC emulator on the Makerfabs MaTouch ESP32-S3 7" and the M5Stack Tab5.
 *
 * The board side (tasks, LCD, touch, WiFi, Bluetooth, setup screen, web
 * page, console) is the shared runtime in ../esp32emu; this file plugs the
 * PC into it: memory, settings, the keyboard map, the disk drives, status.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_memory_utils.h"
#include "sdkconfig.h"
#include "emu/console.h"
#include "emu/emu.h"
#include "pc.h"
#include "settings.h"
#include "x86.h"

static const char *TAG = "main";

static pc_config_t s_cfg;
static uint32_t s_ram_bytes;        /* reserved */
static uint32_t s_table_pages;

/* ---- Memory: the PC's memory and the CPU's page table, first thing at
 * power-on (PSRAM fragments later). The Tab5 takes the AT's most (16 MB of
 * its 32); the MaTouch what the settings need, and restarts into the
 * machine for more. The table: the CPU looks there when a segment changes
 * and on the way to the bus, not at each access. In internal RAM on the
 * Tab5; in PSRAM on the MaTouch, whose internal RAM (16 KB for the AT's
 * table) goes to the instruction cache instead.
 */
#if CONFIG_IDF_TARGET_ESP32P4
#define RESERVE_EXT_KB  15360
#else
#define RESERVE_EXT_KB  settings.ext_kb
#endif

static void *alloc_table(uint32_t pages)
{
    size_t size = pages * sizeof(uintptr_t);
#if CONFIG_IDF_TARGET_ESP32P4
    void *t = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return t ? t : heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
#else
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
#endif
}

static void reserve_memory(void)
{
    settings_load();
    uint32_t ext = settings.cpu == X86_80286 ? RESERVE_EXT_KB : 0;
    s_ram_bytes = PC_RAM_BYTES(ext);
    s_cfg.ram = heap_caps_aligned_alloc(4096, s_ram_bytes, MALLOC_CAP_SPIRAM);
#if CONFIG_IDF_TARGET_ESP32P4
    s_table_pages = X86_PAGES;
#else
    s_table_pages = PC_PAGES(settings.cpu == X86_80286 ? PC_AT : PC_XT);
#endif
    s_cfg.page_table = alloc_table(s_table_pages);
    ESP_LOGI(TAG, "RAM %u KB %s, page table in %s RAM", (unsigned)(s_ram_bytes >> 10), s_cfg.ram ? "ok" : "MISSING",
             esp_ptr_internal(s_cfg.page_table) ? "internal" : "PSRAM");
}

/* ---- The picture: each frame's card memory copied on core 0 ---- */

static TaskHandle_t s_copier;

static void copier_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        pc_copy_picture();
    }
}

static void picture_ready(void)
{
    xTaskNotifyGive(s_copier);
}

/* ---- Starting ---- */

/* The presenter and the console's screenshots draw lines at once */
static SemaphoreHandle_t s_render_mutex;

static void render_lock(bool take)
{
    if (take) {
        xSemaphoreTake(s_render_mutex, portMAX_DELAY);
    } else {
        xSemaphoreGive(s_render_mutex);
    }
}

/* ---- LPT1's printer: PRINTER.TXT in the disks' folder ----
 *
 * The emulation puts the bytes in a ring; once a second they're added to
 * the file (core 0: the card's writes would hold up the emulation). The
 * printer is busy while the ring is full: 16 KB a second at most.
 */
#define PRINTER_RING    16384

static uint8_t *s_printer_ring;
static volatile uint32_t s_printer_head, s_printer_tail;
static uint32_t s_printed;

static bool printer(uint8_t byte)
{
    uint32_t head = s_printer_head;
    if (head - __atomic_load_n(&s_printer_tail, __ATOMIC_ACQUIRE) >= PRINTER_RING) {
        return false;
    }
    s_printer_ring[head % PRINTER_RING] = byte;
    __atomic_store_n(&s_printer_head, head + 1, __ATOMIC_RELEASE);
    return true;
}

static void every_second(void)
{
    uint32_t head = __atomic_load_n(&s_printer_head, __ATOMIC_ACQUIRE), tail = s_printer_tail;
    if (head == tail) {
        return;
    }
    char path[160];
    snprintf(path, sizeof(path), "%s/PRINTER.TXT", disks_folder());
    FILE *f = fopen(path, "ab");
    while (f && tail != head) {
        uint32_t at = tail % PRINTER_RING, n = head - tail;
        n = n < PRINTER_RING - at ? n : PRINTER_RING - at;
        fwrite(s_printer_ring + at, 1, n, f);
        tail += n;
    }
    if (f) {
        fclose(f);
    }
    s_printed += tail - s_printer_tail;
    __atomic_store_n(&s_printer_tail, head, __ATOMIC_RELEASE);
}

static bool start(void)
{
    bool at = settings.cpu == X86_80286;
    uint32_t ext = at ? settings.ext_kb : 0;
    if (s_cfg.ram == NULL || PC_RAM_BYTES(ext) > s_ram_bytes) {
        /* more than was reserved: only right after power-on is there room */
        emu_reboot_into_machine();
        return false;
    }
    if (PC_PAGES(at ? PC_AT : PC_XT) > s_table_pages) {
        void *t = alloc_table(X86_PAGES);
        if (t) {
            free(s_cfg.page_table);
            s_cfg.page_table = t;
            s_table_pages = X86_PAGES;
        }
    }
    if (s_cfg.page_table == NULL || PC_PAGES(at ? PC_AT : PC_XT) > s_table_pages) {
        emu_set_message("Not enough memory for the PC.");
        return false;
    }
    s_cfg.board = at ? PC_AT : PC_XT;
    s_cfg.cpu = settings.cpu;
    s_cfg.cpu_hz = settings.khz == 4772 ? 4772727 : settings.khz * 1000u;
    s_cfg.fpu = settings.fpu != 0;
    s_cfg.video = settings.video;
    s_cfg.conv_kb = settings.ram_kb;
    s_cfg.ext_kb = ext;
    if (s_copier == NULL) {
        xTaskCreatePinnedToCore(copier_task, "picture", 3072, NULL, 6, &s_copier, 0);
    }
    s_cfg.picture_ready = s_copier ? picture_ready : NULL;
    if (s_render_mutex == NULL) {
        s_render_mutex = xSemaphoreCreateMutex();
    }
    s_cfg.render_lock = s_render_mutex ? render_lock : NULL;
    if (s_printer_ring == NULL && disks_ready()) {
        s_printer_ring = heap_caps_malloc(PRINTER_RING, MALLOC_CAP_SPIRAM);
    }
    s_cfg.printer = s_printer_ring ? printer : NULL;
    if (!pc_init(&s_cfg)) {
        emu_set_message("The PC could not be set up: see the log.");
        return false;
    }
    return true;
}

static void screen_mode(emu_screen_t *mode)
{
    pc_screen_size(&mode->width, &mode->height);
    mode->hz = pc_screen_hz();
}

/* ---- Keyboard ----
 *
 * HID keyboard usages 0x00-0x67 to scan code set 1 (US positions; 0xE0xx
 * the 101-key keyboard's extended keys, which the XT's keyboard sends as
 * its keypad's and main block's)
 */
static const uint16_t usage_to_pc[0x68] = {
    [0x04] = 0x1E, [0x05] = 0x30, [0x06] = 0x2E, [0x07] = 0x20,     /* a b c d */
    [0x08] = 0x12, [0x09] = 0x21, [0x0A] = 0x22, [0x0B] = 0x23,     /* e f g h */
    [0x0C] = 0x17, [0x0D] = 0x24, [0x0E] = 0x25, [0x0F] = 0x26,     /* i j k l */
    [0x10] = 0x32, [0x11] = 0x31, [0x12] = 0x18, [0x13] = 0x19,     /* m n o p */
    [0x14] = 0x10, [0x15] = 0x13, [0x16] = 0x1F, [0x17] = 0x14,     /* q r s t */
    [0x18] = 0x16, [0x19] = 0x2F, [0x1A] = 0x11, [0x1B] = 0x2D,     /* u v w x */
    [0x1C] = 0x15, [0x1D] = 0x2C,                                   /* y z */
    [0x1E] = 0x02, [0x1F] = 0x03, [0x20] = 0x04, [0x21] = 0x05,     /* 1 2 3 4 */
    [0x22] = 0x06, [0x23] = 0x07, [0x24] = 0x08, [0x25] = 0x09,     /* 5 6 7 8 */
    [0x26] = 0x0A, [0x27] = 0x0B,                                   /* 9 0 */
    [0x28] = 0x1C,      /* Enter */
    [0x29] = 0x01,      /* Escape */
    [0x2A] = 0x0E,      /* Backspace */
    [0x2B] = 0x0F,      /* Tab */
    [0x2C] = 0x39,      /* Space */
    [0x2D] = 0x0C,      /* - */
    [0x2E] = 0x0D,      /* = */
    [0x2F] = 0x1A,      /* [ */
    [0x30] = 0x1B,      /* ] */
    [0x31] = 0x2B,      /* \ */
    [0x32] = 0x2B,      /* non-US # */
    [0x33] = 0x27,      /* ; */
    [0x34] = 0x28,      /* ' */
    [0x35] = 0x29,      /* ` */
    [0x36] = 0x33,      /* , */
    [0x37] = 0x34,      /* . */
    [0x38] = 0x35,      /* / */
    [0x39] = 0x3A,      /* Caps Lock */
    [0x3A] = 0x3B, [0x3B] = 0x3C, [0x3C] = 0x3D, [0x3D] = 0x3E,     /* F1-F4 */
    [0x3E] = 0x3F, [0x3F] = 0x40, [0x40] = 0x41, [0x41] = 0x42,     /* F5-F8 */
    [0x42] = 0x43, [0x43] = 0x44, [0x44] = 0x57, [0x45] = 0x58,     /* F9-F12 */
    [0x46] = 0xE037,    /* Print Screen */
    [0x47] = 0x46,      /* Scroll Lock */
    [0x49] = 0xE052,    /* Insert */
    [0x4A] = 0xE047,    /* Home */
    [0x4B] = 0xE049,    /* Page Up */
    [0x4C] = 0xE053,    /* Delete */
    [0x4D] = 0xE04F,    /* End */
    [0x4E] = 0xE051,    /* Page Down */
    [0x4F] = 0xE04D,    /* Right */
    [0x50] = 0xE04B,    /* Left */
    [0x51] = 0xE050,    /* Down */
    [0x52] = 0xE048,    /* Up */
    [0x53] = 0x45,      /* Num Lock */
    [0x54] = 0xE035,    /* keypad / */
    [0x55] = 0x37,      /* keypad * */
    [0x56] = 0x4A,      /* keypad - */
    [0x57] = 0x4E,      /* keypad + */
    [0x58] = 0xE01C,    /* keypad Enter */
    [0x59] = 0x4F, [0x5A] = 0x50, [0x5B] = 0x51,                    /* keypad 1 2 3 */
    [0x5C] = 0x4B, [0x5D] = 0x4C, [0x5E] = 0x4D,                    /* keypad 4 5 6 */
    [0x5F] = 0x47, [0x60] = 0x48, [0x61] = 0x49,                    /* keypad 7 8 9 */
    [0x62] = 0x52,      /* keypad 0 */
    [0x63] = 0x53,      /* keypad . */
    [0x64] = 0x2B,      /* non-US \ (left of Z) */
};

/* Modifier usages $E0-$E7: LCtrl LShift LAlt LGUI RCtrl RShift RAlt RGUI
 * (the PC's keyboards had no GUI keys: nothing)
 */
static const uint16_t modifier_to_pc[8] = { 0x1D, 0x2A, 0x38, 0, 0xE01D, 0x36, 0xE038, 0 };

static void hid_key(uint8_t usage, bool pressed)
{
    uint16_t code = 0;
    if (usage >= 0xE0 && usage <= 0xE7) {
        code = modifier_to_pc[usage - 0xE0];
    } else if (usage < sizeof(usage_to_pc) / sizeof(usage_to_pc[0])) {
        code = usage_to_pc[usage];
    }
    if (code) {
        pc_key(code, pressed);
    }
}

/* ---- The mouse on COM1: the Bluetooth one, touch as a touchpad ---- */

static void hid_mouse(int dx, int dy, uint8_t buttons)
{
    pc_mouse(dx, dy, buttons & 3);
}

#define TOUCH_SLOP      6               /* picture pixels before a touch is a drag */
#define TOUCH_RIGHT_US  600000          /* a press held this long: the right button */

/* A drag moves the pointer as far as the finger goes (the picture's pixels
 * as mickeys of a 640 x 400 screen), a tap clicks the left button, a long
 * press the right one
 */
static void touch(int x, int y, bool pressed)
{
    static enum { IDLE, PENDING, DRAG, HELD } state;
    static int x0, y0, lx, ly, fx, fy;
    static int64_t t0;
    int64_t now = esp_timer_get_time();
    if (!pressed) {
        if (state == PENDING) {
            pc_mouse(0, 0, 1);          /* (the press is kept for a packet) */
            pc_mouse(0, 0, 0);
        }
        state = IDLE;
        return;
    }
    switch (state) {
    case IDLE:
        x0 = lx = x;
        y0 = ly = y;
        t0 = now;
        state = PENDING;
        break;
    case PENDING:
        if (abs(x - x0) > TOUCH_SLOP || abs(y - y0) > TOUCH_SLOP) {
            state = DRAG;
        } else if (now - t0 >= TOUCH_RIGHT_US) {
            pc_mouse(0, 0, 2);
            pc_mouse(0, 0, 0);
            state = HELD;
            break;
        } else {
            break;
        }
        __attribute__((fallthrough));
    case DRAG: {
        int w, h;
        pc_screen_size(&w, &h);
        fx += (x - lx) * 640;
        fy += (y - ly) * 400;
        int dx = fx / w, dy = fy / h;
        fx -= dx * w;
        fy -= dy * h;
        lx = x;
        ly = y;
        if (dx || dy) {
            pc_mouse(dx, dy, 0);
        }
        break;
    }
    case HELD:
        break;
    }
}

/* ---- Setup screen choices ----
 *
 * Machine: the PC/XT and PC/AT (choosing one sets the others to its
 * defaults); Processor: the machine's; Coprocessor: off or on (the 8087,
 * the 287); Video; Memory: the processor's. The BIOS is the built-in one
 * (a real ROM would come with the firmware: pc_config_t.bios_rom).
 */
enum { CHOICE_MACHINE, CHOICE_PROCESSOR, CHOICE_FPU, CHOICE_VIDEO, CHOICE_MEMORY, CHOICES };

static const char *const machine_names[] = { "PC/XT", "PC/AT", "" };
static const char *const fpu_names[] = { "Off", "On", "" };
static const char *const video_names[] = { "CGA", "Hercules", "EGA", "VGA", "" };

static int machine_get(void)
{
    return settings.machine;
}

static void machine_set(int i)
{
    if (i >= MACHINE_XT && i <= MACHINE_AT && i != settings.machine) {
        settings_defaults(i);
        settings_save();
    }
}

static int fpu_get(void)
{
    return settings.fpu != 0;
}

static void fpu_set(int i)
{
    if (i == 0 || i == 1) {
        settings.fpu = (uint8_t)i;
        settings_save();
    }
}

static int processor_get(void)
{
    int n;
    const processor_t *p = settings_processors(settings.machine, &n);
    for (int i = 0; i < n; i++) {
        if (p[i].cpu == settings.cpu && p[i].khz == settings.khz) {
            return i;
        }
    }
    return 0;
}

static void processor_set(int i)
{
    int n;
    const processor_t *p = settings_processors(settings.machine, &n);
    if (i >= 0 && i < n) {
        bool was_286 = settings.cpu == X86_80286;
        settings.cpu = p[i].cpu;
        settings.khz = p[i].khz;
        if (was_286 != (settings.cpu == X86_80286)) {
            settings.ram_kb = 640;                 /* the other memory row */
            settings.ext_kb = settings.cpu == X86_80286 ? 1024 : 0;
        }
        settings_save();
    }
}

/* (the row's order is pc_video_t's) */
static int video_get(void)
{
    return settings.video <= PC_VGA ? settings.video : 0;
}

static void video_set(int i)
{
    if (i >= PC_CGA && i <= PC_VGA) {
        settings.video = (uint8_t)i;
        settings_save();
    }
}

static int memory_get(void)
{
    int n;
    const memory_t *m = settings_memories(settings.cpu, &n);
    for (int i = 0; i < n; i++) {
        if (m[i].ram_kb == settings.ram_kb && m[i].ext_kb == settings.ext_kb) {
            return i;
        }
    }
    return n - 1;
}

static void memory_set(int i)
{
    int n;
    const memory_t *m = settings_memories(settings.cpu, &n);
    if (i >= 0 && i < n) {
        settings.ram_kb = m[i].ram_kb;
        settings.ext_kb = m[i].ext_kb;
        settings_save();
    }
}

/* The Processor and Memory rows' buttons follow the machine and processor */
static const char *s_processor_names[5], *s_memory_names[5];

static const char *const *choice_names(int i)
{
    int n;
    if (i == CHOICE_PROCESSOR) {
        const processor_t *p = settings_processors(settings.machine, &n);
        for (int k = 0; k < n; k++) {
            s_processor_names[k] = p[k].name;
        }
        s_processor_names[n] = "";
        return s_processor_names;
    }
    if (i == CHOICE_MEMORY) {
        const memory_t *m = settings_memories(settings.cpu, &n);
        for (int k = 0; k < n; k++) {
            s_memory_names[k] = m[k].name;
        }
        s_memory_names[n] = "";
        return s_memory_names;
    }
    return NULL;
}

static const char *const default_names[] = { "-", "" };

static const emu_choice_t choices[CHOICES] = {
    { "Machine", machine_names, machine_get, machine_set, false },
    { "Processor", default_names, processor_get, processor_set, false },
    { "Coprocessor", fpu_names, fpu_get, fpu_set, false },
    { "Video", video_names, video_get, video_set, false },
    { "Memory", default_names, memory_get, memory_set, false },
};

/* ---- Disks: two floppy drives, two hard disks ---- */

#define DRIVES      (PC_FLOPPIES + PC_HARD_DISKS)
#define MB(n)       ((uint32_t)(n) * 1024 * 1024 / 516096 * 516096)    /* whole cylinders */

static const char *const extensions[] = { ".img", ".ima", ".vfd", ".flp", ".hdd", ".vhd", NULL };
static const char *const drive_names[DRIVES] = { "A:", "B:", "C:", "D:" };
static const disks_size_t sizes[] = {
    { 1474560, "1.44 MB floppy (3.5\")" },
    { 737280, "720 KB floppy (3.5\")" },
    { 1228800, "1.2 MB floppy (5.25\")" },
    { 368640, "360 KB floppy (5.25\")" },
    { MB(10), "10 MB hard disk (FAT12: DOS 2 and later)" },
    { MB(20), "20 MB hard disk (FAT16: DOS 3 and later)" },
    { MB(32), "32 MB hard disk (FAT16: DOS 3.3 and later)" },
    { MB(64), "64 MB hard disk (FAT16: DOS 4 and later)" },
    { MB(128), "128 MB hard disk (FAT16: DOS 4 and later)" },
};

/* A hard disk: .hdd, .vhd, an .img too big for a floppy, or a folder ("NAME/") */
static bool hd_name(const char *name)
{
    size_t n = strlen(name);
    if (n > 4 && (!strcasecmp(name + n - 4, ".hdd") || !strcasecmp(name + n - 4, ".vhd"))) {
        return true;
    }
    if (n > 1 && name[n - 1] == '/') {
        return true;
    }
    if (n > 4 && !strcasecmp(name + n - 4, ".img")) {
        char path[160];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", disks_folder(), name);
        return stat(path, &st) == 0 && st.st_size > 2880 * 1024;
    }
    return false;
}

static bool disk_accepts(int drive, const char *name)
{
    return (drive >= PC_FLOPPIES) == hd_name(name);
}

static uint32_t disk_default_size(int drive)
{
    return drive >= PC_FLOPPIES ? MB(32) : 1474560;
}

static const char *disk_extension_for(uint32_t bytes)
{
    return bytes > 2880 * 1024 ? ".hdd" : ".img";
}

static bool disk_insert(int drive, const char *path)
{
    return drive >= PC_FLOPPIES ? pc_hd_insert(drive - PC_FLOPPIES, path) : pc_floppy_insert(drive, path);
}

static void disk_eject(int drive)
{
    if (drive >= PC_FLOPPIES) {
        pc_hd_eject(drive - PC_FLOPPIES);
    } else {
        pc_floppy_eject(drive);
    }
}

static const char *disk_path(int drive)
{
    return drive >= PC_FLOPPIES ? pc_hd_path(drive - PC_FLOPPIES) : pc_floppy_path(drive);
}

static bool disk_write_protected(int drive)
{
    return drive >= PC_FLOPPIES ? pc_hd_write_protected(drive - PC_FLOPPIES) : pc_floppy_write_protected(drive);
}

static bool disk_create(const char *path, uint32_t bytes)
{
    return bytes > 2880 * 1024 ? pc_hd_create(path, bytes) : pc_floppy_create(path, bytes);
}

static const disks_config_t disks = {
    .folder = "pc",
    .extensions = extensions,
    .description = "Disk images (.IMG, .IMA, .VFD; .HDD, .VHD hard disks)",
    .title = "Disks",
    .button = "Disks",
    .nvs_namespace = "pcdisks",
    .drives = DRIVES,
    .drive_names = drive_names,
    .new_prefix = "BLANK",
    .new_extension = ".img",
    .sizes = sizes,
    .size_count = sizeof(sizes) / sizeof(sizes[0]),
    .max_upload = 256ull * 1024 * 1024,
    .folders = true,
    .insert = disk_insert,
    .eject = disk_eject,
    .path = disk_path,
    .write_protected = disk_write_protected,
    .create = disk_create,
    .accepts = disk_accepts,
    .default_size = disk_default_size,
    .extension_for = disk_extension_for,
};

/* ---- Status ---- */

static void status(emu_json_t *j, int section)
{
    char text[160];
    pc_status_t s;
    pc_get_status(&s);
    if (section == EMU_STATUS_ROWS) {
        /* the settings until the machine starts */
        bool started = s_cfg.conv_kb != 0;
        pc_video_t video = started ? s_cfg.video : (pc_video_t)settings.video;
        bool at = started ? s_cfg.board == PC_AT : settings.cpu == X86_80286;
        unsigned ext = started ? s_cfg.ext_kb : settings.ext_kb;
        char mem[32];
        if (at && ext) {
            snprintf(mem, sizeof(mem), "640 KB + %u MB", ext / 1024);
        } else {
            snprintf(mem, sizeof(mem), "%u KB", started ? s_cfg.conv_kb : settings.ram_kb);
        }
        snprintf(text, sizeof(text), "IBM %s, %s", at ? "PC/AT" : "PC/XT", mem);
        emu_json_row(j, "System", text);
        x86_model_t cpu = started ? s_cfg.cpu : (x86_model_t)settings.cpu;
        unsigned khz = started ? s_cfg.cpu_hz / 1000 : settings.khz;
        bool fpu = started ? s_cfg.fpu : settings.fpu != 0;
        snprintf(text, sizeof(text), "%s%s at %u%s MHz <span class=\"dim\">(%.2f million instructions a second)</span>",
                 cpu == X86_80286 ? "80286" : cpu == X86_8086 ? "8086" : "8088",
                 !fpu ? "" : cpu == X86_80286 ? " + 287" : " + 8087", khz / 1000, khz == 4772 ? ".77" : "",
                 s.insns * (double)pc_screen_hz() / 1e6);
        emu_json_row(j, "CPU", text);
        snprintf(text, sizeof(text), "%s: %s", video_names[video <= PC_VGA ? video : 0], s.mode);
        emu_json_row(j, "Video", text);
        /* DOS's, below 640 KB (FreeCOM keeps ~68 KB of it at the prompt) */
        uint32_t free_kb, largest_kb;
        if (started && pc_dos_memory(&free_kb, &largest_kb)) {
            int len = snprintf(text, sizeof(text), "%u KB of %u KB", (unsigned)free_kb, (unsigned)s_cfg.conv_kb);
            if (largest_kb < free_kb) {
                snprintf(text + len, sizeof(text) - (size_t)len, " <span class=\"dim\">(largest block %u KB)</span>",
                         (unsigned)largest_kb);
            }
            emu_json_row_bar(j, "Free RAM", text, 100 - (int)(free_kb * 100 / s_cfg.conv_kb));
        }
        if (s_printed) {
            snprintf(text, sizeof(text), "%u bytes printed to PRINTER.TXT", (unsigned)s_printed);
            emu_json_row(j, "Printer", text);
        }
        return;
    }
    for (int d = 0; d < DRIVES; d++) {
        const char *name = disks_name(d);
        if (d >= PC_FLOPPIES) {
            snprintf(text, sizeof(text), "%u MB, cylinder %u", (unsigned)(pc_hd_size(d - PC_FLOPPIES) >> 20),
                     (unsigned)s.cylinder[d]);
        } else {
            snprintf(text, sizeof(text), "cylinder %u", (unsigned)s.cylinder[d]);
        }
        emu_json_drive(j, drive_names[d], name, disk_write_protected(d), s.light == d, s.reads[d], s.writes[d],
                       name ? text : NULL);
    }
}

/* ---- Console ---- */

static void console_info(char *buf, size_t len)
{
    pc_status_t s;
    pc_get_status(&s);
    snprintf(buf, len, "frames %lu CS:IP %04X:%04X insns %lu %s %dx%d", (unsigned long)s.frames, s.cs, s.ip,
             (unsigned long)s.insns, s.mode, s.width, s.height);
}

/*   k <hex>            tap the key with that scan code (set 1; E0xx extended)
 *   K <hex> <0|1>      key down (1) or up (0)
 *   T <text>           type text (\n Enter)
 */
static bool console_command(const char *line)
{
    switch (line[0]) {
    case 'k': {
        unsigned code = strtoul(line + 1, NULL, 16);
        pc_key((uint16_t)code, true);
        vTaskDelay(pdMS_TO_TICKS(80));
        pc_key((uint16_t)code, false);
        return true;
    }
    case 'K': {
        unsigned code = 0, down = 0;
        if (sscanf(line + 1, "%x %u", &code, &down) == 2) {
            pc_key((uint16_t)code, down);
        }
        return true;
    }
    case 'T': {
        static char text[256];
        const char *p = line + 1;
        while (*p == ' ') {
            p++;
        }
        int n = 0;
        for (; *p && n < (int)sizeof(text) - 1; p++) {
            if (p[0] == '\\' && p[1] == 'n') {
                text[n++] = '\n';
                p++;
            } else {
                text[n++] = *p;
            }
        }
        text[n] = 0;
        pc_type(text);
        return true;
    }
    default:
        return false;
    }
}

/* ---- The machine ---- */

#if CONFIG_EMU_WEB
/* web/pcfiles.js: files and folders dropped on the web page become a FAT
 * floppy or hard disk image in the browser */
extern const char pcfiles_js_start[] asm("_binary_pcfiles_js_start");
extern const char pcfiles_js_end[] asm("_binary_pcfiles_js_end");
#endif

static const emu_machine_t machine = {
    .name = "IBM PC",
    .short_name = "PC",
    .view_width = 800,          /* 4:3 for every mode: 960 x 720 on the Tab5 */
    .scale = EMU_SCALE_SMOOTH,
    .reserve_memory = reserve_memory,
    .start = start,
    .reset = pc_reset,
    .run_frame = pc_run_frame,
    .halted = pc_halted,
    .screen_mode = screen_mode,
    .screen_signature = pc_screen_signature,
    .render_line = pc_render_line_rgb565,
    .audio_hz = PC_AUDIO_HZ,
    .audio_enable = pc_audio_enable,
    .audio_render = pc_audio_render,
    .audio_pending = pc_audio_pending,
    .hid_key = hid_key,
    .hid_mouse = hid_mouse,
    .touch = touch,
    .choices = choices,
    .choice_count = CHOICES,
    .choice_names = choice_names,
    .disks = &disks,
#if CONFIG_EMU_WEB
    .web_script = pcfiles_js_start,
    .web_script_end = pcfiles_js_end,
#endif
    .status = status,
    .console_info = console_info,
    .console_command = console_command,
    .every_second = every_second,
};

void app_main(void)
{
    emu_run(&machine);
}
