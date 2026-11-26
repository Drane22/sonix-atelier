#ifndef ARCADE_FB_H
#define ARCADE_FB_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

#define ARCADE_DIRTY_MAX 24
#define ARCADE_PARTICLES_MAX 64

typedef struct {
	int w;
	int h;
	uint16_t *pixels;
	uint16_t *displayed; // Native RGB565 output for compact games on the 480px panels
	lv_image_dsc_t dsc;
	lv_obj_t *img;
	lv_area_t dirty[ARCADE_DIRTY_MAX];
	int dirty_count;
	bool full_dirty;
} arcade_fb_t;

typedef struct {
	float x;
	float y;
	float vx;
	float vy;
	float life;
	float max_life;
	float size;
	float gravity;
	uint16_t color;
	uint8_t shape; // 0 = circle, 1 = square, 2 = expanding ring
	bool active;
} arcade_particle_t;

typedef struct {
	arcade_particle_t items[ARCADE_PARTICLES_MAX];
} arcade_particles_t;

static inline uint16_t arcade_rgb565(uint8_t r, uint8_t g, uint8_t b) {
	return (uint16_t)(((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3));
}

static inline uint16_t arcade_lv_to_565(lv_color_t c) { return arcade_rgb565(c.red, c.green, c.blue); }

// Fast SWAR RGB565 alpha blend (alpha in 0..255).
static inline uint16_t arcade_blend565(uint16_t bg, uint16_t fg, uint8_t alpha) {
	if (alpha == 0) {
		return bg;
	}
	if (alpha >= 252) {
		return fg;
	}
	uint32_t a = ((uint32_t)alpha + 4u) >> 3; // 0..32
	uint32_t b_exp = (((uint32_t)bg & 0x07E0u) << 16) | ((uint32_t)bg & 0xF81Fu);
	uint32_t f_exp = (((uint32_t)fg & 0x07E0u) << 16) | ((uint32_t)fg & 0xF81Fu);
	uint32_t mixed = ((b_exp * (32u - a) + f_exp * a) >> 5) & 0x07E0F81Fu;
	return (uint16_t)(mixed | (mixed >> 16));
}

static inline uint16_t arcade_lerp565(uint16_t a, uint16_t b, uint8_t t) { return arcade_blend565(a, b, t); }

// 256-entry sin/cos lookup table with linear interpolation + monotonic PRNG.
void arcade_lut_init(void);
uint32_t arcade_rng_seed(void);
uint32_t arcade_rng_next(uint32_t *state);
float arcade_sin_ang(float rad);
float arcade_cos_ang(float rad);

// Framebuffer lifecycle & dirty region management.
void arcade_fb_init(arcade_fb_t *fb, lv_obj_t *parent, int w, int h);
bool arcade_fb_alloc(arcade_fb_t *fb, bool compact);
void arcade_fb_free(arcade_fb_t *fb);
void arcade_fb_mark_dirty(arcade_fb_t *fb, int x1, int y1, int x2, int y2);
void arcade_fb_mark_full(arcade_fb_t *fb);
void arcade_fb_flush(arcade_fb_t *fb);

// Clipped RGB565 primitives (zero heap allocation).
void arcade_fb_clear(arcade_fb_t *fb, uint16_t color);
void arcade_fb_fill_vgrad(arcade_fb_t *fb, int y0, int y1, uint16_t top, uint16_t bottom);
void arcade_fb_fill_rect(arcade_fb_t *fb, int x, int y, int w, int h, uint16_t color);
void arcade_fb_fill_rect_alpha(arcade_fb_t *fb, int x, int y, int w, int h, uint16_t color, uint8_t alpha);
void arcade_fb_fill_rrect(arcade_fb_t *fb, int x, int y, int w, int h, int radius, uint16_t color);
void arcade_fb_fill_rrect_alpha(arcade_fb_t *fb, int x, int y, int w, int h, int radius, uint16_t color, uint8_t alpha);
void arcade_fb_hline(arcade_fb_t *fb, int x1, int x2, int y, uint16_t color, uint8_t alpha);
void arcade_fb_vline(arcade_fb_t *fb, int x, int y1, int y2, uint16_t color, uint8_t alpha);
void arcade_fb_draw_line_thick(arcade_fb_t *fb, int x0, int y0, int x1, int y1, int thickness, uint16_t color,
							   uint8_t alpha);
void arcade_fb_fill_circle(arcade_fb_t *fb, int cx, int cy, int r, uint16_t color);
void arcade_fb_fill_circle_alpha(arcade_fb_t *fb, int cx, int cy, int r, uint16_t color, uint8_t alpha);
void arcade_fb_draw_ring(arcade_fb_t *fb, int cx, int cy, int r_in, int r_out, uint16_t color, uint8_t alpha);
void arcade_fb_draw_arc(arcade_fb_t *fb, int cx, int cy, int r_in, int r_out, float a0_rad, float a1_rad,
						uint16_t color, uint8_t alpha);
void arcade_fb_textured_quad(arcade_fb_t *fb, const float x[4], const float y[4],
							 const uint16_t *texture, int stride, int origin_y, int rows);
void arcade_fb_fill_quad(arcade_fb_t *fb, const float x[4], const float y[4], uint16_t color, uint8_t alpha);
void arcade_fb_blit_region(arcade_fb_t *fb, const uint16_t *src, int src_w, int x, int y, int w, int h);

// Zero-malloc particle pool helpers.
void arcade_particles_clear(arcade_particles_t *p);
void arcade_particles_spawn_burst(arcade_particles_t *p, float x, float y, int count, float min_spd, float max_spd,
								  float gravity, float size, float max_life, uint16_t color, uint8_t shape);
void arcade_particles_spawn_ring(arcade_particles_t *p, float x, float y, float start_r, float expand_speed,
								 float max_life, uint16_t color);
void arcade_particles_update(arcade_particles_t *p, float dt);
void arcade_particles_draw(const arcade_particles_t *p, arcade_fb_t *fb, float cam_y);

#endif // ARCADE_FB_H
