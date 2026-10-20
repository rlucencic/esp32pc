/*
 * Sound cards: an AdLib (the YM3812, OPL2, at 388h-389h) and a Sound
 * Blaster 2.0 at 220h (its DSP's 8-bit sound through DMA channel 1 and
 * IRQ 7, and the same OPL2 at 228h-229h): BLASTER=A220 I7 D1 T3
 *
 * The OPL2's timers and status run here, on the machine's time, as games
 * find the card by them; its sound is emu8950's (../esp32emu, MIT) on the
 * speaker's task: the register writes go there in a ring, each stamped with
 * the sound ring's sample it happened at, and sound_mix() applies them as
 * it reaches those samples. The chip runs at its own 49,716 Hz, resampled
 * to 44.1 kHz.
 *
 * The DSP takes its bytes from DMA as speaker.c makes each 44.1 kHz sample
 * (sound_sample()): at the time constant's rate, held between them; a
 * block's last byte raises IRQ 7 (an event wakes speaker.c for it), then
 * auto-init goes on or single-cycle stops. Direct DAC output, the speaker
 * switch, pause and continue, the version (2.01), the test register and
 * the forced interrupt as detection routines use them; recording gives
 * silence, ADPCM blocks pass at their rate without their sound.
 */
#include "pc_internal.h"
#include "emu8950.h"

#define OPL_CLOCK       3579545
#define OPL_RATE        (OPL_CLOCK / 72)                /* 49,716 Hz */
#define QUEUE           1024                            /* register writes */
#define SB_BASE         0x220
#define SB_IRQ          7
#define SB_DMA          1
#define SB_LEVEL        40                              /* a DSP step's size in the mix */
#define OPL_GAIN        4                               /* the AdLib's in the mix */

/* ---- The OPL2: the machine's side ---- */

static struct {
    uint8_t index;
    uint8_t status;                 /* IRQ, timer 1, timer 2 (bits 7-5) */
    uint8_t reg[256];               /* as written (a resync's copy) */
    bool t1_on, t2_on;
    uint64_t t1_start, t2_start;    /* CPU clocks */
} o;

/* The writes for the speaker's task: reg 0x100 resets the chip */
static struct {
    uint32_t stamp;
    uint16_t reg;
    uint8_t value;
} s_queue[QUEUE];
static uint32_t s_head, s_tail;     /* head: the machine's, tail: the speaker's task's */
static volatile bool s_resync;      /* writes were lost: all registers again */

static void queue(uint16_t reg, uint8_t value)
{
    uint32_t head = s_head;
    if (head - __atomic_load_n(&s_tail, __ATOMIC_ACQUIRE) >= QUEUE) {
        s_resync = true;            /* (the speaker's task isn't taking them: sound off) */
        return;
    }
    s_queue[head % QUEUE].stamp = speaker_position();
    s_queue[head % QUEUE].reg = reg;
    s_queue[head % QUEUE].value = value;
    __atomic_store_n(&s_head, head + 1, __ATOMIC_RELEASE);
}

static uint64_t us(uint32_t n)
{
    return (uint64_t)n * pc.cfg.cpu_hz / 1000000;
}

/* The status: the timers that ran out since they started (and aren't masked) */
static uint8_t opl_status(void)
{
    uint64_t now = pc_now();
    if (o.t1_on && !(o.reg[4] & 0x40) && now - o.t1_start >= us((256u - o.reg[2]) * 80u)) {
        o.status |= 0xC0;
    }
    if (o.t2_on && !(o.reg[4] & 0x20) && now - o.t2_start >= us((256u - o.reg[3]) * 320u)) {
        o.status |= 0xA0;
    }
    return (uint8_t)(o.status | 0x06);  /* (the YM3812's low bits: not an OPL3) */
}

static void opl_write(uint8_t v)
{
    uint8_t r = o.index;
    if (r == 4) {
        if (v & 0x80) {
            o.status = 0;           /* the IRQ reset: the flags clear, nothing else */
            return;
        }
        if ((v & 1) && !o.t1_on) {
            o.t1_start = pc_now();
        }
        if ((v & 2) && !o.t2_on) {
            o.t2_start = pc_now();
        }
        o.t1_on = v & 1;
        o.t2_on = v & 2;
    }
    o.reg[r] = v;
    queue(r, v);
}

/* ---- The OPL2: the speaker's task's side ---- */

static OPL *s_opl;
static int32_t s_opl_prev, s_opl_cur;
static uint32_t s_opl_frac;         /* 16.16: where between them */
static uint32_t s_quiet;            /* samples in a row with nothing: then it rests */
static bool s_opl_on;

void sound_mix(int16_t *dst, int n, uint32_t first)
{
    if (__atomic_load_n(&s_opl, __ATOMIC_ACQUIRE) == NULL) {
        return;
    }
    static const uint32_t step = (uint32_t)(((uint64_t)OPL_RATE << 16) / PC_AUDIO_HZ);
    for (int i = 0; i < n; i++) {
        uint32_t at = first + (uint32_t)i;
        uint32_t head = __atomic_load_n(&s_head, __ATOMIC_ACQUIRE);
        if (s_resync) {
            s_resync = false;
            s_tail = head;
            for (int r = 1; r < 0xF6; r++) {
                OPL_writeReg(s_opl, (uint32_t)r, o.reg[r]);
            }
            s_opl_on = true;
        }
        while (s_tail != head && (int32_t)(s_queue[s_tail % QUEUE].stamp - at) <= 0) {
            uint16_t reg = s_queue[s_tail % QUEUE].reg;
            if (reg == 0x100) {
                OPL_reset(s_opl);
            } else {
                OPL_writeReg(s_opl, reg, s_queue[s_tail % QUEUE].value);
            }
            __atomic_store_n(&s_tail, s_tail + 1, __ATOMIC_RELEASE);
            s_opl_on = true;
            s_quiet = 0;
        }
        if (!s_opl_on) {
            continue;
        }
        s_opl_frac += step;
        while (s_opl_frac >= 0x10000) {
            s_opl_frac -= 0x10000;
            s_opl_prev = s_opl_cur;
            s_opl_cur = OPL_calc(s_opl);
        }
        int32_t v = s_opl_prev + (int32_t)(((int64_t)(s_opl_cur - s_opl_prev) * s_opl_frac) >> 16);
        if (v == 0 && s_opl_cur == 0) {
            if (++s_quiet > (uint32_t)PC_AUDIO_HZ) {
                s_opl_on = false;   /* a second of silence: it rests till the next write */
            }
            continue;
        }
        s_quiet = 0;
        v *= OPL_GAIN;
        for (int c = 0; c < 2; c++) {
            int32_t m = dst[2 * i + c] + v;
            dst[2 * i + c] = (int16_t)(m < -32768 ? -32768 : m > 32767 ? 32767 : m);
        }
    }
}

/* ---- The Sound Blaster's DSP ---- */

enum { DSP_IDLE, DSP_SINGLE, DSP_AUTO };

static struct {
    uint8_t cmd, need, got;         /* the command being taken, its parameter bytes */
    uint8_t param[3];
    uint8_t out[16];                /* its answers (22Ah) */
    int out_head, out_count;
    bool reset;                     /* 226h's bit 0 */
    int mode;                       /* playing: DSP_SINGLE or DSP_AUTO */
    bool paused, speaker, exit_auto, silence, record;
    uint8_t per_byte;               /* samples a byte (ADPCM: 2-4, their sound left out) */
    uint32_t rate;                  /* samples a second */
    uint32_t phase;                 /* x PC_AUDIO_HZ: towards the next byte */
    uint32_t block;                 /* the auto-init and high-speed length (48h), bytes */
    uint32_t left;                  /* this block's bytes */
    uint8_t level;                  /* the last byte out */
    uint8_t test;
    bool irq;
} d;

static void dsp_answer(uint8_t v)
{
    if (d.out_count < (int)sizeof(d.out)) {
        d.out[(d.out_head + d.out_count++) % (int)sizeof(d.out)] = v;
    }
}

static void dsp_irq(void)
{
    d.irq = true;
    pic_irq(SB_IRQ, true);
}

/* The current block's end, for the event */
static void wake_for_block(void)
{
    pc_schedule(sound_next_event());
}

static void dsp_start(int mode, uint32_t bytes, bool silence, uint8_t per_byte, bool record)
{
    speaker_run();                  /* (what came before at the old settings) */
    d.mode = mode;
    d.left = bytes;
    d.silence = silence;
    d.per_byte = per_byte;
    d.record = record;
    d.paused = false;
    d.exit_auto = false;
    d.phase = 0;
    wake_for_block();
}

/* A byte's time: from DMA (or none: silence), the block's end */
static void dsp_byte(void)
{
    if (d.record) {
        uint8_t b = 0x80;
        bool tc;
        dma_transfer(SB_DMA, &b, 1, true, &tc);
    } else if (!d.silence) {
        uint8_t b;
        bool tc;
        if (dma_transfer(SB_DMA, &b, 1, false, &tc) == 1) {
            d.level = d.per_byte == 1 ? b : 0x80;
        }
    }
    if (--d.left == 0) {
        dsp_irq();
        if (d.mode == DSP_AUTO && !d.exit_auto) {
            d.left = d.block;
            wake_for_block();
        } else {
            d.mode = DSP_IDLE;
            d.level = 0x80;
        }
    }
}

int32_t sound_sample(void)
{
    if (d.mode != DSP_IDLE && !d.paused && d.rate) {
        d.phase += d.rate / d.per_byte;
        while (d.phase >= PC_AUDIO_HZ && d.mode != DSP_IDLE) {
            d.phase -= PC_AUDIO_HZ;
            dsp_byte();
        }
    }
    return d.speaker ? ((int32_t)d.level - 0x80) * SB_LEVEL : 0;
}

static uint32_t length(void)
{
    return (uint32_t)(d.param[0] | d.param[1] << 8) + 1u;
}

static void dsp_command(void)
{
    switch (d.cmd) {
    case 0x10:                      /* direct DAC */
        speaker_run();
        d.level = d.param[0];
        break;
    case 0x14:                      /* 8-bit single-cycle DMA out */
        dsp_start(DSP_SINGLE, length(), false, 1, false);
        break;
    case 0x16: case 0x17:           /* 2-bit ADPCM (17h: with its reference byte) */
        dsp_start(DSP_SINGLE, length(), false, 4, false);
        break;
    case 0x74: case 0x75:           /* 4-bit ADPCM */
        dsp_start(DSP_SINGLE, length(), false, 2, false);
        break;
    case 0x76: case 0x77:           /* 2.6-bit ADPCM */
        dsp_start(DSP_SINGLE, length(), false, 3, false);
        break;
    case 0x1C:                      /* 8-bit auto-init DMA out */
    case 0x90:                      /* (high speed) */
        dsp_start(DSP_AUTO, d.block, false, 1, false);
        break;
    case 0x91:                      /* high speed single-cycle */
        dsp_start(DSP_SINGLE, d.block, false, 1, false);
        break;
    case 0x1F: case 0x7D: case 0x7F:        /* ADPCM auto-init */
        dsp_start(DSP_AUTO, d.block, false, d.cmd == 0x7D ? 2 : d.cmd == 0x7F ? 3 : 4, false);
        break;
    case 0x20:                      /* direct ADC: silence */
        dsp_answer(0x80);
        break;
    case 0x24:                      /* 8-bit single-cycle DMA in: silence */
        dsp_start(DSP_SINGLE, length(), false, 1, true);
        break;
    case 0x2C: case 0x98: case 0x99:        /* auto-init (high speed) DMA in */
        dsp_start(d.cmd == 0x99 ? DSP_SINGLE : DSP_AUTO, d.block, false, 1, true);
        break;
    case 0x40:                      /* the time constant */
        speaker_run();
        d.rate = 1000000u / (256u - d.param[0]);
        break;
    case 0x41: case 0x42:           /* (the SB16's rate: taken as it is) */
        speaker_run();
        d.rate = (uint32_t)(d.param[0] << 8 | d.param[1]);
        break;
    case 0x48:                      /* the block length */
        d.block = length();
        break;
    case 0x80:                      /* silence, then the interrupt */
        dsp_start(DSP_SINGLE, length(), true, 1, false);
        break;
    case 0xD0:                      /* pause */
        speaker_run();
        d.paused = true;
        break;
    case 0xD4:                      /* continue */
        speaker_run();
        d.paused = false;
        wake_for_block();
        break;
    case 0xD1:                      /* the speaker on, off */
    case 0xD3:
        speaker_run();
        d.speaker = d.cmd == 0xD1;
        break;
    case 0xD8:
        dsp_answer(d.speaker ? 0xFF : 0x00);
        break;
    case 0xDA:                      /* auto-init: this block is the last */
        d.exit_auto = true;
        break;
    case 0xE0:                      /* identification: the byte inverted */
        dsp_answer((uint8_t)~d.param[0]);
        break;
    case 0xE1:                      /* the version: 2.01 */
        dsp_answer(0x02);
        dsp_answer(0x01);
        break;
    case 0xE4:
        d.test = d.param[0];
        break;
    case 0xE8:
        dsp_answer(d.test);
        break;
    case 0xF2:                      /* the interrupt, now */
        dsp_irq();
        break;
    case 0xF8:
        dsp_answer(0x00);
        break;
    default:                        /* MIDI and the rest: taken, nothing done */
        break;
    }
}

static uint8_t params(uint8_t cmd)
{
    switch (cmd) {
    case 0x10: case 0x38: case 0x40: case 0xE0: case 0xE2: case 0xE4:
        return 1;
    case 0x14: case 0x16: case 0x17: case 0x24: case 0x41: case 0x42: case 0x48:
    case 0x74: case 0x75: case 0x76: case 0x77: case 0x80:
        return 2;
    default:
        return 0;
    }
}

static void dsp_write(uint8_t v)
{
    if (d.need) {
        d.param[d.got++] = v;
        if (d.got == d.need) {
            d.need = 0;
            dsp_command();
        }
        return;
    }
    d.cmd = v;
    d.got = 0;
    d.need = params(v);
    if (d.need == 0) {
        dsp_command();
    }
}

static void dsp_reset(void)
{
    speaker_run();
    uint32_t rate = d.rate ? d.rate : 11025;
    memset(&d, 0, sizeof(d));
    d.rate = rate;
    d.level = 0x80;
    d.block = 0x800;
    d.per_byte = 1;
    pic_irq(SB_IRQ, false);
}

uint64_t sound_next_event(void)
{
    if (d.mode == DSP_IDLE || d.paused || d.rate == 0) {
        return NEVER;
    }
    /* the output samples till the block's last byte */
    uint64_t need = (uint64_t)d.left * PC_AUDIO_HZ;
    uint32_t per = d.rate / d.per_byte;
    uint64_t samples = need > d.phase ? (need - d.phase + per - 1) / per : 1;
    return speaker_clock_after((uint32_t)(samples < 0x7FFFFFFF ? samples : 0x7FFFFFFF));
}

void sound_event(uint64_t now)
{
    if (now >= sound_next_event()) {
        speaker_run();
    }
}

/* ---- Ports ---- */

bool sound_in(uint16_t port, uint8_t *v)
{
    if (port == 0x388 || port == SB_BASE + 8) {
        *v = opl_status();
        return true;
    }
    if (port == 0x389 || port == SB_BASE + 9) {
        *v = 0xFF;
        return true;
    }
    if (port < SB_BASE || port > SB_BASE + 0x0F) {
        return false;
    }
    switch (port - SB_BASE) {
    case 0x0A:                      /* read data */
        if (d.out_count) {
            *v = d.out[d.out_head];
            d.out_head = (d.out_head + 1) % (int)sizeof(d.out);
            d.out_count--;
        } else {
            *v = 0xFF;
        }
        break;
    case 0x0C:                      /* write status: ready */
        *v = 0x7F;
        break;
    case 0x0E:                      /* read status (data there?), and the interrupt's acknowledge */
        *v = (uint8_t)(d.out_count ? 0xFF : 0x7F);
        if (d.irq) {
            d.irq = false;
            pic_irq(SB_IRQ, false);
        }
        break;
    default:
        *v = 0xFF;
        break;
    }
    return true;
}

bool sound_out(uint16_t port, uint8_t v)
{
    if (port == 0x388 || port == SB_BASE + 8) {
        o.index = v;
        return true;
    }
    if (port == 0x389 || port == SB_BASE + 9) {
        opl_write(v);
        return true;
    }
    if (port < SB_BASE || port > SB_BASE + 0x0F) {
        return false;
    }
    switch (port - SB_BASE) {
    case 0x06:                      /* reset: 1 then 0, then AAh to read */
        if ((v & 1) && !d.reset) {
            d.reset = true;
        } else if (!(v & 1) && d.reset) {
            dsp_reset();
            dsp_answer(0xAA);
        }
        break;
    case 0x0C:
        dsp_write(v);
        break;
    default:
        break;
    }
    return true;
}

void sound_reset(void)
{
    if (s_opl == NULL) {
        OPL *opl = OPL_new(OPL_CLOCK, OPL_RATE);    /* (its own rate: no converter) */
        if (opl) {
            OPL_setChipType(opl, 2);                /* the YM3812 */
            OPL_reset(opl);
            __atomic_store_n(&s_opl, opl, __ATOMIC_RELEASE);
        }
    }
    memset(&o, 0, sizeof(o));
    queue(0x100, 0);                /* the chip's reset, in turn */
    d.rate = 0;
    dsp_reset();
}
