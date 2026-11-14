/*
 * SAGA chunky display for ppengine (port plan T09).
 *
 * Registers (KB core/saga-registers/chunky-plane.md, pointer.md):
 *   GFXMODE $DFF1F4 (W), chunky pointer $DFF1EC (L), modulo $DFF1E6 (W);
 *   read back at $DFE1F4/$DFE1EC/$DFE1E6.
 *   Pointer position $DFF1D0/$DFF1D2 (W), read back at $DFE1D0/$DFE1D2.
 *
 * Three buffers. A finished frame is handed to the VBlank interrupt server
 * (irq_servers.s), which puts it on screen at the start of the next VBlank.
 * The next frame is drawn into the buffer that is neither on screen nor
 * waiting. (With two buffers and a flip from the frame loop, frames that took
 * longer than a VBlank flipped mid-frame and were drawn into the buffer on
 * screen: flicker, test on the V4 2026-11-14.)
 */

#include <exec/memory.h>
#include <proto/exec.h>
#include "platform/amiga/saga_display.h"
#include "platform/amiga/interrupts.h"

#define GFXMODE_W    ((volatile uint16_t *)0xDFF1F4)
#define GFXMODE_R    ((volatile uint16_t *)0xDFE1F4)
#define CHUNKY_PTR_W ((volatile uint32_t *)0xDFF1EC)
#define CHUNKY_PTR_R ((volatile uint32_t *)0xDFE1EC)
#define MODULO_W     ((volatile uint16_t *)0xDFF1E6)
#define MODULO_R     ((volatile uint16_t *)0xDFE1E6)
#define POINTER_X_W  ((volatile uint16_t *)0xDFF1D0)
#define POINTER_X_R  ((volatile uint16_t *)0xDFE1D0)
#define POINTER_Y_W  ((volatile uint16_t *)0xDFF1D2)
#define POINTER_Y_R  ((volatile uint16_t *)0xDFE1D2)

#define GFXMODE_320X240_R5G6B5 0x0202
#define GFXMODE_640X480_R5G6B5 0x0502
#define BUFFER_COUNT 3
#define BUFFER_BYTES (MAX_SCREEN_WIDTH * MAX_SCREEN_HEIGHT * 2)
#define ALIGNMENT 32

int screen_width = 640;
int screen_height = 480;
static UWORD gfxmode = GFXMODE_640X480_R5G6B5;

void saga_display_select(int high_resolution) {
	if (high_resolution) {
		screen_width = 640;
		screen_height = 480;
		gfxmode = GFXMODE_640X480_R5G6B5;
	} else {
		screen_width = 320;
		screen_height = 240;
		gfxmode = GFXMODE_320X240_R5G6B5;
	}
}

static void *allocations[BUFFER_COUNT];
static uint16_t *buffers[BUFFER_COUNT];

static uint16_t saved_gfxmode;
static uint32_t saved_pointer;
static uint16_t saved_modulo;
static uint16_t saved_pointer_x;
static uint16_t saved_pointer_y;
static int shown;

int saga_display_open(void) {
	int i;

	for (i = 0; i < BUFFER_COUNT; i++) {
		allocations[i] = AllocMem(BUFFER_BYTES + ALIGNMENT, MEMF_ANY | MEMF_CLEAR);
		if (!allocations[i]) {
			saga_display_close();
			return 0;
		}
		buffers[i] = (uint16_t *)(((uint32_t)allocations[i] + ALIGNMENT - 1) & ~(uint32_t)(ALIGNMENT - 1));
	}
	return 1;
}

void saga_display_show(void) {
	saved_gfxmode = *GFXMODE_R;
	saved_pointer = *CHUNKY_PTR_R;
	saved_modulo = *MODULO_R;
	saved_pointer_x = *POINTER_X_R;
	saved_pointer_y = *POINTER_Y_R;
	shown = 1;

	*GFXMODE_W = gfxmode;
	*MODULO_W = 0;
	irq_state.pending = 0;
	irq_state.displayed = (ULONG)buffers[0];
	*CHUNKY_PTR_W = (uint32_t)buffers[0];
	/* Park the mouse pointer outside the visible area. */
	*POINTER_X_W = 0x7ff;
	*POINTER_Y_W = 0x7ff;
}

uint16_t *saga_display_back_buffer(void) {
	/* Read pending first: the VBlank server only ever moves pending into
	   displayed, so whatever it does between the two reads, the buffer that
	   is on screen afterwards is one of the two excluded below. */
	ULONG pending = irq_state.pending;
	ULONG displayed = irq_state.displayed;
	int i;

	for (i = 0; i < BUFFER_COUNT; i++) {
		if ((ULONG)buffers[i] != displayed && (ULONG)buffers[i] != pending) {
			return buffers[i];
		}
	}
	return buffers[0];  /* not reached with three buffers */
}

void saga_display_present(uint16_t *buffer) {
	irq_state.pending = (ULONG)buffer;
}

void saga_display_restore(void) {
	if (!shown) {
		return;
	}
	*GFXMODE_W = saved_gfxmode;
	*MODULO_W = saved_modulo;
	*CHUNKY_PTR_W = saved_pointer;
	*POINTER_X_W = saved_pointer_x;
	*POINTER_Y_W = saved_pointer_y;
	shown = 0;
}

void saga_display_close(void) {
	int i;

	for (i = 0; i < BUFFER_COUNT; i++) {
		if (allocations[i]) {
			FreeMem(allocations[i], BUFFER_BYTES + ALIGNMENT);
			allocations[i] = NULL;
			buffers[i] = NULL;
		}
	}
}
