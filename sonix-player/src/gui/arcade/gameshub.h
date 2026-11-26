#ifndef GUI_GAMESHUB_H
#define GUI_GAMESHUB_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Games Hub page: hosts preserved classics (Flappy Bird and Game Boy
// emulator) and upcoming Sonix Atelier arcade titles under one dedicated roof.

extern lv_obj_t *games_screen;

void gameshub_init(gui_config_t *cfg);

#endif // GUI_GAMESHUB_H
