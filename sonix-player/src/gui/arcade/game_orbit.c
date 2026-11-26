#include "game_orbit.h"
#include "arcade_scene.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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

#define ORBIT_CX 240
#define ORBIT_MAX_H 804
#define ORBIT_RING_COUNT 3
#define ORBIT_TRAIL_LEN 16
#define ORBIT_OBS_MAX 14
#define ORBIT_STARS_MAX 8
#define TRANSFER_DUR 0.130f
#define ARENA_HALF_BOX 224

static const int RING_RADII[ORBIT_RING_COUNT] = {74, 132, 190};

typedef struct {
	int ring;
	float angle;
	float half_arc;
	float speed;
	float telegraph;
	uint8_t kind; // 0 = coral barrier arc, 1 = amber sentinel orb
	bool grazed;
	bool active;
} orbit_obstacle_t;

typedef struct {
	int ring;
	float angle;
	float pulse;
	float life;
	float max_life;
	uint8_t kind; // 0 = Lumen star (+1), 1 = Nova shockwave bonus (+5)
	bool active;
} orbit_star_t;

typedef struct {
	arcade_shell_t *shell;

	int encounter, safe_lane;
	float encounter_time;

	// Player orb state
	int ring;
	int next_dir;
	int target_ring;
	bool transferring;
	float transfer_t;
	float from_r;
	float to_r;
	float cur_r;
	float orb_angle;
	int buffered_step; // -1 = inward, +1 = outward, 0 = none
	float buffer_timer;
	float grace_t;
	float orb_trail_x[ORBIT_TRAIL_LEN];
	float orb_trail_y[ORBIT_TRAIL_LEN];
	int trail_head;
	int trail_fill;

	// Wave 3+ Pulsar beam sweep
	bool pulsar_active;
	int pulsar_safe_ring;
	float pulsar_angle;
	float pulsar_telegraph;
	float pulsar_fire_t;
	float pulsar_cd;

	// World & progression state
	int wave;
	int stars_collected;
	int combo;
	float combo_t;
	float time_s;
	float spawn_timer;
	float core_pulse_r;
	float core_pulse_a;
	float shake_t;

	orbit_obstacle_t obstacles[ORBIT_OBS_MAX];
	orbit_star_t stars[ORBIT_STARS_MAX];
	arcade_particles_t particles;
	uint32_t rng;
} orbit_ctx_t;

static orbit_ctx_t g_orbit;

static inline int orbit_cy_for_h(int h) { return (h < 760) ? 404 : 435; }

static float rand_unit(orbit_ctx_t *o) {
	uint32_t x = arcade_rng_next(&o->rng);
	return (float)(x & 0xFFFFu) / 65535.0f;
}

float orbit_wrap_angle(float a) {
	float w = fmodf(a, TWO_PI_F);
	if (w < 0.0f) {
		w += TWO_PI_F;
	}
	return w;
}

float orbit_angle_diff(float a, float b) {
	float d = orbit_wrap_angle(a - b);
	if (d > (float)M_PI) {
		d -= TWO_PI_F;
	}
	return d;
}

int orbit_next_ring(int cur_ring, int *dir_inout) {
	int dir = (dir_inout && *dir_inout < 0) ? -1 : 1;
	int nxt = cur_ring + dir;
	if (nxt >= ORBIT_RING_COUNT) {
		dir = -1;
		nxt = cur_ring - 1;
	} else if (nxt < 0) {
		dir = 1;
		nxt = cur_ring + 1;
	}
	if (dir_inout) {
		*dir_inout = dir;
	}
	return nxt;
}

bool orbit_check_arc_hit(int orb_ring, float orb_angle, int obs_ring, float obs_center_ang, float obs_half_arc) {
	if (orb_ring != obs_ring) {
		return false;
	}
	float margin = obs_half_arc - 0.04f;
	if (margin < 0.04f) {
		margin = 0.04f;
	}
	return fabsf(orbit_angle_diff(orb_angle, obs_center_ang)) <= margin;
}

static void draw_arena_backdrop(arcade_fb_t *fb, int cx, int cy, int y0, int y1, bool dark) {
	uint16_t ring_c = dark ? arcade_rgb565(156, 183, 214) : arcade_rgb565(180, 192, 208);
	uint16_t axis_c = dark ? arcade_rgb565(88, 105, 145) : arcade_rgb565(198, 208, 222);
	uint16_t halo_c = dark ? arcade_rgb565(28, 46, 70) : arcade_rgb565(178, 202, 228);

	arcade_fb_fill_circle_alpha(fb, cx, cy, 162, halo_c, dark ? 40 : 32);
	arcade_fb_fill_circle_alpha(fb, cx, cy, 92, halo_c, dark ? 50 : 38);

	// Deterministic starfield clipped to [y0..y1]
	uint32_t seed = 0x13572468u;
	uint16_t star_c = dark ? arcade_rgb565(140, 165, 195) : arcade_rgb565(135, 150, 170);
	for (int i = 0; i < 42; i++) {
		seed = seed * 1664525u + 1013904223u;
		int sx = (int)((seed >> 16) % (uint32_t)fb->w);
		seed = seed * 1664525u + 1013904223u;
		int sy = (int)((seed >> 16) % (uint32_t)fb->h);
		if (sy < y0 - 2 || sy > y1 + 2) continue;
		int dx = sx - cx, dy = sy - cy;
		if (dx * dx + dy * dy < 60 * 60) continue;
		arcade_fb_fill_circle_alpha(fb, sx, sy, (i % 7 == 0) ? 2 : 1, star_c, (uint8_t)(45 + (i % 4) * 20));
	}

	arcade_fb_hline(fb, cx - 208, cx - 38, cy, axis_c, 120);
	arcade_fb_hline(fb, cx + 38, cx + 208, cy, axis_c, 120);
	arcade_fb_vline(fb, cx, cy - 208, cy - 38, axis_c, 120);
	arcade_fb_vline(fb, cx, cy + 38, cy + 208, axis_c, 120);

	for (int i = 0; i < ORBIT_RING_COUNT; i++) {
		arcade_fb_draw_ring(fb, cx, cy, RING_RADII[i] - 1, RING_RADII[i] + 1, ring_c, 210);
	}

	// Composite the small center planet icon (64x64) directly
	scene_sphere(fb, cx, cy, 26, 0xfda4);
}

static bool sector_blocked_on_other_rings(const orbit_ctx_t *o, int skip_ring, float ang) {
	int free_rings = 0;
	for (int r = 0; r < ORBIT_RING_COUNT; r++) {
		if (r == skip_ring) continue;
		bool blocked = false;
		for (int i = 0; i < ORBIT_OBS_MAX; i++) {
			const orbit_obstacle_t *ob = &o->obstacles[i];
			if (ob->active && ob->ring == r && fabsf(orbit_angle_diff(ang, ob->angle)) < ob->half_arc + 0.55f) {
				blocked = true;
				break;
			}
		}
		if (!blocked) free_rings++;
	}
	return free_rings == 0;
}

static void spawn_star(orbit_ctx_t *o, uint8_t kind) {
	int slot = -1;
	for (int i = 0; i < ORBIT_STARS_MAX; i++) {
		if (!o->stars[i].active) {
			slot = i;
			break;
		}
	}
	if (slot < 0) return;

	int ring = (o->ring + 1 + (int)(rand_unit(o) * 2.0f)) % ORBIT_RING_COUNT;
	float ang = orbit_wrap_angle(o->orb_angle + 1.1f + rand_unit(o) * 4.0f);
	o->stars[slot] = (orbit_star_t){
		.ring = ring,
		.angle = ang,
		.pulse = rand_unit(o) * TWO_PI_F,
		.life = kind == 1 ? 7.0f : 0.0f,
		.max_life = 7.0f,
		.kind = kind,
		.active = true,
	};
}

static void spawn_obstacle(orbit_ctx_t *o) {
	int slot = -1;
	int active_count = 0;
	int max_allowed = LV_MIN(7, 2 + o->wave);
	for (int i = 0; i < ORBIT_OBS_MAX; i++) {
		if (o->obstacles[i].active) active_count++;
		else if (slot < 0) slot = i;
	}
	if (slot < 0 || active_count >= max_allowed) return;

	int ring = (int)(rand_unit(o) * (float)ORBIT_RING_COUNT) % ORBIT_RING_COUNT;
	if (ring == o->safe_lane) ring = (ring + 1) % ORBIT_RING_COUNT;
	float ang = orbit_wrap_angle(o->orb_angle + (float)M_PI * (0.65f + rand_unit(o) * 0.85f));
	if (sector_blocked_on_other_rings(o, ring, ang)) return;

	uint8_t kind = (o->encounter == 1 || (o->wave >= 2 && rand_unit(o) > 0.55f)) ? 1 : 0;
	float spd_mag = fminf(1.85f, 0.75f + rand_unit(o) * 0.55f + (float)(o->wave - 1) * 0.14f);
	float dir = (ring & 1) ? -1.0f : 1.0f;
	if (rand_unit(o) > 0.72f) dir = -dir;

	o->obstacles[slot] = (orbit_obstacle_t){
		.ring = ring,
		.angle = ang,
		.half_arc = kind == 0 ? (0.24f + (o->wave > 3 ? 0.04f : 0.0f)) : 0.14f,
		.speed = spd_mag * dir,
		.telegraph = 0.72f,
		.kind = kind,
		.grazed = false,
		.active = true,
	};
}

// Every encounter reserves one reachable lane and shows its reward path ahead of the player.
static void begin_encounter(orbit_ctx_t *o) {
	o->encounter = (o->encounter + 1 + arcade_rng_next(&o->rng) % 2) % 3;
	o->encounter_time = 5.5f;
	o->safe_lane = o->ring == 1 ? (arcade_rng_next(&o->rng) % 2) * 2 : 1;
	memset(o->obstacles, 0, sizeof(o->obstacles));
	memset(o->stars, 0, sizeof(o->stars));
	o->pulsar_active = false;
	o->pulsar_cd = 1.0f;
	int trail_ring = (o->safe_lane + 1 + arcade_rng_next(&o->rng) % 2) % 3;
	for (int i = 0; i < 3; i++) {
		o->stars[i] = (orbit_star_t){.ring = trail_ring, .angle = orbit_wrap_angle(o->orb_angle + 1.75f + i*0.37f),
			.active = true, .kind = i == 2, .life = 5.5f, .max_life = 5.5f};
	}
	spawn_obstacle(o);
	if (o->encounter == 1) spawn_obstacle(o);
}

static void reset_game_state(orbit_ctx_t *o, bool attract) {
	memset(o->obstacles, 0, sizeof(o->obstacles));
	memset(o->stars, 0, sizeof(o->stars));
	arcade_particles_clear(&o->particles);

	o->ring = 1;
	o->next_dir = 1;
	o->target_ring = 1;
	o->transferring = false;
	o->transfer_t = 0.0f;
	o->cur_r = (float)RING_RADII[1];
	o->from_r = o->cur_r;
	o->to_r = o->cur_r;
	o->orb_angle = -((float)M_PI * 0.5f);
	o->buffered_step = 0;
	o->buffer_timer = 0.0f;
	o->grace_t = 0.85f;
	o->trail_head = 0;
	o->trail_fill = 0;

	o->pulsar_active = false;
	o->pulsar_safe_ring = 1;
	o->pulsar_angle = 0.0f;
	o->pulsar_telegraph = 0.0f;
	o->pulsar_fire_t = 0.0f;
	o->pulsar_cd = 7.5f;

	o->wave = 1;
	o->stars_collected = 0;
	o->combo = 0;
	o->combo_t = 0.0f;
	o->time_s = 0.0f;
	o->spawn_timer = 0.5f;
	o->core_pulse_r = 26.0f;
	o->core_pulse_a = 0.0f;
	o->shake_t = 0.0f;
	o->rng = arcade_rng_seed();

	if (attract) {
		spawn_star(o, 0);
		o->obstacles[0] = (orbit_obstacle_t){
			.ring = 0, .angle = 0.8f, .half_arc = 0.28f, .speed = 0.9f, .active = true,
		};
		o->obstacles[1] = (orbit_obstacle_t){
			.ring = 2, .angle = 3.8f, .half_arc = 0.26f, .speed = -0.8f, .active = true,
		};
	} else {
		o->encounter = arcade_rng_next(&o->rng) % 3;
		begin_encounter(o);
	}
}

static void start_directed_transfer(orbit_ctx_t *o, int step_dir) {
	int cy = orbit_cy_for_h(o->shell ? arcade_shell_height(o->shell) : 800);
	int nxt = LV_CLAMP(0, o->ring + step_dir, ORBIT_RING_COUNT - 1);
	if (nxt == o->ring) {
		arcade_sfx_play(ARCADE_SFX_TAP);
		return;
	}
	o->next_dir = step_dir;
	o->ring = nxt;
	o->target_ring = nxt;
	o->from_r = o->cur_r;
	o->to_r = (float)RING_RADII[nxt];
	o->transferring = true;
	o->transfer_t = 0.0f;

	float ox = (float)ORBIT_CX + o->cur_r * arcade_cos_ang(o->orb_angle);
	float oy = (float)cy + o->cur_r * arcade_sin_ang(o->orb_angle);
	arcade_sfx_play_pitch(ARCADE_SFX_SLIDE, o->ring * 2);
	arcade_particles_spawn_burst(&o->particles, ox, oy, 6, 30.0f, 95.0f, 0.0f, 3.0f, 0.25f,
								 arcade_lv_to_565(theme()->accent), 0);
}

static void on_enter(arcade_shell_t *shell, void *ctx) {
	orbit_ctx_t *o = (orbit_ctx_t *)ctx;
	o->shell = shell;
	
	
	reset_game_state(o, true);
}

static void orbit_on_exit(arcade_shell_t *shell, void *ctx) {
	(void)shell;
	orbit_ctx_t *o = (orbit_ctx_t *)ctx;
	arcade_particles_clear(&o->particles);
}

static void on_start(arcade_shell_t *shell, void *ctx) {
	orbit_ctx_t *o = (orbit_ctx_t *)ctx;
	o->shell = shell;
	
	reset_game_state(o, false);
	arcade_shell_set_score(shell, 0);
}

static void on_theme_changed(arcade_shell_t *shell, void *ctx) {
	(void)ctx;
	arcade_shell_force_full_redraw(shell);
}

static void on_touch(arcade_shell_t *shell, void *ctx, bool pressed, int x, int y) {
	(void)y;
	orbit_ctx_t *o = (orbit_ctx_t *)ctx;
	if (!pressed) return;
	int step_dir = (x < arcade_shell_width(shell) / 2) ? -1 : 1;
	if (!o->transferring) {
		start_directed_transfer(o, step_dir);
	} else {
		o->buffered_step = step_dir;
		o->buffer_timer = 0.09f;
	}
}

static void trigger_nova_shockwave(orbit_ctx_t *o, int cy, float sx, float sy) {
	uint16_t gold = arcade_rgb565(245, 205, 108);
	arcade_particles_spawn_ring(&o->particles, (float)ORBIT_CX, (float)cy, 32.0f, 380.0f, 0.55f, gold);
	arcade_particles_spawn_burst(&o->particles, sx, sy, 22, 60.0f, 220.0f, 0.0f, 4.5f, 0.5f, gold, 0);
	o->grace_t = 0.6f;
	o->pulsar_active = false;

	for (int i = 0; i < ORBIT_OBS_MAX; i++) {
		orbit_obstacle_t *ob = &o->obstacles[i];
		if (!ob->active) continue;
		float ox = (float)ORBIT_CX + (float)RING_RADII[ob->ring] * arcade_cos_ang(ob->angle);
		float oy = (float)cy + (float)RING_RADII[ob->ring] * arcade_sin_ang(ob->angle);
		arcade_particles_spawn_burst(&o->particles, ox, oy, 8, 40.0f, 150.0f, 0.0f, 3.5f, 0.38f, gold, 1);
		ob->active = false;
	}
}

static void on_step(arcade_shell_t *shell, void *ctx, float dt) {
	orbit_ctx_t *o = (orbit_ctx_t *)ctx;
	o->time_s += dt;
	bool playing = arcade_shell_state(shell) == ARCADE_ST_PLAYING;
	int cy = orbit_cy_for_h(arcade_shell_height(shell));

	if (o->shake_t > 0.0f) o->shake_t = fmaxf(0.0f, o->shake_t - dt);
	if (o->grace_t > 0.0f) o->grace_t = fmaxf(0.0f, o->grace_t - dt);
	if (o->combo_t > 0.0f) {
		o->combo_t -= dt;
		if (o->combo_t <= 0.0f) o->combo = 0;
	}
	if (o->core_pulse_a > 0.0f) {
		o->core_pulse_r += 240.0f * dt;
		o->core_pulse_a = fmaxf(0.0f, o->core_pulse_a - 1.8f * dt);
	}

	// Radial ring transfer interpolation
	if (o->transferring) {
		o->transfer_t += dt / TRANSFER_DUR;
		if (o->transfer_t >= 1.0f) {
			o->transfer_t = 1.0f;
			o->cur_r = o->to_r;
			o->transferring = false;
		} else {
			float t = o->transfer_t;
			float eased = 1.0f - (1.0f - t) * (1.0f - t);
			o->cur_r = o->from_r + (o->to_r - o->from_r) * eased;
		}
	} else if (o->buffered_step != 0) {
		int step_dir = o->buffered_step;
		o->buffered_step = 0;
		start_directed_transfer(o, step_dir);
	}
	if (o->buffered_step != 0) {
		o->buffer_timer -= dt;
		if (o->buffer_timer <= 0.0f) o->buffered_step = 0;
	}

	float wave_mul = fminf(1.65f, 1.0f + (float)(o->wave - 1) * 0.08f);
	float base_omega = o->ring == 0 ? 2.25f : (o->ring == 1 ? 1.85f : 1.50f);
	o->orb_angle = orbit_wrap_angle(o->orb_angle + base_omega * wave_mul * dt);

	float orb_x = (float)ORBIT_CX + o->cur_r * arcade_cos_ang(o->orb_angle);
	float orb_y = (float)cy + o->cur_r * arcade_sin_ang(o->orb_angle);
	o->orb_trail_x[o->trail_head] = orb_x;
	o->orb_trail_y[o->trail_head] = orb_y;
	o->trail_head = (o->trail_head + 1) % ORBIT_TRAIL_LEN;
	if (o->trail_fill < ORBIT_TRAIL_LEN) o->trail_fill++;

	// Obstacles, true radial-interpolated collision (fixes transfer invulnerability!), and Near-Miss Graze
	for (int i = 0; i < ORBIT_OBS_MAX; i++) {
		orbit_obstacle_t *ob = &o->obstacles[i];
		if (!ob->active) continue;
		ob->angle = orbit_wrap_angle(ob->angle + ob->speed * dt);
		if (ob->telegraph > 0.0f) {
			ob->telegraph -= dt;
			continue;
		}

		float ang_dist = fabsf(orbit_angle_diff(o->orb_angle, ob->angle));
		if (ang_dist > 1.3f) {
			ob->grazed = false;
		}
		float rad_dist = fabsf(o->cur_r - (float)RING_RADII[ob->ring]);
		float hit_margin = fmaxf(0.04f, ob->half_arc - 0.04f);

		if (playing && o->grace_t <= 0.0f && rad_dist <= 10.5f && ang_dist <= hit_margin) {
			o->shake_t = 0.25f;
			arcade_particles_spawn_burst(&o->particles, orb_x, orb_y, 26, 65.0f, 240.0f, 0.0f, 4.5f, 0.55f,
										 arcade_rgb565(242, 109, 125), 0);
			arcade_shell_trigger_game_over(shell, 480);
			return;
		}

		// Near-Miss Graze: last-split-second ring transfer dodge or outer-fringe skim!
		if (playing && !ob->grazed && o->grace_t <= 0.0f &&
			((o->transferring && rad_dist > 10.5f && rad_dist <= 38.0f && ang_dist <= ob->half_arc + 0.08f) ||
			 (rad_dist <= 10.5f && ang_dist > hit_margin && ang_dist <= ob->half_arc + 0.11f))) {
			ob->grazed = true;
			arcade_sfx_play_pitch(ARCADE_SFX_COMBO, 5);
			arcade_shell_set_score(shell, arcade_shell_score(shell) + 2);
			arcade_particles_spawn_burst(&o->particles, orb_x, orb_y, 8, 45.0f, 140.0f, 0.0f, 2.8f, 0.30f, 0xFFE6, 0);
			arcade_shell_show_callout(shell, "+2", lv_color_hex(0xF7C552));
		}
	}

	// Wave 3+ telegraphed Pulsar Sweep beam (hits 2 rings, leaving pulsar_safe_ring open!)
	if (playing && o->encounter == 2) {
		if (!o->pulsar_active) {
			o->pulsar_cd -= dt;
			if (o->pulsar_cd <= 0.0f) {
				o->pulsar_active = true;
				o->pulsar_safe_ring = o->safe_lane;
				o->pulsar_angle = orbit_wrap_angle(o->orb_angle + (float)M_PI * (0.42f + rand_unit(o) * 0.28f));
				o->pulsar_telegraph = 1.15f;
				o->pulsar_fire_t = 0.42f;
				arcade_sfx_play(ARCADE_SFX_TAP);
			}
		} else if (o->pulsar_telegraph > 0.0f) {
			o->pulsar_telegraph -= dt;
			if (o->pulsar_telegraph <= 0.0f) {
				arcade_sfx_play(ARCADE_SFX_SLIDE);
			}
		} else {
			o->pulsar_fire_t -= dt;
			o->pulsar_angle = orbit_wrap_angle(o->pulsar_angle + 0.85f * dt);
			float safe_r = (float)RING_RADII[o->pulsar_safe_ring];
			if (o->grace_t <= 0.0f && fabsf(o->cur_r - safe_r) > 22.0f &&
				fabsf(orbit_angle_diff(o->orb_angle, o->pulsar_angle)) <= 0.11f) {
				o->shake_t = 0.25f;
				arcade_particles_spawn_burst(&o->particles, orb_x, orb_y, 26, 65.0f, 240.0f, 0.0f, 4.5f, 0.55f,
											 arcade_rgb565(242, 109, 125), 0);
				arcade_shell_trigger_game_over(shell, 480);
				return;
			}
			if (o->pulsar_fire_t <= 0.0f) {
				o->pulsar_active = false;
				o->pulsar_cd = fmaxf(5.5f, 8.8f - (float)o->wave * 0.35f);
			}
		}
	}

	// Stars & Nova pickups
	int active_stars = 0;
	for (int i = 0; i < ORBIT_STARS_MAX; i++) {
		orbit_star_t *st = &o->stars[i];
		if (!st->active) continue;
		st->pulse += dt * 4.5f;
		if (st->kind == 1) {
			st->life -= dt;
			if (st->life <= 0.0f) {
				st->active = false;
				continue;
			}
		}
		active_stars++;

		if (playing) {
			float sx = (float)ORBIT_CX + (float)RING_RADII[st->ring] * arcade_cos_ang(st->angle);
			float sy = (float)cy + (float)RING_RADII[st->ring] * arcade_sin_ang(st->angle);
			float dx = orb_x - sx, dy = orb_y - sy;
			if (dx * dx + dy * dy <= 24.0f * 24.0f) {
				bool is_nova = st->kind == 1;
				st->active = false;
				o->stars_collected++;
				o->combo = (o->combo_t > 0.0f) ? (o->combo + 1) : 1;
				o->combo_t = 3.2f;

				int pts = is_nova ? 5 : (o->combo >= 3 ? 2 : 1);
				int next_score = arcade_shell_score(shell) + pts;
				arcade_shell_set_score(shell, next_score);

				o->core_pulse_r = 26.0f;
				o->core_pulse_a = 1.0f;

				if (is_nova) {
					arcade_sfx_play(ARCADE_SFX_SMASH);
					trigger_nova_shockwave(o, cy, sx, sy);
				} else {
					arcade_sfx_play_pitch(ARCADE_SFX_POP, LV_MIN(7, o->combo));
					arcade_particles_spawn_burst(&o->particles, sx, sy, 12, 45.0f, 155.0f, 0.0f, 3.4f, 0.36f,
												 arcade_rgb565(120, 232, 235), 0);
				}

				int next_wave = 1 + (next_score / 10);
				if (next_wave > o->wave) {
					o->wave = next_wave;
					char msg[32];
					snprintf(msg, sizeof(msg), tr("arcade_wave"), o->wave);
					arcade_shell_show_callout(shell, msg, theme()->accent);
					arcade_particles_spawn_ring(&o->particles, (float)ORBIT_CX, (float)cy, 40.0f, 300.0f, 0.45f,
												arcade_lv_to_565(theme()->accent));
				} else if (!is_nova && o->combo >= 3) {
					char msg[32];
					snprintf(msg, sizeof(msg), "+%d  ×%d", pts, o->combo);
					arcade_shell_show_callout(shell, msg, lv_color_hex(0x78E8EB));
				}

				if (o->stars_collected % 7 == 0) {
					spawn_star(o, 1);
				}
			}
		}
	}

	if (playing && active_stars < 2) spawn_star(o, 0);

	if (playing) {
		o->encounter_time -= dt;
		if (o->encounter_time <= 0) begin_encounter(o);
		o->spawn_timer -= dt;
		if (o->spawn_timer <= 0.0f) {
			spawn_obstacle(o);
			o->spawn_timer = fmaxf(1.15f, 2.6f - (float)(o->wave - 1) * 0.22f);
		}
	}

	arcade_particles_update(&o->particles, dt);
}

static void on_render(arcade_shell_t *shell, void *ctx, bool full_redraw) {
	orbit_ctx_t *o = (orbit_ctx_t *)ctx;
	arcade_fb_t *fb = arcade_shell_fb(shell);
	if (!fb || !fb->pixels) return;

	
	const theme_palette_t *pal = theme();
	int base_cy = orbit_cy_for_h(fb->h);

	int sx_shake = 0, sy_shake = 0;
	if (o->shake_t > 0.0f) {
		int ph = (int)(o->shake_t * 140.0f);
		sx_shake = (ph & 1) ? 3 : -3;
		sy_shake = (ph & 2) ? 2 : -2;
	}
	int cx = ORBIT_CX + sx_shake;
	int cy = base_cy + sy_shake;

	int band_y0 = full_redraw ? 0 : LV_MAX(0, base_cy - ARENA_HALF_BOX - 8);
	int band_y1 = full_redraw ? (fb->h - 1) : LV_MIN(fb->h - 1, base_cy + ARENA_HALF_BOX + 8);
	scene_backdrop(fb, scene_observatory, band_y0, band_y1);
	draw_arena_backdrop(fb, cx, cy, band_y0, band_y1, true);
	if (full_redraw) {
		arcade_fb_mark_full(fb);
	} else {
		arcade_fb_mark_dirty(fb, 0, band_y0, fb->w - 1, band_y1);
	}

	uint16_t accent565 = arcade_lv_to_565(pal->accent);
	float beat = arcade_shell_music_pulse(shell);

	// Subtle two-side control guide arcs on left/right during first 2.5s of play
	if (arcade_shell_state(shell) == ARCADE_ST_PLAYING && o->time_s < 2.5f) {
		uint8_t guide_a = (uint8_t)((1.0f - o->time_s / 2.5f) * 110.0f);
		if (o->ring > 0) {
			arcade_fb_draw_arc(fb, cx, cy, RING_RADII[o->ring - 1] - 1, RING_RADII[o->ring - 1] + 1, 2.55f, 3.73f,
							   accent565, guide_a);
		}
		if (o->ring < ORBIT_RING_COUNT - 1) {
			arcade_fb_draw_arc(fb, cx, cy, RING_RADII[o->ring + 1] - 1, RING_RADII[o->ring + 1] + 1, -0.59f, 0.59f,
							   accent565, guide_a);
		}
	}

	// Rotating tick marks along each ring
	for (int r = 0; r < ORBIT_RING_COUNT; r++) {
		float base_a = o->time_s * ((r & 1) ? -0.45f : 0.35f);
		uint16_t pip_c = pal->dark ? arcade_rgb565(75, 96, 124) : arcade_rgb565(140, 156, 178);
		for (int k = 0; k < 6; k++) {
			float a = base_a + (float)k * (TWO_PI_F / 6.0f);
			int px = cx + (int)lroundf((float)RING_RADII[r] * arcade_cos_ang(a));
			int py = cy + (int)lroundf((float)RING_RADII[r] * arcade_sin_ang(a));
			arcade_fb_fill_circle_alpha(fb, px, py, 2, pip_c, 160);
		}
	}

	if (o->core_pulse_a > 0.01f) {
		int pr = (int)lroundf(o->core_pulse_r);
		if (pr > 4 && pr < 215) {
			arcade_fb_draw_ring(fb, cx, cy, pr - 2, pr, accent565, (uint8_t)(190.0f * o->core_pulse_a));
		}
	}

	float breathe = 0.5f + 0.5f * arcade_sin_ang(o->time_s * 3.0f);
	int outer_core_r = 24 + (int)lroundf(breathe * 5.0f + beat * 14.0f);
	arcade_fb_draw_ring(fb, cx, cy, outer_core_r + 3, outer_core_r + 5, accent565, (uint8_t)(95 + beat * 130.0f));

	// Wave 3+ Pulsar beam visualization (segments on unsafe rings, emerald safe-gate arc on safe ring)
	if (o->pulsar_active) {
		float ca = arcade_cos_ang(o->pulsar_angle), sa = arcade_sin_ang(o->pulsar_angle);
		uint16_t beam_c = arcade_rgb565(255, 98, 128);
		uint8_t beam_a = o->pulsar_telegraph > 0.0f
							 ? (uint8_t)(60.0f + 75.0f * fabsf(arcade_sin_ang(o->time_s * 18.0f)))
							 : 235;
		int thick = o->pulsar_telegraph > 0.0f ? 2 : 7;
		for (int r = 0; r < ORBIT_RING_COUNT; r++) {
			int rr = RING_RADII[r];
			if (r == o->pulsar_safe_ring) {
				arcade_fb_draw_arc(fb, cx, cy, rr - 3, rr + 3, o->pulsar_angle - 0.22f, o->pulsar_angle + 0.22f,
								   arcade_rgb565(96, 236, 172), 185);
			} else {
				int r0 = rr - 25, r1 = rr + 25;
				arcade_fb_draw_line_thick(fb, cx + (int)lroundf((float)r0 * ca), cy + (int)lroundf((float)r0 * sa),
										  cx + (int)lroundf((float)r1 * ca), cy + (int)lroundf((float)r1 * sa), thick,
										  beam_c, beam_a);
			}
		}
	}

	uint16_t coral = arcade_rgb565(242, 109, 125);
	uint16_t amber = arcade_rgb565(242, 166, 90);
	for (int i = 0; i < ORBIT_OBS_MAX; i++) {
		const orbit_obstacle_t *ob = &o->obstacles[i];
		if (!ob->active) continue;
		int rr = RING_RADII[ob->ring];
		uint16_t col = ob->kind == 1 ? amber : coral;
		if (ob->telegraph > 0.0f) {
			uint8_t alpha = (uint8_t)(65.0f + 75.0f * fabsf(arcade_sin_ang(o->time_s * 14.0f)));
			arcade_fb_draw_arc(fb, cx, cy, rr - 3, rr + 3, ob->angle - ob->half_arc, ob->angle + ob->half_arc, col,
							   alpha);
		} else if (ob->kind == 0) {
			arcade_fb_draw_arc(fb, cx, cy, rr - 6, rr + 6, ob->angle - ob->half_arc, ob->angle + ob->half_arc, col,
							   245);
			arcade_fb_draw_arc(fb, cx, cy, rr - 2, rr + 2, ob->angle - ob->half_arc * 0.7f,
							   ob->angle + ob->half_arc * 0.7f, arcade_rgb565(255, 215, 220), 190);
		} else {
			int ox = cx + (int)lroundf((float)rr * arcade_cos_ang(ob->angle));
			int oy = cy + (int)lroundf((float)rr * arcade_sin_ang(ob->angle));
			arcade_fb_fill_circle_alpha(fb, ox, oy, 14, col, 70);
			float d = 10.0f;
			float qx[4] = {(float)ox, (float)ox + d, (float)ox, (float)ox - d};
			float qy[4] = {(float)oy - d, (float)oy, (float)oy + d, (float)oy};
			arcade_fb_fill_quad(fb, qx, qy, col, 255);
			arcade_fb_fill_circle(fb, ox, oy, 4, arcade_rgb565(255, 240, 215));
		}
	}

	for (int i = 0; i < ORBIT_STARS_MAX; i++) {
		const orbit_star_t *st = &o->stars[i];
		if (!st->active) continue;
		int rr = RING_RADII[st->ring];
		int sx = cx + (int)lroundf((float)rr * arcade_cos_ang(st->angle));
		int sy = cy + (int)lroundf((float)rr * arcade_sin_ang(st->angle));
		float sc = 1.0f + 0.16f * arcade_sin_ang(st->pulse);

		if (st->kind == 1) {
			uint16_t gold = arcade_rgb565(245, 205, 108);
			float frac = fmaxf(0.0f, st->life / st->max_life);
			arcade_fb_draw_arc(fb, sx, sy, 13, 16, -(float)M_PI * 0.5f, -(float)M_PI * 0.5f + TWO_PI_F * frac, gold,
							   220);
			arcade_fb_fill_circle_alpha(fb, sx, sy, 12, gold, 85);
			arcade_fb_fill_circle(fb, sx, sy, (int)lroundf(7.5f * sc), gold);
			arcade_fb_fill_circle(fb, sx, sy, 3, arcade_rgb565(255, 252, 240));
		} else {
			uint16_t cyan = arcade_rgb565(118, 232, 236);
			arcade_fb_fill_circle_alpha(fb, sx, sy, 13, cyan, 65);
			float d = 8.5f * sc;
			float qx[4] = {(float)sx, (float)sx + d, (float)sx, (float)sx - d};
			float qy[4] = {(float)sy - d, (float)sy, (float)sy + d, (float)sy};
			arcade_fb_fill_quad(fb, qx, qy, cyan, 255);
			arcade_fb_fill_circle(fb, sx, sy, 3, arcade_rgb565(245, 255, 255));
		}
	}

	for (int i = 0; i < o->trail_fill; i++) {
		int idx = (o->trail_head - o->trail_fill + i + ORBIT_TRAIL_LEN * 2) % ORBIT_TRAIL_LEN;
		float norm = (float)(i + 1) / (float)ORBIT_TRAIL_LEN;
		uint8_t alpha = (uint8_t)(165.0f * norm * norm);
		int r = 2 + (int)lroundf(4.0f * norm);
		arcade_fb_fill_circle_alpha(fb, (int)lroundf(o->orb_trail_x[idx]) + sx_shake,
									(int)lroundf(o->orb_trail_y[idx]) + sy_shake, r, accent565, alpha);
	}

	int ox = cx + (int)lroundf(o->cur_r * arcade_cos_ang(o->orb_angle));
	int oy = cy + (int)lroundf(o->cur_r * arcade_sin_ang(o->orb_angle));
	arcade_fb_fill_circle_alpha(fb, ox, oy, 16, accent565, 85);
	arcade_fb_fill_circle(fb, ox, oy, 9, accent565);
	arcade_fb_fill_circle(fb, ox, oy, 4, arcade_rgb565(250, 253, 255));

	arcade_particles_draw(&o->particles, fb, 0.0f);
}

void game_orbit_open(gui_config_t *cfg) {
	static const arcade_game_ops_t ops = {
		.id = "orbit",
		.title_tag = "game_orbit",
		.subtitle_tag = "game_orbit_desc",
		.hint_tag = "game_orbit_hint",
		.icon = &icon_atelier_orbit,
		.dark_backdrop = true,
		.target_fps = 30,
		.use_framebuffer = true,
		.music_reactive = true,
		.on_enter = on_enter,
		.on_exit = orbit_on_exit,
		.on_start = on_start,
		.on_step = on_step,
		.on_render = on_render,
		.on_touch = on_touch,
		.on_theme_changed = on_theme_changed,
	};
	if (!g_orbit.shell) {
		g_orbit.shell = arcade_shell_create(cfg, &ops, &g_orbit);
	}
	arcade_shell_open(g_orbit.shell);
}

int game_orbit_best(void) { return arcade_shell_read_best("orbit"); }
