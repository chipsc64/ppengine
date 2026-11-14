#ifndef __PLATFORM_AMIGA_KEYBOARD_H
#define __PLATFORM_AMIGA_KEYBOARD_H

#include <inttypes.h>

/* Amiga raw key codes (standard Amiga keyboard layout). */
#define KEY_1      0x01
#define KEY_SPACE  0x40
#define KEY_ESCAPE 0x45
#define KEY_UP     0x4C
#define KEY_DOWN   0x4D
#define KEY_RIGHT  0x4E
#define KEY_LEFT   0x4F

void keyboard_open(void);            /* after interrupts_install() */
void keyboard_poll(void);            /* decode the bytes the interrupt servers queued */
int keyboard_held(uint8_t key);
int keyboard_take_press(uint8_t key);  /* 1 once per press since the last call */

#endif
