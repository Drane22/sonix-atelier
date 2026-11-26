#include "player.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/library/audiobookextras.h"
#include "src/gui/library/browser.h"
#include "src/gui/library/medialist.h"
#include "src/gui/nowplaying/ambient_palette.h"
#include "src/gui/nowplaying/chapters.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/coverloader.h"
#include "src/gui/nowplaying/lyricsearch.h"
#include "src/gui/nowplaying/trackmenu.h"
#include "src/gui/nowplaying/upnext.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/quickpanel.h"
#include "src/gui/shell/scrolltext.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/shell/topbar.h"
#include "src/system/audio/audio.h"
#include "src/system/audio/readahead.h"
#include "src/system/audio/waveform.h"
#include "src/system/bluetooth/btreceiver.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/device/led.h"
#include "src/system/device/power.h"
#include "src/system/library/library.h"
#include "src/system/library/lrclib.h"
#include "src/system/library/lyrics.h"
#include "src/system/net/wifi.h"
#include "src/system/playback/audiobook.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"
#include "src/system/playback/sleeptimer.h"
#include "src/system/gearboy/gearboy.h"
#include "src/system/remote/airplay.h"
#include "src/system/remote/dlna.h"
#include "src/system/streaming/podcast.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/podcaststate.h"
#include "src/system/streaming/podcastsubs.h"
#include "src/system/streaming/qobuzcache.h"
#include "src/system/streaming/qobuzsync.h"
#include "src/system/streaming/radio.h"
#include "src/system/streaming/tidalcache.h"
#include "src/system/streaming/tidalsync.h"

#include "lvgl/lvgl.h"

// ===========================================================================
// Sonix Atelier — Ambient Edition
// Now Playing & First-Class Synchronized Lyrics Experience
// ===========================================================================

lv_obj_t *player_screen;

// Motion & polling timings (ms)
#define POLL_PERIOD_PLAYING_MS 500
#define POLL_PERIOD_IDLE_MS 2000
#define SMOOTH_PERIOD_MS 60
#define POLLS_FAST_AFTER_STOP 4
#define COVER_POLL_MS 30

#define CAROUSEL_SLIDE_MS 240
#define PALETTE_TRANSITION_MS 280
#define LYRICS_MODE_ANIM_MS 220
#define LYRICS_SCROLL_MS 250
#define SHEET_ANIM_MS 220

#define CAROUSEL_PEEK_PX 14
#define CAROUSEL_GAP_PX 12
#define CAROUSEL_DRAG_PX 10
#define BADGE_INSET 12

#define WAVE_BAR_SOLID 4
#define WAVE_BAR_GAP 2
#define WAVE_BAR_PITCH (WAVE_BAR_SOLID + WAVE_BAR_GAP)
#define WAVE_MIN_HALF 2
#define WAVE_TODO_OPA 145

#define LYRICS_USER_HOLD_MS 8000
#define LYRICS_TAP_WAIT_MS 2000
#define LYRICS_LINE_GAP_INT 14
#define LYRICS_LINE_GAP_FS 18
#define LYRICS_OFFSET_STEP_MS 100
#define LYRICS_OFFSET_LIMIT_MS 5000

typedef enum {
	LYRICS_VIEW_NONE = 0,
	LYRICS_VIEW_INTEGRATED = 1,
	LYRICS_VIEW_FULLSCREEN = 2,
} lyrics_view_state_t;

// ---------------------------------------------------------------------------
// Geometry & Preferences State
// ---------------------------------------------------------------------------

static int screen_w = 480;
static int screen_h = 800;
static int bar_height = 44;
static bool bar_always = true;
static bool is_tall_panel = true; // true on HiBy R1 (480x800), false on R3PII (480x720)

static int content_w = 430;  // 72 * 6 - 2 = 430 px exact integer grid
static int side_margin = 25; // (480 - 430) / 2
static int header_y = 34;
static int header_h = 40;
static int stage_y = 76;
static int stage_h = 432;
static int hero_box = 430;
static int peek_box = 340;
static int wave_h = 34;

static player_layout_t layout_choice = PLAYER_LAYOUT_STANDARD;
static bool adaptive_accent = true;
static player_lyrics_mode_t lyrics_default_mode = PLAYER_LYRICS_INTEGRATED;
static bool lyrics_remember_mode = true;
static player_lyrics_mode_t lyrics_last_mode = PLAYER_LYRICS_INTEGRATED;
static player_lyrics_text_size_t lyrics_text_size = PLAYER_LYRICS_TEXT_MEDIUM;
static int lyrics_offset_ms = 0;

// Remembered shuffle flag so cycling Repeat through REPEAT_ONE and back keeps
// shuffle enabled when the user had Shuffle switched on.
static bool shuffle_remembered = false;

// ---------------------------------------------------------------------------
// Palette & Animation State
// ---------------------------------------------------------------------------

static ambient_palette_t pal_cur;
static ambient_palette_t pal_from;
static ambient_palette_t pal_target;
static int pal_anim_tag;

static uint32_t current_raw_tone = 0;
static int current_mean_luma = -1;
static bool current_has_cover = false;

// ---------------------------------------------------------------------------
// Widget Tree Handles
// ---------------------------------------------------------------------------

// Header
static lv_obj_t *header_row;
static lv_obj_t *header_title_label;
static lv_obj_t *more_btn_obj;
static lv_obj_t *more_btn_icon;

// Center stage: Carousel (Mode 0) + Integrated Lyrics (Mode A)
static lv_obj_t *stage_container;
static lv_obj_t *carousel_stage;

typedef struct {
	lv_obj_t *frame;
	lv_obj_t *placeholder;
	lv_obj_t *img;
	cover_image_t cover;
	bool has_cover;
	char path[512];
} carousel_slot_t;

static carousel_slot_t cslots[3]; // 0 = prev, 1 = curr (hero), 2 = next
static bool carousel_has_prev;
static bool carousel_has_next;
static bool prev_cover_pending;
static bool next_cover_pending;
static int32_t carousel_offset_px; // horizontal translation from resting positions
static int carousel_anim_tag;

// Source badges on hero frame
static lv_obj_t *live_badge;
static lv_obj_t *qobuz_badge;
static lv_obj_t *tidal_badge;
static lv_obj_t *podcast_badge;

// Drag Up-Next / Previous preview caption inside carousel_stage
static lv_obj_t *drag_caption_pill;
static lv_obj_t *drag_caption_label;

// Track Info Row
static lv_obj_t *track_info_row;
static lv_obj_t *song_title_label;
static lv_obj_t *song_artist_label;
static lv_obj_t *fav_btn_obj;
static lv_obj_t *fav_btn_icon;

// Waveform & Seek Section
static lv_obj_t *wave_section;
static lv_obj_t *wave_box;
static lv_obj_t *wave_canvas;
static uint8_t *wave_buf;
static int wave_w;
static uint8_t wave_bars[WAVEFORM_BARS];
static int wave_ready;
static int wave_drawn_px = -1;
static uint32_t wave_drawn_sig = 0;
static lv_obj_t *progress_slider;
static lv_obj_t *meta_clocks_row;
static lv_obj_t *elapsed_label;
static lv_obj_t *format_label;
static lv_obj_t *remaining_label;

// 5-Button Transport Row
static lv_obj_t *controls_row;
static lv_obj_t *shuffle_btn_obj;
static lv_obj_t *shuffle_btn_icon;
static lv_obj_t *shuffle_dot;
static lv_obj_t *prev_btn_obj;
static lv_obj_t *prev_icon;
static lv_obj_t *play_btn;
static lv_obj_t *play_btn_icon;
static lv_obj_t *next_btn_obj;
static lv_obj_t *next_icon;
static lv_obj_t *repeat_btn_obj;
static lv_obj_t *repeat_btn_icon;
static lv_obj_t *repeat_dot;
static lv_obj_t *speed_btn_obj;
static lv_obj_t *speed_btn_icon;

// Bottom Shortcuts Row
static lv_obj_t *shortcuts_row;
static lv_obj_t *lyrics_shortcut_btn;
static lv_obj_t *lyrics_shortcut_icon;
static lv_obj_t *lyrics_shortcut_label;
static lv_obj_t *upnext_bearing_btn;
static lv_obj_t *upnext_bearing_label;
static lv_obj_t *queue_shortcut_btn;
static lv_obj_t *queue_shortcut_icon;
static lv_obj_t *queue_shortcut_label;

// Lyrics Shared Model & Mode A / Mode B Containers
static lyrics_view_state_t lyrics_state = LYRICS_VIEW_NONE;
static lv_obj_t *integrated_lyrics_box;
static lv_obj_t *int_lyrics_search_btn;
static lv_obj_t *int_lyrics_search_icon;
static lv_obj_t *int_lyrics_expand_btn;
static lv_obj_t *int_lyrics_expand_icon;

static lv_obj_t *fullscreen_lyrics_box;
static lv_obj_t *fs_header_row;
static lv_obj_t *fs_thumb_box;
static lv_obj_t *fs_thumb_fallback;
static lv_obj_t *fs_thumb_img;
static cover_image_t fs_thumb_cover;
static bool fs_has_thumb;
static lv_obj_t *fs_title_label;
static lv_obj_t *fs_artist_label;
static lv_obj_t *fs_size_btn;
static lv_obj_t *fs_size_icon;
static lv_obj_t *fs_offset_toggle_btn;
static lv_obj_t *fs_offset_toggle_label;
static lv_obj_t *fs_close_btn;
static lv_obj_t *fs_close_icon;

static lv_obj_t *fs_offset_bar;
static lv_obj_t *fs_offset_minus_btn;
static lv_obj_t *fs_offset_value_btn;
static lv_obj_t *fs_offset_value_label;
static lv_obj_t *fs_offset_plus_btn;
static lv_obj_t *fs_offset_search_btn;
static lv_obj_t *fs_offset_search_icon;
static bool fs_offset_bar_open;

static lv_obj_t *fs_stage_area;
static lv_obj_t *fs_bottom_deck;
static lv_obj_t *fs_slider;
static lv_obj_t *fs_elapsed_label;
static lv_obj_t *fs_duration_label;
static lv_obj_t *fs_prev_btn;
static lv_obj_t *fs_prev_icon;
static lv_obj_t *fs_play_btn;
static lv_obj_t *fs_play_icon;
static lv_obj_t *fs_next_btn;
static lv_obj_t *fs_next_icon;

// Shared lyrics viewport & empty state (reparented between Mode A & Mode B)
static lv_obj_t *lyrics_view;
static lv_obj_t *lyrics_empty_box;
static lv_obj_t *lyrics_empty_icon;
static lv_obj_t *lyrics_empty_label;
static lv_obj_t *lyrics_empty_search_btn;
static lv_obj_t *lyrics_empty_search_label;
static lv_obj_t *resume_sync_btn;
static lv_obj_t *resume_sync_label;

static lyrics_t lyrics_cur;
static bool lyrics_loaded;
static char lyrics_path[1024];
static unsigned lyrics_generation;
static int lyrics_lit = -1;
static bool lyrics_manual_hold;
static uint32_t lyrics_user_until;
static bool lyrics_centred;
static int32_t lyrics_tap_ms = -1;
static uint32_t lyrics_tap_until;

// Playback & Timers State
static lv_timer_t *progress_slider_timer;
static lv_timer_t *smooth_timer;
static lv_timer_t *sleep_timer;
static lv_timer_t *cover_poll_timer;

static bool sheet_open;
static int sheet_width;
static lv_obj_t *sheet_under;
static int drag_origin_x;
static lv_point_t drag_start_point;
static bool dragging;
static bool drag_committed;

static double progress_anchor_secs;
static uint32_t progress_anchor_tick;
static bool progress_running;
static int polls_since_stop = POLLS_FAST_AFTER_STOP;
static double current_total_length;
static double view_start;
static double view_length;
static char progress_label_text[32];
static bool slider_dragging;

static bool live_mode;
static bool live_transport_hidden;
static bool live_custom_nav;
static bool connecting_shown;
static bool audiobook_mode;
static bool podcast_mode;
static bool audiobook_has_chapters;
static int audiobook_skip_shown_back;
static int audiobook_skip_shown_forward;
static bool audiobook_mode_valid;

// Artwork loader tracking
static bool cover_request_outstanding;
static uint64_t cover_shown_id;
static uint64_t cover_incoming_id;
static enum {
	COVER_ID_IDLE,
	COVER_ID_NEW_TRACK,
	COVER_ID_MEASURE,
} cover_id_state;
static char cover_shown_path[512];
static void (*cover_release_cb)(void);

extern const uint8_t player_sleeve_pixels[];
static const lv_image_dsc_t player_sleeve = {
	.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, .w = 480, .h = 480, .stride = 960},
	.data_size = 480 * 480 * 2,
	.data = player_sleeve_pixels,
};

#define MAX_PLAYER_STATE_CBS 8
static player_state_cb_t state_cbs[MAX_PLAYER_STATE_CBS];
static int state_cb_count;

// Forward declarations
static void notify_state_cbs(void);
static void cover_poll_start(void);
static void refresh_now_playing(void);
static void update_progress(void);
static void update_fav_button(void);
static void update_transport_modes(void);
static void update_speed_button(void);
static void update_format_label(const device_state_t *state);
static void update_queue_and_shortcuts(void);
static void apply_palette_to_widgets(void);
static void recompute_palette(bool animate);
static void carousel_sync_adjacent_requests(void);
static void carousel_layout_slots(void);
static void carousel_animate_from(int32_t start_offset);
static void wave_paint(int played_px);
static void wave_refresh(const char *path, double position);
static void lyrics_set_mode(lyrics_view_state_t mode, bool animate);
static void lyrics_apply_line_styles(void);
static void lyrics_follow(double seconds);
static void lyrics_request(const device_state_t *state);
static void lyrics_search_start(void);
static void bar_over_player_update(void);
static void update_fs_thumb(void);

// ---------------------------------------------------------------------------
// Utility Helpers
// ---------------------------------------------------------------------------

static bool motion_enabled(void) {
	return !theme_reduced_motion() && sheet_open && power_screen_is_on();
}

static void set_label_text_if_changed(lv_obj_t *label, const char *text) {
	if (!label || !text) {
		return;
	}
	if (strcmp(lv_label_get_text(label), text) != 0) {
		lv_label_set_text(label, text);
	}
}

static void style_clean_box(lv_obj_t *obj) {
	lv_obj_remove_style_all(obj);
	lv_obj_set_scrollable(obj, false);
	lv_obj_set_clickable(obj, false);
}

static void smooth_timer_sync(void) {
	if (!smooth_timer) {
		return;
	}
	if (progress_running && sheet_open && power_screen_is_on() && !slider_dragging) {
		lv_timer_resume(smooth_timer);
	} else {
		lv_timer_pause(smooth_timer);
	}
}

static void compute_geometry(const gui_config_t *cfg) {
	screen_w = (int)cfg->screen_width;
	screen_h = (int)cfg->screen_height;
	bar_height = (int)cfg->top_bar_height;
	is_tall_panel = (screen_h >= 780);

	// 72 bars * 6 px pitch - 2 px gap = 430 px exact integer grid on 480 px panel.
	content_w = WAVEFORM_BARS * WAVE_BAR_PITCH - WAVE_BAR_GAP;
	if (content_w > screen_w - 32) {
		content_w = screen_w - 32;
	}
	side_margin = (screen_w - content_w) / 2;

	int top_offset = bar_always ? (bar_height - 8) : 4;
	if (top_offset < 4) {
		top_offset = 4;
	}
	header_y = top_offset;
	header_h = 40;
	stage_y = header_y + header_h + 2;

	if (is_tall_panel) {
		// HiBy R1 (480 x 800):
		//   Header ~36..76, Stage ~78..508 (430 px), Track Info ~514..592,
		//   Waveform+Meta ~596..662, Controls ~666..744, Shortcuts ~748..796.
		int bottom_reserved = bar_always ? 290 : 298;
		stage_h = screen_h - stage_y - bottom_reserved;
		if (stage_h > content_w) {
			stage_h = content_w;
		}
		if (stage_h < 320) {
			stage_h = 320;
		}
		hero_box = stage_h < content_w ? stage_h : content_w;
		peek_box = hero_box * 80 / 100;
		wave_h = 34;
	} else {
		// HiBy R3 Pro II (480 x 720):
		int bottom_reserved = 272;
		stage_h = screen_h - stage_y - bottom_reserved;
		if (stage_h > 360) {
			stage_h = 360;
		}
		if (stage_h < 260) {
			stage_h = 260;
		}
		hero_box = stage_h;
		peek_box = hero_box * 80 / 100;
		wave_h = 30;
	}
}

// ---------------------------------------------------------------------------
// Adaptive & Fixed Ambient Palette Engine Integration
// ---------------------------------------------------------------------------

lv_color_t player_active_accent(void) {
	return pal_cur.accent;
}

static void compute_target_palette(ambient_palette_t *out) {
	if (adaptive_accent && current_has_cover && current_raw_tone != 0) {
		ambient_palette_adaptive(true, current_raw_tone, current_mean_luma, out);
		return;
	}
	int idx = theme_get_accent();
	uint32_t preset_rgb = 0;
	if (idx > 0) {
		lv_color_t c = theme_accent_preset(idx);
		preset_rgb = ((uint32_t)c.red << 16) | ((uint32_t)c.green << 8) | c.blue;
	}
	ambient_palette_fixed(idx, preset_rgb, out);
}

static void apply_palette_to_widgets(void) {
	if (!player_screen) {
		return;
	}

	// Main screen & Mode B Full-screen share the same album-derived dark vertical gradient.
	lv_obj_t *canvases[] = {player_screen, fullscreen_lyrics_box};
	for (size_t i = 0; i < sizeof(canvases) / sizeof(canvases[0]); i++) {
		if (!canvases[i]) {
			continue;
		}
		lv_obj_set_style_bg_color(canvases[i], pal_cur.bg_top, 0);
		lv_obj_set_style_bg_grad_color(canvases[i], pal_cur.bg_bottom, 0);
		lv_obj_set_style_bg_grad_dir(canvases[i], LV_GRAD_DIR_VER, 0);
		lv_obj_set_style_bg_opa(canvases[i], LV_OPA_COVER, 0);
	}

	if (header_title_label) {
		lv_obj_set_style_text_color(header_title_label, pal_cur.text_secondary, 0);
	}
	if (more_btn_icon) {
		lv_obj_set_style_image_recolor(more_btn_icon, pal_cur.text_primary, 0);
		lv_obj_set_style_image_recolor_opa(more_btn_icon, LV_OPA_80, 0);
	}

	for (int i = 0; i < 3; i++) {
		if (cslots[i].frame) {
			lv_obj_set_style_bg_color(cslots[i].frame, pal_cur.surface, 0);
		}
		if (cslots[i].placeholder && lv_image_get_src(cslots[i].placeholder) != &player_sleeve) {
			lv_obj_set_style_image_recolor(cslots[i].placeholder, pal_cur.text_secondary, 0);
		}
	}

	if (drag_caption_pill) {
		lv_obj_set_style_bg_color(drag_caption_pill, pal_cur.surface, 0);
		lv_obj_set_style_border_color(drag_caption_pill, pal_cur.border, 0);
	}
	if (drag_caption_label) {
		lv_obj_set_style_text_color(drag_caption_label, pal_cur.text_primary, 0);
	}

	if (song_title_label) {
		lv_obj_set_style_text_color(song_title_label, pal_cur.text_primary, 0);
	}
	if (song_artist_label) {
		lv_obj_set_style_text_color(song_artist_label, pal_cur.text_secondary, 0);
	}
	if (elapsed_label) {
		lv_obj_set_style_text_color(elapsed_label, pal_cur.text_secondary, 0);
	}
	if (format_label) {
		lv_obj_set_style_text_color(format_label, pal_cur.text_secondary, 0);
	}
	if (remaining_label) {
		lv_obj_set_style_text_color(remaining_label, pal_cur.text_secondary, 0);
	}

	// Conventional fallback seek bar & Mode B compact seek bar
	lv_obj_t *sliders[] = {progress_slider, fs_slider};
	for (size_t i = 0; i < sizeof(sliders) / sizeof(sliders[0]); i++) {
		if (!sliders[i]) {
			continue;
		}
		lv_obj_set_style_bg_color(sliders[i], pal_cur.wave_unplayed, LV_PART_MAIN);
		lv_obj_set_style_bg_color(sliders[i], pal_cur.accent, LV_PART_INDICATOR);
		lv_obj_set_style_bg_color(sliders[i], pal_cur.accent, LV_PART_KNOB);
	}

	// Transport
	if (play_btn) {
		lv_obj_set_style_bg_color(play_btn, pal_cur.accent, 0);
		lv_obj_set_style_bg_color(play_btn, pal_cur.accent_pressed, LV_STATE_PRESSED);
	}
	if (play_btn_icon) {
		lv_obj_set_style_image_recolor(play_btn_icon, pal_cur.accent_ink, 0);
		lv_obj_set_style_image_recolor_opa(play_btn_icon, LV_OPA_COVER, 0);
	}
	if (fs_play_btn) {
		lv_obj_set_style_bg_color(fs_play_btn, pal_cur.accent, 0);
		lv_obj_set_style_bg_color(fs_play_btn, pal_cur.accent_pressed, LV_STATE_PRESSED);
	}
	if (fs_play_icon) {
		lv_obj_set_style_image_recolor(fs_play_icon, pal_cur.accent_ink, 0);
		lv_obj_set_style_image_recolor_opa(fs_play_icon, LV_OPA_COVER, 0);
	}

	lv_obj_t *primary_icons[] = {prev_icon, next_icon, fs_prev_icon, fs_next_icon};
	for (size_t i = 0; i < sizeof(primary_icons) / sizeof(primary_icons[0]); i++) {
		if (primary_icons[i]) {
			lv_obj_set_style_image_recolor(primary_icons[i], pal_cur.text_primary, 0);
			lv_obj_set_style_image_recolor_opa(primary_icons[i], LV_OPA_COVER, 0);
		}
	}

	update_transport_modes();
	update_speed_button();
	update_fav_button();
	update_queue_and_shortcuts();

	// Lyrics chrome
	if (integrated_lyrics_box) {
		lv_obj_set_style_bg_color(integrated_lyrics_box, pal_cur.surface, 0);
		lv_obj_set_style_border_color(integrated_lyrics_box, pal_cur.border, 0);
	}
	lv_obj_t *lyr_icons[] = {int_lyrics_search_icon, int_lyrics_expand_icon, fs_size_icon, fs_close_icon,
							 fs_offset_search_icon};
	for (size_t i = 0; i < sizeof(lyr_icons) / sizeof(lyr_icons[0]); i++) {
		if (lyr_icons[i]) {
			lv_obj_set_style_image_recolor(lyr_icons[i], pal_cur.text_primary, 0);
			lv_obj_set_style_image_recolor_opa(lyr_icons[i], LV_OPA_80, 0);
		}
	}
	if (fs_thumb_box) {
		lv_obj_set_style_bg_color(fs_thumb_box, pal_cur.surface, 0);
	}
	if (fs_thumb_fallback) {
		lv_obj_set_style_image_recolor(fs_thumb_fallback, pal_cur.text_secondary, 0);
	}
	if (fs_title_label) {
		lv_obj_set_style_text_color(fs_title_label, pal_cur.text_primary, 0);
	}
	if (fs_artist_label) {
		lv_obj_set_style_text_color(fs_artist_label, pal_cur.text_secondary, 0);
	}
	if (fs_elapsed_label) {
		lv_obj_set_style_text_color(fs_elapsed_label, pal_cur.text_secondary, 0);
	}
	if (fs_duration_label) {
		lv_obj_set_style_text_color(fs_duration_label, pal_cur.text_secondary, 0);
	}
	if (fs_offset_toggle_label) {
		lv_obj_set_style_text_color(fs_offset_toggle_label,
									(fs_offset_bar_open || lyrics_offset_ms != 0) ? pal_cur.accent
																				  : pal_cur.text_primary,
									0);
	}
	if (fs_offset_bar) {
		lv_obj_set_style_bg_color(fs_offset_bar, pal_cur.surface, 0);
		lv_obj_set_style_border_color(fs_offset_bar, pal_cur.border, 0);
	}
	if (fs_offset_value_label) {
		lv_obj_set_style_text_color(fs_offset_value_label,
									lyrics_offset_ms != 0 ? pal_cur.accent : pal_cur.text_primary, 0);
	}
	if (resume_sync_btn) {
		lv_obj_set_style_bg_color(resume_sync_btn, pal_cur.surface_pressed, 0);
		lv_obj_set_style_border_color(resume_sync_btn, pal_cur.accent, 0);
	}
	if (resume_sync_label) {
		lv_obj_set_style_text_color(resume_sync_label, pal_cur.text_primary, 0);
	}
	if (lyrics_empty_icon) {
		lv_obj_set_style_image_recolor(lyrics_empty_icon, pal_cur.text_secondary, 0);
		lv_obj_set_style_image_recolor_opa(lyrics_empty_icon, LV_OPA_60, 0);
	}
	if (lyrics_empty_label) {
		lv_obj_set_style_text_color(lyrics_empty_label, pal_cur.text_secondary, 0);
	}
	if (lyrics_empty_search_btn) {
		lv_obj_set_style_bg_color(lyrics_empty_search_btn, pal_cur.surface_pressed, 0);
		lv_obj_set_style_border_color(lyrics_empty_search_btn, pal_cur.border, 0);
	}
	if (lyrics_empty_search_label) {
		lv_obj_set_style_text_color(lyrics_empty_search_label, pal_cur.accent, 0);
	}

	lyrics_apply_line_styles();

	// Force waveform redraw with new accent/unplayed colours
	wave_drawn_sig = 0;
	if (wave_ready > 0 && wave_canvas && !lv_obj_is_hidden(wave_canvas)) {
		wave_paint(wave_drawn_px >= 0 ? wave_drawn_px : 0);
	}
}

static void palette_anim_exec_cb(void *var, int32_t v) {
	(void)var;
	ambient_palette_lerp(&pal_from, &pal_target, (int)v, &pal_cur);
	apply_palette_to_widgets();
}

static void recompute_palette(bool animate) {
	ambient_palette_t next_pal;
	compute_target_palette(&next_pal);

	lv_anim_delete(&pal_anim_tag, palette_anim_exec_cb);

	if (!animate || !motion_enabled()) {
		pal_cur = next_pal;
		pal_target = next_pal;
		apply_palette_to_widgets();
		return;
	}

	pal_from = pal_cur;
	pal_target = next_pal;
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, &pal_anim_tag);
	lv_anim_set_exec_cb(&a, palette_anim_exec_cb);
	lv_anim_set_values(&a, 0, 1000);
	lv_anim_set_duration(&a, PALETTE_TRANSITION_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_start(&a);
}

// ---------------------------------------------------------------------------
// Source Mode Helpers (Local, Radio, Audiobook, Podcast)
// ---------------------------------------------------------------------------

static bool playing_local_file(const device_state_t *state) {
	if (state->live || !state->current_file[0] || audiobook_is_playing()) {
		return false;
	}
	airplay_state_t air;
	airplay_get_state(&air);
	if (air.playing) {
		return false;
	}
	const char *f = state->current_file;
	return !qobuzcache_owns(f) && !tidalcache_owns(f) && !podcastcache_is_episode(f) && !dlna_owns_path(f) &&
		   !dlna_owns_playback();
}

static sleeptimer_kind_t sleeptimer_kind_playing(void) {
	if (radio_is_active() || gearboy_running() || btreceiver_is_active()) {
		return SLEEPTIMER_COUNT;
	}
	if (audiobook_is_playing()) {
		return SLEEPTIMER_AUDIOBOOK;
	}
	char path[512];
	audio_get_current_file(path, sizeof(path));
	if (podcastcache_is_episode(path)) {
		return SLEEPTIMER_PODCAST;
	}
	return SLEEPTIMER_MUSIC;
}

static void refresh_cover_placeholders(void) {
	const lv_image_dsc_t *art = &player_sleeve;
	if (live_mode) {
		art = &icon_radio_player;
	} else if (audiobook_mode) {
		art = &icon_book_headphones;
	} else if (podcast_mode) {
		art = &icon_podcast_cover;
	}
	bool is_sleeve = (art == &player_sleeve);

	for (int i = 0; i < 3; i++) {
		lv_obj_t *ph = cslots[i].placeholder;
		if (!ph) {
			continue;
		}
		int box = (i == 1) ? hero_box : peek_box;
		if (lv_image_get_src(ph) != art) {
			lv_image_set_src(ph, art);
		}
		if (is_sleeve) {
			lv_obj_set_size(ph, box, box);
			lv_image_set_inner_align(ph, LV_IMAGE_ALIGN_COVER);
			lv_obj_set_style_image_recolor_opa(ph, LV_OPA_TRANSP, 0);
		} else {
			lv_obj_set_size(ph, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
			lv_image_set_inner_align(ph, LV_IMAGE_ALIGN_CENTER);
			lv_obj_set_style_image_recolor(ph, pal_cur.text_secondary, 0);
			lv_obj_set_style_image_recolor_opa(ph, LV_OPA_COVER, 0);
		}
		lv_obj_center(ph);
	}
}

static void sync_header_title(void) {
	if (!header_title_label) {
		return;
	}
	const char *tag = "miniplayer_playing";
	if (live_mode) {
		tag = "radio";
	} else if (audiobook_mode) {
		tag = "audiobooks";
	} else if (podcast_mode) {
		tag = "podcast";
	}
	set_label_text_if_changed(header_title_label, tr(tag));
}

// ---------------------------------------------------------------------------
// 3-Slot Swipeable Album Artwork Carousel
// ---------------------------------------------------------------------------

static void slot_clear_cover(carousel_slot_t *s) {
	if (s->img) {
		lv_image_set_src(s->img, NULL);
		lv_obj_set_hidden(s->img, true);
	}
	if (s->placeholder) {
		lv_obj_set_hidden(s->placeholder, false);
	}
	if (s->has_cover) {
		cover_free(&s->cover);
		s->has_cover = false;
	}
}

static void slot_apply_geometry(carousel_slot_t *s, int max_box) {
	if (!s->frame) {
		return;
	}
	if (s->has_cover && s->cover.pixels) {
		int iw = (int)s->cover.dsc.header.w;
		int ih = (int)s->cover.dsc.header.h;
		// Maintain original aspect ratio without stretching or boxing; for tiny
		// low-res thumbnails (< 55% of stage), rest them inside a clean matte
		// sleeve square so they feel intentional rather than lost.
		bool tiny = (iw < max_box * 55 / 100 && ih < max_box * 55 / 100);
		int fw = tiny ? max_box : iw;
		int fh = tiny ? max_box : ih;
		lv_obj_set_size(s->frame, fw, fh);
		lv_obj_set_style_bg_opa(s->frame, tiny ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
		lv_image_set_src(s->img, &s->cover.dsc);
		lv_obj_set_size(s->img, iw, ih);
		lv_image_set_inner_align(s->img, LV_IMAGE_ALIGN_CENTER);
		lv_obj_center(s->img);
		lv_obj_set_hidden(s->img, false);
		if (s->placeholder) {
			lv_obj_set_hidden(s->placeholder, true);
		}
	} else {
		lv_obj_set_size(s->frame, max_box, max_box);
		lv_obj_set_style_bg_opa(s->frame, LV_OPA_COVER, 0);
		if (s->img) {
			lv_obj_set_hidden(s->img, true);
		}
		if (s->placeholder) {
			lv_obj_set_hidden(s->placeholder, false);
			lv_obj_center(s->placeholder);
		}
	}
}

static int carousel_stride(void) {
	return screen_w - 2 * CAROUSEL_PEEK_PX;
}

static void carousel_layout_slots(void) {
	if (!carousel_stage) {
		return;
	}

	int stride = carousel_stride();
	int32_t dx = carousel_offset_px;
	int cy = stage_h / 2;

	// Hero slot (1)
	int hw = lv_obj_get_width(cslots[1].frame);
	int hh = lv_obj_get_height(cslots[1].frame);
	if (hw <= 0) hw = hero_box;
	if (hh <= 0) hh = hero_box;
	int hero_cx = screen_w / 2 + (int)dx;
	lv_obj_set_pos(cslots[1].frame, hero_cx - hw / 2, cy - hh / 2);

	int abs_dx = dx < 0 ? (int)(-dx) : (int)dx;
	if (abs_dx > stride) {
		abs_dx = stride;
	}
	lv_opa_t hero_opa = (lv_opa_t)(255 - (abs_dx * 85) / stride);
	lv_obj_set_style_opa(cslots[1].frame, hero_opa, 0);

	// Previous slot (0)
	if (carousel_has_prev && lyrics_state == LYRICS_VIEW_NONE) {
		int pw = lv_obj_get_width(cslots[0].frame);
		int ph = lv_obj_get_height(cslots[0].frame);
		if (pw <= 0) pw = peek_box;
		if (ph <= 0) ph = peek_box;
		int rest_cx = CAROUSEL_PEEK_PX - pw / 2;
		int target_cx = screen_w / 2;
		int cx0;
		lv_opa_t opa0 = 165;
		if (dx >= 0) {
			int t = (int)dx > stride ? stride : (int)dx;
			cx0 = rest_cx + (target_cx - rest_cx) * t / stride;
			opa0 = (lv_opa_t)(165 + (90 * t) / stride);
		} else {
			cx0 = rest_cx + (int)dx;
		}
		lv_obj_set_pos(cslots[0].frame, cx0 - pw / 2, cy - ph / 2);
		lv_obj_set_style_opa(cslots[0].frame, opa0, 0);
		lv_obj_set_hidden(cslots[0].frame, false);
	} else {
		lv_obj_set_hidden(cslots[0].frame, true);
	}

	// Next slot (2)
	if (carousel_has_next && lyrics_state == LYRICS_VIEW_NONE) {
		int nw = lv_obj_get_width(cslots[2].frame);
		int nh = lv_obj_get_height(cslots[2].frame);
		if (nw <= 0) nw = peek_box;
		if (nh <= 0) nh = peek_box;
		int rest_cx = (screen_w - CAROUSEL_PEEK_PX) + nw / 2;
		int target_cx = screen_w / 2;
		int cx2;
		lv_opa_t opa2 = 165;
		if (dx <= 0) {
			int t = (int)(-dx) > stride ? stride : (int)(-dx);
			cx2 = rest_cx - (rest_cx - target_cx) * t / stride;
			opa2 = (lv_opa_t)(165 + (90 * t) / stride);
		} else {
			cx2 = rest_cx + (int)dx;
		}
		lv_obj_set_pos(cslots[2].frame, cx2 - nw / 2, cy - nh / 2);
		lv_obj_set_style_opa(cslots[2].frame, opa2, 0);
		lv_obj_set_hidden(cslots[2].frame, false);
	} else {
		lv_obj_set_hidden(cslots[2].frame, true);
	}

	// Badges ride the top-right corner of the hero frame
	lv_obj_t *const badges[] = {live_badge, qobuz_badge, tidal_badge, podcast_badge};
	for (size_t i = 0; i < sizeof(badges) / sizeof(badges[0]); i++) {
		if (badges[i] && !lv_obj_is_hidden(badges[i])) {
			lv_obj_align(badges[i], LV_ALIGN_TOP_RIGHT, -BADGE_INSET, BADGE_INSET);
			lv_obj_move_foreground(badges[i]);
		}
	}
}

static void carousel_update_drag_caption(int32_t dx) {
	if (!drag_caption_pill || !drag_caption_label) {
		return;
	}
	if (dx > -22 && dx < 22) {
		lv_obj_set_hidden(drag_caption_pill, true);
		return;
	}

	int dir = (dx < 0) ? 1 : -1;
	char title[256] = "";
	char artist[256] = "";
	if (!upnext_get_adjacent(dir, NULL, 0, title, sizeof(title), artist, sizeof(artist)) || !title[0]) {
		lv_obj_set_hidden(drag_caption_pill, true);
		return;
	}

	char line[384];
	const char *prefix = (dir > 0) ? tr("up_next") : tr("previous");
	if (artist[0]) {
		snprintf(line, sizeof(line), "%s · %s — %s", prefix, title, artist);
	} else {
		snprintf(line, sizeof(line), "%s · %s", prefix, title);
	}
	set_label_text_if_changed(drag_caption_label, line);

	int abs_dx = dx < 0 ? (int)(-dx) : (int)dx;
	int opa = 120 + (abs_dx - 22) * 2;
	if (opa > 240) {
		opa = 240;
	}
	lv_obj_set_style_opa(drag_caption_pill, (lv_opa_t)opa, 0);
	lv_obj_set_hidden(drag_caption_pill, false);
	lv_obj_move_foreground(drag_caption_pill);
}

static void carousel_offset_exec_cb(void *var, int32_t v) {
	(void)var;
	carousel_offset_px = v;
	carousel_layout_slots();
	if (v == 0 && drag_caption_pill) {
		lv_obj_set_hidden(drag_caption_pill, true);
	}
}

static void carousel_animate_to_rest(void) {
	lv_anim_delete(&carousel_anim_tag, carousel_offset_exec_cb);
	if (drag_caption_pill) {
		lv_obj_set_hidden(drag_caption_pill, true);
	}
	if (carousel_offset_px == 0 || !motion_enabled()) {
		carousel_offset_px = 0;
		carousel_layout_slots();
		return;
	}
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, &carousel_anim_tag);
	lv_anim_set_exec_cb(&a, carousel_offset_exec_cb);
	lv_anim_set_values(&a, carousel_offset_px, 0);
	lv_anim_set_duration(&a, CAROUSEL_SLIDE_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_start(&a);
}

static void carousel_animate_from(int32_t start_offset) {
	if (!motion_enabled() || lyrics_state != LYRICS_VIEW_NONE) {
		carousel_offset_px = 0;
		carousel_layout_slots();
		return;
	}
	carousel_offset_px = start_offset;
	carousel_layout_slots();
	carousel_animate_to_rest();
}

static void carousel_sync_adjacent_Slot(int slot_idx, int coverloader_slot, bool *pending_flag, int dir,
										bool *has_dir_out) {
	carousel_slot_t *s = &cslots[slot_idx];
	char want_path[512] = "";
	bool exists = power_screen_is_on() && !live_mode &&
				  upnext_get_adjacent(dir, want_path, sizeof(want_path), NULL, 0, NULL, 0) && want_path[0];

	*has_dir_out = exists;
	if (!exists) {
		s->path[0] = '\0';
		*pending_flag = false;
		coverloader_release(coverloader_slot);
		slot_clear_cover(s);
		slot_apply_geometry(s, peek_box);
		return;
	}

	if (strcmp(s->path, want_path) == 0 && (s->has_cover || *pending_flag)) {
		return;
	}

	snprintf(s->path, sizeof(s->path), "%s", want_path);
	slot_clear_cover(s);

	// Fast synchronous reuse if identical to hero cover already settled in RAM.
	if (cslots[1].has_cover && cslots[1].cover.pixels && !cover_request_outstanding &&
		cover_id_state == COVER_ID_IDLE && strcmp(cover_shown_path, want_path) == 0) {
		cover_image_t scaled;
		if (cover_scaled_copy(&cslots[1].cover, peek_box, &scaled)) {
			s->cover = scaled;
			s->cover.dsc.data = s->cover.pixels;
			s->has_cover = true;
			*pending_flag = false;
			coverloader_release(coverloader_slot);
			slot_apply_geometry(s, peek_box);
			return;
		}
	}

	slot_apply_geometry(s, peek_box);
	*pending_flag = true;
	coverloader_request_carousel(coverloader_slot, want_path, peek_box, hero_box, hero_box);
	cover_poll_start();
}

static void carousel_sync_adjacent_requests(void) {
	carousel_sync_adjacent_Slot(0, COVERLOADER_CAROUSEL_PREV, &prev_cover_pending, -1, &carousel_has_prev);
	carousel_sync_adjacent_Slot(2, COVERLOADER_CAROUSEL_NEXT, &next_cover_pending, 1, &carousel_has_next);
	carousel_layout_slots();
}

static void carousel_poll_adjacent(void) {
	bool updated = false;
	if (prev_cover_pending) {
		cover_image_t img = {0};
		bool finished = false;
		bool ok = coverloader_take(COVERLOADER_CAROUSEL_PREV, &img, &finished);
		if (finished) {
			prev_cover_pending = false;
			slot_clear_cover(&cslots[0]);
			if (ok) {
				cslots[0].cover = img;
				cslots[0].cover.dsc.data = cslots[0].cover.pixels;
				cslots[0].has_cover = true;
			}
			slot_apply_geometry(&cslots[0], peek_box);
			updated = true;
		}
	}
	if (next_cover_pending) {
		cover_image_t img = {0};
		bool finished = false;
		bool ok = coverloader_take(COVERLOADER_CAROUSEL_NEXT, &img, &finished);
		if (finished) {
			next_cover_pending = false;
			slot_clear_cover(&cslots[2]);
			if (ok) {
				cslots[2].cover = img;
				cslots[2].cover.dsc.data = cslots[2].cover.pixels;
				cslots[2].has_cover = true;
			}
			slot_apply_geometry(&cslots[2], peek_box);
			updated = true;
		}
	}
	if (updated) {
		carousel_layout_slots();
	}
}

// Touch gesture on carousel_stage: horizontal track skip with live translation,
// Up Next reveal, rubber-band bounds, and strict single-track commit.
static void carousel_drag_cb(lv_event_t *e) {
	static lv_point_t start_pt;
	static lv_point_t last_pt;
	static uint32_t last_tick;
	static int32_t vel_q8; // EWMA dx velocity in (px * 256) / ms
	static bool tracking;
	static bool engaged;

	lv_event_code_t code = lv_event_get_code(e);
	lv_indev_t *indev = lv_indev_active();
	if (!indev || lyrics_state != LYRICS_VIEW_NONE) {
		return;
	}

	if (code == LV_EVENT_PRESSED) {
		lv_indev_get_point(indev, &start_pt);
		last_pt = start_pt;
		last_tick = lv_tick_get();
		vel_q8 = 0;
		tracking = true;
		engaged = false;
		return;
	}
	if (!tracking) {
		return;
	}

	lv_point_t p;
	lv_indev_get_point(indev, &p);
	int dx = p.x - start_pt.x;
	int dy = p.y - start_pt.y;

	if (code == LV_EVENT_PRESSING) {
		uint32_t now = lv_tick_get();
		uint32_t dt = now - last_tick;
		if (dt > 0) {
			int32_t sample = ((int32_t)(p.x - last_pt.x) * 256) / (int32_t)dt;
			vel_q8 = (vel_q8 + sample) / 2;
			last_pt = p;
			last_tick = now;
		}

		if (!engaged) {
			int adx = dx < 0 ? -dx : dx;
			int ady = dy < 0 ? -dy : dy;
			if (ady >= CAROUSEL_DRAG_PX && ady > adx) {
				tracking = false;
				return;
			}
			if (adx < CAROUSEL_DRAG_PX || adx <= ady) {
				return;
			}
			engaged = true;
			lv_anim_delete(&carousel_anim_tag, carousel_offset_exec_cb);
		}

		// Determine if this swipe direction can commit a track step
		bool can_step = live_mode ? live_custom_nav : device_state_can_skip_track();

		int32_t eff_dx = dx;
		if (!can_step) {
			eff_dx = dx / 4; // tactile rubber-band resistance at boundary
		}
		int stride = carousel_stride();
		if (eff_dx > stride) eff_dx = stride;
		if (eff_dx < -stride) eff_dx = -stride;

		carousel_offset_px = eff_dx;
		carousel_layout_slots();
		carousel_update_drag_caption(dx);
		return;
	}

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		tracking = false;
		if (!engaged) {
			return;
		}
		engaged = false;
		if (drag_caption_pill) {
			lv_obj_set_hidden(drag_caption_pill, true);
		}

		if (lv_tick_elaps(last_tick) > 80) {
			vel_q8 = 0;
		}

		bool can_step = live_mode ? live_custom_nav : device_state_can_skip_track();

		int threshold = screen_w / 5; // 96 px on R1
		bool flick_next = (dx <= -36 && vel_q8 < -115); // ~0.45 px/ms
		bool flick_prev = (dx >= 36 && vel_q8 > 115);

		if (dx < 0 && can_step && (-dx >= threshold || flick_next)) {
			int32_t carry = carousel_stride() + carousel_offset_px;
			if (carry < 0) carry = 0;
			if (live_mode && live_custom_nav) {
				if (radio_step(1)) {
					refresh_now_playing();
					carousel_animate_from(carry);
					return;
				}
			} else if (device_state_next(NULL, 0)) {
				refresh_now_playing();
				carousel_animate_from(carry);
				return;
			}
		} else if (dx > 0 && can_step && (dx >= threshold || flick_prev)) {
			int32_t carry = -carousel_stride() + carousel_offset_px;
			if (carry > 0) carry = 0;
			if (live_mode && live_custom_nav) {
				if (radio_step(-1)) {
					refresh_now_playing();
					carousel_animate_from(carry);
					return;
				}
			} else if (device_state_prev(NULL, 0)) {
				refresh_now_playing();
				carousel_animate_from(carry);
				return;
			}
		}

		carousel_animate_to_rest();
	}
}

// ---------------------------------------------------------------------------
// Hero Artwork Loader & Same-Album Deduplication
// ---------------------------------------------------------------------------

void player_set_cover_release_cb(void (*cb)(void)) {
	cover_release_cb = cb;
}

void player_cover_box(int *w, int *h) {
	if (w) *w = hero_box;
	if (h) *h = hero_box;
}

const lv_image_dsc_t *player_cover_image(void) {
	return (cslots[1].has_cover && cslots[1].cover.pixels) ? &cslots[1].cover.dsc : NULL;
}

// Ambient Edition avoids keeping a 750 KiB full-screen runtime blur buffer in
// memory; screensaver.c falls back cleanly to its built-in dark strip.
const lv_image_dsc_t *player_backdrop_image(void) {
	return NULL;
}

bool player_cover_copy(int size, cover_image_t *out, uint64_t *id) {
	if (id) *id = 0;
	if (!cslots[1].has_cover || !cslots[1].cover.pixels || cover_request_outstanding ||
		cover_id_state != COVER_ID_IDLE) {
		return false;
	}
	if (!cover_scaled_copy(&cslots[1].cover, size, out)) {
		return false;
	}
	if (id) *id = cover_shown_id;
	return true;
}

static void update_fs_thumb(void) {
	if (!fs_thumb_img || !fs_thumb_fallback) {
		return;
	}
	if (fs_has_thumb) {
		lv_image_set_src(fs_thumb_img, NULL);
		cover_free(&fs_thumb_cover);
		fs_has_thumb = false;
	}
	if (cslots[1].has_cover && cslots[1].cover.pixels && cover_scaled_copy(&cslots[1].cover, 44, &fs_thumb_cover)) {
		fs_thumb_cover.dsc.data = fs_thumb_cover.pixels;
		fs_has_thumb = true;
		lv_image_set_src(fs_thumb_img, &fs_thumb_cover.dsc);
		lv_obj_set_hidden(fs_thumb_img, false);
		lv_obj_set_hidden(fs_thumb_fallback, true);
	} else {
		lv_image_set_src(fs_thumb_img, NULL);
		lv_obj_set_hidden(fs_thumb_img, true);
		lv_obj_set_hidden(fs_thumb_fallback, false);
	}
}

static void reload_hero_cover(const char *filepath) {
	if (!filepath || !filepath[0]) {
		if (cover_release_cb) {
			cover_release_cb();
		}
		slot_clear_cover(&cslots[1]);
		slot_apply_geometry(&cslots[1], hero_box);
		carousel_layout_slots();

		current_has_cover = false;
		current_raw_tone = 0;
		current_mean_luma = -1;
		cover_shown_id = 0;
		cover_request_outstanding = false;

		update_fs_thumb();
		recompute_palette(true);
		bar_over_player_update();
		return;
	}

	// Request ONLY the hero cover (0 x 0 backdrop skips the 750 KiB blur buffer).
	coverloader_request_player(filepath, hero_box, hero_box, 0, 0);
	cover_request_outstanding = true;
	lv_timer_resume(progress_slider_timer);
	cover_poll_start();
}

static void refresh_cover(const char *filepath) {
	if (!cslots[1].frame) {
		return;
	}
	snprintf(cover_shown_path, sizeof(cover_shown_path), "%s", filepath ? filepath : "");

	if (cover_shown_id != 0 && cslots[1].has_cover && cslots[1].cover.pixels && filepath && filepath[0]) {
		cover_id_state = COVER_ID_NEW_TRACK;
		coverloader_request_player_id(filepath);
		lv_timer_resume(progress_slider_timer);
		cover_poll_start();
		return;
	}

	cover_id_state = COVER_ID_IDLE;
	cover_incoming_id = 0;
	reload_hero_cover(filepath);
}

static void apply_cover_id_result(void) {
	if (cover_id_state == COVER_ID_IDLE) {
		return;
	}
	uint64_t id = 0;
	bool finished = false;
	coverloader_take_player_id(&id, &finished);
	if (!finished) {
		return;
	}

	if (cover_id_state == COVER_ID_MEASURE) {
		cover_id_state = COVER_ID_IDLE;
		cover_shown_id = id;
		carousel_sync_adjacent_requests();
		return;
	}

	cover_id_state = COVER_ID_IDLE;
	if (id != 0 && id == cover_shown_id) {
		// Identical artwork across tracks of the same album: zero decode, zero blink.
		carousel_sync_adjacent_requests();
		return;
	}

	cover_incoming_id = id;
	reload_hero_cover(cover_shown_path);
}

static void apply_cover_result(void) {
	if (!cover_request_outstanding) {
		return;
	}

	cover_image_t new_cover = {0}, unused_backdrop = {0};
	bool finished = false;
	bool has_cover = coverloader_take_player(&new_cover, &unused_backdrop, &finished);
	if (!finished) {
		return;
	}
	cover_request_outstanding = false;
	cover_free(&unused_backdrop);

	if (cover_release_cb) {
		cover_release_cb();
	}
	slot_clear_cover(&cslots[1]);

	if (has_cover && new_cover.pixels) {
		cslots[1].cover = new_cover;
		cslots[1].cover.dsc.data = cslots[1].cover.pixels;
		cslots[1].has_cover = true;

		cover_shown_id = cover_incoming_id;
		if (cover_shown_id == 0 && cover_shown_path[0]) {
			cover_id_state = COVER_ID_MEASURE;
			coverloader_request_player_id(cover_shown_path);
			cover_poll_start();
		}

		uint32_t tone = 0;
		int luma = -1;
		bool light = false;
		// Extract the dominant luminous chromatic family (light_from = 0) so dark
		// sleeves with vivid focal art yield their focal accent hue rather than
		// their near-black background shadow.
		if (!cover_page_tone(&cslots[1].cover, 0, &tone, &luma, &light)) {
			if (cslots[1].cover.has_tone) {
				tone = cslots[1].cover.tone;
				luma = cslots[1].cover.tone_luma;
			} else {
				tone = 0;
				luma = cover_mean_luma(&cslots[1].cover);
			}
		}
		current_has_cover = true;
		current_raw_tone = tone;
		current_mean_luma = luma;
	} else {
		cover_free(&new_cover);
		cover_shown_id = 0;
		current_has_cover = false;
		current_raw_tone = 0;
		current_mean_luma = -1;
	}

	cover_incoming_id = 0;
	slot_apply_geometry(&cslots[1], hero_box);
	carousel_layout_slots();
	update_fs_thumb();
	recompute_palette(true);
	bar_over_player_update();
	carousel_sync_adjacent_requests();
	notify_state_cbs();
}

static void cover_poll_cb(lv_timer_t *timer) {
	apply_cover_id_result();
	apply_cover_result();
	carousel_poll_adjacent();
	if (!cover_request_outstanding && cover_id_state == COVER_ID_IDLE && !prev_cover_pending && !next_cover_pending) {
		lv_timer_pause(timer);
	}
}

static void cover_poll_start(void) {
	if (!cover_poll_timer) {
		cover_poll_timer = lv_timer_create(cover_poll_cb, COVER_POLL_MS, NULL);
	}
	lv_timer_resume(cover_poll_timer);
}

// ---------------------------------------------------------------------------
// 72-Bar Minimalist Audio Waveform & Seek Bar
// ---------------------------------------------------------------------------

static void view_update(double position, double total) {
	view_start = 0;
	view_length = total > 0 ? total : 0;
	if (view_length <= 0 || !audiobook_mode || !audiobook_has_chapters || !audiobook_duration_per_chapter()) {
		return;
	}
	int index = audiobook_chapter_at(position);
	double start = 0;
	if (index < 0 || !audiobook_chapter(index, NULL, 0, &start)) {
		return;
	}
	double end = total;
	if (!audiobook_chapter(index + 1, NULL, 0, &end) || end > total) {
		end = total;
	}
	if (start < 0 || end - start < 1.0) {
		return;
	}
	view_start = start;
	view_length = end - start;
}

static int view_value(double position) {
	if (view_length <= 0) {
		return 0;
	}
	double fraction = (position - view_start) / view_length;
	if (fraction < 0) fraction = 0;
	if (fraction > 1) fraction = 1;
	return (int)(fraction * 1000.0 + 0.5);
}

static double view_seconds(int value) {
	return view_start + view_length * (double)value / 1000.0;
}

static int wave_played_px(double position) {
	double fraction = view_length > 0.1 ? (double)view_value(position) / 1000.0 : 0.0;
	int total_span = WAVEFORM_BARS * WAVE_BAR_PITCH - WAVE_BAR_GAP;
	int left = (wave_w - total_span) / 2;
	return left + (int)(fraction * (double)total_span + 0.5);
}

static uint32_t current_wave_color_sig(void) {
	return ((uint32_t)pal_cur.accent.red << 24) | ((uint32_t)pal_cur.accent.green << 16) |
		   ((uint32_t)pal_cur.accent.blue << 8) | (uint32_t)pal_cur.wave_unplayed.blue | 1u;
}

static void sync_wave_or_slider_mode(bool show_waveform) {
	if (!progress_slider || !wave_box) {
		return;
	}
	bool seekable = !live_mode && view_length > 0.1;
	if (seekable) {
		lv_obj_remove_state(progress_slider, LV_STATE_DISABLED);
		if (fs_slider) lv_obj_remove_state(fs_slider, LV_STATE_DISABLED);
	} else {
		lv_obj_add_state(progress_slider, LV_STATE_DISABLED);
		if (fs_slider) lv_obj_add_state(fs_slider, LV_STATE_DISABLED);
	}

	bool use_wave = show_waveform && wave_canvas != NULL && wave_ready > 0 && !live_mode;
	if (wave_canvas) {
		lv_obj_set_hidden(wave_canvas, !use_wave);
	}

	if (use_wave) {
		lv_obj_set_size(progress_slider, lv_pct(100), wave_h);
		lv_obj_align(progress_slider, LV_ALIGN_CENTER, 0, 0);
		lv_obj_set_style_bg_opa(progress_slider, LV_OPA_TRANSP, LV_PART_MAIN);
		lv_obj_set_style_bg_opa(progress_slider, LV_OPA_TRANSP, LV_PART_INDICATOR);
		lv_obj_set_style_opa(progress_slider, LV_OPA_TRANSP, LV_PART_KNOB);
	} else {
		lv_obj_set_size(progress_slider, lv_pct(100), 4);
		lv_obj_align(progress_slider, LV_ALIGN_CENTER, 0, 0);
		lv_obj_set_style_bg_opa(progress_slider, LV_OPA_COVER, LV_PART_MAIN);
		lv_obj_set_style_bg_opa(progress_slider, LV_OPA_COVER, LV_PART_INDICATOR);
		lv_obj_set_style_opa(progress_slider, seekable ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_KNOB);
	}
	lv_obj_move_foreground(progress_slider);
}

static void wave_paint(int played_px) {
	if (!wave_buf || !wave_canvas || wave_w <= 0) {
		return;
	}

	int w = wave_w;
	int h = wave_h;
	uint16_t *colour = (uint16_t *)wave_buf;
	uint8_t *alpha = wave_buf + (size_t)w * (size_t)h * 2;
	memset(alpha, 0, (size_t)w * (size_t)h);

	lv_color_t c_elap = pal_cur.accent;
	lv_color_t c_rem = pal_cur.wave_unplayed;
	uint16_t px_elap = (uint16_t)(((c_elap.red & 0xF8) << 8) | ((c_elap.green & 0xFC) << 3) | (c_elap.blue >> 3));
	uint16_t px_rem = (uint16_t)(((c_rem.red & 0xF8) << 8) | ((c_rem.green & 0xFC) << 3) | (c_rem.blue >> 3));

	int total_span = WAVEFORM_BARS * WAVE_BAR_PITCH - WAVE_BAR_GAP;
	int left = (w - total_span) / 2;
	int middle = h / 2;
	int reach = h / 2 - 1;
	if (reach < WAVE_MIN_HALF) {
		reach = WAVE_MIN_HALF;
	}

	for (int i = 0; i < WAVEFORM_BARS; i++) {
		int val = (i < wave_ready) ? wave_bars[i] : 0;
		int half = (val * reach) / 235;
		if (half > reach) half = reach;
		if (half < WAVE_MIN_HALF) half = WAVE_MIN_HALF;

		int x0 = left + i * WAVE_BAR_PITCH;
		for (int dx = 0; dx < WAVE_BAR_SOLID; dx++) {
			int x = x0 + dx;
			if (x < 0 || x >= w) {
				continue;
			}
			bool elapsed = (x < played_px);
			uint16_t packed = elapsed ? px_elap : px_rem;
			uint8_t opa = elapsed ? 255 : WAVE_TODO_OPA;

			// Rounded 4px capsule ends: outer columns (dx==0 or dx==3) inset 1px vertically.
			int cap = (dx == 0 || dx == WAVE_BAR_SOLID - 1) ? 1 : 0;
			int y0 = middle - half + cap;
			int y1 = middle + half - cap;
			if (y0 > y1) {
				y0 = y1 = middle;
			}
			for (int y = y0; y <= y1; y++) {
				if (y >= 0 && y < h) {
					size_t at = (size_t)y * (size_t)w + (size_t)x;
					colour[at] = packed;
					alpha[at] = opa;
				}
			}
		}
	}

	wave_drawn_px = played_px;
	wave_drawn_sig = current_wave_color_sig();
	lv_obj_invalidate(wave_canvas);
}

static void wave_preview(double seconds) {
	if (!wave_canvas || lv_obj_is_hidden(wave_canvas) || view_length <= 0.1) {
		return;
	}
	int px = wave_played_px(seconds);
	if (px == wave_drawn_px && wave_drawn_sig == current_wave_color_sig()) {
		return;
	}
	wave_paint(px);
}

static void wave_refresh(const char *path, double position) {
	device_state_t state;
	device_state_get(&state);
	bool local = playing_local_file(&state);

	if (!local || !path || !path[0] || !wave_canvas) {
		wave_ready = 0;
		sync_wave_or_slider_mode(false);
		return;
	}

	int had = wave_ready;
	uint8_t before[WAVEFORM_BARS];
	memcpy(before, wave_bars, sizeof(before));
	wave_ready = waveform_get(path, wave_bars);

	sync_wave_or_slider_mode(wave_ready > 0);
	if (wave_ready <= 0) {
		return;
	}

	bool same_shape = (wave_ready == had && memcmp(before, wave_bars, (size_t)wave_ready) == 0);
	int px = wave_played_px(position);
	uint32_t sig = current_wave_color_sig();
	if (same_shape && px == wave_drawn_px && sig == wave_drawn_sig) {
		return;
	}
	wave_paint(px);
}

static void set_progress_label(double current_secs, double total_secs) {
	view_update(current_secs, total_secs);
	double shown = 0;
	int val = 0;
	if (view_length > 0) {
		val = view_value(current_secs);
		if (!slider_dragging) {
			if (progress_slider) lv_slider_set_value(progress_slider, val, LV_ANIM_OFF);
			if (fs_slider) lv_slider_set_value(fs_slider, val, LV_ANIM_OFF);
		}
		shown = current_secs - view_start;
		if (shown < 0) shown = 0;
	} else if (!slider_dragging) {
		if (progress_slider) lv_slider_set_value(progress_slider, 0, LV_ANIM_OFF);
		if (fs_slider) lv_slider_set_value(fs_slider, 0, LV_ANIM_OFF);
	}

	if (!slider_dragging) {
		formatDoubleSeconds(shown, progress_label_text, sizeof(progress_label_text));
		set_label_text_if_changed(elapsed_label, progress_label_text);
		set_label_text_if_changed(fs_elapsed_label, progress_label_text);
	}

	formatDoubleSeconds(view_length, progress_label_text, sizeof(progress_label_text));
	set_label_text_if_changed(remaining_label, progress_label_text);
	set_label_text_if_changed(fs_duration_label, progress_label_text);
}

static void seek_slider_event_cb(lv_event_t *e) {
	lv_obj_t *active_slider = lv_event_get_current_target(e);
	lv_event_code_t code = lv_event_get_code(e);

	if (live_mode || view_length <= 0.1) {
		return;
	}

	if (code == LV_EVENT_PRESSED) {
		slider_dragging = true;
		lv_timer_pause(progress_slider_timer);
		smooth_timer_sync();
		return;
	}

	if (code == LV_EVENT_VALUE_CHANGED) {
		int value = (int)lv_slider_get_value(active_slider);
		if (active_slider == progress_slider && fs_slider) {
			lv_slider_set_value(fs_slider, value, LV_ANIM_OFF);
		} else if (active_slider == fs_slider && progress_slider) {
			lv_slider_set_value(progress_slider, value, LV_ANIM_OFF);
		}
		double seconds = view_seconds(value);
		double shown = seconds - view_start;
		if (shown < 0) shown = 0;
		formatDoubleSeconds(shown, progress_label_text, sizeof(progress_label_text));
		set_label_text_if_changed(elapsed_label, progress_label_text);
		set_label_text_if_changed(fs_elapsed_label, progress_label_text);
		wave_preview(seconds);
		return;
	}

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		if (!slider_dragging) {
			return;
		}
		slider_dragging = false;
		int value = (int)lv_slider_get_value(active_slider);
		double seconds = view_seconds(value);
		device_state_seek(seconds);
		progress_anchor_secs = seconds;
		progress_anchor_tick = lv_tick_get();
		lyrics_centred = false;
		lyrics_follow(seconds);
		lv_timer_resume(progress_slider_timer);
		smooth_timer_sync();
	}
}

// ---------------------------------------------------------------------------
// Authoritative Audio Metadata Line ("FLAC · 24-bit · 96 kHz")
// ---------------------------------------------------------------------------

static void format_description(const device_state_t *state, char *text, size_t size) {
	text[0] = '\0';
	if (state->live) {
		radio_now_t now;
		radio_get_now(&now);
		if (now.bits > 0 && now.sample_rate > 0 && now.codec[0]) {
			snprintf(text, size, "%s · %d-bit · %g kHz", now.codec, now.bits, now.sample_rate / 1000.0);
		} else if (now.bitrate > 0) {
			snprintf(text, size, "%s · %d kbps", now.codec[0] ? now.codec : "MP3", now.bitrate);
		} else if (now.codec[0]) {
			snprintf(text, size, "%s", now.codec);
		}
		return;
	}

	if (state->stream_sample_rate <= 0 || !state->current_file[0]) {
		return;
	}
	if (podcastcache_is_episode(state->current_file)) {
		return;
	}

	const char *dot = strrchr(state->current_file, '.');
	char kind[8] = "";
	if (dot) {
		size_t k = 0;
		for (const char *c = dot + 1; *c && k < sizeof(kind) - 1; c++) {
			kind[k++] = (char)((*c >= 'a' && *c <= 'z') ? *c - 32 : *c);
		}
		kind[k] = '\0';
	}

	int dsd = audio_get_dsd_multiple();
	if (dsd > 0) {
		snprintf(text, size, "DSD%d", dsd);
		return;
	}

	char codec[16] = "";
	audio_get_stream_codec(codec, sizeof(codec));
	const char *name = codec[0] ? codec : kind;

	if (audio_stream_is_lossy()) {
		int kbps = audio_get_stream_bitrate_kbps();
		struct stat st;
		if (kbps <= 0 && !qobuzcache_owns(state->current_file) && !tidalcache_owns(state->current_file) &&
			state->progress_total_secs > 0.5 && stat(state->current_file, &st) == 0 && st.st_size > 0) {
			kbps = (int)((double)st.st_size * 8.0 / state->progress_total_secs / 1000.0 + 0.5);
		}
		if (kbps > 0 && name[0]) {
			snprintf(text, size, "%s · %d kbps", name, kbps);
		} else if (kbps > 0) {
			snprintf(text, size, "%d kbps", kbps);
		} else {
			snprintf(text, size, "%s", name);
		}
		return;
	}

	int bits = audio_get_stream_bits();
	if (bits > 0 && name[0]) {
		snprintf(text, size, "%s · %d-bit · %g kHz", name, bits, state->stream_sample_rate / 1000.0);
	} else if (name[0]) {
		snprintf(text, size, "%s · %g kHz", name, state->stream_sample_rate / 1000.0);
	} else {
		snprintf(text, size, "%g kHz", state->stream_sample_rate / 1000.0);
	}
}

static void update_format_label(const device_state_t *state) {
	if (!format_label) {
		return;
	}
	char text[80];
	format_description(state, text, sizeof(text));
	set_label_text_if_changed(format_label, text);
	lv_obj_set_hidden(format_label, text[0] == '\0');
}

// ---------------------------------------------------------------------------
// Favorites, Badges, Speed & 5-Button Transport Controls
// ---------------------------------------------------------------------------

static long current_qobuz_track(void) {
	device_state_t state;
	device_state_get(&state);
	return state.current_file[0] ? qobuzcache_track_id(state.current_file) : 0;
}

static long current_tidal_track(void) {
	device_state_t state;
	device_state_get(&state);
	return state.current_file[0] ? tidalcache_track_id(state.current_file) : 0;
}

static bool podcast_current_feed(long long *id_out, podcast_feed_t *feed_out) {
	device_state_t state;
	device_state_get(&state);
	if (state.live || !state.current_file[0]) {
		return false;
	}
	long long id = 0;
	if (!podcastcache_feed_id(state.current_file, &id) || id <= 0) {
		return false;
	}
	if (id_out) *id_out = id;
	if (feed_out) {
		memset(feed_out, 0, sizeof(*feed_out));
		feed_out->id = id;
		snprintf(feed_out->title, sizeof(feed_out->title), "%.*s", (int)sizeof(feed_out->title) - 1,
				 state.metadata.album);
		podcastcache_tag(state.current_file, "feed_author", feed_out->author, sizeof(feed_out->author));
		podcastcache_tag(state.current_file, "feed_image", feed_out->image, sizeof(feed_out->image));
	}
	return true;
}

bool player_current_podcast_feed(long long *id_out, podcast_feed_t *feed_out) {
	return podcast_current_feed(id_out, feed_out);
}

static bool current_podcast_episode(void) {
	device_state_t state;
	device_state_get(&state);
	return state.current_file[0] && podcastcache_is_episode(state.current_file);
}

static void update_service_badges(void) {
	bool from_qobuz = !live_mode && current_qobuz_track() != 0;
	bool from_tidal = !live_mode && !from_qobuz && current_tidal_track() != 0;
	bool from_podcast = !live_mode && !from_qobuz && !from_tidal && current_podcast_episode();

	if (live_badge) lv_obj_set_hidden(live_badge, !live_mode);
	if (qobuz_badge) lv_obj_set_hidden(qobuz_badge, !from_qobuz);
	if (tidal_badge) lv_obj_set_hidden(tidal_badge, !from_tidal);
	if (podcast_badge) lv_obj_set_hidden(podcast_badge, !from_podcast);
	carousel_layout_slots();
}

static void fav_refresh_async(void *user) {
	(void)user;
	update_fav_button();
}

static void qobuz_favorites_changed(void) { gui_post(fav_refresh_async, NULL); }
static void tidal_favorites_changed(void) { gui_post(fav_refresh_async, NULL); }

static void update_fav_button(void) {
	if (!fav_btn_icon) {
		return;
	}
	radio_station_t station;
	bool starred = false;
	long qobuz_id = 0;
	long tidal_id = 0;
	long long feed_id = 0;

	if (radio_current_station(&station)) {
		starred = radio_fav_contains(station.uuid);
	} else if (podcast_current_feed(&feed_id, NULL)) {
		starred = podcastsubs_is_followed(feed_id);
	} else if ((qobuz_id = current_qobuz_track()) > 0) {
		starred = qobuzsync_is_favorite(qobuz_id);
		qobuzsync_favorites_refresh(qobuz_favorites_changed);
	} else if ((tidal_id = current_tidal_track()) > 0) {
		starred = tidalsync_is_favorite(tidal_id);
		tidalsync_favorites_refresh(tidal_favorites_changed);
	} else {
		device_state_t state;
		device_state_get(&state);
		starred = state.current_file[0] && library_fav_contains(state.current_file);
	}

	if (starred) {
		lv_image_set_src(fav_btn_icon, &icon_star_filled);
		lv_obj_set_style_image_recolor(fav_btn_icon, pal_cur.accent, 0);
		lv_obj_set_style_image_opa(fav_btn_icon, LV_OPA_COVER, 0);
	} else {
		lv_image_set_src(fav_btn_icon, &icon_star);
		lv_obj_set_style_image_recolor(fav_btn_icon, pal_cur.text_secondary, 0);
		lv_obj_set_style_image_opa(fav_btn_icon, LV_OPA_80, 0);
	}
	lv_obj_set_style_image_recolor_opa(fav_btn_icon, LV_OPA_COVER, 0);
}

static char fav_error[192];

static void fav_error_async(void *user) {
	(void)user;
	if (fav_error[0]) {
		gui_notify_popup(fav_error);
	}
	update_fav_button();
}

static void qobuz_fav_done(bool ok, const char *error, void *user) {
	(void)user;
	snprintf(fav_error, sizeof(fav_error), "%s", ok ? "" : (error && error[0] ? error : tr("qobuz_favourites_failed")));
	gui_post(fav_error_async, NULL);
}

static void tidal_fav_done(bool ok, const char *error, void *user) {
	(void)user;
	snprintf(fav_error, sizeof(fav_error), "%s", ok ? "" : (error && error[0] ? error : tr("tidal_favourites_failed")));
	gui_post(fav_error_async, NULL);
}

static void fav_btn_event_cb(lv_event_t *e) {
	(void)e;
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}

	radio_station_t station;
	if (radio_current_station(&station)) {
		if (radio_fav_contains(station.uuid)) {
			radio_fav_remove(station.uuid);
		} else {
			radio_fav_add(&station);
		}
		update_fav_button();
		return;
	}

	long long feed_id = 0;
	podcast_feed_t feed;
	if (podcast_current_feed(&feed_id, &feed)) {
		if (podcastsubs_is_followed(feed_id)) {
			podcastsubs_unfollow(feed_id);
		} else {
			podcastsubs_follow(&feed);
		}
		update_fav_button();
		return;
	}

	device_state_t state;
	device_state_get(&state);
	if (!state.current_file[0]) {
		return;
	}

	long qobuz_id = qobuzcache_track_id(state.current_file);
	if (qobuz_id > 0) {
		bool want = !qobuzsync_is_favorite(qobuz_id);
		qobuzsync_favorite_toggle(qobuz_id, want, qobuz_fav_done, NULL);
		update_fav_button();
		return;
	}

	long tidal_id = tidalcache_track_id(state.current_file);
	if (tidal_id > 0) {
		bool want = !tidalsync_is_favorite(tidal_id);
		tidalsync_favorite_toggle(tidal_id, want, tidal_fav_done, NULL);
		update_fav_button();
		return;
	}

	const char *slash = strrchr(state.current_file, '/');
	const char *title = state.metadata.title[0] ? state.metadata.title : (slash ? slash + 1 : state.current_file);
	library_fav_toggle(state.current_file, title, state.metadata.artist);
	update_fav_button();
}

static void update_transport_modes(void) {
	playback_mode_t mode = playlist_get_mode();
	if (mode == PLAYBACK_MODE_SHUFFLE || mode == PLAYBACK_MODE_SHUFFLE_REPEAT) {
		shuffle_remembered = true;
	} else if (mode != PLAYBACK_MODE_REPEAT_ONE) {
		shuffle_remembered = false;
	}

	bool shuf_on = (mode == PLAYBACK_MODE_SHUFFLE || mode == PLAYBACK_MODE_SHUFFLE_REPEAT ||
					(mode == PLAYBACK_MODE_REPEAT_ONE && shuffle_remembered));
	bool rep_all = (mode == PLAYBACK_MODE_REPEAT_ALL || mode == PLAYBACK_MODE_SHUFFLE_REPEAT);
	bool rep_one = (mode == PLAYBACK_MODE_REPEAT_ONE);

	if (shuffle_btn_icon) {
		lv_obj_set_style_image_recolor(shuffle_btn_icon, shuf_on ? pal_cur.accent : pal_cur.text_secondary, 0);
		lv_obj_set_style_image_recolor_opa(shuffle_btn_icon, LV_OPA_COVER, 0);
		lv_obj_set_style_image_opa(shuffle_btn_icon, shuf_on ? LV_OPA_COVER : LV_OPA_60, 0);
	}
	if (shuffle_dot) {
		lv_obj_set_style_bg_color(shuffle_dot, pal_cur.accent, 0);
		lv_obj_set_hidden(shuffle_dot, !shuf_on);
	}

	if (repeat_btn_icon) {
		const lv_image_dsc_t *ricon = rep_one ? &icon_repeat_one : &icon_repeat_all;
		if (lv_image_get_src(repeat_btn_icon) != ricon) {
			lv_image_set_src(repeat_btn_icon, ricon);
		}
		bool rep_active = rep_all || rep_one;
		lv_obj_set_style_image_recolor(repeat_btn_icon, rep_active ? pal_cur.accent : pal_cur.text_secondary, 0);
		lv_obj_set_style_image_recolor_opa(repeat_btn_icon, LV_OPA_COVER, 0);
		lv_obj_set_style_image_opa(repeat_btn_icon, rep_active ? LV_OPA_COVER : LV_OPA_60, 0);
	}
	if (repeat_dot) {
		lv_obj_set_style_bg_color(repeat_dot, pal_cur.accent, 0);
		lv_obj_set_hidden(repeat_dot, !(rep_all || rep_one));
	}
}

static void shuffle_btn_event_cb(lv_event_t *e) {
	(void)e;
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	playback_mode_t mode = playlist_get_mode();
	bool endless = config_get_bool("player", "endless_shuffle", false);
	if (mode == PLAYBACK_MODE_SHUFFLE || mode == PLAYBACK_MODE_SHUFFLE_REPEAT) {
		shuffle_remembered = false;
		playlist_set_mode(mode == PLAYBACK_MODE_SHUFFLE_REPEAT ? PLAYBACK_MODE_REPEAT_ALL : PLAYBACK_MODE_NORMAL);
	} else if (mode == PLAYBACK_MODE_REPEAT_ONE) {
		shuffle_remembered = !shuffle_remembered;
	} else if (mode == PLAYBACK_MODE_REPEAT_ALL) {
		shuffle_remembered = true;
		playlist_set_mode(PLAYBACK_MODE_SHUFFLE_REPEAT);
	} else {
		shuffle_remembered = true;
		playlist_set_mode(endless ? PLAYBACK_MODE_SHUFFLE_REPEAT : PLAYBACK_MODE_SHUFFLE);
	}
	update_transport_modes();
	upnext_refresh();
	carousel_sync_adjacent_requests();
	device_state_queue_changed();
}

static void repeat_btn_event_cb(lv_event_t *e) {
	(void)e;
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	playback_mode_t mode = playlist_get_mode();
	switch (mode) {
	case PLAYBACK_MODE_NORMAL:
		playlist_set_mode(PLAYBACK_MODE_REPEAT_ALL);
		break;
	case PLAYBACK_MODE_REPEAT_ALL:
		shuffle_remembered = false;
		playlist_set_mode(PLAYBACK_MODE_REPEAT_ONE);
		break;
	case PLAYBACK_MODE_SHUFFLE:
		playlist_set_mode(PLAYBACK_MODE_SHUFFLE_REPEAT);
		break;
	case PLAYBACK_MODE_SHUFFLE_REPEAT:
		shuffle_remembered = true;
		playlist_set_mode(PLAYBACK_MODE_REPEAT_ONE);
		break;
	case PLAYBACK_MODE_REPEAT_ONE:
	default:
		playlist_set_mode(shuffle_remembered ? PLAYBACK_MODE_SHUFFLE : PLAYBACK_MODE_NORMAL);
		break;
	}
	update_transport_modes();
	upnext_refresh();
	carousel_sync_adjacent_requests();
	device_state_queue_changed();
}

static bool speed_is_podcast(void) {
	return podcast_mode && !audiobook_mode;
}

static int speed_current_permille(void) {
	return speed_is_podcast() ? podcast_speed_permille() : audiobook_speed_permille();
}

static void update_speed_button(void) {
	if (!speed_btn_icon) {
		return;
	}
	bool normal = speed_current_permille() == AUDIOBOOK_SPEED_NORMAL;
	lv_obj_set_style_image_recolor(speed_btn_icon, normal ? pal_cur.text_secondary : pal_cur.accent, 0);
	lv_obj_set_style_image_recolor_opa(speed_btn_icon, LV_OPA_COVER, 0);
	lv_obj_set_style_image_opa(speed_btn_icon, normal ? LV_OPA_70 : LV_OPA_COVER, 0);
}

static void speed_pick(void *user) {
	int permille = (int)(intptr_t)user;
	if (speed_is_podcast()) {
		podcast_set_speed_permille(permille);
		permille = podcast_speed_permille();
	} else {
		audiobook_set_speed_permille(permille);
		permille = audiobook_speed_permille();
	}
	audio_set_speed((double)permille / 1000.0);
	update_speed_button();
}

static void speed_btn_event_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	static const int CHOICES[] = {500, 750, 1000, 1250, 1500, 1750, 2000};
	static const char *const LABELS[] = {"0.5x", "0.75x", "1.0x", "1.25x", "1.5x", "1.75x", "2.0x"};
	enum { CHOICE_COUNT = sizeof(CHOICES) / sizeof(CHOICES[0]) };

	popover_item_t items[CHOICE_COUNT] = {0};
	int current = speed_current_permille();
	for (int i = 0; i < CHOICE_COUNT; i++) {
		items[i].label = LABELS[i];
		items[i].action = speed_pick;
		items[i].user = (void *)(intptr_t)CHOICES[i];
		items[i].checked = (CHOICES[i] == current);
	}
	popover_show(lv_event_get_current_target(e), items, CHOICE_COUNT);
}

// ---------------------------------------------------------------------------
// Queue Position & Bottom Shortcuts Row
// ---------------------------------------------------------------------------

bool player_queue_position(int *position, int *count) {
	*position = 0;
	*count = 0;
	if (live_mode) {
		int index, total;
		if (live_custom_nav && radio_list_position(&index, &total)) {
			*position = index + 1;
			*count = total;
			return true;
		}
		return false;
	}
	int total = playlist_count();
	int index = playlist_current_index();
	if (total <= 1 || index < 0 || audiobook_mode || dlna_owns_playback()) {
		return false;
	}
	*position = (int)playlist_current_entry() + 1;
	*count = total;
	return true;
}

static void update_queue_and_shortcuts(void) {
	// Lyrics shortcut pill state
	if (lyrics_shortcut_btn && lyrics_shortcut_icon && lyrics_shortcut_label) {
		bool in_int = (lyrics_state == LYRICS_VIEW_INTEGRATED);
		const lv_image_dsc_t *ic = in_int ? &icon_album_small : &icon_lyrics;
		if (lv_image_get_src(lyrics_shortcut_icon) != ic) {
			lv_image_set_src(lyrics_shortcut_icon, ic);
		}
		set_label_text_if_changed(lyrics_shortcut_label, tr(in_int ? "lyrics_artwork" : "lyrics"));
		lv_color_t fg = in_int ? pal_cur.accent : pal_cur.text_secondary;
		lv_obj_set_style_image_recolor(lyrics_shortcut_icon, fg, 0);
		lv_obj_set_style_image_recolor_opa(lyrics_shortcut_icon, LV_OPA_COVER, 0);
		lv_obj_set_style_text_color(lyrics_shortcut_label, fg, 0);
		lv_obj_set_style_bg_color(lyrics_shortcut_btn, pal_cur.surface, 0);
		lv_obj_set_style_bg_opa(lyrics_shortcut_btn, in_int ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
		lv_obj_set_hidden(lyrics_shortcut_btn, live_mode || audiobook_mode);
	}

	// Queue shortcut pill state
	if (queue_shortcut_btn && queue_shortcut_icon && queue_shortcut_label) {
		int pos = 0, total = 0;
		char qtxt[64];
		if (player_queue_position(&pos, &total)) {
			snprintf(qtxt, sizeof(qtxt), "%d / %d", pos, total);
		} else {
			snprintf(qtxt, sizeof(qtxt), "%s", tr("queue"));
		}
		set_label_text_if_changed(queue_shortcut_label, qtxt);
		lv_obj_set_style_image_recolor(queue_shortcut_icon, pal_cur.text_secondary, 0);
		lv_obj_set_style_image_recolor_opa(queue_shortcut_icon, LV_OPA_COVER, 0);
		lv_obj_set_style_text_color(queue_shortcut_label, pal_cur.text_secondary, 0);
		lv_obj_set_hidden(queue_shortcut_btn, live_mode || audiobook_mode || dlna_owns_playback());
	}

	// Compact unobtrusive Up Next one-line bearing in center of bottom strip
	if (upnext_bearing_btn && upnext_bearing_label) {
		char next_title[256] = "";
		bool rep_one = false;
		if (!live_mode && !audiobook_mode && lyrics_state == LYRICS_VIEW_NONE &&
			upnext_get_auto(next_title, sizeof(next_title), NULL, 0, &rep_one) && next_title[0] && !rep_one) {
			char bearing[300];
			snprintf(bearing, sizeof(bearing), "%s · %s", tr("up_next"), next_title);
			set_label_text_if_changed(upnext_bearing_label, bearing);
			lv_obj_set_style_text_color(upnext_bearing_label, pal_cur.text_secondary, 0);
			lv_obj_set_style_text_opa(upnext_bearing_label, LV_OPA_70, 0);
			lv_obj_set_hidden(upnext_bearing_btn, false);
		} else {
			lv_obj_set_hidden(upnext_bearing_btn, true);
		}
	}
}

static void on_upnext_resolved(void) {
	carousel_sync_adjacent_requests();
	update_queue_and_shortcuts();
}

// ---------------------------------------------------------------------------
// Live Radio & Audiobook Mode Switching
// ---------------------------------------------------------------------------

static void apply_connecting(void) {
	bool connecting = radio_is_connecting();
	if (connecting == connecting_shown) {
		return;
	}
	connecting_shown = connecting;
	lv_obj_t *const buttons[] = {play_btn, prev_btn_obj, next_btn_obj, fs_play_btn, fs_prev_btn, fs_next_btn};
	for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
		if (!buttons[i]) continue;
		if (connecting) {
			lv_obj_add_state(buttons[i], LV_STATE_DISABLED);
		} else {
			lv_obj_remove_state(buttons[i], LV_STATE_DISABLED);
		}
	}
}

static void apply_live_mode(bool live) {
	bool custom_nav = live && radio_can_step();
	bool transport_hidden = live && !custom_nav;

	if (live == live_mode && transport_hidden == live_transport_hidden && custom_nav == live_custom_nav &&
		cslots[1].placeholder) {
		return;
	}
	live_mode = live;
	live_transport_hidden = transport_hidden;
	live_custom_nav = custom_nav;

	if (live && lyrics_state != LYRICS_VIEW_NONE) {
		lyrics_set_mode(LYRICS_VIEW_NONE, false);
	}

	lv_obj_t *const hidden_when_live[] = {more_btn_obj, shuffle_btn_obj, repeat_btn_obj, wave_box, elapsed_label,
										  remaining_label};
	for (size_t i = 0; i < sizeof(hidden_when_live) / sizeof(hidden_when_live[0]); i++) {
		if (hidden_when_live[i]) {
			lv_obj_set_hidden(hidden_when_live[i], live);
		}
	}

	lv_obj_t *const transport[] = {prev_btn_obj, next_btn_obj};
	for (size_t i = 0; i < sizeof(transport) / sizeof(transport[0]); i++) {
		if (transport[i]) {
			lv_obj_set_hidden(transport[i], transport_hidden);
		}
	}

	sync_header_title();
	update_service_badges();
	refresh_cover_placeholders();
	update_queue_and_shortcuts();
}

static void apply_audiobook_mode(bool book, bool podcast) {
	bool chapters = book && (audiobook_chapter_count() > 0 || audiobook_part_count() > 1);
	bool skips = book || podcast;
	int back = (podcast && !book) ? podcast_skip_back() : audiobook_skip_back();
	int forward = (podcast && !book) ? podcast_skip_forward() : audiobook_skip_forward();

	if (audiobook_mode_valid && book == audiobook_mode && podcast == podcast_mode &&
		chapters == audiobook_has_chapters && back == audiobook_skip_shown_back &&
		forward == audiobook_skip_shown_forward) {
		return;
	}
	audiobook_mode = book;
	podcast_mode = podcast;
	audiobook_has_chapters = chapters;
	audiobook_skip_shown_back = back;
	audiobook_skip_shown_forward = forward;
	audiobook_mode_valid = true;

	if (book && lyrics_state != LYRICS_VIEW_NONE) {
		lyrics_set_mode(LYRICS_VIEW_NONE, false);
	}

	const lv_image_dsc_t *p_ic = !skips                            ? &icon_skip_back
								 : back == AUDIOBOOK_SKIP_HUGE ? &icon_prev_60
								 : back == AUDIOBOOK_SKIP_LONG ? &icon_prev_30
															   : &icon_prev_10;
	const lv_image_dsc_t *n_ic = !skips                               ? &icon_skip_forward
								 : forward == AUDIOBOOK_SKIP_HUGE ? &icon_next_60
								 : forward == AUDIOBOOK_SKIP_LONG ? &icon_next_30
																  : &icon_next_10;
	if (prev_icon) lv_image_set_src(prev_icon, p_ic);
	if (next_icon) lv_image_set_src(next_icon, n_ic);
	if (fs_prev_icon) lv_image_set_src(fs_prev_icon, p_ic);
	if (fs_next_icon) lv_image_set_src(fs_next_icon, n_ic);

	if (more_btn_icon) {
		lv_image_set_src(more_btn_icon, (podcast && !book) ? &icon_podcast_episodes : &icon_ellipsis_vertical);
	}
	if (fav_btn_obj) {
		lv_obj_set_hidden(fav_btn_obj, book);
	}
	if (shuffle_btn_obj) {
		lv_obj_set_hidden(shuffle_btn_obj, book || podcast || live_mode);
	}
	if (repeat_btn_obj) {
		lv_obj_set_hidden(repeat_btn_obj, book || podcast || live_mode);
	}
	if (speed_btn_obj) {
		lv_obj_set_hidden(speed_btn_obj, !(book || podcast));
		if (book || podcast) {
			update_speed_button();
		}
	}

	sync_header_title();
	refresh_cover_placeholders();
	update_queue_and_shortcuts();
}

static double current_chapter_end(double position) {
	int index = audiobook_chapter_at(position);
	if (index < 0) return 0;
	double start = 0;
	if (!audiobook_chapter(index + 1, NULL, 0, &start)) return 0;
	return start;
}

static void pause_book_at(double seconds) {
	device_state_seek(seconds);
	if (audio_get_status() == AUDIO_STATUS_PLAYING) {
		audiobook_suppress_rewind_once();
		device_state_toggle_play_pause();
	}
	audiobook_note_position(seconds, 0, true);
}

static bool audiobook_auto_stop(const device_state_t *state) {
	if (state->status != AUDIO_STATUS_PLAYING || !audiobook_stop_at_chapter_end()) {
		return false;
	}
	double end = current_chapter_end(state->progress_current_secs);
	if (end > 0 && state->progress_current_secs >= end - 0.5) {
		pause_book_at(end);
		return true;
	}
	return false;
}

static void audiobook_seek_by(double delta, const device_state_t *state) {
	double target = state->progress_current_secs + delta;
	if (target < 0) target = 0;
	double length = state->progress_total_secs > 0 ? state->progress_total_secs : current_total_length;
	if (length > 1.0 && target > length - 1.0) {
		target = length - 1.0;
	}
	device_state_seek(target);
	audiobook_note_position(target, length, true);
}

static void apply_playback_status(audio_status_t status) {
	bool playing = (status == AUDIO_STATUS_PLAYING);
	const lv_image_dsc_t *glyph = radio_is_connecting() ? &icon_loader_small
								  : playing             ? (live_mode ? &icon_stop : &icon_pause)
														: &icon_play;
	if (play_btn_icon && lv_image_get_src(play_btn_icon) != glyph) {
		lv_image_set_src(play_btn_icon, glyph);
	}
	if (fs_play_icon && lv_image_get_src(fs_play_icon) != glyph) {
		lv_image_set_src(fs_play_icon, glyph);
	}

	if (playing) {
		polls_since_stop = 0;
	} else if (polls_since_stop < POLLS_FAST_AFTER_STOP) {
		polls_since_stop++;
	}

	bool settling = polls_since_stop < POLLS_FAST_AFTER_STOP;
	if (progress_slider_timer) {
		lv_timer_set_period(progress_slider_timer, (playing || settling) ? POLL_PERIOD_PLAYING_MS : POLL_PERIOD_IDLE_MS);
		lv_timer_resume(progress_slider_timer);
	}

	topbar_refresh_playback();

	sleeptimer_note_playing(playing ? sleeptimer_kind_playing() : SLEEPTIMER_COUNT);
	if (sleep_timer) {
		if (sleeptimer_any_running()) {
			lv_timer_resume(sleep_timer);
		} else {
			lv_timer_pause(sleep_timer);
		}
	}
	notify_state_cbs();
}

// ---------------------------------------------------------------------------
// First-Class Lyrics Experience (Mode A Integrated & Mode B Full-Screen)
// ---------------------------------------------------------------------------

typedef struct {
	unsigned generation;
	char path[1024];
	char title[256];
	lyrics_t lyrics;
	bool found;
} lyrics_job_t;

static const lv_font_t *lyrics_font_for(bool active_line, bool fullscreen) {
	switch (lyrics_text_size) {
	case PLAYER_LYRICS_TEXT_SMALL:
		if (fullscreen) return active_line ? &font_ui_24_bold : &font_ui_20;
		return active_line ? &font_ui_22_bold : &font_ui_18;
	case PLAYER_LYRICS_TEXT_LARGE:
		if (fullscreen) return active_line ? &font_ui_32_bold : &font_ui_24;
		return active_line ? &font_ui_28_bold : &font_ui_22;
	case PLAYER_LYRICS_TEXT_MEDIUM:
	default:
		if (fullscreen) return active_line ? &font_ui_28_bold : &font_ui_22;
		return active_line ? &font_ui_24_bold : &font_ui_20;
	}
}

static void lyrics_screen_hold_update(void) {
	power_hold_screen_for_view(sheet_open && lyrics_state != LYRICS_VIEW_NONE);
}

static void lyrics_scroll_exec(void *obj, int32_t y) {
	lv_obj_scroll_to_y((lv_obj_t *)obj, y, LV_ANIM_OFF);
}

static void lyrics_sync_resume_btn(void) {
	if (!resume_sync_btn) {
		return;
	}
	bool show = (lyrics_state != LYRICS_VIEW_NONE) && lyrics_cur.synced && lyrics_cur.count > 0 && lyrics_manual_hold;
	lv_obj_set_hidden(resume_sync_btn, !show);
	if (show) {
		lv_obj_move_foreground(resume_sync_btn);
	}
}

static void lyrics_paint_single_line(int index) {
	if (!lyrics_view || index < 0 || index >= (int)lv_obj_get_child_count(lyrics_view) || index >= lyrics_cur.count) {
		return;
	}
	lv_obj_t *label = lv_obj_get_child(lyrics_view, index);
	bool fs = (lyrics_state == LYRICS_VIEW_FULLSCREEN);

	if (!lyrics_cur.synced) {
		lv_obj_set_style_text_font(label, lyrics_font_for(false, fs), 0);
		lv_obj_set_style_text_color(label, pal_cur.text_primary, 0);
		lv_obj_set_style_text_opa(label, LV_OPA_90, 0);
		return;
	}

	bool active = (index == lyrics_lit);
	lv_obj_set_style_text_font(label, lyrics_font_for(active, fs), 0);
	if (active) {
		lv_obj_set_style_text_color(label, pal_cur.accent, 0);
		lv_obj_set_style_text_opa(label, LV_OPA_COVER, 0);
	} else {
		int dist = (lyrics_lit >= 0) ? LV_ABS(index - lyrics_lit) : (index + 1);
		lv_opa_t opa = (dist == 1) ? 185 : (dist == 2) ? 132 : 92;
		lv_obj_set_style_text_color(label, pal_cur.text_primary, 0);
		lv_obj_set_style_text_opa(label, opa, 0);
	}
}

static void lyrics_apply_line_styles(void) {
	if (!lyrics_view) {
		return;
	}
	uint32_t n = lv_obj_get_child_count(lyrics_view);
	for (uint32_t i = 0; i < n; i++) {
		lyrics_paint_single_line((int)i);
	}
}

static void lyrics_update_padding(void) {
	if (!lyrics_view) {
		return;
	}
	bool fs = (lyrics_state == LYRICS_VIEW_FULLSCREEN);
	int vh = lv_obj_get_height(lyrics_view);
	if (vh <= 0) {
		vh = fs ? (screen_h - 230) : stage_h;
	}
	int pad = lyrics_cur.synced ? (vh / 2 - 28) : 18;
	if (pad < 16) pad = 16;
	lv_obj_set_style_pad_top(lyrics_view, pad, 0);
	lv_obj_set_style_pad_bottom(lyrics_view, lyrics_cur.synced ? pad : 28, 0);
	lv_obj_set_style_pad_row(lyrics_view, fs ? LYRICS_LINE_GAP_FS : LYRICS_LINE_GAP_INT, 0);
}

static void lyrics_sync_empty_state(void) {
	if (!lyrics_empty_box || !lyrics_view) {
		return;
	}
	if (!lyrics_loaded) {
		set_label_text_if_changed(lyrics_empty_label, tr("lyrics_loading"));
		if (lyrics_empty_search_btn) lv_obj_set_hidden(lyrics_empty_search_btn, true);
		lv_obj_set_hidden(lyrics_empty_box, false);
		lv_obj_set_hidden(lyrics_view, true);
		return;
	}
	if (lyrics_cur.count == 0) {
		set_label_text_if_changed(lyrics_empty_label, tr("lyrics_none"));
		device_state_t st;
		device_state_get(&st);
		if (lyrics_empty_search_btn) {
			lv_obj_set_hidden(lyrics_empty_search_btn, !playing_local_file(&st));
		}
		lv_obj_set_hidden(lyrics_empty_box, false);
		lv_obj_set_hidden(lyrics_view, true);
		return;
	}
	lv_obj_set_hidden(lyrics_empty_box, true);
	lv_obj_set_hidden(lyrics_view, false);
}

static void lyrics_fill(void) {
	if (!lyrics_view) {
		return;
	}
	lv_anim_delete(lyrics_view, lyrics_scroll_exec);
	lv_obj_clean(lyrics_view);
	lyrics_lit = -1;
	lyrics_manual_hold = false;
	lyrics_user_until = 0;
	lyrics_centred = false;
	lv_obj_scroll_to_y(lyrics_view, 0, LV_ANIM_OFF);
	lyrics_sync_resume_btn();
	lyrics_sync_empty_state();

	if (!lyrics_loaded || lyrics_cur.count == 0) {
		return;
	}

	lyrics_update_padding();
	bool fs = (lyrics_state == LYRICS_VIEW_FULLSCREEN);
	for (int i = 0; i < lyrics_cur.count; i++) {
		lv_obj_t *line = lv_label_create(lyrics_view);
		const char *txt = lyrics_cur.lines[i].text;
		// Instrumental timestamped break: subtle minimalist rest dots, never invented words.
		const char *shown = (txt && txt[0]) ? txt : (lyrics_cur.synced ? "· · ·" : " ");
		lv_label_set_text(line, shown);
		lv_label_set_long_mode(line, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(line, lv_pct(100));
		lv_obj_set_style_text_align(line, fs ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_event_bubble(line, true);
		lv_obj_set_clickable(line, false);
	}
	lyrics_apply_line_styles();
}

static void lyrics_centre_line(int index, bool animate) {
	if (!lyrics_view || index < 0 || index >= (int)lv_obj_get_child_count(lyrics_view)) {
		return;
	}
	lv_obj_t *label = lv_obj_get_child(lyrics_view, index);
	lv_obj_update_layout(lyrics_view);
	lv_area_t line_area, view_area;
	lv_obj_get_coords(label, &line_area);
	lv_obj_get_coords(lyrics_view, &view_area);

	int32_t from = lv_obj_get_scroll_y(lyrics_view);
	int32_t to = from + (line_area.y1 + line_area.y2) / 2 - (view_area.y1 + view_area.y2) / 2;
	if (to < 0) to = 0;

	lv_anim_delete(lyrics_view, lyrics_scroll_exec);
	if (to == from || !animate || !motion_enabled()) {
		lv_obj_scroll_to_y(lyrics_view, to, LV_ANIM_OFF);
		return;
	}

	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, lyrics_view);
	lv_anim_set_exec_cb(&a, lyrics_scroll_exec);
	lv_anim_set_values(&a, from, to);
	lv_anim_set_duration(&a, LYRICS_SCROLL_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_start(&a);
}

static void lyrics_follow(double seconds) {
	if (lyrics_state == LYRICS_VIEW_NONE || !lyrics_view || !lyrics_cur.synced || lyrics_cur.count == 0) {
		return;
	}

	int32_t raw_ms = (int32_t)(seconds * 1000.0);
	if (lyrics_tap_ms >= 0) {
		if ((int32_t)(lv_tick_get() - lyrics_tap_until) < 0 && LV_ABS(raw_ms - lyrics_tap_ms) > 1500) {
			return;
		}
		lyrics_tap_ms = -1;
	}

	int32_t eff_ms = raw_ms + lyrics_offset_ms;
	if (eff_ms < 0) eff_ms = 0;
	int line = lyrics_line_at(&lyrics_cur, eff_ms);

	if (line != lyrics_lit) {
		int prev = lyrics_lit;
		lyrics_lit = line;
		// Repaint only lines whose progressive distance band changed
		int lo1 = (prev >= 0 ? prev : 0) - 3;
		int hi1 = (prev >= 0 ? prev : 0) + 3;
		int lo2 = (line >= 0 ? line : 0) - 3;
		int hi2 = (line >= 0 ? line : 0) + 3;
		for (int i = lo1; i <= hi1; i++) lyrics_paint_single_line(i);
		for (int i = lo2; i <= hi2; i++) {
			if (i < lo1 || i > hi1) lyrics_paint_single_line(i);
		}
		lyrics_centred = false;
	}

	if (lyrics_manual_hold) {
		if ((int32_t)(lv_tick_get() - lyrics_user_until) >= 0 && audio_get_status() == AUDIO_STATUS_PLAYING) {
			lyrics_manual_hold = false;
			lyrics_user_until = 0;
			lyrics_sync_resume_btn();
			lyrics_centred = false;
		} else {
			return;
		}
	}

	if (!lyrics_centred) {
		lyrics_centred = true;
		lyrics_centre_line(line >= 0 ? line : 0, true);
	}
}

static void lyrics_arrived(void *user) {
	lyrics_job_t *job = user;
	if (job->generation == lyrics_generation && strcmp(job->path, lyrics_path) == 0) {
		lyrics_free(&lyrics_cur);
		lyrics_cur = job->lyrics;
		lyrics_loaded = true;
		lyrics_fill();
		if (lyrics_state != LYRICS_VIEW_NONE && lyrics_cur.synced && lyrics_cur.count > 0) {
			device_state_t st;
			device_state_get(&st);
			lyrics_follow(st.progress_current_secs);
		}
	} else {
		lyrics_free(&job->lyrics);
	}
	free(job);
}

static void *lyrics_worker(void *arg) {
	lyrics_job_t *job = arg;
	thread_be_low_priority("lyricsload");
	job->found = lyrics_load(job->path, job->title, &job->lyrics);
	if (!gui_post(lyrics_arrived, job)) {
		lyrics_free(&job->lyrics);
		free(job);
	}
	return NULL;
}

static void lyrics_request(const device_state_t *state) {
	if (!state->current_file[0] || !playing_local_file(state)) {
		if (lyrics_path[0]) {
			lyrics_path[0] = '\0';
			lyrics_free(&lyrics_cur);
			lyrics_loaded = true;
			lyrics_generation++;
			lyrics_fill();
		}
		return;
	}
	if (strcmp(state->current_file, lyrics_path) == 0) {
		return;
	}
	snprintf(lyrics_path, sizeof(lyrics_path), "%s", state->current_file);
	lyrics_free(&lyrics_cur);
	lyrics_loaded = false;
	lyrics_generation++;
	lyrics_fill();

	lyrics_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		return;
	}
	job->generation = lyrics_generation;
	snprintf(job->path, sizeof(job->path), "%s", state->current_file);
	snprintf(job->title, sizeof(job->title), "%s", state->metadata.title);
	pthread_t thread;
	if (pthread_create(&thread, NULL, lyrics_worker, job) == 0) {
		pthread_detach(thread);
	} else {
		free(job);
	}
}

static void lyrics_scroll_cb(lv_event_t *e) {
	(void)e;
	if (lv_indev_active() && lyrics_cur.synced && lyrics_cur.count > 0) {
		lv_anim_delete(lyrics_view, lyrics_scroll_exec);
		lyrics_manual_hold = true;
		lyrics_user_until = lv_tick_get() + LYRICS_USER_HOLD_MS;
		lyrics_centred = false;
		lyrics_sync_resume_btn();
	}
}

static void resume_sync_btn_cb(lv_event_t *e) {
	(void)e;
	lyrics_manual_hold = false;
	lyrics_user_until = 0;
	lyrics_centred = false;
	lyrics_sync_resume_btn();
	device_state_t st;
	device_state_get(&st);
	lyrics_follow(st.progress_current_secs);
}

static void lyrics_tap_cb(lv_event_t *e) {
	(void)e;
	if (player_sheet_drag_active() || switcher_back_drag_active() || !lyrics_cur.synced || !lyrics_view) {
		return;
	}
	lv_indev_t *indev = lv_indev_active();
	if (!indev) {
		return;
	}
	lv_point_t p;
	lv_indev_get_point(indev, &p);
	uint32_t count = lv_obj_get_child_count(lyrics_view);
	int gap = (lyrics_state == LYRICS_VIEW_FULLSCREEN) ? LYRICS_LINE_GAP_FS : LYRICS_LINE_GAP_INT;

	for (uint32_t i = 0; i < count && i < (uint32_t)lyrics_cur.count; i++) {
		lv_area_t area;
		lv_obj_get_coords(lv_obj_get_child(lyrics_view, i), &area);
		if (p.y >= area.y1 && p.y <= area.y2 + gap) {
			int32_t ms = lyrics_cur.lines[i].ms;
			if (ms < 0) return;
			int32_t target_ms = ms - lyrics_offset_ms;
			if (target_ms < 0) target_ms = 0;
			double secs = (double)target_ms / 1000.0;
			device_state_seek(secs);
			lyrics_manual_hold = false;
			lyrics_user_until = 0;
			lyrics_centred = false;
			lyrics_sync_resume_btn();
			lyrics_tap_ms = target_ms;
			lyrics_tap_until = lv_tick_get() + LYRICS_TAP_WAIT_MS;
			lyrics_follow(secs);
			return;
		}
	}
}

static void update_fs_offset_labels(void) {
	if (!fs_offset_value_label || !fs_offset_toggle_label) {
		return;
	}
	char buf[40];
	if (lyrics_offset_ms == 0) {
		snprintf(buf, sizeof(buf), "%s · 0.0s", tr("lyrics_offset"));
	} else {
		snprintf(buf, sizeof(buf), "%s · %+0.1fs", tr("lyrics_offset"), (double)lyrics_offset_ms / 1000.0);
	}
	set_label_text_if_changed(fs_offset_value_label, buf);
	lv_obj_set_style_text_color(fs_offset_value_label,
								lyrics_offset_ms != 0 ? pal_cur.accent : pal_cur.text_primary, 0);
	lv_obj_set_style_text_color(fs_offset_toggle_label,
								(fs_offset_bar_open || lyrics_offset_ms != 0) ? pal_cur.accent : pal_cur.text_primary,
								0);
}

static void layout_fs_stage(void) {
	if (!fullscreen_lyrics_box || !fs_stage_area) {
		return;
	}
	int top_y = bar_always ? (bar_height + 2) : 12;
	lv_obj_set_pos(fs_header_row, side_margin, top_y);
	lv_obj_set_size(fs_header_row, content_w, 54);

	int stage_top = top_y + 58;
	if (fs_offset_bar) {
		lv_obj_set_hidden(fs_offset_bar, !fs_offset_bar_open);
		if (fs_offset_bar_open) {
			lv_obj_set_pos(fs_offset_bar, side_margin, stage_top);
			lv_obj_set_size(fs_offset_bar, content_w, 42);
			stage_top += 48;
		}
	}

	int deck_h = is_tall_panel ? 136 : 126;
	lv_obj_set_pos(fs_bottom_deck, side_margin, screen_h - deck_h);
	lv_obj_set_size(fs_bottom_deck, content_w, deck_h);

	int sh = (screen_h - deck_h - 6) - stage_top;
	if (sh < 140) sh = 140;
	lv_obj_set_pos(fs_stage_area, side_margin, stage_top);
	lv_obj_set_size(fs_stage_area, content_w, sh);

	if (lyrics_state == LYRICS_VIEW_FULLSCREEN && lyrics_view) {
		lv_obj_set_size(lyrics_view, content_w, sh);
		lv_obj_set_pos(lyrics_view, 0, 0);
		lyrics_update_padding();
	}
}

static void stage_opa_exec_cb(void *var, int32_t v) {
	lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void lyrics_reparent_shared_stage(lv_obj_t *host, int w, int h, bool fullscreen) {
	if (!host || !lyrics_view || !lyrics_empty_box || !resume_sync_btn) {
		return;
	}
	lv_obj_set_parent(lyrics_view, host);
	lv_obj_set_pos(lyrics_view, 0, fullscreen ? 0 : 40);
	lv_obj_set_size(lyrics_view, w, fullscreen ? h : (h - 42));

	lv_obj_set_parent(lyrics_empty_box, host);
	lv_obj_center(lyrics_empty_box);

	lv_obj_set_parent(resume_sync_btn, host);
	lv_obj_align(resume_sync_btn, LV_ALIGN_BOTTOM_MID, 0, -12);

	uint32_t count = lv_obj_get_child_count(lyrics_view);
	for (uint32_t i = 0; i < count; i++) {
		lv_obj_set_style_text_align(lv_obj_get_child(lyrics_view, i),
									fullscreen ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);
	}
	lyrics_update_padding();
	lyrics_apply_line_styles();
	lyrics_sync_empty_state();
	lyrics_sync_resume_btn();
}

static void lyrics_set_mode(lyrics_view_state_t mode, bool animate) {
	if (mode == lyrics_state) {
		return;
	}

	lv_anim_delete(carousel_stage, stage_opa_exec_cb);
	lv_anim_delete(integrated_lyrics_box, stage_opa_exec_cb);
	lv_anim_delete(fullscreen_lyrics_box, stage_opa_exec_cb);

	lyrics_state = mode;
	if (mode == LYRICS_VIEW_INTEGRATED) {
		lyrics_last_mode = PLAYER_LYRICS_INTEGRATED;
		if (lyrics_remember_mode) {
			config_set_int("lyrics", "last_mode", (int)lyrics_last_mode);
			config_save();
		}
	} else if (mode == LYRICS_VIEW_FULLSCREEN) {
		lyrics_last_mode = PLAYER_LYRICS_FULLSCREEN;
		if (lyrics_remember_mode) {
			config_set_int("lyrics", "last_mode", (int)lyrics_last_mode);
			config_save();
		}
	}

	if (mode == LYRICS_VIEW_NONE) {
		if (sheet_open) {
			back_btn_force_hidden(false);
			back_btn_sync_visibility();
		}
		if (fullscreen_lyrics_box) lv_obj_set_hidden(fullscreen_lyrics_box, true);
		if (integrated_lyrics_box) lv_obj_set_hidden(integrated_lyrics_box, true);
		if (carousel_stage) {
			lv_obj_set_hidden(carousel_stage, false);
			carousel_layout_slots();
			if (animate && motion_enabled()) {
				lv_obj_set_style_opa(carousel_stage, LV_OPA_TRANSP, 0);
				lv_anim_t a;
				lv_anim_init(&a);
				lv_anim_set_var(&a, carousel_stage);
				lv_anim_set_exec_cb(&a, stage_opa_exec_cb);
				lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
				lv_anim_set_duration(&a, LYRICS_MODE_ANIM_MS);
				lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
				lv_anim_start(&a);
			} else {
				lv_obj_set_style_opa(carousel_stage, LV_OPA_COVER, 0);
			}
		}
	} else if (mode == LYRICS_VIEW_INTEGRATED) {
		if (sheet_open) {
			back_btn_force_hidden(false);
			back_btn_sync_visibility();
		}
		if (fullscreen_lyrics_box) lv_obj_set_hidden(fullscreen_lyrics_box, true);
		if (carousel_stage) lv_obj_set_hidden(carousel_stage, true);

		lyrics_reparent_shared_stage(integrated_lyrics_box, content_w - 24, stage_h, false);
		if (int_lyrics_search_btn) lv_obj_move_foreground(int_lyrics_search_btn);
		if (int_lyrics_expand_btn) lv_obj_move_foreground(int_lyrics_expand_btn);
		lv_obj_set_hidden(integrated_lyrics_box, false);

		if (animate && motion_enabled()) {
			lv_obj_set_style_opa(integrated_lyrics_box, LV_OPA_TRANSP, 0);
			lv_anim_t a;
			lv_anim_init(&a);
			lv_anim_set_var(&a, integrated_lyrics_box);
			lv_anim_set_exec_cb(&a, stage_opa_exec_cb);
			lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
			lv_anim_set_duration(&a, LYRICS_MODE_ANIM_MS);
			lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
			lv_anim_start(&a);
		} else {
			lv_obj_set_style_opa(integrated_lyrics_box, LV_OPA_COVER, 0);
		}

		device_state_t st;
		device_state_get(&st);
		lyrics_request(&st);
		lyrics_centred = false;
		lyrics_follow(st.progress_current_secs);
	} else if (mode == LYRICS_VIEW_FULLSCREEN) {
		if (sheet_open) {
			back_btn_force_hidden(true);
		}
		layout_fs_stage();
		lyrics_reparent_shared_stage(fs_stage_area, content_w, lv_obj_get_height(fs_stage_area), true);
		lv_obj_set_hidden(fullscreen_lyrics_box, false);
		lv_obj_move_foreground(fullscreen_lyrics_box);

		if (animate && motion_enabled()) {
			lv_obj_set_style_opa(fullscreen_lyrics_box, LV_OPA_TRANSP, 0);
			lv_anim_t a;
			lv_anim_init(&a);
			lv_anim_set_var(&a, fullscreen_lyrics_box);
			lv_anim_set_exec_cb(&a, stage_opa_exec_cb);
			lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
			lv_anim_set_duration(&a, LYRICS_MODE_ANIM_MS);
			lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
			lv_anim_start(&a);
		} else {
			lv_obj_set_style_opa(fullscreen_lyrics_box, LV_OPA_COVER, 0);
		}

		device_state_t st;
		device_state_get(&st);
		lyrics_request(&st);
		lyrics_centred = false;
		lyrics_follow(st.progress_current_secs);
	}

	lyrics_screen_hold_update();
	update_queue_and_shortcuts();
}

static void lyrics_shortcut_cb(lv_event_t *e) {
	(void)e;
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	if (lyrics_state != LYRICS_VIEW_NONE) {
		lyrics_set_mode(LYRICS_VIEW_NONE, true);
		return;
	}
	player_lyrics_mode_t preferred = lyrics_remember_mode ? lyrics_last_mode : lyrics_default_mode;
	lyrics_set_mode(preferred == PLAYER_LYRICS_FULLSCREEN ? LYRICS_VIEW_FULLSCREEN : LYRICS_VIEW_INTEGRATED, true);
}

static void lyrics_expand_cb(lv_event_t *e) {
	(void)e;
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	lyrics_set_mode(LYRICS_VIEW_FULLSCREEN, true);
}

static void fs_collapse_to_integrated_cb(lv_event_t *e) {
	(void)e;
	lyrics_set_mode(LYRICS_VIEW_INTEGRATED, true);
}

static void fs_return_to_artwork_cb(lv_event_t *e) {
	(void)e;
	lyrics_set_mode(LYRICS_VIEW_NONE, true);
}

static void fs_size_cycle_cb(lv_event_t *e) {
	(void)e;
	player_lyrics_text_size_t next = (player_lyrics_text_size_t)(((int)lyrics_text_size + 1) % 3);
	player_lyrics_set_text_size(next);
}

static void fs_offset_toggle_cb(lv_event_t *e) {
	(void)e;
	fs_offset_bar_open = !fs_offset_bar_open;
	update_fs_offset_labels();
	layout_fs_stage();
	lyrics_centred = false;
	device_state_t st;
	device_state_get(&st);
	lyrics_follow(st.progress_current_secs);
}

static void fs_offset_step_cb(lv_event_t *e) {
	int delta = (int)(intptr_t)lv_event_get_user_data(e);
	if (delta == 0) {
		player_lyrics_set_offset_ms(0);
	} else {
		player_lyrics_set_offset_ms(lyrics_offset_ms + delta);
	}
}

// ---------------------------------------------------------------------------
// LRCLIB Search & Wi-Fi Assist (Preserved)
// ---------------------------------------------------------------------------

typedef struct {
	char path[1024];
	char title[256];
	char artist[256];
	char album[256];
	int duration;
	lrclib_result_t result;
	lrclib_record_t *records;
	int count;
} lyrics_fetch_t;

static bool lyrics_fetching;

#define LYRICS_WIFI_POLL_MS 500
#define LYRICS_WIFI_WAIT_MS 30000
static lv_timer_t *lyrics_wifi_timer;
static uint32_t lyrics_wifi_since;
static char lyrics_wifi_path[1024];

void player_lyrics_reload(const char *path) {
	device_state_t state;
	device_state_get(&state);
	if (!path || strcmp(state.current_file, path) != 0) {
		return;
	}
	lyrics_path[0] = '\0';
	lyrics_request(&state);
	if (lyrics_state == LYRICS_VIEW_NONE) {
		lyrics_set_mode(LYRICS_VIEW_INTEGRATED, true);
	}
}

static void lyrics_fetched(void *user) {
	lyrics_fetch_t *job = user;
	lyrics_fetching = false;
	toast_busy_end();

	switch (job->result) {
	case LRCLIB_OK: {
		device_state_t state;
		device_state_get(&state);
		if (sheet_open && strcmp(state.current_file, job->path) == 0) {
			if (lyrics_state == LYRICS_VIEW_FULLSCREEN) {
				lyrics_set_mode(LYRICS_VIEW_INTEGRATED, false);
			}
			lyricsearch_open(job->path, job->records, job->count);
			job->records = NULL;
		}
		break;
	}
	case LRCLIB_INSTRUMENTAL:
		toast_plain("lyrics_instrumental");
		break;
	case LRCLIB_NOT_FOUND:
		toast_plain("lyrics_not_found");
		break;
	case LRCLIB_NO_TAGS:
		toast_error("lyrics_no_tags");
		break;
	default:
		toast_error("lyrics_download_failed");
		break;
	}
	lrclib_free(job->records, job->count);
	free(job);
}

static void *lyrics_fetch_worker(void *arg) {
	lyrics_fetch_t *job = arg;
	job->result = lrclib_search(job->title, job->artist, job->album, job->duration, &job->records, &job->count);
	if (!gui_post(lyrics_fetched, job)) {
		lrclib_free(job->records, job->count);
		free(job);
	}
	return NULL;
}

static bool lyrics_wifi_online(void) {
	wifi_status_t wifi;
	wifi_get_status(&wifi);
	return wifi.state == WIFI_STATE_CONNECTED && wifi.ip[0] != '\0';
}

static void lyrics_wifi_wait_end(void) {
	if (lyrics_wifi_timer) {
		lv_timer_delete(lyrics_wifi_timer);
		lyrics_wifi_timer = NULL;
	}
}

static void lyrics_search_run(void);

static void lyrics_wifi_tick_cb(lv_timer_t *t) {
	(void)t;
	device_state_t state;
	device_state_get(&state);
	if (!wifi_get_enabled() || strcmp(state.current_file, lyrics_wifi_path) != 0) {
		lyrics_wifi_wait_end();
		toast_busy_end();
		return;
	}
	if (lyrics_wifi_online()) {
		lyrics_wifi_wait_end();
		toast_busy_end();
		lyrics_search_run();
		return;
	}
	if (lv_tick_elaps(lyrics_wifi_since) >= LYRICS_WIFI_WAIT_MS) {
		lyrics_wifi_wait_end();
		toast_busy_end();
		toast_error("lyrics_wifi_failed");
	}
}

static void lyrics_enable_wifi_cb(void *user) {
	(void)user;
	device_state_t state;
	device_state_get(&state);
	if (!state.current_file[0] || !playing_local_file(&state)) {
		return;
	}
	snprintf(lyrics_wifi_path, sizeof(lyrics_wifi_path), "%s", state.current_file);
	wifi_set_enabled(true);
	lyrics_wifi_since = lv_tick_get();
	if (!lyrics_wifi_timer) {
		lyrics_wifi_timer = lv_timer_create(lyrics_wifi_tick_cb, LYRICS_WIFI_POLL_MS, NULL);
	}
	toast_busy_dismissable("lyrics_wifi_connecting");
}

static bool lyrics_offer_wifi(void) {
	if (lyrics_wifi_timer) {
		toast_busy_dismissable("lyrics_wifi_connecting");
		return true;
	}
	if (wifi_get_enabled() || !wifi_available()) {
		return false;
	}
	confirm_show("lyrics_download_title", "lyrics_wifi_off_note", "lyrics_enable_wifi", lyrics_enable_wifi_cb, NULL);
	return true;
}

static void lyrics_search_run(void) {
	if (lyrics_fetching) {
		toast_busy_dismissable("lyrics_downloading");
		return;
	}
	wifi_status_t wifi;
	wifi_get_status(&wifi);
	if (wifi.state != WIFI_STATE_CONNECTED) {
		toast_error("lyrics_no_wifi");
		return;
	}
	device_state_t state;
	device_state_get(&state);
	if (!state.current_file[0] || !playing_local_file(&state)) {
		return;
	}

	lyrics_fetch_t *job = calloc(1, sizeof(*job));
	if (!job) {
		return;
	}
	snprintf(job->path, sizeof(job->path), "%s", state.current_file);
	snprintf(job->title, sizeof(job->title), "%s", state.metadata.title);
	snprintf(job->artist, sizeof(job->artist), "%s",
			 state.metadata.artist[0] ? state.metadata.artist : state.metadata.album_artist);
	snprintf(job->album, sizeof(job->album), "%s", state.metadata.album);
	job->duration = (int)(state.progress_total_secs + 0.5);

	pthread_t thread;
	if (pthread_create(&thread, NULL, lyrics_fetch_worker, job) != 0) {
		free(job);
		return;
	}
	pthread_detach(thread);
	lyrics_fetching = true;
	toast_busy_dismissable("lyrics_downloading");
}

static void lyrics_search_start(void) {
	if (!lyrics_fetching && lyrics_offer_wifi()) {
		return;
	}
	lyrics_search_run();
}

static void lyrics_find_btn_cb(lv_event_t *e) {
	(void)e;
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	lyrics_search_start();
}

// ---------------------------------------------------------------------------
// Now Playing Refresh & Playback Poll Loop
// ---------------------------------------------------------------------------

static void sync_fs_metadata(const char *title, const char *artist) {
	if (fs_title_label) {
		set_label_text_if_changed(fs_title_label, title ? title : "");
	}
	if (fs_artist_label) {
		set_label_text_if_changed(fs_artist_label, artist ? artist : "");
		lv_obj_set_hidden(fs_artist_label, !(artist && artist[0]));
	}
}

static void refresh_now_playing(void) {
	current_total_length = 0;

	medialist_notify_now_playing();
	browser_notify_now_playing();
	trackmenu_notify_now_playing();

	slider_dragging = false;
	set_progress_label(0, 0);

	device_state_t state;
	device_state_get(&state);

	apply_live_mode(state.live);
	apply_connecting();
	apply_audiobook_mode(!state.live && audiobook_is_playing(),
						 !state.live && podcastcache_is_episode(state.current_file));

	if (state.live) {
		scrolltext_set(song_title_label, state.metadata.title);
		scrolltext_set(song_artist_label, state.metadata.artist);
		sync_fs_metadata(state.metadata.title, state.metadata.artist);
		update_fav_button();
		update_format_label(&state);
		update_service_badges();
		refresh_cover(state.live_cover);
		upnext_refresh();
		update_queue_and_shortcuts();
		notify_state_cbs();
		return;
	}

	const char *file = state.current_file;
	const char *slash = strrchr(file, '/');
	const char *shown_title = state.metadata.title[0] ? state.metadata.title
							  : file[0]               ? (slash ? slash + 1 : file)
													  : tr("player_no_track_loaded");
	const char *artist = metadata_shown_artist(&state.metadata);

	scrolltext_set(song_title_label, shown_title);
	scrolltext_set(song_artist_label, artist ? artist : "");
	sync_fs_metadata(shown_title, artist ? artist : "");

	update_fav_button();
	update_format_label(&state);
	update_service_badges();

	lyrics_request(&state);
	wave_refresh(file, state.progress_current_secs);
	refresh_cover(file);
	upnext_refresh();
	carousel_sync_adjacent_requests();
	update_queue_and_shortcuts();
	notify_state_cbs();
}

static void refresh_live_texts(const device_state_t *state) {
	scrolltext_set(song_title_label, state->metadata.title);
	scrolltext_set(song_artist_label, state->metadata.artist);
	sync_fs_metadata(state->metadata.title, state->metadata.artist);
	update_fav_button();
	update_format_label(state);
	if (strcmp(cover_shown_path, state->live_cover) != 0) {
		refresh_cover(state->live_cover);
	}
	notify_state_cbs();
}

static void handle_track_finished(void) {
	if (dlna_owns_playback()) {
		dlna_notify_finished();
		return;
	}
	char path[512];
	audio_get_current_file(path, sizeof(path));
	if (podcast_stop_at_episode_end() && podcastcache_is_episode(path)) {
		fprintf(stderr, "player: end of the episode; not starting the next\n");
		refresh_now_playing();
		return;
	}
	if (device_state_advance_auto(NULL, 0)) {
		refresh_now_playing();
		carousel_animate_from(carousel_stride() / 3);
	} else {
		trackmenu_notify_now_playing();
	}
}

static void track_end_async(void *user) {
	(void)user;
	if (device_state_take_completion()) {
		handle_track_finished();
	}
}

static void track_end_hook(void) {
	gui_post(track_end_async, NULL);
}

static void update_progress(void) {
	upnext_poll();
	if (device_state_take_completion()) {
		handle_track_finished();
	}

	device_state_t state;
	device_state_get(&state);

	{
		static bool was_live;
		static unsigned last_serial;
		unsigned serial = radio_now_serial();
		if (state.live != was_live) {
			was_live = state.live;
			last_serial = serial;
			refresh_now_playing();
			device_state_get(&state);
		} else if (state.live && serial != last_serial) {
			last_serial = serial;
			refresh_live_texts(&state);
		}
	}

	apply_live_mode(state.live);
	apply_connecting();

	if (state.progress_total_secs > 0) {
		current_total_length = state.progress_total_secs;
	}

	progress_anchor_secs = state.progress_current_secs;
	progress_anchor_tick = lv_tick_get();
	progress_running = (state.status == AUDIO_STATUS_PLAYING && state.progress_total_secs > 0);
	smooth_timer_sync();

	set_progress_label(state.progress_current_secs, current_total_length);
	wave_refresh(state.current_file, state.progress_current_secs);

	if (state.status == AUDIO_STATUS_PLAYING && !state.live) {
		device_state_read_ahead(state.progress_current_secs, state.progress_total_secs);
	}
	lyrics_request(&state);
	lyrics_follow(state.progress_current_secs);
	apply_playback_status(state.status);

	bool book = !state.live && audiobook_is_playing();
	bool episode = !state.live && podcastcache_is_episode(state.current_file);
	apply_audiobook_mode(book, episode);
	update_format_label(&state);

	led_update_playback(state.status == AUDIO_STATUS_PLAYING, state.stream_sample_rate, episode,
						!state.live && audio_get_dsd_multiple() > 0);

	if (!state.live && audiobook_mode) {
		bool stopped_itself = audiobook_auto_stop(&state);
		static audio_status_t book_last_status = AUDIO_STATUS_STOPPED;
		bool changed = (state.status != book_last_status);
		book_last_status = state.status;
		if (!stopped_itself) {
			audiobook_note_position(state.progress_current_secs, state.progress_total_secs,
									changed || state.status != AUDIO_STATUS_PLAYING);
		}
	}

	if (episode) {
		static audio_status_t episode_last_status = AUDIO_STATUS_STOPPED;
		bool episode_changed = (state.status != episode_last_status);
		episode_last_status = state.status;
		podcaststate_note(state.current_file, state.progress_current_secs, state.progress_total_secs, episode_changed);
	}

	device_state_remember_note();

	// Reconcile queue / shuffle / chaining mutations from outside Now Playing
	{
		static unsigned last_rev;
		static playback_mode_t last_mode = (playback_mode_t)-1;
		static int last_idx = -2;
		static bool last_achain;
		static bool last_fchain;
		static bool last_book;
		static bool last_pod_stop;

		unsigned rev = playlist_revision();
		playback_mode_t mode = playlist_get_mode();
		int idx = playlist_current_index();
		bool achain = device_state_album_chaining();
		bool fchain = device_state_folder_chaining();
		bool pod_stop = podcast_stop_at_episode_end();

		if (rev != last_rev || mode != last_mode || idx != last_idx || achain != last_achain ||
			fchain != last_fchain || book != last_book || pod_stop != last_pod_stop) {
			last_rev = rev;
			last_mode = mode;
			last_idx = idx;
			last_achain = achain;
			last_fchain = fchain;
			last_book = book;
			last_pod_stop = pod_stop;
			update_transport_modes();
			upnext_refresh();
			carousel_sync_adjacent_requests();
			update_queue_and_shortcuts();
		}
	}
}

static void smooth_timer_cb(lv_timer_t *timer) {
	(void)timer;
	if (!progress_running || view_length <= 0.1 || slider_dragging || !sheet_open || !power_screen_is_on()) {
		return;
	}
	double seconds = progress_anchor_secs + (double)lv_tick_elaps(progress_anchor_tick) / 1000.0;
	if (seconds > view_start + view_length) {
		seconds = view_start + view_length;
	}
	int value = view_value(seconds);
	if (progress_slider && value != lv_slider_get_value(progress_slider)) {
		lv_slider_set_value(progress_slider, value, LV_ANIM_OFF);
	}
	if (fs_slider && value != lv_slider_get_value(fs_slider)) {
		lv_slider_set_value(fs_slider, value, LV_ANIM_OFF);
	}
	wave_preview(seconds);
	lyrics_follow(seconds);
}

static void progress_slider_timer_cb(lv_timer_t *timer) {
	(void)timer;
	apply_cover_id_result();
	apply_cover_result();
	carousel_poll_adjacent();
	update_progress();
}

static void sleep_timer_cb(lv_timer_t *timer) {
	(void)timer;
	bool expired = false;
	for (int i = 0; i < SLEEPTIMER_COUNT; i++) {
		expired = sleeptimer_expired((sleeptimer_kind_t)i) || expired;
	}
	if (!expired || audio_get_status() != AUDIO_STATUS_PLAYING) {
		return;
	}
	if (audiobook_is_playing()) {
		device_state_t state;
		device_state_get(&state);
		audiobook_suppress_rewind_once();
		audio_pause();
		apply_playback_status(AUDIO_STATUS_PAUSED);
		audiobook_note_position(state.progress_current_secs, 0, true);
		return;
	}
	audio_pause();
	apply_playback_status(AUDIO_STATUS_PAUSED);
}

// ---------------------------------------------------------------------------
// Transport Actions & Hardware Keys
// ---------------------------------------------------------------------------

static bool live_step(int step) {
	if (!radio_is_active()) {
		return false;
	}
	if (radio_is_connecting()) {
		return true;
	}
	if (radio_step(step)) {
		refresh_now_playing();
		carousel_animate_from(step > 0 ? carousel_stride() / 2 : -carousel_stride() / 2);
	}
	return true;
}

static void play_btn_event_cb(lv_event_t *e) {
	(void)e;
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	if (radio_is_active()) {
		if (radio_is_connecting()) return;
		if (radio_is_playing()) {
			radio_stop();
		} else {
			radio_resume();
		}
		player_refresh_now_playing();
		return;
	}
	audio_status_t new_status = device_state_toggle_play_pause();
	apply_playback_status(new_status);
}

void player_screen_prev(void) {
	if (live_step(-1)) {
		return;
	}
	device_state_t state;
	device_state_get(&state);

	if (audiobook_mode || podcast_mode) {
		int back = (podcast_mode && !audiobook_mode) ? podcast_skip_back() : audiobook_skip_back();
		audiobook_seek_by(-(double)back, &state);
		update_progress();
		return;
	}

	if (state.progress_current_secs > 3.0) {
		device_state_seek(0);
		lyrics_centred = false;
		lyrics_follow(0);
	} else if (device_state_prev(NULL, 0)) {
		refresh_now_playing();
		carousel_animate_from(-carousel_stride() / 2);
	} else {
		device_state_seek(0);
	}
	update_progress();
}

void player_screen_next(void) {
	if (live_step(1)) {
		return;
	}
	if (audiobook_mode || podcast_mode) {
		device_state_t state;
		device_state_get(&state);
		int fwd = (podcast_mode && !audiobook_mode) ? podcast_skip_forward() : audiobook_skip_forward();
		audiobook_seek_by((double)fwd, &state);
		update_progress();
		return;
	}
	if (device_state_next(NULL, 0)) {
		refresh_now_playing();
		carousel_animate_from(carousel_stride() / 2);
	}
	update_progress();
}

static void prev_btn_event_cb(lv_event_t *e) {
	(void)e;
	if (player_sheet_drag_active() || switcher_back_drag_active()) return;
	player_screen_prev();
}

static void next_btn_event_cb(lv_event_t *e) {
	(void)e;
	if (player_sheet_drag_active() || switcher_back_drag_active()) return;
	player_screen_next();
}

void player_key_play_pause(void) {
	apply_playback_status(device_state_toggle_play_pause());
}

void player_key_next(void) {
	if (live_step(1)) return;
	if (dlna_owns_playback()) {
		dlna_request_next();
		return;
	}
	if (audiobook_mode) {
		device_state_t state;
		device_state_get(&state);
		audiobook_seek_by((double)audiobook_skip_forward(), &state);
		update_progress();
		return;
	}
	if (device_state_next(NULL, 0)) {
		refresh_now_playing();
		carousel_animate_from(carousel_stride() / 2);
	}
	update_progress();
}

void player_key_prev(void) {
	if (live_step(-1)) return;
	device_state_t state;
	device_state_get(&state);

	if (dlna_owns_playback()) {
		if (state.progress_current_secs > 3.0) {
			device_state_seek(0);
		} else {
			dlna_request_prev();
		}
		return;
	}
	if (audiobook_mode) {
		audiobook_seek_by(-(double)audiobook_skip_back(), &state);
		update_progress();
		return;
	}
	if (state.progress_current_secs > 3.0) {
		device_state_seek(0);
		lyrics_centred = false;
		lyrics_follow(0);
	} else if (device_state_prev(NULL, 0)) {
		refresh_now_playing();
		carousel_animate_from(-carousel_stride() / 2);
	} else {
		device_state_seek(0);
	}
	update_progress();
}

static void book_chapters_action(void *user) {
	(void)user;
	if (audiobook_has_chapters) {
		chapters_open();
	} else {
		gui_notify_popup("player_this_audiobook_has_no_chapters_2");
	}
}

static void book_add_bookmark_action(void *user) {
	(void)user;
	audiobookextras_add_bookmark();
}

static void book_bookmarks_action(void *user) {
	(void)user;
	audiobookextras_open_current_bookmarks();
}

static void book_summary_action(void *user) {
	(void)user;
	audiobookextras_open_current_summary();
}

static void more_btn_event_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	if (audiobook_mode) {
		static const popover_item_t BOOK_ITEMS[] = {
			{"chapters", book_chapters_action, NULL},
			{"audiobook_add_bookmark", book_add_bookmark_action, NULL},
			{"bookmarks", book_bookmarks_action, NULL},
			{"audiobook_summary", book_summary_action, NULL},
		};
		popover_show(lv_event_get_current_target(e), BOOK_ITEMS, (int)(sizeof(BOOK_ITEMS) / sizeof(BOOK_ITEMS[0])));
		return;
	}
	if (podcast_mode) {
		trackmenu_open_queue();
		return;
	}
	trackmenu_open(lv_event_get_current_target(e));
}

static void open_queue_btn_cb(lv_event_t *e) {
	(void)e;
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	trackmenu_open_queue();
}

// ---------------------------------------------------------------------------
// Public Entry Points & Preferences API
// ---------------------------------------------------------------------------

void player_restore_track(const char *filepath, double position) {
	device_state_restore_file(filepath, position);
	player_refresh_now_playing();
}

void player_restore_index(library_index_t *ix, int start_index, const char *const *extra, const int *extra_slots,
						  int extra_count, const char *filepath, double position) {
	device_state_restore_index(ix, start_index, extra, extra_slots, extra_count, filepath, position);
	player_refresh_now_playing();
}

void player_restore_list(const char *const *list, int count, int start_index, bool custom, const char *filepath,
						 double position) {
	device_state_restore_list(list, count, start_index, custom, filepath, position);
	player_refresh_now_playing();
}

void player_play_file(const char *filepath) {
	device_state_play_file(filepath);
	player_refresh_now_playing();
}

void player_refresh_artist(void) {
	if (!song_artist_label) return;
	device_state_t state;
	device_state_get(&state);
	if (state.live || !state.current_file[0]) return;
	const char *artist = metadata_shown_artist(&state.metadata);
	scrolltext_set(song_artist_label, artist ? artist : "");
	sync_fs_metadata(lv_label_get_text(song_title_label), artist ? artist : "");
}

void player_refresh_now_playing(void) {
	refresh_now_playing();
	update_transport_modes();
	device_state_t state;
	device_state_get(&state);
	apply_playback_status(state.status);
}

void player_output_unplugged(const char *what) {
	if (audio_get_status() != AUDIO_STATUS_PLAYING) return;
	printf("player: %s unplugged; pausing\n", what ? what : "the output");
	audio_pause();
	apply_playback_status(AUDIO_STATUS_PAUSED);
}

player_layout_t player_layout_get(void) { return layout_choice; }

void player_layout_set(player_layout_t layout) {
	layout_choice = layout;
	config_set_int("screen", "player_layout_alt", (int)layout);
	config_save();
}

bool player_cover_accent(void) { return adaptive_accent; }

void player_cover_accent_set(bool on) {
	if (on == adaptive_accent) return;
	adaptive_accent = on;
	config_set_int("ambient", "accent_mode", on ? 1 : 0);
	config_set_bool("screen", "cover_accent", on);
	config_save();
	recompute_palette(true);
}

player_lyrics_mode_t player_lyrics_default_mode(void) { return lyrics_default_mode; }

void player_lyrics_set_default_mode(player_lyrics_mode_t mode) {
	lyrics_default_mode = (mode == PLAYER_LYRICS_FULLSCREEN) ? PLAYER_LYRICS_FULLSCREEN : PLAYER_LYRICS_INTEGRATED;
	config_set_int("lyrics", "default_mode", (int)lyrics_default_mode);
	config_save();
}

bool player_lyrics_remember_mode(void) { return lyrics_remember_mode; }

void player_lyrics_set_remember_mode(bool on) {
	lyrics_remember_mode = on;
	config_set_bool("lyrics", "remember_mode", on);
	config_save();
}

player_lyrics_text_size_t player_lyrics_text_size(void) { return lyrics_text_size; }

void player_lyrics_set_text_size(player_lyrics_text_size_t size) {
	if ((int)size < 0 || (int)size > 2) {
		size = PLAYER_LYRICS_TEXT_MEDIUM;
	}
	if (lyrics_text_size == size) return;
	lyrics_text_size = size;
	config_set_int("lyrics", "text_size", (int)size);
	config_save();
	lyrics_update_padding();
	lyrics_apply_line_styles();
	lyrics_centred = false;
	device_state_t st;
	device_state_get(&st);
	lyrics_follow(st.progress_current_secs);
}

int player_lyrics_offset_ms(void) { return lyrics_offset_ms; }

void player_lyrics_set_offset_ms(int offset_ms) {
	if (offset_ms < -LYRICS_OFFSET_LIMIT_MS) offset_ms = -LYRICS_OFFSET_LIMIT_MS;
	if (offset_ms > LYRICS_OFFSET_LIMIT_MS) offset_ms = LYRICS_OFFSET_LIMIT_MS;
	if (lyrics_offset_ms == offset_ms) return;
	lyrics_offset_ms = offset_ms;
	config_set_int("lyrics", "offset_ms", offset_ms);
	config_save();
	update_fs_offset_labels();
	lyrics_centred = false;
	device_state_t st;
	device_state_get(&st);
	lyrics_follow(st.progress_current_secs);
}

void player_register_state_cb(player_state_cb_t cb) {
	if (!cb || state_cb_count >= MAX_PLAYER_STATE_CBS) return;
	for (int i = 0; i < state_cb_count; i++) {
		if (state_cbs[i] == cb) return;
	}
	state_cbs[state_cb_count++] = cb;
}

static void notify_state_cbs(void) {
	for (int i = 0; i < state_cb_count; i++) {
		state_cbs[i]();
	}
}

// ---------------------------------------------------------------------------
// Sheet Open / Close & Status Bar Integration
// ---------------------------------------------------------------------------

#define SHEET_COMMIT_FRACTION 3
#define DRAG_COMMIT_PX 10

static void bar_over_player_update(void) {
	if (!sheet_open) {
		return;
	}
	if (bar_always) {
		topbar_set_hidden(false);
		// Transparent over our dark ambient surface, crisp white glyphs, zero software blur.
		topbar_set_over_artwork(true, false);
	} else {
		topbar_set_over_artwork(false, false);
		topbar_set_hidden(true);
	}
}

bool player_statusbar_always(void) { return bar_always; }

static void relayout_screen_vertical(void) {
	if (!header_row || !stage_container) {
		return;
	}
	gui_config_t cfg_snap = {
		.screen_width = (uint32_t)screen_w,
		.screen_height = (uint32_t)screen_h,
		.padding = 15,
		.top_bar_height = (int8_t)bar_height,
		.sd_root_path = NULL,
	};
	compute_geometry(&cfg_snap);

	int cy = back_btn_centre_y();
	lv_obj_set_pos(header_row, 0, cy - header_h / 2);
	lv_obj_set_size(header_row, screen_w, header_h);

	stage_y = cy + header_h / 2 + 4;
	lv_obj_set_pos(stage_container, 0, stage_y);
	lv_obj_set_size(stage_container, screen_w, stage_h);
	lv_obj_set_size(carousel_stage, screen_w, stage_h);
	lv_obj_set_pos(integrated_lyrics_box, side_margin, 0);
	lv_obj_set_size(integrated_lyrics_box, content_w, stage_h);

	int y = stage_y + stage_h + (is_tall_panel ? 8 : 6);
	int info_h = is_tall_panel ? 74 : 66;
	lv_obj_set_pos(track_info_row, side_margin, y);
	lv_obj_set_size(track_info_row, content_w, info_h);
	y += info_h + (is_tall_panel ? 4 : 2);

	int wsec_h = wave_h + 26;
	lv_obj_set_pos(wave_section, side_margin, y);
	lv_obj_set_size(wave_section, content_w, wsec_h);
	y += wsec_h + (is_tall_panel ? 6 : 4);

	int ctrl_h = is_tall_panel ? 76 : 68;
	lv_obj_set_pos(controls_row, side_margin, y);
	lv_obj_set_size(controls_row, content_w, ctrl_h);
	y += ctrl_h + (is_tall_panel ? 4 : 2);

	int foot_h = screen_h - y - 4;
	if (foot_h < 38) foot_h = 38;
	if (foot_h > 50) foot_h = 50;
	lv_obj_set_pos(shortcuts_row, side_margin, screen_h - foot_h - 4);
	lv_obj_set_size(shortcuts_row, content_w, foot_h);

	slot_apply_geometry(&cslots[0], peek_box);
	slot_apply_geometry(&cslots[1], hero_box);
	slot_apply_geometry(&cslots[2], peek_box);
	carousel_layout_slots();
	layout_fs_stage();
	if (lyrics_state == LYRICS_VIEW_INTEGRATED) {
		lyrics_reparent_shared_stage(integrated_lyrics_box, content_w - 24, stage_h, false);
		if (int_lyrics_search_btn) lv_obj_move_foreground(int_lyrics_search_btn);
		if (int_lyrics_expand_btn) lv_obj_move_foreground(int_lyrics_expand_btn);
	}
}

void player_set_statusbar_always(bool on) {
	if (on == bar_always) {
		return;
	}
	bar_always = on;
	back_btn_player_below_bar(on);
	relayout_screen_vertical();
	bar_over_player_update();
}

static void sheet_anim_x_cb(void *obj, int32_t value) {
	lv_obj_set_x((lv_obj_t *)obj, (int)value);
	back_btn_translate(value >= sheet_width ? 0 : value);
}

static void sheet_move_to(int x, bool animate) {
	if (x < 0) x = 0;
	if (x > sheet_width) x = sheet_width;
	lv_anim_delete(player_screen, sheet_anim_x_cb);

	if (!animate || theme_reduced_motion()) {
		lv_obj_set_x(player_screen, x);
		back_btn_translate(x >= sheet_width ? 0 : x);
		return;
	}

	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, player_screen);
	lv_anim_set_exec_cb(&a, sheet_anim_x_cb);
	lv_anim_set_values(&a, lv_obj_get_x(player_screen), x);
	lv_anim_set_duration(&a, SHEET_ANIM_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_start(&a);
}

bool player_sheet_is_open(void) { return sheet_open; }
bool player_sheet_drag_active(void) { return drag_committed; }

void player_sheet_open(bool animate) {
	if (btreceiver_is_active()) {
		return;
	}
	if (!sheet_open) {
		sheet_under = lv_screen_active();
	}
	sheet_open = true;
	smooth_timer_sync();
	back_btn_player_mode(true);
	back_btn_over_cover(true);
	if (lyrics_state == LYRICS_VIEW_FULLSCREEN) {
		back_btn_force_hidden(true);
	}
	lyrics_screen_hold_update();
	upnext_refresh();
	carousel_sync_adjacent_requests();
	update_queue_and_shortcuts();

	lv_obj_set_hidden(player_screen, false);
	lv_obj_move_background(player_screen);
	bar_over_player_update();
	sheet_move_to(0, animate);
}

static void sheet_hidden_anim_ready_cb(lv_anim_t *a) {
	(void)a;
	if (!sheet_open) {
		lv_obj_set_hidden(player_screen, true);
	}
	back_btn_sync_visibility();
}

void player_sheet_close(bool animate) {
	lv_anim_delete(&carousel_anim_tag, carousel_offset_exec_cb);
	lv_anim_delete(&pal_anim_tag, palette_anim_exec_cb);
	carousel_offset_px = 0;

	bool was_open = sheet_open;
	sheet_open = false;
	smooth_timer_sync();
	back_btn_force_hidden(false);
	back_btn_over_cover(false);
	back_btn_player_mode(false);
	lyrics_screen_hold_update();
	topbar_set_over_artwork(false, false);
	topbar_set_hidden(false);

	if (was_open && sheet_under && lv_screen_active() != sheet_under) {
		switch_screen_no_history(sheet_under);
	}
	medialist_refresh_visible();

	if (!animate || theme_reduced_motion()) {
		lv_obj_set_x(player_screen, sheet_width);
		back_btn_translate(0);
		lv_obj_set_hidden(player_screen, true);
		back_btn_sync_visibility();
		return;
	}

	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, player_screen);
	lv_anim_set_exec_cb(&a, sheet_anim_x_cb);
	lv_anim_set_values(&a, lv_obj_get_x(player_screen), sheet_width);
	lv_anim_set_duration(&a, SHEET_ANIM_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_completed_cb(&a, sheet_hidden_anim_ready_cb);
	lv_anim_start(&a);
}

static void drag_committed_clear_cb(void *unused) {
	(void)unused;
	drag_committed = false;
}

static void sheet_drag_cb(lv_event_t *e) {
	bool opening = (bool)(uintptr_t)lv_event_get_user_data(e);
	lv_event_code_t code = lv_event_get_code(e);
	lv_indev_t *indev = lv_indev_active();
	if (!indev) return;

	if (code == LV_EVENT_PRESSED) {
		if (opening == sheet_open) return;
		lv_indev_get_point(indev, &drag_start_point);
		drag_origin_x = opening ? sheet_width : 0;
		dragging = true;
		drag_committed = false;
		if (opening) {
			lv_obj_set_hidden(player_screen, false);
			lv_obj_move_background(player_screen);
			lv_obj_set_x(player_screen, sheet_width);
		}
		return;
	}
	if (!dragging) return;

	lv_point_t point;
	lv_indev_get_point(indev, &point);
	int dx = point.x - drag_start_point.x;

	if (code == LV_EVENT_PRESSING) {
		if (!drag_committed) {
			if (lv_indev_get_scroll_obj(indev)) {
				dragging = false;
				return;
			}
			int dy = point.y - drag_start_point.y;
			if (LV_ABS(dy) >= DRAG_COMMIT_PX && LV_ABS(dy) > LV_ABS(dx)) {
				dragging = false;
				return;
			}
			if (LV_ABS(dx) < DRAG_COMMIT_PX || LV_ABS(dx) < 2 * LV_ABS(dy)) {
				return;
			}
			drag_committed = true;
		}
		sheet_move_to(drag_origin_x + dx, false);
		return;
	}

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		dragging = false;
		int travelled = opening ? -dx : dx;
		bool commit = travelled > (sheet_width / SHEET_COMMIT_FRACTION);
		if (opening) {
			commit ? player_sheet_open(true) : player_sheet_close(true);
		} else {
			commit ? player_sheet_close(true) : player_sheet_open(true);
		}
		lv_async_call(drag_committed_clear_cb, NULL);
	}
}

void player_sheet_attach_drag(lv_obj_t *obj, bool opening) {
	if (!obj) return;
	lv_obj_set_clickable(obj, true);
	lv_obj_add_event_cb(obj, sheet_drag_cb, LV_EVENT_PRESSED, (void *)(uintptr_t)opening);
	lv_obj_add_event_cb(obj, sheet_drag_cb, LV_EVENT_PRESSING, (void *)(uintptr_t)opening);
	lv_obj_add_event_cb(obj, sheet_drag_cb, LV_EVENT_RELEASED, (void *)(uintptr_t)opening);
	lv_obj_add_event_cb(obj, sheet_drag_cb, LV_EVENT_PRESS_LOST, (void *)(uintptr_t)opening);
}

// Control centre pull-down when statusbar_always is off
static void panel_edge_drag_cb(lv_event_t *e) {
	static lv_point_t start;
	static bool tracking;
	static bool engaged;

	lv_indev_t *indev = lv_indev_active();
	if (!indev) return;

	lv_event_code_t code = lv_event_get_code(e);
	if (code == LV_EVENT_PRESSED) {
		lv_indev_get_point(indev, &start);
		tracking = !quickpanel_is_open();
		engaged = false;
		return;
	}
	if (!tracking) return;

	lv_point_t p;
	lv_indev_get_point(indev, &p);
	int dy = p.y - start.y;
	int dx = p.x - start.x;

	if (code == LV_EVENT_PRESSING) {
		if (!engaged) {
			int adx = dx < 0 ? -dx : dx;
			if (dy < 8 || dy <= adx) return;
			engaged = true;
			quickpanel_drag_begin();
		}
		quickpanel_drag_update(dy);
		return;
	}
	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		tracking = false;
		if (engaged) {
			engaged = false;
			quickpanel_drag_end();
		}
	}
}

static void player_refresh_theme(void) {
	recompute_palette(false);
}

static void player_on_font_change(void) {
	lyrics_update_padding();
	lyrics_apply_line_styles();
}

// ---------------------------------------------------------------------------
// Screen Construction (`player_init`)
// ---------------------------------------------------------------------------

static lv_obj_t *make_icon_btn(lv_obj_t *parent, int w, int h, const lv_image_dsc_t *src, lv_event_cb_t cb,
							   void *user_data, lv_obj_t **icon_out) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_remove_style_all(btn);
	lv_obj_set_size(btn, w, h);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(btn, lv_color_white(), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(btn, LV_OPA_10, LV_STATE_PRESSED);
	lv_obj_set_style_opa(btn, LV_OPA_40, LV_STATE_DISABLED);
	if (cb) {
		lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
	}
	lv_obj_t *ic = lv_image_create(btn);
	if (src) {
		lv_image_set_src(ic, src);
	}
	lv_obj_center(ic);
	lv_obj_set_clickable(ic, false);
	lv_obj_set_event_bubble(ic, true);
	if (icon_out) {
		*icon_out = ic;
	}
	return btn;
}

void player_init(gui_config_t *cfg) {
	coverloader_start();
	readahead_set_picture_known(cover_picture_known);
	upnext_init(on_upnext_resolved);

	// Load persisted preferences with backward-compatible defaults
	bar_height = (int)cfg->top_bar_height;
	bar_always = config_get_int("screen", "statusbar_always", 1) != 0;
	back_btn_player_below_bar(bar_always);

	int saved_layout = (int)config_get_int("screen", "player_layout_alt", 0);
	layout_choice = (saved_layout == (int)PLAYER_LAYOUT_ALTERNATIVE) ? PLAYER_LAYOUT_ALTERNATIVE
					: (saved_layout == (int)PLAYER_LAYOUT_STUDIO)    ? PLAYER_LAYOUT_STUDIO
																	 : PLAYER_LAYOUT_STANDARD;

	int saved_accent_mode = (int)config_get_int("ambient", "accent_mode", -1);
	if (saved_accent_mode < 0) {
		// Default to Adaptive in Sonix Atelier Ambient Edition unless explicitly set false
		const char *legacy = config_get("screen", "cover_accent", NULL);
		adaptive_accent = legacy ? config_get_bool("screen", "cover_accent", true) : true;
	} else {
		adaptive_accent = (saved_accent_mode != 0);
	}

	lyrics_default_mode = config_get_int("lyrics", "default_mode", 0) == 1 ? PLAYER_LYRICS_FULLSCREEN
																		   : PLAYER_LYRICS_INTEGRATED;
	lyrics_remember_mode = config_get_bool("lyrics", "remember_mode", true);
	lyrics_last_mode = config_get_int("lyrics", "last_mode", (int)lyrics_default_mode) == 1 ? PLAYER_LYRICS_FULLSCREEN
																							: PLAYER_LYRICS_INTEGRATED;
	int saved_lsize = (int)config_get_int("lyrics", "text_size", 1);
	lyrics_text_size = (saved_lsize >= 0 && saved_lsize <= 2) ? (player_lyrics_text_size_t)saved_lsize
															  : PLAYER_LYRICS_TEXT_MEDIUM;
	lyrics_offset_ms = (int)config_get_int("lyrics", "offset_ms", 0);
	if (lyrics_offset_ms < -LYRICS_OFFSET_LIMIT_MS) lyrics_offset_ms = -LYRICS_OFFSET_LIMIT_MS;
	if (lyrics_offset_ms > LYRICS_OFFSET_LIMIT_MS) lyrics_offset_ms = LYRICS_OFFSET_LIMIT_MS;

	playback_mode_t init_mode = playlist_get_mode();
	shuffle_remembered = (init_mode == PLAYBACK_MODE_SHUFFLE || init_mode == PLAYBACK_MODE_SHUFFLE_REPEAT);

	compute_geometry(cfg);
	compute_target_palette(&pal_cur);
	pal_target = pal_cur;

	// Root sheet setup
	sheet_width = screen_w;
	lv_obj_remove_style_all(player_screen);
	lv_obj_set_size(player_screen, screen_w, screen_h);
	lv_obj_set_pos(player_screen, sheet_width, 0);
	lv_obj_set_scrollable(player_screen, false);
	lv_obj_set_hidden(player_screen, true);

	// -----------------------------------------------------------------------
	// 1. Minimal Navigation Header
	// -----------------------------------------------------------------------
	header_row = lv_obj_create(player_screen);
	style_clean_box(header_row);
	lv_obj_set_ignore_layout(header_row, true);

	header_title_label = lv_label_create(header_row);
	lv_label_set_text(header_title_label, tr("miniplayer_playing"));
	lv_obj_set_style_text_font(header_title_label, &font_ui_16, 0);
	lv_obj_set_style_text_letter_space(header_title_label, 1, 0);
	lv_obj_center(header_title_label);

	more_btn_obj = make_icon_btn(header_row, 44, 40, &icon_ellipsis_vertical, more_btn_event_cb, NULL, &more_btn_icon);
	lv_obj_align(more_btn_obj, LV_ALIGN_RIGHT_MID, -15, 0);
	lv_obj_set_ext_click_area(more_btn_obj, 12);

	// -----------------------------------------------------------------------
	// 2. Center Stage: 3-Slot Carousel & Mode A Integrated Lyrics
	// -----------------------------------------------------------------------
	stage_container = lv_obj_create(player_screen);
	style_clean_box(stage_container);
	lv_obj_set_ignore_layout(stage_container, true);

	carousel_stage = lv_obj_create(stage_container);
	style_clean_box(carousel_stage);
	lv_obj_set_size(carousel_stage, screen_w, stage_h);
	lv_obj_set_pos(carousel_stage, 0, 0);
	lv_obj_set_clickable(carousel_stage, true);
	lv_obj_add_event_cb(carousel_stage, carousel_drag_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(carousel_stage, carousel_drag_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(carousel_stage, carousel_drag_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(carousel_stage, carousel_drag_cb, LV_EVENT_PRESS_LOST, NULL);

	// Create prev (0), next (2), then hero (1) last so hero stays visually on top
	static const int CREATE_ORDER[3] = {0, 2, 1};
	for (int k = 0; k < 3; k++) {
		int idx = CREATE_ORDER[k];
		carousel_slot_t *s = &cslots[idx];
		int box = (idx == 1) ? hero_box : peek_box;
		int rad = (idx == 1) ? 16 : 14;

		s->frame = lv_obj_create(carousel_stage);
		style_clean_box(s->frame);
		lv_obj_set_ignore_layout(s->frame, true);
		lv_obj_set_size(s->frame, box, box);
		lv_obj_set_style_radius(s->frame, rad, 0);
		lv_obj_set_style_clip_corner(s->frame, true, 0);
		lv_obj_set_style_bg_opa(s->frame, LV_OPA_COVER, 0);
		lv_obj_set_event_bubble(s->frame, true);
		if (idx != 1) {
			lv_obj_set_hidden(s->frame, true);
		}

		s->placeholder = lv_image_create(s->frame);
		lv_obj_set_clickable(s->placeholder, false);
		lv_obj_set_event_bubble(s->placeholder, true);

		s->img = lv_image_create(s->frame);
		lv_obj_set_clickable(s->img, false);
		lv_obj_set_event_bubble(s->img, true);
		lv_obj_set_hidden(s->img, true);
	}
	refresh_cover_placeholders();

	// Service badges inside hero frame
	live_badge = lv_label_create(cslots[1].frame);
	lv_label_set_text(live_badge, tr("player_live"));
	lv_obj_set_style_text_font(live_badge, &font_ui_14, 0);
	lv_obj_set_style_text_color(live_badge, lv_color_white(), 0);
	lv_obj_set_style_bg_color(live_badge, lv_color_make(224, 27, 36), 0);
	lv_obj_set_style_bg_opa(live_badge, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(live_badge, 6, 0);
	lv_obj_set_style_pad_hor(live_badge, 8, 0);
	lv_obj_set_style_pad_ver(live_badge, 4, 0);
	lv_obj_set_hidden(live_badge, true);

	qobuz_badge = lv_image_create(cslots[1].frame);
	lv_image_set_src(qobuz_badge, &icon_qobuz_badge);
	lv_obj_set_hidden(qobuz_badge, true);

	tidal_badge = lv_image_create(cslots[1].frame);
	lv_image_set_src(tidal_badge, &icon_tidal_badge);
	lv_obj_set_hidden(tidal_badge, true);

	podcast_badge = lv_image_create(cslots[1].frame);
	lv_image_set_src(podcast_badge, &icon_podcast_badge);
	lv_obj_set_hidden(podcast_badge, true);

	// Unobtrusive Drag Up-Next / Previous floating caption
	drag_caption_pill = lv_obj_create(carousel_stage);
	style_clean_box(drag_caption_pill);
	lv_obj_set_size(drag_caption_pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_max_width(drag_caption_pill, content_w - 20, 0);
	lv_obj_set_style_radius(drag_caption_pill, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(drag_caption_pill, 224, 0);
	lv_obj_set_style_border_width(drag_caption_pill, 1, 0);
	lv_obj_set_style_border_opa(drag_caption_pill, LV_OPA_60, 0);
	lv_obj_set_style_pad_hor(drag_caption_pill, 14, 0);
	lv_obj_set_style_pad_ver(drag_caption_pill, 6, 0);
	lv_obj_align(drag_caption_pill, LV_ALIGN_BOTTOM_MID, 0, -10);
	lv_obj_set_hidden(drag_caption_pill, true);

	drag_caption_label = lv_label_create(drag_caption_pill);
	lv_label_set_text(drag_caption_label, "");
	lv_label_set_long_mode(drag_caption_label, LV_LABEL_LONG_DOT);
	lv_obj_set_style_max_width(drag_caption_label, content_w - 48, 0);
	lv_obj_set_style_text_font(drag_caption_label, &font_ui_14, 0);

	// Mode A: Integrated Lyrics Stage (same vertical bounds as artwork)
	integrated_lyrics_box = lv_obj_create(stage_container);
	style_clean_box(integrated_lyrics_box);
	lv_obj_set_pos(integrated_lyrics_box, side_margin, 0);
	lv_obj_set_size(integrated_lyrics_box, content_w, stage_h);
	lv_obj_set_style_radius(integrated_lyrics_box, 16, 0);
	lv_obj_set_style_bg_opa(integrated_lyrics_box, 115, 0);
	lv_obj_set_style_border_width(integrated_lyrics_box, 1, 0);
	lv_obj_set_style_border_opa(integrated_lyrics_box, LV_OPA_40, 0);
	lv_obj_set_style_pad_hor(integrated_lyrics_box, 12, 0);
	lv_obj_set_style_clip_corner(integrated_lyrics_box, true, 0);
	lv_obj_set_hidden(integrated_lyrics_box, true);

	int_lyrics_expand_btn =
		make_icon_btn(integrated_lyrics_box, 38, 36, &icon_expand, lyrics_expand_cb, NULL, &int_lyrics_expand_icon);
	lv_obj_align(int_lyrics_expand_btn, LV_ALIGN_TOP_RIGHT, 4, 4);
	lv_obj_set_ext_click_area(int_lyrics_expand_btn, 8);

	int_lyrics_search_btn = make_icon_btn(integrated_lyrics_box, 38, 36, &icon_lyrics_search, lyrics_find_btn_cb, NULL,
										  &int_lyrics_search_icon);
	lv_obj_align(int_lyrics_search_btn, LV_ALIGN_TOP_RIGHT, -38, 4);
	lv_obj_set_ext_click_area(int_lyrics_search_btn, 6);

	// Shared Lyrics Scroll Viewport + Empty State + Resume Sync Button
	lyrics_view = lv_obj_create(integrated_lyrics_box);
	lv_obj_remove_style_all(lyrics_view);
	lv_obj_set_ignore_layout(lyrics_view, true);
	lv_obj_set_flex_flow(lyrics_view, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(lyrics_view, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_hor(lyrics_view, 4, 0);
	lv_obj_set_scroll_dir(lyrics_view, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(lyrics_view, LV_SCROLLBAR_MODE_OFF);
	lv_obj_add_event_cb(lyrics_view, lyrics_scroll_cb, LV_EVENT_SCROLL_BEGIN, NULL);
	lv_obj_add_event_cb(lyrics_view, lyrics_tap_cb, LV_EVENT_CLICKED, NULL);

	lyrics_empty_box = lv_obj_create(integrated_lyrics_box);
	style_clean_box(lyrics_empty_box);
	lv_obj_set_size(lyrics_empty_box, content_w - 40, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(lyrics_empty_box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(lyrics_empty_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(lyrics_empty_box, 12, 0);
	lv_obj_center(lyrics_empty_box);

	lyrics_empty_icon = lv_image_create(lyrics_empty_box);
	lv_image_set_src(lyrics_empty_icon, &icon_lyrics_empty);

	lyrics_empty_label = lv_label_create(lyrics_empty_box);
	lv_label_set_text(lyrics_empty_label, tr("lyrics_none"));
	lv_label_set_long_mode(lyrics_empty_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(lyrics_empty_label, lv_pct(100));
	lv_obj_set_style_text_align(lyrics_empty_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(lyrics_empty_label, &font_ui_20, 0);

	lyrics_empty_search_btn = lv_btn_create(lyrics_empty_box);
	lv_obj_remove_style_all(lyrics_empty_search_btn);
	lv_obj_set_size(lyrics_empty_search_btn, LV_SIZE_CONTENT, 40);
	lv_obj_set_style_radius(lyrics_empty_search_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(lyrics_empty_search_btn, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(lyrics_empty_search_btn, 1, 0);
	lv_obj_set_style_border_opa(lyrics_empty_search_btn, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_hor(lyrics_empty_search_btn, 18, 0);
	lv_obj_set_style_opa(lyrics_empty_search_btn, LV_OPA_80, LV_STATE_PRESSED);
	lv_obj_add_event_cb(lyrics_empty_search_btn, lyrics_find_btn_cb, LV_EVENT_CLICKED, NULL);

	lyrics_empty_search_label = lv_label_create(lyrics_empty_search_btn);
	lv_label_set_text(lyrics_empty_search_label, tr("lyrics_download"));
	lv_obj_set_style_text_font(lyrics_empty_search_label, &font_ui_16, 0);
	lv_obj_center(lyrics_empty_search_label);

	resume_sync_btn = lv_btn_create(integrated_lyrics_box);
	lv_obj_remove_style_all(resume_sync_btn);
	lv_obj_set_ignore_layout(resume_sync_btn, true);
	lv_obj_set_size(resume_sync_btn, LV_SIZE_CONTENT, 36);
	lv_obj_set_style_radius(resume_sync_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(resume_sync_btn, 236, 0);
	lv_obj_set_style_border_width(resume_sync_btn, 1, 0);
	lv_obj_set_style_border_opa(resume_sync_btn, LV_OPA_80, 0);
	lv_obj_set_style_pad_hor(resume_sync_btn, 16, 0);
	lv_obj_align(resume_sync_btn, LV_ALIGN_BOTTOM_MID, 0, -12);
	lv_obj_add_event_cb(resume_sync_btn, resume_sync_btn_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_set_hidden(resume_sync_btn, true);

	resume_sync_label = lv_label_create(resume_sync_btn);
	lv_label_set_text(resume_sync_label, tr("lyrics_resume_sync"));
	lv_obj_set_style_text_font(resume_sync_label, &font_ui_14, 0);
	lv_obj_center(resume_sync_label);

	// -----------------------------------------------------------------------
	// 3. Left-Aligned Editorial Track Info + Right Favorite Button
	// -----------------------------------------------------------------------
	track_info_row = lv_obj_create(player_screen);
	style_clean_box(track_info_row);
	lv_obj_set_ignore_layout(track_info_row, true);
	lv_obj_set_flex_flow(track_info_row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(track_info_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(track_info_row, 12, 0);

	lv_obj_t *text_col = lv_obj_create(track_info_row);
	style_clean_box(text_col);
	lv_obj_set_flex_grow(text_col, 1);
	lv_obj_set_height(text_col, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(text_col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(text_col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_row(text_col, 3, 0);

	song_title_label = lv_label_create(text_col);
	lv_label_set_text(song_title_label, tr("player_no_track_loaded"));
	lv_obj_set_width(song_title_label, lv_pct(100));
	lv_obj_set_style_text_font(song_title_label, is_tall_panel ? &font_ui_28_bold : &font_ui_24_bold, 0);
	scrolltext_apply(song_title_label);

	song_artist_label = lv_label_create(text_col);
	lv_label_set_text(song_artist_label, "");
	lv_obj_set_width(song_artist_label, lv_pct(100));
	lv_obj_set_style_text_font(song_artist_label, &font_ui_20, 0);
	scrolltext_apply(song_artist_label);

	fav_btn_obj = make_icon_btn(track_info_row, 44, 44, &icon_star, fav_btn_event_cb, NULL, &fav_btn_icon);
	lv_obj_set_ext_click_area(fav_btn_obj, 8);

	// -----------------------------------------------------------------------
	// 4. Minimalist 72-Bar Audio Waveform + Timestamps & Real Audio Metadata
	// -----------------------------------------------------------------------
	wave_section = lv_obj_create(player_screen);
	style_clean_box(wave_section);
	lv_obj_set_ignore_layout(wave_section, true);
	lv_obj_set_flex_flow(wave_section, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(wave_section, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(wave_section, 6, 0);

	wave_box = lv_obj_create(wave_section);
	style_clean_box(wave_box);
	lv_obj_set_size(wave_box, lv_pct(100), wave_h);

	wave_w = content_w;
	wave_buf = malloc((size_t)wave_w * (size_t)wave_h * 3);
	if (wave_buf) {
		memset(wave_buf, 0, (size_t)wave_w * (size_t)wave_h * 3);
		wave_canvas = lv_canvas_create(wave_box);
		lv_canvas_set_buffer(wave_canvas, wave_buf, wave_w, wave_h, LV_COLOR_FORMAT_RGB565A8);
		lv_obj_set_size(wave_canvas, wave_w, wave_h);
		lv_obj_align(wave_canvas, LV_ALIGN_CENTER, 0, 0);
		lv_obj_set_clickable(wave_canvas, false);
		lv_obj_set_hidden(wave_canvas, true);
	}

	progress_slider = lv_slider_create(wave_box);
	lv_obj_set_size(progress_slider, lv_pct(100), 4);
	lv_obj_align(progress_slider, LV_ALIGN_CENTER, 0, 0);
	lv_slider_set_range(progress_slider, 0, 1000);
	lv_obj_set_ext_click_area(progress_slider, 12);
	lv_obj_set_style_radius(progress_slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
	lv_obj_set_style_radius(progress_slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
	lv_obj_set_style_radius(progress_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
	lv_obj_set_style_pad_all(progress_slider, 4, LV_PART_KNOB);
	lv_obj_set_style_border_width(progress_slider, 0, LV_PART_KNOB);
	lv_obj_set_style_shadow_width(progress_slider, 0, LV_PART_KNOB);
	lv_obj_add_event_cb(progress_slider, seek_slider_event_cb, LV_EVENT_ALL, NULL);

	meta_clocks_row = lv_obj_create(wave_section);
	style_clean_box(meta_clocks_row);
	lv_obj_set_size(meta_clocks_row, lv_pct(100), 20);

	elapsed_label = lv_label_create(meta_clocks_row);
	lv_label_set_text(elapsed_label, "0:00");
	lv_obj_set_style_text_font(elapsed_label, &font_ui_14, 0);
	lv_obj_align(elapsed_label, LV_ALIGN_LEFT_MID, 0, 0);

	format_label = lv_label_create(meta_clocks_row);
	lv_label_set_text(format_label, "");
	lv_obj_set_style_text_font(format_label, &font_ui_14, 0);
	lv_obj_align(format_label, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_hidden(format_label, true);

	remaining_label = lv_label_create(meta_clocks_row);
	lv_label_set_text(remaining_label, "0:00");
	lv_obj_set_style_text_font(remaining_label, &font_ui_14, 0);
	lv_obj_align(remaining_label, LV_ALIGN_RIGHT_MID, 0, 0);

	// -----------------------------------------------------------------------
	// 5. Minimal 5-Control Horizontal Transport
	//    Shuffle — Previous — Play/Pause — Next — Repeat
	// -----------------------------------------------------------------------
	controls_row = lv_obj_create(player_screen);
	style_clean_box(controls_row);
	lv_obj_set_ignore_layout(controls_row, true);

	int play_dia = is_tall_panel ? 72 : 66;
	int skip_dia = 54;
	int mode_dia = 46;

	shuffle_btn_obj =
		make_icon_btn(controls_row, mode_dia, mode_dia, &icon_shuffle, shuffle_btn_event_cb, NULL, &shuffle_btn_icon);
	lv_obj_align(shuffle_btn_obj, LV_ALIGN_LEFT_MID, 0, 0);
	lv_obj_set_ext_click_area(shuffle_btn_obj, 8);
	shuffle_dot = lv_obj_create(shuffle_btn_obj);
	style_clean_box(shuffle_dot);
	lv_obj_set_size(shuffle_dot, 5, 5);
	lv_obj_set_style_radius(shuffle_dot, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(shuffle_dot, LV_OPA_COVER, 0);
	lv_obj_align(shuffle_dot, LV_ALIGN_BOTTOM_MID, 0, -2);
	lv_obj_set_hidden(shuffle_dot, true);

	prev_btn_obj = make_icon_btn(controls_row, skip_dia, skip_dia, &icon_skip_back, prev_btn_event_cb, NULL, &prev_icon);
	lv_obj_align(prev_btn_obj, LV_ALIGN_CENTER, -(play_dia / 2 + 24 + skip_dia / 2), 0);
	lv_obj_set_ext_click_area(prev_btn_obj, 6);

	play_btn = lv_btn_create(controls_row);
	lv_obj_remove_style_all(play_btn);
	lv_obj_set_size(play_btn, play_dia, play_dia);
	lv_obj_align(play_btn, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_style_radius(play_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(play_btn, LV_OPA_COVER, 0);
	lv_obj_set_style_opa(play_btn, LV_OPA_40, LV_STATE_DISABLED);
	lv_obj_add_event_cb(play_btn, play_btn_event_cb, LV_EVENT_CLICKED, NULL);
	play_btn_icon = lv_image_create(play_btn);
	lv_image_set_src(play_btn_icon, &icon_play);
	lv_obj_center(play_btn_icon);
	lv_obj_set_clickable(play_btn_icon, false);
	lv_obj_set_event_bubble(play_btn_icon, true);

	next_btn_obj =
		make_icon_btn(controls_row, skip_dia, skip_dia, &icon_skip_forward, next_btn_event_cb, NULL, &next_icon);
	lv_obj_align(next_btn_obj, LV_ALIGN_CENTER, (play_dia / 2 + 24 + skip_dia / 2), 0);
	lv_obj_set_ext_click_area(next_btn_obj, 6);

	repeat_btn_obj =
		make_icon_btn(controls_row, mode_dia, mode_dia, &icon_repeat_all, repeat_btn_event_cb, NULL, &repeat_btn_icon);
	lv_obj_align(repeat_btn_obj, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_obj_set_ext_click_area(repeat_btn_obj, 8);
	repeat_dot = lv_obj_create(repeat_btn_obj);
	style_clean_box(repeat_dot);
	lv_obj_set_size(repeat_dot, 5, 5);
	lv_obj_set_style_radius(repeat_dot, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(repeat_dot, LV_OPA_COVER, 0);
	lv_obj_align(repeat_dot, LV_ALIGN_BOTTOM_MID, 0, -2);
	lv_obj_set_hidden(repeat_dot, true);

	speed_btn_obj =
		make_icon_btn(controls_row, mode_dia, mode_dia, &icon_play_speed, speed_btn_event_cb, NULL, &speed_btn_icon);
	lv_obj_align(speed_btn_obj, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_obj_set_ext_click_area(speed_btn_obj, 8);
	lv_obj_set_hidden(speed_btn_obj, true);

	// -----------------------------------------------------------------------
	// 6. Bottom Shortcuts Row: One-Tap Lyrics · Subtle Up Next · Queue
	// -----------------------------------------------------------------------
	shortcuts_row = lv_obj_create(player_screen);
	style_clean_box(shortcuts_row);
	lv_obj_set_ignore_layout(shortcuts_row, true);

	lyrics_shortcut_btn = lv_btn_create(shortcuts_row);
	lv_obj_remove_style_all(lyrics_shortcut_btn);
	lv_obj_set_size(lyrics_shortcut_btn, LV_SIZE_CONTENT, 36);
	lv_obj_align(lyrics_shortcut_btn, LV_ALIGN_LEFT_MID, 0, 0);
	lv_obj_set_style_radius(lyrics_shortcut_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_pad_hor(lyrics_shortcut_btn, 12, 0);
	lv_obj_set_style_pad_column(lyrics_shortcut_btn, 7, 0);
	lv_obj_set_flex_flow(lyrics_shortcut_btn, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(lyrics_shortcut_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_bg_color(lyrics_shortcut_btn, lv_color_white(), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(lyrics_shortcut_btn, LV_OPA_10, LV_STATE_PRESSED);
	lv_obj_set_ext_click_area(lyrics_shortcut_btn, 8);
	lv_obj_add_event_cb(lyrics_shortcut_btn, lyrics_shortcut_cb, LV_EVENT_CLICKED, NULL);

	lyrics_shortcut_icon = lv_image_create(lyrics_shortcut_btn);
	lv_image_set_src(lyrics_shortcut_icon, &icon_lyrics);
	lv_obj_set_clickable(lyrics_shortcut_icon, false);
	lv_obj_set_event_bubble(lyrics_shortcut_icon, true);

	lyrics_shortcut_label = lv_label_create(lyrics_shortcut_btn);
	lv_label_set_text(lyrics_shortcut_label, tr("lyrics"));
	lv_obj_set_style_text_font(lyrics_shortcut_label, &font_ui_14, 0);
	lv_obj_set_clickable(lyrics_shortcut_label, false);
	lv_obj_set_event_bubble(lyrics_shortcut_label, true);

	upnext_bearing_btn = lv_btn_create(shortcuts_row);
	lv_obj_remove_style_all(upnext_bearing_btn);
	lv_obj_set_size(upnext_bearing_btn, 212, 34);
	lv_obj_align(upnext_bearing_btn, LV_ALIGN_CENTER, 0, 0);
	lv_obj_add_event_cb(upnext_bearing_btn, open_queue_btn_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_set_hidden(upnext_bearing_btn, true);

	upnext_bearing_label = lv_label_create(upnext_bearing_btn);
	lv_label_set_text(upnext_bearing_label, "");
	lv_label_set_long_mode(upnext_bearing_label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(upnext_bearing_label, 208);
	lv_obj_set_height(upnext_bearing_label, lv_font_get_line_height(&font_ui_14));
	lv_obj_set_style_text_align(upnext_bearing_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(upnext_bearing_label, &font_ui_14, 0);
	lv_obj_center(upnext_bearing_label);
	lv_obj_set_clickable(upnext_bearing_label, false);
	lv_obj_set_event_bubble(upnext_bearing_label, true);

	queue_shortcut_btn = lv_btn_create(shortcuts_row);
	lv_obj_remove_style_all(queue_shortcut_btn);
	lv_obj_set_size(queue_shortcut_btn, LV_SIZE_CONTENT, 36);
	lv_obj_align(queue_shortcut_btn, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_obj_set_style_radius(queue_shortcut_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_pad_hor(queue_shortcut_btn, 12, 0);
	lv_obj_set_style_pad_column(queue_shortcut_btn, 7, 0);
	lv_obj_set_flex_flow(queue_shortcut_btn, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(queue_shortcut_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_bg_color(queue_shortcut_btn, lv_color_white(), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(queue_shortcut_btn, LV_OPA_10, LV_STATE_PRESSED);
	lv_obj_set_ext_click_area(queue_shortcut_btn, 8);
	lv_obj_add_event_cb(queue_shortcut_btn, open_queue_btn_cb, LV_EVENT_CLICKED, NULL);

	queue_shortcut_label = lv_label_create(queue_shortcut_btn);
	lv_label_set_text(queue_shortcut_label, tr("queue"));
	lv_obj_set_style_text_font(queue_shortcut_label, &font_ui_14, 0);
	lv_obj_set_clickable(queue_shortcut_label, false);
	lv_obj_set_event_bubble(queue_shortcut_label, true);

	queue_shortcut_icon = lv_image_create(queue_shortcut_btn);
	lv_image_set_src(queue_shortcut_icon, &icon_queue_small);
	lv_obj_set_clickable(queue_shortcut_icon, false);
	lv_obj_set_event_bubble(queue_shortcut_icon, true);

	// -----------------------------------------------------------------------
	// 7. Mode B: Immersive Full-Screen Lyrics Overlay
	// -----------------------------------------------------------------------
	fullscreen_lyrics_box = lv_obj_create(player_screen);
	style_clean_box(fullscreen_lyrics_box);
	lv_obj_set_ignore_layout(fullscreen_lyrics_box, true);
	lv_obj_set_pos(fullscreen_lyrics_box, 0, 0);
	lv_obj_set_size(fullscreen_lyrics_box, screen_w, screen_h);
	lv_obj_set_clickable(fullscreen_lyrics_box, true);
	lv_obj_set_hidden(fullscreen_lyrics_box, true);

	fs_header_row = lv_obj_create(fullscreen_lyrics_box);
	style_clean_box(fs_header_row);
	lv_obj_set_ignore_layout(fs_header_row, true);
	lv_obj_set_flex_flow(fs_header_row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(fs_header_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(fs_header_row, 10, 0);

	// Tapping the 44x44 thumbnail returns straight to Artwork mode
	fs_thumb_box = lv_btn_create(fs_header_row);
	lv_obj_remove_style_all(fs_thumb_box);
	lv_obj_set_size(fs_thumb_box, 44, 44);
	lv_obj_set_style_radius(fs_thumb_box, 8, 0);
	lv_obj_set_style_bg_opa(fs_thumb_box, LV_OPA_COVER, 0);
	lv_obj_set_style_clip_corner(fs_thumb_box, true, 0);
	lv_obj_add_event_cb(fs_thumb_box, fs_return_to_artwork_cb, LV_EVENT_CLICKED, NULL);

	fs_thumb_fallback = lv_image_create(fs_thumb_box);
	lv_image_set_src(fs_thumb_fallback, &icon_music2);
	lv_obj_set_style_image_recolor_opa(fs_thumb_fallback, LV_OPA_COVER, 0);
	lv_obj_center(fs_thumb_fallback);
	lv_obj_set_clickable(fs_thumb_fallback, false);
	lv_obj_set_event_bubble(fs_thumb_fallback, true);

	fs_thumb_img = lv_image_create(fs_thumb_box);
	lv_obj_set_size(fs_thumb_img, 44, 44);
	lv_obj_center(fs_thumb_img);
	lv_obj_set_clickable(fs_thumb_img, false);
	lv_obj_set_event_bubble(fs_thumb_img, true);
	lv_obj_set_hidden(fs_thumb_img, true);

	lv_obj_t *fs_meta_col = lv_obj_create(fs_header_row);
	style_clean_box(fs_meta_col);
	lv_obj_set_flex_grow(fs_meta_col, 1);
	lv_obj_set_height(fs_meta_col, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(fs_meta_col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(fs_meta_col, 1, 0);

	fs_title_label = lv_label_create(fs_meta_col);
	lv_label_set_text(fs_title_label, "");
	lv_label_set_long_mode(fs_title_label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(fs_title_label, lv_pct(100));
	lv_obj_set_height(fs_title_label, lv_font_get_line_height(&font_ui_20_bold));
	lv_obj_set_style_text_font(fs_title_label, &font_ui_20_bold, 0);

	fs_artist_label = lv_label_create(fs_meta_col);
	lv_label_set_text(fs_artist_label, "");
	lv_label_set_long_mode(fs_artist_label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(fs_artist_label, lv_pct(100));
	lv_obj_set_height(fs_artist_label, lv_font_get_line_height(&font_ui_14));
	lv_obj_set_style_text_font(fs_artist_label, &font_ui_14, 0);

	fs_size_btn = make_icon_btn(fs_header_row, 38, 38, &icon_lyrics_aa, fs_size_cycle_cb, NULL, &fs_size_icon);
	lv_obj_set_ext_click_area(fs_size_btn, 4);

	fs_offset_toggle_btn = lv_btn_create(fs_header_row);
	lv_obj_remove_style_all(fs_offset_toggle_btn);
	lv_obj_set_size(fs_offset_toggle_btn, 38, 38);
	lv_obj_set_style_radius(fs_offset_toggle_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(fs_offset_toggle_btn, lv_color_white(), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(fs_offset_toggle_btn, LV_OPA_10, LV_STATE_PRESSED);
	lv_obj_add_event_cb(fs_offset_toggle_btn, fs_offset_toggle_cb, LV_EVENT_CLICKED, NULL);
	fs_offset_toggle_label = lv_label_create(fs_offset_toggle_btn);
	lv_label_set_text(fs_offset_toggle_label, "±");
	lv_obj_set_style_text_font(fs_offset_toggle_label, &font_ui_22_bold, 0);
	lv_obj_center(fs_offset_toggle_label);

	fs_close_btn =
		make_icon_btn(fs_header_row, 38, 38, &icon_collapse, fs_collapse_to_integrated_cb, NULL, &fs_close_icon);
	lv_obj_set_ext_click_area(fs_close_btn, 6);

	// Collapsible Synchronization Offset Strip
	fs_offset_bar = lv_obj_create(fullscreen_lyrics_box);
	style_clean_box(fs_offset_bar);
	lv_obj_set_ignore_layout(fs_offset_bar, true);
	lv_obj_set_style_radius(fs_offset_bar, 12, 0);
	lv_obj_set_style_bg_opa(fs_offset_bar, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(fs_offset_bar, 1, 0);
	lv_obj_set_style_border_opa(fs_offset_bar, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_hor(fs_offset_bar, 10, 0);
	lv_obj_set_flex_flow(fs_offset_bar, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(fs_offset_bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_hidden(fs_offset_bar, true);

	fs_offset_minus_btn = lv_btn_create(fs_offset_bar);
	lv_obj_remove_style_all(fs_offset_minus_btn);
	lv_obj_set_size(fs_offset_minus_btn, 64, 34);
	lv_obj_set_style_radius(fs_offset_minus_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(fs_offset_minus_btn, lv_color_white(), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(fs_offset_minus_btn, LV_OPA_10, LV_STATE_PRESSED);
	lv_obj_add_event_cb(fs_offset_minus_btn, fs_offset_step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(-200));
	lv_obj_t *minus_lbl = lv_label_create(fs_offset_minus_btn);
	lv_label_set_text(minus_lbl, "-0.2s");
	lv_obj_set_style_text_font(minus_lbl, &font_ui_14, 0);
	lv_obj_set_style_text_color(minus_lbl, lv_color_hex(0xF1F5F9), 0);
	lv_obj_center(minus_lbl);

	fs_offset_value_btn = lv_btn_create(fs_offset_bar);
	lv_obj_remove_style_all(fs_offset_value_btn);
	lv_obj_set_size(fs_offset_value_btn, LV_SIZE_CONTENT, 34);
	lv_obj_set_style_pad_hor(fs_offset_value_btn, 10, 0);
	lv_obj_add_event_cb(fs_offset_value_btn, fs_offset_step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)0);
	fs_offset_value_label = lv_label_create(fs_offset_value_btn);
	lv_label_set_text(fs_offset_value_label, "");
	lv_obj_set_style_text_font(fs_offset_value_label, &font_ui_14, 0);
	lv_obj_center(fs_offset_value_label);

	fs_offset_plus_btn = lv_btn_create(fs_offset_bar);
	lv_obj_remove_style_all(fs_offset_plus_btn);
	lv_obj_set_size(fs_offset_plus_btn, 64, 34);
	lv_obj_set_style_radius(fs_offset_plus_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(fs_offset_plus_btn, lv_color_white(), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(fs_offset_plus_btn, LV_OPA_10, LV_STATE_PRESSED);
	lv_obj_add_event_cb(fs_offset_plus_btn, fs_offset_step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(+200));
	lv_obj_t *plus_lbl = lv_label_create(fs_offset_plus_btn);
	lv_label_set_text(plus_lbl, "+0.2s");
	lv_obj_set_style_text_font(plus_lbl, &font_ui_14, 0);
	lv_obj_set_style_text_color(plus_lbl, lv_color_hex(0xF1F5F9), 0);
	lv_obj_center(plus_lbl);

	fs_offset_search_btn =
		make_icon_btn(fs_offset_bar, 36, 34, &icon_lyrics_search, lyrics_find_btn_cb, NULL, &fs_offset_search_icon);

	fs_stage_area = lv_obj_create(fullscreen_lyrics_box);
	style_clean_box(fs_stage_area);
	lv_obj_set_ignore_layout(fs_stage_area, true);
	lv_obj_set_style_clip_corner(fs_stage_area, true, 0);

	// Mode B Compact Bottom Deck
	fs_bottom_deck = lv_obj_create(fullscreen_lyrics_box);
	style_clean_box(fs_bottom_deck);
	lv_obj_set_ignore_layout(fs_bottom_deck, true);
	lv_obj_set_flex_flow(fs_bottom_deck, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(fs_bottom_deck, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(fs_bottom_deck, 6, 0);

	fs_slider = lv_slider_create(fs_bottom_deck);
	lv_obj_set_size(fs_slider, lv_pct(100), 4);
	lv_slider_set_range(fs_slider, 0, 1000);
	lv_obj_set_ext_click_area(fs_slider, 12);
	lv_obj_set_style_radius(fs_slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
	lv_obj_set_style_radius(fs_slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
	lv_obj_set_style_radius(fs_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
	lv_obj_set_style_pad_all(fs_slider, 4, LV_PART_KNOB);
	lv_obj_set_style_border_width(fs_slider, 0, LV_PART_KNOB);
	lv_obj_set_style_shadow_width(fs_slider, 0, LV_PART_KNOB);
	lv_obj_add_event_cb(fs_slider, seek_slider_event_cb, LV_EVENT_ALL, NULL);

	lv_obj_t *fs_clocks = lv_obj_create(fs_bottom_deck);
	style_clean_box(fs_clocks);
	lv_obj_set_size(fs_clocks, lv_pct(100), 18);

	fs_elapsed_label = lv_label_create(fs_clocks);
	lv_label_set_text(fs_elapsed_label, "0:00");
	lv_obj_set_style_text_font(fs_elapsed_label, &font_ui_14, 0);
	lv_obj_align(fs_elapsed_label, LV_ALIGN_LEFT_MID, 0, 0);

	fs_duration_label = lv_label_create(fs_clocks);
	lv_label_set_text(fs_duration_label, "0:00");
	lv_obj_set_style_text_font(fs_duration_label, &font_ui_14, 0);
	lv_obj_align(fs_duration_label, LV_ALIGN_RIGHT_MID, 0, 0);

	lv_obj_t *fs_transport = lv_obj_create(fs_bottom_deck);
	style_clean_box(fs_transport);
	lv_obj_set_size(fs_transport, lv_pct(100), 66);

	fs_prev_btn = make_icon_btn(fs_transport, 50, 50, &icon_skip_back, prev_btn_event_cb, NULL, &fs_prev_icon);
	lv_obj_align(fs_prev_btn, LV_ALIGN_CENTER, -76, 0);

	fs_play_btn = lv_btn_create(fs_transport);
	lv_obj_remove_style_all(fs_play_btn);
	lv_obj_set_size(fs_play_btn, 60, 60);
	lv_obj_align(fs_play_btn, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_style_radius(fs_play_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(fs_play_btn, LV_OPA_COVER, 0);
	lv_obj_set_style_opa(fs_play_btn, LV_OPA_40, LV_STATE_DISABLED);
	lv_obj_add_event_cb(fs_play_btn, play_btn_event_cb, LV_EVENT_CLICKED, NULL);
	fs_play_icon = lv_image_create(fs_play_btn);
	lv_image_set_src(fs_play_icon, &icon_play);
	lv_obj_center(fs_play_icon);
	lv_obj_set_clickable(fs_play_icon, false);
	lv_obj_set_event_bubble(fs_play_icon, true);

	fs_next_btn = make_icon_btn(fs_transport, 50, 50, &icon_skip_forward, next_btn_event_cb, NULL, &fs_next_icon);
	lv_obj_align(fs_next_btn, LV_ALIGN_CENTER, 76, 0);

	// Invisible top edge strip for Control Centre pull-down when status bar is hidden
	lv_obj_t *panel_edge = lv_obj_create(player_screen);
	style_clean_box(panel_edge);
	lv_obj_set_ignore_layout(panel_edge, true);
	lv_obj_set_size(panel_edge, screen_w, bar_height);
	lv_obj_set_pos(panel_edge, 0, 0);
	lv_obj_set_clickable(panel_edge, true);
	lv_obj_add_event_cb(panel_edge, panel_edge_drag_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(panel_edge, panel_edge_drag_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(panel_edge, panel_edge_drag_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(panel_edge, panel_edge_drag_cb, LV_EVENT_PRESS_LOST, NULL);
	lv_obj_move_foreground(panel_edge);

	// Timers & Observers
	smooth_timer = lv_timer_create(smooth_timer_cb, SMOOTH_PERIOD_MS, NULL);
	lv_timer_pause(smooth_timer);

	progress_slider_timer = lv_timer_create(progress_slider_timer_cb, POLL_PERIOD_IDLE_MS, NULL);
	audio_set_completion_hook(track_end_hook);
	power_slow_in_standby(progress_slider_timer, POLL_PERIOD_PLAYING_MS);

	sleep_timer = lv_timer_create(sleep_timer_cb, 1000, NULL);
	lv_timer_pause(sleep_timer);
	power_slow_in_standby(sleep_timer, 5000);

	device_state_register_queue_cb(upnext_refresh);
	theme_register_refresh(player_refresh_theme);
	fonts_register_change(player_on_font_change);

	update_fs_offset_labels();
	relayout_screen_vertical();
	apply_palette_to_widgets();
	apply_playback_status(AUDIO_STATUS_STOPPED);
}
