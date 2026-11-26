#include "arcade_scene.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTORS 18
#define FLOORS 6
#define BREAK_SECONDS 0.50f
// At terminal speed, at most three floors can break in half a second.
#define BREAK_SLOTS 3
#define STAGE_FLOORS 25
#define SPACING 62
#define TAU 6.283185307f
#define BALL_X 240
#define BALL_RADIUS 19
#define BALL_DIAMETER (2 * BALL_RADIUS + 1)
#define CONTACT_Y 320
#define RING_RADIUS 182
#define RING_RISE 51
#define RING_DEPTH 26
#define CORE_RADIUS 55
#define CORE_RISE (RING_RISE * (float)CORE_RADIUS / RING_RADIUS)

enum { SUBDIV = 4, FACETS = SECTORS * SUBDIV };

typedef enum { FLOOR_SAFE, FLOOR_GAP, FLOOR_DANGER, FLOOR_GOAL } sector_t;

typedef struct {
	int gap, gap_width, base_danger, danger_width;
	uint16_t color;
	float osc_phase;
	bool moving_danger;
	bool is_goal;
} floor_t;

typedef struct {
	floor_t floors[FLOORS]; // current landing surface followed by the visible descent
	struct {
		floor_t floor;
		float cs[FACETS + 1], sn[FACETS + 1];
		float started;
		int depth;
		bool active;
	} breaks[BREAK_SLOTS];
	float angle, ball_y, velocity, camera, time_s;
	int route_gap, route_stride, route_left;
	int depth, stage, stage_pos, freefall, last_touch;
	bool dragging, broken;
	float painted_angle, painted_camera;
	int painted_depth, painted_ball;
	uint16_t behind_ball[BALL_DIAMETER][BALL_DIAMETER];
	bool saved_ball, painted_broken, painted_fireball;
	arcade_particles_t sparks;
	uint32_t rng;
} tower_t;

extern const uint16_t stone_column[], stone_limestone[];
static arcade_shell_t *shell;
static tower_t tower;
static const uint16_t tones[][5] = {
	{0xf6d5, 0xef16, 0xe6b3, 0xf737, 0xeed6},
	{0xcf19, 0xd739, 0xe777, 0xce97, 0xdf39},
	{0xf612, 0xe5f2, 0xeeb7, 0xf653, 0xe611},
};

static bool particles_alive(const arcade_particles_t *p) {
	for (int i = 0; i < ARCADE_PARTICLES_MAX; i++) {
		if (p->items[i].active) return true;
	}
	return false;
}

static bool any_moving_floor(const tower_t *g) {
	for (int i = 0; i < FLOORS; i++) {
		if (g->floors[i].moving_danger) return true;
	}
	return false;
}

static int effective_danger_start(const floor_t *f, float time_s) {
	if (!f->moving_danger || f->danger_width <= 0) return f->base_danger;
	int shift = (int)lroundf(arcade_sin_ang(time_s * 2.1f + f->osc_phase) * 1.35f);
	return (f->base_danger + shift + SECTORS) % SECTORS;
}

static sector_t material(const floor_t *f, int sector, float time_s) {
	if (f->is_goal) return FLOOR_GOAL;
	if ((sector - f->gap + SECTORS) % SECTORS < f->gap_width) return FLOOR_GAP;
	int d0 = effective_danger_start(f, time_s);
	if ((sector - d0 + SECTORS) % SECTORS < f->danger_width) return FLOOR_DANGER;
	return FLOOR_SAFE;
}

static floor_t build_floor(tower_t *g, int stage, int floor_in_stage) {
	if (floor_in_stage >= STAGE_FLOORS) {
		return (floor_t){
			.gap = 0,
			.gap_width = 0,
			.base_danger = 0,
			.danger_width = 0,
			.color = 0xFED0,
			.is_goal = true,
		};
	}

	const uint16_t *pal = tones[(stage - 1) % LV_ARRAYLEN(tones)];
	int gw = (floor_in_stage < 6 && stage == 1) ? 5 : (stage >= 4 ? 3 : 4);
	int dw = (floor_in_stage <= 0) ? 0 : LV_MIN(5, 1 + stage + floor_in_stage / 10);
	if (floor_in_stage == 0) {
		g->route_gap = 8;
		g->route_left = 0;
	} else {
		if (g->route_left == 0) {
			static const int strides[] = {0, 1, -1, 3, -3};
			g->route_stride = strides[arcade_rng_next(&g->rng) % LV_ARRAYLEN(strides)];
			g->route_left = 2 + arcade_rng_next(&g->rng) % 3;
		}
		g->route_gap = (g->route_gap + SECTORS + g->route_stride) % SECTORS;
		g->route_left--;
	}
	int gap = g->route_gap;
	int span = SECTORS - gw - dw - 1;
	int offset = span > 1 ? (1 + (int)(arcade_rng_next(&g->rng) % (uint32_t)span)) : 1;
	bool moving = ((stage >= 2 || floor_in_stage >= 12) && dw > 0 && floor_in_stage > 2 && (arcade_rng_next(&g->rng) % 100u) < 42u);
	// Leave buffer on both sides of the gap so oscillating danger never seals the gap!
	if (moving && span <= 3) moving = false;
	if (moving && offset < 2) offset = 2;
	if (moving && offset > SECTORS - gw - dw - 2) offset = SECTORS - gw - dw - 2;

	return (floor_t){
		.gap = gap,
		.gap_width = gw,
		.base_danger = (gap + gw + offset) % SECTORS,
		.danger_width = dw,
		.color = pal[floor_in_stage % 5],
		.osc_phase = (float)(arcade_rng_next(&g->rng) % 628u) * 0.01f,
		.moving_danger = moving,
		.is_goal = false,
	};
}

static float wrap(float a) { return a - TAU * floorf(a / TAU); }

static sector_t landing_material(const tower_t *g) {
	const floor_t *f = &g->floors[0];
	float front = TAU / 4.0f - g->angle;
	int sector = (int)(wrap(front) * SECTORS / TAU) % SECTORS;
	sector_t center = material(f, sector, g->time_s);
	if (center != FLOOR_GAP) return center;

	// At the front rim the orb spans asin(radius / ring radius) on either side.
	const float half_width = asinf((float)BALL_RADIUS / RING_RADIUS);
	for (int side = -1; side <= 1; side += 2) {
		sector = (int)(wrap(front + side * half_width) * SECTORS / TAU) % SECTORS;
		sector_t edge = material(f, sector, g->time_s);
		if (edge != FLOOR_GAP) return edge;
	}
	return FLOOR_GAP;
}

static void sync_hud(arcade_shell_t *s, const tower_t *g) {
	if (!s || arcade_shell_state(s) == ARCADE_ST_TITLE) return;
	char lvl[36], sub[72];
	snprintf(lvl, sizeof(lvl), tr("arcade_level"), g->stage);
	snprintf(sub, sizeof(sub), "%s  ·  %d/%d", lvl, LV_MIN(STAGE_FLOORS, g->stage_pos), STAGE_FLOORS);
	arcade_shell_set_sub_hud(s, sub);
}

static void populate_stage_floors(tower_t *g) {
	for (int i = 0; i < FLOORS; i++) {
		g->floors[i] = build_floor(g, g->stage, i);
	}
}

static void reset(arcade_shell_t *s, void *ctx) {
	tower_t *g = ctx;
	*g = (tower_t){
		.ball_y = -27.0f,
		.stage = 1,
		.stage_pos = 0,
		.painted_depth = -1,
		.rng = arcade_rng_seed(),
	};
	arcade_particles_clear(&g->sparks);
	populate_stage_floors(g);
	sync_hud(s, g);
}

static void ring_angles(float angle, float cs[FACETS + 1], float sn[FACETS + 1]) {
	for (int i = 0; i < FACETS; i++) {
		float a = angle + TAU * (float)i / FACETS;
		cs[i] = arcade_cos_ang(a);
		sn[i] = arcade_sin_ang(a);
	}
	cs[FACETS] = cs[0];
	sn[FACETS] = sn[0];
}

// Rebase the ball and camera together so neither moves on screen at the handoff.
static void pass_floor(tower_t *g) {
	int slot = g->depth % BREAK_SLOTS;
	g->breaks[slot].floor = g->floors[0];
	ring_angles(g->angle, g->breaks[slot].cs, g->breaks[slot].sn);
	g->breaks[slot].started = g->time_s;
	g->breaks[slot].depth = g->depth;
	g->breaks[slot].active = true;
	g->depth++;
	g->ball_y -= SPACING;
	g->camera -= SPACING;

	if (g->floors[0].is_goal) {
		g->stage++;
		g->stage_pos = 0;
		populate_stage_floors(g);
	} else {
		g->stage_pos++;
		memmove(g->floors, g->floors + 1, (FLOORS - 1) * sizeof(floor_t));
		g->floors[FLOORS - 1] = build_floor(g, g->stage, g->stage_pos + FLOORS - 1);
	}
}

static float camera_world_y(const tower_t *g) {
	return 2 * (g->depth * SPACING + g->camera);
}

static void step(arcade_shell_t *s, void *ctx, float dt) {
	tower_t *g = ctx;
	g->time_s += dt;
	for (int i = 0; i < BREAK_SLOTS; i++) {
		if (g->time_s - g->breaks[i].started >= BREAK_SECONDS) g->breaks[i].active = false;
	}
	arcade_particles_update(&g->sparks, dt);
	if (g->broken) return;

	bool demo = arcade_shell_state(s) == ARCADE_ST_TITLE;
	if (demo) g->angle = wrap(g->angle + dt * 0.96f);
	g->velocity = fminf(325.0f, g->velocity + 345.0f * dt);
	g->ball_y += g->velocity * dt;
	// Follow the descent, never pan to a floor the ball has yet to approach.
	float target = fminf(0.0f, g->ball_y);
	if (target > g->camera) g->camera += (target - g->camera) * fminf(1.0f, dt * 8.0f);

	// Fireball trail embers while plummeting through 3+ consecutive gaps
	if (g->freefall >= 3 && g->velocity > 80.0f) {
		float by = (float)CONTACT_Y - BALL_RADIUS + 2 * (g->ball_y + g->depth * SPACING);
		arcade_particles_spawn_burst(&g->sparks, (float)BALL_X, by - 4.0f, 1, 10.0f, 36.0f, -60.0f, 2.6f, 0.24f,
									 0xFD20, 0);
	}

	if (g->ball_y < 0.0f || g->velocity < 0.0f) return;

	sector_t mat = landing_material(g);

	if (mat == FLOOR_GOAL) {
		pass_floor(g);
		g->freefall = 0;
		g->velocity = 95.0f;
		if (!demo) {
			arcade_sfx_play(ARCADE_SFX_WIN);
			arcade_shell_set_score(s, arcade_shell_score(s) + 10 * g->stage);
			arcade_shell_show_callout(s, tr("arcade_level_clear"), lv_color_hex(0xF7C552));
			sync_hud(s, g);
		}
		return;
	}

	if (mat == FLOOR_GAP) {
		g->freefall++;
		if (!demo) {
			int pts = (g->freefall >= 3) ? 2 : 1;
			arcade_shell_set_score(s, arcade_shell_score(s) + pts);
			arcade_sfx_play_pitch(g->freefall >= 3 ? ARCADE_SFX_COMBO : ARCADE_SFX_POP, LV_MIN(7, g->freefall));
		}
		pass_floor(g);
		sync_hud(s, g);
		return;
	}

	// Fireball Smash: falling through 3+ gaps shatters ANY ring (including Danger!) on impact!
	if (g->freefall >= 3) {
		int mul = LV_MIN(8, g->freefall);
		g->freefall = 0;
		pass_floor(g);
		g->velocity = 95.0f;
		if (!demo) {
			arcade_sfx_play(ARCADE_SFX_SMASH);
			arcade_shell_set_score(s, arcade_shell_score(s) + mul);
			char msg[48];
			snprintf(msg, sizeof(msg), tr("arcade_combo"), mul);
			arcade_shell_show_callout(s, msg, lv_color_hex(0xFF9E44));
			sync_hud(s, g);
		}
		return;
	}

	g->freefall = 0;
	if (mat == FLOOR_DANGER && !demo) {
		g->broken = true;
		g->ball_y = 0.0f;
		arcade_particles_spawn_burst(&g->sparks, (float)BALL_X, CONTACT_Y + 2.0f * g->depth * SPACING, 14, 25.0f, 95.0f, 140.0f, 2.5f, 0.42f,
									 0xFB2F, 0);
		arcade_shell_trigger_game_over(s, 260);
		return;
	}

	// FLOOR_SAFE (or demo danger)
	g->ball_y = 0.0f;
	g->velocity = -111.0f;
	if (!demo) arcade_sfx_play(ARCADE_SFX_BOUNCE);
}

static void touch(arcade_shell_t *s, void *ctx, bool down, int x, int y) {
	(void)s;
	(void)y;
	tower_t *g = ctx;
	g->dragging = down;
	g->last_touch = x;
}

static void drag(arcade_shell_t *s, void *ctx, int x, int y) {
	(void)y;
	tower_t *g = ctx;
	if (g->dragging) g->angle = wrap(g->angle + (float)(x - g->last_touch) * TAU / (float)arcade_shell_width(s));
	g->last_touch = x;
}

static void draw_ring(arcade_fb_t *fb, const floor_t *f, const float *cs, const float *sn, float y,
					  bool front, float material_time, float progress) {
	if (y + RING_RISE + RING_DEPTH + 120 * progress < 0 || y - RING_RISE - 40 * progress >= fb->h) return;
	if (progress == 0.0f && !f->is_goal && f->gap_width > 0) {
		// Close the two cut ends of the ring wherever the opening faces the player.
		for (int end = 0; end < 2; end++) {
			int edge = end ? (f->gap + f->gap_width) % SECTORS : f->gap;
			int solid = end ? edge : (edge + SECTORS - 1) % SECTORS;
			edge *= SUBDIV;
			if ((sn[solid * SUBDIV] + sn[(solid + 1) * SUBDIV] > 0) != front || (end ? cs[edge] >= 0 : cs[edge] <= 0)) continue;
			float outer = BALL_X + RING_RADIUS * cs[edge], inner = BALL_X + CORE_RADIUS * cs[edge];
			float yo = y + RING_RISE * sn[edge], yi = y + CORE_RISE * sn[edge];
			float cap_x[] = {outer, inner, inner, outer};
			float cap_y[] = {yo, yi, yi + RING_DEPTH, yo + RING_DEPTH};
			uint16_t color = material(f, solid, material_time) == FLOOR_DANGER ? 0xece3 : f->color;
			arcade_fb_fill_quad(fb, cap_x, cap_y, arcade_blend565(color, 0, 142), 255);
		}
	}

	for (int k = 0; k < FACETS; k++) {
		sector_t m = material(f, k / SUBDIV, material_time);
		bool near = sn[k] + sn[k + 1] > 0;
		if (m == FLOOR_GAP || near != front) continue;
		uint16_t c = (m == FLOOR_DANGER) ? 0xece3 : (m == FLOOR_GOAL ? ((k & 1) ? 0xFFE8 : 0xFD64) : f->color);
		float x[] = {BALL_X + RING_RADIUS * cs[k], BALL_X + RING_RADIUS * cs[k + 1],
					 BALL_X + CORE_RADIUS * cs[k + 1], BALL_X + CORE_RADIUS * cs[k]};
		float top[] = {y + RING_RISE * sn[k], y + RING_RISE * sn[k + 1], y + CORE_RISE * sn[k + 1],
					   y + CORE_RISE * sn[k]};
		// Each wedge separates along its own radius, tumbles, and falls away.
		float thickness = RING_DEPTH;
		int texture_y = (int)y - 53;
		if (progress > 0.0f) {
			int sector = k / SUBDIV;
			int mid = sector * SUBDIV + SUBDIV / 2;
			float scale = 1.0f - progress * progress;
			float spread = (64 + 8 * (sector % 3)) * progress;
			float pivot_x = BALL_X + 0.5f * (RING_RADIUS + CORE_RADIUS) * cs[mid];
			float pivot_y = y + 0.5f * (RING_RISE + CORE_RISE) * sn[mid];
			float drop = 115 * progress * progress + (18 * sn[mid] - 20) * progress;
			float tilt = (sector % 2 ? 0.65f : -0.65f) * progress;
			float cos_tilt = scale * arcade_cos_ang(tilt), sin_tilt = scale * arcade_sin_ang(tilt);
			for (int v = 0; v < 4; v++) {
				float dx = x[v] - pivot_x, dy = top[v] - pivot_y;
				x[v] = pivot_x + dx * cos_tilt - dy * sin_tilt + spread * cs[mid];
				top[v] = pivot_y + dx * sin_tilt + dy * cos_tilt + drop;
			}
			texture_y += (int)drop;
			thickness *= scale;
		}
		if (near) {
			float side_x[] = {x[0], x[1], x[1], x[0]};
			float side_y[] = {top[0], top[1], top[1] + thickness, top[0] + thickness};
			if (m == FLOOR_SAFE)
				arcade_fb_textured_quad(fb, side_x, side_y, stone_limestone, 480, texture_y, 133);
			else arcade_fb_fill_quad(fb, side_x, side_y, arcade_blend565(c, 0, (uint8_t)(96 + 42 * cs[k])), 255);
		}
		if (m == FLOOR_SAFE) {
			arcade_fb_textured_quad(fb, x, top, stone_limestone, 480, texture_y, 133);
			if (k % (2*SUBDIV) == 0) {
				arcade_fb_draw_line_thick(fb, x[0], top[0], x[3], top[3], 1, 0x6b6b, 68);
				if (near) arcade_fb_vline(fb, x[0], top[0], top[0]+thickness, 0x5aeb, 70);
			}
		} else {
			arcade_fb_fill_quad(fb, x, top, arcade_blend565(c, 0xffff, (uint8_t)(32 - 18 * cs[k])), 255);
		}
		arcade_fb_draw_line_thick(fb, (int)x[0], (int)top[0], (int)x[1], (int)top[1], 1, 0xffff, 120);
		if (m == FLOOR_DANGER && (k % 2 == 0)) {
			arcade_fb_draw_line_thick(fb, x[3] + 0.14f * (x[0] - x[3]), top[3] + 0.14f * (top[0] - top[3]),
									  x[2] + 0.86f * (x[1] - x[2]), top[2] + 0.86f * (top[1] - top[2]),
									  2, 0xff72, 240);
		}
	}
}

static void draw_stage(arcade_fb_t *fb, const tower_t *g) {
	float cs[FACETS + 1], sn[FACETS + 1];
	ring_angles(g->angle, cs, sn);
	scene_backdrop(fb, scene_cliffs, 0, fb->h - 1);

	// Back halves sit behind the spindle. Front halves overlap its lit face.
	for (int front = 0; front <= 1; front++) {
		if (front) {
			int scroll = (int)lroundf(2 * g->camera) + g->depth * 2 * SPACING;
			for (int y = 0; y < fb->h; y++) {
				int row = ((y + scroll) % 160 + 160) % 160;
				memcpy(fb->pixels + y*fb->w + BALL_X - CORE_RADIUS,
					   stone_column + row * (2*CORE_RADIUS + 1), (2*CORE_RADIUS + 1) * sizeof(uint16_t));
			}
		}
		for (int i = 0; i < BREAK_SLOTS; i++) {
			if (!g->breaks[i].active) continue;
			float y = CONTACT_Y - RING_RISE + 2 * ((g->breaks[i].depth - g->depth) * SPACING - g->camera);
			float progress = (g->time_s - g->breaks[i].started) / BREAK_SECONDS;
			draw_ring(fb, &g->breaks[i].floor, g->breaks[i].cs, g->breaks[i].sn, y, front, g->breaks[i].started, progress);
		}
		for (int i = 0; i < FLOORS; i++) {
			float y = CONTACT_Y - RING_RISE + 2 * (i * SPACING - g->camera);
			draw_ring(fb, &g->floors[i], cs, sn, y, front, g->time_s, 0);
		}
	}

	// The landing shadow belongs to the solid surface at the contact point.
	if (landing_material(g) != FLOOR_GAP) {
		for (int dy = -4; dy <= 4; dy++) {
			int half = 16 - dy * dy / 2;
			arcade_fb_hline(fb, BALL_X - half, BALL_X + half, CONTACT_Y - 2 * g->camera + dy - 4, 0x39c7, 80);
		}
	}
}

static void render(arcade_shell_t *s, void *ctx, bool full) {
	tower_t *g = ctx;
	arcade_fb_t *fb = arcade_shell_fb(s);
	int y = (int)lroundf((float)(CONTACT_Y - BALL_RADIUS) + 2 * (g->ball_y - g->camera));
	bool fireball = g->freefall >= 3;
	bool dynamic_stage = particles_alive(&g->sparks) || any_moving_floor(g) || fireball;

	for (int i = 0; i < BREAK_SLOTS; i++) dynamic_stage |= g->breaks[i].active;
	if (full || !g->saved_ball || dynamic_stage || g->painted_angle != g->angle || g->painted_depth != g->depth ||
		lroundf(2 * g->painted_camera) != lroundf(2 * g->camera)) {
		arcade_fb_mark_full(fb);
		draw_stage(fb, g);
	} else if (g->saved_ball) {
		if (g->painted_ball == y && g->painted_broken == g->broken && g->painted_fireball == fireball) return;
		for (int row = 0; row < BALL_DIAMETER; row++)
			memcpy(fb->pixels + (g->painted_ball - BALL_RADIUS + row) * fb->w + BALL_X - BALL_RADIUS,
				   g->behind_ball[row], sizeof(g->behind_ball[row]));
		arcade_fb_mark_dirty(fb, BALL_X - BALL_RADIUS, g->painted_ball - BALL_RADIUS, BALL_X + BALL_RADIUS,
							 g->painted_ball + BALL_RADIUS);
	}

	g->saved_ball = !dynamic_stage && (y >= BALL_RADIUS && y < fb->h - BALL_RADIUS);
	if (y >= BALL_RADIUS && y < fb->h - BALL_RADIUS) {
		if (g->saved_ball) {
			for (int row = 0; row < BALL_DIAMETER; row++)
				memcpy(g->behind_ball[row], fb->pixels + (y - BALL_RADIUS + row) * fb->w + BALL_X - BALL_RADIUS,
					   sizeof(g->behind_ball[row]));
		}
		scene_sphere(fb, BALL_X, y, BALL_RADIUS, fireball ? 0xfb40 : (g->broken ? 0xe9e8 : 0x235e));
	}

	arcade_particles_draw(&g->sparks, fb, camera_world_y(g));
	g->painted_broken = g->broken;
	g->painted_fireball = fireball;
	g->painted_angle = g->angle;
	g->painted_camera = g->camera;
	g->painted_depth = g->depth;
	g->painted_ball = y;
}

void game_towerblast_open(gui_config_t *cfg) {
	static const arcade_game_ops_t ops = {
		.id = "towerblast",
		.title_tag = "game_towerblast",
		.subtitle_tag = "game_towerblast_desc",
		.hint_tag = "game_towerblast_hint",
		.icon = &icon_atelier_towerblast,
		.use_framebuffer = true,
		.compact_scene = false,
		.dark_backdrop = true,
		.on_enter = reset,
		.on_start = reset,
		.on_step = step,
		.on_render = render,
		.on_touch = touch,
		.on_drag = drag,
	};
	if (!shell) shell = arcade_shell_create(cfg, &ops, &tower);
	arcade_shell_open(shell);
}
