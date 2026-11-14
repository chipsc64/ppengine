/*
 * Asset loading for the Maggie renderer (port plan T18/T19).
 *
 * Textures: maggie.library copies DXT1 data byte for byte, one mip level per
 * magUploadTexture() call, levels 2^10 down to 2^5 (32x32) only
 * (third_party/MaggieLibrary/maggie_texture.c, commit 64c78ef). The .dds
 * files hold the full chain in DDS (little-endian) layout, as the library
 * samples load them, so each level is uploaded as it is.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/Maggie.h>
#include <maggie_vertex.h>
#include <maggie_flags.h>
#include "scene/maggie/assets.h"

#define MAX_TEXTURES 32

struct loaded_texture {
	char name[32];
	UWORD handle;
};

static struct loaded_texture textures[MAX_TEXTURES];
static int texture_count;
static UWORD white_texture = NO_TEXTURE;
static char error_text[128];

const char *assets_error(void) {
	return error_text;
}

static void set_error(const char *what, const char *name) {
	snprintf(error_text, sizeof(error_text), "%s: %s", what, name);
}

/* Reads a whole file; free with free(). */
static UBYTE *read_file(const char *path, ULONG *size) {
	FILE *file = fopen(path, "rb");
	UBYTE *data;
	long length;

	if (!file) {
		set_error("cannot open", path);
		return NULL;
	}
	fseek(file, 0, SEEK_END);
	length = ftell(file);
	fseek(file, 0, SEEK_SET);
	data = (UBYTE *)malloc(length > 0 ? (size_t)length : 1);
	if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) {
		set_error("cannot read", path);
		free(data);
		fclose(file);
		return NULL;
	}
	fclose(file);
	*size = (ULONG)length;
	return data;
}

static ULONG read_le32(const UBYTE *p) {
	return (ULONG)p[0] | ((ULONG)p[1] << 8) | ((ULONG)p[2] << 16) | ((ULONG)p[3] << 24);
}

static UWORD read_be16(const UBYTE *p) {
	return (UWORD)((p[0] << 8) | p[1]);
}

static UWORD load_dds(const char *path) {
	ULONG size;
	UBYTE *data = read_file(path, &size);
	ULONG width, mip_count, offset;
	int log2size, level;
	UWORD handle;

	if (!data) {
		return NO_TEXTURE;
	}
	if (size < 128 || memcmp(data, "DDS ", 4) != 0 || memcmp(data + 84, "DXT1", 4) != 0) {
		set_error("not a DXT1 .dds file", path);
		free(data);
		return NO_TEXTURE;
	}
	width = read_le32(data + 16);
	mip_count = read_le32(data + 28);
	for (log2size = 5; log2size <= 10 && (1UL << log2size) != width; log2size++) {
	}
	if (log2size > 10 || read_le32(data + 12) != width) {
		set_error("texture must be square, 32 to 1024 pixels", path);
		free(data);
		return NO_TEXTURE;
	}
	handle = magAllocateTexture(log2size);
	if (handle == 0xffff) {
		set_error("magAllocateTexture failed", path);
		free(data);
		return NO_TEXTURE;
	}
	offset = 128;
	for (level = log2size; level >= 5 && (ULONG)(log2size - level) < mip_count; level--) {
		ULONG level_bytes = (1UL << level) * (1UL << level) / 2;

		if (offset + level_bytes > size) {
			break;
		}
		magUploadTexture(handle, level, data + offset, MAG_TEXFMT_DXT1);
		offset += level_bytes;
	}
	free(data);
	return handle;
}

int assets_open(void) {
	texture_count = 0;
	white_texture = assets_texture("white");
	return white_texture != NO_TEXTURE;
}

void assets_close(void) {
	int i;

	for (i = 0; i < texture_count; i++) {
		magFreeTexture(textures[i].handle);
	}
	texture_count = 0;
	white_texture = NO_TEXTURE;
}

UWORD assets_texture(const char *name) {
	char path[128];
	UWORD handle;
	int i;

	if (!name[0]) {
		return white_texture;
	}
	for (i = 0; i < texture_count; i++) {
		if (strcmp(textures[i].name, name) == 0) {
			return textures[i].handle;
		}
	}
	if (texture_count == MAX_TEXTURES) {
		set_error("too many textures", name);
		return NO_TEXTURE;
	}
	snprintf(path, sizeof(path), ASSET_DIR "textures/%s.dds", name);
	handle = load_dds(path);
	if (handle != NO_TEXTURE) {
		strncpy(textures[texture_count].name, name, sizeof(textures[texture_count].name) - 1);
		textures[texture_count].handle = handle;
		texture_count++;
	}
	return handle;
}

int assets_load_model(struct model *model, const char *name) {
	char path[128];
	ULONG size;
	UBYTE *data;
	const UBYTE *p;
	int part_index;
	float radius_squared = 0.0f;

	memset(model, 0, sizeof(*model));
	snprintf(path, sizeof(path), ASSET_DIR "models/%s.ppm", name);
	data = read_file(path, &size);
	if (!data) {
		return 0;
	}
	if (size < 8 || memcmp(data, "PPM1", 4) != 0) {
		set_error("not a PPM1 model", path);
		free(data);
		return 0;
	}
	model->part_count = read_be16(data + 4);
	model->parts = (struct model_part *)calloc((size_t)model->part_count, sizeof(struct model_part));
	if (!model->parts) {
		set_error("out of memory", path);
		free(data);
		return 0;
	}

	p = data + 8;
	for (part_index = 0; part_index < model->part_count; part_index++) {
		struct model_part *part = &model->parts[part_index];
		struct MaggieVertex *vertices;
		const float *source;
		char texture_name[33];
		UWORD i;

		part->vertex_buffer = 0xffff;
		part->index_buffer = 0xffff;
		memcpy(part->name, p, 31);
		memcpy(texture_name, p + 32, 32);
		texture_name[32] = 0;
		part->rgb = ((ULONG)p[64] << 16) | ((ULONG)p[65] << 8) | p[66];
		part->vertex_count = read_be16(p + 68);
		part->index_count = read_be16(p + 70);
		p += 72;

		part->texture = assets_texture(texture_name);
		if (part->texture == NO_TEXTURE) {
			free(data);
			assets_free_model(model);
			return 0;
		}

		/* The file holds big-endian IEEE floats: the 68080's own format. */
		source = (const float *)p;
		vertices = (struct MaggieVertex *)malloc(sizeof(struct MaggieVertex) * part->vertex_count);
		if (!vertices) {
			set_error("out of memory", path);
			free(data);
			assets_free_model(model);
			return 0;
		}
		for (i = 0; i < part->vertex_count; i++, source += 8) {
			float r2 = source[0] * source[0] + source[1] * source[1] + source[2] * source[2];

			if (r2 > radius_squared) {
				radius_squared = r2;
			}
			vertices[i].pos.x = source[0];
			vertices[i].pos.y = source[1];
			vertices[i].pos.z = source[2];
			vertices[i].normal.x = source[3];
			vertices[i].normal.y = source[4];
			vertices[i].normal.z = source[5];
			vertices[i].tex[0].u = source[6];
			vertices[i].tex[0].v = source[7];
			vertices[i].tex[0].w = 0.0f;
			vertices[i].colour = 0x00ffffff;
		}
		p = (const UBYTE *)source;

		part->vertex_buffer = magAllocateVertexBuffer(part->vertex_count);
		part->index_buffer = magAllocateIndexBuffer(part->index_count);
		if (part->vertex_buffer == 0xffff || part->index_buffer == 0xffff) {
			set_error("cannot allocate maggie buffers", path);
			free(vertices);
			free(data);
			assets_free_model(model);
			return 0;
		}
		magUploadVertexBuffer(part->vertex_buffer, vertices, 0, part->vertex_count);
		magUploadIndexBuffer(part->index_buffer, (UWORD *)p, 0, part->index_count);
		part->polygon_count = part->index_count ? 1 : 0;
		for (i = 0; i < part->index_count; i++) {
			if (((const UWORD *)p)[i] == 0xffff) {
				part->polygon_count++;
			}
		}
		free(vertices);
		p += part->index_count * 2;
		p += (p - data) & 1;
	}
	free(data);
	model->radius = (float)sqrt(radius_squared);
	return 1;
}

void assets_free_model(struct model *model) {
	int i;

	if (!model->parts) {
		return;
	}
	for (i = 0; i < model->part_count; i++) {
		/* Parts the loader did not reach are still all zero (calloc), and 0
		   is a valid handle: only free what was allocated. */
		if (model->parts[i].vertex_count == 0) {
			continue;
		}
		if (model->parts[i].vertex_buffer != 0xffff) {
			magFreeVertexBuffer(model->parts[i].vertex_buffer);
		}
		if (model->parts[i].index_buffer != 0xffff) {
			magFreeIndexBuffer(model->parts[i].index_buffer);
		}
	}
	free(model->parts);
	model->parts = NULL;
	model->part_count = 0;
}

int assets_load_image(struct image565 *image, const char *name) {
	char path[128];
	ULONG size;
	UBYTE *data;
	ULONG pixels;

	memset(image, 0, sizeof(*image));
	snprintf(path, sizeof(path), ASSET_DIR "gfx/%s.565", name);
	data = read_file(path, &size);
	if (!data) {
		return 0;
	}
	if (size < 12 || memcmp(data, "P565", 4) != 0) {
		set_error("not a P565 image", path);
		free(data);
		return 0;
	}
	image->width = read_be16(data + 4);
	image->height = read_be16(data + 6);
	image->key = read_be16(data + 8);
	image->keyed = read_be16(data + 10) & 1;
	pixels = (ULONG)image->width * image->height;
	if (size < 12 + pixels * 2) {
		set_error("image file too short", path);
		free(data);
		return 0;
	}
	image->pixels = (UWORD *)malloc(pixels * 2);
	if (!image->pixels) {
		set_error("out of memory", path);
		free(data);
		return 0;
	}
	memcpy(image->pixels, data + 12, pixels * 2);
	free(data);
	return 1;
}

void assets_free_image(struct image565 *image) {
	free(image->pixels);
	image->pixels = NULL;
}

int assets_load_font(UBYTE *glyphs, ULONG size) {
	ULONG file_size;
	UBYTE *data = read_file(ASSET_DIR "font8x8.bin", &file_size);

	if (!data) {
		return 0;
	}
	if (file_size != size) {
		set_error("font has the wrong size", ASSET_DIR "font8x8.bin");
		free(data);
		return 0;
	}
	memcpy(glyphs, data, size);
	free(data);
	return 1;
}
