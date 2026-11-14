#ifndef __SCENE_MAGGIE_ASSETS_H
#define __SCENE_MAGGIE_ASSETS_H

/*
 * Loads the files made by tools/convert_assets.py (formats documented
 * there) into maggie.library buffers and textures. Load while the OS runs.
 */

#include <exec/types.h>

#define ASSET_DIR "PROGDIR:data/"
#define NO_TEXTURE 0xffff

struct model_part {
	char name[32];
	UWORD texture;     /* maggie texture handle (white.dds when the part has none) */
	ULONG rgb;         /* tint, $00RRGGBB */
	UWORD vertex_buffer;
	UWORD index_buffer;
	UWORD vertex_count;
	UWORD index_count;
	UWORD polygon_count;
};

struct model {
	int part_count;
	struct model_part *parts;
	float radius;      /* largest distance of a vertex from the model origin */
};

struct image565 {
	UWORD width;
	UWORD height;
	UWORD key;         /* colour of transparent pixels */
	UWORD keyed;       /* 1 when the image has transparent pixels */
	UWORD *pixels;
};

int assets_open(void);   /* loads white.dds; 0 on failure */
void assets_close(void); /* frees every texture loaded through this module */
UWORD assets_texture(const char *name);  /* loads textures/<name>.dds once; NO_TEXTURE on failure */
int assets_load_model(struct model *model, const char *name);
void assets_free_model(struct model *model);
int assets_load_image(struct image565 *image, const char *name);
void assets_free_image(struct image565 *image);
int assets_load_font(UBYTE *glyphs, ULONG size);  /* 96 * 8 bytes */
const char *assets_error(void);

#endif
