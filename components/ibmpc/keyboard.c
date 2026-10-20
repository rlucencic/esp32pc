/*
 * The keyboard itself: keys from other tasks, the bytes it sends (scan
 * code set 1), typematic repeat, typing text
 *
 * The XT's 83-key keyboard has no extended keys: theirs (the arrows,
 * Insert, Delete, Home, End, Page Up/Down, the right Ctrl and Alt, keypad
 * Enter and /) send the keypad's and main block's codes without the E0.
 */
#include "pc_internal.h"
#include "keyboard.h"

#define RING        64
#define OUT_MAX     32

/* Keys from any task: a slot each, bit 31 set when it's filled */
static uint32_t s_ring[RING];
static uint32_t s_write, s_read;

/* The bytes waiting to be sent */
static uint8_t s_out[OUT_MAX];
static int s_out_head, s_out_count;

/* Typematic: the last key pressed, while it's held */
static uint16_t s_repeat;
static uint32_t s_repeat_frames;

/* Text being typed */
static char s_text[256];
static volatile int s_text_len;
static int s_text_pos;

void pc_key(uint16_t code, bool down)
{
    uint32_t i = __atomic_fetch_add(&s_write, 1, __ATOMIC_ACQ_REL);
    __atomic_store_n(&s_ring[i % RING], 0x80000000u | (down ? 0x10000u : 0) | code, __ATOMIC_RELEASE);
}

void pc_type(const char *text)
{
    if (s_text_len) {
        return;                     /* still typing the last */
    }
    size_t n = strlen(text);
    if (n >= sizeof(s_text)) {
        n = sizeof(s_text) - 1;
    }
    memcpy(s_text, text, n);
    s_text_pos = 0;
    __atomic_store_n(&s_text_len, (int)n, __ATOMIC_RELEASE);
}

static void out(uint8_t b)
{
    if (s_out_count < OUT_MAX) {
        s_out[(s_out_head + s_out_count++) % OUT_MAX] = b;
    }
}

bool keyboard_pending(void)
{
    return s_out_count > 0;
}

uint8_t keyboard_take(void)
{
    if (!s_out_count) {
        return 0;
    }
    uint8_t b = s_out[s_out_head];
    s_out_head = (s_out_head + 1) % OUT_MAX;
    s_out_count--;
    return b;
}

void keyboard_reset(void)
{
    s_out_head = s_out_count = 0;
    s_repeat = 0;
    out(0xAA);                      /* its self-test passed */
}

static void send(uint16_t code, bool down)
{
    bool xt = pc.cfg.board == PC_XT;
    if ((code >> 8) == 0xE0 && !xt) {
        out(0xE0);
    }
    out((uint8_t)((code & 0x7F) | (down ? 0 : 0x80)));
}

static void key(uint16_t code, bool down)
{
    send(code, down);
    if (down) {
        s_repeat = code;
        s_repeat_frames = 0;
    } else if (code == s_repeat) {
        s_repeat = 0;
    }
}

/* US keyboard: the key for each printable character (bit 8: with Shift) */
static uint16_t ascii_key(char ch)
{
    static const char normal[] = "\x1b" "1234567890-=\b\tqwertyuiop[]\n\0asdfghjkl;'`\0\\zxcvbnm,./";
    static const char shifted[] = "\x1b" "!@#$%^&*()_+\b\tQWERTYUIOP{}\n\0ASDFGHJKL:\"~\0|ZXCVBNM<>?";
    if (ch == ' ') {
        return 0x39;
    }
    for (int i = 0; i < (int)sizeof(normal) - 1; i++) {
        if (normal[i] && normal[i] == ch) {
            return (uint16_t)(i + 1);
        }
        if (shifted[i] && shifted[i] == ch) {
            return (uint16_t)(0x100 | (i + 1));
        }
    }
    return 0;
}

void keyboard_frame(void)
{
    /* Keys from the other tasks */
    for (;;) {
        uint32_t slot = __atomic_load_n(&s_ring[s_read % RING], __ATOMIC_ACQUIRE);
        if (!(slot & 0x80000000u)) {
            break;
        }
        __atomic_store_n(&s_ring[s_read % RING], 0, __ATOMIC_RELAXED);
        s_read++;
        key((uint16_t)slot, (slot & 0x10000u) != 0);
    }

    /* Typematic: after half a second, about 11 a second */
    int hz = pc_screen_hz();
    if (s_repeat && ++s_repeat_frames >= (uint32_t)hz / 2 && s_out_count == 0) {
        send(s_repeat, true);
        s_repeat_frames = (uint32_t)hz / 2 - (uint32_t)(hz * 2 / 23);
    }

    /* Text, a character a frame while nothing else waits */
    int len = __atomic_load_n(&s_text_len, __ATOMIC_ACQUIRE);
    if (len && s_out_count == 0) {
        uint16_t k = ascii_key(s_text[s_text_pos]);
        if (k & 0x100) {
            send(0x2A, true);
        }
        if (k & 0xFF) {
            send(k & 0xFF, true);
            send(k & 0xFF, false);
        }
        if (k & 0x100) {
            send(0x2A, false);
        }
        if (++s_text_pos >= len) {
            __atomic_store_n(&s_text_len, 0, __ATOMIC_RELEASE);
        }
    }
}
