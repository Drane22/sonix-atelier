#ifndef GAME_PRISM_H
#define GAME_PRISM_H

#include "src/gui/shell/gui.h"

void game_prism_open(gui_config_t *cfg);
int game_prism_best(void);

// Pure geometry & color-matching helpers (exposed for unit tests).
int prism_segment_color_at_angle(float angle_rad, float ring_rot_rad);
int prism_check_ring_collision(float bx, float by, float br, int ball_color, float cx, float cy, float r_in,
							   float r_out, float rot_rad);

#endif // GAME_PRISM_H
