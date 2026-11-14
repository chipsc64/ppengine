/*
 * Controls to arcade inputs (port plan T10, T11). Keyboard, USB joypad and
 * DB9 joystick all work at the same time.
 *
 *              steer        accelerate  brake     gear        credit
 *   keyboard   left/right   up          down      space       1
 *   USB pad    d-pad l/r    A           B         X or Y      Start
 *   DB9 stick  left/right   up          down      fire        (key 1)
 *
 * Esc quits (keyboard only, so a pad button cannot end a game by accident).
 *
 * USB joypad 0 at $DFF220 (KB core/saga-registers/input.md,
 * toolchain/apollolib/input.md): bit 0 = connected; a set bit is a pressed
 * button, as ApolloJoypad reads it: 15 right, 14 left, 10 Start, 4 Y, 3 X,
 * 2 B, 1 A.
 *
 * DB9 joystick in port 2: $DFF20C, "the layout of JOY1DAT" (KB). The
 * direction decoding (right = bit 1, left = bit 9, down = bit 1 xor bit 0,
 * up = bit 9 xor bit 8) and the fire button (CIA-A $BFE001 bit 7, 0 =
 * pressed) are the classic Amiga ones; the KB does not describe them for
 * the V4 yet.
 *
 * Called once per engine tick (60 Hz). The Linux version changed the
 * steering once per rendered frame (up to 30 fps), so steering is up to
 * twice as fast here; tune STEERING_STEP when playing.
 */

#include "platform/amiga/game_input.h"
#include "platform/amiga/keyboard.h"
#include "ppengine/engine.h"
#include <stdio.h>

#define STEERING_STEP 1
#define PEDAL_FULL 7

#define JOYPAD0      (*(volatile uint16_t *)0xDFF220)
#define PAD_CONNECTED 0x0001
#define PAD_A        0x0002
#define PAD_B        0x0004
#define PAD_X        0x0008
#define PAD_Y        0x0010
#define PAD_START    0x0400
#define PAD_LEFT     0x4000
#define PAD_RIGHT    0x8000

#define DB9_PORT2    (*(volatile uint16_t *)0xDFF20C)
#define CIAA_PRA     (*(volatile uint8_t *)0xBFE001)
#define CIAA_FIRE1   0x80   /* port 2 fire button, 0 = pressed */

struct controls {
	int left, right, accelerate, brake, gear, credit;
};

static int previous_gear;
static int previous_credit;

static void read_joypad(struct controls *c) {
	uint16_t pad = JOYPAD0;

	if (!(pad & PAD_CONNECTED)) {
		return;
	}
	c->left |= (pad & PAD_LEFT) != 0;
	c->right |= (pad & PAD_RIGHT) != 0;
	c->accelerate |= (pad & PAD_A) != 0;
	c->brake |= (pad & PAD_B) != 0;
	c->gear |= (pad & (PAD_X | PAD_Y)) != 0;
	c->credit |= (pad & PAD_START) != 0;
}

static void read_db9(struct controls *c) {
	uint16_t joy = DB9_PORT2;
	uint16_t pad = JOYPAD0;
	int right = (joy >> 1) & 1;
	int left = (joy >> 9) & 1;

	c->right |= right;
	c->left |= left;
	c->brake |= right ^ (joy & 1);                /* down */
	c->accelerate |= left ^ ((joy >> 8) & 1);     /* up */
	/* On the V4 (Core 12856, measured with INPUTTEST) button A of the USB
	   joypad also pulls the port 2 fire line low. While A is down, the fire
	   line is the pad's A (gas), not a joystick's fire (gear). */
	if (!((pad & PAD_CONNECTED) && (pad & PAD_A))) {
		c->gear |= (CIAA_PRA & CIAA_FIRE1) == 0;
	}
}

void game_input_apply_tick(void) {
	struct controls c = { 0, 0, 0, 0, 0, 0 };

	c.left = keyboard_held(KEY_LEFT);
	c.right = keyboard_held(KEY_RIGHT);
	c.accelerate = keyboard_held(KEY_UP);
	c.brake = keyboard_held(KEY_DOWN);
	read_joypad(&c);
	read_db9(&c);

	if (c.left && !c.right) {
		steering_wheel_rotation -= STEERING_STEP;
	} else if (c.right && !c.left) {
		steering_wheel_rotation += STEERING_STEP;
	}
	internal_accel_pedal = c.accelerate ? PEDAL_FULL : 0;
	internal_brake_pedal = c.brake ? PEDAL_FULL : 0;

	/* Buttons count on the press, not while held. */
	while (keyboard_take_press(KEY_1)) {
		credits_remaining++;
	}
	if (c.credit && !previous_credit) {
		credits_remaining++;
	}
	previous_credit = c.credit;

	while (keyboard_take_press(KEY_SPACE)) {
		gear_binary = gear_binary ? 0 : 1;
	}
	if (c.gear && !previous_gear) {
		gear_binary = gear_binary ? 0 : 1;
	}
	previous_gear = c.gear;
}

/* For INPUTTEST: the raw registers and what is decoded from them. */
void game_input_describe(char *text, int size) {
	struct controls pad = { 0, 0, 0, 0, 0, 0 }, db9 = { 0, 0, 0, 0, 0, 0 };
	uint16_t raw_pad = JOYPAD0, raw_db9 = DB9_PORT2;
	uint8_t raw_cia = CIAA_PRA;

	read_joypad(&pad);
	read_db9(&db9);
	snprintf(text, size,
		"pad $%04x %s%s%s%s%s%s| db9 $%04x cia $%02x %s%s%s%s%s",
		(unsigned)raw_pad, pad.left ? "left " : "", pad.right ? "right " : "",
		pad.accelerate ? "A=gas " : "", pad.brake ? "B=brake " : "", pad.gear ? "gear " : "",
		pad.credit ? "start " : "",
		(unsigned)raw_db9, (unsigned)raw_cia, db9.left ? "left " : "", db9.right ? "right " : "",
		db9.accelerate ? "up=gas " : "", db9.brake ? "down=brake " : "", db9.gear ? "fire=gear " : "");
}

int game_input_quit_requested(void) {
	return keyboard_held(KEY_ESCAPE);
}
