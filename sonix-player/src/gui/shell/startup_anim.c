#include "startup_anim.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/system/core/config.h"

static lv_obj_t *overlay, *cassette, *wordmark;
static bool played_this_boot;

bool startup_anim_enabled(void) { return config_get_int("ui", "startup_animation", 1) != 0; }

void startup_anim_set_enabled(bool enabled) {
	config_set_int("ui", "startup_animation", enabled);
	config_save();
}

bool startup_anim_should_run(bool wizard) {
	return !wizard && !played_this_boot && startup_anim_enabled();
}

bool startup_anim_is_active(void) { return overlay != NULL; }
void startup_anim_reset_boot_flag_for_test(void) { played_this_boot = false; }

void startup_anim_cancel(void) {
	if (overlay) {
		lv_anim_delete(overlay, NULL);
		lv_obj_delete(overlay);
		overlay = NULL;
	}
}

static void frame(void *obj, int32_t ms) {
	int reveal = LV_CLAMP(0, ms * 255 / 360, 255);
	int title = LV_CLAMP(0, (ms - 120) * 255 / 300, 255);
	int fade = LV_CLAMP(0, (1250 - ms) * 255 / 250, 255);
	lv_obj_set_style_bg_opa(obj, fade, 0);
	lv_obj_set_style_image_opa(cassette, LV_MIN(reveal, fade), 0);
	lv_obj_set_style_translate_y(cassette, (255 - reveal) * 7 / 255, 0);
	lv_obj_set_style_text_opa(wordmark, LV_MIN(title, fade), 0);
}

static void done(lv_anim_t *a) {
	(void)a;
	lv_obj_delete(overlay);
	overlay = NULL;
}

bool startup_anim_start_preview(const gui_config_t *cfg) {
	if (!cfg) return false;
	startup_anim_cancel();
	overlay = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(overlay);
	lv_obj_set_size(overlay, cfg->screen_width, cfg->screen_height);
	lv_obj_set_style_bg_color(overlay, lv_color_hex(0x10182B), 0);
	lv_obj_set_scrollable(overlay, false);
	cassette = lv_image_create(overlay);
	lv_image_set_src(cassette, &icon_atelier_cassette);
	lv_obj_align(cassette, LV_ALIGN_CENTER, 0, -20);
	wordmark = lv_label_create(overlay);
	lv_label_set_text(wordmark, "Sonix Atelier");
	lv_obj_set_style_text_font(wordmark, &font_ui_28, 0);
	lv_obj_set_style_text_color(wordmark, lv_color_hex(0xE1F1FF), 0);
	lv_obj_align(wordmark, LV_ALIGN_CENTER, 0, 68);

	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, overlay);
	lv_anim_set_exec_cb(&a, frame);
	lv_anim_set_values(&a, 0, 1250);
	lv_anim_set_duration(&a, 1250);
	lv_anim_set_completed_cb(&a, done);
	lv_anim_start(&a);
	return true;
}

bool startup_anim_start(const gui_config_t *cfg, bool wizard) {
	if (!startup_anim_should_run(wizard) || !startup_anim_start_preview(cfg)) return false;
	played_this_boot = true;
	return true;
}
