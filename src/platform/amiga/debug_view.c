/*
 * Debug view for phase 1: shows that the engine runs on the V4 before the
 * Maggie renderer exists (phase 4). Left: the track seen from above with
 * every visible moving object. Right: bars for speed, gear, game state and
 * credits.
 */

#include "platform/amiga/debug_view.h"
#include "platform/amiga/saga_display.h"
#include "track_geometry/track_geometry.h"
#include "track_geometry/segments.h"
#include "ppengine/engine.h"

#define MAP_SIZE 232
#define MAP_X0 4
#define MAP_Y0 4
#define PANEL_X 248

#define COLOUR_BACKGROUND RGB565(16, 48, 16)
#define COLOUR_TRACK      RGB565(150, 150, 150)
#define COLOUR_START      RGB565(255, 255, 255)
#define COLOUR_PANEL      RGB565(24, 24, 40)
#define COLOUR_PLAYER     RGB565(255, 230, 0)
#define COLOUR_PUDDLE     RGB565(0, 200, 255)
#define COLOUR_BAR_EMPTY  RGB565(60, 60, 60)
#define COLOUR_SPEED      RGB565(0, 220, 0)
#define COLOUR_GEAR_LOW   RGB565(220, 120, 0)
#define COLOUR_GEAR_HIGH  RGB565(255, 40, 40)
#define COLOUR_STATE      RGB565(120, 120, 255)
#define COLOUR_CREDIT     RGB565(255, 255, 255)

static const uint16_t car_colours[4] = {
	RGB565(255, 40, 40), RGB565(60, 120, 255), RGB565(255, 255, 255), RGB565(255, 0, 255)
};

static uint16_t background[MAX_SCREEN_WIDTH * MAX_SCREEN_HEIGHT];
static double map_min_x, map_min_y, map_scale;

static void put_pixel(uint16_t *buffer, int x, int y, uint16_t colour) {
	if (x >= 0 && x < SCREEN_WIDTH && y >= 0 && y < SCREEN_HEIGHT) {
		buffer[y * SCREEN_WIDTH + x] = colour;
	}
}

static void fill_rect(uint16_t *buffer, int x, int y, int w, int h, uint16_t colour) {
	int i, j;

	for (j = 0; j < h; j++) {
		for (i = 0; i < w; i++) {
			put_pixel(buffer, x + i, y + j, colour);
		}
	}
}

static void world_to_map(double wx, double wy, int *x, int *y) {
	*x = MAP_X0 + (int)((wx - map_min_x) * map_scale);
	*y = MAP_Y0 + MAP_SIZE - 1 - (int)((wy - map_min_y) * map_scale);
}

void debug_view_init(void) {
	struct segment *segments = track_geometry_get_segments();
	double max_x, max_y, span;
	int i, x, y;

	map_min_x = max_x = segments[0].centre.x;
	map_min_y = max_y = segments[0].centre.y;
	for (i = 1; i < NUMBER_OF_SEGMENTS; i++) {
		double sx = segments[i].centre.x;
		double sy = segments[i].centre.y;

		if (sx < map_min_x) map_min_x = sx;
		if (sx > max_x) max_x = sx;
		if (sy < map_min_y) map_min_y = sy;
		if (sy > max_y) max_y = sy;
	}
	span = (max_x - map_min_x) > (max_y - map_min_y) ? (max_x - map_min_x) : (max_y - map_min_y);
	map_scale = (MAP_SIZE - 1) / span;

	for (i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
		background[i] = COLOUR_BACKGROUND;
	}
	fill_rect(background, PANEL_X - 4, 0, SCREEN_WIDTH - PANEL_X + 4, SCREEN_HEIGHT, COLOUR_PANEL);
	for (i = 0; i < NUMBER_OF_SEGMENTS; i += 16) {
		world_to_map(segments[i].centre.x, segments[i].centre.y, &x, &y);
		put_pixel(background, x, y, COLOUR_TRACK);
		put_pixel(background, x + 1, y, COLOUR_TRACK);
		put_pixel(background, x, y + 1, COLOUR_TRACK);
	}
	world_to_map(segments[0].centre.x, segments[0].centre.y, &x, &y);
	fill_rect(background, x - 2, y - 2, 5, 5, COLOUR_START);
}

static void draw_bar(uint16_t *buffer, int y, int value, int maximum, uint16_t colour) {
	int width = SCREEN_WIDTH - PANEL_X - 8;
	int filled;

	if (value < 0) value = 0;
	if (value > maximum) value = maximum;
	filled = value * width / maximum;
	fill_rect(buffer, PANEL_X, y, width, 10, COLOUR_BAR_EMPTY);
	fill_rect(buffer, PANEL_X, y, filled, 10, colour);
}

void debug_view_render(uint16_t *buffer) {
	uint32_t *source = (uint32_t *)background;
	uint32_t *destination = (uint32_t *)buffer;
	int i;

	for (i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT / 2; i++) {
		destination[i] = source[i];
	}

	/* Moving objects, as src/scene/scene.c decides visibility and model. */
	for (i = 0; i < 12; i++) {
		struct movingobject *object = &moving_objects[i];
		int type = object->type_and_visibility & 0xf;
		uint16_t colour;
		struct vertex2d world;
		uint32_t object_forward_position;
		int x, y, size;

		if (!((type >= 1 && type <= 4) || type == 0x7 || type == 0xa)) {
			continue;
		}
		if ((object->type_and_visibility >> 8) == 0) {
			continue;
		}
		if (i == PLAYER_CAR_OBJECT_INDEX) {
			colour = COLOUR_PLAYER;
			size = 5;
		} else if (type == 0x7) {
			colour = COLOUR_PUDDLE;
			size = 3;
		} else {
			colour = car_colours[(object->display_variant & 0xc) >> 2];
			size = 3;
		}
		object_forward_position = forward_position + ((uint32_t)(uint16_t)object->player_forward_position_difference << 16);
		world = track_geometry_get_object_world_coordinates(object_forward_position, object->sideways_position);
		world_to_map(world.x, world.y, &x, &y);
		fill_rect(buffer, x - size / 2, y - size / 2, size, size, colour);
	}

	/* Panel: speed, gear, game state, credits. */
	draw_bar(buffer, 8, current_speed_8_bit, 255, COLOUR_SPEED);
	fill_rect(buffer, PANEL_X, 24, 20, 10, gear_binary ? COLOUR_GEAR_HIGH : COLOUR_GEAR_LOW);
	draw_bar(buffer, 40, jump_table_function_number, 8, COLOUR_STATE);
	draw_bar(buffer, 54, jump_table_subfunction_number, 16, COLOUR_STATE);
	for (i = 0; i < (int)credits_remaining && i < 8; i++) {
		fill_rect(buffer, PANEL_X + i * 8, 70, 6, 6, COLOUR_CREDIT);
	}
}
