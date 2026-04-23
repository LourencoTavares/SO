#ifndef PARSER_H
#define PARSER_H

#include "board.h"

int parse_level_file(const char *path, board_t *board, int *default_pac_x, int *default_pac_y);

int parse_behavior_file(const char *path, int *passo, int *row, int *col, command_t *moves, int *n_moves);

#endif
