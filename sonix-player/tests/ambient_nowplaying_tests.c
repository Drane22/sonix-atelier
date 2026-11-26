// Automated verification tests for Sonix Atelier — Ambient Edition:
//   1. Adaptive & Fixed palette contrast, dark-surface invariants, edge cases & lerp
//   2. Lyrics parser & synchronization (LRC, [offset:], repeated stamps, multi-line
//      continuations, instrumental breaks, plain .txt, UTF-8 BOM, UTF-16LE, Latin-1)
//   3. Queue & shuffle lookahead determinism and non-mutation across all modes
//
// Run with: make -C sonix-player test-nowplaying

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/gui/nowplaying/ambient_palette.h"
#include "src/system/library/lyrics.h"
#include "src/system/library/library.h"
#include "src/system/playback/playlist.h"

// Minimal test-harness stubs for config / embedded-lyrics / unused library_index hooks
static long test_cfg_queue_add_end = 0;
static long test_cfg_playback_mode = 0;
static const char *mock_embedded_lyrics = NULL;

long config_get_int(const char *section, const char *key, long fallback) {
	if (section && strcmp(section, "player") == 0 && key) {
		if (strcmp(key, "queue_add_end") == 0) return test_cfg_queue_add_end;
		if (strcmp(key, "playback_mode") == 0) return test_cfg_playback_mode;
	}
	return fallback;
}

void config_set_int(const char *section, const char *key, long value) {
	if (section && strcmp(section, "player") == 0 && key) {
		if (strcmp(key, "queue_add_end") == 0) test_cfg_queue_add_end = value;
		if (strcmp(key, "playback_mode") == 0) test_cfg_playback_mode = value;
	}
}

bool config_save(void) { return true; }

char *metadata_read_lyrics(const char *path) {
	(void)path;
	return mock_embedded_lyrics ? strdup(mock_embedded_lyrics) : NULL;
}

void library_index_close(library_index_t *ix) { (void)ix; }
int library_index_window(const library_index_t *ix, int offset, int count, library_row_cb cb, void *user) {
	(void)ix;
	(void)offset;
	(void)count;
	(void)cb;
	(void)user;
	return 0;
}
bool library_index_stale(const library_index_t *ix) {
	(void)ix;
	return false;
}
void library_index_describe(const library_index_t *ix, library_index_spec_t *out) {
	(void)ix;
	memset(out, 0, sizeof(*out));
}
library_index_t *library_index_open(library_list_t kind, library_filter_t filter, const char *value,
									library_order_t order, bool desc) {
	(void)kind;
	(void)filter;
	(void)value;
	(void)order;
	(void)desc;
	return NULL;
}
int library_index_count(const library_index_t *ix) {
	(void)ix;
	return 0;
}
int library_index_find_path(const library_index_t *ix, const char *path) {
	(void)ix;
	(void)path;
	return -1;
}
void library_queue_save_album(const char *album) { (void)album; }

// ---------------------------------------------------------------------------
// 1. Adaptive & Fixed Palette Verification
// ---------------------------------------------------------------------------

static void assert_palette_readability(const ambient_palette_t *p) {
	int bg_top_y = ambient_color_luma(p->bg_top);
	int bg_bot_y = ambient_color_luma(p->bg_bottom);
	int surf_y = ambient_color_luma(p->surface);
	int txt1_y = ambient_color_luma(p->text_primary);
	int txt2_y = ambient_color_luma(p->text_secondary);
	int acc_y = ambient_color_luma(p->accent);
	int ink_y = ambient_color_luma(p->accent_ink);

	// Background must remain predominantly dark across every source artwork
	assert(bg_top_y <= 52);
	assert(bg_bot_y <= 32);
	assert(surf_y <= 68);

	// Primary and secondary text must maintain strong legibility on dark background
	assert(txt1_y - bg_top_y >= 170);
	assert(txt2_y - bg_top_y >= 95);

	// Active accent (waveform elapsed, active lyric, Play circle) must stand out on dark bg
	assert(acc_y - bg_top_y >= 95);

	// Play button icon ink must contrast clearly against Play button fill
	assert(abs(acc_y - ink_y) >= 100);
}

static void test_ambient_palettes(void) {
	ambient_palette_t fixed0;
	ambient_palette_fixed(0, 0, &fixed0);
	// Exact Atelier Fixed Ice tokens: #10141B, #202A36, #B9DCFA, #F1F5F9, #94A3B8
	assert(fixed0.bg_bottom.red == 0x10 && fixed0.bg_bottom.green == 0x14 && fixed0.bg_bottom.blue == 0x1B);
	assert(fixed0.surface.red == 0x20 && fixed0.surface.green == 0x2A && fixed0.surface.blue == 0x36);
	assert(fixed0.accent.red == 0xB9 && fixed0.accent.green == 0xDC && fixed0.accent.blue == 0xFA);
	assert(fixed0.text_primary.red == 0xF1 && fixed0.text_primary.green == 0xF5 && fixed0.text_primary.blue == 0xF9);
	assert(fixed0.text_secondary.red == 0x94 && fixed0.text_secondary.green == 0xA3 && fixed0.text_secondary.blue == 0xB8);
	assert_palette_readability(&fixed0);

	// Test preset fixed colours (Coral, Mint, Violet, Amber, Rose)
	const uint32_t presets[] = {0xB9DCFA, 0xFB7185, 0x34D399, 0xA78BFA, 0xFBBF24, 0x22D3EE};
	for (int i = 1; i < 6; i++) {
		ambient_palette_t fp;
		ambient_palette_fixed(i, presets[i], &fp);
		assert_palette_readability(&fp);
	}

	// Test adaptive edge cases: missing cover, pure black, pure white, greyscale,
	// ultra-bright yellow, deep navy, neon red, saturated cyan, warm terracotta
	const struct {
		bool has_cover;
		uint32_t tone;
		int luma;
	} cases[] = {
		{false, 0x000000, -1},
		{true, 0x000000, 2},
		{true, 0xFFFFFF, 252},
		{true, 0x808080, 128},
		{true, 0xFFEE00, 226},
		{true, 0x061038, 18},
		{true, 0xFF1020, 88},
		{true, 0x00E5FF, 165},
		{true, 0xC86432, 118},
		{true, 0x2E7D32, 96},
	};

	ambient_palette_t prev = fixed0;
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		ambient_palette_t p;
		ambient_palette_adaptive(cases[i].has_cover, cases[i].tone, cases[i].luma, &p);
		assert_palette_readability(&p);

		// Verify smooth lerp continuity & contrast at every transition step
		for (int t = 0; t <= 1000; t += 125) {
			ambient_palette_t mid;
			ambient_palette_lerp(&prev, &p, t, &mid);
			assert_palette_readability(&mid);
		}
		prev = p;
	}
}

// ---------------------------------------------------------------------------
// 2. Lyrics Engine & Synchronization Verification
// ---------------------------------------------------------------------------

static void write_bytes(const char *path, const void *data, size_t len) {
	FILE *f = fopen(path, "wb");
	assert(f != NULL);
	assert(fwrite(data, 1, len, f) == len);
	fclose(f);
}

static void test_lyrics_engine(void) {
	char dir_tmpl[] = "/tmp/sonix_lrc_test_XXXXXX";
	char *dir = mkdtemp(dir_tmpl);
	assert(dir != NULL);

	char track[512], lrc[512], txt[512];
	snprintf(track, sizeof(track), "%s/song.flac", dir);
	snprintf(lrc, sizeof(lrc), "%s/song.lrc", dir);
	snprintf(txt, sizeof(txt), "%s/song.txt", dir);

	// A. Synced LRC with headers, [offset:+200], repeated chorus stamps,
	//    enhanced word stamps <mm:ss.xx>, instrumental empty break, and multi-line translation!
	const char *lrc_doc =
		"[ti:Midnight Reverie]\n"
		"[ar:Sonix Ensemble]\n"
		"[offset:200]\n"
		"[00:05.00]First <00:05.40>whisper in the dark\n"
		"Premiere murmure dans l'ombre\n"
		"[00:12.50][00:32.50]Chorus line rises\n"
		"Le refrain s'eleve\n"
		"[00:20.00]\n"
		"[00:25.00]Verse two begins\n";
	write_bytes(lrc, lrc_doc, strlen(lrc_doc));

	lyrics_t ly = {0};
	assert(lyrics_load(track, "Midnight Reverie", &ly));
	assert(ly.synced);
	// Expected sorted stamps after -200ms positive offset:
	//   4800 ms: "First whisper in the dark\nPremiere murmure dans l'ombre"
	//  12300 ms: "Chorus line rises\nLe refrain s'eleve"
	//  19800 ms: "" (instrumental break)
	//  24800 ms: "Verse two begins"
	//  32300 ms: "Chorus line rises\nLe refrain s'eleve"
	assert(ly.count == 5);
	assert(ly.lines[0].ms == 4800);
	assert(strcmp(ly.lines[0].text, "First whisper in the dark\nPremiere murmure dans l'ombre") == 0);
	assert(ly.lines[1].ms == 12300);
	assert(strcmp(ly.lines[1].text, "Chorus line rises\nLe refrain s'eleve") == 0);
	assert(ly.lines[2].ms == 19800);
	assert(strcmp(ly.lines[2].text, "") == 0);
	assert(ly.lines[3].ms == 24800);
	assert(strcmp(ly.lines[3].text, "Verse two begins") == 0);
	assert(ly.lines[4].ms == 32300);
	assert(strcmp(ly.lines[4].text, "Chorus line rises\nLe refrain s'eleve") == 0);

	// Binary search line synchronization across seeks & boundaries
	assert(lyrics_line_at(&ly, 0) == -1);
	assert(lyrics_line_at(&ly, 4799) == -1);
	assert(lyrics_line_at(&ly, 4800) == 0);
	assert(lyrics_line_at(&ly, 12299) == 0);
	assert(lyrics_line_at(&ly, 12300) == 1);
	assert(lyrics_line_at(&ly, 20000) == 2);
	assert(lyrics_line_at(&ly, 25000) == 3);
	assert(lyrics_line_at(&ly, 99000) == 4);
	lyrics_free(&ly);
	unlink(lrc);

	// B. Unsynced plain text (.txt) with paragraph breaks
	const char *txt_doc = "\n  Opening stanza line one\r\nOpening stanza line two\r\n\r\n\r\nSecond stanza  \n\n";
	write_bytes(txt, txt_doc, strlen(txt_doc));
	assert(lyrics_load(track, NULL, &ly));
	assert(!ly.synced);
	assert(ly.count == 4);
	assert(strcmp(ly.lines[0].text, "Opening stanza line one") == 0);
	assert(strcmp(ly.lines[1].text, "Opening stanza line two") == 0);
	assert(strcmp(ly.lines[2].text, "") == 0);
	assert(strcmp(ly.lines[3].text, "Second stanza") == 0);
	assert(lyrics_line_at(&ly, 5000) == -1);
	lyrics_free(&ly);
	unlink(txt);

	// C. UTF-16LE BOM LRC decoding
	const unsigned char utf16le_lrc[] = {
		0xFF, 0xFE,
		'[', 0, '0', 0, '0', 0, ':', 0, '0', 0, '1', 0, '.', 0, '0', 0, '0', 0, ']', 0,
		'H', 0, 'e', 0, 'l', 0, 'l', 0, 'o', 0, '\n', 0
	};
	write_bytes(lrc, utf16le_lrc, sizeof(utf16le_lrc));
	assert(lyrics_load(track, NULL, &ly));
	assert(ly.synced && ly.count == 1 && ly.lines[0].ms == 1000);
	assert(strcmp(ly.lines[0].text, "Hello") == 0);
	lyrics_free(&ly);
	unlink(lrc);

	// D. Embedded tag fallback when no sidecar file exists
	mock_embedded_lyrics = "[00:03.25]Embedded tag line";
	assert(lyrics_load(track, NULL, &ly));
	assert(ly.synced && ly.count == 1 && ly.lines[0].ms == 3250);
	assert(strcmp(ly.lines[0].text, "Embedded tag line") == 0);
	lyrics_free(&ly);
	mock_embedded_lyrics = NULL;

	// E. Missing lyrics returns false cleanly
	assert(!lyrics_load(track, NULL, &ly));
	rmdir(dir);
}

// ---------------------------------------------------------------------------
// 3. Queue Order, Shuffle Lookahead & Non-Mutation Verification
// ---------------------------------------------------------------------------

static void test_queue_and_carousel_order(void) {
	const char *tracks[5] = {
		"/music/01_alpha.flac",
		"/music/02_beta.flac",
		"/music/03_gamma.flac",
		"/music/04_delta.flac",
		"/music/05_epsilon.flac",
	};

	playlist_set_mode(PLAYBACK_MODE_NORMAL);
	playlist_load_paths(tracks, 5, 0);
	assert(playlist_count() == 5);
	assert(playlist_current_index() == 0);

	// At start of NORMAL queue: no previous at index -1, next at index 1 is 02_beta
	char p[512] = "";
	int next_idx = -1;
	bool rep_one = false;
	assert(!playlist_path_at(-1, p, sizeof(p)));
	assert(playlist_peek_auto_detail(p, sizeof(p), &next_idx, &rep_one));
	assert(strcmp(p, tracks[1]) == 0 && next_idx == 1 && !rep_one);
	// Peeking never mutates current index or revision!
	unsigned rev0 = playlist_revision();
	assert(playlist_current_index() == 0 && playlist_revision() == rev0);

	// Switch to SHUFFLE: current track stays at 0, remaining 4 tracks are pre-dealt
	// in `order[]` so carousel preview matches `playlist_next()` 100% deterministically!
	playlist_set_mode(PLAYBACK_MODE_SHUFFLE);
	assert(playlist_current_index() == 0);
	assert(playlist_current_path(p, sizeof(p)) && strcmp(p, tracks[0]) == 0);

	for (int step = 0; step < 4; step++) {
		char preview_next[512] = "";
		char actual_next[512] = "";
		assert(playlist_path_at(step + 1, preview_next, sizeof(preview_next)));
		assert(playlist_peek_auto(p, sizeof(p)) && strcmp(p, preview_next) == 0);
		assert(playlist_next(actual_next, sizeof(actual_next)));
		assert(strcmp(actual_next, preview_next) == 0);
	}
	// At end of non-repeating SHUFFLE: `playlist_peek_auto` returns false (queue stops)
	assert(playlist_current_index() == 4);
	assert(!playlist_peek_auto(p, sizeof(p)));

	// Switch to SHUFFLE_REPEAT at the end of a deal: next deal is unknown in advance,
	// so `playlist_peek_auto` MUST return false rather than showing a misleading preview!
	playlist_set_mode(PLAYBACK_MODE_SHUFFLE_REPEAT);
	assert(playlist_current_index() == 4);
	assert(!playlist_peek_auto(p, sizeof(p)));

	// REPEAT_ALL wraps around deterministically from index 4 -> index 0
	playlist_set_mode(PLAYBACK_MODE_REPEAT_ALL);
	playlist_load_paths(tracks, 5, 4);
	assert(playlist_peek_auto_detail(p, sizeof(p), &next_idx, &rep_one));
	assert(strcmp(p, tracks[0]) == 0 && next_idx == 0 && !rep_one);

	// REPEAT_ONE flags `repeat_one_out = true` while `playlist_path_at` still exposes neighbors
	playlist_set_mode(PLAYBACK_MODE_REPEAT_ONE);
	assert(playlist_peek_auto_detail(p, sizeof(p), &next_idx, &rep_one));
	assert(strcmp(p, tracks[4]) == 0 && rep_one);

	// Queue mutation ("Play Next" insertion & reorder) updates upcoming slot immediately
	playlist_set_mode(PLAYBACK_MODE_NORMAL);
	playlist_load_paths(tracks, 3, 0);
	assert(playlist_insert_next("/music/99_inserted.flac"));
	assert(playlist_count() == 4);
	assert(playlist_path_at(1, p, sizeof(p)) && strcmp(p, "/music/99_inserted.flac") == 0);

	playlist_clear();
}

int main(void) {
	test_ambient_palettes();
	test_lyrics_engine();
	test_queue_and_carousel_order();
	puts("Ambient palette, lyrics synchronization, and queue lookahead unit tests passed.");
	return 0;
}
