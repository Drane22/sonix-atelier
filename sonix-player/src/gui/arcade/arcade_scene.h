#ifndef ARCADE_SCENE_H
#define ARCADE_SCENE_H

#include "arcade_shell.h"

extern const uint16_t scene_cliffs[], scene_reef[], scene_observatory[], scene_crystal[];
extern const uint16_t scene_enamel[];
extern const lv_image_dsc_t scene_tabletop_image, scene_lake_image;
void scene_backdrop(arcade_fb_t *fb, const uint16_t *art, int first_row, int last_row);
void scene_sphere(arcade_fb_t *fb, int cx, int cy, int radius, uint16_t color);

// Logical pixels on the compact 240 x 400 scenery layer.
void scene_block(arcade_fb_t *fb, int x, int y, int w, int h, int depth, uint16_t color);
void scene_orb(arcade_fb_t *fb, int x, int y, int radius, uint16_t color);
void scene_stars(arcade_fb_t *fb, int height);
void game_towerblast_open(gui_config_t *cfg);
void game_blockmosaic_open(gui_config_t *cfg);
void game_breakwater_open(gui_config_t *cfg);

#endif
