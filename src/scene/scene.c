#include "scene/scene.h"
#include "track_geometry/track_geometry.h"
#include "scene/backend.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

/* Logging to the console is too slow under an os-takeover. */
#ifdef SCENE_DEBUG
#define SCENE_LOG(...) printf(__VA_ARGS__)
#else
#define SCENE_LOG(...) do { } while (0)
#endif

#define DISPLAY_MODE_BLANK 0
#define DISPLAY_MODE_TITLE_SCREEN 1
#define DISPLAY_MODE_DEMO 2
#define DISPLAY_MODE_IN_GAME 3

static int previous_car_object_type_and_visibility[12];
static int explosion_detection_enabled=0;
static struct hudentry previous_frame_hud_content[HUD_CONTENT_LENGTH];
static int previous_gantry_and_starting_lights_appearance;
static int old_display_mode=DISPLAY_MODE_BLANK;

static void update_scene_graph_from_display_mode(int display_mode);

int scene_open(struct sceneryobject *scenery_objects) {
	int index;
	struct sceneryobject *current_scenery_object;
	struct vertex2d world_coordinates;
	double rotation_radians;

	current_scenery_object=scenery_objects;
	if (!scene_backend_open()) {
		return 0;
	}
	for (index=0; index<24; index++) {
		world_coordinates=track_geometry_get_object_world_coordinates((uint32_t)current_scenery_object->forward_position<<16,current_scenery_object->sideways_position);
		rotation_radians=track_geometry_get_object_natural_rotation_radians((uint32_t)current_scenery_object->forward_position<<16);
		scene_backend_add_scenery_object(world_coordinates.x,world_coordinates.y,rotation_radians,current_scenery_object->scenery_appearance);
		current_scenery_object++;
	}
	/* Gantry over the start line, just beyond the front row of the grid
	   (segment 32; the OSG version used segment 65504, behind the front row:
	   see START_LINE_QUAD in scene/maggie/maggie_backend.c). */
	world_coordinates=track_geometry_get_object_world_coordinates(0x00200000,0);
	rotation_radians=track_geometry_get_object_natural_rotation_radians(0x00200000);
	scene_backend_add_gantry(world_coordinates.x,world_coordinates.y,rotation_radians);
	for (index=0; index<4; index++) {
		scene_backend_set_gantry_light_illumination(index,0);
	}
	explosion_detection_enabled=0;
	return 1;
}

void scene_close() {
	scene_backend_close();
}

void scene_generate_frame() {
	scene_backend_generate_frame();
}

void scene_update_moving_objects_and_camera(struct movingobject *moving_objects, uint32_t player_forward_position, int16_t steering_or_car_orientation) {
	int type;
	int visible;
	struct vertex2d world_coordinates;
	struct vertex2d camera_coordinates;
	struct movingobject *current_object;
	int object_index;
	int model=0;
	double natural_rotation_radians;
	double steering_adjusted_rotation_radians;
	uint32_t object_forward_position;
	int paint_job;

	current_object=moving_objects;
	object_index=0;

	for (object_index=0; object_index<=11; object_index++) {
		type=current_object->type_and_visibility&0xf;
		if ((type>=1 && type<=4) || type==0x7 || type==0xa) {
			visible=(current_object->type_and_visibility>>8)>0;
			switch(type) {
				case 0x1:
				case 0x4:
					model=0;
					if (explosion_detection_enabled) {
						if ((current_object->type_and_visibility==0x8004) && (previous_car_object_type_and_visibility[object_index]!=0x8004)) {
							SCENE_LOG("car at object index %d changed from %x to %x - starting explosion\n",object_index,previous_car_object_type_and_visibility[object_index],current_object->type_and_visibility);
							scene_backend_start_car_explosion(object_index);
						} else if ((previous_car_object_type_and_visibility[object_index]==0x8004) && (current_object->type_and_visibility!=0x8004)) {
							SCENE_LOG("car at object index %d changed from %x to %x - ending explosion\n",object_index,previous_car_object_type_and_visibility[object_index],current_object->type_and_visibility);
							scene_backend_end_car_explosion(object_index);
						}
					}
					break;
				case 0x2:
				case 0x3:
					paint_job=current_object->display_variant&0xc;
					if (paint_job==0x0) {
						model=0;
					} else if (paint_job==0x4) {
						model=1;
					} else if (paint_job==0x8) {
						model=2;
					} else {
						model=3;
					}
					if (explosion_detection_enabled) {
						if ((current_object->type_and_visibility==0x8003) && (previous_car_object_type_and_visibility[object_index]!=0x8003)) {
							SCENE_LOG("car at object index %d changed from %x to %x - starting explosion\n",object_index,previous_car_object_type_and_visibility[object_index],current_object->type_and_visibility);
							scene_backend_start_car_explosion(object_index);
						} else if ((previous_car_object_type_and_visibility[object_index]==0x8003) && (current_object->type_and_visibility!=0x8003)) {
							SCENE_LOG("car at object index %d changed from %x to %x - ending explosion\n",object_index,previous_car_object_type_and_visibility[object_index],current_object->type_and_visibility);
							scene_backend_end_car_explosion(object_index);
						}
					}
					break;
				case 0x7:
					model=MOVING_OBJECT_MODEL_PUDDLE; // the OSG version showed car model 1 here
					break;
				case 0xa:
					model=0;
					break;
			}
		} else {
			model=0;
			visible=0;
		}

		previous_car_object_type_and_visibility[object_index]=current_object->type_and_visibility;
		object_forward_position=player_forward_position+(current_object->player_forward_position_difference<<16);

		world_coordinates=track_geometry_get_object_world_coordinates(object_forward_position, current_object->sideways_position);
		natural_rotation_radians=track_geometry_get_object_natural_rotation_radians(object_forward_position);
		steering_adjusted_rotation_radians=natural_rotation_radians;
		if (object_index==11) {
			steering_adjusted_rotation_radians=natural_rotation_radians+((float)steering_or_car_orientation/400);
		}

		scene_backend_set_moving_object_attributes(
			object_index,
			world_coordinates.x,
			world_coordinates.y,
			steering_adjusted_rotation_radians,
			model,
			visible
		);

		current_object++;
	}

	current_object=&moving_objects[11];
	world_coordinates=track_geometry_get_object_world_coordinates(player_forward_position, current_object->sideways_position);
	camera_coordinates.x=world_coordinates.x-cos(natural_rotation_radians)*22;
	camera_coordinates.y=world_coordinates.y-sin(natural_rotation_radians)*22;

	scene_backend_set_camera_position(
		camera_coordinates.x,
		camera_coordinates.y,
		world_coordinates.x,
		world_coordinates.y
	);

	explosion_detection_enabled=1;
}

void scene_update_gantry(int gantry_and_starting_lights_appearance) {
	int index;

	if (gantry_and_starting_lights_appearance!=previous_gantry_and_starting_lights_appearance) {
		SCENE_LOG("gantry appearance changed to %d\n",gantry_and_starting_lights_appearance);
		if (gantry_and_starting_lights_appearance>=0 && gantry_and_starting_lights_appearance<=4) {
			for (index=0; index<4; index++) {
				scene_backend_set_gantry_light_illumination(index,(index+1)==gantry_and_starting_lights_appearance);
			}
		}
	}
	previous_gantry_and_starting_lights_appearance=gantry_and_starting_lights_appearance;
}

void scene_update_hud_content(struct hudentry *hud_content) {
	int hud_x,hud_y;
	struct hudentry *current_hud_entry;
	struct hudentry *current_last_frame_hud_entry;

	current_hud_entry=hud_content;
	current_last_frame_hud_entry=previous_frame_hud_content;
	for (hud_y=0; hud_y<HUD_CHARACTER_HEIGHT; hud_y++) {
		for (hud_x=0; hud_x<HUD_CHARACTER_WIDTH; hud_x++) {
			if ((current_hud_entry->character!=current_last_frame_hud_entry->character) ||
                (current_hud_entry->attribute!=current_last_frame_hud_entry->attribute)) {
				scene_backend_update_hud_character(hud_x,hud_y,current_hud_entry->character,current_hud_entry->attribute);
			}
			current_hud_entry++;
			current_last_frame_hud_entry++;
		}
	}

	memcpy(previous_frame_hud_content,hud_content,sizeof(struct hudentry)*HUD_CONTENT_LENGTH);
}

void scene_update_display_mode_from_game_state(int jump_table_function_number, int jump_table_subfunction_number) {
	int new_display_mode;

	switch((jump_table_function_number<<16)|jump_table_subfunction_number) {
		case 0x00010001:
		case 0x00030000:
			new_display_mode=DISPLAY_MODE_TITLE_SCREEN;
		break;
		case 0x00010003:
			new_display_mode=DISPLAY_MODE_DEMO;
		break;
		case 0x00030002:
			new_display_mode=DISPLAY_MODE_IN_GAME;
		break;
		default:
			new_display_mode=DISPLAY_MODE_BLANK;
		break;
	}

	if (new_display_mode!=old_display_mode) {
		old_display_mode=new_display_mode;
		update_scene_graph_from_display_mode(new_display_mode);
	}

}

int scene_active() {
	return scene_backend_active();
}

static void update_scene_graph_from_display_mode(int display_mode) {
	switch(display_mode) {
		case DISPLAY_MODE_TITLE_SCREEN:
			scene_backend_set_display_channel(DISPLAY_CHANNEL_TITLE_SCREEN);
			scene_backend_set_game_over_visibility(0);
		break;
		case DISPLAY_MODE_DEMO:
			scene_backend_set_display_channel(DISPLAY_CHANNEL_IN_GAME);
			scene_backend_set_game_over_visibility(1);
		break;
		case DISPLAY_MODE_IN_GAME:
			scene_backend_set_display_channel(DISPLAY_CHANNEL_IN_GAME);
			scene_backend_set_game_over_visibility(0);
		break;
	}
}

