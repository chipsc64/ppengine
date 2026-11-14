/*
 * Sound on Arne, the SAGA audio unit (port plan phase 5, T27-T28).
 *
 * KB: core/saga-registers/arne-channels.md, arne-dma-and-interrupts.md,
 *     core/saga-audio-arne/registers.md, toolchain/apollolib/sound.md
 *
 * Channel n: $DFF400 + n * $10: +0 pointer (L), +4 length in sample pairs
 * (L), +8 volume (left in bits 15-8, right in 7-0), +A control (bit 0
 * 16-bit, bit 1 one-shot), +C period (3546895 / sample rate, at least 59 for
 * Arne's 60 kHz maximum). Channels 4-15 start and stop with DMACON2 ($DFF296,
 * bit n-4, bit 15 = set). The start order is the one ApolloPlaySound uses:
 * pointer, length, volume 0, control, period, DMA on, volume.
 *
 * Channels: 5 for voice samples, 6-13 for the 8 engine sounds, 14 for
 * skidding, 15 for effects. 0-3 are Paula's and left to the OS; 4 is left
 * free.
 *
 * The Linux version (Gorilla Audio) played the same two loops with a pitch,
 * pan and gain per voice; this maps those to period and left/right volume.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include "audio/backend.h"

#define ASSET_DIR "PROGDIR:data/sounds/"

#define ARNE_CHANNEL(n) (0xDFF400 + (n) * 0x10)
#define ARNE_POINTER(n) (*(volatile ULONG *)(ARNE_CHANNEL(n) + 0x0))
#define ARNE_LENGTH(n)  (*(volatile ULONG *)(ARNE_CHANNEL(n) + 0x4))
#define ARNE_VOLUME(n)  (*(volatile UWORD *)(ARNE_CHANNEL(n) + 0x8))
#define ARNE_CONTROL(n) (*(volatile UWORD *)(ARNE_CHANNEL(n) + 0xA))
#define ARNE_PERIOD(n)  (*(volatile UWORD *)(ARNE_CHANNEL(n) + 0xC))
#define DMACON2         (*(volatile UWORD *)0xDFF296)

#define CONTROL_16BIT_MONO_LOOP 0x0001
#define CONTROL_16BIT_MONO_ONESHOT 0x0003
#define PAL_CLOCK 3546895UL
#define MIN_PERIOD 59
#define FIRST_ENGINE_CHANNEL 6
#define SKIDDING_CHANNEL 14
#define EFFECT_CHANNEL 15
#define VOICE_CHANNEL 5
#define VOICE_LEVEL 230
#define EFFECT_RATE 22050
#define EFFECT_LEVEL 200

/* Overall levels: eight engines and the skid play at once. */
#define ENGINE_LEVEL 150
#define SKIDDING_LEVEL 220
/* Sideways distance (engine units) at which a car is fully left or right. */
#define PAN_RANGE 1800

struct sample {
	void *memory;      /* AllocMem block */
	ULONG memory_size;
	UWORD *data;       /* 32-byte aligned, big-endian 16-bit */
	ULONG frames;
	ULONG rate;
};

static struct sample engine_sample, skidding_sample;
static struct sample effects[AUDIO_EFFECT_COUNT];
static struct sample voices[AUDIO_VOICE_COUNT];
static const char *const voice_names[AUDIO_VOICE_COUNT] = { "prepare_to_qualify" };
static int engine_playing[AUDIO_ENGINE_SOUNDS];
static int skidding_playing;
static int opened;

static ULONG read_le32(const UBYTE *p) {
	return (ULONG)p[0] | ((ULONG)p[1] << 8) | ((ULONG)p[2] << 16) | ((ULONG)p[3] << 24);
}

static UWORD read_le16(const UBYTE *p) {
	return (UWORD)(p[0] | (p[1] << 8));
}

/* 16-bit mono PCM WAV (tools/convert_assets.py writes these). */
static int load_wav(struct sample *sample, const char *name) {
	char path[96];
	FILE *file;
	UBYTE header[12], chunk[8], format[16];
	ULONG size, i;
	UBYTE *bytes;
	int have_format = 0;

	memset(sample, 0, sizeof(*sample));
	snprintf(path, sizeof(path), ASSET_DIR "%s.wav", name);
	file = fopen(path, "rb");
	if (!file) {
		return 0;
	}
	if (fread(header, 1, 12, file) != 12 || memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
		fclose(file);
		return 0;
	}
	while (fread(chunk, 1, 8, file) == 8) {
		size = read_le32(chunk + 4);
		if (memcmp(chunk, "fmt ", 4) == 0 && size >= 16) {
			if (fread(format, 1, 16, file) != 16) {
				break;
			}
			fseek(file, (long)(size - 16 + (size & 1)), SEEK_CUR);
			if (read_le16(format) != 1 || read_le16(format + 2) != 1 || read_le16(format + 14) != 16) {
				break;  /* not PCM, not mono or not 16-bit */
			}
			sample->rate = read_le32(format + 4);
			have_format = 1;
		} else if (memcmp(chunk, "data", 4) == 0 && have_format) {
			sample->frames = size / 2;
			sample->memory_size = size + 32;
			sample->memory = AllocMem(sample->memory_size, MEMF_ANY | MEMF_CLEAR);
			if (!sample->memory) {
				break;
			}
			sample->data = (UWORD *)(((ULONG)sample->memory + 31) & ~31UL);
			if (fread(sample->data, 1, size, file) != size) {
				FreeMem(sample->memory, sample->memory_size);
				sample->memory = NULL;
				break;
			}
			/* WAV is little-endian; Arne reads big-endian words. */
			bytes = (UBYTE *)sample->data;
			for (i = 0; i < size; i += 2) {
				UBYTE swap = bytes[i];
				bytes[i] = bytes[i + 1];
				bytes[i + 1] = swap;
			}
			fclose(file);
			return 1;
		} else {
			fseek(file, (long)(size + (size & 1)), SEEK_CUR);
		}
	}
	fclose(file);
	return 0;
}

static void free_sample(struct sample *sample) {
	if (sample->memory) {
		FreeMem(sample->memory, sample->memory_size);
	}
	memset(sample, 0, sizeof(*sample));
}

static void channel_stop(int channel) {
	DMACON2 = (UWORD)(1 << (channel - 4));
	ARNE_VOLUME(channel) = 0;
}

static void channel_start(int channel, const struct sample *sample, UWORD control) {
	channel_stop(channel);
	ARNE_POINTER(channel) = (ULONG)sample->data;
	ARNE_LENGTH(channel) = sample->frames / 2;   /* pairs of 16-bit samples */
	ARNE_VOLUME(channel) = 0;
	ARNE_CONTROL(channel) = control;
	ARNE_PERIOD(channel) = (UWORD)(PAL_CLOCK / sample->rate);
	DMACON2 = (UWORD)(0x8000 | (1 << (channel - 4)));
}

static void channel_start_loop(int channel, const struct sample *sample) {
	channel_start(channel, sample, CONTROL_16BIT_MONO_LOOP);
}

/* ------------------------------------------------------------------------ */
/* Generated effects */

static int allocate_sample(struct sample *sample, ULONG frames) {
	memset(sample, 0, sizeof(*sample));
	frames = (frames + 1) & ~1UL;                /* whole pairs */
	sample->memory_size = frames * 2 + 32;
	sample->memory = AllocMem(sample->memory_size, MEMF_ANY | MEMF_CLEAR);
	if (!sample->memory) {
		return 0;
	}
	sample->data = (UWORD *)(((ULONG)sample->memory + 31) & ~31UL);
	sample->frames = frames;
	sample->rate = EFFECT_RATE;
	return 1;
}

/* Square-wave beeps: `count` beeps of `on_ms` with `off_ms` between,
   amplitude falling off over the last 20 % of each beep. */
static int make_beeps(struct sample *sample, int frequency, int on_ms, int off_ms, int count) {
	ULONG on = EFFECT_RATE * on_ms / 1000;
	ULONG off = EFFECT_RATE * off_ms / 1000;
	ULONG half_period = EFFECT_RATE / (2 * frequency);
	ULONG i, beep;
	WORD *out;

	if (!allocate_sample(sample, (on + off) * count)) {
		return 0;
	}
	out = (WORD *)sample->data;
	for (beep = 0; beep < (ULONG)count; beep++) {
		for (i = 0; i < on; i++) {
			LONG amplitude = 9000;
			if (i > on * 4 / 5) {
				amplitude = amplitude * (LONG)(on - i) / (LONG)(on / 5 + 1);
			}
			*out++ = (WORD)(((i / half_period) & 1) ? amplitude : -amplitude);
		}
		out += off;   /* silence (cleared memory) */
	}
	return 1;
}

/* Explosion: noise from a 16-bit LFSR, smoothed (darker sound) and fading. */
static int make_explosion(struct sample *sample) {
	ULONG frames = EFFECT_RATE * 12 / 10;
	UWORD lfsr = 0xACE1;
	LONG smooth = 0;
	ULONG i;
	WORD *out;

	if (!allocate_sample(sample, frames)) {
		return 0;
	}
	out = (WORD *)sample->data;
	for (i = 0; i < frames; i++) {
		LONG noise, amplitude;

		lfsr = (UWORD)((lfsr >> 1) ^ (-(lfsr & 1) & 0xB400u));
		noise = (LONG)(lfsr & 0xffff) - 32768;
		smooth += (noise - smooth) / 6;
		amplitude = 30000 - (LONG)(30000 * i / frames);
		out[i] = (WORD)(smooth * amplitude / 32768);
	}
	return 1;
}

static int make_effects(void) {
	return make_beeps(&effects[AUDIO_EFFECT_START_LIGHT], 440, 350, 0, 1)
		&& make_beeps(&effects[AUDIO_EFFECT_START_GO], 880, 900, 0, 1)
		&& make_beeps(&effects[AUDIO_EFFECT_EXTENDED_PLAY], 1320, 80, 60, 3)
		&& make_beeps(&effects[AUDIO_EFFECT_BONUS_TICK], 1760, 25, 0, 1)
		&& (load_wav(&effects[AUDIO_EFFECT_EXPLOSION], "crash") || make_explosion(&effects[AUDIO_EFFECT_EXPLOSION]));
}

void audio_backend_play_voice(enum audio_voice voice) {
	if (!opened || voice < 0 || voice >= AUDIO_VOICE_COUNT || !voices[voice].data) {
		return;
	}
	channel_start(VOICE_CHANNEL, &voices[voice], CONTROL_16BIT_MONO_ONESHOT);
	ARNE_VOLUME(VOICE_CHANNEL) = (UWORD)((VOICE_LEVEL << 8) | VOICE_LEVEL);
}

void audio_backend_play_effect(enum audio_effect effect) {
	if (!opened || effect < 0 || effect >= AUDIO_EFFECT_COUNT || !effects[effect].data) {
		return;
	}
	channel_start(EFFECT_CHANNEL, &effects[effect], CONTROL_16BIT_MONO_ONESHOT);
	ARNE_VOLUME(EFFECT_CHANNEL) = (UWORD)((EFFECT_LEVEL << 8) | EFFECT_LEVEL);
}

static UWORD period_for(ULONG rate) {
	ULONG period;

	if (rate == 0) {
		return 0xffff;
	}
	period = PAL_CLOCK / rate;
	if (period < MIN_PERIOD) {
		period = MIN_PERIOD;
	}
	if (period > 0xffff) {
		period = 0xffff;
	}
	return (UWORD)period;
}

/* level 0-255 for the whole sound, pan -1..1 as -PAN_RANGE..PAN_RANGE */
static UWORD stereo_volume(int level, int pan) {
	int left = level, right = level;

	if (pan > PAN_RANGE) pan = PAN_RANGE;
	if (pan < -PAN_RANGE) pan = -PAN_RANGE;
	if (pan > 0) {
		left = level * (PAN_RANGE - pan) / PAN_RANGE;
	} else {
		right = level * (PAN_RANGE + pan) / PAN_RANGE;
	}
	if (left > 255) left = 255;
	if (right > 255) right = 255;
	return (UWORD)((left << 8) | right);
}

int audio_backend_open(void) {
	int i;

	if (!load_wav(&engine_sample, "engine") || !load_wav(&skidding_sample, "skidding") || !make_effects()) {
		free_sample(&engine_sample);
		free_sample(&skidding_sample);
		for (i = 0; i < AUDIO_EFFECT_COUNT; i++) {
			free_sample(&effects[i]);
		}
		return 0;
	}
	/* Voices are optional: without the file the game stays silent there. */
	for (i = 0; i < AUDIO_VOICE_COUNT; i++) {
		load_wav(&voices[i], voice_names[i]);
	}
	memset(engine_playing, 0, sizeof(engine_playing));
	skidding_playing = 0;
	opened = 1;
	return 1;
}

void audio_backend_silence(void) {
	int i;

	if (!opened) {
		return;
	}
	for (i = 0; i < AUDIO_ENGINE_SOUNDS; i++) {
		channel_stop(FIRST_ENGINE_CHANNEL + i);
		engine_playing[i] = 0;
	}
	channel_stop(SKIDDING_CHANNEL);
	channel_stop(EFFECT_CHANNEL);
	channel_stop(VOICE_CHANNEL);
	skidding_playing = 0;
}

void audio_backend_close(void) {
	int i;

	audio_backend_silence();
	free_sample(&engine_sample);
	free_sample(&skidding_sample);
	for (i = 0; i < AUDIO_EFFECT_COUNT; i++) {
		free_sample(&effects[i]);
	}
	for (i = 0; i < AUDIO_VOICE_COUNT; i++) {
		free_sample(&voices[i]);
	}
	opened = 0;
}

void audio_backend_start_engine_loop(int index) {
	if (opened && index >= 0 && index < AUDIO_ENGINE_SOUNDS && !engine_playing[index]) {
		channel_start_loop(FIRST_ENGINE_CHANNEL + index, &engine_sample);
		engine_playing[index] = 1;
	}
}

void audio_backend_stop_engine_loop(int index) {
	if (opened && index >= 0 && index < AUDIO_ENGINE_SOUNDS && engine_playing[index]) {
		channel_stop(FIRST_ENGINE_CHANNEL + index);
		engine_playing[index] = 0;
	}
}

void audio_backend_set_engine_attributes(int index, uint32_t pitch, int16_t pan, int32_t volume) {
	int channel = FIRST_ENGINE_CHANNEL + index;
	/* Gorilla version: pitch factor 0.5 + (pitch / 65536) / 128, in integer
	   steps of 1/256: (128 + (pitch >> 16) * 2) / 256. */
	ULONG factor_256 = 128 + (pitch >> 16) * 2;
	ULONG rate = engine_sample.rate * factor_256 / 256;

	if (!opened || index < 0 || index >= AUDIO_ENGINE_SOUNDS || !engine_playing[index]) {
		return;
	}
	if (volume < 0) volume = 0;
	if (volume > 256) volume = 256;
	ARNE_PERIOD(channel) = period_for(rate);
	ARNE_VOLUME(channel) = stereo_volume((int)(volume * ENGINE_LEVEL / 256), pan);
}

void audio_backend_start_skidding_loop(void) {
	if (opened && !skidding_playing) {
		channel_start_loop(SKIDDING_CHANNEL, &skidding_sample);
		skidding_playing = 1;
	}
}

void audio_backend_stop_skidding_loop(void) {
	if (opened && skidding_playing) {
		channel_stop(SKIDDING_CHANNEL);
		skidding_playing = 0;
	}
}

void audio_backend_set_skidding_loop_volume(int skidding_volume) {
	/* Gorilla version: gain = skidding_volume / 8 */
	int level = skidding_volume * SKIDDING_LEVEL / 8;

	if (!opened || !skidding_playing) {
		return;
	}
	if (level < 0) level = 0;
	if (level > 255) level = 255;
	ARNE_VOLUME(SKIDDING_CHANNEL) = stereo_volume(level, 0);
}

void audio_backend_update(void) {
	/* Arne plays by DMA; nothing to do per frame. */
}
