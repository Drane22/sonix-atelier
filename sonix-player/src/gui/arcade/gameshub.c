#include "gameshub.h"

#include <stdio.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/arcade/arcade_scene.h"
#include "src/gui/arcade/arcade_shell.h"
#include "src/gui/arcade/game_2048.h"
#include "src/gui/arcade/game_orbit.h"
#include "src/gui/arcade/game_prism.h"
#include "src/gui/arcade/game_word.h"
#include "src/gui/flappybird/flappybird.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/gearboy/gearboypage.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"

lv_obj_t *games_screen;

typedef struct {
	const char *id, *title, *description;
	const lv_image_dsc_t *icon;
	uint32_t dark_bg, light_bg;
	void (*open)(gui_config_t *);
} arcade_card_t;

static const arcade_card_t collection[] = {
	{"blockmosaic", "game_blockmosaic", "game_blockmosaic_desc", &icon_atelier_blockblast, 0x193639, 0xe4f3ef,
	 game_blockmosaic_open},
	{"2048", "game_2048", "game_2048_desc", &icon_atelier_2048, 0x2a2112, 0xfcf0d4, game_2048_open},
	{"breakwater", "game_breakwater", "game_breakwater_desc", &icon_atelier_breakwater, 0x153637, 0xe1f4ef,
	 game_breakwater_open},
	{"towerblast", "game_towerblast", "game_towerblast_desc", &icon_atelier_towerblast, 0x182230, 0xe5f0fc,
	 game_towerblast_open},
	{"orbit", "game_orbit", "game_orbit_desc", &icon_atelier_orbit, 0x152428, 0xe0f4f6, game_orbit_open},
	{"prism", "game_prism", "game_prism_desc", &icon_atelier_prism, 0x201930, 0xeee8fc, game_prism_open},
};

typedef struct {
	lv_obj_t *card;
	lv_obj_t *badge;
	lv_obj_t *pips[3];
	lv_obj_t *saved_dot;
	lv_obj_t *best_label;
	const char *id;
	lv_color_t dark_bg;
	lv_color_t light_bg;
} game_card_visual_t;

#define MAX_GAME_CARDS (sizeof(collection) / sizeof(collection[0]) + 2)
static game_card_visual_t game_cards[MAX_GAME_CARDS];
static size_t game_card_count;
static gui_config_t *hub_config;

static lv_obj_t *word_status_pill;
static lv_obj_t *word_status_lbl;

static void arcade_card_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) return;
	const arcade_card_t *game = lv_event_get_user_data(e);
	if (game && game->open) {
		game->open(hub_config);
	}
}

static void word_card_cb(lv_event_t *e) {
	(void)e;
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	game_word_open(hub_config);
}

static void flappy_card_cb(lv_event_t *e) {
	(void)e;
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	flappybird_open();
}

static void gameboy_card_cb(lv_event_t *e) {
	(void)e;
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	switch_screen(gearboypage_screen);
}

static void refresh_word_banner(void) {
	if (!word_status_pill || !word_status_lbl) {
		return;
	}
	int puzzle_num = 1, streak = 0, prog_row = 0;
	bool solved = false;
	game_word_hub_status(&puzzle_num, &streak, &solved, &prog_row);

	char buf[96];
	if (solved) {
		if (streak > 0) {
			snprintf(buf, sizeof(buf), "%s #%d  ·  ✓  ·  ★ %d", tr("word_daily"), puzzle_num, streak);
		} else {
			snprintf(buf, sizeof(buf), "%s #%d  ·  ✓", tr("word_daily"), puzzle_num);
		}
		lv_obj_set_style_bg_color(word_status_pill, lv_color_hex(0x1B5E42), 0);
		lv_obj_set_style_border_color(word_status_pill, lv_color_hex(0x3BC48B), 0);
	} else if (prog_row > 0) {
		snprintf(buf, sizeof(buf), "%s #%d  ·  %d / 6", tr("word_daily"), puzzle_num, prog_row + 1);
		lv_obj_set_style_bg_color(word_status_pill, lv_color_hex(0x523915), 0);
		lv_obj_set_style_border_color(word_status_pill, lv_color_hex(0xE6A947), 0);
	} else if (streak > 0) {
		snprintf(buf, sizeof(buf), "%s #%d  ·  ★ %d", tr("word_daily"), puzzle_num, streak);
		lv_obj_set_style_bg_color(word_status_pill, lv_color_hex(0x152238), 0);
		lv_obj_set_style_border_color(word_status_pill, lv_color_hex(0x6C99CC), 0);
	} else {
		snprintf(buf, sizeof(buf), "%s #%d", tr("word_daily"), puzzle_num);
		lv_obj_set_style_bg_color(word_status_pill, lv_color_hex(0x152238), 0);
		lv_obj_set_style_border_color(word_status_pill, lv_color_hex(0x587AA6), 0);
	}
	lv_label_set_text(word_status_lbl, buf);
}

static void refresh_best_badges(void) {
	const theme_palette_t *pal = theme();
	refresh_word_banner();

	for (size_t i = 0; i < game_card_count; i++) {
		game_card_visual_t *c = &game_cards[i];
		if (!c->id) {
			continue;
		}
		int best = arcade_shell_read_best(c->id);
		arcade_medal_t medal = arcade_medal_for_score(c->id, best, NULL);
		for (int m = 0; m < 3; m++) {
			if (!c->pips[m]) continue;
			arcade_medal_t tier = (arcade_medal_t)(m + 1);
			bool lit = (int)medal >= (int)tier;
			lv_color_t mc = arcade_medal_color(tier, pal->dark);
			if (lit) {
				lv_obj_set_style_bg_color(c->pips[m], mc, 0);
				lv_obj_set_style_bg_opa(c->pips[m], LV_OPA_COVER, 0);
				lv_obj_set_style_border_color(c->pips[m], mc, 0);
			} else {
				lv_obj_set_style_bg_color(c->pips[m], pal->elevated, 0);
				lv_obj_set_style_bg_opa(c->pips[m], LV_OPA_70, 0);
				lv_obj_set_style_border_color(c->pips[m], pal->border, 0);
			}
		}

		bool has_save = arcade_game_has_saved_run(c->id);
		if (c->saved_dot) {
			lv_obj_set_hidden(c->saved_dot, !has_save);
			lv_obj_set_style_bg_color(c->saved_dot, pal->accent, 0);
		}
		if (c->best_label) {
			if (has_save && best > 0) {
				char buf[40];
				snprintf(buf, sizeof(buf), "%s · %d", tr("arcade_resume"), best);
				lv_label_set_text(c->best_label, buf);
				lv_obj_set_hidden(c->best_label, false);
			} else if (has_save) {
				lv_label_set_text(c->best_label, tr("arcade_resume"));
				lv_obj_set_hidden(c->best_label, false);
			} else if (best > 0) {
				char buf[32];
				snprintf(buf, sizeof(buf), "%s %d", tr("arcade_best"), best);
				lv_label_set_text(c->best_label, buf);
				lv_obj_set_hidden(c->best_label, false);
			} else {
				lv_label_set_text(c->best_label, "—");
				lv_obj_set_hidden(c->best_label, false);
			}
		}
	}
}

static void gameshub_loaded_cb(lv_event_t *e) {
	(void)e;
	refresh_best_badges();
}

static void refresh_theme(void) {
	const theme_palette_t *pal = theme();
	bool dark = pal->dark;

	for (size_t i = 0; i < game_card_count; i++) {
		game_card_visual_t *c = &game_cards[i];
		if (c->card) {
			lv_obj_set_style_border_color(c->card, pal->border, 0);
		}
		if (c->badge) {
			lv_obj_set_style_bg_color(c->badge, dark ? c->dark_bg : c->light_bg, 0);
		}
		if (c->best_label) {
			lv_obj_set_style_text_color(c->best_label, pal->text_secondary, 0);
		}
	}
	refresh_best_badges();
}

static lv_obj_t *make_pair_row(lv_obj_t *parent, int col_gap) {
	lv_obj_t *row = lv_obj_create(parent);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_column(row, col_gap, 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_clickable(row, false);
	lv_obj_set_event_bubble(row, true);
	return row;
}

static lv_obj_t *add_tile_card(lv_obj_t *row, int tile_w, const lv_image_dsc_t *glyph, const char *title_tag,
							   const char *desc_tag, lv_color_t dark_bg, lv_color_t light_bg,
							   const arcade_card_t *game, lv_event_cb_t cb) {
	lv_obj_t *card = lv_btn_create(row);
	lv_obj_set_size(card, tile_w, 140);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_add_style(card, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(card, 18, 0);
	lv_obj_set_style_border_width(card, 1, 0);
	lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_hor(card, 13, 0);
	lv_obj_set_style_pad_ver(card, 12, 0);
	lv_obj_set_style_pad_row(card, 8, 0);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_event_bubble(card, true);
	lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, (void *)game);

	// Top header inside tile: icon badge on left, medals + score on right
	lv_obj_t *top = lv_obj_create(card);
	lv_obj_remove_style_all(top);
	lv_obj_set_size(top, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_scrollable(top, false);
	lv_obj_set_clickable(top, false);
	lv_obj_set_event_bubble(top, true);

	lv_obj_t *badge = lv_obj_create(top);
	lv_obj_remove_style_all(badge);
	lv_obj_set_size(badge, 56, 56);
	lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(badge, 15, 0);
	lv_obj_set_scrollable(badge, false);
	lv_obj_set_clickable(badge, false);
	lv_obj_set_event_bubble(badge, true);

	lv_obj_t *icon = lv_image_create(badge);
	lv_image_set_src(icon, glyph);
	if (!game) {
		lv_obj_set_size(icon, 48, 48);
		lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_STRETCH);
	}
	lv_obj_center(icon);
	lv_obj_set_event_bubble(icon, true);

	lv_obj_t *pips[3] = {NULL, NULL, NULL};
	lv_obj_t *saved_dot = NULL;
	lv_obj_t *best_label = NULL;

	if (game) {
		lv_obj_t *meta = lv_obj_create(top);
		lv_obj_remove_style_all(meta);
		lv_obj_set_size(meta, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
		lv_obj_set_style_pad_row(meta, 5, 0);
		lv_obj_set_flex_flow(meta, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(meta, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
		lv_obj_set_scrollable(meta, false);
		lv_obj_set_clickable(meta, false);
		lv_obj_set_event_bubble(meta, true);

		lv_obj_t *pip_row = lv_obj_create(meta);
		lv_obj_remove_style_all(pip_row);
		lv_obj_set_size(pip_row, LV_SIZE_CONTENT, 12);
		lv_obj_set_style_pad_column(pip_row, 4, 0);
		lv_obj_set_flex_flow(pip_row, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(pip_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_scrollable(pip_row, false);
		lv_obj_set_clickable(pip_row, false);
		lv_obj_set_event_bubble(pip_row, true);

		saved_dot = lv_obj_create(pip_row);
		lv_obj_remove_style_all(saved_dot);
		lv_obj_set_size(saved_dot, 7, 7);
		lv_obj_set_style_radius(saved_dot, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(saved_dot, LV_OPA_COVER, 0);
		lv_obj_set_clickable(saved_dot, false);
		lv_obj_set_hidden(saved_dot, true);

		for (int m = 0; m < 3; m++) {
			lv_obj_t *pip = lv_obj_create(pip_row);
			lv_obj_remove_style_all(pip);
			lv_obj_set_size(pip, 10, 10);
			lv_obj_set_style_radius(pip, LV_RADIUS_CIRCLE, 0);
			lv_obj_set_style_border_width(pip, 1, 0);
			lv_obj_set_style_border_opa(pip, LV_OPA_COVER, 0);
			lv_obj_set_scrollable(pip, false);
			lv_obj_set_clickable(pip, false);
			lv_obj_set_event_bubble(pip, true);
			pips[m] = pip;
		}

		best_label = lv_label_create(meta);
		lv_obj_set_style_text_font(best_label, &font_ui_14, 0);
		lv_label_set_long_mode(best_label, LV_LABEL_LONG_DOT);
		lv_obj_set_style_max_width(best_label, tile_w - 86, 0);
		lv_obj_set_event_bubble(best_label, true);
	} else {
		lv_obj_t *chevron = lv_image_create(top);
		lv_image_set_src(chevron, &icon_chevron_right);
		lv_obj_add_style(chevron, &theme_style_icon, 0);
		lv_obj_set_style_image_opa(chevron, LV_OPA_50, 0);
		lv_obj_set_event_bubble(chevron, true);
	}

	// Bottom text stack
	lv_obj_t *texts = lv_obj_create(card);
	lv_obj_remove_style_all(texts);
	lv_obj_set_size(texts, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_row(texts, 2, 0);
	lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scrollable(texts, false);
	lv_obj_set_clickable(texts, false);
	lv_obj_set_event_bubble(texts, true);

	lv_obj_t *title = lv_label_create(texts);
	lv_label_set_text(title, tr(title_tag));
	lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
	lv_obj_set_width(title, lv_pct(100));
	lv_obj_set_height(title, lv_font_get_line_height(&font_ui_20_bold));
	lv_obj_add_style(title, &theme_style_text, 0);
	lv_obj_set_style_text_font(title, &font_ui_20_bold, 0);
	lv_obj_set_event_bubble(title, true);

	lv_obj_t *desc = lv_label_create(texts);
	lv_label_set_text(desc, tr(desc_tag));
	lv_label_set_long_mode(desc, LV_LABEL_LONG_DOT);
	lv_obj_set_width(desc, lv_pct(100));
	lv_obj_set_height(desc, lv_font_get_line_height(&font_ui_14));
	lv_obj_add_style(desc, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(desc, &font_ui_14, 0);
	lv_obj_set_event_bubble(desc, true);

	if (game_card_count < MAX_GAME_CARDS) {
		game_cards[game_card_count++] = (game_card_visual_t){
			.card = card,
			.badge = badge,
			.pips = {pips[0], pips[1], pips[2]},
			.saved_dot = saved_dot,
			.best_label = best_label,
			.id = game ? game->id : NULL,
			.dark_bg = dark_bg,
			.light_bg = light_bg,
		};
	}

	return card;
}

static void add_word_feature(lv_obj_t *parent) {
	lv_obj_t *card = lv_button_create(parent);
	lv_obj_remove_style_all(card);
	lv_obj_set_size(card, lv_pct(100), 142);
	lv_obj_set_style_radius(card, 22, 0);
	lv_obj_set_style_clip_corner(card, true, 0);
	lv_obj_set_style_bg_color(card, lv_color_hex(0x18284A), 0);
	lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
	lv_obj_set_style_opa(card, LV_OPA_80, LV_STATE_PRESSED);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_event_bubble(card, true);
	lv_obj_add_event_cb(card, word_card_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *sky = lv_image_create(card);
	lv_image_set_src(sky, &atelier_dusk);
	lv_obj_set_size(sky, lv_pct(100), 340);
	lv_image_set_inner_align(sky, LV_IMAGE_ALIGN_STRETCH);
	lv_obj_align(sky, LV_ALIGN_CENTER, 0, -45);
	lv_obj_set_style_image_recolor(sky, lv_color_hex(0x10182B), 0);
	lv_obj_set_style_image_recolor_opa(sky, LV_OPA_50, 0);

	lv_obj_t *art = lv_image_create(card);
	lv_image_set_src(art, &icon_atelier_word);
	lv_image_set_scale(art, 385);
	lv_obj_align(art, LV_ALIGN_RIGHT_MID, -24, 6);

	word_status_pill = lv_obj_create(card);
	lv_obj_remove_style_all(word_status_pill);
	lv_obj_set_size(word_status_pill, LV_SIZE_CONTENT, 26);
	lv_obj_set_style_radius(word_status_pill, 13, 0);
	lv_obj_set_style_bg_color(word_status_pill, lv_color_hex(0x152238), 0);
	lv_obj_set_style_bg_opa(word_status_pill, 215, 0);
	lv_obj_set_style_border_width(word_status_pill, 1, 0);
	lv_obj_set_style_border_color(word_status_pill, lv_color_hex(0x587AA6), 0);
	lv_obj_set_style_pad_hor(word_status_pill, 11, 0);
	lv_obj_align(word_status_pill, LV_ALIGN_TOP_LEFT, 18, 14);
	lv_obj_set_scrollable(word_status_pill, false);
	lv_obj_set_clickable(word_status_pill, false);
	lv_obj_set_event_bubble(word_status_pill, true);

	word_status_lbl = lv_label_create(word_status_pill);
	lv_obj_set_style_text_font(word_status_lbl, &font_ui_14, 0);
	lv_obj_set_style_text_color(word_status_lbl, lv_color_white(), 0);
	lv_obj_center(word_status_lbl);
	lv_obj_set_event_bubble(word_status_lbl, true);

	lv_obj_t *title = lv_label_create(card);
	lv_label_set_text(title, tr("game_word"));
	lv_obj_set_style_text_font(title, &font_ui_28, 0);
	lv_obj_set_style_text_color(title, lv_color_white(), 0);
	lv_obj_align(title, LV_ALIGN_LEFT_MID, 18, 6);

	lv_obj_t *caption = lv_label_create(card);
	lv_label_set_text(caption, tr("game_word_desc"));
	lv_obj_set_width(caption, lv_pct(66));
	lv_label_set_long_mode(caption, LV_LABEL_LONG_DOT);
	lv_obj_set_height(caption, lv_font_get_line_height(&font_ui_16));
	lv_obj_set_style_text_font(caption, &font_ui_16, 0);
	lv_obj_set_style_text_color(caption, lv_color_make(225, 232, 245), 0);
	lv_obj_align(caption, LV_ALIGN_BOTTOM_LEFT, 18, -14);
}

void gameshub_init(gui_config_t *cfg) {
	hub_config = cfg;
	game_card_count = 0;

	lv_obj_t *container = settingsrow_page(games_screen, cfg, "games");
	lv_obj_set_style_pad_row(container, 10, 0);
	lv_obj_set_style_pad_bottom(container, 22, 0);
	lv_obj_set_event_bubble(container, true);
	player_sheet_attach_drag(container, true);
	switcher_attach_back_gesture(container);
	lv_obj_add_event_cb(games_screen, gameshub_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);

	const int col_gap = 12;
	const int content_w = (int)cfg->screen_width - 2 * (int)cfg->padding;
	const int tile_w = (content_w - col_gap) / 2;

	add_word_feature(container);
	settingsrow_heading(container, "games_collection");

	lv_obj_t *pair_row = NULL;
	for (size_t i = 0; i < sizeof(collection) / sizeof(collection[0]); i++) {
		if ((i & 1u) == 0u) {
			pair_row = make_pair_row(container, col_gap);
		}
		const arcade_card_t *game = &collection[i];
		add_tile_card(pair_row, tile_w, game->icon, game->title, game->description, lv_color_hex(game->dark_bg),
					  lv_color_hex(game->light_bg), game, arcade_card_cb);
	}

	settingsrow_heading(container, "games_featured");
	lv_obj_t *classics_row = make_pair_row(container, col_gap);
	add_tile_card(classics_row, tile_w, &icon_menu_flappy_bird, "flappy_bird", "games_flappy_desc",
				  lv_color_make(38, 33, 21), lv_color_make(247, 238, 215), NULL, flappy_card_cb);

#if GB_CORE
	add_tile_card(classics_row, tile_w, &icon_menu_gearboy, "gearboy", "games_gearboy_desc",
				  lv_color_make(26, 32, 47), lv_color_make(233, 241, 252), NULL, gameboy_card_cb);
#endif

	refresh_best_badges();
	refresh_theme();
	theme_register_refresh(refresh_theme);
}
