/*
 * The BIOS's keyboard: INT 09h (a scan code from port 60h) and INT 16h
 *
 * The shift states (40:17, 40:18), the US key table (scan code and ASCII,
 * with Shift, Ctrl, Alt; Num Lock and Caps Lock), Alt and the keypad's
 * digits for a character code, Ctrl-Break (INT 1Bh), Shift-PrtSc (INT 5),
 * Ctrl-Alt-Del (a warm restart). Keys go to the buffer at 40:1E (its
 * start and end at 40:80/82). INT 16h reads it, waiting with HLT.
 */
#include "pc_internal.h"

/* Each key's codes: normal, Shift, Ctrl, Alt (scan << 8 | ASCII; 0: none) */
typedef struct {
    uint16_t normal, shift, ctrl, alt;
} keydef_t;

#define LETTER(sc, ch)  { (sc) << 8 | (ch), (sc) << 8 | ((ch) - 32), (sc) << 8 | ((ch) - 96), (sc) << 8 }

static const keydef_t s_keys[0x59] = {
    [0x01] = { 0x011B, 0x011B, 0x011B, 0x0100 },
    [0x02] = { 0x0231, 0x0221, 0x0000, 0x7800 },     /* 1 ! */
    [0x03] = { 0x0332, 0x0340, 0x0300, 0x7900 },     /* 2 @ */
    [0x04] = { 0x0433, 0x0423, 0x0000, 0x7A00 },
    [0x05] = { 0x0534, 0x0524, 0x0000, 0x7B00 },
    [0x06] = { 0x0635, 0x0625, 0x0000, 0x7C00 },
    [0x07] = { 0x0736, 0x075E, 0x071E, 0x7D00 },     /* 6 ^ */
    [0x08] = { 0x0837, 0x0826, 0x0000, 0x7E00 },
    [0x09] = { 0x0938, 0x092A, 0x0000, 0x7F00 },
    [0x0A] = { 0x0A39, 0x0A28, 0x0000, 0x8000 },
    [0x0B] = { 0x0B30, 0x0B29, 0x0000, 0x8100 },
    [0x0C] = { 0x0C2D, 0x0C5F, 0x0C1F, 0x8200 },     /* - _ */
    [0x0D] = { 0x0D3D, 0x0D2B, 0x0000, 0x8300 },     /* = + */
    [0x0E] = { 0x0E08, 0x0E08, 0x0E7F, 0x0E00 },     /* Backspace */
    [0x0F] = { 0x0F09, 0x0F00, 0x9400, 0xA500 },     /* Tab */
    [0x10] = LETTER(0x10, 'q'), [0x11] = LETTER(0x11, 'w'), [0x12] = LETTER(0x12, 'e'),
    [0x13] = LETTER(0x13, 'r'), [0x14] = LETTER(0x14, 't'), [0x15] = LETTER(0x15, 'y'),
    [0x16] = LETTER(0x16, 'u'), [0x17] = LETTER(0x17, 'i'), [0x18] = LETTER(0x18, 'o'),
    [0x19] = LETTER(0x19, 'p'),
    [0x1A] = { 0x1A5B, 0x1A7B, 0x1A1B, 0x1A00 },     /* [ { */
    [0x1B] = { 0x1B5D, 0x1B7D, 0x1B1D, 0x1B00 },     /* ] } */
    [0x1C] = { 0x1C0D, 0x1C0D, 0x1C0A, 0x1C00 },     /* Enter */
    [0x1E] = LETTER(0x1E, 'a'), [0x1F] = LETTER(0x1F, 's'), [0x20] = LETTER(0x20, 'd'),
    [0x21] = LETTER(0x21, 'f'), [0x22] = LETTER(0x22, 'g'), [0x23] = LETTER(0x23, 'h'),
    [0x24] = LETTER(0x24, 'j'), [0x25] = LETTER(0x25, 'k'), [0x26] = LETTER(0x26, 'l'),
    [0x27] = { 0x273B, 0x273A, 0x0000, 0x2700 },     /* ; : */
    [0x28] = { 0x2827, 0x2822, 0x0000, 0x2800 },     /* ' " */
    [0x29] = { 0x2960, 0x297E, 0x0000, 0x2900 },     /* ` ~ */
    [0x2B] = { 0x2B5C, 0x2B7C, 0x2B1C, 0x2B00 },     /* \ | */
    [0x2C] = LETTER(0x2C, 'z'), [0x2D] = LETTER(0x2D, 'x'), [0x2E] = LETTER(0x2E, 'c'),
    [0x2F] = LETTER(0x2F, 'v'), [0x30] = LETTER(0x30, 'b'), [0x31] = LETTER(0x31, 'n'),
    [0x32] = LETTER(0x32, 'm'),
    [0x33] = { 0x332C, 0x333C, 0x0000, 0x3300 },     /* , < */
    [0x34] = { 0x342E, 0x343E, 0x0000, 0x3400 },     /* . > */
    [0x35] = { 0x352F, 0x353F, 0x0000, 0x3500 },     /* / ? */
    [0x37] = { 0x372A, 0x372A, 0x7200, 0x3700 },     /* keypad * (PrtSc) */
    [0x39] = { 0x3920, 0x3920, 0x3920, 0x3920 },     /* Space */
    [0x3B] = { 0x3B00, 0x5400, 0x5E00, 0x6800 },     /* F1 */
    [0x3C] = { 0x3C00, 0x5500, 0x5F00, 0x6900 },
    [0x3D] = { 0x3D00, 0x5600, 0x6000, 0x6A00 },
    [0x3E] = { 0x3E00, 0x5700, 0x6100, 0x6B00 },
    [0x3F] = { 0x3F00, 0x5800, 0x6200, 0x6C00 },
    [0x40] = { 0x4000, 0x5900, 0x6300, 0x6D00 },
    [0x41] = { 0x4100, 0x5A00, 0x6400, 0x6E00 },
    [0x42] = { 0x4200, 0x5B00, 0x6500, 0x6F00 },
    [0x43] = { 0x4300, 0x5C00, 0x6600, 0x7000 },
    [0x44] = { 0x4400, 0x5D00, 0x6700, 0x7100 },     /* F10 */
    /* The keypad: the cursor keys (Shift / Num Lock: the digits, below) */
    [0x47] = { 0x4700, 0x4737, 0x7700, 0x0000 },     /* Home 7 */
    [0x48] = { 0x4800, 0x4838, 0x8D00, 0x0000 },     /* Up 8 */
    [0x49] = { 0x4900, 0x4939, 0x8400, 0x0000 },     /* PgUp 9 */
    [0x4A] = { 0x4A2D, 0x4A2D, 0x8E00, 0x4A00 },     /* - */
    [0x4B] = { 0x4B00, 0x4B34, 0x7300, 0x0000 },     /* Left 4 */
    [0x4C] = { 0x4C00, 0x4C35, 0x8F00, 0x0000 },     /* 5 */
    [0x4D] = { 0x4D00, 0x4D36, 0x7400, 0x0000 },     /* Right 6 */
    [0x4E] = { 0x4E2B, 0x4E2B, 0x9000, 0x4E00 },     /* + */
    [0x4F] = { 0x4F00, 0x4F31, 0x7500, 0x0000 },     /* End 1 */
    [0x50] = { 0x5000, 0x5032, 0x9100, 0x0000 },     /* Down 2 */
    [0x51] = { 0x5100, 0x5133, 0x7600, 0x0000 },     /* PgDn 3 */
    [0x52] = { 0x5200, 0x5230, 0x9200, 0x0000 },     /* Ins 0 */
    [0x53] = { 0x5300, 0x532E, 0x9300, 0x0000 },     /* Del . */
    [0x57] = { 0x8500, 0x8700, 0x8900, 0x8B00 },     /* F11 */
    [0x58] = { 0x8600, 0x8800, 0x8A00, 0x8C00 },     /* F12 */
};

/* 40:17 and 40:18 */
enum { RSHIFT = 0x01, LSHIFT = 0x02, CTRL = 0x04, ALT = 0x08, SCROLL = 0x10, NUM = 0x20, CAPS = 0x40, INS = 0x80 };

void bios_keyboard_init(void)
{
    *bda(0x17) = 0;
    *bda(0x18) = 0;
    *bda(0x19) = 0;
}

static bool buffer_put(uint16_t key)
{
    uint16_t tail = bda16(0x1C), next = (uint16_t)(tail + 2);
    if (next >= bda16(0x82)) {
        next = bda16(0x80);
    }
    if (next == bda16(0x1A)) {
        return false;               /* full */
    }
    set_bda16(tail, key);
    set_bda16(0x1C, next);
    return true;
}

static bool buffer_get(uint16_t *key, bool remove)
{
    uint16_t head = bda16(0x1A);
    if (head == bda16(0x1C)) {
        return false;
    }
    *key = bda16(head);
    if (remove) {
        head = (uint16_t)(head + 2);
        if (head >= bda16(0x82)) {
            head = bda16(0x80);
        }
        set_bda16(0x1A, head);
    }
    return true;
}

void bios_int09(x86_t *c)
{
    uint8_t code = R_AL(c);
    uint8_t *f1 = bda(0x17), *f2 = bda(0x18);
    bool up = code & 0x80;
    uint8_t sc = code & 0x7F;

    if (code == 0xE0 || code == 0xE1) {
        *bda(0x96) |= 0x02;         /* the next code is an extended key's */
        return;
    }
    bool extended = *bda(0x96) & 0x02;
    *bda(0x96) &= (uint8_t)~0x02;

    /* The shift keys */
    switch (sc) {
    case 0x2A: if (!extended) { *f1 = up ? *f1 & ~LSHIFT : *f1 | LSHIFT; } return;
    case 0x36: *f1 = up ? (uint8_t)(*f1 & ~RSHIFT) : (uint8_t)(*f1 | RSHIFT); return;
    case 0x1D: *f1 = up ? (uint8_t)(*f1 & ~CTRL) : (uint8_t)(*f1 | CTRL); return;
    case 0x38:
        if (up) {
            *f1 &= (uint8_t)~ALT;
            if (*bda(0x19)) {       /* Alt and keypad digits: that character */
                buffer_put(*bda(0x19));
                *bda(0x19) = 0;
            }
        } else {
            *f1 |= ALT;
        }
        return;
    case 0x3A:                      /* Caps Lock */
    case 0x45:                      /* Num Lock */
    case 0x46:                      /* Scroll Lock (Ctrl: Break) */
    case 0x52: {                    /* Insert */
        uint8_t bit = sc == 0x3A ? CAPS : sc == 0x45 ? NUM : sc == 0x46 ? SCROLL : INS;
        if (sc == 0x46 && (*f1 & CTRL) && !up) {
            /* Ctrl-Break: the buffer emptied, a 0000 key, INT 1Bh */
            set_bda16(0x1A, bda16(0x80));
            set_bda16(0x1C, bda16(0x80));
            buffer_put(0x0000);
            *bda(0x71) |= 0x80;
            x86_interrupt(c, 0x1B);
            return;
        }
        if (sc == 0x52 && (*f1 & (NUM | LSHIFT | RSHIFT)) && !extended && !up) {
            break;                  /* Insert as the keypad's 0 */
        }
        if (up) {
            *f2 &= (uint8_t)~bit;
        } else if (!(*f2 & bit)) {
            *f2 |= bit;
            *f1 ^= bit;
            if (sc == 0x52) {
                buffer_put(0x5200);
            }
        }
        return;
    }
    default:
        break;
    }
    if (up || sc >= sizeof(s_keys) / sizeof(s_keys[0])) {
        return;
    }

    /* Ctrl-Alt-Del: a warm restart */
    if (sc == 0x53 && (*f1 & (CTRL | ALT)) == (CTRL | ALT)) {
        set_bda16(0x72, 0x1234);
        x86_load_seg(c, X86_CS, 0xF000);
        c->ip = 0xE05B;
        return;
    }
    /* Shift-PrtSc (the 101-key keyboard's own key: E0 37): INT 5 */
    if (sc == 0x37 && ((*f1 & (LSHIFT | RSHIFT)) || extended)) {
        x86_interrupt(c, 0x05);
        return;
    }

    const keydef_t *k = &s_keys[sc];
    bool keypad = sc >= 0x47 && sc <= 0x53 && sc != 0x4A && sc != 0x4E;
    uint16_t key;
    if (*f1 & ALT) {
        if (keypad && !extended && s_keys[sc].shift) {
            /* Alt and the keypad's digits */
            uint8_t digit = (uint8_t)(s_keys[sc].shift & 0xFF) - '0';
            if (digit <= 9) {
                *bda(0x19) = (uint8_t)(*bda(0x19) * 10 + digit);
            }
            return;
        }
        key = k->alt;
    } else if (*f1 & CTRL) {
        key = k->ctrl;
    } else if (keypad && !extended) {
        bool shift = (*f1 & (LSHIFT | RSHIFT)) != 0;
        key = (shift != ((*f1 & NUM) != 0)) ? k->shift : k->normal;
    } else if (*f1 & (LSHIFT | RSHIFT)) {
        key = k->shift;
        uint8_t ch = (uint8_t)key;
        if ((*f1 & CAPS) && ch >= 'A' && ch <= 'Z') {
            key = k->normal;
        }
    } else {
        key = k->normal;
        uint8_t ch = (uint8_t)key;
        if ((*f1 & CAPS) && ch >= 'a' && ch <= 'z') {
            key = k->shift;
        }
    }
    if (extended && (uint8_t)key == 0 && key) {
        key |= 0xE0;                /* the grey keys: ASCII E0 (INT 16h AH=10h) */
    }
    if (extended && sc == 0x35) {
        key = 0xE02F;               /* keypad / */
    }
    if (extended && sc == 0x1C) {
        key = (*f1 & CTRL) ? 0xE00A : 0xE00D;  /* keypad Enter */
    }
    if (key) {
        buffer_put(key);
    }
}

/* AH=00h/01h give the old keyboard's codes: the grey keys' E0 as 00, and
 * none of the keys it didn't have (F11, F12)
 */
static bool old_key(uint16_t *key)
{
    uint8_t sc = (uint8_t)(*key >> 8), ch = (uint8_t)*key;
    if (sc >= 0x85 && sc <= 0x8C) {
        return false;
    }
    if (ch == 0xE0 && sc) {
        *key &= 0xFF00;
    }
    if (*key == 0xE00D || *key == 0xE00A) {
        *key = (uint16_t)(0x1C00 | ch);
    }
    if (*key == 0xE02F) {
        *key = 0x352F;
    }
    return true;
}

void bios_int16(x86_t *c)
{
    uint8_t ah = R_AH(c);
    uint16_t key;
    switch (ah) {
    case 0x00:
    case 0x10:
        while (buffer_get(&key, true)) {
            if (ah == 0x10 || old_key(&key)) {
                c->r.w[X86_AX] = key;
                bios_wait_result(c, false);
                return;
            }
        }
        bios_wait_result(c, true);  /* none yet: the stub sleeps and asks again */
        return;
    case 0x01:
    case 0x11:
        while (buffer_get(&key, false)) {
            if (ah == 0x11 || old_key(&key)) {
                c->r.w[X86_AX] = key;
                bios_return_zf(c, false);
                bios_wait_result(c, false);
                return;
            }
            buffer_get(&key, true); /* one the old calls don't see */
        }
        bios_return_zf(c, true);
        bios_wait_result(c, false);
        return;
    case 0x02:
        R_AL(c) = *bda(0x17);
        break;
    case 0x12:
        R_AL(c) = *bda(0x17);
        R_AH(c) = (uint8_t)((*bda(0x18) & 0x73) | (*bda(0x17) & CTRL ? 0x01 : 0) | (*bda(0x17) & ALT ? 0x02 : 0));
        break;
    case 0x05:                      /* put a key in the buffer */
        R_AL(c) = buffer_put(c->r.w[X86_CX]) ? 0 : 1;
        break;
    default:
        break;
    }
    bios_wait_result(c, false);
}
