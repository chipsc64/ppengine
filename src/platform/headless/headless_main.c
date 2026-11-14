/*
 * ppengine headless reference run (port plan T05).
 *
 * Runs the arcade engine without graphics, sound or input, feeds it a fixed
 * input script and prints a checksum of the engine state. Build it for the
 * host and for the Apollo V4: identical output proves that the engine
 * behaves the same on the 68080.
 *
 * Usage: ppengine_headless [frames]   (default 7200 = 2 minutes at 60 Hz)
 *
 * Output: one line every 60 frames, then a summary. The checksum covers
 * the engine state only (integers). The track geometry uses floating point
 * and is printed separately with 3 decimals: small differences there come
 * from x87 versus 68881 rounding and are expected.
 */

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "ppengine/engine.h"
#include "track_geometry/track_geometry.h"
#include "track_geometry/segments.h"
#include "ppengine/sub/data/track_curve_data.h"

/* Engine globals that engine.h does not declare (see engine.c). */
extern uint32_t credits_remaining;
extern uint16_t internal_accel_pedal;
extern uint16_t internal_brake_pedal;
extern uint16_t gear_binary;
extern uint16_t steering_wheel_rotation;
extern int16_t clipped_steering;
extern uint16_t engine_sound_pitch;
extern uint16_t engine_sound_active;
extern uint16_t skidding_volume;
extern uint16_t gantry_and_starting_lights_appearance;
extern uint16_t jump_table_function_number;
extern uint16_t jump_table_subfunction_number;
extern uint32_t forward_position;
extern uint32_t current_score;

static uint32_t checksum;
static uint32_t sound_effects;

/* FNV-1a over values, not over raw struct bytes: struct padding differs
   between the host and m68k (m68k aligns 32-bit fields to 2 bytes). */
static void hash_u32(uint32_t value) {
	int i;

	for (i = 0; i < 4; i++) {
		checksum ^= (value >> (i * 8)) & 0xff;
		checksum *= 16777619u;
	}
}

static void hash_engine_state(void) {
	int i;
	struct movingobject *o;

	for (i = 0; i < 32; i++) {
		o = &moving_objects[i];
		hash_u32(o->type_and_visibility);
		hash_u32(o->forward_position);
		hash_u32((uint16_t)o->sideways_position);
		hash_u32((uint16_t)o->player_forward_position_difference);
		hash_u32((uint16_t)o->player_sideways_position_difference);
		hash_u32(o->display_variant);
		hash_u32((uint32_t)o->current_speed);
		hash_u32(o->maximum_speed);
		hash_u32(o->unknown_attribute);
		hash_u32((uint16_t)o->sideways_movement);
		hash_u32((uint16_t)o->one_time_speed_modification);
		hash_u32((uint16_t)o->explosion_countdown);
	}
	for (i = 0; i < HUD_CONTENT_LENGTH; i++) {
		hash_u32(((uint32_t)hud_content[i].character << 8) | hud_content[i].attribute);
	}
	hash_u32(forward_position);
	hash_u32(current_score);
	hash_u32(jump_table_function_number);
	hash_u32(jump_table_subfunction_number);
	hash_u32(engine_sound_pitch);
	hash_u32(engine_sound_active);
	hash_u32(skidding_volume);
	hash_u32(gantry_and_starting_lights_appearance);
	hash_u32((uint16_t)clipped_steering);
}

static void count_sound_effect(int effect_index) {
	sound_effects++;
	hash_u32(0x50000000u | (uint32_t)effect_index);
}

/* Fixed input script: demo mode first, then insert a coin, start, drive
   with alternating steering, change gear and brake now and then. */
static void apply_input_script(uint32_t frame) {
	if (frame == 900) {
		credits_remaining++;
	}
	if (frame >= 1000) {
		uint32_t t = frame - 1000;

		internal_accel_pedal = ((t % 600) < 540) ? 7 : 0;
		internal_brake_pedal = ((t % 600) >= 560) ? 7 : 0;
		if ((t % 240) < 40) {
			steering_wheel_rotation -= 1;
		} else if ((t % 240) >= 120 && (t % 240) < 160) {
			steering_wheel_rotation += 1;
		}
		if (t == 120) {
			gear_binary = 1;
		}
	}
}

static void print_track_sample(void) {
	struct segment *segments = track_geometry_get_segments();
	static const uint32_t sample[] = { 0, 16384, 32768, 49152, 65535 };
	unsigned int i;

	for (i = 0; i < sizeof(sample) / sizeof(sample[0]); i++) {
		struct segment *s = &segments[sample[i]];

		printf("track %5lu centre %10.3f %10.3f angle %7.3f\n",
			(unsigned long)sample[i], s->centre.x, s->centre.y, s->angle_radians);
	}
}

int main(int argc, char **argv) {
	uint32_t frames = 7200;
	uint32_t frame;

	if (argc > 1) {
		/* ignore an empty or non-numeric argument: a launcher may pass one */
		uint32_t requested = (uint32_t)strtoul(argv[1], NULL, 10);

		if (requested > 0) {
			frames = requested;
		}
	}

	checksum = 2166136261u;
	engine_initialise();
	engine_register_sound_effect_callback(count_sound_effect);
	if (!track_geometry_initialise(track_curve_data)) {
		return 20;
	}
	print_track_sample();

	for (frame = 1; frame <= frames; frame++) {
		apply_input_script(frame);
		engine_execute_frame();
		hash_engine_state();
		if (frame % 60 == 0) {
			printf("frame %6lu state %2u/%2u pos %08lx score %6lu checksum %08lx\n",
				(unsigned long)frame,
				(unsigned int)jump_table_function_number,
				(unsigned int)jump_table_subfunction_number,
				(unsigned long)forward_position,
				(unsigned long)current_score,
				(unsigned long)checksum);
		}
	}
	printf("done frames %lu sound_effects %lu checksum %08lx\n",
		(unsigned long)frames, (unsigned long)sound_effects, (unsigned long)checksum);

	track_geometry_terminate();
	return 0;
}
