#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "settings.h"

#define NAMESPACE   "pc"

static const char *TAG = "settings";

settings_t settings;

/* The PC/XT: IBM's 4.77 MHz 8088. The PC/AT: IBM's 6 and 8 MHz 80286. Custom:
 * the 8086 and a 12 MHz 286 too (the board follows the processor). Only
 * what the board runs at full speed: the MaTouch's S3 runs the 286 at
 * 6 MHz (8 MHz came to 97-99%), the Tab5's P4 at 12 MHz too. The XT and
 * the AT with or without the coprocessor (the socket was empty in most);
 * Custom always with it (its buttons don't say).
 */
static const processor_t s_xt[] = {
    { X86_8088, 4772, false, "8088\n4.77 MHz" },
    { X86_8088, 4772, true, "8088\n4.77 MHz + 8087" },
};
#if CONFIG_IDF_TARGET_ESP32P4
static const processor_t s_at[] = {
    { X86_80286, 6000, false, "80286\n6 MHz" },
    { X86_80286, 6000, true, "80286\n6 MHz + 287" },
    { X86_80286, 8000, false, "80286\n8 MHz" },
    { X86_80286, 8000, true, "80286\n8 MHz + 287" },
};
static const processor_t s_custom[] = {
    { X86_8088, 4772, true, "8088\n4.77 MHz" },
    { X86_8086, 8000, true, "8086\n8 MHz" },
    { X86_80286, 8000, true, "80286\n8 MHz" },
    { X86_80286, 12000, true, "80286\n12 MHz" },
};
#else
static const processor_t s_at[] = {
    { X86_80286, 6000, false, "80286\n6 MHz" },
    { X86_80286, 6000, true, "80286\n6 MHz + 287" },
};
static const processor_t s_custom[] = {
    { X86_8088, 4772, true, "8088\n4.77 MHz" },
    { X86_8086, 8000, true, "8086\n8 MHz" },
};
#endif

/* The 8088 and 8086: conventional memory. The 286: 640 KB and extended
 * memory, as much as the board has room for (the MaTouch's 8 MB of PSRAM
 * share the screen, WiFi and Bluetooth)
 */
static const memory_t s_mem_xt[] = {
    { 256, 0, "256 KB" },
    { 512, 0, "512 KB" },
    { 640, 0, "640 KB" },
};
#if CONFIG_IDF_TARGET_ESP32P4
static const memory_t s_mem_at[] = {
    { 640, 0, "640 KB" },
    { 640, 1024, "640 KB\n+ 1 MB" },
    { 640, 4096, "640 KB\n+ 4 MB" },
    { 640, 15360, "640 KB\n+ 15 MB" },
};
#else
static const memory_t s_mem_at[] = {
    { 640, 0, "640 KB" },
    { 640, 1024, "640 KB\n+ 1 MB" },
    { 640, 2048, "640 KB\n+ 2 MB" },
};
#endif

#define COUNT(a)    ((int)(sizeof(a) / sizeof((a)[0])))

const processor_t *settings_processors(int machine, int *count)
{
    switch (machine) {
    case MACHINE_AT: *count = COUNT(s_at); return s_at;
    case MACHINE_CUSTOM: *count = COUNT(s_custom); return s_custom;
    default: *count = COUNT(s_xt); return s_xt;
    }
}

const memory_t *settings_memories(int cpu, int *count)
{
    if (cpu == X86_80286) {
        *count = COUNT(s_mem_at);
        return s_mem_at;
    }
    *count = COUNT(s_mem_xt);
    return s_mem_xt;
}

void settings_defaults(int machine)
{
    settings.machine = (uint8_t)machine;
    settings.fpu = machine == MACHINE_CUSTOM;
    if (machine == MACHINE_AT) {
        settings.cpu = X86_80286;
        settings.khz = 6000;
        settings.ram_kb = 640;
        settings.ext_kb = 1024;
    } else if (machine == MACHINE_XT) {
        settings.cpu = X86_8088;
        settings.khz = 4772;
        settings.ram_kb = 640;
        settings.ext_kb = 0;
    }
    settings.video = machine == MACHINE_AT ? PC_EGA : PC_CGA;      /* (the BIOS stays as chosen) */
}

/* Settings that don't make a machine go back to its defaults */
static void validate(void)
{
    if (settings.machine > MACHINE_CUSTOM) {
        settings_defaults(MACHINE_XT);
        return;
    }
    int n;
    const processor_t *p = settings_processors(settings.machine, &n);
    bool ok = false;
    for (int i = 0; i < n; i++) {
        ok |= p[i].cpu == settings.cpu && p[i].khz == settings.khz && p[i].fpu == (settings.fpu != 0);
    }
    for (int i = 0; i < n && !ok; i++) {
        if (p[i].cpu == settings.cpu && p[i].khz == settings.khz) {
            settings.fpu = p[i].fpu;    /* (the processor, the coprocessor as the machine has it) */
            ok = true;
        }
    }
    if (!ok) {
        settings.cpu = p[0].cpu;
        settings.khz = p[0].khz;
        settings.fpu = p[0].fpu;
    }
    const memory_t *m = settings_memories(settings.cpu, &n);
    ok = false;
    for (int i = 0; i < n; i++) {
        ok |= m[i].ram_kb == settings.ram_kb && m[i].ext_kb == settings.ext_kb;
    }
    if (!ok) {
        settings.ram_kb = m[n > 1 ? 1 : 0].ram_kb;
        settings.ext_kb = m[n > 1 ? 1 : 0].ext_kb;
        if (settings.cpu != X86_80286) {
            settings.ram_kb = 640;
            settings.ext_kb = 0;
        }
    }
    if (settings.video > PC_VGA) {
        settings.video = PC_CGA;
    }
    if (settings.bios > 1) {
        settings.bios = 0;
    }
}

void settings_load(void)
{
    settings_defaults(MACHINE_XT);
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    nvs_get_u8(h, "machine", &settings.machine);
    nvs_get_u8(h, "cpu", &settings.cpu);
    nvs_get_u16(h, "khz", &settings.khz);
    nvs_get_u8(h, "fpu", &settings.fpu);
    nvs_get_u8(h, "video", &settings.video);
    nvs_get_u16(h, "ram_kb", &settings.ram_kb);
    nvs_get_u16(h, "ext_kb", &settings.ext_kb);
    nvs_get_u8(h, "bios", &settings.bios);
    nvs_close(h);
    settings_t before = settings;
    validate();
    if (memcmp(&before, &settings, sizeof(settings))) {
        ESP_LOGW(TAG, "settings didn't make a machine: back to its defaults");
    }
}

void settings_save(void)
{
    validate();
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "can't open NVS");
        return;
    }
    nvs_set_u8(h, "machine", settings.machine);
    nvs_set_u8(h, "cpu", settings.cpu);
    nvs_set_u16(h, "khz", settings.khz);
    nvs_set_u8(h, "fpu", settings.fpu);
    nvs_set_u8(h, "video", settings.video);
    nvs_set_u16(h, "ram_kb", settings.ram_kb);
    nvs_set_u16(h, "ext_kb", settings.ext_kb);
    nvs_set_u8(h, "bios", settings.bios);
    nvs_commit(h);
    nvs_close(h);
}
