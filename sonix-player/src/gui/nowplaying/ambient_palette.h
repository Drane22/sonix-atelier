#ifndef AMBIENT_PALETTE_H
#define AMBIENT_PALETTE_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

// Sonix Atelier — Ambient Edition colour tokens for Now Playing & Lyrics.
//
// Predominantly dark surfaces with restrained album-derived hue in Adaptive
// mode, or the signature Slate/Ice fixed palette (#10141B / #202A36 / #B9DCFA)
// in Fixed mode. Designed to guarantee readable text contrast regardless of
// source artwork brightness or saturation, with zero full-screen blur buffers.
typedef struct {
	lv_color_t bg_top;			 // upper vertical gradient stop (dark ambient)
	lv_color_t bg_bottom;		 // lower vertical gradient stop (grounded obsidian)
	lv_color_t surface;			 // secondary pills, empty cover frame, subtle chips
	lv_color_t surface_pressed;	 // pressed state for secondary controls
	lv_color_t border;			 // subtle hairline divider where needed
	lv_color_t accent;			 // active control fill, elapsed waveform, active lyric
	lv_color_t accent_pressed;	 // pressed state for accent circle
	lv_color_t accent_ink;		 // icon/text ink drawn on top of `accent`
	lv_color_t text_primary;	 // primary title & active ink (#F1F5F9)
	lv_color_t text_secondary;	 // artist, timestamps, codec metadata (#94A3B8-class)
	lv_color_t wave_unplayed;	 // remaining waveform bars (muted surface tint)
} ambient_palette_t;

// Default Fixed Ice palette (preset 0) or preset-derived fixed accent (1..5).
// `fixed_preset_rgb` is 0xRRGGBB of the selected theme preset when index > 0,
// or 0 to use the default Ice accent (#B9DCFA).
void ambient_palette_fixed(int preset_index, uint32_t fixed_preset_rgb, ambient_palette_t *out);

// Adaptive album-derived palette from `cover_page_tone()`'s 0xRRGGBB `raw_tone`
// (and `mean_luma` 0..255, or -1 if unknown). Falls back gracefully on missing,
// monochrome, extremely dark, or extremely bright artwork.
void ambient_palette_adaptive(bool has_cover, uint32_t raw_tone, int mean_luma, ambient_palette_t *out);

// Linearly interpolates every token between `a` and `b` for `t_permille` in
// 0..1000, recomputing `accent_ink` for contrast safety at intermediate steps.
void ambient_palette_lerp(const ambient_palette_t *a, const ambient_palette_t *b, int t_permille,
						  ambient_palette_t *out);

// Rec. 601 integer luma (0..255) and dark/light contrast ink helper.
int ambient_color_luma(lv_color_t c);
lv_color_t ambient_contrast_ink(lv_color_t bg);

#endif /* AMBIENT_PALETTE_H */
