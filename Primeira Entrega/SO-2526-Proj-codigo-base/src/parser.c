#include "board.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

/**
 * @brief Remove espaços em branco e caracteres de controlo (newline, tab)
 * do início e do fim de uma string (trimming).
 */
static void trim(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);

    size_t l = strlen(s);
    while (l > 0 && (s[l-1] == '\n' || s[l-1] == '\r' || s[l-1] == ' ' || s[l-1] == '\t')) {
        s[--l] = '\0';
    }
}

/**
 * @brief Lê o conteúdo completo de um ficheiro para um buffer em memória.
 * 
 * @note O buffer retornado em *out_buf deve ser libertado com free() pelo chamador.
 */
static int read_entire_file(const char *path, char **out_buf, ssize_t *out_len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        close(fd);
        return -1;
    }

    ssize_t size = st.st_size;
    char *buf = malloc(size + 1);
    if (!buf) {
        perror("malloc");
        close(fd);
        return -1;
    }

    ssize_t total = 0;
    while (total < size) {
        ssize_t n = read(fd, buf + total, size - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("read");
            free(buf);
            close(fd);
            return -1;
        }
        if (n == 0) break;
        total += n;
    }

    buf[total] = '\0';
    close(fd);

    *out_buf = buf;
    if (out_len) *out_len = total;
    return 0;
}

/**
 * @brief Faz o parse do ficheiro de nível (.lvl).
 * 
 * Lê dimensões, tempo, ficheiros de comportamento e a grelha do tabuleiro.
 * Aloca memória para a estrutura board->board.
 */
int parse_level_file(const char *path, board_t *board, int *default_pac_x, int *default_pac_y) {
    char *buf = NULL;
    ssize_t len = 0;
    if (read_entire_file(path, &buf, &len) != 0) {
        return -1;
    }

    board->width = 0;
    board->height = 0;
    board->tempo = 0;
    board->n_ghosts = 0;
    board->n_pacmans = 1;
    board->board = NULL;
    board->pacmans = NULL;
    board->ghosts = NULL;

    // limpa nomes de ficheiros
    board->pacman_file[0] = '\0';
    for (int i = 0; i < MAX_GHOSTS; ++i) {
        board->ghosts_files[i][0] = '\0';
    }

    *default_pac_x = 1;
    *default_pac_y = 1;
    int found_pac_default = 0;

    char *saveptr = NULL;
    char *line = strtok_r(buf, "\n", &saveptr);
    int reading_grid = 0;
    int grid_row = 0;

    while (line) {
        trim(line);
        
        // avança para o primeiro char não espaço
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        if (*p == '#' || *p == '\0') {
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        if (!reading_grid) {
            if (strncmp(line, "DIM", 3) == 0) {
                int h, w;
                if (sscanf(line, "DIM %d %d", &h, &w) == 2) {
                    board->height = h;
                    board->width = w;
                }
            } else if (strncmp(line, "TEMPO", 5) == 0) {
                int t;
                if (sscanf(line, "TEMPO %d", &t) == 1) {
                    board->tempo = t;
                }
            } else if (strncmp(line, "PAC", 3) == 0) {
                // linha com ficheiro de comportamento do pacman
                char fname[256];
                if (sscanf(line, "PAC %255s", fname) == 1) {
                    strncpy(board->pacman_file, fname, sizeof(board->pacman_file) - 1);
                    board->pacman_file[sizeof(board->pacman_file) - 1] = '\0';
                }
            } else if (strncmp(line, "MON", 3) == 0) {
                // linha com ficheiros de comportamento dos monstros
                char *tok = strtok(line + 3, " \t");
                board->n_ghosts = 0;
                while (tok && board->n_ghosts < MAX_GHOSTS) {
                    while (*tok == ' ' || *tok == '\t') tok++;
                    if (*tok != '\0') {
                        strncpy(board->ghosts_files[board->n_ghosts], tok, sizeof(board->ghosts_files[0]) - 1);
                        board->ghosts_files[board->n_ghosts]
                            [sizeof(board->ghosts_files[0]) - 1] = '\0';
                        board->n_ghosts++;
                    }
                    tok = strtok(NULL, " \t");
                }
            } else {
                reading_grid = 1;
            }
        }

        if (reading_grid) {
            // estamos a ler as linhas da grelha do nível
            if (!board->board) {
                if (board->width <= 0 || board->height <= 0) {
                    free(buf);
                    return -1;
                }
                board->board = calloc(board->width * board->height, sizeof(board_pos_t));
                if (!board->board) {
                    perror("calloc");
                    free(buf);
                    return -1;
                }
            }

            if (grid_row < board->height) {
                // copia cada caracter da linha para a grelha
                for (int j = 0; j < board->width && line[j] != '\0'; ++j) {
                    char ch = line[j];
                    int idx = grid_row * board->width + j;

                    board->board[idx].content = ' ';
                    board->board[idx].has_dot = 0;
                    board->board[idx].has_portal = 0;

                    if (ch == 'X') {
                        board->board[idx].content = 'W';    //parede
                    } else if (ch == 'o') {
                        board->board[idx].has_dot = 1;
                        // primeira 'o' encontrada pode definir pos default do pacman
                        if (!found_pac_default) {
                            *default_pac_x = j;
                            *default_pac_y = grid_row;
                            found_pac_default = 1;
                        }
                    } else if (ch == '@') {
                        board->board[idx].has_portal = 1;   //portal
                    }
                }
                grid_row++;
            }
        }

        line = strtok_r(NULL, "\n", &saveptr);
    }

    free(buf);

    // guarda o nome do nível (caminho do ficheiro) para debug
    strncpy(board->level_name, path, sizeof(board->level_name) - 1);
    board->level_name[sizeof(board->level_name) - 1] = '\0';

    return 0;
}

/**
 * @brief Faz o parse de ficheiros de comportamento (.p ou .m).
 * Lê comandos de movimento (WASD, T, etc.) e posições iniciais.
 */
int parse_behavior_file(const char *path, int *passo, int *row, int *col, command_t *moves, int *n_moves) {
    char *buf = NULL;
    ssize_t len = 0;
    if (read_entire_file(path, &buf, &len) != 0) return -1;

    int passo_val = 0;
    int r = 0, c = 0;
    int count = 0;

    char *saveptr = NULL;
    char *line = strtok_r(buf, "\n", &saveptr);

    while (line) {
        trim(line);
        // ignora comentários e linhas vazias
        if (line[0] == '#' || line[0] == '\0') {
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        if (strncmp(line, "PASSO", 5) == 0) {
            int tmp;
            if (sscanf(line, "PASSO %d", &tmp) == 1) {
                passo_val = tmp;
            }
        } else if (strncmp(line, "POS", 3) == 0) {
            int rr, cc;
            if (sscanf(line, "POS %d %d", &rr, &cc) == 2) {
                r = rr;
                c = cc;
            }
        } else {
            // linhas com comandos de movimento
            if (count < MAX_MOVES) {
                command_t *cmd = &moves[count];
                if (line[0] == 'T') {
                    int n;
                    // comando "T (numero)", espera (numero) turnos
                    if (sscanf(line, "T %d", &n) == 1) {
                        cmd->command = 'T';
                        cmd->turns = n;
                        cmd->turns_left = n;
                        count++;
                    }
                } else {
                    // comandos normais (WASD)
                    cmd->command = line[0];
                    cmd->turns = 1;
                    cmd->turns_left = 1;
                    count++;
                }
            }
        }

        line = strtok_r(NULL, "\n", &saveptr);
    }

    free(buf);

    if (passo){
        *passo = passo_val;
    }
    if (row){
        *row = r;
    }
    if (col){
        *col = c;
    }
    if (n_moves){
        *n_moves = count;
    }

    return 0;
}