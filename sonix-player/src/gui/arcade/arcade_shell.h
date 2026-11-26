#ifndef ARCADE_SHELL_H
#define ARCADE_SHELL_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

#include "src/gui/arcade/arcade_fb.h"
#include "src/gui/arcade/arcade_sfx.h"
#include "src/gui/shell/gui.h"

typedef enum {
	ARCADE_ST_TITLE = 0,
	ARCADE_ST_PLAYING,
	ARCADE_ST_PAUSED,
	ARCADE_ST_GAMEOVER,
} arcade_state_t;

typedef enum {
	ARCADE_MEDAL_NONE = 0,
	ARCADE_MEDAL_BRONZE = 1,
	ARCADE_MEDAL_SILVER = 2,
	ARCADE_MEDAL_GOLD = 3,
} arcade_medal_t;

typedef struct arcade_shell arcade_shell_t;

typedef struct {
	const char *id;			  // e.g. "towerblast" -> [arcade] towerblast_best
	const char *title_tag;	  // lang key
	const char *subtitle_tag; // lang key
	const char *hint_tag;	  // lang key
	const lv_image_dsc_t *icon;
	int target_fps;		  // default 30 (33 ms tick)
	bool dark_backdrop;	  // Fixed dark scenery needs light HUD text in both themes.
	bool title_in_hud;	  // Puzzles can show their title while the score is settled at the end.
	bool compact_scene;	  // 240 x 400, scaled behind native-resolution controls
	bool music_reactive;  // Redraw when the music envelope changes.
	bool use_framebuffer; // true -> allocates RGB565 surface on open, frees on exit

	void (*on_enter)(arcade_shell_t *shell, void *ctx);
	void (*on_exit)(arcade_shell_t *shell, void *ctx);
	void (*on_start)(arcade_shell_t *shell, void *ctx);
	bool (*has_saved_run)(const void *ctx);
	void (*on_resume_saved)(arcade_shell_t *shell, void *ctx);
	void (*on_step)(arcade_shell_t *shell, void *ctx, float dt);
	bool (*is_idle)(const void *ctx); // During play. Input wakes the clock with arcade_shell_mark_dirty.
	void (*on_render)(arcade_shell_t *shell, void *ctx, bool full_redraw);
	void (*on_touch)(arcade_shell_t *shell, void *ctx, bool pressed, int x, int y);
	void (*on_cancel)(arcade_shell_t *shell, void *ctx);
	void (*on_drag)(arcade_shell_t *shell, void *ctx, int x, int y);
	void (*on_theme_changed)(arcade_shell_t *shell, void *ctx);
} arcade_game_ops_t;

arcade_shell_t *arcade_shell_create(gui_config_t *cfg, const arcade_game_ops_t *ops, void *ctx);
bool arcade_shell_pause_for_screen_off(void);
void arcade_shell_open(arcade_shell_t *shell);
void arcade_shell_close(arcade_shell_t *shell);

void arcade_shell_switch_mode(arcade_shell_t *s, const char *score_id, bool resume);
void arcade_shell_set_score(arcade_shell_t *shell, int score);
void arcade_shell_restore_score(arcade_shell_t *shell, int score);
void arcade_shell_set_sub_hud(arcade_shell_t *shell, const char *text);
void arcade_shell_show_callout(arcade_shell_t *shell, const char *text, lv_color_t color);
void arcade_shell_set_end_heading(arcade_shell_t *shell, const char *text);
void arcade_shell_set_end_score_visible(arcade_shell_t *shell, bool visible);
void arcade_shell_set_end_summary(arcade_shell_t *shell, const char *text);
void arcade_shell_set_end_continue(arcade_shell_t *shell, const char *text,
								   void (*cb)(arcade_shell_t *shell, void *ctx));
void arcade_shell_trigger_game_over(arcade_shell_t *shell, int delay_ms);
void arcade_shell_resume_playing(arcade_shell_t *shell);
void arcade_shell_mark_dirty(arcade_shell_t *shell);
void arcade_shell_force_full_redraw(arcade_shell_t *shell);

arcade_fb_t *arcade_shell_fb(arcade_shell_t *shell);
lv_obj_t *arcade_shell_screen(arcade_shell_t *shell);
lv_obj_t *arcade_shell_content(arcade_shell_t *shell);
lv_obj_t *arcade_shell_hud(arcade_shell_t *shell);
lv_obj_t *arcade_shell_title_extra(arcade_shell_t *shell);
void arcade_shell_set_title_extra_visible(arcade_shell_t *shell, bool visible);
arcade_state_t arcade_shell_state(const arcade_shell_t *shell);
int arcade_shell_score(const arcade_shell_t *shell);
int arcade_shell_best(const arcade_shell_t *shell);
int arcade_shell_width(const arcade_shell_t *shell);
int arcade_shell_height(const arcade_shell_t *shell);
bool arcade_shell_is_active(const arcade_shell_t *shell);
float arcade_shell_music_pulse(const arcade_shell_t *shell);

int arcade_shell_read_best(const char *game_id);
bool arcade_medal_thresholds(const char *game_id, int out_tiers[3]);
arcade_medal_t arcade_medal_for_score(const char *game_id, int score, int *next_target_out);
lv_color_t arcade_medal_color(arcade_medal_t medal, bool dark);
bool arcade_game_has_saved_run(const char *game_id);

#endif // ARCADE_SHELL_H
