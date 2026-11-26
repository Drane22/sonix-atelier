#include "arcade_shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/topbar.h"
#include "src/gui/shell/volume_overlay.h"
#include "src/system/audio/audio.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"

#define MAX_SHELLS 12

typedef struct {
	const char *id;
	int tiers[3]; // Bronze, Silver, Gold
} medal_spec_t;

static const medal_spec_t MEDAL_SPECS[] = {
	{"towerblast", {15, 40, 90}},
	{"blockmosaic", {250, 750, 1800}},
	{"breakwater", {60, 180, 420}},
	{"orbit", {18, 45, 100}},
	{"prism", {8, 20, 40}},
	{"2048", {2500, 8000, 20000}},
	{"word", {200, 400, 500}},
};

struct arcade_shell {
	arcade_game_ops_t ops;
	const char *score_id;
	void *ctx;
	int width;
	int height;
	uint32_t tick_ms;

	lv_obj_t *screen;
	arcade_fb_t fb;
	lv_obj_t *content_layer;
	lv_obj_t *touch_layer;

	// Gameplay HUD
	lv_obj_t *hud_layer;
	lv_obj_t *hud_best_pill;
	lv_obj_t *hud_best_pips[3];
	lv_obj_t *hud_best_label;
	lv_obj_t *hud_score_label;
	lv_obj_t *hud_sub_pill;
	lv_obj_t *hud_sub_label;
	lv_obj_t *hud_callout_pill;
	lv_obj_t *hud_callout_label;
	lv_obj_t *hud_pause_btn;
	lv_obj_t *hud_pause_icon;

	// Persistent top-right Sound button (sits above overlays in every state)
	lv_obj_t *sound_btn;
	lv_obj_t *sound_icon;
	int last_sound_vis;

	// Overlays
	lv_obj_t *title_overlay;
	lv_obj_t *title_card;
	lv_obj_t *title_badge;
	lv_obj_t *title_icon;
	lv_obj_t *title_label;
	lv_obj_t *title_sub;
	lv_obj_t *title_hint;
	lv_obj_t *title_extra;
	lv_obj_t *title_best_pill;
	lv_obj_t *title_best_pips[3];
	lv_obj_t *title_best_label;
	lv_obj_t *title_play_btn;
	lv_obj_t *title_play_lbl;
	lv_obj_t *title_exit_btn;
	lv_obj_t *title_exit_lbl;

	lv_obj_t *pause_overlay;
	lv_obj_t *pause_card;
	lv_obj_t *pause_heading;
	lv_obj_t *pause_stat_label;
	lv_obj_t *pause_resume_btn;
	lv_obj_t *pause_resume_lbl;
	lv_obj_t *pause_restart_btn;
	lv_obj_t *pause_restart_lbl;
	lv_obj_t *pause_exit_btn;
	lv_obj_t *pause_exit_lbl;

	lv_obj_t *over_overlay;
	lv_obj_t *over_card;
	lv_obj_t *over_heading;
	lv_obj_t *over_new_best_pill;
	lv_obj_t *over_new_best_lbl;
	lv_obj_t *over_medal_row;
	lv_obj_t *over_medal_pips[3];
	lv_obj_t *over_next_lbl;
	lv_obj_t *over_summary_lbl;
	lv_obj_t *over_stats_box;
	lv_obj_t *over_score_cap;
	lv_obj_t *over_score_val;
	lv_obj_t *over_best_cap;
	lv_obj_t *over_best_val;
	lv_obj_t *over_continue_btn;
	lv_obj_t *over_continue_lbl;
	void (*over_continue_cb)(arcade_shell_t *shell, void *ctx);
	lv_obj_t *over_again_btn;
	lv_obj_t *over_again_lbl;
	lv_obj_t *over_exit_btn;
	lv_obj_t *over_exit_lbl;

	lv_timer_t *callout_timer;
	lv_timer_t *over_delay_timer;

	arcade_state_t state;
	bool active;
	bool dirty;
	bool full_redraw;
	bool best_dirty;
	bool over_settling;
	bool new_best_this_run;
	bool has_sub_hud;
	bool custom_end_heading;
	bool hide_end_score;
	int score;
	int best;
	arcade_medal_t medal_before_run;
	float music_pulse;

	int64_t last_us;
	struct {
		int64_t since_us;
		uint32_t frames;
		uint32_t late;
		uint32_t slow;
		int64_t work_us;
		int64_t longest_work_us;
	} stats;
};

static arcade_shell_t *registered_shells[MAX_SHELLS];
static int registered_count = 0;
static lv_timer_t *shared_tick_timer = NULL;
static arcade_shell_t *active_tick_shell = NULL;

static int64_t now_us(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}

static void best_key(const char *id, char *out, size_t out_sz) { snprintf(out, out_sz, "%s_best", id); }

bool arcade_medal_thresholds(const char *game_id, int out_tiers[3]) {
	if (!game_id || !game_id[0]) {
		return false;
	}
	for (size_t i = 0; i < sizeof(MEDAL_SPECS) / sizeof(MEDAL_SPECS[0]); i++) {
		if (strcmp(MEDAL_SPECS[i].id, game_id) == 0) {
			if (out_tiers) {
				out_tiers[0] = MEDAL_SPECS[i].tiers[0];
				out_tiers[1] = MEDAL_SPECS[i].tiers[1];
				out_tiers[2] = MEDAL_SPECS[i].tiers[2];
			}
			return true;
		}
	}
	return false;
}

arcade_medal_t arcade_medal_for_score(const char *game_id, int score, int *next_target_out) {
	int tiers[3] = {0, 0, 0};
	if (!arcade_medal_thresholds(game_id, tiers)) {
		if (next_target_out) *next_target_out = 0;
		return ARCADE_MEDAL_NONE;
	}
	if (score >= tiers[2]) {
		if (next_target_out) *next_target_out = 0;
		return ARCADE_MEDAL_GOLD;
	}
	if (score >= tiers[1]) {
		if (next_target_out) *next_target_out = tiers[2];
		return ARCADE_MEDAL_SILVER;
	}
	if (score >= tiers[0]) {
		if (next_target_out) *next_target_out = tiers[1];
		return ARCADE_MEDAL_BRONZE;
	}
	if (next_target_out) *next_target_out = tiers[0];
	return ARCADE_MEDAL_NONE;
}

lv_color_t arcade_medal_color(arcade_medal_t medal, bool dark) {
	switch (medal) {
	case ARCADE_MEDAL_BRONZE:
		return dark ? lv_color_make(212, 138, 86) : lv_color_make(182, 108, 56);
	case ARCADE_MEDAL_SILVER:
		return dark ? lv_color_make(194, 210, 228) : lv_color_make(118, 136, 158);
	case ARCADE_MEDAL_GOLD:
		return dark ? lv_color_make(247, 198, 76) : lv_color_make(214, 154, 22);
	default:
		return dark ? lv_color_make(58, 70, 86) : lv_color_make(198, 206, 218);
	}
}

bool arcade_game_has_saved_run(const char *game_id) {
	if (!game_id || !game_id[0]) {
		return false;
	}
	if (strcmp(game_id, "word") == 0) {
		time_t now = time(NULL);
		struct tm today;
		localtime_r(&now, &today);
		long day_id = (long)((today.tm_year + 1900) * 1000 + today.tm_yday + 1);
		if (config_get_int("word", "completed", 0) == day_id) {
			return false;
		}
		if (config_get_int("word", "day", 0) == day_id) {
			const char *g = config_get("word", "guesses", "");
			return g && g[0] != '\0';
		}
		return false;
	}
	char key[64];
	snprintf(key, sizeof(key), "%s_saved", game_id);
	return config_get_int("arcade", key, 0) != 0;
}

static lv_color_t legible_accent_text(lv_color_t c, bool dark_surface) {
	if (dark_surface) {
		return c;
	}
	int luma = (299 * c.red + 587 * c.green + 114 * c.blue) / 1000;
	if (luma <= 115) {
		return c;
	}
	return lv_color_make((uint8_t)((c.red * 100) / luma), (uint8_t)((c.green * 100) / luma),
						 (uint8_t)((c.blue * 100) / luma));
}

int arcade_shell_read_best(const char *game_id) {
	if (!game_id || !game_id[0]) {
		return 0;
	}
	char key[64];
	best_key(game_id, key, sizeof(key));
	long v = config_get_int("arcade", key, 0);
	return v > 0 ? (int)v : 0;
}

static void save_best_if_dirty(arcade_shell_t *s) {
	if (!s || !s->best_dirty) {
		return;
	}
	char key[64];
	best_key(s->score_id, key, sizeof(key));
	config_set_int("arcade", key, s->best);
	config_save();
	s->best_dirty = false;
}

static void paint_medal_pips(lv_obj_t *pips[3], arcade_medal_t earned, const theme_palette_t *pal) {
	for (int i = 0; i < 3; i++) {
		if (!pips[i]) continue;
		arcade_medal_t tier = (arcade_medal_t)(i + 1);
		bool lit = (int)earned >= (int)tier;
		lv_color_t mc = arcade_medal_color(tier, pal->dark);
		if (lit) {
			lv_obj_set_style_bg_color(pips[i], mc, 0);
			lv_obj_set_style_bg_opa(pips[i], LV_OPA_COVER, 0);
			lv_obj_set_style_border_color(pips[i], mc, 0);
		} else {
			lv_obj_set_style_bg_color(pips[i], pal->surface, 0);
			lv_obj_set_style_bg_opa(pips[i], 60, 0);
			lv_obj_set_style_border_color(pips[i], pal->border, 0);
		}
	}
}

static int current_sound_vis(void) {
	if (!arcade_sfx_enabled()) return 0;
	if (arcade_sfx_yielding_to_music()) return 1;
	return 2;
}

static void refresh_sound_btn(arcade_shell_t *s) {
	if (!s || !s->sound_btn || !s->sound_icon) {
		return;
	}
	const theme_palette_t *pal = theme();
	int vis = current_sound_vis();
	s->last_sound_vis = vis;

	if (vis == 0) {
		lv_image_set_src(s->sound_icon, &icon_volume_mute);
		lv_obj_set_style_image_recolor(s->sound_icon, pal->text_secondary, 0);
		lv_obj_set_style_image_opa(s->sound_icon, LV_OPA_60, 0);
		lv_obj_set_style_border_color(s->sound_btn, pal->border, 0);
	} else if (vis == 1) {
		// Smart-Yielding to active music: headphones glyph shows music owns the DAC.
		lv_image_set_src(s->sound_icon, &icon_headphones);
		lv_obj_set_style_image_recolor(s->sound_icon, pal->accent, 0);
		lv_obj_set_style_image_opa(s->sound_icon, LV_OPA_90, 0);
		lv_obj_set_style_border_color(s->sound_btn, pal->border, 0);
	} else {
		lv_image_set_src(s->sound_icon, &icon_volume_high);
		lv_obj_set_style_image_recolor(s->sound_icon, pal->text_primary, 0);
		lv_obj_set_style_image_opa(s->sound_icon, LV_OPA_COVER, 0);
		lv_obj_set_style_border_color(s->sound_btn, pal->accent, 0);
	}
}

static void refresh_labels_text(arcade_shell_t *s) {
	const theme_palette_t *pal = theme();
	char buf[80];
	snprintf(buf, sizeof(buf), "%s %d", tr("arcade_best"), s->best);
	if (s->hud_best_label) {
		lv_label_set_text(s->hud_best_label, buf);
	}
	if (s->title_best_label) {
		lv_label_set_text(s->title_best_label, buf);
	}
	if (s->title_best_pill) {
		lv_obj_set_hidden(s->title_best_pill, s->best <= 0);
	}

	int next_from_best = 0;
	arcade_medal_t best_medal = arcade_medal_for_score(s->score_id, s->best, &next_from_best);
	paint_medal_pips(s->hud_best_pips, best_medal, pal);
	paint_medal_pips(s->title_best_pips, best_medal, pal);

	snprintf(buf, sizeof(buf), "%d", s->score);
	if (s->hud_score_label) {
		lv_label_set_text(s->hud_score_label, s->ops.title_in_hud ? tr(s->ops.title_tag) : buf);
	}
	if (s->pause_stat_label) {
		snprintf(buf, sizeof(buf), "%s %d  ·  %s %d", tr("arcade_score"), s->score, tr("arcade_best"), s->best);
		lv_label_set_text(s->pause_stat_label, buf);
	}
	if (s->over_score_val) {
		snprintf(buf, sizeof(buf), "%d", s->score);
		lv_label_set_text(s->over_score_val, buf);
	}
	if (s->over_best_val) {
		snprintf(buf, sizeof(buf), "%d", s->best);
		lv_label_set_text(s->over_best_val, buf);
	}
	if (s->over_new_best_pill) {
		lv_obj_set_hidden(s->over_new_best_pill, !s->new_best_this_run || s->hide_end_score);
	}

	if (s->over_stats_box) lv_obj_set_hidden(s->over_stats_box, s->hide_end_score);

	// Game Over medal showcase + "Next medal" hook
	int next_target = 0;
	arcade_medal_t run_or_best = arcade_medal_for_score(s->score_id, s->best, &next_target);
	bool has_tiers = arcade_medal_thresholds(s->score_id, NULL);
	if (s->over_medal_row) {
		lv_obj_set_hidden(s->over_medal_row, !has_tiers || s->hide_end_score);
		paint_medal_pips(s->over_medal_pips, run_or_best, pal);
	}
	if (s->over_next_lbl) {
		if (has_tiers && next_target > 0) {
			snprintf(buf, sizeof(buf), "★  %d / %d", s->score, next_target);
			lv_label_set_text(s->over_next_lbl, buf);
			lv_obj_set_hidden(s->over_next_lbl, false);
		} else {
			lv_obj_set_hidden(s->over_next_lbl, true);
		}
	}
}

static void style_primary_btn(lv_obj_t *btn, lv_obj_t *lbl, const theme_palette_t *pal) {
	if (!btn || !lbl) {
		return;
	}
	lv_obj_set_style_bg_color(btn, pal->accent, 0);
	lv_obj_set_style_bg_color(btn, pal->accent_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
	lv_obj_set_style_text_color(lbl, pal->accent_ink, 0);
}

static void style_secondary_btn(lv_obj_t *btn, lv_obj_t *lbl, const theme_palette_t *pal) {
	if (!btn || !lbl) {
		return;
	}
	lv_obj_set_style_bg_color(btn, pal->elevated, 0);
	lv_obj_set_style_bg_color(btn, pal->surface_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(btn, pal->border, 0);
	lv_obj_set_style_border_width(btn, 1, 0);
	lv_obj_set_style_text_color(lbl, pal->text_primary, 0);
}

static void style_ghost_btn(lv_obj_t *btn, lv_obj_t *lbl, const theme_palette_t *pal) {
	if (!btn || !lbl) {
		return;
	}
	lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_bg_color(btn, pal->surface_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_set_style_text_color(lbl, pal->text_secondary, 0);
}

static void style_card(lv_obj_t *card, const theme_palette_t *pal) {
	if (!card) {
		return;
	}
	lv_obj_set_style_bg_color(card, pal->surface, 0);
	lv_obj_set_style_bg_opa(card, 248, 0);
	lv_obj_set_style_border_color(card, pal->border, 0);
	lv_obj_set_style_border_width(card, 1, 0);
	lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
}

static void refresh_over_buttons(arcade_shell_t *s, const theme_palette_t *pal) {
	if (!s || !pal) {
		return;
	}
	if (s->over_continue_btn && s->over_continue_cb) {
		style_primary_btn(s->over_continue_btn, s->over_continue_lbl, pal);
		style_secondary_btn(s->over_again_btn, s->over_again_lbl, pal);
	} else {
		style_primary_btn(s->over_again_btn, s->over_again_lbl, pal);
	}
}

static void apply_shell_theme(arcade_shell_t *s) {
	if (!s) {
		return;
	}
	const theme_palette_t *pal = theme();
	lv_color_t scrim = pal->dark ? lv_color_hex(0x06080B) : lv_color_hex(0x101822);
	lv_color_t ink_acc = legible_accent_text(pal->accent, pal->dark);

	lv_obj_set_style_bg_color(s->screen, pal->screen_bg, 0);

	// HUD styling
	lv_obj_set_style_bg_color(s->hud_best_pill, pal->surface, 0);
	lv_obj_set_style_bg_opa(s->hud_best_pill, 215, 0);
	lv_obj_set_style_border_color(s->hud_best_pill, pal->border, 0);
	lv_obj_set_style_text_color(s->hud_best_label, pal->text_secondary, 0);
	lv_obj_set_style_text_color(s->hud_score_label, (!s->ops.title_in_hud || s->ops.dark_backdrop) ? lv_color_white() : pal->text_primary, 0);

	lv_obj_set_style_bg_color(s->hud_sub_pill, pal->surface, 0);
	lv_obj_set_style_bg_opa(s->hud_sub_pill, 215, 0);
	lv_obj_set_style_border_color(s->hud_sub_pill, pal->border, 0);
	lv_obj_set_style_text_color(s->hud_sub_label, pal->text_secondary, 0);

	lv_obj_set_style_bg_color(s->hud_callout_pill, pal->surface, 0);
	lv_obj_set_style_bg_opa(s->hud_callout_pill, 225, 0);
	lv_obj_set_style_border_color(s->hud_callout_pill, pal->border, 0);

	lv_obj_set_style_bg_color(s->hud_pause_btn, pal->surface, 0);
	lv_obj_set_style_bg_color(s->hud_pause_btn, pal->surface_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(s->hud_pause_btn, 220, 0);
	lv_obj_set_style_border_color(s->hud_pause_btn, pal->border, 0);
	lv_obj_set_style_image_recolor(s->hud_pause_icon, pal->text_primary, 0);

	lv_obj_set_style_bg_color(s->sound_btn, pal->surface, 0);
	lv_obj_set_style_bg_color(s->sound_btn, pal->surface_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(s->sound_btn, 220, 0);
	refresh_sound_btn(s);

	// Title overlay
	lv_obj_set_style_bg_color(s->title_overlay, scrim, 0);
	lv_obj_set_style_bg_opa(s->title_overlay, pal->dark ? 110 : 75, 0);
	style_card(s->title_card, pal);
	lv_obj_set_style_bg_color(s->title_badge, pal->elevated, 0);
	lv_obj_set_style_text_color(s->title_label, pal->text_primary, 0);
	lv_obj_set_style_text_color(s->title_sub, pal->text_secondary, 0);
	lv_obj_set_style_text_color(s->title_hint, pal->text_secondary, 0);
	lv_obj_set_style_bg_color(s->title_best_pill, pal->elevated, 0);
	lv_obj_set_style_border_color(s->title_best_pill, pal->border, 0);
	lv_obj_set_style_text_color(s->title_best_label, ink_acc, 0);
	style_primary_btn(s->title_play_btn, s->title_play_lbl, pal);
	style_ghost_btn(s->title_exit_btn, s->title_exit_lbl, pal);

	// Pause overlay
	lv_obj_set_style_bg_color(s->pause_overlay, scrim, 0);
	lv_obj_set_style_bg_opa(s->pause_overlay, pal->dark ? 165 : 120, 0);
	style_card(s->pause_card, pal);
	lv_obj_set_style_text_color(s->pause_heading, pal->text_primary, 0);
	lv_obj_set_style_text_color(s->pause_stat_label, pal->text_secondary, 0);
	style_primary_btn(s->pause_resume_btn, s->pause_resume_lbl, pal);
	style_secondary_btn(s->pause_restart_btn, s->pause_restart_lbl, pal);
	style_ghost_btn(s->pause_exit_btn, s->pause_exit_lbl, pal);

	// Game over overlay
	lv_obj_set_style_bg_color(s->over_overlay, scrim, 0);
	lv_obj_set_style_bg_opa(s->over_overlay, pal->dark ? 170 : 125, 0);
	style_card(s->over_card, pal);
	lv_obj_set_style_text_color(s->over_heading, pal->text_primary, 0);
	lv_obj_set_style_bg_color(s->over_new_best_pill, pal->accent, 0);
	lv_obj_set_style_text_color(s->over_new_best_lbl, pal->accent_ink, 0);
	if (s->over_next_lbl) {
		lv_obj_set_style_text_color(s->over_next_lbl, pal->text_secondary, 0);
	}
	lv_obj_set_style_text_color(s->over_summary_lbl, pal->text_secondary, 0);
	lv_obj_set_style_bg_color(s->over_stats_box, pal->elevated, 0);
	lv_obj_set_style_border_color(s->over_stats_box, pal->border, 0);
	lv_obj_set_style_text_color(s->over_score_cap, pal->text_secondary, 0);
	lv_obj_set_style_text_color(s->over_score_val, pal->text_primary, 0);
	lv_obj_set_style_text_color(s->over_best_cap, pal->text_secondary, 0);
	lv_obj_set_style_text_color(s->over_best_val, ink_acc, 0);
	refresh_over_buttons(s, pal);
	style_ghost_btn(s->over_exit_btn, s->over_exit_lbl, pal);

	refresh_labels_text(s);

	if (s->active && s->ops.on_theme_changed) {
		s->ops.on_theme_changed(s, s->ctx);
		if (s->ops.use_framebuffer || s->ops.on_render) {
			s->dirty = true;
			s->full_redraw = true;
		}
	}
}

static void refresh_all_themes(void) {
	for (int i = 0; i < registered_count; i++) {
		apply_shell_theme(registered_shells[i]);
	}
}

static void reset_run_overrides(arcade_shell_t *s) {
	if (!s) {
		return;
	}
	s->hide_end_score = false;
	s->custom_end_heading = false;
	if (s->over_heading) {
		lv_label_set_text(s->over_heading, tr("arcade_game_over"));
	}
	if (s->over_summary_lbl) {
		lv_label_set_text(s->over_summary_lbl, "");
		lv_obj_set_hidden(s->over_summary_lbl, true);
	}
	s->over_continue_cb = NULL;
	if (s->over_continue_btn) {
		lv_obj_set_hidden(s->over_continue_btn, true);
	}
	s->has_sub_hud = false;
	if (s->hud_sub_pill) {
		lv_obj_set_hidden(s->hud_sub_pill, true);
	}
	if (s->hud_callout_pill) {
		lv_obj_align(s->hud_callout_pill, LV_ALIGN_TOP_MID, 0, 114);
	}
	refresh_over_buttons(s, theme());
}

static void set_state_ui(arcade_shell_t *s, arcade_state_t st) {
	if (s->state == ARCADE_ST_PLAYING && st != ARCADE_ST_PLAYING && s->ops.on_cancel) {
		s->ops.on_cancel(s, s->ctx);
	}
	s->state = st;
	if (s->active && s->ops.music_reactive)
		audio_enable_music_envelope(st == ARCADE_ST_TITLE || st == ARCADE_ST_PLAYING);
	if (active_tick_shell == s && (st == ARCADE_ST_TITLE || st == ARCADE_ST_PLAYING)) {
		s->last_us = now_us();
		lv_timer_resume(shared_tick_timer);
	}
	lv_obj_set_hidden(s->title_overlay, st != ARCADE_ST_TITLE);
	lv_obj_set_hidden(s->pause_overlay, st != ARCADE_ST_PAUSED);
	lv_obj_set_hidden(s->over_overlay, st != ARCADE_ST_GAMEOVER);

	bool show_hud = (st == ARCADE_ST_PLAYING || st == ARCADE_ST_PAUSED);
	lv_obj_set_hidden(s->hud_layer, !show_hud);
	lv_obj_set_hidden(s->hud_pause_btn, st != ARCADE_ST_PLAYING);

	if (s->sound_btn) {
		lv_obj_align(s->sound_btn, LV_ALIGN_TOP_RIGHT, st == ARCADE_ST_PLAYING ? -70 : -18, 16);
		lv_obj_move_foreground(s->sound_btn);
		refresh_sound_btn(s);
	}

	if (st != ARCADE_ST_PLAYING && s->hud_callout_pill) {
		lv_obj_set_hidden(s->hud_callout_pill, true);
	}
	refresh_labels_text(s);
}

bool arcade_shell_pause_for_screen_off(void) {
	arcade_shell_t *s = active_tick_shell;
	if (!s || !s->active || s->state != ARCADE_ST_PLAYING || s->over_settling) return false;
	set_state_ui(s, ARCADE_ST_PAUSED);
	return true;
}

static void begin_run(arcade_shell_t *s, bool resume) {
	if (s->over_delay_timer) {
		lv_timer_delete(s->over_delay_timer);
		s->over_delay_timer = NULL;
	}
	s->over_settling = false;
	s->new_best_this_run = false;
	s->score = 0;
	s->medal_before_run = arcade_medal_for_score(s->score_id, s->best, NULL);
	reset_run_overrides(s);
	set_state_ui(s, ARCADE_ST_PLAYING);
	arcade_sfx_play(ARCADE_SFX_TAP);
	if (resume && s->ops.has_saved_run && s->ops.has_saved_run(s->ctx) && s->ops.on_resume_saved) {
		s->ops.on_resume_saved(s, s->ctx);
	} else if (s->ops.on_start) {
		s->ops.on_start(s, s->ctx);
	}
	if (s->ops.use_framebuffer || s->ops.on_render) {
		s->dirty = true;
		s->full_redraw = true;
	}
}

static void start_new_run(arcade_shell_t *s) { begin_run(s, false); }

void arcade_shell_switch_mode(arcade_shell_t *s, const char *score_id, bool resume) {
	save_best_if_dirty(s);
	s->score_id = score_id;
	s->best = arcade_shell_read_best(score_id);
	begin_run(s, resume);
}

static void callout_timer_cb(lv_timer_t *t) {
	arcade_shell_t *s = (arcade_shell_t *)lv_timer_get_user_data(t);
	if (s && s->hud_callout_pill) {
		lv_obj_set_hidden(s->hud_callout_pill, true);
	}
	if (s) {
		s->callout_timer = NULL;
	}
	lv_timer_delete(t);
}

static void over_delay_cb(lv_timer_t *t) {
	arcade_shell_t *s = (arcade_shell_t *)lv_timer_get_user_data(t);
	if (s) {
		s->over_delay_timer = NULL;
		s->over_settling = false;
		save_best_if_dirty(s);
		if (s->active) {
			set_state_ui(s, ARCADE_ST_GAMEOVER);
		}
	}
	lv_timer_delete(t);
}

static void touch_event_cb(lv_event_t *e) {
	arcade_shell_t *s = (arcade_shell_t *)lv_event_get_user_data(e);
	if (!s || !s->active || s->state != ARCADE_ST_PLAYING || s->over_settling) {
		return;
	}
	lv_event_code_t code = lv_event_get_code(e);
	if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED &&
		code != LV_EVENT_PRESS_LOST) {
		return;
	}
	lv_point_t pt = {s->width / 2, s->height / 2};
	lv_indev_t *indev = lv_indev_active();
	if (indev) {
		lv_indev_get_point(indev, &pt);
	}
	if (code == LV_EVENT_PRESSING) {
		if (s->ops.on_drag) {
			s->ops.on_drag(s, s->ctx, (int)pt.x, (int)pt.y);
		}
		return;
	}
	if (code == LV_EVENT_PRESS_LOST && s->ops.on_cancel) {
		s->ops.on_cancel(s, s->ctx);
		return;
	}
	if (s->ops.on_touch) {
		bool pressed = (code == LV_EVENT_PRESSED);
		s->ops.on_touch(s, s->ctx, pressed, (int)pt.x, (int)pt.y);
	}
}

static void play_btn_cb(lv_event_t *e) {
	arcade_shell_t *s = (arcade_shell_t *)lv_event_get_user_data(e);
	if (s) {
		start_new_run(s);
	}
}

static void continue_btn_cb(lv_event_t *e) {
	arcade_shell_t *s = (arcade_shell_t *)lv_event_get_user_data(e);
	if (s && s->active && s->state == ARCADE_ST_GAMEOVER && s->over_continue_cb) {
		arcade_sfx_play(ARCADE_SFX_TAP);
		s->over_continue_cb(s, s->ctx);
	}
}

static void pause_btn_cb(lv_event_t *e) {
	arcade_shell_t *s = (arcade_shell_t *)lv_event_get_user_data(e);
	if (s && s->active && s->state == ARCADE_ST_PLAYING && !s->over_settling) {
		arcade_sfx_play(ARCADE_SFX_TAP);
		set_state_ui(s, ARCADE_ST_PAUSED);
	}
}

static void sound_btn_cb(lv_event_t *e) {
	arcade_shell_t *s = (arcade_shell_t *)lv_event_get_user_data(e);
	arcade_sfx_toggle();
	if (s) {
		refresh_sound_btn(s);
	}
}

static void resume_btn_cb(lv_event_t *e) {
	arcade_shell_t *s = (arcade_shell_t *)lv_event_get_user_data(e);
	if (s && s->active && s->state == ARCADE_ST_PAUSED) {
		arcade_sfx_play(ARCADE_SFX_TAP);
		s->last_us = now_us();
		set_state_ui(s, ARCADE_ST_PLAYING);
		if (s->ops.use_framebuffer || s->ops.on_render) {
			s->dirty = true;
		}
	}
}

static void exit_btn_cb(lv_event_t *e) {
	(void)e;
	back_btn_cb(NULL);
}

static void tick_cb(lv_timer_t *timer) {
	arcade_shell_t *s = active_tick_shell;
	if (!s || !s->active) {
		lv_timer_pause(timer);
		return;
	}

	int vis = current_sound_vis();
	if (vis != s->last_sound_vis) {
		refresh_sound_btn(s);
	}

	int env = s->ops.music_reactive ? audio_get_music_envelope() : 0;
	float target_pulse = (float)env / 1000.0f;
	float prev_pulse = s->music_pulse;
	s->music_pulse = target_pulse > prev_pulse ? (prev_pulse * 0.35f + target_pulse * 0.65f)
											   : (prev_pulse * 0.82f + target_pulse * 0.18f);
	if (s->music_pulse < 0.005f) {
		s->music_pulse = 0.0f;
	}
	bool pulse_moved = (s->ops.music_reactive && (s->state == ARCADE_ST_PLAYING || s->state == ARCADE_ST_TITLE) &&
						(s->music_pulse > 0.02f || prev_pulse > 0.02f));

	bool stepping = s->ops.on_step && ((s->state == ARCADE_ST_TITLE && s->ops.on_render) || s->state == ARCADE_ST_PLAYING);
	if (stepping && s->state == ARCADE_ST_PLAYING && s->ops.is_idle) {
		stepping = !s->ops.is_idle(s->ctx);
	}
	if (!stepping && !s->dirty && !pulse_moved) {
		lv_timer_pause(timer);
		return;
	}

	int64_t now = now_us();
	int64_t elapsed = now - s->last_us;
	s->last_us = now;

	float dt = (float)elapsed / 1000000.0f;
	if (dt < 0.0f) {
		dt = 0.0f;
	}
	if (dt > 0.05f) {
		dt = 0.05f;
	}

	if (elapsed > (int64_t)s->tick_ms * 1500LL) {
		s->stats.late++;
	}

	if (stepping) {
		s->ops.on_step(s, s->ctx, dt);
		if (s->ops.use_framebuffer || s->ops.on_render) {
			s->dirty = true;
		}
	} else if (pulse_moved) {
		s->dirty = true;
	}

	if (s->dirty) {
		s->dirty = false;
		bool full = s->full_redraw;
		s->full_redraw = false;
		if (s->ops.on_render) {
			s->ops.on_render(s, s->ctx, full);
		}
		if (s->ops.use_framebuffer) {
			if (s->ops.compact_scene && !s->fb.displayed) arcade_fb_mark_full(&s->fb);
			arcade_fb_flush(&s->fb);
		}
		int64_t work = now_us() - now;
		if (work > s->stats.longest_work_us) {
			s->stats.longest_work_us = work;
		}
		if (work > (int64_t)s->tick_ms * 1000LL) {
			s->stats.slow++;
		}
		s->stats.work_us += work;
		s->stats.frames++;
	}
}

static void screen_loaded_cb(lv_event_t *e) {
	arcade_shell_t *s = (arcade_shell_t *)lv_event_get_user_data(e);
	if (!s) {
		return;
	}
	topbar_set_hidden(true);
	back_btn_force_hidden(true);
	back_gesture_blocked(true);
	volume_overlay_allow_outside_player(true);

	arcade_sfx_enter_game();
	refresh_sound_btn(s);

	s->active = true;
	audio_enable_music_envelope(s->ops.music_reactive && (s->state == ARCADE_ST_TITLE || s->state == ARCADE_ST_PLAYING));
	s->last_us = now_us();
	memset(&s->stats, 0, sizeof(s->stats));
	s->stats.since_us = s->last_us;
	if ((s->ops.use_framebuffer || s->ops.on_step || s->ops.on_render) && shared_tick_timer) {
		s->dirty = true;
		s->full_redraw = true;
		active_tick_shell = s;
		lv_timer_set_period(shared_tick_timer, s->tick_ms);
		lv_timer_resume(shared_tick_timer);
		lv_timer_ready(shared_tick_timer);
	}
}

static void screen_unloaded_cb(lv_event_t *e) {
	arcade_shell_t *s = (arcade_shell_t *)lv_event_get_user_data(e);
	if (!s) {
		return;
	}
	s->active = false;
	arcade_sfx_exit_game();

	if (s->stats.frames > 0) {
		printf("arcade[%s]: %u frames in %.1f s, %u late ticks; frames %.2f ms avg, %u over %ums, longest %.2f ms\n",
			   s->ops.id ? s->ops.id : "game", s->stats.frames, (double)(now_us() - s->stats.since_us) / 1e6,
			   s->stats.late, (double)s->stats.work_us / (double)s->stats.frames / 1000.0, s->stats.slow,
			   (unsigned)s->tick_ms, (double)s->stats.longest_work_us / 1000.0);
	}
	if (s->callout_timer) {
		lv_timer_delete(s->callout_timer);
		s->callout_timer = NULL;
	}
	if (s->over_delay_timer) {
		lv_timer_delete(s->over_delay_timer);
		s->over_delay_timer = NULL;
	}
	s->over_settling = false;
	save_best_if_dirty(s);
	if (active_tick_shell == s) {
		active_tick_shell = NULL;
		audio_enable_music_envelope(false);
		if (shared_tick_timer) {
			lv_timer_pause(shared_tick_timer);
		}
	}

	if (s->ops.on_exit) {
		s->ops.on_exit(s, s->ctx);
	}
	if (s->ops.use_framebuffer) {
		arcade_fb_free(&s->fb);
	}

	volume_overlay_allow_outside_player(false);
	back_gesture_blocked(false);
	back_btn_force_hidden(false);
	topbar_set_hidden(false);
}

static lv_obj_t *make_button(lv_obj_t *parent, int h, int radius, const lv_font_t *font, const char *tag,
							 lv_event_cb_t cb, arcade_shell_t *s, lv_obj_t **lbl_out) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_width(btn, lv_pct(100));
	lv_obj_set_height(btn, h);
	lv_obj_set_style_radius(btn, radius, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_event_bubble(btn, false);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, s);

	lv_obj_t *lbl = lv_label_create(btn);
	lv_label_set_text(lbl, tr(tag));
	lv_obj_set_style_text_font(lbl, font, 0);
	lv_obj_center(lbl);
	*lbl_out = lbl;
	return btn;
}

static lv_obj_t *make_overlay_root(lv_obj_t *screen) {
	lv_obj_t *ov = lv_obj_create(screen);
	lv_obj_remove_style_all(ov);
	lv_obj_set_size(ov, lv_pct(100), lv_pct(100));
	lv_obj_set_style_bg_opa(ov, LV_OPA_50, 0);
	lv_obj_set_scrollable(ov, false);
	lv_obj_set_clickable(ov, true);
	lv_obj_set_event_bubble(ov, false);
	return ov;
}

static lv_obj_t *make_modal_card(lv_obj_t *parent, int w) {
	lv_obj_t *card = lv_obj_create(parent);
	lv_obj_set_width(card, w);
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_set_style_radius(card, 24, 0);
	lv_obj_set_style_pad_hor(card, 24, 0);
	lv_obj_set_style_pad_ver(card, 22, 0);
	lv_obj_set_style_pad_row(card, 10, 0);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_event_bubble(card, false);
	lv_obj_center(card);
	return card;
}

static void build_pip_trio(lv_obj_t *parent, int sz, lv_obj_t *out_pips[3]) {
	for (int i = 0; i < 3; i++) {
		lv_obj_t *pip = lv_obj_create(parent);
		lv_obj_remove_style_all(pip);
		lv_obj_set_size(pip, sz, sz);
		lv_obj_set_style_radius(pip, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_border_width(pip, 1, 0);
		lv_obj_set_style_border_opa(pip, LV_OPA_COVER, 0);
		lv_obj_set_scrollable(pip, false);
		lv_obj_set_clickable(pip, false);
		out_pips[i] = pip;
	}
}

arcade_shell_t *arcade_shell_create(gui_config_t *cfg, const arcade_game_ops_t *ops, void *ctx) {
	if (!cfg || !ops) {
		return NULL;
	}
	arcade_lut_init();

	arcade_shell_t *s = calloc(1, sizeof(*s));
	if (!s) {
		return NULL;
	}
	s->ops = *ops;
	s->ctx = ctx;
	s->width = (int)cfg->screen_width;
	s->height = (int)cfg->screen_height;
	s->last_sound_vis = -1;
	int fps = ops->target_fps > 0 ? ops->target_fps : 30;
	s->tick_ms = (uint32_t)(1000 / fps);
	if (s->tick_ms < 16) {
		s->tick_ms = 16;
	}

	s->screen = lv_obj_create(NULL);
	lv_obj_set_style_bg_opa(s->screen, LV_OPA_COVER, 0);
	lv_obj_set_scrollable(s->screen, false);
	lv_obj_add_event_cb(s->screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, s);
	lv_obj_add_event_cb(s->screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, s);

	if (ops->use_framebuffer) {
		arcade_fb_init(&s->fb, s->screen, ops->compact_scene ? 240 : s->width,
					   ops->compact_scene ? 400 : s->height);
		if (ops->compact_scene) {
			lv_obj_set_size(s->fb.img, s->width, s->height);
			lv_image_set_antialias(s->fb.img, false);
			lv_image_set_inner_align(s->fb.img, LV_IMAGE_ALIGN_STRETCH);
		}
	}

	// Native LVGL widget layer (used by the puzzle games underneath overlays)
	s->content_layer = lv_obj_create(s->screen);
	lv_obj_remove_style_all(s->content_layer);
	lv_obj_set_size(s->content_layer, s->width, s->height);
	lv_obj_set_pos(s->content_layer, 0, 0);
	lv_obj_set_scrollable(s->content_layer, false);
	lv_obj_set_clickable(s->content_layer, false);

	// Full-screen touch catcher underneath HUD pause & overlays
	s->touch_layer = lv_obj_create(s->screen);
	lv_obj_remove_style_all(s->touch_layer);
	lv_obj_set_size(s->touch_layer, s->width, s->height);
	lv_obj_set_scrollable(s->touch_layer, false);
	bool need_touch_layer = ops->use_framebuffer || ops->on_touch || ops->on_drag;
	lv_obj_set_clickable(s->touch_layer, need_touch_layer);
	lv_obj_set_hidden(s->touch_layer, !need_touch_layer);
	lv_obj_add_event_cb(s->touch_layer, touch_event_cb, LV_EVENT_PRESSED, s);
	lv_obj_add_event_cb(s->touch_layer, touch_event_cb, LV_EVENT_PRESSING, s);
	lv_obj_add_event_cb(s->touch_layer, touch_event_cb, LV_EVENT_RELEASED, s);
	lv_obj_add_event_cb(s->touch_layer, touch_event_cb, LV_EVENT_PRESS_LOST, s);

	// Gameplay HUD layer
	s->hud_layer = lv_obj_create(s->screen);
	lv_obj_remove_style_all(s->hud_layer);
	lv_obj_set_size(s->hud_layer, s->width, 190);
	lv_obj_set_pos(s->hud_layer, 0, 0);
	lv_obj_set_scrollable(s->hud_layer, false);
	lv_obj_set_clickable(s->hud_layer, false);
	lv_obj_set_hidden(s->hud_layer, true);

	s->hud_best_pill = lv_obj_create(s->hud_layer);
	lv_obj_set_pos(s->hud_best_pill, 16, 16);
	lv_obj_set_size(s->hud_best_pill, LV_SIZE_CONTENT, 36);
	lv_obj_set_style_radius(s->hud_best_pill, 18, 0);
	lv_obj_set_style_border_width(s->hud_best_pill, 1, 0);
	lv_obj_set_style_pad_hor(s->hud_best_pill, 12, 0);
	lv_obj_set_style_pad_ver(s->hud_best_pill, 0, 0);
	lv_obj_set_style_pad_column(s->hud_best_pill, 5, 0);
	lv_obj_set_style_shadow_width(s->hud_best_pill, 0, 0);
	lv_obj_set_flex_flow(s->hud_best_pill, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(s->hud_best_pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(s->hud_best_pill, false);
	lv_obj_set_clickable(s->hud_best_pill, false);

	if (arcade_medal_thresholds(ops->id, NULL)) {
		build_pip_trio(s->hud_best_pill, 8, s->hud_best_pips);
	}
	s->hud_best_label = lv_label_create(s->hud_best_pill);
	lv_obj_set_style_text_font(s->hud_best_label, &font_ui_14, 0);

	s->hud_score_label = lv_label_create(s->hud_layer);
	lv_obj_set_style_text_font(s->hud_score_label, ops->title_in_hud ? &font_ui_32 : &font_ui_24_bold, 0);
	lv_obj_set_style_text_align(s->hud_score_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_width(s->hud_score_label, ops->title_in_hud ? 240 : LV_SIZE_CONTENT);
	if (!ops->title_in_hud) {
		lv_obj_set_style_bg_color(s->hud_score_label, lv_color_hex(0x162234), 0);
		lv_obj_set_style_bg_opa(s->hud_score_label, 205, 0);
		lv_obj_set_style_pad_hor(s->hud_score_label, 14, 0);
		lv_obj_set_style_pad_ver(s->hud_score_label, 4, 0);
		lv_obj_set_style_radius(s->hud_score_label, 18, 0);
	}
	lv_obj_align(s->hud_score_label, LV_ALIGN_TOP_MID, 0, ops->title_in_hud ? 44 : 16);
	lv_obj_set_clickable(s->hud_score_label, false);

	s->hud_sub_pill = lv_obj_create(s->hud_layer);
	lv_obj_set_size(s->hud_sub_pill, LV_SIZE_CONTENT, 28);
	lv_obj_set_style_radius(s->hud_sub_pill, 14, 0);
	lv_obj_set_style_border_width(s->hud_sub_pill, 1, 0);
	lv_obj_set_style_pad_hor(s->hud_sub_pill, 12, 0);
	lv_obj_set_style_pad_ver(s->hud_sub_pill, 0, 0);
	lv_obj_set_style_shadow_width(s->hud_sub_pill, 0, 0);
	lv_obj_align(s->hud_sub_pill, LV_ALIGN_TOP_MID, 0, ops->title_in_hud ? 118 : 64);
	lv_obj_set_scrollable(s->hud_sub_pill, false);
	lv_obj_set_clickable(s->hud_sub_pill, false);
	lv_obj_set_hidden(s->hud_sub_pill, true);

	s->hud_sub_label = lv_label_create(s->hud_sub_pill);
	lv_obj_set_style_text_font(s->hud_sub_label, &font_ui_14, 0);
	lv_obj_center(s->hud_sub_label);

	s->hud_callout_pill = lv_obj_create(s->hud_layer);
	lv_obj_set_size(s->hud_callout_pill, LV_SIZE_CONTENT, 30);
	lv_obj_set_style_radius(s->hud_callout_pill, 15, 0);
	lv_obj_set_style_border_width(s->hud_callout_pill, 1, 0);
	lv_obj_set_style_pad_hor(s->hud_callout_pill, 14, 0);
	lv_obj_set_style_pad_ver(s->hud_callout_pill, 0, 0);
	lv_obj_set_style_shadow_width(s->hud_callout_pill, 0, 0);
	lv_obj_align(s->hud_callout_pill, LV_ALIGN_TOP_MID, 0, ops->title_in_hud ? 118 : 64);
	lv_obj_set_scrollable(s->hud_callout_pill, false);
	lv_obj_set_clickable(s->hud_callout_pill, false);
	lv_obj_set_hidden(s->hud_callout_pill, true);

	s->hud_callout_label = lv_label_create(s->hud_callout_pill);
	lv_obj_set_style_text_font(s->hud_callout_label, &font_ui_16, 0);
	lv_obj_center(s->hud_callout_label);

	s->hud_pause_btn = lv_btn_create(s->hud_layer);
	lv_obj_set_size(s->hud_pause_btn, 44, 44);
	lv_obj_align(s->hud_pause_btn, LV_ALIGN_TOP_RIGHT, -18, 16);
	lv_obj_set_style_radius(s->hud_pause_btn, 22, 0);
	lv_obj_set_style_border_width(s->hud_pause_btn, 1, 0);
	lv_obj_set_style_shadow_width(s->hud_pause_btn, 0, 0);
	lv_obj_set_ext_click_area(s->hud_pause_btn, 6);
	lv_obj_set_event_bubble(s->hud_pause_btn, false);
	lv_obj_add_event_cb(s->hud_pause_btn, pause_btn_cb, LV_EVENT_CLICKED, s);

	s->hud_pause_icon = lv_image_create(s->hud_pause_btn);
	lv_image_set_src(s->hud_pause_icon, &icon_pause);
	lv_image_set_scale(s->hud_pause_icon, 160);
	lv_obj_set_style_image_recolor_opa(s->hud_pause_icon, LV_OPA_COVER, 0);
	lv_obj_center(s->hud_pause_icon);

	// Title overlay (anchored lower-center so live attract canvas shines above)
	s->title_overlay = make_overlay_root(s->screen);
	s->title_card = make_modal_card(s->title_overlay, 384);
	lv_obj_set_style_pad_ver(s->title_card, 20, 0);
	lv_obj_set_style_pad_row(s->title_card, 10, 0);
	lv_obj_align(s->title_card, LV_ALIGN_BOTTOM_MID, 0, -32);

	s->title_badge = lv_obj_create(s->title_card);
	lv_obj_remove_style_all(s->title_badge);
	lv_obj_set_size(s->title_badge, 64, 64);
	lv_obj_set_style_bg_opa(s->title_badge, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(s->title_badge, 18, 0);
	lv_obj_set_scrollable(s->title_badge, false);
	lv_obj_set_clickable(s->title_badge, false);

	s->title_icon = lv_image_create(s->title_badge);
	if (ops->icon) {
		lv_image_set_src(s->title_icon, ops->icon);
	}
	lv_obj_center(s->title_icon);

	s->title_label = lv_label_create(s->title_card);
	lv_label_set_text(s->title_label, tr(ops->title_tag));
	lv_obj_set_style_text_font(s->title_label, &font_ui_28, 0);
	lv_obj_set_style_text_align(s->title_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_width(s->title_label, lv_pct(100));

	s->title_sub = lv_label_create(s->title_card);
	lv_label_set_text(s->title_sub, tr(ops->subtitle_tag));
	lv_obj_set_style_text_font(s->title_sub, &font_ui_18, 0);
	lv_label_set_long_mode(s->title_sub, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_align(s->title_sub, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_width(s->title_sub, lv_pct(100));

	s->title_hint = lv_label_create(s->title_card);
	lv_label_set_text(s->title_hint, tr(ops->hint_tag));
	lv_obj_set_style_text_font(s->title_hint, &font_ui_16, 0);
	lv_label_set_long_mode(s->title_hint, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_align(s->title_hint, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_width(s->title_hint, lv_pct(100));

	s->title_extra = lv_obj_create(s->title_card);
	lv_obj_remove_style_all(s->title_extra);
	lv_obj_set_width(s->title_extra, lv_pct(100));
	lv_obj_set_height(s->title_extra, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(s->title_extra, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(s->title_extra, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(s->title_extra, 8, 0);
	lv_obj_set_scrollable(s->title_extra, false);
	lv_obj_set_hidden(s->title_extra, true);

	s->title_best_pill = lv_obj_create(s->title_card);
	lv_obj_set_size(s->title_best_pill, LV_SIZE_CONTENT, 32);
	lv_obj_set_style_radius(s->title_best_pill, 16, 0);
	lv_obj_set_style_border_width(s->title_best_pill, 1, 0);
	lv_obj_set_style_pad_hor(s->title_best_pill, 14, 0);
	lv_obj_set_style_pad_ver(s->title_best_pill, 0, 0);
	lv_obj_set_style_pad_column(s->title_best_pill, 6, 0);
	lv_obj_set_style_shadow_width(s->title_best_pill, 0, 0);
	lv_obj_set_flex_flow(s->title_best_pill, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(s->title_best_pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(s->title_best_pill, false);

	if (arcade_medal_thresholds(ops->id, NULL)) {
		build_pip_trio(s->title_best_pill, 9, s->title_best_pips);
	}
	s->title_best_label = lv_label_create(s->title_best_pill);
	lv_obj_set_style_text_font(s->title_best_label, &font_ui_16, 0);

	s->title_play_btn =
		make_button(s->title_card, 52, 26, &font_ui_20_bold, "arcade_play", play_btn_cb, s, &s->title_play_lbl);
	s->title_exit_btn =
		make_button(s->title_card, 40, 20, &font_ui_16, "arcade_exit", exit_btn_cb, s, &s->title_exit_lbl);

	// Pause overlay
	s->pause_overlay = make_overlay_root(s->screen);
	lv_obj_set_hidden(s->pause_overlay, true);
	s->pause_card = make_modal_card(s->pause_overlay, 344);

	s->pause_heading = lv_label_create(s->pause_card);
	lv_label_set_text(s->pause_heading, tr("arcade_paused"));
	lv_obj_set_style_text_font(s->pause_heading, &font_ui_24_bold, 0);
	lv_obj_set_style_text_align(s->pause_heading, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_width(s->pause_heading, lv_pct(100));

	s->pause_stat_label = lv_label_create(s->pause_card);
	lv_obj_set_style_text_font(s->pause_stat_label, &font_ui_18, 0);
	lv_obj_set_style_text_align(s->pause_stat_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_width(s->pause_stat_label, lv_pct(100));

	s->pause_resume_btn = make_button(s->pause_card, 50, 25, &font_ui_20_bold, "arcade_resume", resume_btn_cb, s,
									  &s->pause_resume_lbl);
	s->pause_restart_btn =
		make_button(s->pause_card, 48, 24, &font_ui_18, "arcade_restart", play_btn_cb, s, &s->pause_restart_lbl);
	s->pause_exit_btn =
		make_button(s->pause_card, 40, 20, &font_ui_16, "arcade_exit", exit_btn_cb, s, &s->pause_exit_lbl);

	// Game Over / Victory overlay
	s->over_overlay = make_overlay_root(s->screen);
	lv_obj_set_hidden(s->over_overlay, true);
	s->over_card = make_modal_card(s->over_overlay, 368);

	s->over_heading = lv_label_create(s->over_card);
	lv_label_set_text(s->over_heading, tr("arcade_game_over"));
	lv_obj_set_style_text_font(s->over_heading, &font_ui_24_bold, 0);
	lv_obj_set_style_text_align(s->over_heading, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_width(s->over_heading, lv_pct(100));

	s->over_new_best_pill = lv_obj_create(s->over_card);
	lv_obj_set_size(s->over_new_best_pill, LV_SIZE_CONTENT, 28);
	lv_obj_set_style_radius(s->over_new_best_pill, 14, 0);
	lv_obj_set_style_border_width(s->over_new_best_pill, 0, 0);
	lv_obj_set_style_pad_hor(s->over_new_best_pill, 14, 0);
	lv_obj_set_style_pad_ver(s->over_new_best_pill, 0, 0);
	lv_obj_set_style_shadow_width(s->over_new_best_pill, 0, 0);
	lv_obj_set_scrollable(s->over_new_best_pill, false);
	lv_obj_set_hidden(s->over_new_best_pill, true);

	s->over_new_best_lbl = lv_label_create(s->over_new_best_pill);
	lv_label_set_text(s->over_new_best_lbl, tr("arcade_new_best"));
	lv_obj_set_style_text_font(s->over_new_best_lbl, &font_ui_14, 0);
	lv_obj_center(s->over_new_best_lbl);

	// 3-tier Mastery Medal pips + Next-target motivation row
	s->over_medal_row = lv_obj_create(s->over_card);
	lv_obj_remove_style_all(s->over_medal_row);
	lv_obj_set_size(s->over_medal_row, LV_SIZE_CONTENT, 24);
	lv_obj_set_style_pad_column(s->over_medal_row, 8, 0);
	lv_obj_set_flex_flow(s->over_medal_row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(s->over_medal_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(s->over_medal_row, false);
	build_pip_trio(s->over_medal_row, 14, s->over_medal_pips);

	s->over_next_lbl = lv_label_create(s->over_medal_row);
	lv_obj_set_style_text_font(s->over_next_lbl, &font_ui_14, 0);

	s->over_summary_lbl = lv_label_create(s->over_card);
	lv_obj_set_style_text_font(s->over_summary_lbl, &font_ui_16, 0);
	lv_obj_set_style_text_align(s->over_summary_lbl, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(s->over_summary_lbl, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(s->over_summary_lbl, lv_pct(100));
	lv_obj_set_hidden(s->over_summary_lbl, true);

	s->over_stats_box = lv_obj_create(s->over_card);
	lv_obj_set_width(s->over_stats_box, lv_pct(100));
	lv_obj_set_height(s->over_stats_box, LV_SIZE_CONTENT);
	lv_obj_set_style_radius(s->over_stats_box, 16, 0);
	lv_obj_set_style_border_width(s->over_stats_box, 1, 0);
	lv_obj_set_style_pad_hor(s->over_stats_box, 12, 0);
	lv_obj_set_style_pad_ver(s->over_stats_box, 12, 0);
	lv_obj_set_style_shadow_width(s->over_stats_box, 0, 0);
	lv_obj_set_flex_flow(s->over_stats_box, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(s->over_stats_box, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(s->over_stats_box, false);

	lv_obj_t *col_s = lv_obj_create(s->over_stats_box);
	lv_obj_remove_style_all(col_s);
	lv_obj_set_flex_grow(col_s, 1);
	lv_obj_set_height(col_s, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(col_s, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(col_s, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(col_s, 2, 0);
	lv_obj_set_scrollable(col_s, false);

	s->over_score_cap = lv_label_create(col_s);
	lv_label_set_text(s->over_score_cap, tr("arcade_score"));
	lv_obj_set_style_text_font(s->over_score_cap, &font_ui_14, 0);
	s->over_score_val = lv_label_create(col_s);
	lv_obj_set_style_text_font(s->over_score_val, &font_ui_32, 0);

	lv_obj_t *col_b = lv_obj_create(s->over_stats_box);
	lv_obj_remove_style_all(col_b);
	lv_obj_set_flex_grow(col_b, 1);
	lv_obj_set_height(col_b, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(col_b, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(col_b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(col_b, 2, 0);
	lv_obj_set_scrollable(col_b, false);

	s->over_best_cap = lv_label_create(col_b);
	lv_label_set_text(s->over_best_cap, tr("arcade_best"));
	lv_obj_set_style_text_font(s->over_best_cap, &font_ui_14, 0);
	s->over_best_val = lv_label_create(col_b);
	lv_obj_set_style_text_font(s->over_best_val, &font_ui_32, 0);

	s->over_continue_btn = make_button(s->over_card, 50, 25, &font_ui_20_bold, "arcade_resume", continue_btn_cb, s,
									   &s->over_continue_lbl);
	lv_obj_set_hidden(s->over_continue_btn, true);

	s->over_again_btn =
		make_button(s->over_card, 50, 25, &font_ui_20_bold, "arcade_play_again", play_btn_cb, s, &s->over_again_lbl);
	s->over_exit_btn =
		make_button(s->over_card, 40, 20, &font_ui_16, "arcade_exit", exit_btn_cb, s, &s->over_exit_lbl);

	// Persistent Sound button above overlays
	s->sound_btn = lv_btn_create(s->screen);
	lv_obj_set_size(s->sound_btn, 44, 44);
	lv_obj_align(s->sound_btn, LV_ALIGN_TOP_RIGHT, -18, 16);
	lv_obj_set_style_radius(s->sound_btn, 22, 0);
	lv_obj_set_style_border_width(s->sound_btn, 1, 0);
	lv_obj_set_style_shadow_width(s->sound_btn, 0, 0);
	lv_obj_set_ext_click_area(s->sound_btn, 6);
	lv_obj_set_event_bubble(s->sound_btn, false);
	lv_obj_add_event_cb(s->sound_btn, sound_btn_cb, LV_EVENT_CLICKED, s);

	s->sound_icon = lv_image_create(s->sound_btn);
	lv_image_set_src(s->sound_icon, &icon_volume_high);
	lv_image_set_scale(s->sound_icon, 210);
	lv_obj_set_style_image_recolor_opa(s->sound_icon, LV_OPA_COVER, 0);
	lv_obj_center(s->sound_icon);

	if (!shared_tick_timer) {
		shared_tick_timer = lv_timer_create(tick_cb, s->tick_ms, NULL);
		lv_timer_pause(shared_tick_timer);
		power_pause_in_standby(shared_tick_timer);
	}

	if (registered_count < MAX_SHELLS) {
		registered_shells[registered_count++] = s;
	}
	if (registered_count == 1) {
		theme_register_refresh(refresh_all_themes);
	}
	apply_shell_theme(s);
	return s;
}

static void refresh_static_tr(arcade_shell_t *s) {
	if (s->title_label) lv_label_set_text(s->title_label, tr(s->ops.title_tag));
	if (s->title_sub) lv_label_set_text(s->title_sub, tr(s->ops.subtitle_tag));
	if (s->title_hint) lv_label_set_text(s->title_hint, tr(s->ops.hint_tag));
	if (s->title_play_lbl) lv_label_set_text(s->title_play_lbl, tr("arcade_play"));
	if (s->title_exit_lbl) lv_label_set_text(s->title_exit_lbl, tr("arcade_exit"));
	if (s->pause_heading) lv_label_set_text(s->pause_heading, tr("arcade_paused"));
	if (s->pause_resume_lbl) lv_label_set_text(s->pause_resume_lbl, tr("arcade_resume"));
	if (s->pause_restart_lbl) lv_label_set_text(s->pause_restart_lbl, tr("arcade_restart"));
	if (s->pause_exit_lbl) lv_label_set_text(s->pause_exit_lbl, tr("arcade_exit"));
	if (s->over_heading && !s->custom_end_heading) lv_label_set_text(s->over_heading, tr("arcade_game_over"));
	if (s->over_new_best_lbl) lv_label_set_text(s->over_new_best_lbl, tr("arcade_new_best"));
	if (s->over_score_cap) lv_label_set_text(s->over_score_cap, tr("arcade_score"));
	if (s->over_best_cap) lv_label_set_text(s->over_best_cap, tr("arcade_best"));
	if (s->over_again_lbl) lv_label_set_text(s->over_again_lbl, tr("arcade_play_again"));
	if (s->over_exit_lbl) lv_label_set_text(s->over_exit_lbl, tr("arcade_exit"));
}

void arcade_shell_open(arcade_shell_t *s) {
	if (!s) {
		return;
	}
	if (s->over_delay_timer) {
		lv_timer_delete(s->over_delay_timer);
		s->over_delay_timer = NULL;
	}
	s->over_settling = false;
	s->new_best_this_run = false;
	s->score = 0;
	s->score_id = s->ops.id;
	s->best = arcade_shell_read_best(s->score_id);
	s->medal_before_run = arcade_medal_for_score(s->score_id, s->best, NULL);
	s->best_dirty = false;
	s->music_pulse = 0.0f;

	if (s->ops.use_framebuffer && !arcade_fb_alloc(&s->fb, s->ops.compact_scene)) {
		fprintf(stderr, "arcade[%s]: out of memory for framebuffer\n", s->ops.id ? s->ops.id : "game");
		gui_notify_popup("gearboy_start_failed");
		return;
	}

	reset_run_overrides(s);
	refresh_static_tr(s);
	apply_shell_theme(s);

	bool auto_resume = s->ops.has_saved_run && s->ops.on_resume_saved && s->ops.has_saved_run(s->ctx);
	if (auto_resume) {
		set_state_ui(s, ARCADE_ST_PLAYING);
		if (s->ops.on_enter) {
			s->ops.on_enter(s, s->ctx);
		}
		s->ops.on_resume_saved(s, s->ctx);
	} else {
		set_state_ui(s, ARCADE_ST_TITLE);
		if (s->ops.on_enter) {
			s->ops.on_enter(s, s->ctx);
		}
	}

	if (s->ops.on_render) {
		s->ops.on_render(s, s->ctx, true);
	}
	if (s->ops.use_framebuffer) {
		arcade_fb_flush(&s->fb);
	}

	switch_screen(s->screen);
}

void arcade_shell_close(arcade_shell_t *s) {
	if (s && s->active) {
		back_btn_cb(NULL);
	}
}

static void score_anim_exec(void *obj, int32_t v) { lv_obj_set_style_transform_scale((lv_obj_t *)obj, v, 0); }

void arcade_shell_restore_score(arcade_shell_t *s, int score) {
	if (!s) return;
	s->score = score >= 0 ? score : 0;
	if (s->score > s->best) {
		s->best = s->score;
		s->best_dirty = true;
	}
	refresh_labels_text(s);
}

void arcade_shell_set_score(arcade_shell_t *s, int score) {
	if (!s) {
		return;
	}
	bool changed = (score != s->score);
	s->score = score;
	if (score > s->best) {
		s->best = score;
		s->best_dirty = true;
		s->new_best_this_run = true;
	}
	arcade_medal_t now_medal = arcade_medal_for_score(s->score_id, s->score, NULL);
	if ((int)now_medal > (int)s->medal_before_run) {
		s->medal_before_run = now_medal;
		arcade_sfx_play(ARCADE_SFX_POWERUP);
	}
	refresh_labels_text(s);

	if (!s->ops.title_in_hud && s->hud_score_label && score > 0 && changed) {
		lv_obj_set_style_transform_pivot_x(s->hud_score_label, lv_pct(50), 0);
		lv_obj_set_style_transform_pivot_y(s->hud_score_label, lv_pct(50), 0);
		lv_anim_t a;
		lv_anim_init(&a);
		lv_anim_set_var(&a, s->hud_score_label);
		lv_anim_set_exec_cb(&a, score_anim_exec);
		lv_anim_set_values(&a, 290, 256);
		lv_anim_set_duration(&a, 160);
		lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
		lv_anim_start(&a);
	}
}

void arcade_shell_set_sub_hud(arcade_shell_t *s, const char *text) {
	if (!s || !s->hud_sub_pill || !s->hud_sub_label) {
		return;
	}
	if (text && text[0]) {
		s->has_sub_hud = true;
		lv_label_set_text(s->hud_sub_label, text);
		lv_obj_set_hidden(s->hud_sub_pill, false);
		if (s->hud_callout_pill) {
			lv_obj_align(s->hud_callout_pill, LV_ALIGN_TOP_MID, 0, s->ops.title_in_hud ? 150 : 96);
		}
	} else {
		s->has_sub_hud = false;
		lv_obj_set_hidden(s->hud_sub_pill, true);
		if (s->hud_callout_pill) {
			lv_obj_align(s->hud_callout_pill, LV_ALIGN_TOP_MID, 0, s->ops.title_in_hud ? 118 : 64);
		}
	}
}

void arcade_shell_show_callout(arcade_shell_t *s, const char *text, lv_color_t color) {
	if (!s || !s->hud_callout_pill || !s->hud_callout_label || !text || !text[0]) {
		return;
	}
	if (s->callout_timer) {
		lv_timer_delete(s->callout_timer);
		s->callout_timer = NULL;
	}
	const theme_palette_t *pal = theme();
	lv_label_set_text(s->hud_callout_label, text);
	lv_obj_set_style_text_color(s->hud_callout_label, legible_accent_text(color, pal->dark), 0);
	lv_obj_align(s->hud_callout_pill, LV_ALIGN_TOP_MID, 0, (s->ops.title_in_hud ? 118 : 64) + (s->has_sub_hud ? 32 : 0));
	lv_obj_set_hidden(s->hud_callout_pill, false);
	s->callout_timer = lv_timer_create(callout_timer_cb, 950, s);
	lv_timer_set_repeat_count(s->callout_timer, 1);
}

void arcade_shell_set_end_heading(arcade_shell_t *s, const char *text) {
	if (!s || !s->over_heading) {
		return;
	}
	if (text && text[0]) {
		s->custom_end_heading = true;
		lv_label_set_text(s->over_heading, text);
	} else {
		s->custom_end_heading = false;
		lv_label_set_text(s->over_heading, tr("arcade_game_over"));
	}
}

void arcade_shell_set_end_score_visible(arcade_shell_t *s, bool visible) {
	s->hide_end_score = !visible;
	refresh_labels_text(s);
}

void arcade_shell_set_end_summary(arcade_shell_t *s, const char *text) {
	if (!s || !s->over_summary_lbl) {
		return;
	}
	if (text && text[0]) {
		lv_label_set_text(s->over_summary_lbl, text);
		lv_obj_set_hidden(s->over_summary_lbl, false);
	} else {
		lv_label_set_text(s->over_summary_lbl, "");
		lv_obj_set_hidden(s->over_summary_lbl, true);
	}
}

void arcade_shell_set_end_continue(arcade_shell_t *s, const char *text,
								   void (*cb)(arcade_shell_t *shell, void *ctx)) {
	if (!s || !s->over_continue_btn || !s->over_continue_lbl) {
		return;
	}
	s->over_continue_cb = cb;
	if (cb && text && text[0]) {
		lv_label_set_text(s->over_continue_lbl, text);
		lv_obj_set_hidden(s->over_continue_btn, false);
	} else {
		lv_obj_set_hidden(s->over_continue_btn, true);
	}
	refresh_over_buttons(s, theme());
}

void arcade_shell_trigger_game_over(arcade_shell_t *s, int delay_ms) {
	if (!s || s->state != ARCADE_ST_PLAYING || s->over_settling) {
		return;
	}
	if (s->new_best_this_run || s->over_continue_cb != NULL) {
		arcade_sfx_play(ARCADE_SFX_WIN);
	} else {
		arcade_sfx_play(ARCADE_SFX_MISS);
	}
	s->over_settling = true;
	if (s->over_delay_timer) {
		lv_timer_delete(s->over_delay_timer);
		s->over_delay_timer = NULL;
	}
	if (delay_ms <= 0) {
		save_best_if_dirty(s);
		s->over_settling = false;
		set_state_ui(s, ARCADE_ST_GAMEOVER);
	} else {
		lv_obj_set_hidden(s->hud_pause_btn, true);
		s->over_delay_timer = lv_timer_create(over_delay_cb, (uint32_t)delay_ms, s);
		lv_timer_set_repeat_count(s->over_delay_timer, 1);
	}
}

void arcade_shell_resume_playing(arcade_shell_t *s) {
	if (!s || !s->active) {
		return;
	}
	if (s->over_delay_timer) {
		lv_timer_delete(s->over_delay_timer);
		s->over_delay_timer = NULL;
	}
	s->over_settling = false;
	s->last_us = now_us();
	set_state_ui(s, ARCADE_ST_PLAYING);
	if (s->ops.use_framebuffer || s->ops.on_render) {
		s->dirty = true;
	}
}

void arcade_shell_mark_dirty(arcade_shell_t *s) {
	if (s) {
		s->dirty = true;
		if (active_tick_shell == s && lv_timer_get_paused(shared_tick_timer) && power_screen_is_on()) {
			s->last_us = now_us();
			lv_timer_resume(shared_tick_timer);
		}
	}
}

void arcade_shell_force_full_redraw(arcade_shell_t *s) {
	if (s) {
		s->dirty = true;
		s->full_redraw = true;
		if (active_tick_shell == s) lv_timer_resume(shared_tick_timer);
	}
}

arcade_fb_t *arcade_shell_fb(arcade_shell_t *s) { return s ? &s->fb : NULL; }
lv_obj_t *arcade_shell_screen(arcade_shell_t *s) { return s ? s->screen : NULL; }
lv_obj_t *arcade_shell_content(arcade_shell_t *s) { return s ? s->content_layer : NULL; }
lv_obj_t *arcade_shell_hud(arcade_shell_t *s) { return s ? s->hud_layer : NULL; }
lv_obj_t *arcade_shell_title_extra(arcade_shell_t *s) { return s ? s->title_extra : NULL; }
void arcade_shell_set_title_extra_visible(arcade_shell_t *s, bool visible) {
	if (s && s->title_extra) {
		lv_obj_set_hidden(s->title_extra, !visible);
	}
}
arcade_state_t arcade_shell_state(const arcade_shell_t *s) { return s ? s->state : ARCADE_ST_TITLE; }
int arcade_shell_score(const arcade_shell_t *s) { return s ? s->score : 0; }
int arcade_shell_best(const arcade_shell_t *s) { return s ? s->best : 0; }
int arcade_shell_width(const arcade_shell_t *s) { return s ? s->width : 0; }
int arcade_shell_height(const arcade_shell_t *s) { return s ? s->height : 0; }
bool arcade_shell_is_active(const arcade_shell_t *s) { return s ? s->active : false; }
float arcade_shell_music_pulse(const arcade_shell_t *s) { return s ? s->music_pulse : 0.0f; }
