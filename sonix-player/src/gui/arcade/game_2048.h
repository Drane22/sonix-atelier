#ifndef GAME_2048_H
#define GAME_2048_H

#include <stdbool.h>

#include "src/gui/shell/gui.h"

typedef enum {
	G2048_LEFT = 0,
	G2048_RIGHT = 1,
	G2048_UP = 2,
	G2048_DOWN = 3,
} g2048_dir_t;

void game_2048_open(gui_config_t *cfg);
int game_2048_best(void);

// Pure 2048 board mechanics (exposed for unit tests).
int g2048_slide_board(int board[4][4], g2048_dir_t dir, bool *moved_out, bool *reached_2048_out);
bool g2048_can_move(const int board[4][4]);

#endif // GAME_2048_H
