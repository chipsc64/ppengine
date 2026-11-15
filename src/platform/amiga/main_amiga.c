/*
 * ppengine for the Apollo V4 - main program (port plan phases 1 and 4).
 *
 * Target: AmigaOS 3.9, Apollo V4 (68080, SAGA, Maggie through maggie.library).
 * Execution mode: os-takeover (multitasking off, direct SAGA access,
 * multitasking and the Workbench screen restored on exit).
 * KB: os/os-takeover.md, toolchain/apollolib/system-and-timing.md,
 *     core/saga-registers/chunky-plane.md, os/libraries/maggie-library.md
 *
 * Frame loop: the arcade logic runs one tick per VBlank (60 Hz in the
 * 320x240 mode). VBlanks are counted by ApolloLib's interrupt server
 * (ApolloTimer/GlobalTimer) rather than by polling INTREQR, because the OS
 * interrupt handlers keep running under Forbid() and clear that bit too.
 * When a frame takes longer than a VBlank, the missed ticks are run before
 * the next render (at most MAX_CATCH_UP_TICKS).
 *
 * Display flip and keyboard reading run in interrupt servers (irq_servers.s):
 * three buffers, the finished one goes on screen at the next VBlank.
 *
 * Keys: cursor keys drive, 1 = credit, space = gear, Tab = debug view,
 * Esc = quit. USB joypad and DB9 joystick too (platform/amiga/game_input.c).
 *
 * Arguments (from a Shell; from Workbench the icon's tool type LOWRES):
 *   LOWRES               320x240 instead of 640x480 (both 60 Hz)
 *   CCW                  the other front-face winding (testing)
 *   INPUTTEST            10 s of joypad/joystick readings (checks the mapping)
 *   SOUNDTEST [NOFORBID] engine sound up the revs, panned, skid, effects;
 *                        multitasking off unless NOFORBID
 *   SNAPSHOT t1 t2 ...   no takeover: run the engine with a fixed input
 *                        script and save the frames at ticks t1, t2, ... to
 *                        RAM:ppsnap_<tick>.565 (format P565, see
 *                        tools/convert_assets.py), with the render time
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dos/dos.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <hardware/custom.h>
#include <hardware/dmabits.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <proto/dos.h>
#include <proto/icon.h>
#include "ApolloCrossDev_Lib.h"

#include "ppengine/engine.h"
#include "track_geometry/track_geometry.h"
#include "ppengine/sub/data/track_curve_data.h"
#include "scene/scene.h"
#include "scene/backend.h"
#include "scene/maggie/assets.h"
#include "scene/maggie/maggie_backend.h"
#include "platform/amiga/saga_display.h"
#include "platform/amiga/keyboard.h"
#include "platform/amiga/game_input.h"
#include "platform/amiga/debug_view.h"
#include "platform/amiga/interrupts.h"
#include "audio/audio.h"
#include "audio/backend.h"

#define MAX_CATCH_UP_TICKS 4
#define KEY_TAB 0x42
#define SR_APOLLO_BIT 0x0800  /* KB core/cpu-68080/prm/apollo-bit.md */

extern struct ExecBase *SysBase;
extern struct Custom custom;
extern struct WBStartup *_WBenchMsg;   /* libnix: set when started from Workbench */
struct Library *IconBase;

/* Started from Workbench: the icon's tool types are read (LOWRES gives
   320x240), and messages go to a console window that opens only when
   something is written (there is no Shell). Informational messages are left
   out then, so a normal game opens no window at all; an error stays on
   screen for 5 seconds, then the window closes with the program. */
static int from_workbench;
#define INFO(...) do { if (!from_workbench) printf(__VA_ARGS__); } while (0)

static int finish(int result) {
	if (from_workbench && result != RETURN_OK) {
		fflush(stdout);
		Delay(5 * 50);
	}
	return result;
}

static void workbench_start(void) {
	struct WBArg *arg;
	struct DiskObject *icon;
	BPTR old_dir;

	from_workbench = 1;
	freopen("CON:40/40/600/120/ppengine/AUTO/CLOSE", "w", stdout);
	IconBase = OpenLibrary((CONST_STRPTR)"icon.library", 36);
	if (!IconBase || !_WBenchMsg || _WBenchMsg->sm_NumArgs < 1) {
		if (IconBase) {
			CloseLibrary(IconBase);
			IconBase = NULL;
		}
		return;
	}
	arg = &_WBenchMsg->sm_ArgList[0];
	old_dir = CurrentDir(arg->wa_Lock);
	icon = GetDiskObject((CONST_STRPTR)arg->wa_Name);
	if (icon) {
		if (FindToolType((CONST_STRPTR *)icon->do_ToolTypes, (CONST_STRPTR)"LOWRES")) {
			saga_display_select(0);
		}
		FreeDiskObject(icon);
	}
	CurrentDir(old_dir);
	CloseLibrary(IconBase);
	IconBase = NULL;
}

/* KB os/best-practices/detect-68080-and-fpu-at-run-time.md */
static int has_68080(void) { return (SysBase->AttnFlags & (1 << 10)) != 0; }
static int has_fpu(void)   { return (SysBase->AttnFlags & AFF_68881) != 0; }

static volatile ULONG *const vblank_counter = &GlobalTimer;

static void sound_effect(int effect_index) {
	(void)effect_index;  /* phase 5 */
}

/* CPU cycles per millisecond: the clock multiplier at $DFF3FC (low byte,
   14 when 0) times 7.094 MHz, as ApolloCPUDelay uses it
   (KB toolchain/apollolib/system-and-timing.md). */
static ULONG cycles_per_ms(void) {
	ULONG multiplier = *(volatile UWORD *)0xDFF3FC & 0xff;
	return (multiplier ? multiplier : 14) * 7094;
}

/* The scene update calls of src/client.c, once per rendered frame. */
static void update_scene(void) {
	scene_update_display_mode_from_game_state(jump_table_function_number, jump_table_subfunction_number);
	scene_update_moving_objects_and_camera(moving_objects, forward_position, clipped_steering);
	scene_update_hud_content(hud_content);
	scene_update_gantry(gantry_and_starting_lights_appearance);
}

/* Wait until at least one VBlank has passed since last_vblank (returns at
   once when the last frame took longer). Returns the number of VBlanks. */
static uint32_t wait_for_vblank(ULONG *last_vblank) {
	ULONG now;
	uint32_t elapsed;

	while ((now = *vblank_counter) == *last_vblank) {
		keyboard_poll();
		if (game_input_quit_requested()) {
			break;
		}
	}
	elapsed = (uint32_t)(now - *last_vblank);
	*last_vblank = now;
	return elapsed;
}

/* ------------------------------------------------------------------------ */
/* SNAPSHOT mode */

/* The input script of src/platform/headless/headless_main.c */
static void apply_input_script(uint32_t tick) {
	if (tick == 900) {
		credits_remaining++;
	}
	if (tick >= 1000) {
		uint32_t t = tick - 1000;

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

static int save_snapshot(const UWORD *pixels, uint32_t tick) {
	char path[64];
	UWORD header[4] = { SCREEN_WIDTH, SCREEN_HEIGHT, 0, 0 };
	FILE *file;

	snprintf(path, sizeof(path), "RAM:ppsnap_%05lu.565", (unsigned long)tick);
	file = fopen(path, "wb");
	if (!file) {
		return 0;
	}
	fwrite("P565", 1, 4, file);
	fwrite(header, 2, 4, file);
	fwrite(pixels, 2, SCREEN_WIDTH * SCREEN_HEIGHT, file);
	fclose(file);
	return 1;
}

static int benchmark;
static int depth_test;

static int run_snapshots(int count, char **ticks) {
	void *memory = AllocMem(SCREEN_WIDTH * SCREEN_HEIGHT * 2 + 32, MEMF_ANY | MEMF_CLEAR);
	UWORD *pixels;
	uint32_t last = 0, tick = 0;
	ULONG per_ms = cycles_per_ms();
	int i;

	if (!memory) {
		return RETURN_FAIL;
	}
	pixels = (UWORD *)(((ULONG)memory + 31) & ~31UL);
	for (i = 0; i < count; i++) {
		uint32_t wanted = (uint32_t)strtoul(ticks[i], NULL, 10);
		if (wanted > last) {
			last = wanted;
		}
	}
	while (tick < last) {
		tick++;
		apply_input_script(tick);
		engine_execute_frame();
		update_scene();
		for (i = 0; i < count; i++) {
			if ((uint32_t)strtoul(ticks[i], NULL, 10) == tick) {
				const struct backend_stats *stats;
				ULONG start, cycles;

				scene_backend_set_target(pixels);
				start = ApolloCPUTick();
				scene_generate_frame();
				cycles = ApolloCPUTick() - start;
				stats = maggie_backend_stats();
				save_snapshot(pixels, tick);
				if (depth_test) {
					maggie_backend_depth_test();
					save_snapshot(pixels, tick + 100000);
				}
				printf("tick %5lu: %lu.%02lu ms (background %lu, 3D %lu, HUD %lu us), %d objects, %d parts, %d road quads\n",
					(unsigned long)tick, (unsigned long)(cycles / per_ms),
					(unsigned long)((cycles % per_ms) * 100 / per_ms),
					stats->background_cycles / (per_ms / 1000), stats->scene_cycles / (per_ms / 1000),
					stats->hud_cycles / (per_ms / 1000),
					stats->objects, stats->parts, stats->road_quads);
				printf("            3D: setup %lu, road %lu, cars %lu, scenery %lu us\n",
					stats->setup_cycles / (per_ms / 1000), stats->road_cycles / (per_ms / 1000),
					stats->car_cycles / (per_ms / 1000), stats->scenery_cycles / (per_ms / 1000));
				if (benchmark) {
					unsigned long bench_cycles[MAGGIE_LOD_LEVELS];
					int bench_polygons[MAGGIE_LOD_LEVELS], bench_parts[MAGGIE_LOD_LEVELS], level;

					maggie_backend_benchmark(bench_cycles, bench_polygons, bench_parts);
					for (level = 0; level < MAGGIE_LOD_LEVELS; level++) {
						printf("            player car LOD %d: %d polygons, %d parts: %lu us\n", level,
							bench_polygons[level], bench_parts[level], bench_cycles[level] / (per_ms / 1000));
					}
				}
			}
		}
	}
	FreeMem(memory, SCREEN_WIDTH * SCREEN_HEIGHT * 2 + 32);
	return RETURN_OK;
}

/* ------------------------------------------------------------------------ */
/* SOUNDTEST mode: the player's engine sound rises from idle to full revs,
   then pans from left to right, then the skid plays, then the effects.
   Multitasking is off while it plays, as in the game; with NOFORBID it stays
   on (both work: a first silent run on 2026-11-14 turned out to be a mistake
   in the test setup, not Arne). The Arne DMA bits (DMACON2R, $DFF202) are
   printed afterwards. */

static int run_sound_test(int multitasking_off) {
	volatile UWORD *dmacon2r = (volatile UWORD *)0xDFF202;
	UWORD before, engine_on, skid_on, after;
	int step;

	if (!audio_open()) {
		printf("ppengine: cannot load PROGDIR:data/sounds/\n");
		return RETURN_FAIL;
	}
	printf("soundtest: 20 seconds, multitasking %s...\n", multitasking_off ? "off" : "on");
	if (multitasking_off) {
		ApolloForbid();
	}
	before = *dmacon2r;
	audio_backend_start_engine_loop(7);
	engine_on = *dmacon2r;
	for (step = 0; step <= 60; step++) {          /* 3 s: idle to full revs */
		audio_backend_set_engine_attributes(7, (uint32_t)step * 4 * 65536, 0, 256);
		ApolloCPUDelay(50);
	}
	for (step = -30; step <= 30; step++) {        /* 3 s: left to right */
		audio_backend_set_engine_attributes(7, 120UL * 65536, (int16_t)(step * 60), 256);
		ApolloCPUDelay(50);
	}
	audio_backend_stop_engine_loop(7);
	audio_backend_start_skidding_loop();
	audio_backend_set_skidding_loop_volume(8);
	skid_on = *dmacon2r;
	ApolloCPUDelay(1500);
	audio_backend_stop_skidding_loop();
	/* effects: three red lights, green, extended play, bonus ticks, explosion */
	for (step = 0; step < 3; step++) {
		audio_backend_play_effect(AUDIO_EFFECT_START_LIGHT);
		ApolloCPUDelay(1000);
	}
	audio_backend_play_effect(AUDIO_EFFECT_START_GO);
	ApolloCPUDelay(1500);
	audio_backend_play_effect(AUDIO_EFFECT_EXTENDED_PLAY);
	ApolloCPUDelay(1000);
	for (step = 0; step < 10; step++) {
		audio_backend_play_effect(AUDIO_EFFECT_BONUS_TICK);
		ApolloCPUDelay(133);
	}
	audio_backend_play_effect(AUDIO_EFFECT_EXPLOSION);
	ApolloCPUDelay(2600);
	audio_backend_play_voice(AUDIO_VOICE_PREPARE_TO_QUALIFY);
	ApolloCPUDelay(2000);
	audio_close();
	after = *dmacon2r;
	if (multitasking_off) {
		ApolloPermit();
	}
	printf("soundtest: DMACON2R before $%04x, engine $%04x, skid $%04x, after $%04x\n",
		(unsigned)before, (unsigned)engine_on, (unsigned)skid_on, (unsigned)after);
	return RETURN_OK;
}

/* ------------------------------------------------------------------------ */
/* INPUTTEST mode: 10 seconds, four lines per second with the joypad and
   joystick registers and what the game decodes from them. OS running. */

static int run_input_test(void) {
	char text[160], last[160] = "";
	int step;

	printf("inputtest: 10 seconds, move the stick and press the buttons...\n");
	for (step = 0; step < 40; step++) {
		game_input_describe(text, sizeof(text));
		if (strcmp(text, last) != 0) {
			printf("%4d ms  %s\n", step * 250, text);
			strcpy(last, text);
		}
		ApolloCPUDelay(250);
	}
	return RETURN_OK;
}

/* ------------------------------------------------------------------------ */
/* The game */

static int run_game(void) {
	ULONG last_vblank, start_vblank;
	uint32_t ticks;
	uint32_t frames = 0, engine_ticks = 0, dropped_ticks = 0;
	ULONG render_cycles = 0, render_worst = 0, per_ms = cycles_per_ms();
	ULONG frame_budget = per_ms * 1000 / 60;   /* one VBlank at 60 Hz */
	ULONG within[4] = { 0, 0, 0, 0 };          /* render took <=1, 2, 3, >3 VBlanks */
	UWORD old_dma;
	int show_debug_view = 0;
	int sound = audio_open();

	if (!sound) {
		printf("ppengine: no sound (PROGDIR:data/sounds/ missing or unreadable).\n");
	} else {
		engine_register_sound_effect_callback(audio_play_effect);
	}
	debug_view_init();
	if (!saga_display_open()) {
		printf("ppengine: not enough memory for the screen buffers.\n");
		audio_close();
		return RETURN_FAIL;
	}
	INFO("ppengine: starting. Cursor keys/joypad/joystick drive, 1/Start = credit, space/X/Y/fire = gear, Tab = debug view, Esc = quit.\n");

	/* Take over (T06): VBlank counter, our interrupt servers (display flip,
	   keyboard), Workbench closed, multitasking off. */
	ApolloTimer();
	interrupts_install();
	ApolloTakeOver();
	ApolloForbid();

	/* Display (T09). As the maggie.library samples: raster and copper DMA
	   off while our chunky screen is up (KB os/libraries/maggie-library.md). */
	old_dma = custom.dmaconr & (DMAF_RASTER | DMAF_COPPER);
	custom.dmacon = DMAF_RASTER | DMAF_COPPER;
	keyboard_open();
	saga_display_show();

	last_vblank = *vblank_counter;
	start_vblank = last_vblank;

	while (!game_input_quit_requested()) {
		ULONG start, cycles;

		uint16_t *buffer;

		ticks = wait_for_vblank(&last_vblank);
		if (ticks > MAX_CATCH_UP_TICKS) {
			dropped_ticks += ticks - MAX_CATCH_UP_TICKS;
			ticks = MAX_CATCH_UP_TICKS;
		}
		while (ticks--) {
			keyboard_poll();
			game_input_apply_tick();
			engine_execute_frame();
			engine_ticks++;
		}
		if (keyboard_take_press(KEY_TAB)) {
			show_debug_view = !show_debug_view;
		}
		update_scene();
		if (sound) {
			audio_update(moving_objects, engine_sound_pitch, engine_sound_active, skidding_volume);
		}
		start = ApolloCPUTick();
		buffer = saga_display_back_buffer();
		if (show_debug_view) {
			debug_view_render(buffer);
		} else {
			scene_backend_set_target(buffer);
			scene_generate_frame();
		}
		saga_display_present(buffer);  /* on screen at the next VBlank */
		cycles = ApolloCPUTick() - start;
		render_cycles += cycles / 64;  /* keeps the sum in 32 bits */
		if (cycles > render_worst) {
			render_worst = cycles;
		}
		within[cycles <= frame_budget ? 0 : cycles <= 2 * frame_budget ? 1 : cycles <= 3 * frame_budget ? 2 : 3]++;
		frames++;
	}

	/* Undo everything in reverse order. */
	if (sound) {
		audio_close();  /* channels off, samples freed */
	}
	interrupts_remove();
	saga_display_restore();
	custom.dmacon = DMAF_SETCLR | old_dma;
	ApolloPermit();
	ApolloRelease();
	ApolloTimerStop();
	saga_display_close();

	INFO("ppengine: %lu frames (%lu shown), %lu engine ticks, %lu ticks dropped, %lu VBlanks.\n",
		(unsigned long)frames, (unsigned long)irq_state.flips, (unsigned long)engine_ticks,
		(unsigned long)dropped_ticks, (unsigned long)(last_vblank - start_vblank));
	if (frames) {
		INFO("ppengine: render %lu ms average, %lu ms worst.\n",
			(unsigned long)(render_cycles / frames * 64 / per_ms),
			(unsigned long)(render_worst / per_ms));
		INFO("ppengine: frames rendered within 1 VBlank %lu%%, 2 %lu%%, 3 %lu%%, more %lu%%.\n",
			(unsigned long)(within[0] * 100 / frames), (unsigned long)(within[1] * 100 / frames),
			(unsigned long)(within[2] * 100 / frames), (unsigned long)(within[3] * 100 / frames));
	}
	return RETURN_OK;
}

int main(int argc, char **argv) {
	int result;
	int first_tick_argument = 0;
	int i;
	ULONG old_sr;

	if (argc == 0) {
		workbench_start();
	}
	if (!has_68080() || !has_fpu()) {
		printf("ppengine needs an Apollo 68080 with its FPU switched on.\n");
		return finish(RETURN_FAIL);
	}
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "LOWRES") == 0) {
			saga_display_select(0);
		} else if (strcmp(argv[i], "CCW") == 0) {
			maggie_backend_set_cull_ccw(1);
		} else if (strcmp(argv[i], "NOBILINEAR") == 0) {
			maggie_backend_set_option(MAGGIE_OPTION_NO_BILINEAR, 1);
		} else if (strcmp(argv[i], "NODEPTH") == 0) {
			maggie_backend_set_option(MAGGIE_OPTION_NO_DEPTH, 1);
		} else if (strcmp(argv[i], "NEARFIRST") == 0) {
			maggie_backend_set_option(MAGGIE_OPTION_NEAR_FIRST, 1);
		} else if (strcmp(argv[i], "DEPTHTEST") == 0) {
			depth_test = 1;
		} else if (strcmp(argv[i], "FULLDETAIL") == 0) {
			maggie_backend_set_option(MAGGIE_OPTION_FULL_DETAIL, 1);
		} else if (strcmp(argv[i], "BENCH") == 0) {
			benchmark = 1;
		} else if (strcmp(argv[i], "AFFINE") == 0) {
			maggie_backend_set_option(MAGGIE_OPTION_AFFINE, 1);
		} else if (strcmp(argv[i], "INPUTTEST") == 0) {
			return run_input_test();
		} else if (strcmp(argv[i], "SOUNDTEST") == 0) {
			/* SOUNDTEST NOFORBID: the same with multitasking on */
			return run_sound_test(!(i + 1 < argc && strcmp(argv[i + 1], "NOFORBID") == 0));
		} else if (strcmp(argv[i], "SNAPSHOT") == 0) {
			first_tick_argument = i + 1;
			break;
		}
	}

	/* maggie.library keeps values in the E registers during a draw; the
	   Apollo bit makes the OS save them on a task switch
	   (KB os/pitfalls/maggie-library-side-effects.md). */
	old_sr = SetSR(SR_APOLLO_BIT, SR_APOLLO_BIT);

	/* While the OS runs: track, assets, maggie buffers (T07). */
	INFO("ppengine: building the track and loading the data...\n");
	engine_initialise();
	engine_register_sound_effect_callback(sound_effect);
	if (!track_geometry_initialise(track_curve_data)) {
		SetSR(old_sr, SR_APOLLO_BIT);
		return finish(RETURN_FAIL);
	}
	if (!scene_open(scenery_objects)) {
		printf("ppengine: cannot start the renderer: %s\n",
			assets_error()[0] ? assets_error() : "cannot open maggie.library");
		track_geometry_terminate();
		SetSR(old_sr, SR_APOLLO_BIT);
		return finish(RETURN_FAIL);
	}

	if (first_tick_argument) {
		result = run_snapshots(argc - first_tick_argument, argv + first_tick_argument);
	} else {
		result = run_game();
	}

	scene_close();
	track_geometry_terminate();
	SetSR(old_sr, SR_APOLLO_BIT);
	return finish(result);
}
