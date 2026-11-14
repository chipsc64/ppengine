/*
 * Keyboard state for ppengine (port plan T10).
 *
 * ApolloKeyboard() only reports the newest key, but the game needs several
 * keys held at once (steer + accelerate). The interrupt servers in
 * irq_servers.s read the CIA-A serial data register ($BFEC01, as ApolloLib:
 * KB toolchain/apollolib/input.md) whenever CIA-A signals a keyboard byte and
 * at every VBlank, and queue each new value. keyboard_poll() decodes the
 * queue (not, ror #1; bit 7 of the code is the key-up flag) into a matrix of
 * held keys and counts presses.
 *
 * Earlier the frame loop polled the register itself while waiting for the
 * VBlank; frames that took longer than a VBlank then skipped the wait and
 * the keyboard was hardly read (test on the V4, 2026-11-14).
 */

#include "platform/amiga/keyboard.h"
#include "platform/amiga/interrupts.h"

static UBYTE read_index;
static uint8_t held[128];
static uint8_t presses[128];

static void clear_all(void) {
	int i;

	for (i = 0; i < 128; i++) {
		held[i] = 0;
		presses[i] = 0;
	}
}

void keyboard_open(void) {
	clear_all();
	read_index = irq_state.write;
}

static void decode(uint8_t raw) {
	uint8_t code;
	uint8_t key;

	raw = (uint8_t)~raw;
	code = (uint8_t)((raw >> 1) | (raw << 7));
	key = code & 0x7f;
	if (key >= 0x78) {
		/* 0x78 reset warning, 0x79-0x7f keyboard status codes
		   (lost sync, buffer overflow, ...): forget what we think is held */
		clear_all();
		return;
	}
	if (code & 0x80) {
		held[key] = 0;
	} else {
		if (!held[key] && presses[key] < 255) {
			presses[key]++;
		}
		held[key] = 1;
	}
}

void keyboard_poll(void) {
	while (read_index != irq_state.write) {
		decode(irq_state.ring[read_index]);
		read_index = (UBYTE)((read_index + 1) & 15);
	}
}

int keyboard_held(uint8_t key) {
	return held[key & 0x7f];
}

int keyboard_take_press(uint8_t key) {
	key &= 0x7f;
	if (presses[key]) {
		presses[key]--;
		return 1;
	}
	return 0;
}
