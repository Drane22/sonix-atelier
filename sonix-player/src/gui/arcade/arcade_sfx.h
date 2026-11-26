#ifndef ARCADE_SFX_H
#define ARCADE_SFX_H

#include <stdbool.h>

typedef enum {
	ARCADE_SFX_TAP = 0,
	ARCADE_SFX_SLIDE,
	ARCADE_SFX_POP,
	ARCADE_SFX_BOUNCE,
	ARCADE_SFX_COMBO,
	ARCADE_SFX_POWERUP,
	ARCADE_SFX_SMASH,
	ARCADE_SFX_WIN,
	ARCADE_SFX_MISS,
} arcade_sfx_id_t;

// Enters/leaves an arcade session. With Sound enabled and music already playing,
// Smart Yield leaves the track untouched until arcade_sfx_toggle() claims audio.
void arcade_sfx_enter_game(void);
void arcade_sfx_exit_game(void);

// Cycles/toggles sound:
// - If Sound is ON and currently yielding to active music, pauses music and claims SFX audio.
// - If Sound is ON and active (or idle), mutes Sound (saved in [arcade] sound).
// - If Sound is OFF, enables Sound (pausing music if a track is playing so the tap is heard).
void arcade_sfx_toggle(void);

bool arcade_sfx_enabled(void);
bool arcade_sfx_yielding_to_music(void);

// Triggers a procedural synth sound; `step` (0..10) shifts pitch pentatonically.
void arcade_sfx_play(arcade_sfx_id_t id);
void arcade_sfx_play_pitch(arcade_sfx_id_t id, int step);

#endif // ARCADE_SFX_H
