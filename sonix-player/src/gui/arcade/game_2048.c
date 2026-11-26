#include "game_2048.h"
#include "arcade_scene.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/arcade/arcade_shell.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"

#define BOARD_SIZE 4
#define TILE_POOL_MAX 24
#define BOARD_X 28
#define BOARD_W 424
#define CELL_SZ 91
#define CELL_GAP 12
#define CELL_PAD 12
#define SWIPE_DRAG_PX 30
#define SWIPE_FLICK_PX 20
#define MAX_UNDOS 2

typedef struct {
	lv_obj_t *box;
	lv_obj_t *lbl;
} g2048_tile_widget_t;

typedef struct {
	int from_r;
	int from_c;
	int to_r;
	int to_c;
	int final_val;
	bool merged;
	bool slide_only; // disappears behind merged tile on arrival
} g2048_anim_op_t;

typedef struct {
	arcade_shell_t *shell;
	lv_obj_t *board_box;
	lv_obj_t *slots[BOARD_SIZE * BOARD_SIZE];
	g2048_tile_widget_t pool[TILE_POOL_MAX];
	lv_obj_t *hint_lbl, *mode_label;
	bool challenge;
	uint32_t challenge_seed;
	int target, moves_left;
	lv_obj_t *undo_btn;
	lv_obj_t *undo_icon;
	lv_obj_t *undo_lbl;

	int board[BOARD_SIZE][BOARD_SIZE];
	int prev_board[BOARD_SIZE][BOARD_SIZE];
	int pool_val[TILE_POOL_MAX];
	int prev_score;
	int undos;
	unsigned earned_mask;
	bool has_prev;
	bool won_2048;
	bool end_triggered;
	bool tracking;
	bool swipe_consumed;
	bool config_dirty;
	int press_x;
	int press_y;
	uint32_t rng;
} g2048_ctx_t;

static g2048_ctx_t g_2048;

static inline int cell_px(int idx) { return CELL_PAD + idx * (CELL_SZ + CELL_GAP); }

static void tile_palette(int val, bool dark, lv_color_t *bg, lv_color_t *fg, const lv_font_t **font) {
	if (val >= 16384) {
		*font = &font_ui_20_bold;
	} else if (val >= 1024) {
		*font = &font_ui_24_bold;
	} else if (val >= 128) {
		*font = &font_ui_28;
	} else {
		*font = &font_ui_36_bold;
	}

	// The graphite well stays dark in both themes, giving every occupied tile a visible silhouette.
	static const uint32_t faces[] = {0xf5ead1, 0xffc348, 0x57d8cf, 0x87beee, 0xac9ceb,
		0xf89591, 0xf2a45b, 0xe3ca69, 0xe29ac8, 0x97caf7, 0xffdc80};
	unsigned tier = 0;
	for (int n = val; n > 2 && tier + 1 < LV_ARRAYLEN(faces); n /= 2) tier++;
	*bg = lv_color_hex(faces[tier]);
	if (!dark) *bg = lv_color_mix(*bg, lv_color_white(), 235);
	*fg = lv_color_hex(0x20232b);
}

static void coord_for_dir(g2048_dir_t dir, int line, int pos, int *r, int *c) {
	switch (dir) {
	case G2048_LEFT:
		*r = line;
		*c = pos;
		break;
	case G2048_RIGHT:
		*r = line;
		*c = 3 - pos;
		break;
	case G2048_UP:
		*r = pos;
		*c = line;
		break;
	case G2048_DOWN:
	default:
		*r = 3 - pos;
		*c = line;
		break;
	}
}

static int slide_board_internal(int board[4][4], g2048_dir_t dir, bool *moved_out, bool *reached_2048_out,
								g2048_anim_op_t *ops, int *op_count) {
	int gained = 0;
	bool moved = false;
	bool reached_2048 = false;
	int n_ops = 0;

	for (int line = 0; line < BOARD_SIZE; line++) {
		int vals[BOARD_SIZE];
		int src_pos[BOARD_SIZE];
		int cnt = 0;
		for (int pos = 0; pos < BOARD_SIZE; pos++) {
			int r, c;
			coord_for_dir(dir, line, pos, &r, &c);
			if (board[r][c] > 0) {
				vals[cnt] = board[r][c];
				src_pos[cnt] = pos;
				cnt++;
			}
		}

		int out_vals[BOARD_SIZE] = {0, 0, 0, 0};
		int out_idx = 0;
		int i = 0;
		while (i < cnt) {
			int dst_r, dst_c;
			coord_for_dir(dir, line, out_idx, &dst_r, &dst_c);
			if (i + 1 < cnt && vals[i] == vals[i + 1]) {
				int merged_v = vals[i] * 2;
				out_vals[out_idx] = merged_v;
				gained += merged_v;
				moved = true;
				if (merged_v >= 2048) {
					reached_2048 = true;
				}
				if (ops && n_ops + 2 <= TILE_POOL_MAX) {
					int r1, c1, r2, c2;
					coord_for_dir(dir, line, src_pos[i], &r1, &c1);
					coord_for_dir(dir, line, src_pos[i + 1], &r2, &c2);
					ops[n_ops++] = (g2048_anim_op_t){
						.from_r = r1, .from_c = c1, .to_r = dst_r, .to_c = dst_c,
						.final_val = vals[i], .merged = false, .slide_only = true,
					};
					ops[n_ops++] = (g2048_anim_op_t){
						.from_r = r2, .from_c = c2, .to_r = dst_r, .to_c = dst_c,
						.final_val = merged_v, .merged = true, .slide_only = false,
					};
				}
				i += 2;
			} else {
				out_vals[out_idx] = vals[i];
				if (src_pos[i] != out_idx) {
					moved = true;
				}
				if (ops && n_ops < TILE_POOL_MAX) {
					int r1, c1;
					coord_for_dir(dir, line, src_pos[i], &r1, &c1);
					ops[n_ops++] = (g2048_anim_op_t){
						.from_r = r1, .from_c = c1, .to_r = dst_r, .to_c = dst_c,
						.final_val = vals[i], .merged = false, .slide_only = false,
					};
				}
				i += 1;
			}
			out_idx++;
		}

		for (int pos = 0; pos < BOARD_SIZE; pos++) {
			int r, c;
			coord_for_dir(dir, line, pos, &r, &c);
			board[r][c] = out_vals[pos];
		}
	}

	if (moved_out) *moved_out = moved;
	if (reached_2048_out) *reached_2048_out = reached_2048;
	if (op_count) *op_count = n_ops;
	return gained;
}

int g2048_slide_board(int board[4][4], g2048_dir_t dir, bool *moved_out, bool *reached_2048_out) {
	return slide_board_internal(board, dir, moved_out, reached_2048_out, NULL, NULL);
}

bool g2048_can_move(const int board[4][4]) {
	for (int r = 0; r < BOARD_SIZE; r++) {
		for (int c = 0; c < BOARD_SIZE; c++) {
			int v = board[r][c];
			if (v == 0) {
				return true;
			}
			if (c + 1 < BOARD_SIZE && board[r][c + 1] == v) {
				return true;
			}
			if (r + 1 < BOARD_SIZE && board[r + 1][c] == v) {
				return true;
			}
		}
	}
	return false;
}

static void save_2048_state(g2048_ctx_t *ctx) {
	if (ctx->challenge) return;
	char buf[384];
	int pos = 0;
	for (int r = 0; r < BOARD_SIZE; r++) {
		for (int c = 0; c < BOARD_SIZE; c++) {
			pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "%d,", ctx->board[r][c]);
		}
	}
	for (int r = 0; r < BOARD_SIZE; r++) {
		for (int c = 0; c < BOARD_SIZE; c++) {
			pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "%d,", ctx->prev_board[r][c]);
		}
	}
	snprintf(buf + pos, sizeof(buf) - (size_t)pos, "%d,%d,%d,%d,%u,%d", ctx->has_prev ? 1 : 0,
			 arcade_shell_score(ctx->shell), ctx->prev_score, ctx->undos, ctx->earned_mask, ctx->won_2048 ? 1 : 0);
	config_set("arcade", "2048_state", buf);
	config_set_int("arcade", "2048_saved", 1);
	ctx->config_dirty = true;
}

static void clear_2048_saved(g2048_ctx_t *ctx) {
	if (config_get_int("arcade", "2048_saved", 0) != 0) {
		config_set_int("arcade", "2048_saved", 0);
		config_set("arcade", "2048_state", "");
		ctx->config_dirty = true;
	}
}

static bool load_2048_state(g2048_ctx_t *ctx, int *score_out) {
	if (config_get_int("arcade", "2048_saved", 0) == 0) return false;
	const char *p = config_get("arcade", "2048_state", "");
	if (!p || !p[0]) return false;

	int vals[38];
	for (int i = 0; i < 38; i++) {
		char *end = NULL;
		long v = strtol(p, &end, 10);
		if (end == p) return false;
		vals[i] = (int)v;
		if (i < 37) {
			if (*end != ',') return false;
			p = end + 1;
		}
	}

	int non_zero = 0;
	for (int i = 0; i < 16; i++) {
		if (vals[i] < 0) return false;
		if (vals[i] > 0) non_zero++;
		ctx->board[i / 4][i % 4] = vals[i];
		ctx->prev_board[i / 4][i % 4] = vals[16 + i] >= 0 ? vals[16 + i] : 0;
	}
	if (non_zero == 0 || !g2048_can_move(ctx->board)) return false;

	ctx->has_prev = vals[32] != 0;
	if (score_out) *score_out = vals[33] >= 0 ? vals[33] : 0;
	ctx->prev_score = vals[34] >= 0 ? vals[34] : 0;
	ctx->undos = LV_CLAMP(0, vals[35], MAX_UNDOS);
	ctx->earned_mask = (unsigned)vals[36];
	ctx->won_2048 = vals[37] != 0;
	return true;
}

static bool spawn_tile(int board[4][4], uint32_t *rng, int *out_r, int *out_c) {
	int empty_r[16], empty_c[16];
	int n = 0;
	for (int r = 0; r < BOARD_SIZE; r++) {
		for (int c = 0; c < BOARD_SIZE; c++) {
			if (board[r][c] == 0) {
				empty_r[n] = r;
				empty_c[n] = c;
				n++;
			}
		}
	}
	if (n == 0) {
		return false;
	}
	int pick = (int)(arcade_rng_next(rng) % (uint32_t)n);
	int val = ((arcade_rng_next(rng) % 10u) == 0u) ? 4 : 2;
	board[empty_r[pick]][empty_c[pick]] = val;
	if (out_r) *out_r = empty_r[pick];
	if (out_c) *out_c = empty_c[pick];
	return true;
}

// Generate a reachable opening, then follow a legal witness path until it makes a larger tile.
static void build_challenge(g2048_ctx_t *ctx) {
	uint32_t directions = ctx->challenge_seed;
	for (int attempt = 0; attempt < 48; attempt++) {
		int trial[4][4] = {{0}};
		uint32_t rng = arcade_rng_next(&directions);
		spawn_tile(trial, &rng, NULL, NULL);
		spawn_tile(trial, &rng, NULL, NULL);
		for (int i = 0; i < 18; i++) {
			bool moved;
			g2048_slide_board(trial, arcade_rng_next(&directions) % 4, &moved, NULL);
			if (moved) spawn_tile(trial, &rng, NULL, NULL);
		}
		int largest = 0;
		for (int r = 0; r < 4; r++)
			for (int c = 0; c < 4; c++) largest = LV_MAX(largest, trial[r][c]);
		memcpy(ctx->board, trial, sizeof(trial));
		ctx->rng = rng;
		int moves = 0;
		for (int i = 0; i < 20 && moves < 6; i++) {
			bool moved;
			g2048_slide_board(trial, arcade_rng_next(&directions) % 4, &moved, NULL);
			if (!moved) continue;
			moves++;
			for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
				if (trial[r][c] > largest) {
					ctx->target = trial[r][c];
					ctx->moves_left = moves + 2;
					return;
				}
			}
			spawn_tile(trial, &rng, NULL, NULL);
		}
	}
	memset(ctx->board, 0, sizeof(ctx->board));
	ctx->board[0][0] = ctx->board[0][3] = 2;
	ctx->target = 4;
	ctx->moves_left = 2;
}

static void update_mode(g2048_ctx_t *ctx) {
	lv_label_set_text(ctx->mode_label, tr(ctx->challenge ? "arcade_classic" : "arcade_challenge"));
	lv_obj_set_hidden(ctx->undo_btn, ctx->challenge);
	if (ctx->challenge) {
		char hint[96], moves[45];
		snprintf(moves, sizeof(moves), tr("arcade_moves"), ctx->moves_left);
		snprintf(hint, sizeof(hint), "%s %d   ·   %s", tr("arcade_target"), ctx->target, moves);
		lv_label_set_text(ctx->hint_lbl, hint);
	} else lv_label_set_text(ctx->hint_lbl, tr("arcade_swipe_hint"));
}

static void switch_mode(lv_event_t *e) {
	g2048_ctx_t *ctx = lv_event_get_user_data(e);
	if (ctx->end_triggered || arcade_shell_state(ctx->shell) != ARCADE_ST_PLAYING) return;
	if (!ctx->challenge) {
		save_2048_state(ctx);
		ctx->challenge_seed = arcade_rng_seed();
	}
	ctx->challenge = !ctx->challenge;
	arcade_shell_switch_mode(ctx->shell, ctx->challenge ? "2048_challenge" : "2048", !ctx->challenge);
	update_mode(ctx);
}

static void stop_all_anims(g2048_ctx_t *ctx) {
	for (int i = 0; i < TILE_POOL_MAX; i++) {
		if (ctx->pool[i].box) {
			lv_anim_delete(ctx->pool[i].box, NULL);
		}
	}
}

static void refresh_undo_btn(g2048_ctx_t *ctx) {
	if (!ctx->undo_btn || !ctx->undo_lbl || !ctx->undo_icon) return;
	const theme_palette_t *pal = theme();
	bool ready = (ctx->undos > 0 && ctx->has_prev && !ctx->end_triggered);

	char buf[16];
	snprintf(buf, sizeof(buf), "%d", ctx->undos);
	lv_label_set_text(ctx->undo_lbl, buf);

	lv_obj_set_style_bg_color(ctx->undo_btn, pal->surface, 0);
	lv_obj_set_style_bg_color(ctx->undo_btn, pal->surface_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(ctx->undo_btn, ready ? 225 : 120, 0);
	lv_obj_set_style_border_color(ctx->undo_btn, (ready && ctx->undos >= MAX_UNDOS) ? pal->accent : pal->border, 0);
	lv_obj_set_style_text_color(ctx->undo_lbl, ready ? pal->text_primary : pal->text_secondary, 0);
	lv_obj_set_style_image_recolor(ctx->undo_icon, ready ? pal->accent : pal->text_secondary, 0);
	lv_obj_set_style_image_opa(ctx->undo_icon, ready ? LV_OPA_COVER : LV_OPA_50, 0);
}

static void style_tile_widget(g2048_ctx_t *ctx, int pool_idx, int val) {
	g2048_tile_widget_t *w = &ctx->pool[pool_idx];
	ctx->pool_val[pool_idx] = val;
	if (!w->box || !w->lbl) {
		return;
	}
	lv_color_t bg, fg;
	const lv_font_t *font;
	tile_palette(val, theme_is_dark(), &bg, &fg, &font);
	lv_obj_set_style_bg_color(w->box, bg, 0);
	lv_obj_set_style_bg_grad_dir(w->box, LV_GRAD_DIR_VER, 0);
	lv_obj_set_style_bg_grad_color(w->box, lv_color_mix(bg, lv_color_black(), 225), 0);
	lv_obj_set_style_border_width(w->box, 1, 0);
	lv_obj_set_style_border_color(w->box, lv_color_mix(bg, lv_color_white(), 150), 0);
	lv_obj_set_style_text_color(w->lbl, fg, 0);
	lv_obj_set_style_text_font(w->lbl, font, 0);
	char buf[16];
	snprintf(buf, sizeof(buf), "%d", val);
	lv_label_set_text(w->lbl, buf);
}

static void anim_x_cb(void *obj, int32_t v) { lv_obj_set_x((lv_obj_t *)obj, v); }
static void anim_y_cb(void *obj, int32_t v) { lv_obj_set_y((lv_obj_t *)obj, v); }
static void anim_scale_cb(void *obj, int32_t v) { lv_obj_set_style_transform_scale((lv_obj_t *)obj, v, 0); }

static void slide_only_done_cb(lv_anim_t *a) {
	lv_obj_t *obj = (lv_obj_t *)a->var;
	if (obj) {
		lv_obj_set_hidden(obj, true);
	}
}

static void pop_done_cb(lv_anim_t *a) {
	lv_obj_t *obj = (lv_obj_t *)a->var;
	if (!obj) {
		return;
	}
	lv_anim_t b;
	lv_anim_init(&b);
	lv_anim_set_var(&b, obj);
	lv_anim_set_exec_cb(&b, anim_scale_cb);
	lv_anim_set_values(&b, 294, 256);
	lv_anim_set_duration(&b, 85);
	lv_anim_set_path_cb(&b, lv_anim_path_ease_in_out);
	lv_anim_start(&b);
}

static void sync_board_static(g2048_ctx_t *ctx) {
	stop_all_anims(ctx);
	int idx = 0;
	for (int r = 0; r < BOARD_SIZE; r++) {
		for (int c = 0; c < BOARD_SIZE; c++) {
			int val = ctx->board[r][c];
			if (val > 0 && idx < TILE_POOL_MAX) {
				g2048_tile_widget_t *w = &ctx->pool[idx];
				style_tile_widget(ctx, idx, val);
				lv_obj_set_pos(w->box, cell_px(c), cell_px(r));
				lv_obj_set_style_transform_scale(w->box, 256, 0);
				lv_obj_set_hidden(w->box, false);
				idx++;
			}
		}
	}
	for (int i = idx; i < TILE_POOL_MAX; i++) {
		ctx->pool_val[i] = 0;
		lv_obj_set_hidden(ctx->pool[i].box, true);
	}
}

static void apply_theme_widgets(g2048_ctx_t *ctx) {
	if (!ctx->board_box) {
		return;
	}
	const theme_palette_t *pal = theme();
	lv_obj_set_style_bg_color(ctx->board_box, lv_color_hex(0x32373f), 0);
	lv_obj_set_style_border_color(ctx->board_box, pal->border, 0);
	for (int i = 0; i < BOARD_SIZE * BOARD_SIZE; i++) {
		if (ctx->slots[i]) {
			lv_obj_set_style_bg_color(ctx->slots[i], lv_color_hex(pal->dark ? 0x242832 : 0x202631), 0);
		}
	}
	for (int i = 0; i < TILE_POOL_MAX; i++) {
		if (ctx->pool_val[i] > 0 && ctx->pool[i].box && !lv_obj_is_hidden(ctx->pool[i].box)) {
			style_tile_widget(ctx, i, ctx->pool_val[i]);
		}
	}
	if (ctx->hint_lbl) {
		lv_obj_set_style_text_color(ctx->hint_lbl, lv_color_hex(0xf2efe7), 0);
	}
	refresh_undo_btn(ctx);
}

static void undo_btn_cb(lv_event_t *e) {
	g2048_ctx_t *ctx = (g2048_ctx_t *)lv_event_get_user_data(e);
	if (!ctx || arcade_shell_state(ctx->shell) != ARCADE_ST_PLAYING || ctx->end_triggered) {
		return;
	}
	if (ctx->undos <= 0 || !ctx->has_prev) {
		arcade_sfx_play(ARCADE_SFX_MISS);
		return;
	}
	ctx->undos--;
	ctx->has_prev = false;
	memcpy(ctx->board, ctx->prev_board, sizeof(ctx->board));
	arcade_shell_restore_score(ctx->shell, ctx->prev_score);
	arcade_sfx_play(ARCADE_SFX_SLIDE);
	sync_board_static(ctx);
	refresh_undo_btn(ctx);
	save_2048_state(ctx);
}

static void g2048_continue_cb(arcade_shell_t *shell, void *arg) {
	g2048_ctx_t *ctx = (g2048_ctx_t *)arg;
	arcade_shell_set_end_heading(shell, NULL);
	arcade_shell_set_end_summary(shell, NULL);
	arcade_shell_set_end_continue(shell, NULL, NULL);
	arcade_shell_resume_playing(shell);
	if (ctx && g2048_can_move(ctx->board)) {
		save_2048_state(ctx);
	}
}

static unsigned milestone_bit(int merged_val) {
	switch (merged_val) {
	case 512:
		return 1u << 0;
	case 1024:
		return 1u << 1;
	case 2048:
		return 1u << 2;
	case 4096:
		return 1u << 3;
	default:
		return 0u;
	}
}

static void perform_move(g2048_ctx_t *ctx, g2048_dir_t dir) {
	if (ctx->end_triggered) {
		return;
	}

	int snapshot[BOARD_SIZE][BOARD_SIZE];
	memcpy(snapshot, ctx->board, sizeof(snapshot));
	int snapshot_score = arcade_shell_score(ctx->shell);

	g2048_anim_op_t ops[TILE_POOL_MAX];
	int op_count = 0;
	bool moved = false;
	bool reached_2048 = false;
	int gained = slide_board_internal(ctx->board, dir, &moved, &reached_2048, ops, &op_count);
	if (!moved) {
		return;
	}

	memcpy(ctx->prev_board, snapshot, sizeof(ctx->prev_board));
	ctx->prev_score = snapshot_score;
	ctx->has_prev = true;

	int spawn_r = -1, spawn_c = -1;
	spawn_tile(ctx->board, &ctx->rng, &spawn_r, &spawn_c);

	stop_all_anims(ctx);
	for (int i = 0; i < TILE_POOL_MAX; i++) {
		ctx->pool_val[i] = 0;
		lv_obj_set_hidden(ctx->pool[i].box, true);
	}

	bool earned_undo = false;
	int pool_idx = 0;
	for (int i = 0; i < op_count && pool_idx < TILE_POOL_MAX; i++) {
		g2048_anim_op_t *op = &ops[i];
		g2048_tile_widget_t *w = &ctx->pool[pool_idx];
		style_tile_widget(ctx, pool_idx, op->final_val);
		int sx = cell_px(op->from_c), sy = cell_px(op->from_r);
		int dx = cell_px(op->to_c), dy = cell_px(op->to_r);
		lv_obj_set_pos(w->box, sx, sy);
		lv_obj_set_style_transform_scale(w->box, 256, 0);
		lv_obj_set_hidden(w->box, false);

		if (sx != dx) {
			lv_anim_t a;
			lv_anim_init(&a);
			lv_anim_set_var(&a, w->box);
			lv_anim_set_exec_cb(&a, anim_x_cb);
			lv_anim_set_values(&a, sx, dx);
			lv_anim_set_duration(&a, 110);
			lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
			if (op->slide_only) {
				lv_anim_set_completed_cb(&a, slide_only_done_cb);
			}
			lv_anim_start(&a);
		} else if (sy != dy) {
			lv_anim_t a;
			lv_anim_init(&a);
			lv_anim_set_var(&a, w->box);
			lv_anim_set_exec_cb(&a, anim_y_cb);
			lv_anim_set_values(&a, sy, dy);
			lv_anim_set_duration(&a, 110);
			lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
			if (op->slide_only) {
				lv_anim_set_completed_cb(&a, slide_only_done_cb);
			}
			lv_anim_start(&a);
		} else if (op->slide_only) {
			lv_obj_set_hidden(w->box, true);
		}

		if (op->merged) {
			unsigned mbit = milestone_bit(op->final_val);
			if (mbit && !(ctx->earned_mask & mbit)) {
				ctx->earned_mask |= mbit;
				if (ctx->undos < MAX_UNDOS) {
					ctx->undos++;
					earned_undo = true;
				}
			}

			lv_obj_move_foreground(w->box);
			lv_anim_t p;
			lv_anim_init(&p);
			lv_anim_set_var(&p, w->box);
			lv_anim_set_exec_cb(&p, anim_scale_cb);
			lv_anim_set_values(&p, 256, 294);
			lv_anim_set_delay(&p, 70);
			lv_anim_set_duration(&p, 75);
			lv_anim_set_path_cb(&p, lv_anim_path_ease_out);
			lv_anim_set_completed_cb(&p, pop_done_cb);
			lv_anim_start(&p);
		}
		pool_idx++;
	}

	if (spawn_r >= 0 && spawn_c >= 0 && pool_idx < TILE_POOL_MAX) {
		g2048_tile_widget_t *sw = &ctx->pool[pool_idx];
		style_tile_widget(ctx, pool_idx, ctx->board[spawn_r][spawn_c]);
		lv_obj_set_pos(sw->box, cell_px(spawn_c), cell_px(spawn_r));
		lv_obj_set_style_transform_scale(sw->box, 140, 0);
		lv_obj_set_hidden(sw->box, false);

		lv_anim_t a;
		lv_anim_init(&a);
		lv_anim_set_var(&a, sw->box);
		lv_anim_set_exec_cb(&a, anim_scale_cb);
		lv_anim_set_values(&a, 140, 256);
		lv_anim_set_duration(&a, 120);
		lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
		lv_anim_start(&a);
	}

	if (gained > 0) {
		int step = gained >= 512 ? 6 : (gained >= 128 ? 4 : (gained >= 32 ? 2 : 1));
		arcade_sfx_play_pitch(earned_undo ? ARCADE_SFX_POWERUP : ARCADE_SFX_POP, step);
		arcade_shell_set_score(ctx->shell, snapshot_score + gained);
	} else {
		arcade_sfx_play(ARCADE_SFX_SLIDE);
	}

	if (earned_undo) {
		arcade_shell_show_callout(ctx->shell, "+1  ←", theme()->accent);
	}
	refresh_undo_btn(ctx);

	if (ctx->challenge) {
		ctx->moves_left--;
		update_mode(ctx);
		bool won = false;
		for (int i = 0; i < op_count; i++)
			if (ops[i].merged && ops[i].final_val >= ctx->target) won = true;
		if (won || !ctx->moves_left || !g2048_can_move(ctx->board)) {
			ctx->end_triggered = true;
			arcade_shell_set_end_heading(ctx->shell, tr(won ? "arcade_level_clear" : "arcade_game_over"));
			arcade_shell_set_end_summary(ctx->shell, lv_label_get_text(ctx->hint_lbl));
			arcade_shell_trigger_game_over(ctx->shell, 280);
		}
		return;
	}

	if (reached_2048 && !ctx->won_2048) {
		ctx->won_2048 = true;
		save_2048_state(ctx);
		arcade_shell_set_end_heading(ctx->shell, tr("arcade_2048_win"));
		arcade_shell_set_end_summary(ctx->shell, tr("arcade_2048_win_sub"));
		arcade_shell_set_end_continue(ctx->shell, tr("arcade_keep_playing"), g2048_continue_cb);
		arcade_shell_trigger_game_over(ctx->shell, 280);
		return;
	}

	if (!g2048_can_move(ctx->board)) {
		ctx->end_triggered = true;
		refresh_undo_btn(ctx);
		clear_2048_saved(ctx);
		config_save();
		ctx->config_dirty = false;
		arcade_shell_set_end_heading(ctx->shell, NULL);
		arcade_shell_set_end_summary(ctx->shell, NULL);
		arcade_shell_set_end_continue(ctx->shell, NULL, NULL);
		arcade_shell_trigger_game_over(ctx->shell, 320);
	} else {
		save_2048_state(ctx);
	}
}

static void load_showcase_board(g2048_ctx_t *ctx) {
	static const int SHOWCASE[4][4] = {
		{1024, 512, 256, 128},
		{64, 32, 16, 8},
		{4, 8, 16, 4},
		{2, 4, 2, 0},
	};
	memcpy(ctx->board, SHOWCASE, sizeof(ctx->board));
	sync_board_static(ctx);
}

static void g2048_on_enter(arcade_shell_t *shell, void *arg) {
	(void)shell;
	g2048_ctx_t *ctx = (g2048_ctx_t *)arg;
	ctx->challenge = false;
	update_mode(ctx);
	ctx->rng = arcade_rng_seed();
	ctx->tracking = false;
	ctx->swipe_consumed = false;
	ctx->won_2048 = false;
	ctx->end_triggered = false;
	ctx->undos = 1;
	ctx->has_prev = false;
	if (ctx->hint_lbl) {
		lv_label_set_text(ctx->hint_lbl, tr("arcade_swipe_hint"));
	}
	load_showcase_board(ctx);
	apply_theme_widgets(ctx);
}

static void g2048_on_exit(arcade_shell_t *shell, void *arg) {
	(void)shell;
	g2048_ctx_t *ctx = (g2048_ctx_t *)arg;
	stop_all_anims(ctx);
	if (ctx->config_dirty) {
		config_save();
		ctx->config_dirty = false;
	}
}

static void g2048_on_start(arcade_shell_t *shell, void *arg) {
	(void)shell;
	g2048_ctx_t *ctx = (g2048_ctx_t *)arg;
	if (!ctx->challenge) clear_2048_saved(ctx);
	memset(ctx->board, 0, sizeof(ctx->board));
	memset(ctx->prev_board, 0, sizeof(ctx->prev_board));
	ctx->prev_score = 0;
	ctx->undos = 1;
	ctx->earned_mask = 0;
	ctx->has_prev = false;
	ctx->won_2048 = false;
	ctx->end_triggered = false;
	ctx->tracking = false;
	ctx->swipe_consumed = false;
	if (ctx->challenge) build_challenge(ctx);
	else {
		spawn_tile(ctx->board, &ctx->rng, NULL, NULL);
		spawn_tile(ctx->board, &ctx->rng, NULL, NULL);
	}
	update_mode(ctx);
	sync_board_static(ctx);
	apply_theme_widgets(ctx);
}

static bool g2048_has_saved(const void *arg) {
	(void)arg;
	return config_get_int("arcade", "2048_saved", 0) != 0;
}

static void g2048_resume_saved(arcade_shell_t *shell, void *arg) {
	g2048_ctx_t *ctx = (g2048_ctx_t *)arg;
	int saved_score = 0;
	if (load_2048_state(ctx, &saved_score)) {
		ctx->end_triggered = false;
		ctx->tracking = false;
		ctx->swipe_consumed = false;
		arcade_shell_restore_score(shell, saved_score);
		sync_board_static(ctx);
		apply_theme_widgets(ctx);
	} else {
		g2048_on_start(shell, arg);
	}
}

static void dispatch_delta(g2048_ctx_t *ctx, int dx, int dy) {
	if (abs(dx) >= abs(dy)) {
		perform_move(ctx, dx > 0 ? G2048_RIGHT : G2048_LEFT);
	} else {
		perform_move(ctx, dy > 0 ? G2048_DOWN : G2048_UP);
	}
}

static void g2048_on_touch(arcade_shell_t *shell, void *arg, bool pressed, int x, int y) {
	(void)shell;
	g2048_ctx_t *ctx = (g2048_ctx_t *)arg;
	if (pressed) {
		ctx->tracking = true;
		ctx->swipe_consumed = false;
		ctx->press_x = x;
		ctx->press_y = y;
	} else {
		if (ctx->tracking && !ctx->swipe_consumed) {
			int dx = x - ctx->press_x;
			int dy = y - ctx->press_y;
			if (abs(dx) >= SWIPE_FLICK_PX || abs(dy) >= SWIPE_FLICK_PX) {
				dispatch_delta(ctx, dx, dy);
			}
		}
		ctx->tracking = false;
		ctx->swipe_consumed = false;
	}
}

static void g2048_on_drag(arcade_shell_t *shell, void *arg, int x, int y) {
	(void)shell;
	g2048_ctx_t *ctx = (g2048_ctx_t *)arg;
	if (!ctx->tracking || ctx->swipe_consumed) {
		return;
	}
	int dx = x - ctx->press_x;
	int dy = y - ctx->press_y;
	if (abs(dx) >= SWIPE_DRAG_PX || abs(dy) >= SWIPE_DRAG_PX) {
		ctx->swipe_consumed = true;
		dispatch_delta(ctx, dx, dy);
	}
}

static void g2048_on_theme(arcade_shell_t *shell, void *arg) {
	(void)shell;
	apply_theme_widgets((g2048_ctx_t *)arg);
}

static const arcade_game_ops_t G2048_OPS = {
	.id = "2048",
	.title_tag = "game_2048",
	.subtitle_tag = "game_2048_desc",
	.hint_tag = "game_2048_hint",
	.icon = &icon_atelier_2048,
	.target_fps = 30,
	.use_framebuffer = false,
	.on_enter = g2048_on_enter,
	.on_exit = g2048_on_exit,
	.on_start = g2048_on_start,
	.has_saved_run = g2048_has_saved,
	.on_resume_saved = g2048_resume_saved,
	.on_step = NULL,
	.on_render = NULL,
	.on_touch = g2048_on_touch,
	.on_drag = g2048_on_drag,
	.on_theme_changed = g2048_on_theme,
};

static void build_board_widgets(g2048_ctx_t *ctx) {
	int h = arcade_shell_height(ctx->shell);
	int board_y = (h < 760) ? 184 : 202;
	int undo_y = board_y - 54;

	lv_obj_t *root = arcade_shell_content(ctx->shell);
	lv_obj_t *backdrop = lv_image_create(root);
	lv_image_set_src(backdrop, &scene_tabletop_image);
	lv_obj_set_size(backdrop, lv_pct(100), lv_pct(100));
	lv_image_set_inner_align(backdrop, LV_IMAGE_ALIGN_COVER);
	lv_obj_set_clickable(backdrop, false);
	ctx->board_box = lv_obj_create(root);
	lv_obj_set_pos(ctx->board_box, BOARD_X, board_y);
	lv_obj_set_size(ctx->board_box, BOARD_W, BOARD_W);
	lv_obj_set_style_radius(ctx->board_box, 20, 0);
	lv_obj_set_style_border_width(ctx->board_box, 1, 0);
	lv_obj_set_style_pad_all(ctx->board_box, 0, 0);
	lv_obj_set_style_shadow_width(ctx->board_box, 12, 0);
	lv_obj_set_style_shadow_offset_y(ctx->board_box, 4, 0);
	lv_obj_set_scrollable(ctx->board_box, false);
	lv_obj_set_clickable(ctx->board_box, false);

	for (int r = 0; r < BOARD_SIZE; r++) {
		for (int c = 0; c < BOARD_SIZE; c++) {
			lv_obj_t *slot = lv_obj_create(ctx->board_box);
			lv_obj_remove_style_all(slot);
			lv_obj_set_pos(slot, cell_px(c), cell_px(r));
			lv_obj_set_size(slot, CELL_SZ, CELL_SZ);
			lv_obj_set_style_bg_opa(slot, LV_OPA_COVER, 0);
			lv_obj_set_style_radius(slot, 12, 0);
			lv_obj_set_style_border_width(slot, 1, 0);
			lv_obj_set_style_border_color(slot, lv_color_hex(0x424953), 0);
			lv_obj_set_scrollable(slot, false);
			lv_obj_set_clickable(slot, false);
			ctx->slots[r * BOARD_SIZE + c] = slot;
		}
	}

	for (int i = 0; i < TILE_POOL_MAX; i++) {
		lv_obj_t *box = lv_obj_create(ctx->board_box);
		lv_obj_remove_style_all(box);
		lv_obj_set_size(box, CELL_SZ, CELL_SZ);
		lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
		lv_obj_set_style_radius(box, 12, 0);
		lv_obj_set_style_transform_pivot_x(box, CELL_SZ / 2, 0);
		lv_obj_set_style_transform_pivot_y(box, CELL_SZ / 2, 0);
		lv_obj_set_scrollable(box, false);
		lv_obj_set_clickable(box, false);
		lv_obj_set_hidden(box, true);

		lv_obj_t *lbl = lv_label_create(box);
		lv_obj_center(lbl);
		ctx->pool[i].box = box;
		ctx->pool[i].lbl = lbl;
	}

	lv_obj_t *mode_btn = lv_button_create(arcade_shell_hud(ctx->shell));
	lv_obj_set_pos(mode_btn, BOARD_X, undo_y);
	lv_obj_set_size(mode_btn, LV_SIZE_CONTENT, 38);
	lv_obj_set_style_radius(mode_btn, 19, 0);
	lv_obj_set_style_bg_color(mode_btn, lv_color_hex(0x374151), 0);
	lv_obj_set_style_text_color(mode_btn, lv_color_white(), 0);
	ctx->mode_label = lv_label_create(mode_btn);
	lv_obj_set_style_text_font(ctx->mode_label, &font_ui_16, 0);
	lv_obj_center(ctx->mode_label);
	lv_obj_add_event_cb(mode_btn, switch_mode, LV_EVENT_CLICKED, ctx);

	ctx->hint_lbl = lv_label_create(root);
	lv_label_set_text(ctx->hint_lbl, tr("arcade_swipe_hint"));
	lv_obj_set_style_text_font(ctx->hint_lbl, &font_ui_16, 0);
	lv_obj_set_style_text_align(ctx->hint_lbl, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_width(ctx->hint_lbl, 420);
	lv_obj_set_pos(ctx->hint_lbl, 30, board_y + BOARD_W + (h < 760 ? 14 : 22));
	lv_obj_set_clickable(ctx->hint_lbl, false);

	lv_obj_t *hud = arcade_shell_hud(ctx->shell);
	if (hud) {
		ctx->undo_btn = lv_btn_create(hud);
		lv_obj_set_size(ctx->undo_btn, 82, 36);
		lv_obj_set_pos(ctx->undo_btn, BOARD_X + BOARD_W - 82, undo_y);
		lv_obj_set_style_radius(ctx->undo_btn, 18, 0);
		lv_obj_set_style_border_width(ctx->undo_btn, 1, 0);
		lv_obj_set_style_pad_hor(ctx->undo_btn, 12, 0);
		lv_obj_set_style_pad_ver(ctx->undo_btn, 0, 0);
		lv_obj_set_style_pad_column(ctx->undo_btn, 6, 0);
		lv_obj_set_style_shadow_width(ctx->undo_btn, 0, 0);
		lv_obj_set_flex_flow(ctx->undo_btn, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(ctx->undo_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_ext_click_area(ctx->undo_btn, 8);
		lv_obj_set_event_bubble(ctx->undo_btn, false);
		lv_obj_add_event_cb(ctx->undo_btn, undo_btn_cb, LV_EVENT_CLICKED, ctx);

		ctx->undo_icon = lv_image_create(ctx->undo_btn);
		lv_image_set_src(ctx->undo_icon, &icon_reset);
		lv_image_set_scale(ctx->undo_icon, 190);
		lv_obj_set_style_image_recolor_opa(ctx->undo_icon, LV_OPA_COVER, 0);

		ctx->undo_lbl = lv_label_create(ctx->undo_btn);
		lv_obj_set_style_text_font(ctx->undo_lbl, &font_ui_16, 0);
	}
	refresh_undo_btn(ctx);
}

void game_2048_open(gui_config_t *cfg) {
	if (!g_2048.shell) {
		g_2048.shell = arcade_shell_create(cfg, &G2048_OPS, &g_2048);
		if (g_2048.shell) {
			build_board_widgets(&g_2048);
		}
	}
	arcade_shell_open(g_2048.shell);
}

int game_2048_best(void) { return arcade_shell_read_best("2048"); }
