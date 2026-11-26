#ifndef GAME_ORBIT_H
#define GAME_ORBIT_H

#include <stdbool.h>

#include "src/gui/shell/gui.h"

void game_orbit_open(gui_config_t *cfg);
int game_orbit_best(void);

// Pure geometry & rule helpers (exposed for unit tests).
float orbit_wrap_angle(float a);
float orbit_angle_diff(float a, float b);
int orbit_next_ring(int cur_ring, int *dir_inout);
bool orbit_check_arc_hit(int orb_ring, float orb_angle, int obs_ring, float obs_center_ang, float obs_half_arc);

#endif // GAME_ORBIT_H
