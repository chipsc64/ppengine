#ifndef __SCENE_MAGGIE_BACKEND_H
#define __SCENE_MAGGIE_BACKEND_H

/* Apollo V4 extras of the Maggie scene backend (see scene/backend.h). */

struct backend_stats {
	int objects;     /* models drawn in the last frame */
	int parts;       /* maggie draw calls for model parts */
	int road_quads;
	unsigned long background_cycles;  /* CPU clock cycles of the last frame, per stage */
	unsigned long scene_cycles;
	unsigned long hud_cycles;
	unsigned long road_cycles;        /* parts of scene_cycles */
	unsigned long car_cycles;
	unsigned long scenery_cycles;
	unsigned long setup_cycles;       /* magBeginScene, clear, matrices */
};

void maggie_backend_set_cull_ccw(int ccw);   /* front-face winding switch for testing */

/* Draw mode options, to measure what they cost (main_amiga.c arguments). */
#define MAGGIE_OPTION_NO_BILINEAR 1
#define MAGGIE_OPTION_NO_DEPTH    2
#define MAGGIE_OPTION_AFFINE      4
#define MAGGIE_OPTION_NEAR_FIRST  8   /* depth-test check: draw near objects first */
#define MAGGIE_OPTION_FULL_DETAIL 16  /* always the full model (to compare the levels of detail) */
void maggie_backend_set_option(int option, int on);
const struct backend_stats *maggie_backend_stats(void);
void maggie_backend_depth_test(void);
void maggie_backend_benchmark(unsigned long *cycles_per_level, int *polygons_per_level, int *parts_per_level);
#define MAGGIE_LOD_LEVELS 4

#endif
