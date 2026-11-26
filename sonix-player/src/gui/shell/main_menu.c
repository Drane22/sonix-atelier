#include "main_menu.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/arcade/gameshub.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/library/audiobooks.h"
#include "src/gui/library/music.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/coverloader.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/settings/settings.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/morepage.h"
#include "src/gui/shell/pagetimer.h"
#include "src/gui/shell/scrolltext.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/streaming/streaming.h"
#include "src/gui/wireless/wireless.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"
#include "src/system/library/metadata.h"
#include "src/system/playback/device_state.h"
#include "src/system/streaming/radio.h"

lv_obj_t *main_menu_screen;

#define MINI_COVER_SIZE 88
#define MINI_POLL_MS 500

typedef struct {
	const char *label_tag;
	const lv_image_dsc_t *image;
	lv_obj_t *const *target;
} home_app_t;

static const home_app_t shortcuts[] = {
	{"streaming", &icon_menu_streaming, &streaming_screen},
	{"audiobooks", &icon_menu_audiobooks, &audiobooks_screen},
	{"games", &icon_atelier_games, &games_screen},
};
static const home_app_t dock_apps[] = {
	{"music", &icon_menu_music, &music_screen},
	{"wireless", &icon_menu_wireless, &wireless_screen},
	{"more", &icon_menu_more, &morepage_screen},
	{"settings", &icon_menu_settings, &settings_screen},
};

static lv_obj_t *dock;
static lv_obj_t *mini_card;
static lv_obj_t *mini_cover_box;
static lv_obj_t *mini_cover_fallback;
static lv_obj_t *mini_cover_img;
static lv_obj_t *mini_status_label;
static lv_obj_t *mini_title_label;
static lv_obj_t *mini_artist_label;
static lv_obj_t *mini_play_btn;
static lv_obj_t *mini_play_icon;
static lv_obj_t *mini_progress_bar;

static lv_timer_t *mini_timer;

static cover_image_t mini_cover;
static bool mini_has_cover;
static bool mini_cover_pending;
static char mini_cover_path[512];
static bool mini_playing_now;

static void make_transparent_container(lv_obj_t *obj) {
	lv_obj_set_style_bg_opa(obj, 0, 0);
	lv_obj_set_style_border_width(obj, 0, 0);
	lv_obj_set_style_shadow_width(obj, 0, 0);
	lv_obj_set_style_radius(obj, 0, 0);
	lv_obj_set_style_pad_all(obj, 0, 0);
	lv_obj_set_scrollable(obj, false);
	lv_obj_set_event_bubble(obj, true);
}

static void mini_clear_cover(void) {
	if (mini_cover_img) {
		// Drop LVGL's reference before releasing pixel memory.
		lv_image_set_src(mini_cover_img, NULL);
		lv_obj_set_hidden(mini_cover_img, true);
	}
	if (mini_cover_fallback) {
		lv_obj_set_hidden(mini_cover_fallback, false);
	}
	if (mini_has_cover) {
		cover_free(&mini_cover);
		mini_has_cover = false;
	}
}

static void mini_poll_cover(void) {
	if (!mini_cover_pending) {
		return;
	}
	cover_image_t next;
	bool finished = false;
	bool ok = coverloader_take(COVERLOADER_MINIPLAYER, &next, &finished);
	if (!finished) {
		return;
	}
	mini_cover_pending = false;
	if (!ok) {
		return;
	}
	if (mini_cover_img) {
		lv_image_set_src(mini_cover_img, NULL);
	}
	if (mini_has_cover) {
		cover_free(&mini_cover);
		mini_has_cover = false;
	}
	mini_cover = next;
	mini_has_cover = true;
	if (mini_cover_img) {
		lv_image_set_src(mini_cover_img, &mini_cover.dsc);
		lv_obj_set_hidden(mini_cover_img, false);
	}
	if (mini_cover_fallback) {
		lv_obj_set_hidden(mini_cover_fallback, true);
	}
}

static void mini_refresh_play_colors(bool has_media) {
	const theme_palette_t *pal = theme();
	if (mini_status_label) {
		lv_obj_set_style_text_color(mini_status_label, mini_playing_now ? pal->accent : pal->text_secondary, 0);
	}
	if (mini_play_btn && mini_play_icon) {
		if (has_media) {
			lv_obj_set_style_bg_color(mini_play_btn, pal->accent, 0);
			lv_obj_set_style_image_recolor(mini_play_icon, pal->accent_ink, 0);
		} else {
			lv_obj_set_style_bg_color(mini_play_btn, pal->surface_pressed, 0);
			lv_obj_set_style_image_recolor(mini_play_icon, pal->text_secondary, 0);
		}
		lv_obj_set_style_image_recolor_opa(mini_play_icon, LV_OPA_COVER, 0);
	}
}

static void mini_refresh(void) {
	if (!mini_card) {
		return;
	}

	device_state_t state;
	device_state_get(&state);

	bool playing = state.status == AUDIO_STATUS_PLAYING;
	bool has_media = state.live || state.current_file[0] != '\0';
	mini_playing_now = has_media && playing;

	if (state.live) {
		lv_label_set_text(mini_status_label, playing ? tr("miniplayer_playing") : tr("miniplayer_paused"));
		scrolltext_set(mini_title_label, state.metadata.title);
		lv_label_set_text(mini_artist_label, state.metadata.artist);
	} else if (state.current_file[0]) {
		const char *slash = strrchr(state.current_file, '/');
		const char *title = state.metadata.title[0] ? state.metadata.title : (slash ? slash + 1 : state.current_file);
		const char *artist = metadata_shown_artist(&state.metadata);

		lv_label_set_text(mini_status_label, playing ? tr("miniplayer_playing") : tr("miniplayer_paused"));
		scrolltext_set(mini_title_label, title);
		lv_label_set_text(mini_artist_label, artist);
	} else {
		lv_label_set_text(mini_status_label, tr("miniplayer_idle"));
		scrolltext_set(mini_title_label, tr("music"));
		lv_label_set_text(mini_artist_label, tr("browser_file_browser"));
	}

	const lv_image_dsc_t *glyph = playing ? &icon_pause_small : &icon_play_small;
	if (state.live && playing) {
		glyph = &icon_stop;
	}
	bool connecting = state.live && radio_is_connecting();
	if (!has_media) glyph = &icon_chevron_right;
	lv_image_set_src(mini_play_icon, glyph);
	lv_obj_set_hidden(mini_progress_bar, !has_media || state.live);
	lv_obj_set_hidden(mini_status_label, !has_media);
	if (connecting) {
		lv_obj_add_state(mini_play_btn, LV_STATE_DISABLED);
	} else {
		lv_obj_remove_state(mini_play_btn, LV_STATE_DISABLED);
	}
	mini_refresh_play_colors(has_media);

	int32_t promille = 0;
	if (!state.live && has_media && state.progress_total_secs > 0.0) {
		double ratio = state.progress_current_secs / state.progress_total_secs;
		if (ratio < 0.0) {
			ratio = 0.0;
		} else if (ratio > 1.0) {
			ratio = 1.0;
		}
		promille = (int32_t)(ratio * 1000.0 + 0.5);
	}
	lv_bar_set_value(mini_progress_bar, promille, LV_ANIM_OFF);

	const char *art_path = state.live ? state.live_cover : state.current_file;
	if (!has_media || !art_path || art_path[0] == '\0') {
		if (mini_cover_path[0] != '\0' || mini_cover_pending || mini_has_cover) {
			mini_cover_path[0] = '\0';
			mini_cover_pending = false;
			coverloader_release(COVERLOADER_MINIPLAYER);
			mini_clear_cover();
		}
	} else if (strcmp(art_path, mini_cover_path) != 0) {
		snprintf(mini_cover_path, sizeof(mini_cover_path), "%s", art_path);
		mini_clear_cover();
		mini_cover_pending = true;
		coverloader_request(COVERLOADER_MINIPLAYER, art_path, MINI_COVER_SIZE);
	}

	mini_poll_cover();
}

static void mini_timer_cb(lv_timer_t *timer) {
	(void)timer;
	mini_refresh();
}

static void mini_state_cb(void) {
	if (lv_screen_active() == main_menu_screen) {
		mini_refresh();
	}
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	mini_refresh();
}

static void tile_click_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	lv_obj_t *const *target = (lv_obj_t *const *)lv_event_get_user_data(e);
	if (target && *target) {
		switch_screen(*target);
	}
}

static void mini_card_click_cb(lv_event_t *e) {
	(void)e;
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	device_state_t state;
	device_state_get(&state);
	if (!state.live && !state.current_file[0]) {
		switch_screen(music_screen);
	} else {
		player_sheet_open(true);
	}
}

static void mini_play_click_cb(lv_event_t *e) {
	(void)e;
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	device_state_t state;
	device_state_get(&state);
	if (state.live || state.current_file[0]) {
		player_key_play_pause();
	} else {
		switch_screen(music_screen);
	}
}

static void main_menu_refresh_theme(void) {
	const theme_palette_t *pal = theme();
	bool dark = pal->dark;
	lv_obj_set_style_bg_color(dock, pal->surface, 0);
	lv_obj_set_style_border_color(dock, pal->border, 0);
	if (mini_card) {
		lv_obj_set_style_bg_color(mini_card, dark ? pal->elevated : pal->surface, 0);
		lv_obj_set_style_border_color(mini_card, pal->border, 0);
	}
	if (mini_cover_box) {
		lv_obj_set_style_bg_color(mini_cover_box, pal->cover_bg, 0);
	}
	if (mini_progress_bar) {
		lv_obj_set_style_bg_color(mini_progress_bar, pal->border, LV_PART_MAIN);
		lv_obj_set_style_bg_color(mini_progress_bar, pal->accent, LV_PART_INDICATOR);
	}

	device_state_t state;
	device_state_get(&state);
	bool has_media = state.live || state.current_file[0] != '\0';
	mini_refresh_play_colors(has_media);
}

static void add_app(lv_obj_t *row, const home_app_t *app, int width, bool in_dock) {
	lv_obj_t *tile = lv_button_create(row);
	lv_obj_remove_style_all(tile);
	lv_obj_set_size(tile, width, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(tile, 2, 0);
	lv_obj_set_scrollable(tile, false);
	lv_obj_set_style_opa(tile, LV_OPA_70, LV_STATE_PRESSED);
	lv_obj_set_event_bubble(tile, true);
	lv_obj_add_event_cb(tile, tile_click_cb, LV_EVENT_CLICKED, (void *)app->target);

	lv_obj_t *image = lv_image_create(tile);
	lv_image_set_src(image, app->image);
	lv_obj_set_size(image, in_dock ? 56 : 76, in_dock ? 56 : 76);
	lv_image_set_inner_align(image, LV_IMAGE_ALIGN_STRETCH);
	lv_obj_set_clickable(image, false);

	lv_obj_t *caption = lv_label_create(tile);
	lv_label_set_text(caption, tr(app->label_tag));
	lv_label_set_long_mode(caption, LV_LABEL_LONG_DOT);
	lv_obj_set_width(caption, width);
	lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(caption, &theme_style_text, 0);
	lv_obj_set_style_text_font(caption, &font_ui_16, 0);
	if (!in_dock) {
		lv_obj_set_style_text_color(caption, lv_color_white(), 0);
	}
}

static lv_obj_t *app_row(lv_obj_t *parent) {
	lv_obj_t *row = lv_obj_create(parent);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_event_bubble(row, true);
	return row;
}

void main_menu_init(gui_config_t *cfg) {
	lv_obj_add_style(main_menu_screen, &theme_style_screen, 0);
	lv_obj_set_scrollable(main_menu_screen, false);

	lv_obj_t *wallpaper = lv_image_create(main_menu_screen);
	lv_image_set_src(wallpaper, &atelier_dusk);
	lv_obj_set_size(wallpaper, cfg->screen_width, cfg->screen_height);
	lv_image_set_inner_align(wallpaper, LV_IMAGE_ALIGN_STRETCH);
	lv_obj_set_clickable(wallpaper, false);

	int top = cfg->top_bar_height;
	int width = cfg->screen_width - 40;
	lv_obj_t *workspace = lv_obj_create(main_menu_screen);
	make_transparent_container(workspace);
	lv_obj_set_size(workspace, width, cfg->screen_height - top);
	lv_obj_align(workspace, LV_ALIGN_TOP_MID, 0, top);
	lv_obj_set_style_pad_top(workspace, 20, 0);
	lv_obj_set_style_pad_bottom(workspace, 18, 0);
	lv_obj_set_style_pad_row(workspace, 18, 0);
	lv_obj_set_flex_flow(workspace, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(workspace, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *heading = lv_label_create(workspace);
	lv_label_set_text(heading, "Sonix Atelier");
	lv_obj_set_width(heading, lv_pct(100));
	lv_obj_set_style_text_color(heading, lv_color_white(), 0);
	lv_obj_set_style_text_font(heading, &font_ui_36_bold, 0);

	mini_card = lv_btn_create(workspace);
	lv_obj_set_size(mini_card, lv_pct(100), 146);
	lv_obj_add_style(mini_card, &theme_style_card, 0);
	lv_obj_add_style(mini_card, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(mini_card, 26, 0);
	lv_obj_set_style_border_width(mini_card, 1, 0);
	lv_obj_set_style_border_opa(mini_card, LV_OPA_COVER, 0);
	lv_obj_set_style_shadow_width(mini_card, 0, 0);
	lv_obj_set_style_pad_hor(mini_card, 14, 0);
	lv_obj_set_style_pad_ver(mini_card, 12, 0);
	lv_obj_set_style_pad_row(mini_card, 10, 0);
	lv_obj_set_scrollable(mini_card, false);
	lv_obj_set_event_bubble(mini_card, true);
	lv_obj_set_flex_flow(mini_card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(mini_card, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_event_cb(mini_card, mini_card_click_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *mini_top = lv_obj_create(mini_card);
	make_transparent_container(mini_top);
	lv_obj_set_width(mini_top, lv_pct(100));
	lv_obj_set_flex_grow(mini_top, 1);
	lv_obj_set_flex_flow(mini_top, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(mini_top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(mini_top, 12, 0);

	mini_cover_box = lv_obj_create(mini_top);
	lv_obj_remove_style_all(mini_cover_box);
	lv_obj_set_size(mini_cover_box, MINI_COVER_SIZE, MINI_COVER_SIZE);
	lv_obj_set_style_bg_opa(mini_cover_box, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(mini_cover_box, 12, 0);
	lv_obj_set_style_clip_corner(mini_cover_box, true, 0);
	lv_obj_set_scrollable(mini_cover_box, false);
	lv_obj_set_clickable(mini_cover_box, false);
	lv_obj_set_event_bubble(mini_cover_box, true);

	mini_cover_fallback = lv_image_create(mini_cover_box);
	lv_image_set_src(mini_cover_fallback, &icon_atelier_record);
	lv_obj_center(mini_cover_fallback);

	mini_cover_img = lv_image_create(mini_cover_box);
	lv_obj_set_size(mini_cover_img, MINI_COVER_SIZE, MINI_COVER_SIZE);
	lv_image_set_inner_align(mini_cover_img, LV_IMAGE_ALIGN_STRETCH);
	lv_obj_set_hidden(mini_cover_img, true);

	lv_obj_t *mini_texts = lv_obj_create(mini_top);
	make_transparent_container(mini_texts);
	lv_obj_set_flex_grow(mini_texts, 1);
	lv_obj_set_height(mini_texts, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_row(mini_texts, 2, 0);
	lv_obj_set_flex_flow(mini_texts, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(mini_texts, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	mini_status_label = lv_label_create(mini_texts);
	lv_label_set_text(mini_status_label, tr("miniplayer_idle"));
	lv_label_set_long_mode(mini_status_label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(mini_status_label, lv_pct(100));
	lv_obj_set_height(mini_status_label, lv_font_get_line_height(&font_ui_14));
	lv_obj_set_style_text_font(mini_status_label, &font_ui_14, 0);
	lv_obj_set_event_bubble(mini_status_label, true);

	mini_title_label = lv_label_create(mini_texts);
	lv_obj_set_width(mini_title_label, lv_pct(100));
	lv_obj_set_height(mini_title_label, lv_font_get_line_height(&font_ui_20_bold));
	lv_obj_add_style(mini_title_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(mini_title_label, &font_ui_20_bold, 0);
	scrolltext_apply(mini_title_label);
	scrolltext_set(mini_title_label, tr("music"));
	lv_obj_set_event_bubble(mini_title_label, true);

	mini_artist_label = lv_label_create(mini_texts);
	lv_label_set_text(mini_artist_label, "");
	lv_label_set_long_mode(mini_artist_label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(mini_artist_label, lv_pct(100));
	lv_obj_set_height(mini_artist_label, lv_font_get_line_height(&font_ui_16));
	lv_obj_add_style(mini_artist_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(mini_artist_label, &font_ui_16, 0);
	lv_obj_set_event_bubble(mini_artist_label, true);

	mini_play_btn = lv_btn_create(mini_top);
	lv_obj_set_size(mini_play_btn, 46, 46);
	lv_obj_set_style_radius(mini_play_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(mini_play_btn, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(mini_play_btn, 0, 0);
	lv_obj_set_style_shadow_width(mini_play_btn, 0, 0);
	lv_obj_set_style_opa(mini_play_btn, LV_OPA_40, LV_STATE_DISABLED);
	lv_obj_set_ext_click_area(mini_play_btn, 8);
	// Keep button taps local so Play/Pause does not also open the full sheet.
	lv_obj_set_event_bubble(mini_play_btn, false);
	lv_obj_add_event_cb(mini_play_btn, mini_play_click_cb, LV_EVENT_CLICKED, NULL);

	mini_play_icon = lv_image_create(mini_play_btn);
	lv_image_set_src(mini_play_icon, &icon_play_small);
	lv_obj_set_style_image_recolor_opa(mini_play_icon, LV_OPA_COVER, 0);
	lv_obj_center(mini_play_icon);

	mini_progress_bar = lv_bar_create(mini_card);
	lv_obj_set_size(mini_progress_bar, lv_pct(100), 4);
	lv_bar_set_range(mini_progress_bar, 0, 1000);
	lv_bar_set_value(mini_progress_bar, 0, LV_ANIM_OFF);
	lv_obj_set_style_radius(mini_progress_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(mini_progress_bar, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_radius(mini_progress_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
	lv_obj_set_style_bg_opa(mini_progress_bar, LV_OPA_COVER, LV_PART_INDICATOR);
	lv_obj_set_clickable(mini_progress_bar, false);
	lv_obj_set_event_bubble(mini_progress_bar, true);

	lv_obj_t *apps = app_row(workspace);
	for (size_t i = 0; i < sizeof(shortcuts) / sizeof(shortcuts[0]); i++) {
		add_app(apps, &shortcuts[i], width / 3, false);
	}

	lv_obj_t *space = lv_obj_create(workspace);
	make_transparent_container(space);
	lv_obj_set_size(space, 1, 0);
	lv_obj_set_flex_grow(space, 1);

	dock = app_row(workspace);
	lv_obj_set_style_bg_opa(dock, LV_OPA_90, 0);
	lv_obj_set_style_radius(dock, 30, 0);
	lv_obj_set_style_border_width(dock, 1, 0);
	lv_obj_set_style_pad_ver(dock, 10, 0);
	for (size_t i = 0; i < sizeof(dock_apps) / sizeof(dock_apps[0]); i++) {
		add_app(dock, &dock_apps[i], (width - 8) / 4, true);
	}

	main_menu_refresh_theme();
	mini_refresh();

	theme_register_refresh(main_menu_refresh_theme);
	player_register_state_cb(mini_state_cb);

	mini_timer = lv_timer_create(mini_timer_cb, MINI_POLL_MS, NULL);
	pagetimer_bind(mini_timer, main_menu_screen, NULL);
	power_pause_in_standby(mini_timer);

	lv_obj_add_event_cb(main_menu_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}
