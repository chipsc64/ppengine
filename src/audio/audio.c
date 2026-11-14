#include "audio/audio.h"
#include "audio/backend.h"
#include "engine.h"
#include <stdlib.h>

#define ENGINE_SOUND_COUNT 8

static int previous_car_object_type_and_visibility[ENGINE_SOUND_COUNT];
static int explosion_detection_enabled=0;
static int previous_player_engine_sound_active;
static int previous_player_type_and_visibility;
static int previous_skidding_volume;

int audio_open(void) {
	return audio_backend_open();
}

void audio_close(void) {
	audio_backend_close();
}

void audio_update(struct movingobject *moving_objects, uint16_t player_engine_pitch, uint16_t player_engine_sound_active, uint16_t skidding_volume) {
	struct movingobject *current_object;
	int volume;
	int object_index;

	current_object=moving_objects;
	for (object_index=0; object_index<ENGINE_SOUND_COUNT-1; object_index++) {
		if (explosion_detection_enabled) {
			if ((current_object->type_and_visibility==0x8003) && (previous_car_object_type_and_visibility[object_index]!=0x8003)) {
				audio_backend_stop_engine_loop(object_index);
			} else if ((current_object->type_and_visibility==0x8002) && (previous_car_object_type_and_visibility[object_index]==0x2)) {
				audio_backend_start_engine_loop(object_index);
			} else if ((current_object->type_and_visibility==0x2) && (previous_car_object_type_and_visibility[object_index]==0x8002)) {
				audio_backend_stop_engine_loop(object_index);
			}
		}

		if (current_object->player_forward_position_difference>0) {
			volume=current_object->player_forward_position_difference/4;
		} else {
			volume=-current_object->player_forward_position_difference*2;
		}
		if (player_engine_sound_active) {
			if (volume>256) {
				volume=256;
			}
			volume=256-volume;
		} else {
			volume=0;
		}

		if (current_object->type_and_visibility==0x8002) {
			audio_backend_set_engine_attributes(
				object_index,
				current_object->current_speed,
				current_object->player_sideways_position_difference,
				volume
			);
		}
		previous_car_object_type_and_visibility[object_index]=current_object->type_and_visibility;
		current_object++;
	}

	current_object=&moving_objects[11];
	if ((current_object->type_and_visibility==0x8004) && (previous_player_type_and_visibility!=0x8004)) {
		if (jump_table_function_number==3) {
			audio_backend_play_effect(AUDIO_EFFECT_EXPLOSION);
		}
	}
	if ((player_engine_sound_active&1)!=(previous_player_engine_sound_active&1)) {
		if (player_engine_sound_active) {
			audio_backend_start_engine_loop(ENGINE_SOUND_COUNT-1);
		} else {
			audio_backend_stop_engine_loop(ENGINE_SOUND_COUNT-1);
		}
	}
	if (player_engine_sound_active) {
		audio_backend_set_engine_attributes(
			ENGINE_SOUND_COUNT-1,
			player_engine_pitch*55000,
			0,
			256
		);
	}

	if (!player_engine_sound_active) {
		skidding_volume=0;
	}
	if (previous_skidding_volume!=skidding_volume) {
		if (skidding_volume==0) {
			audio_backend_stop_skidding_loop();
		} else {
			if (previous_skidding_volume==0) {
				audio_backend_start_skidding_loop();
			}
			audio_backend_set_skidding_loop_volume(skidding_volume);
		}
	}

	previous_player_engine_sound_active=player_engine_sound_active;
	previous_player_type_and_visibility=current_object->type_and_visibility;
	previous_skidding_volume=skidding_volume;

	/* Voice samples: the arcade code sets sample_player_1 to a sample number
	   (and the sound CPU cleared it). 2 = "Prepare to qualify", at the start
	   of a game (frame handler 66a). 3, 4 and bit 5 ask for samples we do
	   not have. */
	if (sample_player_1) {
		if ((sample_player_1==2) && (jump_table_function_number==3)) {
			audio_backend_play_voice(AUDIO_VOICE_PREPARE_TO_QUALIFY);
		}
		sample_player_1=0;
	}

	/* Bonus counting: the arcade code sets these sound bits (and the sound
	   CPU cleared them); the Linux version ignored them. */
	if (sample_player_2&(0x80|0x02)) {
		sample_player_2&=~(0x80|0x02);
		audio_backend_play_effect(AUDIO_EFFECT_BONUS_TICK);
	}

	explosion_detection_enabled=1;	
	audio_backend_update();
}

/* The engine's sound effect callback. The Linux version only printed the
   index; the meanings come from the engine source comments. */
void audio_play_effect(int effect_index) {
	/* Silent outside a game, like the engine sounds (the demo uses the same
	   code paths). Game state 3 = a game is running. */
	if (jump_table_function_number!=3) {
		return;
	}
	switch (effect_index) {
		case 12:  /* starting light: sub/common/aba.c */
			audio_backend_play_effect(AUDIO_EFFECT_START_LIGHT);
			break;
		case 13:  /* last starting light, go */
			audio_backend_play_effect(AUDIO_EFFECT_START_GO);
			break;
		case 7:   /* extended play beeps: sub/common/1708.c */
			audio_backend_play_effect(AUDIO_EFFECT_EXTENDED_PLAY);
			break;
		default:
			break;
	}
}


