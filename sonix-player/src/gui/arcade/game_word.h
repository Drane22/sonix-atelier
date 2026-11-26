#ifndef GAME_WORD_H
#define GAME_WORD_H

#include <stdbool.h>

#include "src/gui/shell/gui.h"

void game_word_open(gui_config_t *cfg);
void game_word_hub_status(int *puzzle_num_out, int *streak_out, bool *solved_today_out, int *in_progress_row_out);

#endif
