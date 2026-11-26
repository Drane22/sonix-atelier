#include "arcade_scene.h"

#include <math.h>
#include <string.h>

void scene_block(arcade_fb_t *fb, int x, int y, int w, int h, int depth, uint16_t color) {
	float top_x[] = {x, x + depth, x + w + depth, x + w};
	float top_y[] = {y, y - depth, y - depth, y};
	float side_x[] = {x + w, x + w + depth, x + w + depth, x + w};
	float side_y[] = {y, y - depth, y - depth + h, y + h};
	arcade_fb_fill_quad(fb, top_x, top_y, arcade_blend565(color, 0xffff, 76), 255);
	arcade_fb_fill_quad(fb, side_x, side_y, arcade_blend565(color, 0, 95), 255);
	arcade_fb_fill_rect(fb, x, y, w, h, color);
	arcade_fb_hline(fb, x, x + w - 1, y, 0xffff, 120);
}

void scene_orb(arcade_fb_t *fb, int x, int y, int radius, uint16_t color) {
	arcade_fb_fill_circle(fb, x, y, radius, arcade_blend565(color, 0, 90));
	arcade_fb_fill_circle(fb, x - 1, y - 1, radius - 1, color);
	arcade_fb_fill_circle(fb, x - radius / 3, y - radius / 3, radius / 3, 0xffff);
}

void scene_stars(arcade_fb_t *fb, int height) {
	for (int i = 0; i < 38; i++) {
		int x = (i * 67 + 7) % fb->w;
		int y = (i * i * 13 + i * 11 + 5) % height;
		arcade_fb_hline(fb, x, x + (i % 7 == 0), y, 0xd6ff, 90 + i * 4);
	}
}

// Native RGB565 paintings are mapped read-only. A dirty band needs only one copy.
void scene_backdrop(arcade_fb_t *fb, const uint16_t *art, int first_row, int last_row) {
	bool compact = art == scene_reef || art == scene_enamel;
	int w = compact ? 240 : 480;
	int h = compact ? 400 : 800;
	first_row = LV_MAX(0, first_row);
	last_row = LV_MIN(fb->h - 1, last_row);
	if (last_row < first_row) return;
	if (fb->w == w && fb->h <= h) {
		int offset = (h - fb->h) / 2;
		memcpy(fb->pixels + first_row * w, art + (offset + first_row) * w,
			   (size_t)(last_row - first_row + 1) * w * sizeof(uint16_t));
	} else {
		for (int y = first_row; y <= last_row; y++)
			for (int x = 0; x < fb->w; x++)
				fb->pixels[y * fb->w + x] = art[(y * h / fb->h) * w + x * w / fb->w];
	}
}

void scene_sphere(arcade_fb_t *fb, int cx, int cy, int radius, uint16_t color) {
	for (int y = -radius; y <= radius; y++) {
		if (cy + y < 0 || cy + y >= fb->h) continue;
		for (int x = -radius; x <= radius; x++) {
			if (cx + x < 0 || cx + x >= fb->w) continue;
			float d = sqrtf((float)(x*x + y*y));
			float coverage = radius + 0.5f - d;
			if (coverage <= 0) continue;
			float z = sqrtf(fmaxf(0, radius*radius - d*d));
			float light = (0.72f*z - 0.42f*x - 0.55f*y) / radius;
			uint16_t c = arcade_blend565(0x0845, color, (uint8_t)LV_CLAMP(0, 75 + 180*light, 255));
			float shine = 1 - (fabsf(x + radius*0.32f) + 1.4f*fabsf(y + radius*0.45f)) / (radius*0.44f);
			if (shine > 0) c = arcade_blend565(c, 0xffff, (uint8_t)(shine*245));
			uint16_t *pixel = &fb->pixels[(cy+y)*fb->w + cx+x];
			*pixel = arcade_blend565(*pixel, c, (uint8_t)(255*fminf(1, coverage)));
		}
	}
	arcade_fb_mark_dirty(fb, cx-radius, cy-radius, cx+radius, cy+radius);
}


extern const uint8_t scene_lake[], scene_tabletop[];
const lv_image_dsc_t scene_tabletop_image = {
	.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, .w = 240, .h = 400, .stride = 480},
	.data_size = 240 * 400 * 2, .data = scene_tabletop,
};
const lv_image_dsc_t scene_lake_image = {
	.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, .w = 240, .h = 400, .stride = 480},
	.data_size = 240 * 400 * 2, .data = scene_lake,
};
