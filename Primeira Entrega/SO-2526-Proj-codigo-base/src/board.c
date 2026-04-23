#include "board.h"
#include "parser.h"

#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>

FILE * debugfile;

static char **g_level_files = NULL;
static int  g_num_levels = 0;
static int  g_current_level = 0;
static char g_base_dir[MAX_FILENAME];


/**
 * @brief Procura se existe um Pacman numa coordenada específica e marca-o como morto.
 * Usado quando um fantasma se move para uma casa onde o Pacman está.
 */
static int find_and_kill_pacman(board_t* board, int new_x, int new_y) {
    for (int p = 0; p < board->n_pacmans; p++) {
        pacman_t* pac = &board->pacmans[p];
        if (pac->pos_x == new_x && pac->pos_y == new_y && pac->alive) {
            pac->alive = 0;
            kill_pacman(board, p);
            return DEAD_PACMAN;
        }
    }
    return VALID_MOVE;
}

/**
 * @brief Converte coordenadas (x, y) num índice linear do array do tabuleiro.
 */
static inline int get_board_index(board_t* board, int x, int y) {
    return y * board->width + x;
}

/**
 * @brief Verifica se as coordenadas estão dentro dos limites do tabuleiro.
 */
static inline int is_valid_position(board_t* board, int x, int y) {
    return (x >= 0 && x < board->width) && (y >= 0 && y < board->height); // Inside of the board boundaries
}

void sleep_ms(int milliseconds) {
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (milliseconds % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

/**
 * @brief Executa a lógica de movimento do Pacman.
 * Trata de colisões com paredes, monstros, portais e recolha de pontos.
 * @return Estado do movimento (VALID_MOVE, DEAD_PACMAN, REACHED_PORTAL, etc.)
 */
int move_pacman(board_t* board, int pacman_index, command_t* command) {
    if (pacman_index < 0 || !board->pacmans[pacman_index].alive) {
        return DEAD_PACMAN; // Invalid or dead pacman
    }

    pacman_t* pac = &board->pacmans[pacman_index];
    int new_x = pac->pos_x;
    int new_y = pac->pos_y;

    // check passo
    if (pac->waiting > 0) {
        pac->waiting -= 1;
        return VALID_MOVE;        
    }
    pac->waiting = pac->passo;

    char direction = command->command;

    if (direction == 'R') {
        char directions[] = {'W', 'S', 'A', 'D'};
        direction = directions[rand() % 4];
    }

    // Calculate new position based on direction
    switch (direction) {
        case 'W': // Up
            new_y--;
            break;
        case 'S': // Down
            new_y++;
            break;
        case 'A': // Left
            new_x--;
            break;
        case 'D': // Right
            new_x++;
            break;
        case 'T': // Wait
            if (command->turns_left == 1) {
                pac->current_move += 1; // move on
                command->turns_left = command->turns;
            }
            else command->turns_left -= 1;
            return VALID_MOVE;
        default:
            return INVALID_MOVE; // Invalid direction
    }

    // Logic for the WASD movement
    pac->current_move+=1;

    // Check boundaries
    if (!is_valid_position(board, new_x, new_y)) {
        return INVALID_MOVE;
    }

    int old_index = get_board_index(board, pac->pos_x, pac->pos_y);
    int new_index = get_board_index(board, new_x, new_y);

    // Check for walls
    char target_content = board->board[new_index].content;

    // Check for walls
    if (target_content == 'W') {
        return INVALID_MOVE;
    }

    // Check for ghosts
    if (target_content == 'M') {
        kill_pacman(board, pacman_index);
        return DEAD_PACMAN;
    }

    // Collect points
    if (board->board[new_index].has_dot) {
        pac->points++;
        board->board[new_index].has_dot = 0;
    }

    board->board[old_index].content = ' ';
    pac->pos_x = new_x;
    pac->pos_y = new_y;
    board->board[new_index].content = 'P';

    if (board->board[new_index].has_portal) {
        return REACHED_PORTAL;
    }

    return VALID_MOVE;
}

/**
 * @brief Lógica específica para o movimento "Charged" (C) dos fantasmas.
 * O fantasma move-se em linha reta numa direção até encontrar um obstáculo.
 */
static int move_ghost_charged_direction(board_t* board, ghost_t* ghost, char direction, int* new_x, int* new_y) {
    int x = ghost->pos_x;
    int y = ghost->pos_y;
    *new_x = x;
    *new_y = y;

    switch (direction) {
        case 'W': // Up
            if (y == 0) return INVALID_MOVE;
            *new_y = 0; // In case there is no colision
            for (int i = y - 1; i >= 0; i--) {
                char target_content = board->board[get_board_index(board, x, i)].content;
                if (target_content == 'W' || target_content == 'M') {
                    *new_y = i + 1; // stop before colision
                    return VALID_MOVE;
                }
                else if (target_content == 'P') {
                    *new_y = i;
                    return find_and_kill_pacman(board, *new_x, *new_y);
                }
            }
            break;

        case 'S': // Down
            if (y == board->height - 1) return INVALID_MOVE;
            *new_y = board->height - 1; // In case there is no colision
            for (int i = y + 1; i < board->height; i++) {
                char target_content = board->board[get_board_index(board, x, i)].content;
                if (target_content == 'W' || target_content == 'M') {
                    *new_y = i - 1; // stop before colision
                    return VALID_MOVE;
                }
                if (target_content == 'P') {
                    *new_y = i;
                    return find_and_kill_pacman(board, *new_x, *new_y);
                }
            }
            break;

        case 'A': // Left
            if (x == 0) return INVALID_MOVE;
            *new_x = 0; // In case there is no colision
            for (int j = x - 1; j >= 0; j--) {
                char target_content = board->board[get_board_index(board, j, y)].content;
                if (target_content == 'W' || target_content == 'M') {
                    *new_x = j + 1; // stop before colision
                    return VALID_MOVE;
                }
                if (target_content == 'P') {
                    *new_x = j;
                    return find_and_kill_pacman(board, *new_x, *new_y);
                }
            }
            break;

        case 'D': // Right
            if (x == board->width - 1) return INVALID_MOVE;
            *new_x = board->width - 1; // In case there is no colision
            for (int j = x + 1; j < board->width; j++) {
                char target_content = board->board[get_board_index(board, j, y)].content;
                if (target_content == 'W' || target_content == 'M') {
                    *new_x = j - 1; // stop before colision
                    return VALID_MOVE;
                }
                if (target_content == 'P') {
                    *new_x = j;
                    return find_and_kill_pacman(board, *new_x, *new_y);
                }
            }
            break;
        default:
            debug("DEFAULT CHARGED MOVE - direction = %c\n", direction);
            return INVALID_MOVE;
    }
    return VALID_MOVE;
}   

/**
 * @brief Executa a lógica de movimento de um fantasma.
 * Suporta movimentos aleatórios (R), carregados (C), espera (T) e direcionais (WASD).
 */
int move_ghost(board_t* board, int ghost_index, command_t* command) {
    if (ghost_index < 0) {
        return INVALID_MOVE; // Invalid ghost_index
    }

    ghost_t* ghost = &board->ghosts[ghost_index];
    int new_x = ghost->pos_x;
    int new_y = ghost->pos_y;

    // check passo
    if (ghost->waiting > 0) {
        ghost->waiting -= 1;
        return VALID_MOVE;
    }
    ghost->waiting = ghost->passo;

    char direction = command->command;
    debug("COMMAND: %c\n", command->command);

    // Calculate new position based on direction
    switch (direction) {
        case 'W': // Up
        case 'S': // Down
        case 'A': // Left
        case 'D': // Right
            if (ghost->charged == 1) {
                move_t res = move_ghost_charged_direction(board, ghost, direction, &new_x, &new_y);
                if (res == DEAD_PACMAN || res == VALID_MOVE) {
                    ghost->charged = 0;
                    command->turns_left = command->turns;
                    ghost->current_move += 1;
                    break;
                } else {
                    return INVALID_MOVE;
                }
            }
            if (direction == 'W') new_y--;
            else if (direction == 'S') new_y++;
            else if (direction == 'A') new_x--;
            else if (direction == 'D') new_x++;
            break;
        case 'R': // Random direction
        {
            debug("RANDOM MOVE\n");
            char directions[] = {'W', 'S', 'A', 'D'};
            direction = directions[rand() % 4];

            if (direction == 'W') new_y--;
            else if (direction == 'S') new_y++;
            else if (direction == 'A') new_x--;
            else if (direction == 'D') new_x++;
            break;
        }
        case 'C': // Charge, next movement will be in straight line
            debug("CHARGED MODE\n");
            ghost->charged = 1;
            command->turns_left = command->turns;
            ghost->current_move += 1;
            return VALID_MOVE;
        case 'T': // Wait (T n)
            debug("Wait: %d\n", command->turns_left);
            if (command->turns_left == 1) {
                ghost->current_move += 1; // move on
                command->turns_left = command->turns;
            } else {
                command->turns_left -= 1;
            }
            return VALID_MOVE;
        default:
            return INVALID_MOVE; // Invalid direction
    }

    // Logic for the WASD movement for ghost
    ghost->current_move += 1;

    // Check boundaries
    if (!is_valid_position(board, new_x, new_y)) {
        return INVALID_MOVE;
    }

    int old_index = get_board_index(board, ghost->pos_x, ghost->pos_y);
    int new_index = get_board_index(board, new_x, new_y);

    // Check for walls
    char target_content = board->board[new_index].content;

    if (target_content == 'W') {
        debug("COLISION DETECTED: %c\n", target_content);
        return INVALID_MOVE;
    }
    if (target_content == 'M') {
        debug("Movement monster failure: Another monster in the way\n");
        return INVALID_MOVE;
    }
    if (target_content == 'P') {
        debug("Movement monster success: pacman killed\n");
        return find_and_kill_pacman(board, new_x, new_y);
    }

    // Move ghost
    board->board[old_index].content = ' ';
    ghost->pos_x = new_x;
    ghost->pos_y = new_y;
    board->board[new_index].content = 'M';

    return VALID_MOVE;
}


/**
 * @brief Marca o Pacman como morto (alive = 0).
 */
void kill_pacman(board_t* board, int pacman_index) {
    board->pacmans[pacman_index].alive = 0;
}

/* Static Loading - Funções legadas ou de teste para carregar pacman hardcoded */
int load_pacman(board_t* board, int points) {
    board->board[1 * board->width + 1].content = 'P'; // Pacman
    board->pacmans[0].pos_x = 1;
    board->pacmans[0].pos_y = 1;
    board->pacmans[0].alive = 1;
    board->pacmans[0].points = points;
    return 0;
}

// Static Loading - Funções legadas ou de teste para carregar ghosts hardcoded
int load_ghost(board_t* board) {
    // Ghost 0
    board->board[3 * board->width + 1].content = 'M'; // Monster
    
    board->ghosts[0].pos_x = 1;
    board->ghosts[0].pos_y = 3;
    board->ghosts[0].passo = 0;
    board->ghosts[0].n_moves = 8;
    board->ghosts[0].current_move = 0;
    board->ghosts[0].waiting = 0;
    board->ghosts[0].charged = 0;

    // Ghost 1
    board->board[3 * board->width + 8].content = 'M'; // Monster
    
    board->ghosts[1].pos_x = 8;
    board->ghosts[1].pos_y = 3;
    board->ghosts[1].passo = 1;
    board->ghosts[1].n_moves = 8;
    board->ghosts[1].current_move = 0;
    board->ghosts[1].waiting = 0;
    board->ghosts[1].charged = 0;
    
    command_t move;

    // Movements for the ghosts
    //Ghost 0 movements
    move.command = 'A';
    move.turns = 1;
    move.turns_left = 1;
    board->ghosts[0].moves[0] = move;
    board->ghosts[0].moves[1] = move;

    move.command = 'D';
    move.turns = 1;
    move.turns_left = 1;
    board->ghosts[0].moves[2] = move;
    board->ghosts[0].moves[3] = move;
    
    move.command = 'T';
    move.turns = 2;
    move.turns_left = 2;
    board->ghosts[0].moves[4] = move;

    move.command = 'W';
    move.turns = 1;
    move.turns_left = 1;
    board->ghosts[0].moves[5] = move;
    board->ghosts[0].moves[6] = move;
    board->ghosts[0].moves[7] = move;

    // Ghost 1 movements
    move.command = 'S';
    move.turns = 1;
    move.turns_left = 1;
    board->ghosts[1].moves[0] = move;
    board->ghosts[1].moves[1] = move;

    move.command = 'T';
    move.turns = 1;
    move.turns_left = 1;
    board->ghosts[1].moves[2] = move;

    move.command = 'A';
    move.turns = 1;
    move.turns_left = 1;
    board->ghosts[1].moves[3] = move;

    move.command = 'D';
    move.turns = 1;
    move.turns_left = 1;
    board->ghosts[1].moves[4] = move;

    move.command = 'T';
    move.turns = 2;
    move.turns_left = 2;
    board->ghosts[1].moves[5] = move;

    move.command = 'W';
    move.turns = 1;
    move.turns_left = 1;
    board->ghosts[1].moves[6] = move;
    board->ghosts[1].moves[7] = move;

    return 0;
}

/* extrai o número do nome de ficheiro (ex: level_1.lvl -> 1) */
static int extract_level_number(const char *filename) {
    const char *p = filename;

    // avança até ao primeiro dígito
    while (*p && !(*p >= '0' && *p <= '9')){
        p++;
    }

    return atoi(p); // atoi lê o número até encontrar "."
}

/* compara nomes do nivel para os organizar via qsort */
static int cmp_level_names(const void *a, const void *b) {
    const char *A = *(const char **)a;
    const char *B = *(const char **)b;

    int numA = extract_level_number(A);
    int numB = extract_level_number(B);

    return (numA - numB);
}

/**
 * @brief Inicializa a lista de níveis lendo a diretoria especificada.
 * Filtra ficheiros .lvl e ordena-os numericamente.
 */
int init_levels(const char *level_dir) {
    g_num_levels = 0;
    g_current_level = 0;

    // guarda a diretoria base
    strncpy(g_base_dir, level_dir, sizeof(g_base_dir) - 1);
    g_base_dir[sizeof(g_base_dir) - 1] = '\0';

    // se já existia uma lista, liberta
    if (g_level_files != NULL) {
        for (int i = 0; i < g_num_levels; ++i) {
            free(g_level_files[i]);
        }
        free(g_level_files);
        g_level_files = NULL;
    }

    DIR *dir = opendir(level_dir);
    if (!dir) {
        perror("opendir init_levels");
        return -1;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;

        // ignora "." e ".."
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;

        // aceita só ficheiros que terminem em .lvl
        size_t len = strlen(name);
        if (len < 4 || strcmp(name + (len - 4), ".lvl") != 0)
            continue;

        // constrói caminho completo: "<dir>/<ficheiro>"
        char fullpath[MAX_FILENAME + 2];    // +2 para aceitar o /0 no fullpath (256 + 2)
        snprintf(fullpath, sizeof(fullpath), "%s/%s", level_dir, name);

        // aumenta o array dinamicamente
        char **tmp = realloc(g_level_files, (g_num_levels + 1) * sizeof(char *));
        if (!tmp) {
            perror("realloc g_level_files");
            closedir(dir);
            return -1;
        }
        g_level_files = tmp;

        g_level_files[g_num_levels] = strdup(fullpath);
        if (!g_level_files[g_num_levels]) {
            perror("strdup level path");
            closedir(dir);
            return -1;
        }

        g_num_levels++;
    }

    closedir(dir);

    if (g_num_levels == 0) {
        fprintf(stderr, "No .lvl files found in %s\n", level_dir);
        return -1;
    }

    qsort(g_level_files, g_num_levels, sizeof(char *), cmp_level_names);

    return 0;
}

/**
 * @brief Coloca o Pacman na posição default definida no ficheiro de nível (primeiro 'o').
 */
static void place_default_pacman(board_t *board, int points, int default_pac_x, int default_pac_y) {
    int x = default_pac_x;
    int y = default_pac_y;

    if (!is_valid_position(board, x, y)) {
        x = 1;
        y = 1;
    }

    pacman_t *pac = &board->pacmans[0];

    pac->pos_x = x;
    pac->pos_y = y;
    pac->alive = 1;
    pac->points = points;
    pac->passo = 0;
    pac->waiting = 0;
    pac->current_move = 0;
    pac->n_moves = 0;  /* 0 = controlado pelo utilizador */

    board->board[get_board_index(board, x, y)].content = 'P';
}

/**
 * @brief Carrega o Pacman com base num ficheiro de comportamento (.p).
 */
static int load_pacman_from_behavior(board_t *board, const char *behavior_path, int points) {
    int passo = 0, row = 0, col = 0, n_moves = 0;
    command_t moves[MAX_MOVES];

    if (parse_behavior_file(behavior_path, &passo, &row, &col, moves, &n_moves) != 0) {
        return -1;
    }

    if (!is_valid_position(board, col, row)) {
        return -1;
    }

    pacman_t *pac = &board->pacmans[0];

    pac->pos_x = col;
    pac->pos_y = row;
    pac->alive = 1;
    pac->points = points;
    pac->passo = passo;
    pac->waiting = passo;
    pac->current_move = 0;
    pac->n_moves = n_moves;

    for (int i = 0; i < n_moves; ++i) {
        pac->moves[i] = moves[i];
    }

    board->board[get_board_index(board, col, row)].content = 'P';
    return 0;
}

/**
 * @brief Carrega um fantasma com base num ficheiro de comportamento (.m).
 */
static int load_ghost_from_behavior(board_t *board, int ghost_index, const char *behavior_path) {
    if (ghost_index < 0 || ghost_index >= board->n_ghosts){
        return -1;
    }

    int passo = 0, row = 0, col = 0, n_moves = 0;
    command_t moves[MAX_MOVES];

    if (parse_behavior_file(behavior_path, &passo, &row, &col, moves, &n_moves) != 0) {
        return -1;
    }

    if (!is_valid_position(board, col, row)) {
        return -1;
    }

    ghost_t *g = &board->ghosts[ghost_index];

    g->pos_x = col;
    g->pos_y = row;
    g->passo = passo;
    g->waiting = passo;
    g->current_move = 0;
    g->n_moves = n_moves;
    g->charged = 0;

    for (int i = 0; i < n_moves; ++i) {
        g->moves[i] = moves[i];
    }

    board->board[get_board_index(board, col, row)].content = 'M';
    return 0;
}

/**
 * @brief Carrega o próximo nível da lista global.
 * Reinicializa estruturas e carrega ficheiros de comportamento associados.
 */
int load_level(board_t *board, int points) {
    if (g_current_level >= g_num_levels) {
        return -1;  /* sem mais níveis */
    }

    const char *lvl_path = g_level_files[g_current_level];

    int default_pac_x = 1;
    int default_pac_y = 1;

    if (parse_level_file(lvl_path, board, &default_pac_x, &default_pac_y) != 0) {
        return -1;
    }

    board->n_pacmans = 1;
    board->pacmans = calloc(board->n_pacmans, sizeof(pacman_t));
    if (!board->pacmans) {
        perror("calloc pacmans");
        return -1;
    }

    if (board->n_ghosts > 0) {
        board->ghosts = calloc(board->n_ghosts, sizeof(ghost_t));
        if (!board->ghosts) {
            perror("calloc ghosts");
            free(board->pacmans);
            return -1;
        }
    } else {
        board->ghosts = NULL;
    }

    if (board->pacman_file[0] != '\0') {
        char fullpath[512];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", g_base_dir, board->pacman_file);
        if (load_pacman_from_behavior(board, fullpath, points) != 0) {
            place_default_pacman(board, points, default_pac_x, default_pac_y);
        }
    } else {
        place_default_pacman(board, points, default_pac_x, default_pac_y);
    }

    for (int i = 0; i < board->n_ghosts; ++i) {
        char fullpath[512];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", g_base_dir, board->ghosts_files[i]);
        load_ghost_from_behavior(board, i, fullpath);
    }

    g_current_level++;
    return 0;
}

/**
 * @brief Liberta a memória alocada para o nível atual.
 */
void unload_level(board_t * board) {
    free(board->board);
    free(board->pacmans);
    free(board->ghosts);
}

void open_debug_file(char *filename) {
    debugfile = fopen(filename, "w");
}

void close_debug_file() {
    fclose(debugfile);
}

void debug(const char * format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(debugfile, format, args);
    va_end(args);

    fflush(debugfile);
}

/**
 * @brief Imprime o estado atual do tabuleiro para o ficheiro de debug.
 */
void print_board(board_t *board) {
    if (!board || !board->board) {
        debug("[%d] Board is empty or not initialized.\n", getpid());
        return;
    }

    // Large buffer to accumulate the whole output
    char buffer[8192];
    size_t offset = 0;

    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       "=== [%d] LEVEL INFO ===\n"
                       "Dimensions: %d x %d\n"
                       "Tempo: %d\n"
                       "Pacman file: %s\n",
                       getpid(), board->height, board->width, board->tempo, board->pacman_file);

    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       "Monster files (%d):\n", board->n_ghosts);

    for (int i = 0; i < board->n_ghosts; i++) {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                           "  - %s\n", board->ghosts_files[i]);
    }

    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "Board Layout:\n");

    for (int y = 0; y < board->height; y++) {
        for (int x = 0; x < board->width; x++) {
            int idx = get_board_index(board, x, y);
            if (offset < sizeof(buffer) - 2) {
                buffer[offset++] = board->board[idx].content;
            }
        }
        if (offset < sizeof(buffer) - 2) {
            buffer[offset++] = '\n';
        }
    }

    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "==================\n");

    buffer[offset] = '\0';

    debug("%s", buffer);
}