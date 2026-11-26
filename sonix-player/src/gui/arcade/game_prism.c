#include "game_prism.h"
#include "arcade_scene.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "src/gui/arcade/arcade_fb.h"
#include "src/gui/arcade/arcade_shell.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define TWO_PI_F (2.0f * (float)M_PI)
#define HALF_PI_F (0.5f * (float)M_PI)

#define SCREEN_W 480
#define MAX_SCREEN_H 804
#define PRISM_OBS_MAX 8
// A tap rises at most 81 px. Leave space for that arc and the 24 px orb.
#define REST_CLEARANCE 144.0f
#define GATE_OFFSET 82
#define BAR_HALF_H 9
#define BAR_WIDTH (SCREEN_W / 4)
#define BAR_PIXELS_PER_RAD 76.0f
#define ORB_X 240.0f
#define ORB_R 12.0f
#define ORB_TRAIL 10
#define GRAVITY 1020.0f
#define JUMP_VY -405.0f
#define MAX_FALL_VY 560.0f
#define FEVER_STARS 5

typedef enum {
	PRISM_OBS_RING = 0,
	PRISM_OBS_BARS = 1,
	PRISM_OBS_CROSS = 2,
	PRISM_OBS_DOUBLE = 3,
	PRISM_OBS_DIAMOND = 4,
	PRISM_OBS_GATES = 5,
} prism_obs_kind_t;

typedef struct {
	prism_obs_kind_t kind;
	float world_y;
	float cx;
	float r_in;
	float r_out;
	float rot;
	float rot_speed;
	bool has_star;
	bool star_taken;
	bool has_shifter;
	float shifter_y;
	bool shifter_taken;
	bool active;
} prism_obs_t;

typedef struct {
	arcade_shell_t *shell;
	prism_obs_t obs[PRISM_OBS_MAX];
	arcade_particles_t particles;

	float orb_y;
	float orb_vy;
	int ball_color;
	int fast_combo;
	int fever_charge;
	float fever_t;
	float post_fever_grace;
	float since_star_s;
	bool launched;
	bool dead;
	float cam_y;
	float next_bottom_y;
	float time_s;
	float shake_t;

	float trail_x[ORB_TRAIL];
	float trail_y[ORB_TRAIL];
	int trail_head;
	int trail_fill;
	uint32_t rng;

	// Dirty-band tracking to avoid full 480x800 redraws when camera is stationary
	int prev_cam_px;
	int prev_min_y;
	int prev_max_y;
	bool has_prev_bounds;

	uint8_t bag[6];
	int bag_left, dealt;
} prism_ctx_t;

static prism_ctx_t g_prism;

static uint32_t next_rng(prism_ctx_t *ctx) { return arcade_rng_next(&ctx->rng); }

static int panel_h(const prism_ctx_t *ctx) {
	int h = ctx->shell ? arcade_shell_height(ctx->shell) : 800;
	return h > 0 ? LV_MIN(h, MAX_SCREEN_H - 1) : 800;
}

static uint16_t prism_color_565(int idx, bool dark) {
	static const uint8_t DARK_C[4][3] = {
		{100, 180, 255}, // Ice Blue
		{76, 224, 184},	 // Mint Cyan
		{247, 197, 82},	 // Warm Amber
		{242, 100, 124}, // Studio Coral
	};
	static const uint8_t LIGHT_C[4][3] = {
		{37, 120, 212}, // Cobalt Blue
		{14, 155, 120}, // Emerald Teal
		{212, 138, 8},	// Burnished Amber
		{212, 55, 82},	// Rose Coral
	};
	const uint8_t *rgb = dark ? DARK_C[idx & 3] : LIGHT_C[idx & 3];
	return arcade_rgb565(rgb[0], rgb[1], rgb[2]);
}

static lv_color_t prism_lv_color(int idx) {
	static const uint32_t HEX[4] = {0x64B4FF, 0x4CE0B8, 0xF7C552, 0xF2647C};
	return lv_color_hex(HEX[idx & 3]);
}

int prism_segment_color_at_angle(float angle_rad, float ring_rot_rad) {
	float rel = fmodf(angle_rad - ring_rot_rad, TWO_PI_F);
	if (rel < 0.0f) {
		rel += TWO_PI_F;
	}
	int seg = (int)(rel / HALF_PI_F);
	return seg & 3;
}

int prism_check_ring_collision(float bx, float by, float br, int ball_color, float cx, float cy, float r_in,
							   float r_out, float rot_rad) {
	float dx = bx - cx;
	float dy = by - cy;
	float dist = sqrtf(dx * dx + dy * dy);
	if (dist + br < r_in || dist - br > r_out) {
		return 0;
	}
	if (dist < 1e-4f) {
		return 0;
	}
	int seg = prism_segment_color_at_angle(atan2f(dy, dx), rot_rad);
	return (seg == (ball_color & 3)) ? 1 : -1;
}

static float point_seg_dist2(float px, float py, float ax, float ay, float bx, float by) {
	float vx = bx - ax;
	float vy = by - ay;
	float len2 = vx * vx + vy * vy;
	if (len2 < 1e-4f) {
		return (px - ax) * (px - ax) + (py - ay) * (py - ay);
	}
	float t = ((px - ax) * vx + (py - ay) * vy) / len2;
	if (t < 0.0f) t = 0.0f;
	if (t > 1.0f) t = 1.0f;
	float qx = ax + vx * t;
	float qy = ay + vy * t;
	return (px - qx) * (px - qx) + (py - qy) * (py - qy);
}

// Both gate rows use the same four segments. Reflect the upper row across
// the ball's path so matching colours arrive together from opposite sides.
static int bar_shift(float rot) {
	int shift = (int)lroundf(fmodf(rot * BAR_PIXELS_PER_RAD, SCREEN_W));
	return (shift + SCREEN_W) % SCREEN_W;
}

static int check_bar_row_hit(float bx, float by, float br, int ball_color, float row_y, int shift, bool mirrored) {
	if (fabsf(by - row_y) > BAR_HALF_H + br) return 0;
	float x = mirrored ? SCREEN_W - bx : bx;
	float local = fmodf(x - shift + SCREEN_W, SCREEN_W);
	int segment = (int)(local / BAR_WIDTH) & 3;
	return segment == (ball_color & 3) ? 1 : -1;
}

static int check_obs_collision(const prism_obs_t *o, float bx, float by, float br, int ball_color) {
	if (!o->active) return 0;

	if (o->kind == PRISM_OBS_RING) {
		return prism_check_ring_collision(bx, by, br, ball_color, o->cx, o->world_y, o->r_in, o->r_out, o->rot);
	}
	if (o->kind == PRISM_OBS_DOUBLE) {
		// Outer ring uses mirrored color mapping (1 - q) & 3 with -rot so all 4 colors align along ORB_X!
		float dx = bx - o->cx, dy = by - o->world_y;
		float dist = sqrtf(dx * dx + dy * dy);
		if (dist + br >= o->r_out - 18.0f && dist - br <= o->r_out && dist > 1e-4f) {
			int q_out = prism_segment_color_at_angle(atan2f(dy, dx), -o->rot);
			int c_out = (1 - q_out) & 3;
			if (c_out != (ball_color & 3)) return -1;
		}
		return prism_check_ring_collision(bx, by, br, ball_color, o->cx, o->world_y, o->r_in, o->r_in + 16.0f, o->rot);
	}
	if (o->kind == PRISM_OBS_BARS) {
		return check_bar_row_hit(bx, by, br, ball_color, o->world_y, bar_shift(o->rot), false);
	}
	if (o->kind == PRISM_OBS_GATES) {
		int shift = bar_shift(o->rot);
		int bot_hit = check_bar_row_hit(bx, by, br, ball_color, o->world_y + GATE_OFFSET, shift, false);
		if (bot_hit < 0) return -1;
		int top_hit = check_bar_row_hit(bx, by, br, ball_color, o->world_y - GATE_OFFSET, shift, true);
		return top_hit < 0 ? -1 : (bot_hit ? bot_hit : top_hit);
	}
	if (o->kind == PRISM_OBS_DIAMOND) {
		float hub_d2 = (bx - o->cx) * (bx - o->cx) + (by - o->world_y) * (by - o->world_y);
		if (hub_d2 > (o->r_out + br + 12.0f) * (o->r_out + br + 12.0f)) return 0;
		float hw = 7.5f + br;
		for (int q = 0; q < 4; q++) {
			float a0 = o->rot + (float)q * HALF_PI_F;
			float a1 = a0 + HALF_PI_F;
			float x0 = o->cx + arcade_cos_ang(a0) * o->r_out;
			float y0 = o->world_y + arcade_sin_ang(a0) * o->r_out;
			float x1 = o->cx + arcade_cos_ang(a1) * o->r_out;
			float y1 = o->world_y + arcade_sin_ang(a1) * o->r_out;
			if (point_seg_dist2(bx, by, x0, y0, x1, y1) <= hw * hw && q != (ball_color & 3)) {
				return -1;
			}
		}
		return 0;
	}

	// PRISM_OBS_CROSS
	float hub_d2 = (bx - o->cx) * (bx - o->cx) + (by - o->world_y) * (by - o->world_y);
	if (hub_d2 > (o->r_out + br + 8.0f) * (o->r_out + br + 8.0f)) return 0;
	const float arm_hw = 9.0f + br;
	for (int q = 0; q < 4; q++) {
		float a = o->rot + (float)q * HALF_PI_F;
		float ca = arcade_cos_ang(a), sa = arcade_sin_ang(a);
		float ax = o->cx + ca * 12.0f, ay = o->world_y + sa * 12.0f;
		float px2 = o->cx + ca * o->r_out, py2 = o->world_y + sa * o->r_out;
		if (point_seg_dist2(bx, by, ax, ay, px2, py2) <= arm_hw * arm_hw && q != (ball_color & 3)) {
			return -1;
		}
	}
	return 0;
}

static void sync_sub_hud(arcade_shell_t *shell, const prism_ctx_t *ctx) {
	if (!shell || arcade_shell_state(shell) != ARCADE_ST_PLAYING) return;
	char buf[64];
	if (ctx->fever_t > 0.0f) {
		arcade_shell_set_sub_hud(shell, "FEVER!");
	} else if (ctx->fast_combo >= 2) {
		char cbuf[36];
		snprintf(cbuf, sizeof(cbuf), tr("arcade_combo"), ctx->fast_combo);
		snprintf(buf, sizeof(buf), "%s  ·  %d/%d", cbuf, ctx->fever_charge, FEVER_STARS);
		arcade_shell_set_sub_hud(shell, buf);
	} else if (ctx->fever_charge > 0) {
		snprintf(buf, sizeof(buf), "%d / %d", ctx->fever_charge, FEVER_STARS);
		arcade_shell_set_sub_hud(shell, buf);
	} else {
		arcade_shell_set_sub_hud(shell, NULL);
	}
}

static void populate_obstacle(prism_ctx_t *ctx, prism_obs_t *o, float bottom_y, int score) {
	memset(o, 0, sizeof(*o));
	o->active = true;
	o->has_star = true;
	o->has_shifter = true;

	if (ctx->dealt == 0) {
		o->kind = PRISM_OBS_RING;
	} else {
		if (!ctx->bag_left) {
			static const uint8_t families[] = {PRISM_OBS_BARS, PRISM_OBS_GATES, PRISM_OBS_RING,
											  PRISM_OBS_CROSS, PRISM_OBS_DIAMOND, PRISM_OBS_DOUBLE};
			// The first pair always teaches two different motions; later bags add compound gates.
			ctx->bag_left = ctx->dealt == 1 ? 2 : (ctx->dealt < 6 ? 4 : 6);
			memcpy(ctx->bag, families, ctx->bag_left);
		}
		int index = next_rng(ctx) % ctx->bag_left;
		o->kind = ctx->bag[index];
		ctx->bag[index] = ctx->bag[--ctx->bag_left];
	}
	ctx->dealt++;

	float dir = (next_rng(ctx) & 1u) ? 1.0f : -1.0f;
	float spd = 1.05f + fminf(1.00f, (float)score * 0.035f);
	o->rot = ((float)(next_rng(ctx) % 628u)) * 0.01f;
	o->rot_speed = dir * spd;

	if (o->kind == PRISM_OBS_RING) {
		o->cx = ORB_X;
		o->r_in = 72.0f;
		o->r_out = 92.0f;
	} else if (o->kind == PRISM_OBS_BARS || o->kind == PRISM_OBS_GATES) {
		o->cx = ORB_X;
		o->r_in = 0.0f;
		o->r_out = o->kind == PRISM_OBS_GATES ? GATE_OFFSET + BAR_HALF_H : BAR_HALF_H;
		if (o->kind == PRISM_OBS_GATES) o->rot_speed = dir * fminf(spd, 1.2f);
	} else if (o->kind == PRISM_OBS_CROSS) {
		o->cx = (next_rng(ctx) & 1u) ? 162.0f : 318.0f;
		o->r_in = 12.0f;
		o->r_out = 102.0f;
	} else if (o->kind == PRISM_OBS_DIAMOND) {
		o->cx = ORB_X;
		o->r_in = 68.0f;
		o->r_out = 104.0f;
		o->rot_speed *= 0.85f;
	} else {
		// A full bounce fits between the inner band and the outer band.
		o->cx = ORB_X;
		o->r_in = 64.0f;
		o->r_out = 224.0f;
		o->rot_speed *= 0.82f;
	}
	o->world_y = bottom_y - o->r_out;
	float top = o->world_y - o->r_out;
	o->shifter_y = top - REST_CLEARANCE * 0.5f;
	ctx->next_bottom_y = top - REST_CLEARANCE;
}

static void shatter_obstacle(prism_ctx_t *ctx, prism_obs_t *o, bool dark) {
	o->active = false;
	arcade_sfx_play(ARCADE_SFX_SMASH);
	for (int c = 0; c < 4; c++) {
		arcade_particles_spawn_burst(&ctx->particles, ORB_X, o->world_y, 7, 75.0f, 240.0f, 220.0f, 3.6f, 0.52f,
									 prism_color_565(c, dark), 1);
	}
	arcade_particles_spawn_ring(&ctx->particles, ORB_X, o->world_y, 24.0f, 260.0f, 0.40f, 0xFFFF);
}

static void spawn_death_burst(prism_ctx_t *ctx) {
	bool dark = theme_is_dark();
	ctx->dead = true;
	ctx->shake_t = 0.34f;
	for (int c = 0; c < 4; c++) {
		arcade_particles_spawn_burst(&ctx->particles, ORB_X, ctx->orb_y, 9, 75.0f, 260.0f, 320.0f, 3.6f, 0.72f,
									 prism_color_565(c, dark), 0);
	}
	arcade_particles_spawn_ring(&ctx->particles, ORB_X, ctx->orb_y, ORB_R, 210.0f, 0.45f,
								prism_color_565(ctx->ball_color, dark));
}

static void record_trail(prism_ctx_t *ctx) {
	ctx->trail_head = (ctx->trail_head + 1) % ORB_TRAIL;
	ctx->trail_x[ctx->trail_head] = ORB_X;
	ctx->trail_y[ctx->trail_head] = ctx->orb_y;
	if (ctx->trail_fill < ORB_TRAIL) {
		ctx->trail_fill++;
	}
}

static void prism_on_enter(arcade_shell_t *shell, void *arg) {
	prism_ctx_t *ctx = (prism_ctx_t *)arg;
	ctx->shell = shell;
	ctx->rng = arcade_rng_seed();
	ctx->bag_left = 0;
	ctx->dealt = 0;
	ctx->has_prev_bounds = false;
	ctx->time_s = 0.0f;
	ctx->cam_y = 0.0f;
	ctx->orb_y = 216.0f;
	ctx->orb_vy = 0.0f;
	ctx->ball_color = 0;
	ctx->launched = false;
	ctx->dead = false;
	ctx->fever_t = 0.0f;
	ctx->fever_charge = 0;
	ctx->fast_combo = 0;
	ctx->trail_fill = 0;
	memset(ctx->obs, 0, sizeof(ctx->obs));
	arcade_particles_clear(&ctx->particles);
}

static void prism_on_exit(arcade_shell_t *shell, void *arg) {
	(void)shell;
	prism_ctx_t *ctx = (prism_ctx_t *)arg;
	arcade_particles_clear(&ctx->particles);
}

static void prism_on_start(arcade_shell_t *shell, void *arg) {
	prism_ctx_t *ctx = (prism_ctx_t *)arg;
	ctx->shell = shell;
	int h = panel_h(ctx);
	float start_orb_y = (float)h * 0.72f;

	ctx->rng = arcade_rng_seed();
	ctx->dealt = ctx->bag_left = 0;
	ctx->time_s = 0.0f;
	ctx->cam_y = 0.0f;
	ctx->orb_y = start_orb_y;
	ctx->orb_vy = 0.0f;
	ctx->ball_color = 0;
	ctx->fast_combo = 0;
	ctx->fever_charge = 0;
	ctx->fever_t = 0.0f;
	ctx->post_fever_grace = 0.0f;
	ctx->since_star_s = 10.0f;
	ctx->launched = false;
	ctx->dead = false;
	ctx->shake_t = 0.0f;
	ctx->trail_head = 0;
	ctx->trail_fill = 0;
	ctx->has_prev_bounds = false;
	arcade_particles_clear(&ctx->particles);

	ctx->next_bottom_y = start_orb_y - 148.0f;
	for (int i = 0; i < PRISM_OBS_MAX; i++) {
		populate_obstacle(ctx, &ctx->obs[i], ctx->next_bottom_y, 0);
	}
	record_trail(ctx);
	sync_sub_hud(shell, ctx);
}

static void prism_on_touch(arcade_shell_t *shell, void *arg, bool pressed, int x, int y) {
	(void)shell;
	(void)x;
	(void)y;
	prism_ctx_t *ctx = (prism_ctx_t *)arg;
	if (!pressed || ctx->dead || ctx->fever_t > 0.0f) return;
	ctx->launched = true;
	ctx->orb_vy = JUMP_VY;
	arcade_sfx_play(ARCADE_SFX_BOUNCE);
	arcade_particles_spawn_burst(&ctx->particles, ORB_X, ctx->orb_y + ORB_R, 4, 40.0f, 110.0f, 220.0f, 2.2f, 0.28f,
								 prism_color_565(ctx->ball_color, theme_is_dark()), 0);
}

static void prism_on_step(arcade_shell_t *shell, void *arg, float dt) {
	prism_ctx_t *ctx = (prism_ctx_t *)arg;
	arcade_state_t st = arcade_shell_state(shell);
	int h = panel_h(ctx);
	ctx->time_s += dt;

	if (st == ARCADE_ST_TITLE) {
		ctx->orb_y = 216.0f + arcade_sin_ang(ctx->time_s * 3.1f) * 9.0f;
		arcade_particles_update(&ctx->particles, dt);
		return;
	}
	if (st != ARCADE_ST_PLAYING) return;

	if (ctx->shake_t > 0.0f) ctx->shake_t = fmaxf(0.0f, ctx->shake_t - dt);
	if (ctx->post_fever_grace > 0.0f) ctx->post_fever_grace = fmaxf(0.0f, ctx->post_fever_grace - dt);
	ctx->since_star_s += dt;
	if (ctx->since_star_s > 2.6f && ctx->fast_combo > 0) {
		ctx->fast_combo = 0;
		sync_sub_hud(shell, ctx);
	}

	arcade_particles_update(&ctx->particles, dt);

	for (int i = 0; i < PRISM_OBS_MAX; i++) {
		if (ctx->obs[i].active) {
			ctx->obs[i].rot += ctx->obs[i].rot_speed * dt;
		}
	}
	if (ctx->dead) return;

	if (!ctx->launched) {
		ctx->orb_y = (float)h * 0.72f + arcade_sin_ang(ctx->time_s * 3.6f) * 6.0f;
		record_trail(ctx);
		return;
	}

	bool dark = theme_is_dark();
	if (ctx->fever_t > 0.0f) {
		ctx->fever_t -= dt;
		ctx->orb_vy = -540.0f;
		ctx->orb_y += ctx->orb_vy * dt;
		arcade_particles_spawn_burst(&ctx->particles, ORB_X, ctx->orb_y + 10.0f, 2, 30.0f, 95.0f, 120.0f, 3.0f, 0.28f,
									 prism_color_565((int)(ctx->time_s * 16.0f) & 3, dark), 0);
		if (ctx->fever_t <= 0.0f) {
			ctx->fever_t = 0.0f;
			ctx->orb_vy = -210.0f;
			ctx->post_fever_grace = 0.55f;
			sync_sub_hud(shell, ctx);
		}
	} else {
		ctx->orb_vy = fminf(MAX_FALL_VY, ctx->orb_vy + GRAVITY * dt);
		ctx->orb_y += ctx->orb_vy * dt;
	}

	float cam_anchor = (float)h * 0.54f;
	if (ctx->orb_y - ctx->cam_y < cam_anchor) {
		ctx->cam_y = ctx->orb_y - cam_anchor;
	}
	record_trail(ctx);

	if (ctx->orb_y - ctx->cam_y > (float)h - 20.0f) {
		spawn_death_burst(ctx);
		arcade_shell_trigger_game_over(shell, 620);
		return;
	}

	int cur_score = arcade_shell_score(shell);
	for (int i = 0; i < PRISM_OBS_MAX; i++) {
		prism_obs_t *o = &ctx->obs[i];
		if (o->shifter_y - ctx->cam_y > (float)h + 24.0f) {
			populate_obstacle(ctx, o, ctx->next_bottom_y, cur_score);
			continue;
		}
		if (!o->active) continue;

		// Chromatic Fever rocket dash shatters obstacles cleanly on contact!
		if (ctx->fever_t > 0.0f && fabsf(ctx->orb_y - o->world_y) <= o->r_out + 18.0f) {
			if (o->has_star && !o->star_taken) {
				o->star_taken = true;
				cur_score++;
			}
			cur_score += 2;
			arcade_shell_set_score(shell, cur_score);
			shatter_obstacle(ctx, o, dark);
			continue;
		}

		// Collectible star + Fast-Pass Combo + 5-Star Chromatic Fever charge
		if (o->has_star && !o->star_taken && fabsf(ctx->orb_y - o->world_y) <= 26.0f) {
			o->star_taken = true;
			ctx->fast_combo = (ctx->since_star_s <= 2.4f) ? (ctx->fast_combo + 1) : 1;
			ctx->since_star_s = 0.0f;

			int gain = 1 + (ctx->fast_combo >= 3 ? 1 : 0);
			cur_score += gain;
			arcade_shell_set_score(shell, cur_score);
			arcade_particles_spawn_burst(&ctx->particles, ORB_X, o->world_y, 10, 60.0f, 180.0f, 160.0f, 3.0f, 0.45f,
										 dark ? 0xFFFF : prism_color_565(ctx->ball_color, false), 0);

			if (ctx->fever_t <= 0.0f) {
				ctx->fever_charge++;
				if (ctx->fever_charge >= FEVER_STARS) {
					ctx->fever_charge = 0;
					ctx->fever_t = 1.60f;
					arcade_sfx_play(ARCADE_SFX_POWERUP);
					arcade_shell_show_callout(shell, tr("arcade_perfect"), lv_color_hex(0xF7C552));
				} else if (ctx->fast_combo >= 3) {
					arcade_sfx_play_pitch(ARCADE_SFX_COMBO, LV_MIN(7, ctx->fast_combo));
					char msg[40];
					snprintf(msg, sizeof(msg), tr("arcade_combo"), ctx->fast_combo);
					arcade_shell_show_callout(shell, msg, prism_lv_color(ctx->ball_color));
				} else {
					arcade_sfx_play_pitch(ARCADE_SFX_POP, cur_score % 8);
				}
			}
			sync_sub_hud(shell, ctx);
		}

		// Color shifter orb between obstacles
		if (o->has_shifter && !o->shifter_taken && fabsf(ctx->orb_y - o->shifter_y) <= 22.0f) {
			o->shifter_taken = true;
			ctx->ball_color = (ctx->ball_color + 1 + (int)(next_rng(ctx) % 3u)) & 3;
			arcade_sfx_play_pitch(ARCADE_SFX_SLIDE, ctx->ball_color * 2);
			arcade_particles_spawn_ring(&ctx->particles, ORB_X, o->shifter_y, 10.0f, 190.0f, 0.36f,
										prism_color_565(ctx->ball_color, dark));
		}

		if (ctx->fever_t <= 0.0f && ctx->post_fever_grace <= 0.0f &&
			check_obs_collision(o, ORB_X, ctx->orb_y, ORB_R - 1.5f, ctx->ball_color) < 0) {
			spawn_death_burst(ctx);
			arcade_shell_trigger_game_over(shell, 650);
			return;
		}
	}
}

static void color_mark(arcade_fb_t *fb, int x, int y, int color) {
	for (int i = 0; i <= color; i++)
		arcade_fb_fill_circle(fb, x - 2 * color + 4 * i, y, 1, 0x1084);
}

static void draw_quadrant_ring(arcade_fb_t *fb, int cx, int cy, int r_in, int r_out, float rot, bool mirrored,
							   bool dark) {
	for (int q = 0; q < 4; q++) {
		float a0 = rot + (float)q * HALF_PI_F;
		float a1 = a0 + HALF_PI_F;
		int c_idx = mirrored ? ((1 - q) & 3) : q;
		arcade_fb_draw_arc(fb, cx, cy, r_in, r_out, a0, a1, prism_color_565(c_idx, dark), 255);
		if (r_out >= 30) {
			int radius = (r_in + r_out) / 2;
			float mid = (a0 + a1) * 0.5f;
			arcade_fb_draw_arc(fb, cx, cy, r_out - 2, r_out, a0, a1, 0xffff, 165);
			color_mark(fb, cx + radius * arcade_cos_ang(mid), cy + radius * arcade_sin_ang(mid), c_idx);
		}
	}
}

static void draw_bar_row(arcade_fb_t *fb, int sy, int shift, bool mirrored, bool dark) {
	for (int i = 0; i < 4; i++) {
		int left = shift + i * BAR_WIDTH;
		if (mirrored) left = SCREEN_W - left - BAR_WIDTH;
		left = (left + SCREEN_W) % SCREEN_W;
		uint16_t color = prism_color_565(i, dark);
		for (int x = left; x + BAR_WIDTH > 0; x -= SCREEN_W) {
			arcade_fb_fill_rrect(fb, x + 2, sy - BAR_HALF_H, BAR_WIDTH - 4, 2 * BAR_HALF_H, 6, color);
			int mid = x + BAR_WIDTH / 2;
			arcade_fb_hline(fb, mid - 14, mid + 14, sy - 6, 0xffff, 185);
			color_mark(fb, mid, sy, i);
		}
	}
}

static void draw_obstacle(arcade_fb_t *fb, const prism_obs_t *o, float cam_y, float time_s, bool dark, int *min_y,
						  int *max_y) {
	int sy = (int)lroundf(o->world_y - cam_y);
	if (sy + o->r_out < -2 || o->shifter_y - cam_y > fb->h + 20) return;
	int cx = (int)lroundf(o->cx);
	int bound_r = (int)o->r_out + 24;
	if (sy - bound_r < *min_y) *min_y = sy - bound_r;
	if (sy + bound_r > *max_y) *max_y = sy + bound_r;

	if (o->kind == PRISM_OBS_RING) {
		draw_quadrant_ring(fb, cx, sy, (int)o->r_in, (int)o->r_out, o->rot, false, dark);
	} else if (o->kind == PRISM_OBS_DOUBLE) {
		draw_quadrant_ring(fb, cx, sy, (int)(o->r_out - 18.0f), (int)o->r_out, -o->rot, true, dark);
		draw_quadrant_ring(fb, cx, sy, (int)o->r_in, (int)(o->r_in + 16.0f), o->rot, false, dark);
	} else if (o->kind == PRISM_OBS_BARS) {
		draw_bar_row(fb, sy, bar_shift(o->rot), false, dark);
	} else if (o->kind == PRISM_OBS_GATES) {
		int shift = bar_shift(o->rot);
		draw_bar_row(fb, sy + GATE_OFFSET, shift, false, dark);
		draw_bar_row(fb, sy - GATE_OFFSET, shift, true, dark);
	} else if (o->kind == PRISM_OBS_DIAMOND) {
		for (int q = 0; q < 4; q++) {
			float a0 = o->rot + (float)q * HALF_PI_F;
			float a1 = a0 + HALF_PI_F;
			int x0 = cx + (int)lroundf(arcade_cos_ang(a0) * o->r_out);
			int y0 = sy + (int)lroundf(arcade_sin_ang(a0) * o->r_out);
			int x1 = cx + (int)lroundf(arcade_cos_ang(a1) * o->r_out);
			int y1 = sy + (int)lroundf(arcade_sin_ang(a1) * o->r_out);
			arcade_fb_draw_line_thick(fb, x0, y0, x1, y1, 15, prism_color_565(q, dark), 255);
			color_mark(fb, (x0+x1)/2, (y0+y1)/2, q);
		}
	} else {
		for (int q = 0; q < 4; q++) {
			float a = o->rot + (float)q * HALF_PI_F;
			float ca = arcade_cos_ang(a), sa = arcade_sin_ang(a);
			float px = -sa * 9.0f, py = ca * 9.0f;
			float ax = (float)cx + ca * 12.0f, ay = (float)sy + sa * 12.0f;
			float bx = (float)cx + ca * o->r_out, by = (float)sy + sa * o->r_out;
			float qx[4] = {ax - px, ax + px, bx + px, bx - px};
			float qy[4] = {ay - py, ay + py, by + py, by - py};
			arcade_fb_fill_quad(fb, qx, qy, prism_color_565(q, dark), 255);
			color_mark(fb, (ax+bx)/2, (ay+by)/2, q);
		}
		arcade_fb_fill_circle(fb, cx, sy, 13, dark ? arcade_rgb565(30, 38, 54) : arcade_rgb565(220, 228, 240));
	}

	if (o->has_star && !o->star_taken) {
		float pulse = 10.0f + arcade_sin_ang(time_s * 5.0f) * 1.5f;
		float a = time_s * 2.2f;
		float qx[4], qy[4];
		for (int k = 0; k < 4; k++) {
			float ka = a + (float)k * HALF_PI_F;
			qx[k] = ORB_X + arcade_cos_ang(ka) * pulse;
			qy[k] = (float)sy + arcade_sin_ang(ka) * pulse;
		}
		arcade_fb_fill_quad(fb, qx, qy, dark ? 0xFFFF : arcade_rgb565(38, 48, 66), 255);
	}

	if (o->has_shifter && !o->shifter_taken) {
		int sh_y = (int)lroundf(o->shifter_y - cam_y);
		if (sh_y > -30 && sh_y < fb->h + 30) {
			if (sh_y - 20 < *min_y) *min_y = sh_y - 20;
			if (sh_y + 20 > *max_y) *max_y = sh_y + 20;
			draw_quadrant_ring(fb, (int)ORB_X, sh_y, 7, 15, time_s * 3.0f, false, dark);
			arcade_fb_fill_circle(fb, (int)ORB_X, sh_y, 5, dark ? 0xFFFF : arcade_rgb565(28, 36, 50));
		}
	}
}

static bool particles_bounds(const arcade_particles_t *p, float cam_y, int *min_y, int *max_y) {
	bool any = false;
	for (int i = 0; i < ARCADE_PARTICLES_MAX; i++) {
		if (!p->items[i].active) continue;
		any = true;
		int py = (int)lroundf(p->items[i].y - cam_y);
		int rad = (int)p->items[i].size + 8;
		if (py - rad < *min_y) *min_y = py - rad;
		if (py + rad > *max_y) *max_y = py + rad;
	}
	return any;
}

static void prism_on_render(arcade_shell_t *shell, void *arg, bool full_redraw) {
	prism_ctx_t *ctx = (prism_ctx_t *)arg;
	arcade_fb_t *fb = arcade_shell_fb(shell);
	if (!fb || !fb->pixels) return;

	bool dark = true;
	arcade_state_t st = arcade_shell_state(shell);
	float beat = arcade_shell_music_pulse(shell);

	int cam_px = (int)floorf(ctx->cam_y);
	bool need_full = full_redraw || !ctx->has_prev_bounds || cam_px != ctx->prev_cam_px || ctx->shake_t > 0.0f ||
					 ctx->fever_t > 0.0f || beat > 0.03f;

	if (st == ARCADE_ST_TITLE) {
		int min_y = 128, max_y = 324;
		particles_bounds(&ctx->particles, 0.0f, &min_y, &max_y);
		int wipe_y0 = need_full ? 0 : LV_MAX(0, LV_MIN(min_y, ctx->prev_min_y));
		int wipe_y1 = need_full ? (fb->h - 1) : LV_MIN(fb->h - 1, LV_MAX(max_y, ctx->prev_max_y));
		scene_backdrop(fb, scene_crystal, wipe_y0, wipe_y1);

		draw_quadrant_ring(fb, (int)ORB_X, 216, 66, 86, ctx->time_s * 1.15f, false, dark);
		int cy = (int)lroundf(ctx->orb_y);
		uint16_t oc = prism_color_565(0, dark);
		arcade_fb_fill_circle_alpha(fb, (int)ORB_X, cy, (int)ORB_R + 6, oc, 70);
		arcade_fb_fill_circle(fb, (int)ORB_X, cy, (int)ORB_R, oc);
		arcade_fb_fill_circle(fb, (int)ORB_X - 3, cy - 3, 3, 0xFFFF);
		arcade_particles_draw(&ctx->particles, fb, 0.0f);

		if (need_full) arcade_fb_mark_full(fb);
		else arcade_fb_mark_dirty(fb, 0, wipe_y0, fb->w - 1, wipe_y1);
		ctx->prev_cam_px = cam_px;
		ctx->prev_min_y = min_y;
		ctx->prev_max_y = max_y;
		ctx->has_prev_bounds = true;
		return;
	}

	// Compute active vertical band for dirty-rect invalidation when camera is stationary
	int cur_min_y = fb->h, cur_max_y = 0;
	for (int i = 0; i < PRISM_OBS_MAX; i++) {
		const prism_obs_t *o = &ctx->obs[i];
		if (!o->active && !(o->has_shifter && !o->shifter_taken)) continue;
		int sy = (int)lroundf(o->world_y - ctx->cam_y);
		int br = (int)o->r_out + 26;
		if (sy + br >= 0 && sy - br < fb->h) {
			if (sy - br < cur_min_y) cur_min_y = sy - br;
			if (sy + br > cur_max_y) cur_max_y = sy + br;
		}
		if (o->has_shifter && !o->shifter_taken) {
			int shy = (int)lroundf(o->shifter_y - ctx->cam_y);
			if (shy + 22 >= 0 && shy - 22 < fb->h) {
				if (shy - 22 < cur_min_y) cur_min_y = shy - 22;
				if (shy + 22 > cur_max_y) cur_max_y = shy + 22;
			}
		}
	}
	if (!ctx->dead) {
		int oy = (int)lroundf(ctx->orb_y - ctx->cam_y);
		if (oy - 28 < cur_min_y) cur_min_y = oy - 28;
		if (oy + 28 > cur_max_y) cur_max_y = oy + 28;
		for (int t = 1; t < ctx->trail_fill; t++) {
			int idx = (ctx->trail_head - t + ORB_TRAIL) % ORB_TRAIL;
			int ty = (int)lroundf(ctx->trail_y[idx] - ctx->cam_y);
			if (ty - 14 < cur_min_y) cur_min_y = ty - 14;
			if (ty + 14 > cur_max_y) cur_max_y = ty + 14;
		}
	}
	particles_bounds(&ctx->particles, ctx->cam_y, &cur_min_y, &cur_max_y);
	if (cur_min_y > cur_max_y) {
		cur_min_y = 0;
		cur_max_y = fb->h - 1;
	}

	int wipe_y0 = need_full ? 0 : LV_MAX(0, LV_MIN(cur_min_y, ctx->prev_min_y));
	int wipe_y1 = need_full ? (fb->h - 1) : LV_MIN(fb->h - 1, LV_MAX(cur_max_y, ctx->prev_max_y));
	scene_backdrop(fb, scene_crystal, wipe_y0, wipe_y1);

	int shake_x = 0;
	if (ctx->shake_t > 0.0f) {
		shake_x = (int)lroundf(arcade_sin_ang(ctx->shake_t * 62.0f) * 6.0f * (ctx->shake_t / 0.34f));
	}

	int dummy_min = fb->h, dummy_max = 0;
	for (int i = 0; i < PRISM_OBS_MAX; i++) {
		if (ctx->obs[i].active) {
			draw_obstacle(fb, &ctx->obs[i], ctx->cam_y, ctx->time_s, dark, &dummy_min, &dummy_max);
		}
	}

	if (!ctx->dead) {
		bool fever_vis = (ctx->fever_t > 0.0f);
		uint16_t oc = fever_vis ? prism_color_565((int)(ctx->time_s * 18.0f) & 3, dark)
								: prism_color_565(ctx->ball_color, dark);
		for (int t = 1; t < ctx->trail_fill; t++) {
			int idx = (ctx->trail_head - t + ORB_TRAIL) % ORB_TRAIL;
			int ty = (int)lroundf(ctx->trail_y[idx] - ctx->cam_y);
			uint8_t a = (uint8_t)(115 - t * 10);
			int tr = LV_MAX(2, 9 - (t * 2) / 3);
			uint16_t tc = fever_vis ? prism_color_565(((int)(ctx->time_s * 18.0f) + t) & 3, dark) : oc;
			arcade_fb_fill_circle_alpha(fb, (int)ORB_X + shake_x, ty, tr, tc, a);
		}
		int oy = (int)lroundf(ctx->orb_y - ctx->cam_y);
		if (fever_vis || ctx->post_fever_grace > 0.0f) {
			arcade_fb_draw_ring(fb, (int)ORB_X + shake_x, oy, (int)ORB_R + 5, (int)ORB_R + 8, 0xFFE6, 200);
		}
		arcade_fb_fill_circle_alpha(fb, (int)ORB_X + shake_x, oy, (int)ORB_R + 6, oc, 85);
		arcade_fb_fill_circle(fb, (int)ORB_X + shake_x, oy, (int)ORB_R, oc);
		color_mark(fb, (int)ORB_X + shake_x, oy, ctx->ball_color);
	}

	arcade_particles_draw(&ctx->particles, fb, ctx->cam_y);

	if (need_full) {
		arcade_fb_mark_full(fb);
	} else {
		arcade_fb_mark_dirty(fb, 0, wipe_y0, fb->w - 1, wipe_y1);
	}
	ctx->prev_cam_px = cam_px;
	ctx->prev_min_y = LV_CLAMP(0, cur_min_y, fb->h - 1);
	ctx->prev_max_y = LV_CLAMP(0, cur_max_y, fb->h - 1);
	ctx->has_prev_bounds = true;
}

static void prism_on_theme(arcade_shell_t *shell, void *arg) {
	(void)shell;
	prism_ctx_t *ctx = (prism_ctx_t *)arg;
	
	ctx->has_prev_bounds = false;
}

static const arcade_game_ops_t PRISM_OPS = {
	.id = "prism",
	.title_tag = "game_prism",
	.subtitle_tag = "game_prism_desc",
	.hint_tag = "game_prism_hint",
	.icon = &icon_atelier_prism,
	.dark_backdrop = true,
	.target_fps = 30,
	.use_framebuffer = true,
	.music_reactive = true,
	.on_enter = prism_on_enter,
	.on_exit = prism_on_exit,
	.on_start = prism_on_start,
	.on_step = prism_on_step,
	.on_render = prism_on_render,
	.on_touch = prism_on_touch,
	.on_drag = NULL,
	.on_theme_changed = prism_on_theme,
};

void game_prism_open(gui_config_t *cfg) {
	if (!g_prism.shell) {
		g_prism.shell = arcade_shell_create(cfg, &PRISM_OPS, &g_prism);
	}
	arcade_shell_open(g_prism.shell);
}

int game_prism_best(void) { return arcade_shell_read_best("prism"); }
