#include "arcade_scene.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROWS 6
#define COLS 6
#define PADDLE_Y 341
#define BALL_R 4
#define MAX_BALLS 3
#define MAX_DROPS 3
#define MAX_LIVES 5

typedef enum {
	BRICK_NONE = 0,
	BRICK_GLASS = 1,
	BRICK_ARMOR = 2,   // 2-HP reinforced silver-rimmed brick
	BRICK_CRACKED = 3, // 1-HP damaged reinforced brick
	BRICK_PRISM = 4,   // Explosive 3x3 reef core
} brick_kind_t;

typedef enum {
	DROP_WIDE = 0,
	DROP_MULTI = 1,
	DROP_PIERCE = 2,
	DROP_LIFE = 3,
} drop_kind_t;

typedef struct {
	float x, y, vx, vy;
	bool active;
} bw_ball_t;

typedef struct {
	float x, y;
	drop_kind_t kind;
	bool active;
} bw_drop_t;

typedef struct {
	uint8_t bricks[ROWS][COLS];
	bw_ball_t balls[MAX_BALLS];
	bw_drop_t drops[MAX_DROPS];
	int remaining, wave, lives, chain;
	uint8_t stage_order[8];
	float paddle, target;
	float wide_t, pierce_t, time_s;
	bool served, lost;
	arcade_particles_t sparks;
	uint32_t rng;
} breaker_t;

static breaker_t game;
static arcade_shell_t *shell;
static const uint16_t glass[] = {0x659c, 0x3519, 0x3d7a, 0x3478, 0x451a, 0x563b};

// 8 handcrafted symmetric reef layouts ('1'=glass, '2'=2-HP armor, 'P'=explosive prism, '.'=open)
static const char *const REEF_STAGES[8][ROWS] = {
	{
		".1111.",
		"111111",
		"11PP11",
		"111111",
		"1.11.1",
		"......",
	},
	{
		"21..12",
		"111111",
		"1P11P1",
		"112211",
		".1111.",
		"......",
	},
	{
		"..22..",
		".2112.",
		"21PP12",
		".2112.",
		"11..11",
		".1111.",
	},
	{
		"12.121",
		"12.121",
		"1P.1P1",
		"11.111",
		"22.222",
		"......",
	},
	{
		"111111",
		"1P22P1",
		"111111",
		"222222",
		".1111.",
		"......",
	},
	{
		"2P11P2",
		"121121",
		".1PP1.",
		"121121",
		"21..12",
		".1111.",
	},
	{
		"22..22",
		"122221",
		".1PP1.",
		"..22..",
		".1111.",
		"11..11",
	},
	{
		"2P22P2",
		"211112",
		"12PP21",
		"211112",
		"122221",
		".1PP1.",
	},
};

static float paddle_half_w(const breaker_t *b) { return b->wide_t > 0.0f ? 38.0f : 26.0f; }

static float ball_speed(const breaker_t *b) {
	return 142.0f + fminf(92.0f, (float)b->wave * 8.5f + (float)(32 - LV_MIN(32, b->remaining)) * 0.9f);
}

static bool particles_alive(const arcade_particles_t *p) {
	for (int i = 0; i < ARCADE_PARTICLES_MAX; i++) {
		if (p->items[i].active) return true;
	}
	return false;
}

static bool drops_alive(const breaker_t *b) {
	for (int i = 0; i < MAX_DROPS; i++) {
		if (b->drops[i].active) return true;
	}
	return false;
}

static void sync_hud(arcade_shell_t *s, const breaker_t *b) {
	if (!s || arcade_shell_state(s) == ARCADE_ST_TITLE) return;
	char lvl[32], liv[32], sub[80];
	snprintf(lvl, sizeof(lvl), tr("arcade_level"), b->wave + 1);
	snprintf(liv, sizeof(liv), tr("arcade_lives"), b->lives);
	snprintf(sub, sizeof(sub), "%s  ·  %s", lvl, liv);
	arcade_shell_set_sub_hud(s, sub);
}

static void park_single_ball(breaker_t *b) {
	memset(b->balls, 0, sizeof(b->balls));
	b->served = false;
	b->chain = 0;
	b->balls[0] = (bw_ball_t){.x = b->paddle, .y = PADDLE_Y - 9.0f, .active = true};
}

static void rack(breaker_t *b) {
	b->remaining = 0;
	memset(b->bricks, 0, sizeof(b->bricks));
	memset(b->drops, 0, sizeof(b->drops));
	if (b->wave % 8 == 0) {
		for (int i = 0; i < 8; i++) b->stage_order[i] = i;
		// Shuffle the four introductory reefs and the four armored reefs independently.
		for (int i = 0; i < 8; i++) {
			int j = i + arcade_rng_next(&b->rng) % (4 - i % 4);
			uint8_t swap = b->stage_order[i];
			b->stage_order[i] = b->stage_order[j];
			b->stage_order[j] = swap;
		}
	}
	int stage_idx = b->stage_order[b->wave % 8];
	bool mirror = arcade_rng_next(&b->rng) & 1;
	bool ng_plus = b->wave >= 8;

	for (int r = 0; r < ROWS; r++) {
		const char *line = REEF_STAGES[stage_idx][r];
		for (int c = 0; c < COLS; c++) {
			char ch = line[mirror ? COLS - 1 - c : c];
			uint8_t k = BRICK_NONE;
			if (ch == '1') k = (ng_plus && ((r + c) % 4 == 0)) ? BRICK_ARMOR : BRICK_GLASS;
			else if (ch == '2') k = BRICK_ARMOR;
			else if (ch == 'P') k = BRICK_PRISM;
			b->bricks[r][c] = k;
			if (k != BRICK_NONE) b->remaining++;
		}
	}
	// Put a prism at the first exposed brick on a side wall for an optional bank shot.
	int wall = (arcade_rng_next(&b->rng) & 1) ? COLS - 1 : 0;
	for (int r = ROWS - 1; r >= 0; r--) {
		if (b->bricks[r][wall] != BRICK_NONE) {
			b->bricks[r][wall] = BRICK_PRISM;
			break;
		}
	}
	park_single_ball(b);
}

static void enter_attract(arcade_shell_t *s, void *ctx) {
	(void)s;
	breaker_t *b = ctx;
	*b = (breaker_t){.paddle = 120.0f, .target = 120.0f, .lives = 3, .rng = arcade_rng_seed()};
	arcade_particles_clear(&b->sparks);
	rack(b);
}

static void start_run(arcade_shell_t *s, void *ctx) {
	breaker_t *b = ctx;
	*b = (breaker_t){.paddle = 120.0f, .target = 120.0f, .lives = 3, .rng = arcade_rng_seed()};
	arcade_particles_clear(&b->sparks);
	rack(b);
	sync_hud(s, b);
}

static void serve(breaker_t *b) {
	b->served = true;
	uint32_t r = arcade_rng_next(&b->rng);
	float sign = (r & 1u) ? 1.0f : -1.0f;
	float angle = sign * (0.22f + 0.01f * (float)((r >> 1) % 22u));
	float spd = ball_speed(b);
	b->balls[0].active = true;
	b->balls[0].vx = spd * arcade_sin_ang(angle);
	b->balls[0].vy = -spd * arcade_cos_ang(angle);
}

static void aim(arcade_shell_t *s, void *ctx, int x, int y) {
	(void)y;
	breaker_t *b = ctx;
	float hw = paddle_half_w(b);
	float min_x = 8.0f + hw;
	float max_x = 232.0f - hw;
	float target = fminf(max_x, fmaxf(min_x, (float)x * 240.0f / (float)arcade_shell_width(s)));
	if (target != b->target) {
		b->target = target;
		arcade_shell_mark_dirty(s);
	}
}

static void touch(arcade_shell_t *s, void *ctx, bool down, int x, int y) {
	if (!down) return;
	breaker_t *b = ctx;
	aim(s, ctx, x, y);
	if (!b->served && !b->lost) {
		arcade_sfx_play(ARCADE_SFX_BOUNCE);
		serve(b);
		arcade_shell_mark_dirty(s);
	}
}

static bool waiting_for_serve(const void *ctx) {
	const breaker_t *b = ctx;
	return !b->served && fabsf(b->target - b->paddle) < 0.01f && !particles_alive(&b->sparks) && !drops_alive(b) &&
		   b->wide_t <= 0.0f && b->pierce_t <= 0.0f;
}

static void maybe_spawn_drop(breaker_t *b, float x, float y) {
	if ((arcade_rng_next(&b->rng) % 100u) >= 19u) return;
	int slot = -1;
	for (int i = 0; i < MAX_DROPS; i++) {
		if (!b->drops[i].active) {
			slot = i;
			break;
		}
	}
	if (slot < 0) return;

	uint32_t pick = arcade_rng_next(&b->rng) % 100u;
	drop_kind_t kind = DROP_WIDE;
	if (pick < 34u) kind = DROP_WIDE;
	else if (pick < 66u) kind = DROP_MULTI;
	else if (pick < 88u || b->lives >= MAX_LIVES) kind = DROP_PIERCE;
	else kind = DROP_LIFE;

	b->drops[slot] = (bw_drop_t){.x = x, .y = y, .kind = kind, .active = true};
}

static void apply_powerup(arcade_shell_t *s, breaker_t *b, drop_kind_t kind) {
	arcade_sfx_play(ARCADE_SFX_POWERUP);
	arcade_shell_set_score(s, arcade_shell_score(s) + 5);

	switch (kind) {
	case DROP_WIDE:
		b->wide_t = 14.0f;
		aim(s, b, (int)(b->target * (float)arcade_shell_width(s) / 240.0f), 0);
		arcade_shell_show_callout(s, tr("arcade_power_wide"), lv_color_hex(0x4CE0B8));
		break;
	case DROP_MULTI: {
		arcade_shell_show_callout(s, tr("arcade_power_multi"), lv_color_hex(0xF7C552));
		int src = -1;
		for (int i = 0; i < MAX_BALLS; i++) {
			if (b->balls[i].active) {
				src = i;
				break;
			}
		}
		if (src < 0) {
			park_single_ball(b);
			serve(b);
			src = 0;
		}
		float spd = ball_speed(b);
		static const float fan[2] = {-0.48f, 0.48f};
		int spawned = 0;
		for (int i = 0; i < MAX_BALLS && spawned < 2; i++) {
			if (!b->balls[i].active) {
				float a = fan[spawned++];
				b->balls[i] = (bw_ball_t){
					.x = b->balls[src].x,
					.y = b->balls[src].y,
					.vx = spd * arcade_sin_ang(a),
					.vy = -spd * arcade_cos_ang(a),
					.active = true,
				};
			}
		}
		b->served = true;
		break;
	}
	case DROP_PIERCE:
		b->pierce_t = 9.0f;
		arcade_shell_show_callout(s, tr("arcade_power_pierce"), lv_color_hex(0xFF647C));
		break;
	case DROP_LIFE:
		if (b->lives < MAX_LIVES) b->lives++;
		sync_hud(s, b);
		arcade_shell_show_callout(s, tr("arcade_power_life"), lv_color_hex(0x68E898));
		break;
	}
}

static void damage_brick(arcade_shell_t *s, breaker_t *b, int row, int col, bool demo, int *pts_accum) {
	uint8_t k = b->bricks[row][col];
	if (k == BRICK_NONE) return;

	float cx = (float)(14 + col * 36 + 15);
	float cy = (float)(92 + row * 18 + 6);

	if (k == BRICK_ARMOR && b->pierce_t <= 0.0f) {
		b->bricks[row][col] = BRICK_CRACKED;
		if (!demo) {
			*pts_accum += 1;
			arcade_sfx_play(ARCADE_SFX_BOUNCE);
		}
		arcade_particles_spawn_burst(&b->sparks, cx, cy, 5, 14.0f, 52.0f, 75.0f, 1.8f, 0.30f, 0xDF1E, 1);
		return;
	}

	b->bricks[row][col] = BRICK_NONE;
	if (b->remaining > 0) b->remaining--;
	b->chain++;

	uint16_t spark_c = (k == BRICK_PRISM) ? 0xFFE6 : glass[(row + b->wave) % LV_ARRAYLEN(glass)];
	arcade_particles_spawn_burst(&b->sparks, cx, cy, k == BRICK_PRISM ? 14 : 7, 15.0f, 65.0f, 75.0f, 2.2f, 0.42f,
								 spark_c, 1);

	if (!demo) {
		int loop_bonus = 1 + (b->wave / 8);
		*pts_accum += LV_MIN(8, b->chain) * loop_bonus;
		maybe_spawn_drop(b, cx, cy);
	}

	if (k == BRICK_PRISM) {
		if (!demo) arcade_sfx_play(ARCADE_SFX_SMASH);
		arcade_particles_spawn_ring(&b->sparks, cx, cy, 5.0f, 130.0f, 0.34f, 0xFFE6);
		for (int dr = -1; dr <= 1; dr++) {
			for (int dc = -1; dc <= 1; dc++) {
				int nr = row + dr, nc = col + dc;
				if (nr >= 0 && nr < ROWS && nc >= 0 && nc < COLS && b->bricks[nr][nc] != BRICK_NONE) {
					damage_brick(s, b, nr, nc, demo, pts_accum);
				}
			}
		}
	} else if (!demo) {
		arcade_sfx_play_pitch(ARCADE_SFX_POP, LV_MIN(7, b->chain - 1));
	}
}

static void advance_one_ball(arcade_shell_t *s, breaker_t *b, bw_ball_t *ball, float dt, bool demo) {
	float old_x = ball->x, old_y = ball->y;
	ball->x += ball->vx * dt;
	ball->y += ball->vy * dt;
	if (ball->x < 12.0f) {
		ball->x = 12.0f;
		ball->vx = fabsf(ball->vx);
	}
	if (ball->x > 228.0f) {
		ball->x = 228.0f;
		ball->vx = -fabsf(ball->vx);
	}
	if (ball->y < 79.0f) {
		ball->y = 79.0f;
		ball->vy = fabsf(ball->vy);
	}

	float hw = paddle_half_w(b);
	if (ball->vy > 0.0f && old_y + BALL_R <= PADDLE_Y && ball->y + BALL_R >= PADDLE_Y &&
		fabsf(ball->x - b->paddle) <= hw + BALL_R) {
		float offset = fmaxf(-1.0f, fminf(1.0f, (ball->x - b->paddle) / hw));
		float spd = ball_speed(b);
		ball->vx = spd * arcade_sin_ang(offset * 1.12f);
		ball->vy = -spd * arcade_cos_ang(offset * 1.12f);
		ball->y = PADDLE_Y - BALL_R;
		b->chain = 0;
		if (!demo) arcade_sfx_play(ARCADE_SFX_BOUNCE);
	}

	for (int row = 0; row < ROWS; row++) {
		int y = 92 + row * 18;
		if (ball->y + BALL_R < (float)y || ball->y - BALL_R > (float)(y + 12)) continue;
		for (int col = 0; col < COLS; col++) {
			int x = 14 + col * 36;
			uint8_t k = b->bricks[row][col];
			if (k == BRICK_NONE || ball->x + BALL_R < (float)x || ball->x - BALL_R > (float)(x + 30)) continue;

			bool pierce = (b->pierce_t > 0.0f);
			if (!pierce) {
				if (old_x + BALL_R <= (float)x || old_x - BALL_R >= (float)(x + 30)) {
					ball->vx = -ball->vx;
					ball->x = old_x;
				} else {
					ball->vy = -ball->vy;
					ball->y = old_y;
				}
			}

			int pts = 0;
			damage_brick(s, b, row, col, demo, &pts);
			if (!demo && pts > 0) {
				arcade_shell_set_score(s, arcade_shell_score(s) + pts);
			}

			if (b->remaining <= 0) {
				b->wave++;
				if (!demo) {
					arcade_sfx_play(ARCADE_SFX_WIN);
					arcade_shell_set_score(s, arcade_shell_score(s) + 15 * b->wave);
					arcade_shell_show_callout(s, tr("arcade_level_clear"), lv_color_hex(0x4CE0B8));
				}
				rack(b);
				sync_hud(s, b);
				if (demo) serve(b);
			}
			return;
		}
	}

	if (ball->y > 399.0f) {
		ball->active = false;
	}
}

static void step(arcade_shell_t *s, void *ctx, float dt) {
	breaker_t *b = ctx;
	bool demo = arcade_shell_state(s) == ARCADE_ST_TITLE;
	if (b->lost) {
		arcade_particles_update(&b->sparks, dt);
		return;
	}
	b->time_s += dt;

	if (b->wide_t > 0.0f) {
		b->wide_t = fmaxf(0.0f, b->wide_t - dt);
	}
	if (b->pierce_t > 0.0f) {
		b->pierce_t = fmaxf(0.0f, b->pierce_t - dt);
	}

	if (demo) {
		float lead_x = b->balls[0].active ? b->balls[0].x : 120.0f;
		float lead_y = b->balls[0].active ? b->balls[0].y : 200.0f;
		b->target = fmaxf(34.0f, fminf(206.0f, lead_x + arcade_sin_ang(lead_y * 0.03f) * 13.0f));
		if (!b->served) serve(b);
	}

	float hw = paddle_half_w(b);
	b->target = fminf(232.0f - hw, fmaxf(8.0f + hw, b->target));
	b->paddle += (b->target - b->paddle) * fminf(1.0f, dt * 20.0f);
	arcade_particles_update(&b->sparks, dt);

	// Update falling power-up capsules
	for (int i = 0; i < MAX_DROPS; i++) {
		bw_drop_t *d = &b->drops[i];
		if (!d->active) continue;
		d->y += 76.0f * dt;
		if (d->y >= PADDLE_Y - 5.0f && d->y <= PADDLE_Y + 10.0f && fabsf(d->x - b->paddle) <= hw + 8.0f) {
			d->active = false;
			if (!demo) apply_powerup(s, b, d->kind);
		} else if (d->y > 402.0f) {
			d->active = false;
		}
	}

	if (!b->served) {
		b->balls[0].x = b->paddle;
		b->balls[0].y = PADDLE_Y - 9.0f;
		return;
	}

	int substeps = 1 + (int)(dt * ball_speed(b) / 3.0f);
	for (int st = 0; st < substeps && b->served && !b->lost; st++) {
		float sub_dt = dt / (float)substeps;
		for (int i = 0; i < MAX_BALLS && b->served; i++) {
			if (b->balls[i].active) {
				advance_one_ball(s, b, &b->balls[i], sub_dt, demo);
			}
		}
	}

	if (b->served) {
		int alive = 0;
		for (int i = 0; i < MAX_BALLS; i++) {
			if (b->balls[i].active) alive++;
		}
		if (alive == 0) {
			if (demo) {
				enter_attract(s, b);
			} else {
				b->lives--;
				b->wide_t = 0.0f;
				b->pierce_t = 0.0f;
				arcade_sfx_play(ARCADE_SFX_MISS);
				if (b->lives <= 0) {
					b->lost = true;
					sync_hud(s, b);
					arcade_shell_trigger_game_over(s, 180);
				} else {
					park_single_ball(b);
					sync_hud(s, b);
					arcade_shell_show_callout(s, tr("arcade_tap_launch"), theme()->accent);
				}
			}
		}
	}
}

static uint16_t drop_color(drop_kind_t k) {
	switch (k) {
	case DROP_WIDE:
		return 0x4F17;
	case DROP_MULTI:
		return 0xFE2A;
	case DROP_PIERCE:
		return 0xFB2F;
	case DROP_LIFE:
	default:
		return 0x6753;
	}
}

static void render(arcade_shell_t *s, void *ctx, bool full) {
	(void)full;
	breaker_t *b = ctx;
	arcade_fb_t *fb = arcade_shell_fb(s);
	arcade_fb_mark_full(fb);

	scene_backdrop(fb, scene_reef, 0, fb->h - 1);
	arcade_fb_vline(fb, 6, 76, 354, 0xa757, 130);
	arcade_fb_vline(fb, 234, 76, 354, 0xa757, 130);

	// Life pips along top-left reef rim
	if (arcade_shell_state(s) != ARCADE_ST_TITLE) {
		for (int i = 0; i < b->lives; i++) {
			arcade_fb_fill_circle(fb, 15 + i * 9, 82, 2, 0x7FF9);
		}
	}

	for (int row = 0; row < ROWS; row++) {
		for (int col = 0; col < COLS; col++) {
			uint8_t k = b->bricks[row][col];
			if (k == BRICK_NONE) continue;
			int x = 14 + col * 36, y = 92 + row * 18;
			if (k == BRICK_ARMOR) {
				arcade_fb_fill_rrect(fb, x, y, 32, 13, 4, 0xaeba);
				arcade_fb_hline(fb, x + 7, x + 24, y + 8, 0x5370, 255);
				arcade_fb_hline(fb, x + 2, x + 27, y + 2, 0xFFFF, 190);
				arcade_fb_fill_circle(fb, x + 5, y + 6, 1, 0xE73C);
				arcade_fb_fill_circle(fb, x + 24, y + 6, 1, 0xE73C);
			} else if (k == BRICK_CRACKED) {
				scene_block(fb, x, y, 30, 12, 3, 0x6410);
				arcade_fb_draw_line_thick(fb, x + 8, y + 2, x + 15, y + 7, 1, 0x2104, 220);
				arcade_fb_draw_line_thick(fb, x + 15, y + 7, x + 22, y + 10, 1, 0x2104, 220);
			} else if (k == BRICK_PRISM) {
				uint8_t shimmer = (uint8_t)(120.0f + 90.0f * arcade_sin_ang(b->time_s * 5.0f + (float)col));
				uint16_t pc = arcade_blend565(0xEC2E, 0xFFE8, shimmer);
				arcade_fb_fill_rrect(fb, x, y, 32, 12, 5, pc);
				float qx[4] = {(float)(x + 15), (float)(x + 19), (float)(x + 15), (float)(x + 11)};
				float qy[4] = {(float)(y + 2), (float)(y + 6), (float)(y + 10), (float)(y + 6)};
				arcade_fb_fill_quad(fb, qx, qy, 0xFFFF, 235);
			} else {
				arcade_fb_fill_rrect_alpha(fb, x, y, 32, 13, 2, glass[(row + b->wave) % LV_ARRAYLEN(glass)], 208);
				arcade_fb_hline(fb, x+2, x+29, y+1, 0xbfff, 250);
				arcade_fb_hline(fb, x + 3, x + 13, y + 3, 0xFFFF, 140);
			}
		}
	}

	// Falling power-up capsules
	for (int i = 0; i < MAX_DROPS; i++) {
		const bw_drop_t *d = &b->drops[i];
		if (!d->active) continue;
		uint16_t dc = drop_color(d->kind);
		int dx = (int)lroundf(d->x), dy = (int)lroundf(d->y);
		arcade_fb_fill_rrect_alpha(fb, dx - 9, dy - 5, 18, 10, 4, dc, 80);
		arcade_fb_fill_rrect(fb, dx - 7, dy - 3, 14, 7, 3, dc);
		arcade_fb_hline(fb, dx - 4, dx + 4, dy - 2, 0xFFFF, 200);
	}

	int pw = (int)lroundf(paddle_half_w(b) * 2.0f);
	int px = (int)lroundf(b->paddle) - pw / 2;
	uint16_t pad_c = b->pierce_t > 0.0f ? 0xFB2F : (b->wide_t > 0.0f ? 0x4F17 : 0x6e17);
	arcade_fb_fill_rrect(fb, px + 1, PADDLE_Y + 11, pw + 2, 4, 2, 0x116a);
	arcade_fb_fill_rrect(fb, px, PADDLE_Y - 2, pw, 9, 4, pad_c);
	arcade_fb_hline(fb, px+4, px+pw-5, PADDLE_Y, 0xffff, 210);

	// Active power-up duration bars beneath paddle
	if (b->wide_t > 0.0f) {
		int bw_px = LV_MAX(2, (int)lroundf((b->wide_t / 14.0f) * (float)pw));
		arcade_fb_fill_rect(fb, px, PADDLE_Y + 16, bw_px, 2, 0x4F17);
	}
	if (b->pierce_t > 0.0f) {
		int bp_px = LV_MAX(2, (int)lroundf((b->pierce_t / 9.0f) * (float)pw));
		arcade_fb_fill_rect(fb, px, PADDLE_Y + 19, bp_px, 2, 0xFB2F);
	}

	uint16_t ball_c = b->pierce_t > 0.0f ? 0xFD20 : 0xff77;
	for (int i = 0; i < MAX_BALLS; i++) {
		if (!b->balls[i].active) continue;
		int bx = (int)lroundf(b->balls[i].x), by = (int)lroundf(b->balls[i].y);
		if (b->pierce_t > 0.0f) {
			arcade_fb_fill_circle_alpha(fb, bx, by, BALL_R + 3, 0xFB00, 120);
		}
		scene_orb(fb, bx, by, BALL_R, ball_c);
	}

	arcade_particles_draw(&b->sparks, fb, 0.0f);
}

void game_breakwater_open(gui_config_t *cfg) {
	static const arcade_game_ops_t ops = {
		.id = "breakwater",
		.title_tag = "game_breakwater",
		.subtitle_tag = "game_breakwater_desc",
		.hint_tag = "game_breakwater_hint",
		.icon = &icon_atelier_breakwater,
		.use_framebuffer = true,
		.compact_scene = true,
		.dark_backdrop = true,
		.on_enter = enter_attract,
		.on_start = start_run,
		.on_step = step,
		.is_idle = waiting_for_serve,
		.on_render = render,
		.on_touch = touch,
		.on_drag = aim,
	};
	if (!shell) shell = arcade_shell_create(cfg, &ops, &game);
	arcade_shell_open(shell);
}
