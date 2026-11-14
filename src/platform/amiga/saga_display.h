#ifndef __PLATFORM_AMIGA_SAGA_DISPLAY_H
#define __PLATFORM_AMIGA_SAGA_DISPLAY_H

#include <inttypes.h>

/* SAGA chunky plane, R5G6B5 (KB core/saga-registers/chunky-plane.md):
   640x480 at 60 Hz (GFXMODE $0502, the default) or 320x240 at 60 Hz ($0202,
   argument LOWRES). Both use the 28 MHz pixel clock; $02 shows each pixel
   2x2, so the monitor gets the same signal. */
#define MAX_SCREEN_WIDTH  640
#define MAX_SCREEN_HEIGHT 480
extern int screen_width, screen_height;
#define SCREEN_WIDTH  screen_width
#define SCREEN_HEIGHT screen_height

void saga_display_select(int high_resolution);  /* before saga_display_open and scene drawing */

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3)))

int saga_display_open(void);        /* allocate buffers; OS still running */
void saga_display_show(void);       /* save the current mode, switch to ours */
uint16_t *saga_display_back_buffer(void);   /* neither on screen nor waiting */
void saga_display_present(uint16_t *buffer); /* on screen at the next VBlank (irq_servers.s) */
void saga_display_restore(void);    /* put back the saved mode, pointer and modulo;
                                       after interrupts_remove() */
void saga_display_close(void);      /* free buffers */

#endif
