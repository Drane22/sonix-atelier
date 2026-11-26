#include "arcade_scene.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GRID 8
#define CELL 26
#define BOARD_X 16
#define BOARD_Y 87
#define TRAY_Y 321
#define CELL_COLOR_MASK 0x0Fu
#define CELL_PRISM_BIT 0x80u

// One bit per cell, least significant bit on the left. Rotations are dealt
// as distinct pieces so the shape in the tray is the shape that lands.
typedef struct {
	uint8_t w, h, row[5];
} shape_t;

static const shape_t shapes[] = {
	{1, 1, {1}},		   {2, 1, {3}},			  {1, 2, {1, 1}},	 {3, 1, {7}},	   {1, 3, {1, 1, 1}},
	{4, 1, {15}},		   {1, 4, {1, 1, 1, 1}},  {5, 1, {31}},		 {1, 5, {1, 1, 1, 1, 1}},
	{2, 2, {3, 3}},		   {3, 3, {7, 7, 7}},	  {3, 2, {7, 2}},	 {3, 2, {2, 7}},   {2, 2, {1, 3}},
	{2, 2, {3, 2}},		   {2, 2, {2, 3}},		  {2, 2, {3, 1}},	 {3, 2, {1, 7}},   {3, 2, {7, 4}},
	{3, 2, {3, 6}},		   {3, 2, {6, 3}},		  {2, 3, {1, 1, 3}}, {2, 3, {3, 2, 2}}, {3, 2, {7, 7}},
};
static const uint16_t colors[] = {0x3dd7, 0x5d9f, 0xb4fb, 0xfe2b, 0x9c5f, 0xeb70};

typedef struct {
	uint8_t cells[GRID][GRID], flash[GRID][GRID];
	int tray[3], prism_slot, goal;
	int selected, col, row, finger_x, finger_y;
	int fade, placed_x, placed_y, placed_shape;
	int streak, since_clear;
	bool snapping, animating;
	bool config_dirty;
	arcade_particles_t sparks;
	uint32_t rng;
} mosaic_t;

static mosaic_t puzzle;
static arcade_shell_t *shell;

static int color_index(int piece) { return 1 + piece % (int)LV_ARRAYLEN(colors); }

static bool is_big_shape(int piece) { return piece == 7 || piece == 8 || piece == 10; }

static void first_solid_cell(int piece, int *out_r, int *out_c) {
	const shape_t *shape = &shapes[piece];
	for (int r = 0; r < shape->h; r++) {
		for (int c = 0; c < shape->w; c++) {
			if (shape->row[r] & (1u << c)) {
				*out_r = r;
				*out_c = c;
				return;
			}
		}
	}
	*out_r = 0;
	*out_c = 0;
}

static bool fits(const mosaic_t *g, int piece, int x, int y) {
	if (piece < 0 || piece >= (int)LV_ARRAYLEN(shapes)) return false;
	const shape_t *shape = &shapes[piece];
	if (x < 0 || y < 0 || x + shape->w > GRID || y + shape->h > GRID) return false;
	for (int r = 0; r < shape->h; r++)
		for (int c = 0; c < shape->w; c++)
			if ((shape->row[r] & (1u << c)) && (g->cells[y + r][x + c] & CELL_COLOR_MASK)) return false;
	return true;
}

static bool piece_fits_anywhere(const mosaic_t *g, int piece) {
	const shape_t *shape = &shapes[piece];
	for (int y = 0; y <= GRID - shape->h; y++)
		for (int x = 0; x <= GRID - shape->w; x++)
			if (fits(g, piece, x, y)) return true;
	return false;
}

static bool can_play(const mosaic_t *g) {
	for (int i = 0; i < 3; i++) {
		int piece = g->tray[i];
		if (piece >= 0 && piece_fits_anywhere(g, piece)) return true;
	}
	return false;
}

static bool particles_alive(const arcade_particles_t *p) {
	for (int i = 0; i < ARCADE_PARTICLES_MAX; i++) {
		if (p->items[i].active) return true;
	}
	return false;
}

static void deal(mosaic_t *g, int cur_score) {
	int big_cap = cur_score < 600 ? 1 : 2;
	int big_used = 0;
	for (int i = 0; i < 3; i++) {
		int pick = (int)(arcade_rng_next(&g->rng) % LV_ARRAYLEN(shapes));
		for (int tries = 0; tries < 8 && is_big_shape(pick) && big_used >= big_cap; tries++) {
			pick = (int)(arcade_rng_next(&g->rng) % LV_ARRAYLEN(shapes));
		}
		if (is_big_shape(pick)) big_used++;
		g->tray[i] = pick;
	}

	// Smart / Mercy dealer: if none of the 3 dealt pieces fits, guarantee at least one playable shape if any exists.
	if (!can_play(g)) {
		int fitting[LV_ARRAYLEN(shapes)];
		int n_fit = 0;
		for (int p = 0; p < (int)LV_ARRAYLEN(shapes); p++) {
			if (piece_fits_anywhere(g, p)) fitting[n_fit++] = p;
		}
		if (n_fit > 0) {
			int slot = (int)(arcade_rng_next(&g->rng) % 3u);
			g->tray[slot] = fitting[arcade_rng_next(&g->rng) % (uint32_t)n_fit];
		}
	}

	// Occasional Prism bonus jewel after 500 pts (~22% per tray).
	if (cur_score >= 500 && (arcade_rng_next(&g->rng) % 100u) < 22u) {
		g->prism_slot = (int)(arcade_rng_next(&g->rng) % 3u);
	} else {
		g->prism_slot = -1;
	}
}

static void sync_sub_hud(arcade_shell_t *s, const mosaic_t *g) {
	if (!s) return;
	if (g->streak >= 2 && g->since_clear < 3) {
		char combo_txt[48], sub[80];
		snprintf(combo_txt, sizeof(combo_txt), tr("arcade_combo"), g->streak);
		snprintf(sub, sizeof(sub), "%s  ·  %d/3", combo_txt, 3 - g->since_clear);
		arcade_shell_set_sub_hud(s, sub);
	} else {
		static const char *const goals[] = {"mosaic_goal_line", "mosaic_goal_double", "mosaic_goal_prism"};
		arcade_shell_set_sub_hud(s, tr(goals[g->goal]));
	}
}

static void save_mosaic_state(arcade_shell_t *s, mosaic_t *g) {
	char buf[220];
	int pos = 0;
	for (int r = 0; r < GRID; r++) {
		for (int c = 0; c < GRID; c++) {
			pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "%02X", (unsigned)g->cells[r][c]);
		}
	}
	snprintf(buf + pos, sizeof(buf) - (size_t)pos, ";%d,%d,%d,%d,%d,%d", g->tray[0], g->tray[1], g->tray[2],
			 g->prism_slot, g->streak, g->since_clear);
	config_set_int("arcade", "blockmosaic_goal", g->goal);
	config_set("arcade", "blockmosaic_state", buf);
	config_set_int("arcade", "blockmosaic_score", arcade_shell_score(s));
	config_set_int("arcade", "blockmosaic_saved", 1);
	g->config_dirty = true;
}

static void clear_mosaic_saved(mosaic_t *g) {
	if (config_get_int("arcade", "blockmosaic_saved", 0) != 0) {
		config_set_int("arcade", "blockmosaic_saved", 0);
		config_set("arcade", "blockmosaic_state", "");
		g->config_dirty = true;
	}
}

static bool load_mosaic_state(mosaic_t *g, int *score_out) {
	if (config_get_int("arcade", "blockmosaic_saved", 0) == 0) return false;
	const char *st = config_get("arcade", "blockmosaic_state", "");
	if (!st || strlen(st) < GRID * GRID * 2 + 6 || st[GRID * GRID * 2] != ';') return false;

	uint8_t loaded_cells[GRID][GRID];
	for (int i = 0; i < GRID * GRID; i++) {
		unsigned v = 0;
		if (sscanf(st + i * 2, "%2X", &v) != 1) return false;
		uint8_t col = (uint8_t)(v & CELL_COLOR_MASK);
		if (col > LV_ARRAYLEN(colors)) return false;
		loaded_cells[i / GRID][i % GRID] = (uint8_t)(col | (col ? (v & CELL_PRISM_BIT) : 0));
	}
	int t0 = -1, t1 = -1, t2 = -1, pslot = -1, streak = 0, since = 0;
	if (sscanf(st + GRID * GRID * 2 + 1, "%d,%d,%d,%d,%d,%d", &t0, &t1, &t2, &pslot, &streak, &since) != 6) {
		return false;
	}
	int n_shapes = (int)LV_ARRAYLEN(shapes);
	if (t0 < -1 || t0 >= n_shapes || t1 < -1 || t1 >= n_shapes || t2 < -1 || t2 >= n_shapes) return false;

	memcpy(g->cells, loaded_cells, sizeof(g->cells));
	g->tray[0] = t0;
	g->tray[1] = t1;
	g->tray[2] = t2;
	g->prism_slot = (pslot >= -1 && pslot <= 2) ? pslot : -1;
	g->goal = LV_CLAMP(0, config_get_int("arcade", "blockmosaic_goal", 0), 2);
	g->streak = streak >= 0 ? streak : 0;
	g->since_clear = since >= 0 ? since : 0;
	if (!can_play(g)) return false;
	if (score_out) *score_out = (int)config_get_int("arcade", "blockmosaic_score", 0);
	return true;
}

typedef struct {
	int points;
	int lines;
	int prism_blasts;
	bool all_cleared, goal_met;
} place_outcome_t;

static place_outcome_t place(mosaic_t *g, int slot, int x, int y, int cur_score) {
	place_outcome_t out = {0};
	int piece = g->tray[slot];
	if (piece < 0 || !fits(g, piece, x, y)) return out;

	const shape_t *shape = &shapes[piece];
	int pr_r = -1, pr_c = -1;
	if (slot == g->prism_slot) {
		first_solid_cell(piece, &pr_r, &pr_c);
	}

	int placed_cells = 0;
	for (int r = 0; r < shape->h; r++) {
		for (int c = 0; c < shape->w; c++) {
			if (shape->row[r] & (1u << c)) {
				uint8_t val = (uint8_t)color_index(piece);
				if (r == pr_r && c == pr_c) val |= CELL_PRISM_BIT;
				g->cells[y + r][x + c] = val;
				placed_cells++;
			}
		}
	}
	bool instant_prism = (slot == g->prism_slot && piece == 0);
	if (slot == g->prism_slot) g->prism_slot = -1;

	bool rows[GRID], cols[GRID], to_clear[GRID][GRID] = {{false}}, detonated[GRID][GRID] = {{false}};
	int lines = 0;
	for (int i = 0; i < GRID; i++) {
		rows[i] = cols[i] = true;
		for (int j = 0; j < GRID; j++) {
			if (!(g->cells[i][j] & CELL_COLOR_MASK)) rows[i] = false;
			if (!(g->cells[j][i] & CELL_COLOR_MASK)) cols[i] = false;
		}
		lines += rows[i] + cols[i];
	}
	for (int r = 0; r < GRID; r++) {
		for (int c = 0; c < GRID; c++) {
			if (rows[r] || cols[c]) to_clear[r][c] = true;
		}
	}
	if (instant_prism) {
		to_clear[y][x] = true;
	}

	// Chain-detonate any Prism Jewel caught in a cleared line (or dropped as a 1x1 pocket bomb).
	bool chain_more = true;
	while (chain_more) {
		chain_more = false;
		for (int r = 0; r < GRID; r++) {
			for (int c = 0; c < GRID; c++) {
				if (to_clear[r][c] && !detonated[r][c] && (g->cells[r][c] & CELL_PRISM_BIT)) {
					detonated[r][c] = true;
					out.prism_blasts++;
					chain_more = true;
					float bx = (float)(BOARD_X + c * CELL + CELL / 2);
					float by = (float)(BOARD_Y + r * CELL + CELL / 2);
					arcade_particles_spawn_burst(&g->sparks, bx, by, 14, 24.0f, 95.0f, 80.0f, 2.5f, 0.45f, 0xFFE6, 1);
					arcade_particles_spawn_ring(&g->sparks, bx, by, 6.0f, 110.0f, 0.35f, 0xFFFF);
					for (int nr = LV_MAX(0, r - 1); nr <= LV_MIN(GRID - 1, r + 1); nr++) {
						for (int nc = LV_MAX(0, c - 1); nc <= LV_MIN(GRID - 1, c + 1); nc++) {
							if (g->cells[nr][nc] & CELL_COLOR_MASK) to_clear[nr][nc] = true;
						}
					}
				}
			}
		}
	}

	memset(g->flash, 0, sizeof(g->flash));
	int cleared_cells = 0;
	for (int r = 0; r < GRID; r++) {
		for (int c = 0; c < GRID; c++) {
			if (to_clear[r][c] && (g->cells[r][c] & CELL_COLOR_MASK)) {
				g->flash[r][c] = (uint8_t)(g->cells[r][c] & CELL_COLOR_MASK);
				g->cells[r][c] = 0;
				cleared_cells++;
			}
		}
	}

	int points = placed_cells;
	if (lines > 0 || out.prism_blasts > 0) {
		g->streak = (g->streak > 0 && g->since_clear <= 2) ? (g->streak + 1) : 1;
		g->since_clear = 0;
		int base_clear = lines > 0 ? (12 * lines * lines) : 18;
		points += base_clear * g->streak + out.prism_blasts * 25 + cleared_cells;

		bool any_left = false;
		for (int r = 0; r < GRID && !any_left; r++) {
			for (int c = 0; c < GRID; c++) {
				if (g->cells[r][c] & CELL_COLOR_MASK) {
					any_left = true;
					break;
				}
			}
		}
		if (!any_left) {
			out.all_cleared = true;
			points += 300;
			float cx = (float)(BOARD_X + (GRID * CELL) / 2);
			float cy = (float)(BOARD_Y + (GRID * CELL) / 2);
			arcade_particles_spawn_burst(&g->sparks, cx, cy, 24, 30.0f, 125.0f, 65.0f, 3.0f, 0.55f, 0xFED2, 0);
			arcade_particles_spawn_ring(&g->sparks, cx, cy, 12.0f, 180.0f, 0.45f, 0xFED2);
		}
	} else {
		g->since_clear++;
		if (g->since_clear >= 3) {
			g->streak = 0;
		}
	}

	out.goal_met = (g->goal == 0 && lines >= 1) || (g->goal == 1 && lines >= 2) ||
				   (g->goal == 2 && out.prism_blasts > 0);
	if (out.goal_met) {
		points += 30;
		g->goal = (g->goal + 1 + arcade_rng_next(&g->rng) % 2) % 3;
	}
	g->placed_shape = piece;
	g->placed_x = x;
	g->placed_y = y;
	g->tray[slot] = -1;
	if (g->tray[0] < 0 && g->tray[1] < 0 && g->tray[2] < 0) {
		deal(g, cur_score + points);
	}

	out.points = points;
	out.lines = lines;
	return out;
}

static void fade_exec(void *ctx, int32_t value) {
	mosaic_t *g = ctx;
	g->fade = value;
	arcade_shell_mark_dirty(shell);
}

static void fade_done(lv_anim_t *a) {
	(void)a;
	mosaic_t *g = &puzzle;
	g->animating = false;
	arcade_shell_mark_dirty(shell);
}

static void enter_attract(arcade_shell_t *s, void *ctx) {
	(void)s;
	mosaic_t *g = ctx;
	lv_anim_delete(g, fade_exec);
	*g = (mosaic_t){.selected = -1, .placed_shape = -1, .prism_slot = -1, .rng = arcade_rng_seed()};
	arcade_particles_clear(&g->sparks);
	deal(g, 0);
	const uint8_t tiles[4] = {0x1b, 0x93, 0xdb, 0xef};
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < GRID; c++)
			if (tiles[r] & (1u << c)) g->cells[r + 4][c] = (uint8_t)(1 + (c / 2 + r / 2) % 6);
}

static void start_fresh(arcade_shell_t *s, void *ctx) {
	mosaic_t *g = ctx;
	lv_anim_delete(g, fade_exec);
	clear_mosaic_saved(g);
	*g = (mosaic_t){
		.selected = -1, .placed_shape = -1, .prism_slot = -1, .config_dirty = true, .rng = arcade_rng_seed(),
	};
	arcade_particles_clear(&g->sparks);
	deal(g, 0);
	g->goal = arcade_rng_next(&g->rng) % 3;
	sync_sub_hud(s, g);
}

static bool mosaic_has_saved(const void *ctx) {
	(void)ctx;
	return config_get_int("arcade", "blockmosaic_saved", 0) != 0;
}

static void mosaic_resume_saved(arcade_shell_t *s, void *ctx) {
	mosaic_t *g = ctx;
	lv_anim_delete(g, fade_exec);
	*g = (mosaic_t){.selected = -1, .placed_shape = -1, .prism_slot = -1, .rng = arcade_rng_seed()};
	arcade_particles_clear(&g->sparks);
	int saved_score = 0;
	if (load_mosaic_state(g, &saved_score)) {
		arcade_shell_restore_score(s, saved_score);
		sync_sub_hud(s, g);
	} else {
		start_fresh(s, ctx);
	}
}

static void cancel(arcade_shell_t *s, void *ctx) {
	mosaic_t *g = ctx;
	g->selected = -1;
	g->snapping = false;
	arcade_shell_mark_dirty(s);
}

static void leave(arcade_shell_t *s, void *ctx) {
	mosaic_t *g = ctx;
	lv_anim_delete(ctx, fade_exec);
	arcade_particles_clear(&g->sparks);
	cancel(s, ctx);
	if (g->config_dirty) {
		config_save();
		g->config_dirty = false;
	}
}

static bool mosaic_idle(const void *ctx) {
	const mosaic_t *g = ctx;
	return !g->animating && !particles_alive(&g->sparks);
}

static void step(arcade_shell_t *s, void *ctx, float dt) {
	(void)s;
	mosaic_t *g = ctx;
	if (particles_alive(&g->sparks)) {
		arcade_particles_update(&g->sparks, dt);
	}
}

static void drag(arcade_shell_t *s, void *ctx, int x, int y) {
	mosaic_t *g = ctx;
	if (g->selected < 0) return;
	x = x * 240 / arcade_shell_width(s);
	y = y * 400 / arcade_shell_height(s);
	if (x == g->finger_x && y == g->finger_y) return;
	bool was_snapping = g->snapping;
	int last_col = g->col, last_row = g->row;
	g->finger_x = x;
	g->finger_y = y;
	const shape_t *shape = &shapes[g->tray[g->selected]];
	int cy = y - CELL / 2;
	g->snapping =
		x >= BOARD_X - 4 && x <= BOARD_X + GRID * CELL + 4 && cy >= BOARD_Y - 4 && cy < BOARD_Y + GRID * CELL + 4;
	g->col = LV_CLAMP(0, (x - BOARD_X - shape->w * CELL / 2 + CELL / 2) / CELL, GRID - shape->w);
	g->row = LV_CLAMP(0, (cy - BOARD_Y - shape->h * CELL / 2 + CELL / 2) / CELL, GRID - shape->h);
	if (was_snapping && g->snapping && last_col == g->col && last_row == g->row) return;
	arcade_shell_mark_dirty(s);
}

static void touch(arcade_shell_t *s, void *ctx, bool down, int x, int y) {
	mosaic_t *g = ctx;
	if (down) {
		int px = x * 240 / arcade_shell_width(s), py = y * 400 / arcade_shell_height(s);
		int slot = LV_CLAMP(0, px / 80, 2);
		if (g->animating || py < TRAY_Y - 5 || py > TRAY_Y + 66 || g->tray[slot] < 0) return;
		g->selected = slot;
		g->finger_x = -1;
		arcade_sfx_play(ARCADE_SFX_TAP);
		drag(s, ctx, x, y);
		return;
	}
	if (g->selected < 0) return;
	drag(s, ctx, x, y);
	int cur_score = arcade_shell_score(s);
	place_outcome_t out = g->snapping ? place(g, g->selected, g->col, g->row, cur_score) : (place_outcome_t){0};
	cancel(s, ctx);
	if (!out.points) return;

	if (out.all_cleared) {
		arcade_sfx_play(ARCADE_SFX_WIN);
		arcade_shell_show_callout(s, tr("arcade_board_cleared"), lv_color_hex(0xF7C552));
	} else if (out.prism_blasts > 0) {
		arcade_sfx_play(ARCADE_SFX_SMASH);
		if (g->streak >= 2) {
			char msg[48];
			snprintf(msg, sizeof(msg), tr("arcade_combo"), g->streak);
			arcade_shell_show_callout(s, msg, lv_color_hex(0x78E8EB));
		}
	} else if (out.lines > 0) {
		if (g->streak >= 2) {
			arcade_sfx_play_pitch(ARCADE_SFX_COMBO, LV_MIN(g->streak, 7));
			char msg[48];
			snprintf(msg, sizeof(msg), tr("arcade_combo"), g->streak);
			arcade_shell_show_callout(s, msg, lv_color_hex(0xF7C552));
		} else {
			arcade_sfx_play_pitch(ARCADE_SFX_COMBO, LV_MIN(out.lines, 4));
		}
	} else {
		arcade_sfx_play(ARCADE_SFX_TAP);
	}

	if (out.goal_met) arcade_shell_show_callout(s, "+30", lv_color_hex(0xf6d78e));
	arcade_shell_set_score(s, cur_score + out.points);
	sync_sub_hud(s, g);

	g->animating = true;
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, g);
	lv_anim_set_exec_cb(&a, fade_exec);
	lv_anim_set_values(&a, 0, 255);
	lv_anim_set_duration(&a, 220);
	lv_anim_set_completed_cb(&a, fade_done);
	lv_anim_start(&a);

	if (!can_play(g)) {
		clear_mosaic_saved(g);
		config_save();
		g->config_dirty = false;
		arcade_shell_trigger_game_over(s, 260);
	} else {
		save_mosaic_state(s, g);
	}
}

static void draw_prism_emblem(arcade_fb_t *fb, int cx, int cy, int rad) {
	float d = (float)rad;
	float qx[4] = {(float)cx, (float)cx + d, (float)cx, (float)cx - d};
	float qy[4] = {(float)cy - d, (float)cy, (float)cy + d, (float)cy};
	arcade_fb_fill_quad(fb, qx, qy, 0xFFE6, 255);
	if (rad >= 4) {
		arcade_fb_fill_circle(fb, cx, cy, 1, 0xFFFF);
	}
}

static void ceramic(arcade_fb_t *fb, int x, int y, int size, uint16_t color) {
	int radius = LV_MIN(4, size/3);
	arcade_fb_fill_rrect(fb, x, y, size, size, radius, arcade_blend565(color, 0x0888, 100));
	arcade_fb_fill_rrect(fb, x+1, y, size-2, size-2, radius, color);
	arcade_fb_hline(fb, x+radius, x+size-radius-1, y+1, 0xffff, 160);
	if (size > 12) arcade_fb_hline(fb, x+3, x+size/2, y+3, 0xffff, 85);
}

static void draw_shape(arcade_fb_t *fb, int piece, int x, int y, int size, uint16_t color, bool has_prism) {
	const shape_t *shape = &shapes[piece];
	int pr = -1, pc = -1;
	if (has_prism) first_solid_cell(piece, &pr, &pc);
	for (int r = 0; r < shape->h; r++) {
		for (int c = 0; c < shape->w; c++) {
			if (shape->row[r] & (1u << c)) {
				int bx = x + c * size, by = y + r * size + 2, bs = size - 3;
				ceramic(fb, bx, by, bs, color);
				if (r == pr && c == pc) {
					draw_prism_emblem(fb, bx + bs / 2, by + bs / 2, LV_MAX(2, bs / 4));
				}
			}
		}
	}
}

static void render(arcade_shell_t *s, void *ctx, bool full) {
	(void)full;
	mosaic_t *g = ctx;
	arcade_fb_t *fb = arcade_shell_fb(s);
	arcade_fb_mark_full(fb);

	scene_backdrop(fb, scene_enamel, 0, fb->h-1);

	uint16_t frame_c = (g->streak >= 2) ? arcade_blend565(0x0886, 0x3b72, 130) : 0x0886;
	arcade_fb_fill_rrect(fb, BOARD_X - 5, BOARD_Y - 5, CELL * GRID + 10, CELL * GRID + 11, 6, frame_c);

	for (int r = 0; r < GRID; r++) {
		for (int c = 0; c < GRID; c++) {
			int x = BOARD_X + c * CELL, y = BOARD_Y + r * CELL;
			arcade_fb_fill_rrect(fb, x, y, CELL - 1, CELL - 1, 3, 0x192b);
			uint8_t raw = g->cells[r][c];
			int index = (int)(raw & CELL_COLOR_MASK), inset = 0;
			bool is_prism = (raw & CELL_PRISM_BIT) != 0;
			uint16_t color;
			if (g->animating && g->flash[r][c]) {
				color = arcade_blend565(0xffd6, colors[g->flash[r][c] - 1], (uint8_t)g->fade);
				inset = g->fade * 11 / 255;
			} else if (index) {
				color = colors[index - 1];
				int dx = c - g->placed_x, dy = r - g->placed_y;
				if (g->animating && g->placed_shape >= 0 && dx >= 0 && dy >= 0 &&
					dx < shapes[g->placed_shape].w && dy < shapes[g->placed_shape].h &&
					(shapes[g->placed_shape].row[dy] & (1u << dx))) {
					color = arcade_blend565(color, 0xffff, (uint8_t)((255 - g->fade) / 2));
				}
			} else {
				continue;
			}
			int bs = CELL - 3 - 2 * inset;
			if (bs > 2) {
				ceramic(fb, x + inset, y + inset + 2, bs, color);
				if (is_prism && inset == 0) {
					draw_prism_emblem(fb, x + bs / 2, y + 2 + bs / 2, 5);
				}
			}
		}
	}

	for (int i = 0; i < 3; i++) {
		int x = 5 + i * 79, piece = g->tray[i];
		arcade_fb_fill_rrect(fb, x, TRAY_Y, 72, 60, 7, i == g->selected ? 0x3a92 : 0x1129);
		if (piece >= 0 && i != g->selected) {
			const shape_t *shape = &shapes[piece];
			draw_shape(fb, piece, x + (72 - shape->w * 10) / 2, TRAY_Y + (60 - shape->h * 10) / 2, 10,
					   colors[color_index(piece) - 1], i == g->prism_slot);
		}
	}
	if (g->selected >= 0) {
		int piece = g->tray[g->selected];
		bool has_prism = (g->selected == g->prism_slot);
		if (g->snapping) {
			bool valid = fits(g, piece, g->col, g->row);
			uint16_t edge = valid ? 0xe7ff : 0xfb2e;
			for (int r = 0; r < shapes[piece].h; r++) {
				for (int c = 0; c < shapes[piece].w; c++) {
					if (!(shapes[piece].row[r] & (1u << c))) continue;
					int x = BOARD_X + (g->col+c)*CELL, y = BOARD_Y + (g->row+r)*CELL;
					arcade_fb_fill_rrect_alpha(fb, x+1, y+1, CELL-3, CELL-3, 4, edge, 90);
					arcade_fb_hline(fb, x+3, x+CELL-5, y+1, edge, 255);
					arcade_fb_hline(fb, x+3, x+CELL-5, y+CELL-3, edge, 255);
					arcade_fb_vline(fb, x+1, y+3, y+CELL-5, edge, 255);
					arcade_fb_vline(fb, x+CELL-3, y+3, y+CELL-5, edge, 255);
				}
			}
		} else {
			draw_shape(fb, piece, g->finger_x - shapes[piece].w * 7, g->finger_y - shapes[piece].h * 7 - 12, 14,
					   colors[color_index(piece) - 1], has_prism);
		}
	}

	arcade_particles_draw(&g->sparks, fb, 0.0f);
}

void game_blockmosaic_open(gui_config_t *cfg) {
	static const arcade_game_ops_t ops = {
		.id = "blockmosaic",
		.title_tag = "game_blockmosaic",
		.subtitle_tag = "game_blockmosaic_desc",
		.hint_tag = "game_blockmosaic_hint",
		.icon = &icon_atelier_blockblast,
		.use_framebuffer = true,
		.compact_scene = true,
		.dark_backdrop = true,
		.on_enter = enter_attract,
		.on_exit = leave,
		.on_start = start_fresh,
		.has_saved_run = mosaic_has_saved,
		.on_resume_saved = mosaic_resume_saved,
		.on_step = step,
		.is_idle = mosaic_idle,
		.on_render = render,
		.on_touch = touch,
		.on_drag = drag,
		.on_cancel = cancel,
	};
	if (!shell) shell = arcade_shell_create(cfg, &ops, &puzzle);
	arcade_shell_open(shell);
}
