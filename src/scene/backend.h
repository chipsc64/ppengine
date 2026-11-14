#ifndef __SCENE_BACKEND_H
#define __SCENE_BACKEND_H

/*
 * What scene.c needs from a renderer. The OpenSceneGraph version
 * (src/scene/osg/) had the same functions with an osg_scene_ prefix; the
 * Apollo V4 version is src/scene/maggie/.
 */

#include <inttypes.h>
#include "scene/moving_object_model_type.h"
#include "scene/gantry_appearance_type.h"

#define DISPLAY_CHANNEL_TITLE_SCREEN 1
#define DISPLAY_CHANNEL_IN_GAME 2

int scene_backend_open(void);  /* 0 on failure */
void scene_backend_close(void);
void scene_backend_add_gantry(double world_coordinates_x, double world_coordinates_y, double rotation_radians);
void scene_backend_set_gantry_light_illumination(int light_index, int illuminated);
void scene_backend_add_scenery_object(double world_coordinates_x, double world_coordinates_y, double rotation_radians, int model_index);
void scene_backend_set_moving_object_attributes(int object_index, double world_coordinates_x, double world_coordinates_y, double rotation_radians, int moving_object_model, int visible);
void scene_backend_start_car_explosion(int object_index);
void scene_backend_end_car_explosion(int object_index);
void scene_backend_set_camera_position(double camera_x, double camera_y, double object_x, double object_y);
void scene_backend_update_hud_character(int hud_x, int hud_y, uint8_t character, uint8_t attribute);
void scene_backend_set_display_channel(int display_channel);
void scene_backend_set_game_over_visibility(int visible);
void scene_backend_generate_frame(void);
int scene_backend_active(void);

/* Apollo V4: the 320x240 R5G6B5 buffer the next frame is drawn into. */
void scene_backend_set_target(uint16_t *pixels);

#endif
