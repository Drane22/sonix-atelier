// Run with make test-arcade-contacts. Drive the same steps and tap handler as the shell.
#include <assert.h>

#include "src/gui/arcade/game_prism.c"
#include "src/gui/arcade/game_towerblast.c"

struct arcade_shell {
	int height, score;
	bool ended;
};

arcade_state_t arcade_shell_state(const arcade_shell_t *s) { return ARCADE_ST_PLAYING; }
int arcade_shell_height(const arcade_shell_t *s) { return s->height; }
int arcade_shell_score(const arcade_shell_t *s) { return s->score; }
void arcade_shell_set_score(arcade_shell_t *s, int score) { s->score = score; }
void arcade_shell_trigger_game_over(arcade_shell_t *s, int delay_ms) { s->ended = true; }
void arcade_shell_set_sub_hud(arcade_shell_t *s, const char *text) {}
void arcade_shell_show_callout(arcade_shell_t *s, const char *text, lv_color_t color) {}
void arcade_sfx_play(arcade_sfx_id_t id) {}
void arcade_sfx_play_pitch(arcade_sfx_id_t id, int pitch) {}
bool theme_is_dark(void) { return true; }
const char *tr(const char *key) { return key; }

static void test_tower_landing(float dt, bool lethal_landing) {
	arcade_shell_t ui = {0};
	tower_t g = {.stage = 1, .rng = 3749, .ball_y = -20};
	populate_stage_floors(&g);
	floor_t opening = {.gap = 3, .gap_width = 4};
	floor_t hazard = {.base_danger = 3, .danger_width = 3};
	for (int i = 0; i < 3; i++) g.floors[i] = opening;
	g.floors[3] = hazard;
	g.floors[4] = lethal_landing ? hazard : (floor_t){0};

	bool bounced = false;
	for (int i = 0; i < 3000 && !ui.ended && !bounced; i++) {
		int depth = g.depth;
		float before = depth * SPACING + g.ball_y;
		step(&ui, &g, dt);
		float travel = g.depth * SPACING + g.ball_y - before;
		assert(travel >= -0.001f && travel <= 325 * dt + 0.001f);
		if (depth != g.depth) {
			assert(g.depth == depth + 1 && g.depth <= 4);
			assert(g.breaks[depth % BREAK_SLOTS].active && g.breaks[depth % BREAK_SLOTS].depth == depth);
			assert(g.ball_y < 0 && g.velocity > 0);
		}
		bounced = g.velocity < 0;
	}
	assert(g.depth == 4 && g.ball_y == 0);
	assert(ui.ended == lethal_landing && bounced != lethal_landing);
}

static bool fly_through_gates(int color, float speed, int phase_px, int height, float dt) {
	arcade_shell_t ui = {.height = height};
	prism_ctx_t g = {.shell = &ui, .rng = 1, .ball_color = color};
	float center = height * 0.5f;
	g.obs[0] = (prism_obs_t){
		.kind = PRISM_OBS_GATES, .active = true, .world_y = center, .cx = ORB_X,
		.rot = phase_px / BAR_PIXELS_PER_RAD, .rot_speed = speed, .r_out = GATE_OFFSET + BAR_HALF_H,
	};
	float clearance = GATE_OFFSET + BAR_HALF_H + ORB_R;
	g.orb_y = center + clearance + 35;
	for (int i = 0; i < 300 && !g.dead; i++) {
		if (g.orb_vy >= -150) prism_on_touch(&ui, &g, true, ORB_X, center);
		prism_on_step(&ui, &g, dt);
		if (!g.dead && g.orb_y < center - clearance - 8) return true;
	}
	return false;
}

static void test_bounce_between_rows(float dt) {
	arcade_shell_t ui = {.height = 800};
	float center = ui.height * 0.5f;
	for (int phase = 0; phase < SCREEN_W; phase += BAR_WIDTH) {
		prism_ctx_t g = {.shell = &ui, .rng = 1};
		g.obs[0] = (prism_obs_t){
			.kind = PRISM_OBS_GATES, .active = true, .world_y = center, .cx = ORB_X,
			.rot = phase / BAR_PIXELS_PER_RAD, .rot_speed = 1.2f, .r_out = GATE_OFFSET + BAR_HALF_H,
		};
		float pocket = GATE_OFFSET - BAR_HALF_H - ORB_R;
		float start = center + pocket - 12;
		g.orb_y = start;
		prism_on_touch(&ui, &g, true, ORB_X, start);
		do {
			prism_on_step(&ui, &g, dt);
			assert(!g.dead && fabsf(g.orb_y - center) < pocket);
		} while (g.orb_y < start);
	}
}

static void test_prism_spacing(void) {
	float bounce = JUMP_VY * JUMP_VY / (2 * GRAVITY);
	assert(2 * (GATE_OFFSET - BAR_HALF_H - ORB_R) > bounce + 20);
	for (int seed = 1; seed <= 256; seed++) {
		prism_ctx_t g = {.rng = seed};
		float bottom = 1000;
		for (int i = 0; i < 40; i++) {
			prism_obs_t o;
			populate_obstacle(&g, &o, bottom, i);
			assert(fabsf(o.world_y + o.r_out - bottom) < 0.01f);
			float top = o.world_y - o.r_out;
			assert(top - g.next_bottom_y > bounce + 2 * ORB_R);
			assert(top - o.shifter_y > ORB_R + 22);
			assert(o.shifter_y - g.next_bottom_y > ORB_R + 22);
			if (o.kind == PRISM_OBS_DOUBLE)
				assert((o.r_out - 18) - (o.r_in + 16) - 2 * ORB_R > bounce);
			bottom = g.next_bottom_y;
		}
	}
}

int main(void) {
	arcade_lut_init();
	const int rates[] = {20, 30, 60, 120};
	for (unsigned i = 0; i < LV_ARRAYLEN(rates); i++) {
		float dt = 1.0f / rates[i];
		test_tower_landing(dt, true);
		test_tower_landing(dt, false);
		test_bounce_between_rows(dt);
	}
	test_prism_spacing();
	for (int height = 720; height <= 800; height += 80) {
		for (int color = 0; color < 4; color++) {
			for (int dir = -1; dir <= 1; dir += 2) {
				for (int rate = 20; rate <= 60; rate += 10) {
					int windows = 0;
					for (int phase = 0; phase < SCREEN_W; phase += 4)
						windows += fly_through_gates(color, dir * 1.2f, phase, height, 1.0f / rate);
					assert(windows >= 5);
				}
			}
		}
	}
	puts("Tower Blast floor transitions and Prism gate clearances passed.");
	return 0;
}
