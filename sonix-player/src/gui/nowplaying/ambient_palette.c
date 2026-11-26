#include "ambient_palette.h"

#include <string.h>

static int clamp_i(int v, int lo, int hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

static int luma_rgb(int r, int g, int b) {
	return (r * 299 + g * 587 + b * 114) / 1000;
}

int ambient_color_luma(lv_color_t c) {
	return luma_rgb(c.red, c.green, c.blue);
}

lv_color_t ambient_contrast_ink(lv_color_t bg) {
	return ambient_color_luma(bg) >= 150 ? lv_color_make(11, 15, 22) : lv_color_make(248, 250, 252);
}

static lv_color_t lerp_color(lv_color_t a, lv_color_t b, int permille) {
	permille = clamp_i(permille, 0, 1000);
	int r = a.red + ((int)b.red - (int)a.red) * permille / 1000;
	int g = a.green + ((int)b.green - (int)a.green) * permille / 1000;
	int bl = a.blue + ((int)b.blue - (int)a.blue) * permille / 1000;
	return lv_color_make((uint8_t)clamp_i(r, 0, 255), (uint8_t)clamp_i(g, 0, 255), (uint8_t)clamp_i(bl, 0, 255));
}

// Shapes an 0xRRGGBB seed to a target saturation percentage (0..100 of its
// chroma, optionally boosted when `min_chroma > 0`) and exact target Rec. 601
// luma (`target_luma` in 0..255), preserving hue without clipping channels.
static lv_color_t shape_seed(int sr, int sg, int sb, int sat_pct, int min_chroma, int max_chroma, int target_luma) {
	int hi = sr > sg ? (sr > sb ? sr : sb) : (sg > sb ? sg : sb);
	int lo = sr < sg ? (sr < sb ? sr : sb) : (sg < sb ? sg : sb);
	int span = hi - lo;

	int r = sr, g = sg, b = sb;
	if (span > 0) {
		// Normalize chroma to `wanted_span` first so muted covers still yield a
		// readable accent and hyper-saturated neon covers are restrained.
		int wanted_span = span * sat_pct / 100;
		if (min_chroma > 0 && wanted_span < min_chroma) {
			wanted_span = min_chroma;
		}
		if (max_chroma > 0 && wanted_span > max_chroma) {
			wanted_span = max_chroma;
		}
		int mid = luma_rgb(sr, sg, sb);
		r = mid + (sr - mid) * wanted_span / span;
		g = mid + (sg - mid) * wanted_span / span;
		b = mid + (sb - mid) * wanted_span / span;
		r = clamp_i(r, 0, 255);
		g = clamp_i(g, 0, 255);
		b = clamp_i(b, 0, 255);
	}

	int cur = luma_rgb(r, g, b);
	if (cur > target_luma && cur > 0) {
		r = r * target_luma / cur;
		g = g * target_luma / cur;
		b = b * target_luma / cur;
	} else if (cur < target_luma && cur < 255) {
		int step = (target_luma - cur) * 255 / (255 - cur);
		r += (255 - r) * step / 255;
		g += (255 - g) * step / 255;
		b += (255 - b) * step / 255;
	}

	return lv_color_make((uint8_t)clamp_i(r, 0, 255), (uint8_t)clamp_i(g, 0, 255), (uint8_t)clamp_i(b, 0, 255));
}

void ambient_palette_fixed(int preset_index, uint32_t fixed_preset_rgb, ambient_palette_t *out) {
	if (!out) {
		return;
	}
	memset(out, 0, sizeof(*out));

	// Default Sonix Atelier — Ambient Edition fixed palette:
	//   Base background: #10141B
	//   Ambient surface: #202A36
	//   Ice accent:      #B9DCFA
	//   Primary text:    #F1F5F9
	//   Secondary text:  #94A3B8
	out->bg_top = lv_color_hex(0x131922);
	out->bg_bottom = lv_color_hex(0x10141B);
	out->surface = lv_color_hex(0x202A36);
	out->surface_pressed = lv_color_hex(0x2B3848);
	out->border = lv_color_hex(0x2B3746);
	out->text_primary = lv_color_hex(0xF1F5F9);
	out->text_secondary = lv_color_hex(0x94A3B8);
	out->wave_unplayed = lv_color_hex(0x334155);

	lv_color_t acc = lv_color_hex(0xB9DCFA);
	if (preset_index > 0 && fixed_preset_rgb != 0) {
		int r = (int)((fixed_preset_rgb >> 16) & 0xFF);
		int g = (int)((fixed_preset_rgb >> 8) & 0xFF);
		int b = (int)(fixed_preset_rgb & 0xFF);
		// Ensure non-default fixed presets remain readable over #10141B.
		int luma = luma_rgb(r, g, b);
		int target = luma < 145 ? 162 : (luma > 215 ? 210 : luma);
		acc = shape_seed(r, g, b, 92, 0, 185, target);
	}

	out->accent = acc;
	out->accent_pressed = lerp_color(acc, lv_color_hex(0x0B0F16), 180);
	out->accent_ink = ambient_contrast_ink(acc);
}

void ambient_palette_adaptive(bool has_cover, uint32_t raw_tone, int mean_luma, ambient_palette_t *out) {
	if (!out) {
		return;
	}
	if (!has_cover || raw_tone == 0) {
		ambient_palette_fixed(0, 0, out);
		return;
	}

	int sr = (int)((raw_tone >> 16) & 0xFF);
	int sg = (int)((raw_tone >> 8) & 0xFF);
	int sb = (int)(raw_tone & 0xFF);

	int hi = sr > sg ? (sr > sb ? sr : sb) : (sg > sb ? sg : sb);
	int lo = sr < sg ? (sr < sb ? sr : sb) : (sg < sb ? sg : sb);
	int chroma = hi - lo;

	// Monochromatic / greyscale / near-black / near-white sleeves: keep a
	// warm-neutral obsidian ground and soft ivory-silver accent rather than
	// amplifying compression rounding noise into a false colour.
	if (chroma < 14) {
		int top_luma = (mean_luma >= 180) ? 24 : 18;
		out->bg_top = lv_color_make((uint8_t)top_luma, (uint8_t)(top_luma + 2), (uint8_t)(top_luma + 5));
		out->bg_bottom = lv_color_hex(0x0D1016);
		out->surface = lv_color_hex(0x1E2530);
		out->surface_pressed = lv_color_hex(0x2A3342);
		out->border = lv_color_hex(0x2C3544);
		out->accent = lv_color_hex(0xE2E8F0);
		out->accent_pressed = lv_color_hex(0xCBD5E1);
		out->accent_ink = lv_color_hex(0x0B0F16);
		out->text_primary = lv_color_hex(0xF1F5F9);
		out->text_secondary = lv_color_hex(0x9BA7B8);
		out->wave_unplayed = lv_color_hex(0x364152);
		return;
	}

	// Subtle album-derived dark ambient canvas: noticeable hue identity above,
	// grounding smoothly into deep obsidian below so text and controls always
	// maintain high contrast.
	int top_luma = 22;
	if (mean_luma >= 0 && mean_luma < 45) {
		top_luma = 17; // very dark covers stay extra deep
	} else if (mean_luma > 185) {
		top_luma = 25; // bright covers get a slightly airier top glow, still dark
	}

	out->bg_top = shape_seed(sr, sg, sb, 58, 12, 42, top_luma);
	out->bg_bottom = shape_seed(sr, sg, sb, 40, 6, 24, 11);
	out->surface = shape_seed(sr, sg, sb, 50, 10, 38, 33);
	out->surface_pressed = shape_seed(sr, sg, sb, 52, 12, 44, 45);
	out->border = shape_seed(sr, sg, sb, 45, 8, 36, 48);

	// Readable audiophile accent: restrained saturation (never raw neon),
	// pinned to luma 188 so it pops cleanly over dark surfaces and pairs with
	// crisp dark ink inside the circular Play/Pause control.
	lv_color_t acc = shape_seed(sr, sg, sb, 85, 48, 138, 188);
	out->accent = acc;
	out->accent_pressed = lerp_color(acc, lv_color_hex(0x0B0F16), 180);
	out->accent_ink = ambient_contrast_ink(acc);

	out->text_primary = lv_color_hex(0xF1F5F9);
	// Secondary text borrows a whisper of the sleeve's warmth/coolness while
	// staying firmly around #94A3B8 readability.
	out->text_secondary = lerp_color(lv_color_hex(0x94A3B8), shape_seed(sr, sg, sb, 35, 0, 32, 162), 280);
	out->wave_unplayed = shape_seed(sr, sg, sb, 40, 8, 34, 62);
}

void ambient_palette_lerp(const ambient_palette_t *a, const ambient_palette_t *b, int t_permille,
						  ambient_palette_t *out) {
	if (!a || !b || !out) {
		return;
	}
	if (t_permille <= 0) {
		*out = *a;
		return;
	}
	if (t_permille >= 1000) {
		*out = *b;
		return;
	}
	out->bg_top = lerp_color(a->bg_top, b->bg_top, t_permille);
	out->bg_bottom = lerp_color(a->bg_bottom, b->bg_bottom, t_permille);
	out->surface = lerp_color(a->surface, b->surface, t_permille);
	out->surface_pressed = lerp_color(a->surface_pressed, b->surface_pressed, t_permille);
	out->border = lerp_color(a->border, b->border, t_permille);
	out->accent = lerp_color(a->accent, b->accent, t_permille);
	out->accent_pressed = lerp_color(a->accent_pressed, b->accent_pressed, t_permille);
	out->accent_ink = ambient_contrast_ink(out->accent);
	out->text_primary = lerp_color(a->text_primary, b->text_primary, t_permille);
	out->text_secondary = lerp_color(a->text_secondary, b->text_secondary, t_permille);
	out->wave_unplayed = lerp_color(a->wave_unplayed, b->wave_unplayed, t_permille);
}
