#include "arcade_fb.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "lvgl/src/misc/cache/instance/lv_image_cache.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define LUT_SIZE 256
#define LUT_MASK (LUT_SIZE - 1)
#define TWO_PI_F (2.0f * (float)M_PI)

static float sin_lut[LUT_SIZE + 1];
static float cos_lut[LUT_SIZE + 1];
static bool lut_ready = false;
static uint32_t prng_state = 0;

uint32_t arcade_rng_seed(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint32_t s = (uint32_t)ts.tv_nsec ^ ((uint32_t)ts.tv_sec * 2654435761u) ^ ((uint32_t)getpid() << 13);
	return s ? s : 0x9E3779B9u;
}

uint32_t arcade_rng_next(uint32_t *state) {
	uint32_t x = (state && *state) ? *state : arcade_rng_seed();
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	if (!x) x = 0x6D2B79F5u;
	if (state) *state = x;
	return x;
}

static float next_unit_rand(void) {
	uint32_t x = arcade_rng_next(&prng_state);
	return (float)(x & 0xFFFFu) / 65535.0f;
}

static int isqrt_floor(int n) {
	if (n <= 0) {
		return 0;
	}
	int x = n;
	int y = (x + 1) >> 1;
	while (y < x) {
		x = y;
		y = (x + n / x) >> 1;
	}
	return x;
}

static inline void fill_span_opaque(uint16_t *dst, int count, uint16_t c) {
	if (count <= 0) {
		return;
	}
	if ((uintptr_t)dst & 2u) {
		*dst++ = c;
		count--;
	}
	uint32_t pair = ((uint32_t)c << 16) | c;
	uint32_t *d32 = (uint32_t *)dst;
	int n2 = count >> 1;
	for (int i = 0; i < n2; i++) {
		d32[i] = pair;
	}
	if (count & 1) {
		((uint16_t *)(d32 + n2))[0] = c;
	}
}

static inline void fill_span_alpha(uint16_t *dst, int count, uint16_t c, uint8_t alpha) {
	if (alpha == 0 || count <= 0) {
		return;
	}
	if (alpha >= 252) {
		fill_span_opaque(dst, count, c);
		return;
	}
	for (int i = 0; i < count; i++) {
		dst[i] = arcade_blend565(dst[i], c, alpha);
	}
}

void arcade_lut_init(void) {
	if (lut_ready) {
		return;
	}
	for (int i = 0; i <= LUT_SIZE; i++) {
		double ang = ((double)i / (double)LUT_SIZE) * (2.0 * M_PI);
		sin_lut[i] = (float)sin(ang);
		cos_lut[i] = (float)cos(ang);
	}
	lut_ready = true;
}

static float lut_sample(const float *tab, float rad) {
	if (!lut_ready) {
		arcade_lut_init();
	}
	float pos = rad * ((float)LUT_SIZE / TWO_PI_F);
	pos = fmodf(pos, (float)LUT_SIZE);
	if (pos < 0.0f) {
		pos += (float)LUT_SIZE;
	}
	int i0 = (int)pos;
	float frac = pos - (float)i0;
	i0 &= LUT_MASK;
	return tab[i0] + (tab[i0 + 1] - tab[i0]) * frac;
}

float arcade_sin_ang(float rad) { return lut_sample(sin_lut, rad); }

float arcade_cos_ang(float rad) { return lut_sample(cos_lut, rad); }

void arcade_fb_init(arcade_fb_t *fb, lv_obj_t *parent, int w, int h) {
	memset(fb, 0, sizeof(*fb));
	fb->w = w;
	fb->h = h;
	fb->img = lv_image_create(parent);
	lv_obj_set_pos(fb->img, 0, 0);
	lv_obj_set_size(fb->img, w, h);
	lv_obj_set_clickable(fb->img, false);
	lv_obj_set_event_bubble(fb->img, true);
}

bool arcade_fb_alloc(arcade_fb_t *fb, bool compact) {
	if (!fb || fb->w <= 0 || fb->h <= 0) {
		return false;
	}
	if (fb->pixels) {
		return true;
	}
	size_t bytes = (size_t)fb->w * (size_t)fb->h * sizeof(uint16_t);
	fb->pixels = malloc(bytes);
	if (!fb->pixels) {
		return false;
	}
	memset(fb->pixels, 0, bytes);
	memset(&fb->dsc, 0, sizeof(fb->dsc));
	fb->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	fb->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
	fb->dsc.header.w = (uint32_t)fb->w;
	fb->dsc.header.h = (uint32_t)fb->h;
	fb->dsc.header.stride = (uint32_t)fb->w * 2u;
	fb->dsc.data_size = (uint32_t)bytes;
	// A native RGB565 surface lets LVGL blit changed rectangles directly.
	// Fall back to its image scaler if the second allocation cannot fit.
	if (compact && fb->img) lv_obj_update_layout(fb->img);
	int out_w = fb->img ? lv_obj_get_width(fb->img) : 0;
	int out_h = fb->img ? lv_obj_get_height(fb->img) : 0;
	if (compact && out_w == 2 * fb->w && out_h >= fb->h && out_h <= 2 * fb->h) {
		size_t count = (size_t)out_w * out_h;
		fb->displayed = calloc(count, sizeof(*fb->displayed));
		if (fb->displayed) {
			fb->dsc.header.w = out_w;
			fb->dsc.header.h = out_h;
			fb->dsc.header.stride = out_w * sizeof(*fb->displayed);
			fb->dsc.data_size = count * sizeof(*fb->displayed);
		}
	}
	fb->dsc.data = (const uint8_t *)(fb->displayed ? fb->displayed : fb->pixels);
	if (fb->img) {
		lv_image_set_src(fb->img, &fb->dsc);
	}
	fb->dirty_count = 0;
	fb->full_dirty = true;
	return true;
}

void arcade_fb_free(arcade_fb_t *fb) {
	if (!fb) {
		return;
	}
	if (fb->img) {
		lv_image_set_src(fb->img, NULL);
	}
	if (fb->pixels) {
		lv_image_cache_drop(&fb->dsc);
		free(fb->displayed);
		fb->displayed = NULL;
		free(fb->pixels);
		fb->pixels = NULL;
	}
	memset(&fb->dsc, 0, sizeof(fb->dsc));
	fb->dirty_count = 0;
	fb->full_dirty = false;
}

void arcade_fb_mark_full(arcade_fb_t *fb) {
	if (fb) {
		fb->full_dirty = true;
		fb->dirty_count = 0;
	}
}

void arcade_fb_mark_dirty(arcade_fb_t *fb, int x1, int y1, int x2, int y2) {
	if (!fb || fb->full_dirty) {
		return;
	}
	if (x1 > x2) {
		int t = x1;
		x1 = x2;
		x2 = t;
	}
	if (y1 > y2) {
		int t = y1;
		y1 = y2;
		y2 = t;
	}
	if (x2 < 0 || y2 < 0 || x1 >= fb->w || y1 >= fb->h) {
		return;
	}
	if (x1 < 0) {
		x1 = 0;
	}
	if (y1 < 0) {
		y1 = 0;
	}
	if (x2 >= fb->w) {
		x2 = fb->w - 1;
	}
	if (y2 >= fb->h) {
		y2 = fb->h - 1;
	}

	const int margin = 16;
	for (int i = 0; i < fb->dirty_count; i++) {
		lv_area_t *a = &fb->dirty[i];
		if (x1 <= a->x2 + margin && x2 >= a->x1 - margin && y1 <= a->y2 + margin && y2 >= a->y1 - margin) {
			if (x1 < a->x1) {
				a->x1 = x1;
			}
			if (y1 < a->y1) {
				a->y1 = y1;
			}
			if (x2 > a->x2) {
				a->x2 = x2;
			}
			if (y2 > a->y2) {
				a->y2 = y2;
			}
			return;
		}
	}

	if (fb->dirty_count >= ARCADE_DIRTY_MAX) {
		fb->full_dirty = true;
		fb->dirty_count = 0;
		return;
	}
	fb->dirty[fb->dirty_count++] = (lv_area_t){x1, y1, x2, y2};
}

// The R1 and R3 have the same width. Expand pairs directly into the native
// image, and send only changed bands through the display compositor.
static void flush_compact(arcade_fb_t *fb) {
	int out_h = fb->dsc.header.h;
	bool double_height = out_h == 2 * fb->h;
	lv_area_t bounds = {0, 0, fb->w-1, fb->h-1};
	if (!fb->full_dirty) {
		if (!fb->dirty_count) return;
		bounds = fb->dirty[0];
		for (int i = 1; i < fb->dirty_count; i++) {
			bounds.x1 = LV_MIN(bounds.x1, fb->dirty[i].x1);
			bounds.x2 = LV_MAX(bounds.x2, fb->dirty[i].x2);
			bounds.y1 = LV_MIN(bounds.y1, fb->dirty[i].y1);
			bounds.y2 = LV_MAX(bounds.y2, fb->dirty[i].y2);
		}
	}
	lv_area_t origin;
	lv_obj_get_coords(fb->img, &origin);
	for (int y = bounds.y1; y <= bounds.y2; y += 16) {
		int end = LV_MIN(y + 16, bounds.y2 + 1);
		int left = fb->w, right = -1;
		for (int row = y; row < end; row++) {
			int first = double_height ? 2 * row : row * out_h / fb->h;
			int next = double_height ? first + 2 : (row + 1) * out_h / fb->h;
			const uint16_t *src = fb->pixels + row * fb->w;
			uint32_t *dst = (uint32_t *)fb->displayed + first * fb->w;
			int x1 = bounds.x1, x2 = bounds.x2;
			while (x1 <= x2 && (uint16_t)dst[x1] == src[x1]) x1++;
			if (x1 > x2) continue;
			while ((uint16_t)dst[x2] == src[x2]) x2--;
			for (int x = x1; x <= x2; x++) dst[x] = src[x] * 0x10001u;
			if (next - first == 2) memcpy(dst + fb->w + x1, dst + x1, (x2 - x1 + 1) * sizeof(*dst));
			left = LV_MIN(left, x1);
			right = LV_MAX(right, x2);
		}
		if (right >= left) {
			lv_area_t area = {origin.x1 + left * 2, origin.y1 + y * out_h / fb->h,
							  origin.x1 + right * 2 + 1, origin.y1 + end * out_h / fb->h - 1};
			lv_obj_invalidate_area(fb->img, &area);
		}
	}
}

void arcade_fb_flush(arcade_fb_t *fb) {
	if (!fb || !fb->img || !fb->pixels) {
		return;
	}
	if (fb->displayed) {
		flush_compact(fb);
	} else if (fb->full_dirty) {
		lv_obj_invalidate(fb->img);
	} else {
		for (int i = 0; i < fb->dirty_count; i++) {
			lv_obj_invalidate_area(fb->img, &fb->dirty[i]);
		}
	}
	fb->full_dirty = false;
	fb->dirty_count = 0;
}

void arcade_fb_clear(arcade_fb_t *fb, uint16_t color) {
	if (!fb || !fb->pixels) {
		return;
	}
	fill_span_opaque(fb->pixels, fb->w * fb->h, color);
	arcade_fb_mark_full(fb);
}

static inline uint16_t dither_rgb565(int r16, int g16, int b16, int bayer_idx) {
	// bayer_idx in {0, 1, 2, 3}: sub-LSB bias before 5-6-5 truncation eliminates banding
	static const int bayer[4] = {0, 2, 3, 1};
	int o = bayer[bayer_idx];
	int r = (r16 + o * 2);
	int g = (g16 + o * 1);
	int b = (b16 + o * 2);
	if (r > 255) {
		r = 255;
	}
	if (g > 255) {
		g = 255;
	}
	if (b > 255) {
		b = 255;
	}
	return arcade_rgb565((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

void arcade_fb_fill_vgrad(arcade_fb_t *fb, int y0, int y1, uint16_t top, uint16_t bottom) {
	if (!fb || !fb->pixels || y1 < y0) {
		return;
	}
	int span = y1 - y0;
	int cy0 = y0 < 0 ? 0 : y0;
	int cy1 = y1 >= fb->h ? fb->h - 1 : y1;
	if (cy1 < cy0) {
		return;
	}

	int r0 = (int)((top >> 8) & 0xF8);
	int g0 = (int)((top >> 3) & 0xFC);
	int b0 = (int)((top << 3) & 0xF8);
	int r1 = (int)((bottom >> 8) & 0xF8);
	int g1 = (int)((bottom >> 3) & 0xFC);
	int b1 = (int)((bottom << 3) & 0xF8);

	for (int y = cy0; y <= cy1; y++) {
		int num = y - y0;
		int r = span > 0 ? r0 + ((r1 - r0) * num) / span : r0;
		int g = span > 0 ? g0 + ((g1 - g0) * num) / span : g0;
		int b = span > 0 ? b0 + ((b1 - b0) * num) / span : b0;

		int row_b = (y & 1) << 1;
		uint16_t c_even = dither_rgb565(r, g, b, row_b);
		uint16_t c_odd = dither_rgb565(r, g, b, row_b | 1);
		uint16_t *dst = fb->pixels + (size_t)y * fb->w;
		int count = fb->w;
		if ((uintptr_t)dst & 2u) {
			*dst++ = c_even;
			count--;
			uint16_t swap = c_even;
			c_even = c_odd;
			c_odd = swap;
		}
		uint32_t pair = ((uint32_t)c_odd << 16) | c_even;
		uint32_t *pairs = (uint32_t *)dst;
		for (int i = 0; i < count / 2; i++) pairs[i] = pair;
		if (count & 1) dst[count - 1] = c_even;
	}
	arcade_fb_mark_dirty(fb, 0, cy0, fb->w - 1, cy1);
}

void arcade_fb_fill_rect(arcade_fb_t *fb, int x, int y, int w, int h, uint16_t color) {
	arcade_fb_fill_rect_alpha(fb, x, y, w, h, color, 255);
}

void arcade_fb_fill_rect_alpha(arcade_fb_t *fb, int x, int y, int w, int h, uint16_t color, uint8_t alpha) {
	if (!fb || !fb->pixels || w <= 0 || h <= 0 || alpha == 0) {
		return;
	}
	int x1 = x < 0 ? 0 : x;
	int y1 = y < 0 ? 0 : y;
	int x2 = x + w - 1;
	int y2 = y + h - 1;
	if (x2 >= fb->w) {
		x2 = fb->w - 1;
	}
	if (y2 >= fb->h) {
		y2 = fb->h - 1;
	}
	if (x1 > x2 || y1 > y2) {
		return;
	}
	int cw = x2 - x1 + 1;
	for (int py = y1; py <= y2; py++) {
		fill_span_alpha(fb->pixels + (size_t)py * fb->w + x1, cw, color, alpha);
	}
	arcade_fb_mark_dirty(fb, x1, y1, x2, y2);
}

void arcade_fb_fill_rrect(arcade_fb_t *fb, int x, int y, int w, int h, int radius, uint16_t color) {
	arcade_fb_fill_rrect_alpha(fb, x, y, w, h, radius, color, 255);
}

void arcade_fb_fill_rrect_alpha(arcade_fb_t *fb, int x, int y, int w, int h, int radius, uint16_t color,
								uint8_t alpha) {
	if (!fb || !fb->pixels || w <= 0 || h <= 0 || alpha == 0) {
		return;
	}
	if (radius <= 0) {
		arcade_fb_fill_rect_alpha(fb, x, y, w, h, color, alpha);
		return;
	}
	int max_r = (w < h ? w : h) / 2;
	if (radius > max_r) {
		radius = max_r;
	}

	int y1 = y < 0 ? 0 : y;
	int y2 = y + h - 1;
	if (y2 >= fb->h) {
		y2 = fb->h - 1;
	}
	if (y1 > y2) {
		return;
	}
	int r2 = radius * radius;
	for (int py = y1; py <= y2; py++) {
		int lx = x;
		int rx = x + w - 1;
		int dy = -1;
		if (py < y + radius) {
			dy = (y + radius - 1) - py;
		} else if (py >= y + h - radius) {
			dy = py - (y + h - radius);
		}
		if (dy >= 0) {
			int dx = isqrt_floor(r2 - dy * dy);
			int trim = radius - 1 - dx;
			if (trim > 0) {
				lx += trim;
				rx -= trim;
			}
		}
		if (lx < 0) {
			lx = 0;
		}
		if (rx >= fb->w) {
			rx = fb->w - 1;
		}
		if (lx <= rx) {
			fill_span_alpha(fb->pixels + (size_t)py * fb->w + lx, rx - lx + 1, color, alpha);
		}
	}
	arcade_fb_mark_dirty(fb, x, y1, x + w - 1, y2);
}

void arcade_fb_hline(arcade_fb_t *fb, int x1, int x2, int y, uint16_t color, uint8_t alpha) {
	if (!fb || !fb->pixels || y < 0 || y >= fb->h || alpha == 0) {
		return;
	}
	if (x1 > x2) {
		int t = x1;
		x1 = x2;
		x2 = t;
	}
	if (x2 < 0 || x1 >= fb->w) {
		return;
	}
	if (x1 < 0) {
		x1 = 0;
	}
	if (x2 >= fb->w) {
		x2 = fb->w - 1;
	}
	fill_span_alpha(fb->pixels + (size_t)y * fb->w + x1, x2 - x1 + 1, color, alpha);
	arcade_fb_mark_dirty(fb, x1, y, x2, y);
}

void arcade_fb_vline(arcade_fb_t *fb, int x, int y1, int y2, uint16_t color, uint8_t alpha) {
	if (!fb || !fb->pixels || x < 0 || x >= fb->w || alpha == 0) {
		return;
	}
	if (y1 > y2) {
		int t = y1;
		y1 = y2;
		y2 = t;
	}
	if (y2 < 0 || y1 >= fb->h) {
		return;
	}
	if (y1 < 0) {
		y1 = 0;
	}
	if (y2 >= fb->h) {
		y2 = fb->h - 1;
	}
	for (int py = y1; py <= y2; py++) {
		uint16_t *p = fb->pixels + (size_t)py * fb->w + x;
		*p = alpha >= 252 ? color : arcade_blend565(*p, color, alpha);
	}
	arcade_fb_mark_dirty(fb, x, y1, x, y2);
}

void arcade_fb_draw_line_thick(arcade_fb_t *fb, int x0, int y0, int x1, int y1, int thickness, uint16_t color,
							   uint8_t alpha) {
	if (!fb || !fb->pixels || alpha == 0) {
		return;
	}
	if (thickness <= 1) {
		thickness = 1;
	}
	int dx = abs(x1 - x0);
	int sx = x0 < x1 ? 1 : -1;
	int dy = -abs(y1 - y0);
	int sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;
	int r = thickness / 2;

	int min_x = (x0 < x1 ? x0 : x1) - r;
	int max_x = (x0 > x1 ? x0 : x1) + r;
	int min_y = (y0 < y1 ? y0 : y1) - r;
	int max_y = (y0 > y1 ? y0 : y1) + r;

	int x = x0;
	int y = y0;
	for (;;) {
		if (r <= 0) {
			if (x >= 0 && x < fb->w && y >= 0 && y < fb->h) {
				uint16_t *p = fb->pixels + (size_t)y * fb->w + x;
				*p = alpha >= 252 ? color : arcade_blend565(*p, color, alpha);
			}
		} else {
			int rx1 = x - r;
			int ry1 = y - r;
			int rw = thickness;
			int rh = thickness;
			if (rx1 < 0) {
				rw += rx1;
				rx1 = 0;
			}
			if (ry1 < 0) {
				rh += ry1;
				ry1 = 0;
			}
			if (rx1 + rw > fb->w) {
				rw = fb->w - rx1;
			}
			if (ry1 + rh > fb->h) {
				rh = fb->h - ry1;
			}
			for (int py = 0; py < rh; py++) {
				fill_span_alpha(fb->pixels + (size_t)(ry1 + py) * fb->w + rx1, rw, color, alpha);
			}
		}
		if (x == x1 && y == y1) {
			break;
		}
		int e2 = 2 * err;
		if (e2 >= dy) {
			err += dy;
			x += sx;
		}
		if (e2 <= dx) {
			err += dx;
			y += sy;
		}
	}
	arcade_fb_mark_dirty(fb, min_x, min_y, max_x, max_y);
}

void arcade_fb_fill_circle(arcade_fb_t *fb, int cx, int cy, int r, uint16_t color) {
	arcade_fb_fill_circle_alpha(fb, cx, cy, r, color, 255);
}

void arcade_fb_fill_circle_alpha(arcade_fb_t *fb, int cx, int cy, int r, uint16_t color, uint8_t alpha) {
	if (!fb || !fb->pixels || r < 0 || alpha == 0) {
		return;
	}
	if (r == 0) {
		if (cx >= 0 && cx < fb->w && cy >= 0 && cy < fb->h) {
			uint16_t *p = fb->pixels + (size_t)cy * fb->w + cx;
			*p = alpha >= 252 ? color : arcade_blend565(*p, color, alpha);
			arcade_fb_mark_dirty(fb, cx, cy, cx, cy);
		}
		return;
	}
	int y1 = cy - r;
	int y2 = cy + r;
	if (y1 < 0) {
		y1 = 0;
	}
	if (y2 >= fb->h) {
		y2 = fb->h - 1;
	}
	if (y1 > y2) {
		return;
	}
	int r2 = r * r;
	for (int py = y1; py <= y2; py++) {
		int dy = py - cy;
		int hx = isqrt_floor(r2 - dy * dy);
		int lx = cx - hx;
		int rx = cx + hx;
		if (lx < 0) {
			lx = 0;
		}
		if (rx >= fb->w) {
			rx = fb->w - 1;
		}
		if (lx <= rx) {
			fill_span_alpha(fb->pixels + (size_t)py * fb->w + lx, rx - lx + 1, color, alpha);
		}
	}
	arcade_fb_mark_dirty(fb, cx - r, y1, cx + r, y2);
}

void arcade_fb_draw_ring(arcade_fb_t *fb, int cx, int cy, int r_in, int r_out, uint16_t color, uint8_t alpha) {
	if (!fb || !fb->pixels || r_out < 0 || alpha == 0) {
		return;
	}
	if (r_in <= 0) {
		arcade_fb_fill_circle_alpha(fb, cx, cy, r_out, color, alpha);
		return;
	}
	if (r_in > r_out) {
		return;
	}
	int y1 = cy - r_out;
	int y2 = cy + r_out;
	if (y1 < 0) {
		y1 = 0;
	}
	if (y2 >= fb->h) {
		y2 = fb->h - 1;
	}
	if (y1 > y2) {
		return;
	}
	int r_out2 = r_out * r_out;
	int r_in2 = (r_in - 1) * (r_in - 1);

	for (int py = y1; py <= y2; py++) {
		int dy = py - cy;
		int dy2 = dy * dy;
		int hx_out = isqrt_floor(r_out2 - dy2);
		int ol = cx - hx_out;
		int orx = cx + hx_out;

		if (dy2 >= r_in2) {
			int lx = ol < 0 ? 0 : ol;
			int rx = orx >= fb->w ? fb->w - 1 : orx;
			if (lx <= rx) {
				fill_span_alpha(fb->pixels + (size_t)py * fb->w + lx, rx - lx + 1, color, alpha);
			}
		} else {
			int hx_in = isqrt_floor(r_in2 - dy2);
			int il = cx - hx_in - 1;
			int ir = cx + hx_in + 1;

			int lx1 = ol < 0 ? 0 : ol;
			int rx1 = il >= fb->w ? fb->w - 1 : il;
			if (lx1 <= rx1) {
				fill_span_alpha(fb->pixels + (size_t)py * fb->w + lx1, rx1 - lx1 + 1, color, alpha);
			}

			int lx2 = ir < 0 ? 0 : ir;
			int rx2 = orx >= fb->w ? fb->w - 1 : orx;
			if (lx2 <= rx2) {
				fill_span_alpha(fb->pixels + (size_t)py * fb->w + lx2, rx2 - lx2 + 1, color, alpha);
			}
		}
	}
	arcade_fb_mark_dirty(fb, cx - r_out, y1, cx + r_out, y2);
}

static void fill_quad(arcade_fb_t *fb, const float x[4], const float y[4], uint16_t color, uint8_t alpha,
					  const uint16_t *texture, int stride, int origin_y, int rows) {
	if (!fb || !fb->pixels || alpha == 0) {
		return;
	}
	float min_y_f = y[0], max_y_f = y[0];
	float min_x_f = x[0], max_x_f = x[0];
	for (int i = 1; i < 4; i++) {
		if (y[i] < min_y_f) {
			min_y_f = y[i];
		}
		if (y[i] > max_y_f) {
			max_y_f = y[i];
		}
		if (x[i] < min_x_f) {
			min_x_f = x[i];
		}
		if (x[i] > max_x_f) {
			max_x_f = x[i];
		}
	}
	int y1 = (int)floorf(min_y_f);
	int y2 = (int)ceilf(max_y_f);
	if (y1 < 0) {
		y1 = 0;
	}
	if (y2 >= fb->h) {
		y2 = fb->h - 1;
	}
	if (y1 > y2) {
		return;
	}

	float slope[4];
	for (int i = 0; i < 4; i++) {
		int j = (i + 1) & 3;
		slope[i] = y[i] == y[j] ? 0 : (x[j] - x[i]) / (y[j] - y[i]);
	}
	for (int py = y1; py <= y2; py++) {
		float yc = (float)py + 0.5f;
		float xl = 1e9f;
		float xr = -1e9f;
		int hits = 0;
		for (int i = 0; i < 4; i++) {
			int j = (i + 1) & 3;
			float ya = y[i], yb = y[j];
			if ((ya <= yc && yb > yc) || (yb <= yc && ya > yc)) {
				float ix = x[i] + (yc - ya) * slope[i];
				if (ix < xl) {
					xl = ix;
				}
				if (ix > xr) {
					xr = ix;
				}
				hits++;
			}
		}
		if (hits >= 2) {
			int ix1 = (int)ceilf(xl - 0.5f);
			int ix2 = (int)floorf(xr - 0.5f);
			if (ix1 < 0) {
				ix1 = 0;
			}
			if (ix2 >= fb->w) {
				ix2 = fb->w - 1;
			}
			if (ix1 <= ix2) {
				uint16_t *dst = fb->pixels + (size_t)py * fb->w + ix1;
				if (texture) {
					int sy = LV_CLAMP(0, py - origin_y, rows - 1);
					memcpy(dst, texture + sy * stride + ix1, (ix2 - ix1 + 1) * sizeof(*dst));
				} else {
					fill_span_alpha(dst, ix2 - ix1 + 1, color, alpha);
				}
			}
		}
	}
	arcade_fb_mark_dirty(fb, (int)floorf(min_x_f), y1, (int)ceilf(max_x_f), y2);
}

void arcade_fb_fill_quad(arcade_fb_t *fb, const float x[4], const float y[4], uint16_t color, uint8_t alpha) {
	fill_quad(fb, x, y, color, alpha, NULL, 0, 0, 0);
}

void arcade_fb_textured_quad(arcade_fb_t *fb, const float x[4], const float y[4],
							 const uint16_t *texture, int stride, int origin_y, int rows) {
	if (fb->w <= stride && rows > 0) fill_quad(fb, x, y, 0, 255, texture, stride, origin_y, rows);
}

void arcade_fb_draw_arc(arcade_fb_t *fb, int cx, int cy, int r_in, int r_out, float a0_rad, float a1_rad,
						uint16_t color, uint8_t alpha) {
	if (!fb || !fb->pixels || alpha == 0 || r_out <= 0 || r_in > r_out) {
		return;
	}
	float sweep = a1_rad - a0_rad;
	if (fabsf(sweep) < 0.002f) {
		return;
	}
	if (fabsf(sweep) >= TWO_PI_F - 0.01f) {
		arcade_fb_draw_ring(fb, cx, cy, r_in, r_out, color, alpha);
		return;
	}
	int steps = (int)ceilf(fabsf(sweep) * (float)r_out / 10.0f);
	if (steps < 4) {
		steps = 4;
	}
	if (steps > 36) {
		steps = 36;
	}

	float step = sweep / (float)steps;
	float prev_c = arcade_cos_ang(a0_rad);
	float prev_s = arcade_sin_ang(a0_rad);
	for (int i = 1; i <= steps; i++) {
		float ang = a0_rad + step * (float)i;
		float cur_c = arcade_cos_ang(ang);
		float cur_s = arcade_sin_ang(ang);

		float qx[4] = {
			(float)cx + (float)r_in * prev_c,
			(float)cx + (float)r_out * prev_c,
			(float)cx + (float)r_out * cur_c,
			(float)cx + (float)r_in * cur_c,
		};
		float qy[4] = {
			(float)cy + (float)r_in * prev_s,
			(float)cy + (float)r_out * prev_s,
			(float)cy + (float)r_out * cur_s,
			(float)cy + (float)r_in * cur_s,
		};
		arcade_fb_fill_quad(fb, qx, qy, color, alpha);
		prev_c = cur_c;
		prev_s = cur_s;
	}
}

void arcade_fb_blit_region(arcade_fb_t *fb, const uint16_t *src, int src_w, int x, int y, int w, int h) {
	if (!fb || !fb->pixels || !src || w <= 0 || h <= 0) {
		return;
	}
	int x1 = x < 0 ? 0 : x;
	int y1 = y < 0 ? 0 : y;
	int x2 = x + w - 1;
	int y2 = y + h - 1;
	if (x2 >= fb->w) {
		x2 = fb->w - 1;
	}
	if (y2 >= fb->h) {
		y2 = fb->h - 1;
	}
	if (x1 > x2 || y1 > y2) {
		return;
	}
	size_t row_bytes = (size_t)(x2 - x1 + 1) * sizeof(uint16_t);
	for (int py = y1; py <= y2; py++) {
		memcpy(fb->pixels + (size_t)py * fb->w + x1, src + (size_t)py * src_w + x1, row_bytes);
	}
	arcade_fb_mark_dirty(fb, x1, y1, x2, y2);
}

void arcade_particles_clear(arcade_particles_t *p) {
	if (p) {
		memset(p, 0, sizeof(*p));
	}
}

static void spawn_particle_slot(arcade_particles_t *p, const arcade_particle_t *src) {
	int chosen = -1;
	float lowest_life = 1e9f;
	for (int i = 0; i < ARCADE_PARTICLES_MAX; i++) {
		if (!p->items[i].active) {
			chosen = i;
			break;
		}
		if (p->items[i].life < lowest_life) {
			lowest_life = p->items[i].life;
			chosen = i;
		}
	}
	if (chosen >= 0) {
		p->items[chosen] = *src;
	}
}

void arcade_particles_spawn_burst(arcade_particles_t *p, float x, float y, int count, float min_spd, float max_spd,
								  float gravity, float size, float max_life, uint16_t color, uint8_t shape) {
	if (!p || count <= 0 || max_life <= 0.0f) {
		return;
	}
	for (int i = 0; i < count; i++) {
		float ang = next_unit_rand() * TWO_PI_F;
		float spd = min_spd + (max_spd - min_spd) * next_unit_rand();
		float life = max_life * (0.65f + 0.35f * next_unit_rand());
		arcade_particle_t pt = {
			.x = x,
			.y = y,
			.vx = arcade_cos_ang(ang) * spd,
			.vy = arcade_sin_ang(ang) * spd,
			.life = life,
			.max_life = life,
			.size = size * (0.75f + 0.5f * next_unit_rand()),
			.gravity = gravity,
			.color = color,
			.shape = shape,
			.active = true,
		};
		spawn_particle_slot(p, &pt);
	}
}

void arcade_particles_spawn_ring(arcade_particles_t *p, float x, float y, float start_r, float expand_speed,
								 float max_life, uint16_t color) {
	if (!p || max_life <= 0.0f) {
		return;
	}
	arcade_particle_t pt = {
		.x = x,
		.y = y,
		.vx = expand_speed,
		.vy = 0.0f,
		.life = max_life,
		.max_life = max_life,
		.size = start_r,
		.gravity = 0.0f,
		.color = color,
		.shape = 2,
		.active = true,
	};
	spawn_particle_slot(p, &pt);
}

void arcade_particles_update(arcade_particles_t *p, float dt) {
	if (!p || dt <= 0.0f) {
		return;
	}
	for (int i = 0; i < ARCADE_PARTICLES_MAX; i++) {
		arcade_particle_t *pt = &p->items[i];
		if (!pt->active) {
			continue;
		}
		pt->life -= dt;
		if (pt->life <= 0.0f) {
			pt->active = false;
			continue;
		}
		if (pt->shape == 2) {
			pt->size += pt->vx * dt;
		} else {
			pt->vy += pt->gravity * dt;
			pt->x += pt->vx * dt;
			pt->y += pt->vy * dt;
		}
	}
}

void arcade_particles_draw(const arcade_particles_t *p, arcade_fb_t *fb, float cam_y) {
	if (!p || !fb || !fb->pixels) {
		return;
	}
	for (int i = 0; i < ARCADE_PARTICLES_MAX; i++) {
		const arcade_particle_t *pt = &p->items[i];
		if (!pt->active) {
			continue;
		}
		float norm = pt->life / pt->max_life;
		if (norm < 0.0f) {
			norm = 0.0f;
		}
		if (norm > 1.0f) {
			norm = 1.0f;
		}
		uint8_t alpha = (uint8_t)(235.0f * norm);
		int sx = (int)lroundf(pt->x);
		int sy = (int)lroundf(pt->y - cam_y);

		if (pt->shape == 2) {
			int r_out = (int)lroundf(pt->size);
			int r_in = r_out - 2;
			if (r_in < 1) {
				r_in = 1;
			}
			arcade_fb_draw_ring(fb, sx, sy, r_in, r_out, pt->color, alpha);
		} else if (pt->shape == 1) {
			int s = (int)lroundf(pt->size * (0.45f + 0.55f * norm));
			if (s < 2) {
				s = 2;
			}
			arcade_fb_fill_rect_alpha(fb, sx - s / 2, sy - s / 2, s, s, pt->color, alpha);
		} else {
			int r = (int)lroundf(pt->size * (0.4f + 0.6f * norm));
			if (r < 1) {
				r = 1;
			}
			arcade_fb_fill_circle_alpha(fb, sx, sy, r, pt->color, alpha);
		}
	}
}
