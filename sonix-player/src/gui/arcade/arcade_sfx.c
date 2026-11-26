#include "arcade_sfx.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "src/system/audio/audio.h"
#include "src/system/core/config.h"
#include "src/system/streaming/radio.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SFX_RATE 44100
#define SFX_CHUNK 441 // 10 ms per write
#define SFX_BUFFER_MS 46
#define MAX_VOICES 6
#define WAVE_LEN 256
#define WAVE_MASK (WAVE_LEN - 1)

typedef enum {
	WAVE_SINE = 0,
	WAVE_BELL,	  // fundamental + 2x + 3x soft overtones
	WAVE_TRI,	  // warm woody triangle
	WAVE_SPARKLE, // filtered noise + high harmonic
} wave_kind_t;

typedef struct {
	bool active;
	wave_kind_t wave;
	uint32_t delay_samples;
	uint32_t age_samples;
	uint32_t total_samples;
	uint32_t attack_samples;
	uint32_t phase;
	uint32_t phase_inc;
	int32_t glide_inc_per_chunk;
	int32_t gain_q12; // 0..4096
} voice_t;

static int16_t lut_sine[WAVE_LEN];
static int16_t lut_bell[WAVE_LEN];
static int16_t lut_tri[WAVE_LEN];
static bool lut_built = false;

static voice_t voices[MAX_VOICES];
static uint32_t noise_lfsr = 0xACE1u;

static pthread_mutex_t sfx_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t mixer_thread;
static bool thread_live = false;
static pthread_cond_t sfx_wake = PTHREAD_COND_INITIALIZER;
static bool stop_flag = false;

static bool in_game = false;
static bool pref_loaded = false;
static bool sound_pref = true;
static atomic_int yielding_music = false;

// Pentatonic pitch table (Q10 fixed-point multipliers, 1024 = 1.0x)
static const uint16_t PENTA_Q10[12] = {
	1024, 1152, 1280, 1536, 1707, 2048, 2304, 2560, 3072, 3413, 4096, 4608,
};

static void ensure_luts(void) {
	if (lut_built) {
		return;
	}
	for (int i = 0; i < WAVE_LEN; i++) {
		double a = (2.0 * M_PI * (double)i) / (double)WAVE_LEN;
		double s1 = sin(a);
		double s2 = sin(2.0 * a);
		double s3 = sin(3.0 * a);
		lut_sine[i] = (int16_t)lrint(s1 * 32000.0);
		lut_bell[i] = (int16_t)lrint(((s1 + 0.34 * s2 + 0.14 * s3) / 1.48) * 32000.0);
		double t = (double)i / (double)WAVE_LEN;
		double tri = t < 0.25 ? (4.0 * t) : (t < 0.75 ? (2.0 - 4.0 * t) : (4.0 * t - 4.0));
		lut_tri[i] = (int16_t)lrint(tri * 32000.0);
	}
	lut_built = true;
}

static void ensure_pref(void) {
	if (!pref_loaded) {
		sound_pref = config_get_bool("arcade", "sound", true);
		pref_loaded = true;
	}
}

static bool background_music_playing(void) {
	return audio_get_status() == AUDIO_STATUS_PLAYING || radio_is_playing() || radio_is_connecting();
}

static uint32_t hz_to_inc(float hz) {
	if (hz < 30.0f) hz = 30.0f;
	if (hz > 9000.0f) hz = 9000.0f;
	double ratio = (double)hz / (double)SFX_RATE;
	return (uint32_t)(ratio * 4294967296.0);
}

static void add_voice_locked(wave_kind_t wave, float hz_start, float hz_end, int delay_ms, int dur_ms, int gain_q12) {
	if (dur_ms <= 2) {
		return;
	}
	int slot = -1;
	uint32_t best_rem = UINT32_MAX;
	for (int i = 0; i < MAX_VOICES; i++) {
		if (!voices[i].active) {
			slot = i;
			break;
		}
		uint32_t rem = voices[i].total_samples > voices[i].age_samples
						   ? (voices[i].total_samples - voices[i].age_samples)
						   : 0;
		if (rem < best_rem) {
			best_rem = rem;
			slot = i;
		}
	}
	if (slot < 0) {
		return;
	}

	uint32_t total = (uint32_t)((SFX_RATE * dur_ms) / 1000);
	uint32_t delay = (uint32_t)((SFX_RATE * (delay_ms > 0 ? delay_ms : 0)) / 1000);
	uint32_t attack = (uint32_t)(SFX_RATE * 2 / 1000); // 2 ms clickless attack
	if (attack > total / 3) {
		attack = total / 3;
	}
	uint32_t inc0 = hz_to_inc(hz_start);
	uint32_t inc1 = hz_to_inc(hz_end);
	int32_t chunks = (int32_t)(total / SFX_CHUNK);
	if (chunks < 1) chunks = 1;

	voices[slot] = (voice_t){
		.active = true,
		.wave = wave,
		.delay_samples = delay,
		.age_samples = 0,
		.total_samples = total,
		.attack_samples = attack > 0 ? attack : 1,
		.phase = 0,
		.phase_inc = inc0,
		.glide_inc_per_chunk = ((int32_t)inc1 - (int32_t)inc0) / chunks,
		.gain_q12 = gain_q12,
	};
}

static void render_chunk_locked(int16_t *stereo_out) {
	memset(stereo_out, 0, sizeof(int16_t) * SFX_CHUNK * 2);

	for (int v = 0; v < MAX_VOICES; v++) {
		voice_t *vc = &voices[v];
		if (!vc->active) {
			continue;
		}
		bool stepped_chunk = false;
		for (int f = 0; f < SFX_CHUNK; f++) {
			if (vc->delay_samples > 0) {
				vc->delay_samples--;
				continue;
			}
			if (vc->age_samples >= vc->total_samples) {
				vc->active = false;
				break;
			}
			stepped_chunk = true;

			uint32_t idx = (vc->phase >> 24) & WAVE_MASK;
			int32_t sample = 0;
			switch (vc->wave) {
			case WAVE_SINE:
				sample = lut_sine[idx];
				break;
			case WAVE_BELL:
				sample = lut_bell[idx];
				break;
			case WAVE_TRI:
				sample = lut_tri[idx];
				break;
			case WAVE_SPARKLE: {
				uint32_t bit = ((noise_lfsr >> 0) ^ (noise_lfsr >> 2) ^ (noise_lfsr >> 3) ^ (noise_lfsr >> 5)) & 1u;
				noise_lfsr = (noise_lfsr >> 1) | (bit << 15);
				int16_t nz = (int16_t)((int32_t)(noise_lfsr & 0x7FFFu) - 16384);
				sample = (lut_bell[idx] * 5 + nz * 3) >> 3;
				break;
			}
			}
			vc->phase += vc->phase_inc;

			// Smooth attack + quadratic decay envelope
			uint32_t env_q10;
			if (vc->age_samples < vc->attack_samples) {
				env_q10 = (vc->age_samples * 1024u) / vc->attack_samples;
			} else {
				uint32_t rem = vc->total_samples - vc->age_samples;
				uint32_t body = vc->total_samples - vc->attack_samples;
				uint32_t lin = body > 0 ? (rem * 1024u) / body : 0;
				env_q10 = (lin * lin) >> 10;
			}
			vc->age_samples++;

			int32_t scaled = (sample * (int32_t)env_q10) >> 10;
			scaled = (scaled * vc->gain_q12) >> 12;

			int32_t l = (int32_t)stereo_out[f * 2] + scaled;
			int32_t r = (int32_t)stereo_out[f * 2 + 1] + scaled;
			stereo_out[f * 2] = (int16_t)(l > 32767 ? 32767 : (l < -32768 ? -32768 : l));
			stereo_out[f * 2 + 1] = (int16_t)(r > 32767 ? 32767 : (r < -32768 ? -32768 : r));
		}
		if (stepped_chunk && vc->active) {
			int64_t next_inc = (int64_t)vc->phase_inc + (int64_t)vc->glide_inc_per_chunk;
			if (next_inc < 100000) next_inc = 100000;
			if (next_inc > 900000000) next_inc = 900000000;
			vc->phase_inc = (uint32_t)next_inc;
		}
	}
}

static bool voices_active(void) {
	for (int i = 0; i < MAX_VOICES; i++) {
		if (voices[i].active) return true;
	}
	return false;
}

static void *sfx_mixer_main(void *unused) {
	(void)unused;
	int16_t buf[SFX_CHUNK * 2];
	bool open = false;
	int tail = 0;
	pthread_mutex_lock(&sfx_lock);
	while (!stop_flag) {
		bool active = voices_active();
		if (open && audio_external_wanted()) {
			yielding_music = true;
			break;
		}
		if (!active && tail == 0) {
			if (open) {
				pthread_mutex_unlock(&sfx_lock);
				audio_external_end();
				open = false;
				pthread_mutex_lock(&sfx_lock);
				continue;
			}
			pthread_cond_wait(&sfx_wake, &sfx_lock);
			continue;
		}
		if (active) {
			// Let the last samples reach the DAC before closing with snd_pcm_drop.
			tail = (SFX_BUFFER_MS + 9) / 10 + 1;
		} else {
			tail--;
		}
		render_chunk_locked(buf);
		pthread_mutex_unlock(&sfx_lock);
		if (!open && !background_music_playing()) {
			open = audio_external_begin_yielding(SFX_RATE, 2, 16, SFX_BUFFER_MS);
		}
		if (!open || audio_external_write(buf, SFX_CHUNK) != SFX_CHUNK) {
			if (open) audio_external_end();
			open = false;
			tail = 0;
			// Discard this effect. A later effect can retry after the route settles.
			pthread_mutex_lock(&sfx_lock);
			memset(voices, 0, sizeof(voices));
			pthread_mutex_unlock(&sfx_lock);
			usleep(250000);
		}
		pthread_mutex_lock(&sfx_lock);
	}
	pthread_mutex_unlock(&sfx_lock);
	if (open) audio_external_end();
	return NULL;
}

static void stop_mixer(void) {
	if (thread_live) {
		pthread_mutex_lock(&sfx_lock);
		stop_flag = true;
		pthread_cond_signal(&sfx_wake);
		pthread_mutex_unlock(&sfx_lock);
		pthread_join(mixer_thread, NULL);
		thread_live = false;
	}
	pthread_mutex_lock(&sfx_lock);
	memset(voices, 0, sizeof(voices));
	pthread_mutex_unlock(&sfx_lock);
}

static void start_mixer(void) {
	stop_mixer();
	ensure_luts();
	stop_flag = false;
	yielding_music = false;
	if (pthread_create(&mixer_thread, NULL, sfx_mixer_main, NULL) == 0) {
		thread_live = true;
	}
}

void arcade_sfx_enter_game(void) {
	ensure_pref();
	ensure_luts();
	in_game = true;
	if (!sound_pref) {
		yielding_music = false;
		stop_mixer();
		return;
	}
	if (background_music_playing()) {
		// Smart Yield: keep the active song playing undisturbed.
		yielding_music = true;
		stop_mixer();
		return;
	}
	start_mixer();
}

void arcade_sfx_exit_game(void) {
	in_game = false;
	yielding_music = false;
	stop_mixer();
}

bool arcade_sfx_enabled(void) {
	ensure_pref();
	return sound_pref;
}

bool arcade_sfx_yielding_to_music(void) {
	ensure_pref();
	if (!sound_pref) {
		return false;
	}
	return yielding_music || background_music_playing();
}

void arcade_sfx_toggle(void) {
	ensure_pref();
	if (sound_pref && arcade_sfx_yielding_to_music()) {
		// Explicit tap while yielding: pause background music and hand audio to game SFX.
		audio_pause();
		if (radio_is_playing() || radio_is_connecting()) {
			radio_stop();
		}
		yielding_music = false;
		if (in_game) {
			start_mixer();
			arcade_sfx_play(ARCADE_SFX_POP);
		}
		return;
	}

	sound_pref = !sound_pref;
	config_set_bool("arcade", "sound", sound_pref);
	config_save();

	if (!sound_pref) {
		yielding_music = false;
		stop_mixer();
	} else if (in_game) {
		if (background_music_playing()) {
			audio_pause();
			if (radio_is_playing() || radio_is_connecting()) {
				radio_stop();
			}
		}
		start_mixer();
		arcade_sfx_play(ARCADE_SFX_POP);
	}
}

void arcade_sfx_play(arcade_sfx_id_t id) { arcade_sfx_play_pitch(id, 0); }

void arcade_sfx_play_pitch(arcade_sfx_id_t id, int step) {
	if (!sound_pref || !thread_live || arcade_sfx_yielding_to_music()) {
		return;
	}
	if (step < 0) step = 0;
	if (step > 11) step = 11;
	float mul = (float)PENTA_Q10[step] / 1024.0f;

	pthread_mutex_lock(&sfx_lock);
	switch (id) {
	case ARCADE_SFX_TAP:
		add_voice_locked(WAVE_TRI, 520.0f * mul, 340.0f * mul, 0, 22, 1150);
		break;
	case ARCADE_SFX_SLIDE:
		add_voice_locked(WAVE_SINE, 440.0f * mul, 587.0f * mul, 0, 32, 1050);
		break;
	case ARCADE_SFX_POP:
		add_voice_locked(WAVE_BELL, 660.0f * mul, 880.0f * mul, 0, 62, 1450);
		add_voice_locked(WAVE_SINE, 1320.0f * mul, 1320.0f * mul, 8, 50, 650);
		break;
	case ARCADE_SFX_BOUNCE:
		add_voice_locked(WAVE_TRI, 310.0f * mul, 520.0f * mul, 0, 48, 1400);
		break;
	case ARCADE_SFX_COMBO:
		add_voice_locked(WAVE_BELL, 587.3f * mul, 587.3f * mul, 0, 75, 1450);
		add_voice_locked(WAVE_BELL, 880.0f * mul, 880.0f * mul, 28, 95, 1350);
		break;
	case ARCADE_SFX_POWERUP:
		add_voice_locked(WAVE_BELL, 523.3f, 523.3f, 0, 95, 1350);
		add_voice_locked(WAVE_BELL, 659.3f, 659.3f, 36, 95, 1350);
		add_voice_locked(WAVE_BELL, 784.0f, 1046.5f, 72, 125, 1500);
		break;
	case ARCADE_SFX_SMASH:
		add_voice_locked(WAVE_TRI, 210.0f, 68.0f, 0, 135, 2000);
		add_voice_locked(WAVE_SPARKLE, 1240.0f * mul, 620.0f, 0, 120, 1400);
		break;
	case ARCADE_SFX_WIN:
		add_voice_locked(WAVE_BELL, 523.3f, 523.3f, 0, 120, 1350);
		add_voice_locked(WAVE_BELL, 659.3f, 659.3f, 55, 120, 1350);
		add_voice_locked(WAVE_BELL, 784.0f, 784.0f, 110, 130, 1400);
		add_voice_locked(WAVE_BELL, 1174.7f, 1174.7f, 165, 220, 1550);
		break;
	case ARCADE_SFX_MISS:
		add_voice_locked(WAVE_TRI, 330.0f, 247.0f, 0, 95, 1450);
		add_voice_locked(WAVE_TRI, 233.1f, 155.6f, 65, 140, 1500);
		break;
	}
	pthread_cond_signal(&sfx_wake);
	pthread_mutex_unlock(&sfx_lock);
}
