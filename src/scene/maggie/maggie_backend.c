/*
 * Maggie renderer for ppengine (port plan phase 4, T18-T26).
 *
 * Replaces src/scene/osg/: the same scene (road, cars, signs, start gantry,
 * mountains, HUD), drawn into a 320x240 R5G6B5 buffer.
 *
 * Per frame:
 *   1. CPU: sky, mountain strip (scrolled with the camera heading), grass.
 *   2. maggie.library: road and objects, sorted far to near (painter's
 *      order: on Core 12847 Maggie writes the colour even where the depth
 *      test fails, KB core/pitfalls/maggie-depth-test-colour-write.md).
 *   3. CPU: HUD characters and the GAME OVER plate.
 *
 * KB: os/libraries/maggie-library.md, os/pitfalls/maggie-library-side-effects.md
 * (FPCR left at round-to-zero, E registers, Apollo bit),
 * core/pitfalls/maggie-span-tail.md (flush span before exit).
 */

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/Maggie.h>
#include <maggie_vec.h>
#include <maggie_vertex.h>
#include <maggie_flags.h>
#include "scene/backend.h"
#include "scene/maggie/assets.h"
#include "scene/maggie/maggie_backend.h"
#include "track_geometry/track_geometry.h"
#include "track_geometry/segments.h"
#include "platform/amiga/saga_display.h"
#include "ApolloCrossDev_Lib.h"

struct Library *MaggieBase;

/* Camera: as the OpenSceneGraph version (height 4.5, looking level), and its
   default vertical field of view of 30 degrees on a 4:3 screen. */
#define CAMERA_HEIGHT 4.5f
#define FIELD_OF_VIEW_H 39.3f
#define NEAR_PLANE 1.0f
#define FAR_PLANE 1200.0f
#define DRAW_DISTANCE 900.0f

#define ROAD_QUADS 2048
#define ROAD_SEGMENT_STEP 32
#define ROAD_QUADS_BEHIND 6
/* The chequered start line: quad 1 (segments 32-63), just beyond the noses of
   the front row of the grid (their origin is at segment 0, the model reaches
   3.7 units = about 24 segments forward). The OSG version drew it on the
   last quad (segments 65504-65535), so the front row stood past the line.
   scene.c puts the gantry at the same place. */
#define START_LINE_QUAD 1
#define ROAD_QUADS_NEAR 40          /* full-size quads ahead of the player */
#define ROAD_COARSE 4               /* fine quads per coarse quad further on */
#define ROAD_COARSE_QUADS (ROAD_QUADS / ROAD_COARSE)
#define ROAD_COARSE_AHEAD 16        /* coarse quads after the fine ones */

#define SCENERY_SLOTS 25          /* 24 signs + the start gantry */
#define GANTRY_SLOT 24
#define GANTRY_MODEL 10
#define MOVING_OBJECTS 12
#define EXPLOSION_FRAMES 90

/* The 256x224 arcade HUD, centred; doubled at 640x480. */
#define HUD_SCALE (SCREEN_WIDTH / 320)
#define HUD_X0 ((SCREEN_WIDTH - 256 * HUD_SCALE) / 2)
#define HUD_Y0 ((SCREEN_HEIGHT - 224 * HUD_SCALE) / 2)
#define HUD_COLUMNS 32
#define HUD_ROWS 28
#define HUD_BLANK_CHARACTER 24

#define COLOUR_GRASS  RGB565(56, 120, 40)
#define COLOUR_TITLE  RGB565(128, 128, 128)
#define COLOUR_BLACK  0x0000

static const char *const scenery_model_names[11] = {
	"highsign-amusement", "highsign-atari", "highsign-centipede", "highsign-digdug",
	"highsign-leftarrow", "highsign-poleposition", "highsign-rightarrow", "highsign-usa",
	"lowsign-atari", "lowsign-namco", "startline"
};
static const char *const car_model_names[4] = { "car1", "car2", "car3", "car4" };

/* HUD colours and attribute mapping from src/scene/osg/text_matrix.c */
static const UWORD hud_colours[6] = {
	RGB565(255, 255, 255), RGB565(0, 0, 0), RGB565(255, 31, 67),
	RGB565(255, 255, 112), RGB565(143, 255, 112), RGB565(255, 143, 174)
};
static const UBYTE attribute_to_hud_colour[5] = { 0, 0, 3, 4, 5 };

struct placed_object {
	float x, y, rotation;
	int model;          /* index into scenery or car models, or MOVING_OBJECT_MODEL_PUDDLE */
	int visible;
	int explosion;      /* frames of explosion left */
};

struct draw_item {
	float distance;
	struct placed_object *object;
	int is_car;
};

/* Levels of detail (tools/convert_assets.py LOD_CELLS, SCENERY_LOD_CELLS):
   level n is used up to lod_distance[n]. A cell of c units covers one pixel
   at about 450 c units away. Cars: coarse, solid shapes. Signs and gantry:
   thin bars, so cells stay below half a pixel. */
#define LOD_LEVELS 4
static const float car_lod_distance[LOD_LEVELS - 1] = { 30.0f, 70.0f, 150.0f };
/* Signs and gantry: up to which distance the original lattice (level 0) is
   drawn. At 320x240 its bars of about 0.1 units are thinner than a pixel
   beyond about 45 units and break up; at 640x480 that happens beyond about
   90 units (checked on the V4, 2026-11-14). Further away the lattice panels
   are solid (tools/convert_assets.py, solidify_lattices). */
static const float scenery_lod_distance_low[LOD_LEVELS - 1] = { 25.0f, 50.0f, 150.0f };
static const float scenery_lod_distance_high[LOD_LEVELS - 1] = { 90.0f, 150.0f, 250.0f };

static struct model scenery_models[11][LOD_LEVELS];
static struct model car_models[4][LOD_LEVELS];
static int models_loaded;
static UWORD road_vertex_buffer = 0xffff, road_index_buffer = 0xffff;
static UWORD coarse_vertex_buffer = 0xffff, coarse_index_buffer = 0xffff;
static int last_player_quad;
static UWORD road_texture, start_line_texture, explosion_texture, white_texture;
static struct image565 mountains, game_over;
static UBYTE font[96 * 8];

static struct placed_object scenery[SCENERY_SLOTS];
static int scenery_count;
static struct placed_object moving[MOVING_OBJECTS];
static int gantry_lights[4];
static float camera_x, camera_y, target_x, target_y;
static UBYTE hud_character[HUD_ROWS][HUD_COLUMNS];
static UBYTE hud_attribute[HUD_ROWS][HUD_COLUMNS];
static int display_channel = DISPLAY_CHANNEL_IN_GAME;
static int game_over_visible;
static UWORD *target;
static UWORD cull_flag;   /* 0 or MAG_DRAWMODE_CULL_CCW */
static int scene_open;
static int options;

static int load_lods(struct model *lods, const char *name) {
	char lod_name[48];
	int level;

	if (!assets_load_model(&lods[0], name)) {
		return 0;
	}
	for (level = 1; level < LOD_LEVELS; level++) {
		snprintf(lod_name, sizeof(lod_name), "%s_lod%d", name, level);
		if (!assets_load_model(&lods[level], lod_name)) {
			return 0;
		}
	}
	return 1;
}

static const struct model *pick_lod(const struct model *lods, const float *lod_distance, float distance) {
	int level = 0;

	while (!(options & MAGGIE_OPTION_FULL_DETAIL) && level < LOD_LEVELS - 1 && distance > lod_distance[level]) {
		level++;
	}
	return &lods[level];
}

static struct backend_stats stats;

static int prepare_background(void);
static void free_background(void);

/* ------------------------------------------------------------------------ */
/* FPU control register: maggie.library leaves it at round-to-zero. */

static ULONG get_fpcr(void) {
	ULONG value;
	__asm__ volatile ("fmove.l %%fpcr,%0" : "=d" (value));
	return value;
}

static void set_fpcr(ULONG value) {
	__asm__ volatile ("fmove.l %0,%%fpcr" : : "d" (value));
}

/* ------------------------------------------------------------------------ */

void maggie_backend_set_option(int option, int on) {
	options = on ? (options | option) : (options & ~option);
}

void maggie_backend_set_cull_ccw(int ccw) {
	cull_flag = ccw ? MAG_DRAWMODE_CULL_CCW : 0;
}

const struct backend_stats *maggie_backend_stats(void) {
	return &stats;
}

/* One road buffer: quads of `step` segments; v_of(quad) gives the texture
   rows. The OSG version used 2048 quads of 32 segments, 4 quads per texture
   repeat; the coarse buffer joins 4 of them for the far road. */
static int build_road_buffer(int quads, int step, int coarse, UWORD *vertex_buffer, UWORD *index_buffer) {
	struct segment *segments = track_geometry_get_segments();
	struct MaggieVertex *vertices = (struct MaggieVertex *)AllocMem(sizeof(struct MaggieVertex) * quads * 4, MEMF_ANY | MEMF_CLEAR);
	UWORD *indices = (UWORD *)AllocMem(sizeof(UWORD) * (quads * 5 - 1), MEMF_ANY);
	int quad, corner, ok = 0;

	if (vertices && indices) {
		for (quad = 0; quad < quads; quad++) {
			struct segment *a = &segments[quad * step];
			struct segment *b = &segments[((quad + 1) * step) & (NUMBER_OF_SEGMENTS - 1)];
			struct MaggieVertex *v = &vertices[quad * 4];
			float v0, v1;

			if (coarse) {
				v0 = (float)((quad * 2) & 3) / 4.0f;
				v1 = v0 + 0.5f;
			} else if (quad == START_LINE_QUAD) {
				v0 = 0.0f;  /* the start line quad shows its whole texture */
				v1 = 1.0f;
			} else {
				v0 = (float)((quad / 2) & 3) / 4.0f;
				v1 = v0 + 0.25f;
			}
			v[0].pos.x = (float)a->track_left_edge.x;  v[0].pos.y = (float)a->track_left_edge.y;
			v[1].pos.x = (float)a->track_right_edge.x; v[1].pos.y = (float)a->track_right_edge.y;
			v[2].pos.x = (float)b->track_left_edge.x;  v[2].pos.y = (float)b->track_left_edge.y;
			v[3].pos.x = (float)b->track_right_edge.x; v[3].pos.y = (float)b->track_right_edge.y;
			v[0].tex[0].u = 0.0f; v[0].tex[0].v = v0;
			v[1].tex[0].u = 1.0f; v[1].tex[0].v = v0;
			v[2].tex[0].u = 0.0f; v[2].tex[0].v = v1;
			v[3].tex[0].u = 1.0f; v[3].tex[0].v = v1;
			for (corner = 0; corner < 4; corner++) {
				v[corner].normal.z = 1.0f;
				v[corner].colour = 0x00ffffff;
			}
			/* Same order as the OSG version: 2, 3, 1, 0 */
			indices[quad * 5 + 0] = (UWORD)(quad * 4 + 2);
			indices[quad * 5 + 1] = (UWORD)(quad * 4 + 3);
			indices[quad * 5 + 2] = (UWORD)(quad * 4 + 1);
			indices[quad * 5 + 3] = (UWORD)(quad * 4 + 0);
			if (quad < quads - 1) {
				indices[quad * 5 + 4] = 0xffff;
			}
		}
		*vertex_buffer = magAllocateVertexBuffer(quads * 4);
		*index_buffer = magAllocateIndexBuffer(quads * 5 - 1);
		if (*vertex_buffer != 0xffff && *index_buffer != 0xffff) {
			magUploadVertexBuffer(*vertex_buffer, vertices, 0, quads * 4);
			magUploadIndexBuffer(*index_buffer, indices, 0, quads * 5 - 1);
			ok = 1;
		}
	}
	if (vertices) {
		FreeMem(vertices, sizeof(struct MaggieVertex) * quads * 4);
	}
	if (indices) {
		FreeMem(indices, sizeof(UWORD) * (quads * 5 - 1));
	}
	return ok;
}

static int build_road(void) {
	return build_road_buffer(ROAD_QUADS, ROAD_SEGMENT_STEP, 0, &road_vertex_buffer, &road_index_buffer)
		&& build_road_buffer(ROAD_COARSE_QUADS, ROAD_SEGMENT_STEP * ROAD_COARSE, 1,
			&coarse_vertex_buffer, &coarse_index_buffer);
}

int scene_backend_open(void) {
	int i;

	MaggieBase = OpenLibrary((CONST_STRPTR)"maggie.library", 0);
	if (!MaggieBase) {
		return 0;
	}
	if (!assets_open()) {
		scene_backend_close();
		return 0;
	}
	for (i = 0; i < 11; i++) {
		if (!load_lods(scenery_models[i], scenery_model_names[i])) {
			scene_backend_close();
			return 0;
		}
	}
	for (i = 0; i < 4; i++) {
		if (!load_lods(car_models[i], car_model_names[i])) {
			scene_backend_close();
			return 0;
		}
	}
	models_loaded = 1;
	white_texture = assets_texture("");
	road_texture = assets_texture("road");
	start_line_texture = assets_texture("road_startline");
	explosion_texture = assets_texture("explosion");
	if (road_texture == NO_TEXTURE || start_line_texture == NO_TEXTURE || explosion_texture == NO_TEXTURE
			|| !assets_load_image(&mountains, "mountains") || !assets_load_image(&game_over, "gameover")
			|| !assets_load_font(font, sizeof(font)) || !build_road() || !prepare_background()) {
		scene_backend_close();
		return 0;
	}
	memset(hud_character, HUD_BLANK_CHARACTER, sizeof(hud_character));
	memset(hud_attribute, 0, sizeof(hud_attribute));
	memset(moving, 0, sizeof(moving));
	scenery_count = 0;
	scene_open = 1;
	return 1;
}

/* After the last real span, Maggie keeps up to 8 bytes of colour and depth
   and writes them when the next span starts, even in another program. Draw a
   last small triangle into memory that is never freed, so that write lands
   somewhere harmless (KB core/pitfalls/maggie-span-tail.md). */
static void flush_maggie_spans(void) {
	static UWORD *scratch;
	struct MaggieVertex triangle[3];
	mat4 identity, perspective;
	int i;

	if (!scratch) {
		scratch = (UWORD *)AllocMem(32 * 8 * 2 + 32, MEMF_ANY | MEMF_CLEAR);  /* deliberately kept */
		if (!scratch) {
			return;
		}
		scratch = (UWORD *)(((ULONG)scratch + 31) & ~31UL);
	}
	memset(triangle, 0, sizeof(triangle));
	triangle[0].pos.x = -1.0f; triangle[0].pos.y = -1.0f;
	triangle[1].pos.x =  1.0f; triangle[1].pos.y = -1.0f;
	triangle[2].pos.x =  0.0f; triangle[2].pos.y =  1.0f;
	for (i = 0; i < 3; i++) {
		triangle[i].pos.z = 2.0f;
		triangle[i].colour = 0x00ffffff;
	}
	mat4_identity(&identity);
	mat4_perspective(&perspective, 90.0f, 8.0f / 32.0f, 0.5f, 10.0f);
	magBeginScene();
	magSetScreenMemory((APTR *)scratch, 32, 8);
	magSetDrawMode(MAG_DRAWMODE_DEPTHBUFFER);
	magSetTexture(0, white_texture);
	magSetWorldMatrix((float *)&identity);
	magSetViewMatrix((float *)&identity);
	magSetPerspectiveMatrix((float *)&perspective);
	magDrawTrianglesUP(triangle, 3);
	magSetDrawMode(MAG_DRAWMODE_DEPTHBUFFER | MAG_DRAWMODE_CULL_CCW);
	magDrawTrianglesUP(triangle, 3);  /* one of the two windings is a front face */
	magEndScene();
}

void scene_backend_close(void) {
	int i;
	ULONG fpcr = get_fpcr();

	if (MaggieBase) {
		if (scene_open && white_texture != NO_TEXTURE) {
			flush_maggie_spans();
		}
		if (road_vertex_buffer != 0xffff) {
			magFreeVertexBuffer(road_vertex_buffer);
			road_vertex_buffer = 0xffff;
		}
		if (road_index_buffer != 0xffff) {
			magFreeIndexBuffer(road_index_buffer);
			road_index_buffer = 0xffff;
		}
		if (coarse_vertex_buffer != 0xffff) {
			magFreeVertexBuffer(coarse_vertex_buffer);
			coarse_vertex_buffer = 0xffff;
		}
		if (coarse_index_buffer != 0xffff) {
			magFreeIndexBuffer(coarse_index_buffer);
			coarse_index_buffer = 0xffff;
		}
		for (i = 0; i < 11 * LOD_LEVELS; i++) {
			assets_free_model(&scenery_models[i / LOD_LEVELS][i % LOD_LEVELS]);
		}
		for (i = 0; i < 4 * LOD_LEVELS; i++) {
			assets_free_model(&car_models[i / LOD_LEVELS][i % LOD_LEVELS]);
		}
		assets_close();
		CloseLibrary(MaggieBase);
		MaggieBase = NULL;
	}
	free_background();
	assets_free_image(&mountains);
	assets_free_image(&game_over);
	models_loaded = 0;
	scene_open = 0;
	set_fpcr(fpcr);
}

void scene_backend_add_gantry(double x, double y, double rotation) {
	scene_backend_add_scenery_object(x, y, rotation, GANTRY_MODEL);
}

void scene_backend_set_gantry_light_illumination(int light_index, int illuminated) {
	if (light_index >= 0 && light_index < 4) {
		gantry_lights[light_index] = illuminated;
	}
}

void scene_backend_add_scenery_object(double x, double y, double rotation, int model_index) {
	struct placed_object *object;

	if (scenery_count == SCENERY_SLOTS || model_index < 0 || model_index > 10) {
		return;
	}
	object = &scenery[scenery_count++];
	object->x = (float)x;
	object->y = (float)y;
	object->rotation = (float)rotation;
	object->model = model_index;
	object->visible = 1;
	object->explosion = 0;
}

void scene_backend_set_moving_object_attributes(int index, double x, double y, double rotation, int model, int visible) {
	if (index < 0 || index >= MOVING_OBJECTS) {
		return;
	}
	moving[index].x = (float)x;
	moving[index].y = (float)y;
	moving[index].rotation = (float)rotation;
	moving[index].model = model;
	moving[index].visible = visible;
}

void scene_backend_start_car_explosion(int index) {
	if (index >= 0 && index < MOVING_OBJECTS) {
		moving[index].explosion = EXPLOSION_FRAMES;
	}
}

void scene_backend_end_car_explosion(int index) {
	if (index >= 0 && index < MOVING_OBJECTS) {
		moving[index].explosion = 0;
	}
}

void scene_backend_set_camera_position(double cx, double cy, double tx, double ty) {
	camera_x = (float)cx;
	camera_y = (float)cy;
	target_x = (float)tx;
	target_y = (float)ty;
}

void scene_backend_update_hud_character(int x, int y, uint8_t character, uint8_t attribute) {
	if (x >= 0 && x < HUD_COLUMNS && y >= 0 && y < HUD_ROWS) {
		hud_character[y][x] = character;
		hud_attribute[y][x] = attribute;
	}
}

void scene_backend_set_display_channel(int channel) {
	display_channel = channel;
}

void scene_backend_set_game_over_visibility(int visible) {
	game_over_visible = visible;
}

int scene_backend_active(void) {
	return scene_open;
}

void scene_backend_set_target(uint16_t *pixels) {
	target = pixels;
}

/* ------------------------------------------------------------------------ */
/* 2D parts */

static void fill_rect(int x0, int y0, int w, int h, UWORD colour) {
	int x, y;

	for (y = y0; y < y0 + h; y++) {
		UWORD *row = target + y * SCREEN_WIDTH;
		for (x = x0; x < x0 + w; x++) {
			row[x] = colour;
		}
	}
}

/* Whole rows with ApolloLib's AMMX fill (32 bytes per step, 32-byte aligned
   destination; KB toolchain/apollolib/memory-and-blit.md). The buffers are
   32-byte aligned and a row is 640 or 1280 bytes, so every row start is too.
   AMMX needs the Apollo bit, which main() sets. */
static void fill_rows(int y0, int rows, UWORD colour) {
	ULONG value = ((ULONG)colour << 16) | colour;

	ApolloFillBlock((uint8_t *)(target + y0 * SCREEN_WIDTH), (uint32_t)rows * SCREEN_WIDTH * 2, value);
}


/* The mountain strip, prepared once at screen scale: 2048 x 64 becomes about
   5850 x 175 at 640x480, mirrored so that screen x runs with the index.
   Each frame then copies one slice per row (move16) instead of looking up
   every pixel: 12 ms -> about 2 ms at 640x480. The perspective of a
   cylinder (atan) is left out: at the screen edges that is about 4 %. */
static UWORD *wide_strip;
static void *wide_strip_memory;
static ULONG wide_strip_bytes;
static int wide_width, wide_height;
static float wide_per_radian;
static UWORD sky_colour;

static int prepare_background(void) {
	float focal = (SCREEN_WIDTH / 2) / (float)tan(FIELD_OF_VIEW_H * 0.5f * 3.14159265f / 180.0f);
	float strip_per_radian = mountains.width / (2.0f * 3.14159265f);
	float scale = focal / strip_per_radian;   /* screen pixels per strip pixel */
	int x, y;

	wide_width = (int)(mountains.width * scale);
	wide_height = (int)(mountains.height * scale);
	if (wide_height > SCREEN_HEIGHT / 2) {
		wide_height = SCREEN_HEIGHT / 2;
	}
	wide_per_radian = strip_per_radian * scale;
	wide_strip_bytes = (ULONG)wide_width * wide_height * 2 + 32;
	wide_strip_memory = AllocMem(wide_strip_bytes, MEMF_ANY);
	if (!wide_strip_memory) {
		return 0;
	}
	wide_strip = (UWORD *)(((ULONG)wide_strip_memory + 31) & ~31UL);
	/* Sky above the strip: the colour that occurs most in its top row. (The
	   top row itself, carried upwards, drew a wide white bar where a cloud
	   touches the top of the strip; seen on the V4.) */
	{
		int best = 0, i, j;

		sky_colour = mountains.pixels[0];
		for (i = 0; i < mountains.width; i += 8) {
			int count = 0;

			for (j = 0; j < mountains.width; j++) {
				count += mountains.pixels[j] == mountains.pixels[i];
			}
			if (count > best) {
				best = count;
				sky_colour = mountains.pixels[i];
			}
		}
	}
	for (y = 0; y < wide_height; y++) {
		const UWORD *source = mountains.pixels + (y * mountains.height / wide_height) * mountains.width;
		UWORD *row = wide_strip + y * wide_width;

		for (x = 0; x < wide_width; x++) {
			row[x] = source[(int)((wide_width - 1 - x) / scale) % mountains.width];
		}
	}
	return 1;
}

static void free_background(void) {
	if (wide_strip_memory) {
		FreeMem(wide_strip_memory, wide_strip_bytes);
		wide_strip_memory = NULL;
		wide_strip = NULL;
	}
}

/* Copies screen-width pixels starting at column `first` of a wide row,
   wrapping at its end. */
static void copy_wrapped(UWORD *destination, const UWORD *row, int first) {
	int run = wide_width - first;

	if (run >= SCREEN_WIDTH) {
		ApolloCopyBlock((uint8_t *)(row + first), (uint8_t *)destination, (ULONG)SCREEN_WIDTH * 2);
	} else {
		ApolloCopyBlock((uint8_t *)(row + first), (uint8_t *)destination, (ULONG)run * 2);
		ApolloCopyBlock((uint8_t *)row, (uint8_t *)(destination + run), (ULONG)(SCREEN_WIDTH - run) * 2);
	}
}

static void draw_background(float heading) {
	int horizon = SCREEN_HEIGHT / 2;
	int top = horizon - wide_height;
	int first, y;

	/* Mirrored strip: column for screen x = wide_width - 1 - heading * wide_per_radian + (x - W/2) */
	first = (wide_width - 1 - (int)(heading * wide_per_radian) - SCREEN_WIDTH / 2) % wide_width;
	if (first < 0) {
		first += wide_width;
	}
	if (top > 0) {
		fill_rows(0, top, sky_colour);
	}
	for (y = top; y < horizon; y++) {
		copy_wrapped(target + y * SCREEN_WIDTH, wide_strip + (y - top) * wide_width, first);
	}
	fill_rows(horizon, SCREEN_HEIGHT - horizon, COLOUR_GRASS);
}

static int hud_colour_index(int attribute) {
	int index = 0;

	if (attribute > 0 && attribute < 6) {
		index = attribute_to_hud_colour[attribute - 1];
	} else if (attribute >= 0x20 && attribute <= 0x26) {
		index = attribute - 0x20;
	}
	return index < 6 ? index : 0;
}

static void draw_hud(void) {
	int row, column, y, x;
	int scale = HUD_SCALE;

	for (row = 0; row < HUD_ROWS; row++) {
		for (column = 0; column < HUD_COLUMNS; column++) {
			int character = hud_character[row][column];
			const UBYTE *glyph;
			UWORD colour;

			if (character == HUD_BLANK_CHARACTER || character <= 32 || character > 127) {
				continue;
			}
			glyph = &font[(character - 32) * 8];
			colour = hud_colours[hud_colour_index(hud_attribute[row][column])];
			if (scale == 2) {
				/* each font pixel as 2x2, as one 32-bit store per row pair */
				ULONG pair = ((ULONG)colour << 16) | colour;

				for (y = 0; y < 8; y++) {
					UWORD *pixel = target + (HUD_Y0 + (row * 8 + y) * 2) * SCREEN_WIDTH + HUD_X0 + column * 16;
					UBYTE bits = glyph[y];

					for (x = 0; x < 8; x++, bits <<= 1) {
						if (bits & 0x80) {
							*(ULONG *)(pixel + x * 2) = pair;
							*(ULONG *)(pixel + SCREEN_WIDTH + x * 2) = pair;
						}
					}
				}
				continue;
			}
			for (y = 0; y < 8; y++) {
				UWORD *pixel = target + (HUD_Y0 + row * 8 + y) * SCREEN_WIDTH + HUD_X0 + column * 8;
				UBYTE bits = glyph[y];

				for (x = 0; x < 8; x++) {
					if (bits & (0x80 >> x)) {
						pixel[x] = colour;
					}
				}
			}
		}
	}
}

/* The GAME OVER plate: x 0.23-0.77, y 0.29-0.35 of the HUD, as in
   src/scene/osg/scene.c; scaled with the colour key. */
static void draw_game_over(void) {
	int x0 = HUD_X0 + (int)(0.23f * 256 * HUD_SCALE);
	int x1 = HUD_X0 + (int)(0.77f * 256 * HUD_SCALE);
	int y0 = HUD_Y0 + (int)((1.0f - 0.35f) * 224 * HUD_SCALE);
	int y1 = HUD_Y0 + (int)((1.0f - 0.29f) * 224 * HUD_SCALE);
	static UWORD source_column[MAX_SCREEN_WIDTH];
	int x, y;

	for (x = x0; x < x1; x++) {
		source_column[x - x0] = (UWORD)((x - x0) * game_over.width / (x1 - x0));
	}
	for (y = y0; y < y1; y++) {
		const UWORD *source = game_over.pixels + ((y - y0) * game_over.height / (y1 - y0)) * game_over.width;
		UWORD *row = target + y * SCREEN_WIDTH + x0;
		int width = x1 - x0;

		for (x = 0; x < width; x++) {
			UWORD pixel = source[source_column[x]];
			if (pixel != game_over.key) {
				row[x] = pixel;
			}
		}
	}
}

/* ------------------------------------------------------------------------ */
/* 3D parts */

static void make_world_matrix(mat4 *m, float x, float y, float rotation) {
	float c = (float)cos(rotation);
	float s = (float)sin(rotation);

	mat4_identity(m);
	/* Row vectors (v * M), as OSG and maggie_vec.h: rotate about z, then move. */
	m->m[0][0] = c;  m->m[0][1] = s;
	m->m[1][0] = -s; m->m[1][1] = c;
	m->m[3][0] = x;  m->m[3][1] = y;
}

/* View space: x right, y down (the screen's y), z forward. */
static void make_view_matrix(mat4 *m, float *forward_x, float *forward_y) {
	float fx = target_x - camera_x;
	float fy = target_y - camera_y;
	float length = (float)sqrt(fx * fx + fy * fy);
	float rx, ry;

	if (length < 0.0001f) {
		fx = 1.0f;
		fy = 0.0f;
	} else {
		fx /= length;
		fy /= length;
	}
	rx = fy;      /* right = forward x up */
	ry = -fx;
	mat4_identity(m);
	m->m[0][0] = rx;   m->m[0][1] = 0.0f;  m->m[0][2] = fx;
	m->m[1][0] = ry;   m->m[1][1] = 0.0f;  m->m[1][2] = fy;
	m->m[2][0] = 0.0f; m->m[2][1] = -1.0f; m->m[2][2] = 0.0f;
	m->m[3][0] = -(camera_x * rx + camera_y * ry);
	m->m[3][1] = CAMERA_HEIGHT;
	m->m[3][2] = -(camera_x * fx + camera_y * fy);
	*forward_x = fx;
	*forward_y = fy;
}

/* Draws quads [first, first + count) of a road buffer, wrapping at the end. */
static void draw_road_run(int quads, int first, int count, int start_line_quad) {
	int done = 0;

	while (done < count) {
		int start = (first + done) % quads;
		int run = count - done;

		if (start == start_line_quad) {
			magSetTexture(0, start_line_texture);
			magDrawIndexedPolygons((UWORD)(start * 4), 4, (UWORD)(start * 5), 4);
			stats.road_quads++;
			done++;
			continue;
		}
		if (start + run > quads) {
			run = quads - start;
		}
		if (start_line_quad > start && start + run > start_line_quad) {
			run = start_line_quad - start;
		}
		magSetTexture(0, road_texture);
		magDrawIndexedPolygons((UWORD)(start * 4), (UWORD)(run * 4), (UWORD)(start * 5), (UWORD)(run * 5 - 1));
		stats.road_quads += run;
		done += run;
	}
}

static void draw_road(UWORD mode, int player_quad) {
	int first = (player_quad - ROAD_QUADS_BEHIND + ROAD_QUADS) % ROAD_QUADS;
	/* Fine quads up to a multiple of ROAD_COARSE, then coarse ones. */
	int fine_end = ((player_quad + ROAD_QUADS_NEAR) / ROAD_COARSE) * ROAD_COARSE;
	int fine_count = fine_end - (player_quad - ROAD_QUADS_BEHIND);
	mat4 identity;

	mat4_identity(&identity);
	magSetWorldMatrix((float *)&identity);
	magSetDrawMode(mode);
	magSetRGB(0x00ffffff);
	magSetVertexBuffer(road_vertex_buffer);
	magSetIndexBuffer(road_index_buffer);
	draw_road_run(ROAD_QUADS, first, fine_count, START_LINE_QUAD);
	magSetVertexBuffer(coarse_vertex_buffer);
	magSetIndexBuffer(coarse_index_buffer);
	draw_road_run(ROAD_COARSE_QUADS, (fine_end / ROAD_COARSE) % ROAD_COARSE_QUADS, ROAD_COARSE_AHEAD, -1);
}

static int is_light_part(const char *name, int *light, int *on) {
	/* "Light1On" ... "Light4Off" */
	if (strncmp(name, "Light", 5) != 0 || name[5] < '1' || name[5] > '4') {
		return 0;
	}
	*light = name[5] - '1';
	*on = (strcmp(name + 6, "On") == 0);
	return 1;
}

static void draw_model(const struct model *model, const struct placed_object *object, UWORD mode, int is_gantry) {
	mat4 world;
	int i;

	make_world_matrix(&world, object->x, object->y, object->rotation);
	magSetWorldMatrix((float *)&world);
	magSetDrawMode(mode);
	for (i = 0; i < model->part_count; i++) {
		const struct model_part *part = &model->parts[i];
		int light, on;

		if (is_gantry && is_light_part(part->name, &light, &on) && on != gantry_lights[light]) {
			continue;
		}
		magSetTexture(0, part->texture);
		magSetRGB(part->rgb);
		magSetVertexBuffer(part->vertex_buffer);
		magSetIndexBuffer(part->index_buffer);
		magDrawIndexedPolygons(0, part->vertex_count, 0, part->index_count);
		stats.parts++;
	}
}

static void draw_puddle(const struct placed_object *object, UWORD mode) {
	static struct MaggieVertex quad[4];
	static UWORD indices[4] = { 0, 1, 2, 3 };
	mat4 world;
	int i;

	memset(quad, 0, sizeof(quad));
	quad[0].pos.x = -3.0f; quad[0].pos.y = -2.0f;
	quad[1].pos.x =  3.0f; quad[1].pos.y = -2.0f;
	quad[2].pos.x =  3.0f; quad[2].pos.y =  2.0f;
	quad[3].pos.x = -3.0f; quad[3].pos.y =  2.0f;
	for (i = 0; i < 4; i++) {
		quad[i].pos.z = 0.05f;
		quad[i].normal.z = 1.0f;
		quad[i].colour = 0x00ffffff;
	}
	make_world_matrix(&world, object->x, object->y, object->rotation);
	magSetWorldMatrix((float *)&world);
	magSetDrawMode(mode);
	magSetTexture(0, white_texture);
	magSetRGB(0x00304060);
	magDrawIndexedPolygonsUP(quad, 4, indices, 4);
}

static void draw_explosion(const struct placed_object *object) {
	struct MaggieSpriteVertex sprite;
	mat4 identity;
	float size = 4.0f + 6.0f * (float)(EXPLOSION_FRAMES - object->explosion) / EXPLOSION_FRAMES;

	mat4_identity(&identity);
	magSetWorldMatrix((float *)&identity);
	magSetDrawMode(MAG_DRAWMODE_AFFINE_MAPPING | cull_flag);
	magSetTexture(0, explosion_texture);
	magSetRGB(0x00ffffff);
	sprite.pos.x = object->x;
	sprite.pos.y = object->y;
	sprite.pos.z = 2.0f;
	sprite.colour = 0x00ffffff;
	magDrawSpritesUP(&sprite, 1, size);
}

static void draw_scene_3d(void) {
	struct draw_item items[SCENERY_SLOTS + MOVING_OBJECTS];
	int item_count = 0;
	mat4 view, perspective;
	float forward_x, forward_y;
	UWORD mode = cull_flag;
	int player_quad;
	int i, j;
	ULONG t0, t1;

	if (!(options & MAGGIE_OPTION_NO_DEPTH)) {
		mode |= MAG_DRAWMODE_DEPTHBUFFER;
	}
	if (!(options & MAGGIE_OPTION_NO_BILINEAR)) {
		mode |= MAG_DRAWMODE_BILINEAR;
	}
	if (options & MAGGIE_OPTION_AFFINE) {
		mode |= MAG_DRAWMODE_AFFINE_MAPPING;
	}
	make_view_matrix(&view, &forward_x, &forward_y);
	mat4_perspective(&perspective, FIELD_OF_VIEW_H, (float)SCREEN_HEIGHT / SCREEN_WIDTH, NEAR_PLANE, FAR_PLANE);

	/* Which road quad is under the camera's target (the player car)? Search
	   near last frame's answer; the whole track when that is far off. */
	{
		struct segment *segments = track_geometry_get_segments();
		int best = last_player_quad, offset, range = 64;
		float best_distance = 1e30f;

		for (;;) {
			for (offset = -range; offset <= range; offset++) {
				int quad = (last_player_quad + offset + ROAD_QUADS) % ROAD_QUADS;
				struct segment *seg = &segments[quad * ROAD_SEGMENT_STEP];
				float dx = (float)seg->centre.x - target_x;
				float dy = (float)seg->centre.y - target_y;
				float d = dx * dx + dy * dy;

				if (d < best_distance) {
					best_distance = d;
					best = quad;
				}
			}
			if (best_distance < 400.0f || range >= ROAD_QUADS / 2) {
				break;
			}
			range = ROAD_QUADS / 2;
		}
		player_quad = best;
		last_player_quad = best;
	}

	t0 = ApolloCPUTick();
	magBeginScene();
	magSetScreenMemory((APTR *)target, SCREEN_WIDTH, SCREEN_HEIGHT);
	magSetDrawMode(mode);
	if (mode & MAG_DRAWMODE_DEPTHBUFFER) {
		magClearDepth(0xffff);
		magClear(MAG_CLEAR_DEPTH);
	}
	magSetViewMatrix((float *)&view);
	magSetPerspectiveMatrix((float *)&perspective);

	t1 = ApolloCPUTick();
	/* The road without the depth buffer: it is flat, drawn first and lies
	   under everything else, so it needs no depth test. With bilinear
	   filtering and the depth buffer both on, Maggie drew short red/white
	   lines along the seams between road quads (Core 12856, 2026-11-14;
	   either one alone was clean). */
	draw_road(mode & ~MAG_DRAWMODE_DEPTHBUFFER, player_quad);
	stats.road_cycles = ApolloCPUTick() - t1;
	stats.setup_cycles = t1 - t0;
	stats.car_cycles = 0;
	stats.scenery_cycles = 0;

	/* Objects in front of the camera and within range, far to near. */
	for (i = 0; i < scenery_count + MOVING_OBJECTS; i++) {
		struct placed_object *object = (i < scenery_count) ? &scenery[i] : &moving[i - scenery_count];
		int is_car = (i >= scenery_count);
		const struct model *model;
		float dx, dy, ahead, distance, radius;

		if (!object->visible && !object->explosion) {
			continue;
		}
		if (is_car) {
			model = (object->model >= 0 && object->model < 4) ? &car_models[object->model][0] : NULL;
		} else {
			model = &scenery_models[object->model][0];
		}
		radius = model ? model->radius : 4.0f;
		dx = object->x - camera_x;
		dy = object->y - camera_y;
		ahead = dx * forward_x + dy * forward_y;
		if (ahead < -radius) {
			continue;
		}
		distance = (float)sqrt(dx * dx + dy * dy);
		if (distance - radius > DRAW_DISTANCE) {
			continue;
		}
		/* Insertion sort, farthest first. */
		for (j = item_count; j > 0 && items[j - 1].distance < distance; j--) {
			items[j] = items[j - 1];
		}
		items[j].distance = distance;
		items[j].object = object;
		items[j].is_car = is_car;
		item_count++;
	}

	if (options & MAGGIE_OPTION_NEAR_FIRST) {
		for (i = 0; i < item_count / 2; i++) {
			struct draw_item swap = items[i];
			items[i] = items[item_count - 1 - i];
			items[item_count - 1 - i] = swap;
		}
	}
	for (i = 0; i < item_count; i++) {
		struct placed_object *object = items[i].object;

		t0 = ApolloCPUTick();
		if (object->visible) {
			if (items[i].is_car) {
				if (object->model == MOVING_OBJECT_MODEL_PUDDLE) {
					draw_puddle(object, mode);
				} else if (object->model >= 0 && object->model < 4) {
					draw_model(pick_lod(car_models[object->model], car_lod_distance, items[i].distance), object, mode, 0);
				}
			} else {
				draw_model(pick_lod(scenery_models[object->model],
					SCREEN_WIDTH >= 640 ? scenery_lod_distance_high : scenery_lod_distance_low,
					items[i].distance), object, mode,
					object == &scenery[GANTRY_SLOT]);
			}
			stats.objects++;
		}
		if (object->explosion) {
			draw_explosion(object);
		}
		if (items[i].is_car) {
			stats.car_cycles += ApolloCPUTick() - t0;
		} else {
			stats.scenery_cycles += ApolloCPUTick() - t0;
		}
	}
	magEndScene();
}

/* Depth-test check (KB core/pitfalls/maggie-depth-test-colour-write.md):
   on a plain background, draw a red-tinted car 12 units ahead of the camera
   target first, then a green-tinted car 8 units further behind it on the same
   line, with the depth buffer on. If the green car shows over the red one,
   Maggie still writes colour where the depth test fails. */
void maggie_backend_depth_test(void) {
	mat4 view, perspective;
	float forward_x, forward_y;
	UWORD mode = MAG_DRAWMODE_DEPTHBUFFER | cull_flag;
	struct placed_object car;
	const struct model *model = &car_models[0][0];
	ULONG fpcr = get_fpcr();
	int pass, i;

	fill_rows(0, SCREEN_HEIGHT, COLOUR_GRASS);
	make_view_matrix(&view, &forward_x, &forward_y);
	mat4_perspective(&perspective, FIELD_OF_VIEW_H, (float)SCREEN_HEIGHT / SCREEN_WIDTH, NEAR_PLANE, FAR_PLANE);
	magBeginScene();
	magSetScreenMemory((APTR *)target, SCREEN_WIDTH, SCREEN_HEIGHT);
	magSetDrawMode(mode);
	magClearDepth(0xffff);
	magClear(MAG_CLEAR_DEPTH);
	magSetViewMatrix((float *)&view);
	magSetPerspectiveMatrix((float *)&perspective);
	for (pass = 0; pass < 2; pass++) {
		float ahead = pass == 0 ? 12.0f : 20.0f;
		mat4 world;

		car.x = target_x + forward_x * ahead;
		car.y = target_y + forward_y * ahead;
		car.rotation = (float)atan2(forward_y, forward_x);
		make_world_matrix(&world, car.x, car.y, car.rotation);
		magSetWorldMatrix((float *)&world);
		magSetDrawMode(mode);
		for (i = 0; i < model->part_count; i++) {
			magSetTexture(0, white_texture);
			magSetRGB(pass == 0 ? 0x00ff0000 : 0x0000ff00);
			magSetVertexBuffer(model->parts[i].vertex_buffer);
			magSetIndexBuffer(model->parts[i].index_buffer);
			magDrawIndexedPolygons(0, model->parts[i].vertex_count, 0, model->parts[i].index_count);
		}
	}
	magEndScene();
	set_fpcr(fpcr);
}

/* Measurement: draw the player car at every level of detail, as it stands. */
void maggie_backend_benchmark(unsigned long *cycles_per_level, int *polygons_per_level, int *parts_per_level) {
	mat4 view, perspective;
	float forward_x, forward_y;
	UWORD mode = MAG_DRAWMODE_DEPTHBUFFER | MAG_DRAWMODE_BILINEAR | cull_flag;
	ULONG fpcr = get_fpcr();
	int level, i;

	make_view_matrix(&view, &forward_x, &forward_y);
	mat4_perspective(&perspective, FIELD_OF_VIEW_H, (float)SCREEN_HEIGHT / SCREEN_WIDTH, NEAR_PLANE, FAR_PLANE);
	for (level = 0; level < LOD_LEVELS; level++) {
		const struct model *model = &car_models[0][level];
		ULONG start;

		polygons_per_level[level] = 0;
		for (i = 0; i < model->part_count; i++) {
			polygons_per_level[level] += model->parts[i].polygon_count;
		}
		parts_per_level[level] = model->part_count;
		magBeginScene();
		magSetScreenMemory((APTR *)target, SCREEN_WIDTH, SCREEN_HEIGHT);
		magSetDrawMode(mode);
		magSetViewMatrix((float *)&view);
		magSetPerspectiveMatrix((float *)&perspective);
		start = ApolloCPUTick();
		for (i = 0; i < 10; i++) {
			draw_model(model, &moving[11], mode, 0);
		}
		cycles_per_level[level] = (ApolloCPUTick() - start) / 10;
		magEndScene();
	}
	set_fpcr(fpcr);
}

void scene_backend_generate_frame(void) {
	ULONG fpcr, t0, t1, t2, t3;
	int i;

	if (!target || !scene_open) {
		return;
	}
	stats.objects = 0;
	stats.parts = 0;
	stats.road_quads = 0;

	if (display_channel == DISPLAY_CHANNEL_TITLE_SCREEN) {
		fill_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOUR_BLACK);
		fill_rect(HUD_X0, HUD_Y0, 256 * HUD_SCALE, 224 * HUD_SCALE, COLOUR_TITLE);
		return;
	}

	t0 = ApolloCPUTick();
	draw_background((float)atan2(target_y - camera_y, target_x - camera_x));
	t1 = ApolloCPUTick();
	fpcr = get_fpcr();
	draw_scene_3d();
	set_fpcr(fpcr);  /* maggie.library leaves round-to-zero behind */
	t2 = ApolloCPUTick();
	draw_hud();
	if (game_over_visible) {
		draw_game_over();
	}
	t3 = ApolloCPUTick();
	stats.background_cycles = t1 - t0;
	stats.scene_cycles = t2 - t1;
	stats.hud_cycles = t3 - t2;
	for (i = 0; i < MOVING_OBJECTS; i++) {
		if (moving[i].explosion > 0) {
			moving[i].explosion--;
		}
	}
}
