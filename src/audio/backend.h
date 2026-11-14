#ifndef __AUDIO_BACKEND_H
#define __AUDIO_BACKEND_H

/*
 * What audio.c needs from a sound backend. The Linux version had the same
 * functions with a gorilla_audio_ prefix (src/audio/gorilla/); the Apollo V4
 * version plays on Arne (src/audio/arne/).
 */

#include <inttypes.h>

#define AUDIO_ENGINE_SOUNDS 8   /* 7 CPU cars + the player (index 7) */

int audio_backend_open(void);   /* 0 on failure; load while the OS runs */
void audio_backend_close(void);
void audio_backend_start_engine_loop(int object_index);
void audio_backend_stop_engine_loop(int object_index);
/* pitch: as the engine gives it (speed or player pitch * 55000);
   pan: the car's sideways distance to the player (signed);
   volume: 0-256 */
void audio_backend_set_engine_attributes(int object_index, uint32_t pitch, int16_t pan, int32_t volume);
void audio_backend_start_skidding_loop(void);
void audio_backend_stop_skidding_loop(void);
void audio_backend_set_skidding_loop_volume(int skidding_volume);
void audio_backend_update(void);
void audio_backend_silence(void);   /* all channels quiet (pause, exit) */

/* Short effects, mostly generated at open: one channel, a new effect cuts
   off the previous one. */
enum audio_effect {
	AUDIO_EFFECT_START_LIGHT,    /* red starting light: low beep */
	AUDIO_EFFECT_START_GO,       /* green light: high beep */
	AUDIO_EFFECT_EXTENDED_PLAY,  /* three quick beeps */
	AUDIO_EFFECT_BONUS_TICK,     /* bonus counting */
	AUDIO_EFFECT_EXPLOSION,      /* data/sounds/crash.wav; generated noise without it */
	AUDIO_EFFECT_COUNT
};
void audio_backend_play_effect(enum audio_effect effect);

/* Voice samples of the arcade (data/sounds/), on their own channel. */
enum audio_voice {
	AUDIO_VOICE_PREPARE_TO_QUALIFY,
	AUDIO_VOICE_COUNT
};
void audio_backend_play_voice(enum audio_voice voice);

#endif
