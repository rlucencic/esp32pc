/*
 * The PC speaker: port 61h bit 1 and the timer's channel 2 (gated by bit
 * 0) driving one cone
 *
 * The cone is pushed while bit 1 is set and channel 2's output is high (or
 * its gate is off: then bit 1 alone, which is how programs play samples by
 * toggling it). Each 44.1 kHz sample is the share of its time the cone was
 * pushed (the timer's ticks within it; the square wave worked out), caught
 * up whenever the timer or port 61h change and at each frame's end, then a
 * slow high pass takes the steady level away. The Sound Blaster's samples
 * join each one (sound_sample()), the AdLib's on the speaker's task
 * (sound_mix()). A ring the speaker's task takes from, as esp32arc's.
 */
#include "pc_internal.h"

#define RING        8192            /* stereo frames, ~190 ms */
#define LEVEL       9000            /* the pushed cone */

static int16_t *s_ring;
static volatile uint32_t s_write, s_read;
static bool s_enabled;
/* The next sample's start: PIT ticks from s_origin (32.32). The origin
 * moves on each frame: 32.32 of the ticks since the reset would overflow
 * after an hour (2^32 ticks), and no sample would come after it.
 */
static uint64_t s_origin;
static uint64_t s_at;
static uint64_t s_step;             /* PIT ticks a sample (32.32) */
static int32_t s_dc;                /* the high pass's level (x 256) */
static int s_beep;                  /* frames of the BIOS's bell left */

void speaker_reset(void)
{
    if (!s_ring) {
        s_ring = pc_calloc(RING * 2, sizeof(int16_t));
    }
    s_step = ((uint64_t)PIT_HZ << 32) / PC_AUDIO_HZ;
    s_origin = pc_ticks(pc_now());
    s_at = 0;
    s_dc = 0;
    s_beep = 0;
    s_write = s_read = 0;
}

static void emit(int32_t v)
{
    s_dc += (v * 256 - s_dc) >> 9;
    v -= s_dc >> 8;
    v += sound_sample();            /* the Sound Blaster's (its DMA as it goes) */
    if (!s_enabled || !s_ring || s_write - s_read >= RING) {
        return;
    }
    int16_t s = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
    uint32_t i = (s_write % RING) * 2;
    s_ring[i] = s;
    s_ring[i + 1] = s;
    __atomic_store_n(&s_write, s_write + 1, __ATOMIC_RELEASE);
}

void speaker_run(void)
{
    uint64_t now = (pc_ticks(pc_now()) - s_origin) << 32;
    bool data = ppi_portb & 0x02, gate = ppi_portb & 0x01;
    while (s_at + s_step <= now) {
        uint64_t a = s_origin + (s_at >> 32), b = s_origin + ((s_at + s_step) >> 32);
        s_at += s_step;
        uint32_t span = (uint32_t)(b - a), high = 0;
        if (data && span) {
            high = gate ? pit_high2(a, b) : span;
        }
        emit(span ? (int32_t)(high * LEVEL / span) : 0);
    }
}

/* The ring's next sample (what the AdLib's register writes are stamped with) */
uint32_t speaker_position(void)
{
    speaker_run();
    return s_write;
}

/* The CPU clock by which `samples` more will have been made */
uint64_t speaker_clock_after(uint32_t samples)
{
    uint64_t end = s_at + (uint64_t)samples * s_step;
    return pc_clock_of(s_origin + ((end + 0xFFFFFFFFu) >> 32));
}

void speaker_beep(int frames)
{
    speaker_run();
    pit_write(3, 0xB6);             /* channel 2 at 896 Hz, as IBM's BEEP */
    pit_write(2, 0x33);
    pit_write(2, 0x05);
    port61_write((uint8_t)(ppi_portb | 0x03));
    s_beep = frames;
}

void speaker_frame(void)
{
    speaker_run();
    s_origin += s_at >> 32;         /* (the origin to the next sample's whole tick) */
    s_at &= 0xFFFFFFFFu;
    if (s_beep && --s_beep == 0) {
        port61_write((uint8_t)(ppi_portb & ~0x03));
    }
}

/* ---- The speaker's task's side ---- */

void pc_audio_enable(bool on)
{
    s_enabled = on;
    if (!on) {
        s_read = s_write;
    }
}

int pc_audio_pending(void)
{
    return (int)(__atomic_load_n(&s_write, __ATOMIC_ACQUIRE) - s_read);
}

int pc_audio_render(int16_t *dst, int frames)
{
    uint32_t avail = __atomic_load_n(&s_write, __ATOMIC_ACQUIRE) - s_read;
    int n = (int)avail < frames ? (int)avail : frames;
    for (int i = 0; i < n; i++) {
        uint32_t k = ((s_read + (uint32_t)i) % RING) * 2;
        dst[2 * i] = s_ring[k];
        dst[2 * i + 1] = s_ring[k + 1];
    }
    sound_mix(dst, n, s_read);      /* the AdLib's music */
    s_read += (uint32_t)n;
    return n;
}
