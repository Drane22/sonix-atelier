#include "game_word.h"
#include "arcade_scene.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "arcade_shell.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"

enum { ROWS = 6, COLS = 5, STAGGER_MS = 92, FLIP_MS = 220 };
typedef enum { EMPTY, ABSENT, PRESENT, EXACT } mark_t;

static const char answers[][COLS + 1] = {
	"AMBER", "APPLE", "AUDIO", "BEACH", "BERRY", "BLOOM", "BRASS", "BRAVE", "BRICK", "BRUSH",
	"CABLE", "CEDAR", "CHARM", "CHESS", "CHIME", "CHORD", "CIDER", "CLOUD", "COAST", "CORAL",
	"CRANE", "CREAM", "CREST", "CROWN", "DANCE", "DIARY", "DRIFT", "DRINK", "DRIVE", "DUSKY",
	"EARTH", "EASEL", "EMBER", "FABLE", "FEAST", "FIELD", "FLAME", "FLARE", "FLOAT", "FLOUR",
	"FLUTE", "FOCUS", "FORGE", "FRESH", "FROST", "FRUIT", "GHOST", "GIANT", "GLASS", "GLEAM",
	"GLOVE", "GRAIN", "GRAPE", "GRASS", "GREEN", "GROVE", "GUIDE", "HABIT", "HAZEL", "HEART",
	"HONEY", "HORSE", "HOTEL", "HOUSE", "HOVER", "IVORY", "JELLY", "JEWEL", "JUICE", "KAYAK",
	"KNACK", "LABEL", "LATCH", "LAYER", "LEMON", "LEVEL", "LIGHT", "LILAC", "LINEN", "LUNAR",
	"LYRIC", "MAGIC", "MAPLE", "MARCH", "MEDAL", "MELON", "METAL", "MIRTH", "MISTY", "MODEL",
	"MOTOR", "MOUND", "MOUSE", "MOVIE", "MURAL", "MUSIC", "NIGHT", "NOBLE", "NOISE", "NORTH",
	"NOTCH", "NOVEL", "OASIS", "OCEAN", "OFFER", "OLIVE", "OPERA", "ORBIT", "ORGAN", "OTTER",
	"PAINT", "PANEL", "PAPER", "PATCH", "PAUSE", "PEACH", "PEARL", "PEDAL", "PETAL", "PHASE",
	"PHOTO", "PIANO", "PIECE", "PILOT", "PITCH", "PIXEL", "PLAIN", "PLANE", "PLANT", "PLATE",
	"PLAZA", "PLUCK", "POEMS", "POINT", "POLAR", "PORCH", "POUCH", "POWER", "PRESS", "PRIDE",
	"PRIME", "PRINT", "PRISM", "PROUD", "PULSE", "QUAIL", "QUART", "QUEEN", "QUEST", "QUIET",
	"QUILT", "QUOTA", "RADAR", "RADIO", "RAINY", "RALLY", "RANCH", "RANGE", "RAVEN", "REACH",
	"READY", "RELAY", "RHYME", "RIDGE", "RIVER", "ROBIN", "ROBOT", "ROCKY", "ROUTE", "ROYAL",
	"RURAL", "SALAD", "SCALE", "SCARF", "SCENE", "SCENT", "SCOOP", "SCORE", "SCOUT", "SCRAP",
	"SHADE", "SHAKE", "SHAPE", "SHARE", "SHARP", "SHEEP", "SHEET", "SHELF", "SHELL", "SHIFT",
	"SHINE", "SHIRT", "SHORE", "SHORT", "SHOUT", "SIGHT", "SILKY", "SKATE", "SKILL", "SLATE",
	"SLICE", "SLIDE", "SLOPE", "SMALL", "SMART", "SMILE", "SMOKE", "SNAIL", "SOLAR", "SOLID",
	"SOLVE", "SONAR", "SONIC", "SOUND", "SOUTH", "SPACE", "SPARE", "SPARK", "SPEAK", "SPEED",
	"SPELL", "SPICE", "SPIKE", "SPINE", "SPLIT", "SPOKE", "SPOON", "SPORT", "SPRAY", "SPRIG",
	"STACK", "STAFF", "STAGE", "STAIN", "STAIR", "STAKE", "STAMP", "STAND", "STARE", "START",
	"STATE", "STEAM", "STEEL", "STEEP", "STEER", "STERN", "STICK", "STILL", "STOCK", "STONE",
	"STOOL", "STORE", "STORM", "STORY", "STRAP", "STRAW", "STRIP", "STRUM", "STUDY", "STYLE",
	"SUGAR", "SUITE", "SUNNY", "SURGE", "SWEEP", "SWEET", "SWELL", "SWIFT", "SWING", "SWIRL",
	"TABLE", "TANGO", "TASTE", "TEACH", "TENSE", "TERRA", "THEME", "THICK", "THING", "THINK",
	"THORN", "THREE", "THROW", "THUMB", "TIDAL", "TIGER", "TIMER", "TOAST", "TODAY", "TOKEN",
	"TONAL", "TONIC", "TOOTH", "TOPAZ", "TORCH", "TOTAL", "TOUCH", "TOUGH", "TOWEL", "TOWER",
	"TRACE", "TRACK", "TRADE", "TRAIL", "TRAIN", "TRAIT", "TREAT", "TREND", "TRIAL", "TRIBE",
	"TRICK", "TROOP", "TROUT", "TRUCE", "TRULY", "TRUNK", "TRUST", "TRUTH", "TULIP", "TUNER",
	"TWICE", "TWINE", "TWIRL", "TWIST", "ULTRA", "UNCLE", "UNDER", "UNION", "UNITE", "UNITY",
	"UNTIL", "UPPER", "URBAN", "USAGE", "USHER", "USUAL", "UTTER", "VALID", "VALUE", "VALVE",
	"VAPOR", "VAULT", "VEGAN", "VENOM", "VENUE", "VERSE", "VIDEO", "VIGOR", "VILLA", "VINYL",
	"VIOLA", "VIPER", "VIRAL", "VIRUS", "VISIT", "VISOR", "VISTA", "VITAL", "VIVID", "VOCAL",
	"VODKA", "VOGUE", "VOICE", "VOTER", "VOWEL", "WAFER", "WAGER", "WAGON", "WAIST", "WALTZ",
	"WASTE", "WATCH", "WATER", "WEARY", "WEAVE", "WEDGE", "WEIGH", "WEIRD", "WHALE", "WHEAT",
	"WHEEL", "WHERE", "WHICH", "WHILE", "WHIRL", "WHISK", "WHITE", "WHOLE", "WHOSE", "WIDEN",
	"WIDER", "WIDTH", "WINDY", "WIPER", "WIRED", "WISER", "WITCH", "WITTY", "WOMAN", "WOMEN",
	"WOODS", "WOODY", "WOOLY", "WORDY", "WORLD", "WORRY", "WORSE", "WORST", "WORTH", "WOULD",
	"WOUND", "WOVEN", "WRATH", "WRECK", "WRING", "WRIST", "WRITE", "WRONG", "WROTE", "YACHT",
	"YEARN", "YEAST", "YIELD", "YOUNG", "YOUTH", "ZEBRA", "ZESTY", "ZONAL",
};
static const char guesses[][COLS + 1] = {
#include "word_guesses.inc"
};

static arcade_shell_t *shell;
static lv_obj_t *board, *board_rows[ROWS], *tiles[ROWS][COLS], *letters[ROWS][COLS], *keys[26];
static lv_obj_t *title_mode_btn, *title_mode_lbl, *title_hard_btn, *title_hard_lbl;
static lv_obj_t *hud_hard_btn, *hud_hard_lbl;
static char entries[ROWS][COLS + 1];
static mark_t marks[ROWS][COLS], key_marks[26];
static const char *answer;
static int row, fill;
static bool daily, prefer_practice, hard_mode, revealing;
static unsigned day_id;
static bool series;
static int series_round, series_guesses, series_wins;

static const char *practice_answer(void) {
	// Persist the shuffle and cursor so a fresh launch continues the same bag.
	uint32_t seed = (uint32_t)config_get_int("word", "practice_seed", 0);
	if (!seed) {
		seed = (arcade_rng_seed() & 0x7fffffffu) | 1u;
		config_set_int("word", "practice_seed", seed);
	}
	uint16_t bag[LV_ARRAYLEN(answers)];
	for (unsigned i = 0; i < LV_ARRAYLEN(bag); i++) {
		unsigned j = arcade_rng_next(&seed) % (i + 1);
		if (j < i) bag[i] = bag[j];
		bag[j] = i;
	}
	// Keep the join between two independently shuffled bags free of repeats.
	if (bag[0] == config_get_int("word", "practice_previous", -1)) {
		uint16_t first = bag[0];
		bag[0] = bag[1];
		bag[1] = first;
	}
	unsigned cursor = (unsigned)config_get_int("word", "practice_cursor", 0) % LV_ARRAYLEN(bag);
	if (cursor + 1 == LV_ARRAYLEN(bag)) {
		config_set_int("word", "practice_previous", bag[cursor]);
		config_set_int("word", "practice_seed", (arcade_rng_next(&seed) & 0x7fffffffu) | 1u);
	}
	config_set_int("word", "practice_cursor", (cursor + 1) % LV_ARRAYLEN(bag));
	config_save();
	return answers[bag[cursor]];
}
static float reveal_ms;
static int shown;

static void calendar_day_ids(unsigned *today_out, unsigned *yesterday_out) {
	time_t now = time(NULL);
	struct tm today;
	localtime_r(&now, &today);
	if (today_out) *today_out = (unsigned)((today.tm_year + 1900) * 1000 + today.tm_yday + 1);
	if (yesterday_out) {
		struct tm prev = today;
		prev.tm_mday -= 1;
		mktime(&prev);
		*yesterday_out = (unsigned)((prev.tm_year + 1900) * 1000 + prev.tm_yday + 1);
	}
}

static long active_daily_streak(unsigned today_d, unsigned yesterday_d) {
	long streak = config_get_int("word", "streak", 0);
	long last_win = config_get_int("word", "last_win_day", 0);
	if (streak <= 0) return 0;
	if (last_win == 0) return streak;
	if ((unsigned)last_win == today_d || (unsigned)last_win == yesterday_d) return streak;
	return 0;
}

static bool daily_done_today(void) {
	unsigned d = 0;
	calendar_day_ids(&d, NULL);
	return config_get_int("word", "completed", 0) == (long)d;
}

/* Both arrays are in ASCII sort order for the on-device binary search. */
static int compare_word(const void *a, const void *b) { return strcmp(a, b); }

static bool valid_word(const char *word) {
	return bsearch(word, guesses, LV_ARRAYLEN(guesses), sizeof(guesses[0]), compare_word) ||
		   bsearch(word, answers, LV_ARRAYLEN(answers), sizeof(answers[0]), compare_word);
}

// Exact matches claim their copy first. Every other answer letter can fund one yellow tile.
static void grade(const char *guess, const char *target, mark_t result[COLS]) {
	int available[26] = {0};
	for (int i = 0; i < COLS; i++) {
		result[i] = guess[i] == target[i] ? EXACT : ABSENT;
		if (result[i] != EXACT) available[target[i] - 'A']++;
	}
	for (int i = 0; i < COLS; i++) {
		int k = guess[i] - 'A';
		if (result[i] == ABSENT && available[k] > 0) {
			result[i] = PRESENT;
			available[k]--;
		}
	}
}

static bool check_hard_constraints(const char *guess, char *err_out, size_t err_sz) {
	if (!hard_mode || row <= 0) return true;

	// 1. Every previously revealed EXACT green letter must stay fixed in its slot.
	char locked[COLS] = {0};
	bool slot_violation = false;
	for (int r = 0; r < row; r++) {
		for (int c = 0; c < COLS; c++) {
			if (marks[r][c] == EXACT) {
				locked[c] = entries[r][c];
				if (guess[c] != entries[r][c]) slot_violation = true;
			}
		}
	}
	if (slot_violation) {
		snprintf(err_out, err_sz, "%c %c %c %c %c", locked[0] ? locked[0] : '_', locked[1] ? locked[1] : '_',
				 locked[2] ? locked[2] : '_', locked[3] ? locked[3] : '_', locked[4] ? locked[4] : '_');
		return false;
	}

	// 2. Every revealed PRESENT/EXACT letter count from prior rows must be included.
	int guess_counts[26] = {0};
	for (int c = 0; c < COLS; c++) {
		if (guess[c] >= 'A' && guess[c] <= 'Z') guess_counts[guess[c] - 'A']++;
	}
	for (int r = 0; r < row; r++) {
		int req[26] = {0};
		for (int c = 0; c < COLS; c++) {
			if (marks[r][c] == EXACT || marks[r][c] == PRESENT) {
				req[entries[r][c] - 'A']++;
			}
		}
		for (int k = 0; k < 26; k++) {
			if (guess_counts[k] < req[k]) {
				if (req[k] > 1) {
					snprintf(err_out, err_sz, "+ %c ×%d", (char)('A' + k), req[k]);
				} else {
					snprintf(err_out, err_sz, "+ %c", (char)('A' + k));
				}
				return false;
			}
		}
	}
	return true;
}

static void style_pill_toggle(lv_obj_t *btn, lv_obj_t *lbl, bool active, bool locked) {
	if (!btn || !lbl) return;
	const theme_palette_t *p = theme();
	if (active) {
		lv_obj_set_style_bg_color(btn, p->accent, 0);
		lv_obj_set_style_bg_opa(btn, locked ? LV_OPA_70 : LV_OPA_COVER, 0);
		lv_obj_set_style_border_color(btn, p->accent, 0);
		lv_obj_set_style_text_color(lbl, p->accent_ink, 0);
	} else {
		lv_obj_set_style_bg_color(btn, p->elevated, 0);
		lv_obj_set_style_bg_opa(btn, locked ? LV_OPA_50 : 220, 0);
		lv_obj_set_style_border_color(btn, p->border, 0);
		lv_obj_set_style_text_color(lbl, p->text_secondary, 0);
	}
}

static void refresh_mode_pills(void) {
	bool done_today = daily_done_today();
	bool use_practice = done_today || prefer_practice;
	if (title_mode_btn && title_mode_lbl) {
		lv_label_set_text(title_mode_lbl, tr(series ? "word_series" : (use_practice ? "word_practice" : "word_daily")));
		style_pill_toggle(title_mode_btn, title_mode_lbl, !use_practice || series, false);
	}
	if (title_hard_btn && title_hard_lbl) {
		lv_label_set_text(title_hard_lbl, hard_mode ? "HARD *" : "HARD");
		style_pill_toggle(title_hard_btn, title_hard_lbl, hard_mode, false);
	}
	if (hud_hard_btn && hud_hard_lbl) {
		lv_label_set_text(hud_hard_lbl, hard_mode ? "HARD *" : "HARD");
		style_pill_toggle(hud_hard_btn, hud_hard_lbl, hard_mode, row > 0);
	}
}

static void paint(lv_obj_t *obj, mark_t mark, bool key) {
	const theme_palette_t *p = theme();
	const uint32_t colors[] = {0, 0x455369, 0x88581a, 0x126748};
	lv_color_t bg = mark ? lv_color_hex(colors[mark]) : key ? p->elevated : p->surface;
	lv_obj_set_style_bg_color(obj, bg, 0);
	lv_obj_set_style_text_color(obj, mark ? lv_color_white() : p->text_primary, 0);
	lv_obj_set_style_border_color(obj, mark ? lv_color_mix(bg, lv_color_white(), 183) : p->border, 0);
}

static void redraw(void) {
	memset(key_marks, 0, sizeof(key_marks));
	for (int r = 0; r < ROWS; r++) {
		for (int c = 0; c < COLS; c++) {
			char value[2] = {entries[r][c], 0};
			lv_label_set_text(letters[r][c], value);
			mark_t m = (r < row || (r == row && revealing && c < shown)) ? marks[r][c] : EMPTY;
			paint(tiles[r][c], m, false);
			if (m && value[0]) {
				int k = value[0] - 'A';
				if (m > key_marks[k]) key_marks[k] = m;
			}
		}
	}
	for (int k = 0; k < 26; k++) paint(keys[k], key_marks[k], true);
	refresh_mode_pills();
}

static void reset_board(void) {
	memset(entries, 0, sizeof(entries));
	memset(marks, 0, sizeof(marks));
	row = fill = shown = 0;
	revealing = false;
	for (int r = 0; r < ROWS; r++) {
		lv_anim_delete(board_rows[r], NULL);
		lv_obj_set_style_translate_x(board_rows[r], 0, 0);
		for (int c = 0; c < COLS; c++) {
			lv_anim_delete(tiles[r][c], NULL);
			lv_obj_set_style_transform_scale_y(tiles[r][c], 256, 0);
		}
	}
}

static void show_attempt(void) {
	unsigned today_d = 0, yesterday_d = 0;
	calendar_day_ids(&today_d, &yesterday_d);
	long streak = active_daily_streak(today_d, yesterday_d);
	char message[96];
	if (daily && streak > 0) {
		snprintf(message, sizeof(message), "%s #%u  ·  %d/%d  ·  ×%ld", tr("word_daily"), today_d % 1000u, row + 1,
				 ROWS, streak);
	} else if (series) {
		snprintf(message, sizeof(message), "%s  %d/3   ·   %d/%d", tr("word_series"), series_round, row+1, ROWS);
	} else if (daily) {
		snprintf(message, sizeof(message), "%s #%u   %d / %d", tr("word_daily"), today_d % 1000u, row + 1, ROWS);
	} else {
		snprintf(message, sizeof(message), "%s   %d / %d", tr("word_practice"), row + 1, ROWS);
	}
	arcade_shell_set_sub_hud(shell, message);
	refresh_mode_pills();
}

static void save_daily(void) {
	char history[ROWS * COLS + 1] = "";
	for (int i = 0; i < row; i++) memcpy(history + i * COLS, entries[i], COLS);
	config_set_int("word", "day", (long)day_id);
	config_set("word", "guesses", history);
}

static void next_series_puzzle(arcade_shell_t *s, void *ctx) {
	(void)ctx;
	series_round++;
	reset_board();
	answer = practice_answer();
	arcade_shell_set_end_continue(s, NULL, NULL);
	arcade_shell_resume_playing(s);
	redraw();
	show_attempt();
}

static void finish_row(void) {
	revealing = false;
	bool won = !strcmp(entries[row], answer);
	row++;
	fill = 0;
	if (daily) save_daily();
	if (won || row == ROWS) {
		if (series) {
			series_guesses += row;
			series_wins += won;
			char summary[128];
			snprintf(summary, sizeof(summary), "%s   ·   %d/3   ·   %s %d", answer, series_wins,
					 tr("word_guesses_used"), series_guesses);
			arcade_shell_set_end_summary(shell, summary);
			arcade_shell_set_end_score_visible(shell, false);
			arcade_shell_set_end_heading(shell, tr("word_series"));
			if (series_round < 3) {
				arcade_shell_set_end_continue(shell, tr("word_next_puzzle"), next_series_puzzle);
			} else if (series_wins == 3) {
				long best = config_get_int("word", "series_fewest", 0);
				if (!best || series_guesses < best) {
					config_set_int("word", "series_fewest", series_guesses);
					config_save();
					best = series_guesses;
				}
				snprintf(summary, sizeof(summary), "%s %d   ·   %s %ld", tr("word_guesses_used"),
						 series_guesses, tr("arcade_best"), best);
				arcade_shell_set_end_summary(shell, summary);
			}
			arcade_shell_trigger_game_over(shell, 340);
			return;
		}
		unsigned today_d = 0, yesterday_d = 0;
		calendar_day_ids(&today_d, &yesterday_d);

		long played = config_get_int("word", "played", 0) + 1;
		long wins = config_get_int("word", "wins", 0) + (won ? 1 : 0);
		config_set_int("word", "played", played);
		config_set_int("word", "wins", wins);

		if (won && row >= 1 && row <= ROWS) {
			char dkey[16];
			snprintf(dkey, sizeof(dkey), "dist_%d", row);
			config_set_int("word", dkey, config_get_int("word", dkey, 0) + 1);
		}

		long streak = active_daily_streak(today_d, yesterday_d);
		long max_streak = config_get_int("word", "max_streak", streak);
		if (daily) {
			config_set_int("word", "completed", (long)day_id);
			config_set("word", "guesses", "");
			if (won) {
				long last_win = config_get_int("word", "last_win_day", 0);
				if ((unsigned)last_win != today_d) {
					streak = streak + 1;
				}
				if (streak > max_streak) max_streak = streak;
				config_set_int("word", "streak", streak);
				config_set_int("word", "max_streak", max_streak);
				config_set_int("word", "last_win_day", (long)today_d);
			} else {
				streak = 0;
				config_set_int("word", "streak", 0);
			}
		}

		if (won) {
			int pts = (ROWS + 1 - row) * 100 + (hard_mode ? 100 : 0);
			arcade_shell_set_score(shell, pts);
		}

		long dist[ROWS];
		for (int i = 0; i < ROWS; i++) {
			char dkey[16];
			snprintf(dkey, sizeof(dkey), "dist_%d", i + 1);
			dist[i] = config_get_int("word", dkey, 0);
		}

		char hist_line[112];
		int pos = 0;
		for (int i = 0; i < ROWS; i++) {
			int n = (won && row == i + 1)
						? snprintf(hist_line + pos, sizeof(hist_line) - (size_t)pos, "%s[%d:%ld]", i ? "  " : "", i + 1,
								   dist[i])
						: snprintf(hist_line + pos, sizeof(hist_line) - (size_t)pos, "%s%d:%ld", i ? "  " : "", i + 1,
								   dist[i]);
			if (n > 0 && pos + n < (int)sizeof(hist_line)) pos += n;
		}

		char summary[220];
		if (max_streak > 0) {
			snprintf(summary, sizeof(summary), "%s  ·  %ld/%ld %s  ·  ×%ld (%ld)\n%s", answer, wins, played,
					 tr("word_wins"), streak, max_streak, hist_line);
		} else {
			snprintf(summary, sizeof(summary), "%s  ·  %ld/%ld %s\n%s", answer, wins, played, tr("word_wins"),
					 hist_line);
		}
		arcade_shell_set_end_heading(shell, tr(won ? "word_solved" : "word_finished"));
		arcade_shell_set_end_summary(shell, summary);
		arcade_shell_trigger_game_over(shell, 340);
		refresh_mode_pills();
	} else {
		show_attempt();
	}
	if (daily || won || row == ROWS) config_save();
}

static bool word_idle(const void *ctx) {
	(void)ctx;
	return !revealing;
}

static void step(arcade_shell_t *s, void *ctx, float dt) {
	(void)ctx;
	if (!revealing || arcade_shell_state(s) != ARCADE_ST_PLAYING) return;
	reveal_ms += dt * 1000;
	for (int c = 0; c < COLS; c++) {
		int t = LV_CLAMP(0, (int)reveal_ms - c * STAGGER_MS, FLIP_MS);
		if (t >= FLIP_MS / 2 && c >= shown) {
			shown = c + 1;
			mark_t m = marks[row][c];
			paint(tiles[row][c], m, false);
			int k = entries[row][c] - 'A';
			if (m > key_marks[k]) {
				key_marks[k] = m;
				paint(keys[k], m, true);
			}
			arcade_sfx_play_pitch(m == EXACT ? ARCADE_SFX_COMBO : (m == PRESENT ? ARCADE_SFX_POP : ARCADE_SFX_TAP), c);
		}
		int scale = 18 + abs(2 * t - FLIP_MS) * 238 / FLIP_MS;
		lv_obj_set_style_transform_scale_y(tiles[row][c], scale, 0);
	}
	if (reveal_ms >= (COLS - 1) * STAGGER_MS + FLIP_MS) finish_row();
}

static void nudge(void *obj, int32_t t) {
	static const int offsets[] = {0, -6, 5, -3, 2, 0};
	lv_obj_set_style_translate_x(obj, offsets[t], 0);
}

static void pop_letter(void *obj, int32_t scale) {
	lv_obj_set_style_transform_scale_y(obj, scale, 0);
}

static void shake_current_row(const char *msg) {
	arcade_sfx_play(ARCADE_SFX_MISS);
	arcade_shell_show_callout(shell, msg, theme()->accent);
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, board_rows[row]);
	lv_anim_set_exec_cb(&a, nudge);
	lv_anim_set_values(&a, 0, 5);
	lv_anim_set_duration(&a, 190);
	lv_anim_start(&a);
}

static void key_click(lv_event_t *e) {
	if (arcade_shell_state(shell) != ARCADE_ST_PLAYING || revealing || row >= ROWS ||
		(row && !strcmp(entries[row - 1], answer))) return;
	int ch = (intptr_t)lv_event_get_user_data(e);
	if (ch == '\n') {
		if (fill < COLS) {
			shake_current_row(tr("word_short"));
			return;
		}
		if (!valid_word(entries[row])) {
			shake_current_row(tr("word_unknown"));
			return;
		}
		char hard_err[48];
		if (!check_hard_constraints(entries[row], hard_err, sizeof(hard_err))) {
			shake_current_row(hard_err);
			return;
		}
		arcade_sfx_play(ARCADE_SFX_SLIDE);
		grade(entries[row], answer, marks[row]);
		reveal_ms = 0;
		shown = 0;
		revealing = true;
		arcade_shell_mark_dirty(shell);
		return;
	}
	if (ch == '\b') {
		if (!fill) return;
		arcade_sfx_play(ARCADE_SFX_TAP);
		fill--;
		entries[row][fill] = 0;
		lv_label_set_text(letters[row][fill], "");
		return;
	}
	if (fill == COLS) return;
	arcade_sfx_play_pitch(ARCADE_SFX_TAP, fill);
	entries[row][fill] = (char)ch;
	char letter[2] = {(char)ch, 0};
	lv_label_set_text(letters[row][fill], letter);
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, tiles[row][fill++]);
	lv_anim_set_exec_cb(&a, pop_letter);
	lv_anim_set_values(&a, 286, 256);
	lv_anim_set_duration(&a, 95);
	lv_anim_start(&a);
}

static void mode_pill_cb(lv_event_t *e) {
	(void)e;
	if (series) {
		series = false;
		prefer_practice = daily_done_today();
	} else if (prefer_practice || daily_done_today()) {
		series = true;
		prefer_practice = true;
	} else prefer_practice = true;
	arcade_sfx_play(ARCADE_SFX_TAP);
	refresh_mode_pills();
}

static void hard_pill_cb(lv_event_t *e) {
	(void)e;
	if (arcade_shell_state(shell) == ARCADE_ST_PLAYING && (row > 0 || revealing)) {
		arcade_sfx_play(ARCADE_SFX_MISS);
		return;
	}
	hard_mode = !hard_mode;
	config_set_int("word", "hard", hard_mode ? 1 : 0);
	config_save();
	arcade_sfx_play(ARCADE_SFX_TAP);
	refresh_mode_pills();
}

static lv_obj_t *make_pill_btn(lv_obj_t *parent, int h, lv_event_cb_t cb, lv_obj_t **lbl_out) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, LV_SIZE_CONTENT, h);
	lv_obj_set_style_radius(btn, h / 2, 0);
	lv_obj_set_style_border_width(btn, 1, 0);
	lv_obj_set_style_pad_hor(btn, 14, 0);
	lv_obj_set_style_pad_ver(btn, 0, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_ext_click_area(btn, 6);
	lv_obj_set_event_bubble(btn, false);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *lbl = lv_label_create(btn);
	lv_obj_set_style_text_font(lbl, &font_ui_14, 0);
	lv_obj_center(lbl);
	*lbl_out = lbl;
	return btn;
}

static void make_key(lv_obj_t *parent, int ch, int x, int y, int w, int h) {
	lv_obj_t *btn = lv_button_create(parent);
	lv_obj_set_size(btn, w, h);
	lv_obj_set_pos(btn, x, y);
	lv_obj_set_style_pad_all(btn, 0, 0);
	lv_obj_set_style_radius(btn, 9, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_bg_color(btn, theme()->surface_pressed, LV_STATE_PRESSED);
	lv_obj_add_event_cb(btn, key_click, LV_EVENT_CLICKED, (void *)(intptr_t)ch);
	lv_obj_t *label = lv_label_create(btn);
	char letter[2] = {(char)ch, 0};
	lv_label_set_text(label, ch == '\n' ? tr("word_enter") : ch == '\b' ? "←" : letter);
	lv_obj_set_style_text_font(label, ch == '\n' ? &font_ui_16 : &font_ui_20_bold, 0);
	lv_obj_set_width(label, w - 2);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_center(label);
	paint(btn, EMPTY, true);
	if (ch >= 'A' && ch <= 'Z') keys[ch - 'A'] = btn;
}

static void build_board(void) {
	hard_mode = config_get_int("word", "hard", 0) != 0;

	lv_obj_t *root = arcade_shell_content(shell);
	int w = arcade_shell_width(shell), h = arcade_shell_height(shell);
	lv_obj_t *sky = lv_image_create(root);
	lv_image_set_src(sky, &scene_lake_image);
	lv_obj_set_style_image_recolor(sky, lv_color_hex(0x19263a), 0);
	lv_obj_set_style_image_recolor_opa(sky, 76, 0);
	lv_obj_set_size(sky, w, h);
	lv_image_set_inner_align(sky, LV_IMAGE_ALIGN_STRETCH);
	lv_obj_set_clickable(sky, false);
	int key_h = LV_CLAMP(43, h / 15, 55);
	int pitch = (w - 12) / 10;
	int keyboard_y = h - 3 * (key_h + 6) - 12;
	int size = LV_MIN(60, (keyboard_y - 175) / ROWS - 6);
	board = lv_obj_create(root);
	lv_obj_remove_style_all(board);
	lv_obj_set_clickable(board, false);
	lv_obj_set_scrollable(board, false);
	lv_obj_set_size(board, COLS * (size + 6) + 10, ROWS * (size + 6));
	lv_obj_align(board, LV_ALIGN_TOP_MID, 0, (keyboard_y - ROWS * (size + 6) + 156) / 2);
	for (int r = 0; r < ROWS; r++) {
		lv_obj_t *line = board_rows[r] = lv_obj_create(board);
		lv_obj_remove_style_all(line);
		lv_obj_set_size(line, lv_pct(100), size);
		lv_obj_set_pos(line, 0, r * (size + 6));
		lv_obj_set_scrollable(line, false);
		for (int c = 0; c < COLS; c++) {
			lv_obj_t *tile = tiles[r][c] = lv_obj_create(line);
			lv_obj_remove_style_all(tile);
			lv_obj_set_size(tile, size, size);
			lv_obj_set_pos(tile, 8 + c * (size + 6), 0);
			lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
			lv_obj_set_style_radius(tile, 10, 0);
			lv_obj_set_style_border_width(tile, 1, 0);
			lv_obj_set_style_transform_pivot_y(tile, size / 2, 0);
			lv_obj_set_scrollable(tile, false);
			letters[r][c] = lv_label_create(tile);
			lv_obj_set_style_text_font(letters[r][c], &font_ui_24_bold, 0);
			lv_obj_center(letters[r][c]);
		}
	}
	const char *rows[] = {"QWERTYUIOP", "ASDFGHJKL", "\nZXCVBNM\b"};
	for (int r = 0; r < 3; r++) {
		int total = (r == 1 ? 9 : 10) * pitch;
		int x = (w - total) / 2;
		for (const char *p = rows[r]; *p; p++) {
			int width = (*p == '\n' || *p == '\b') ? pitch * 3 / 2 : pitch;
			make_key(root, *p, x, keyboard_y + r * (key_h + 6), width - 3, key_h);
			x += width;
		}
	}

	lv_obj_t *extra = arcade_shell_title_extra(shell);
	if (extra) {
		title_mode_btn = make_pill_btn(extra, 32, mode_pill_cb, &title_mode_lbl);
		title_hard_btn = make_pill_btn(extra, 32, hard_pill_cb, &title_hard_lbl);
		arcade_shell_set_title_extra_visible(shell, true);
	}
	lv_obj_t *hud = arcade_shell_hud(shell);
	if (hud) {
		hud_hard_btn = make_pill_btn(hud, 28, hard_pill_cb, &hud_hard_lbl);
		lv_obj_set_pos(hud_hard_btn, 16, 58);
	}
	refresh_mode_pills();
}

static void enter(arcade_shell_t *s, void *ctx) {
	(void)s;
	(void)ctx;
	if (!board) build_board();
	hard_mode = config_get_int("word", "hard", 0) != 0;
	if (daily_done_today()) prefer_practice = true;
	reset_board();
	memcpy(entries[0], "WORDS", COLS);
	row = 1;
	for (int c = 0; c < COLS; c++) marks[0][c] = EXACT;
	redraw();
}

static void start(arcade_shell_t *s, void *ctx) {
	(void)s;
	(void)ctx;
	if (!board) build_board();
	reset_board();
	calendar_day_ids(&day_id, NULL);
	bool can_play_daily = config_get_int("word", "completed", 0) != (long)day_id;
	daily = can_play_daily && !prefer_practice;
	series_round = 1;
	series_wins = series_guesses = 0;
	answer = daily ? answers[(day_id * 2654435761u) % LV_ARRAYLEN(answers)] : practice_answer();
	if (daily && config_get_int("word", "day", 0) == (long)day_id) {
		const char *history = config_get("word", "guesses", "");
		size_t len = strlen(history);
		if (len % COLS == 0 && len < ROWS * COLS) {
			while (row < (int)(len / COLS)) {
				char *guess = entries[row];
				memcpy(guess, history + row * COLS, COLS);
				if (!valid_word(guess) || !strcmp(guess, answer)) {
					memset(guess, 0, COLS + 1);
					break;
				}
				grade(guess, answer, marks[row++]);
			}
		}
	}
	redraw();
	show_attempt();
}

void game_word_hub_status(int *puzzle_num_out, int *streak_out, bool *solved_today_out, int *in_progress_row_out) {
	unsigned d = 0, y = 0;
	calendar_day_ids(&d, &y);
	if (puzzle_num_out) *puzzle_num_out = (int)(d % 1000u);
	if (streak_out) *streak_out = (int)active_daily_streak(d, y);
	bool solved = config_get_int("word", "completed", 0) == (long)d;
	if (solved_today_out) *solved_today_out = solved;
	int prog = 0;
	if (!solved && config_get_int("word", "day", 0) == (long)d) {
		const char *h = config_get("word", "guesses", "");
		size_t len = h ? strlen(h) : 0;
		if (len >= COLS && len < ROWS * COLS) prog = (int)(len / COLS);
	}
	if (in_progress_row_out) *in_progress_row_out = prog;
}

static void refresh_theme(arcade_shell_t *s, void *ctx) {
	(void)s;
	(void)ctx;
	if (board) redraw();
}

static bool word_has_saved(const void *ctx) {
	(void)ctx;
	int prog = 0;
	game_word_hub_status(NULL, NULL, NULL, &prog);
	return prog > 0;
}

static void word_resume_saved(arcade_shell_t *s, void *ctx) {
	prefer_practice = false;
	series = false;
	start(s, ctx);
}

void game_word_open(gui_config_t *cfg) {
	static const arcade_game_ops_t ops = {
		.id = "word", .title_tag = "game_word", .subtitle_tag = "game_word_desc", .hint_tag = "game_word_hint",
		.icon = &icon_atelier_word, .title_in_hud = true, .dark_backdrop = true, .target_fps = 30,
		.on_enter = enter, .on_start = start, .has_saved_run = word_has_saved, .on_resume_saved = word_resume_saved,
		.on_step = step, .is_idle = word_idle, .on_theme_changed = refresh_theme,
	};
	if (!shell) shell = arcade_shell_create(cfg, &ops, NULL);
	arcade_shell_open(shell);
}
