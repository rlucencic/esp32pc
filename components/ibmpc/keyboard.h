/*
 * The keyboard's side, for its controller (the XT's 8255, the AT's 8042)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* A byte waiting to be sent, and taking it */
bool keyboard_pending(void);
uint8_t keyboard_take(void);
/* A reset (the clock held low): forgets what waits, sends AA */
void keyboard_reset(void);
/* Once a frame: the keys from the other tasks, repeat, typing */
void keyboard_frame(void);
