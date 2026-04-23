#include "board.h"
#include "display.h"
#include "parser.h"
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <pthread.h>

#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2
#define LOAD_BACKUP 3
#define CREATE_BACKUP 4

static int backup = 0;      //garante que só existe um backup guardado

// sincronização e threads dos fantasmas
static pthread_rwlock_t board_lock;   // Lock para proteger acesso concorrente ao tabuleiro
static int ghosts_should_run = 0;     // 1 enquanto as threads devem correr
static board_t *global_board = NULL;  // ponteiro para o tabuleiro atual

/**
 * @brief Atualiza o ecrã de forma thread-safe e aplica o delay do jogo.
 * Usa draw_board do display.c.
 */
void screen_refresh(board_t * game_board, int mode) {
    debug("REFRESH\n");
    draw_board(game_board, mode);
    refresh_screen();
    if(game_board->tempo != 0)
        sleep_ms(game_board->tempo);       
}

/**
 * @brief Lógica de um "turno" do jogo para o Pacman e Input do utilizador.
 * Verifica input, move o Pacman e verifica condições de vitória/derrota.
 * @return Estado do jogo (CONTINUE, NEXT_LEVEL, QUIT, BACKUP, LOAD_BACKUP).
 */
int play_board(board_t * game_board) {
    pacman_t* pacman = &game_board->pacmans[0];
    command_t* play;
    command_t c; 
    char tecla_pressionada = '\0';

    if (pacman->n_moves == 0) {
        // só em modo manual é que aceita teclado
        tecla_pressionada = get_input();
    }

    if (pacman->n_moves == 0) { // if is user input
        c.command = tecla_pressionada;

        if(c.command == '\0')
            return CONTINUE_PLAY;

        c.turns = 1;
        play = &c;
    } else {// else if the moves are pre-defined in the file
        // avoid buffer overflow wrapping around with modulo of n_moves
        // this ensures that we always access a valid move for the pacman
        play = &pacman->moves[pacman->current_move%pacman->n_moves];
    }

    debug("KEY %c\n", play->command);

    if (tecla_pressionada == 'G') {
        return CREATE_BACKUP;
    }

    if (tecla_pressionada == 'Q') {
        return QUIT_GAME;
    }

    int result = move_pacman(game_board, 0, play);
    if (result == REACHED_PORTAL) {
        // Next level
        return NEXT_LEVEL;
    }

    if (!game_board->pacmans[0].alive) {
        if (backup){
            return LOAD_BACKUP;
        }
        return QUIT_GAME;
    }  

    return CONTINUE_PLAY;  
}

/**
 * @brief Função executada pelas threads dos fantasmas.
 * 
 * IMPORTANTE: Usa Read-Write Locks.
 * - Read Lock: Para ler o próximo movimento sem bloquear outros leitores.
 * - Write Lock: Para efetuar o movimento no tabuleiro (alteração de estado).
 */
void* ghost_worker(void* arg) {
    int ghost_index = *(int*)arg;
    free(arg);

    while (ghosts_should_run) {
        // Adquire lock de leitura para consultar dados
        pthread_rwlock_rdlock(&board_lock);

        if (global_board == NULL || ghost_index >= global_board->n_ghosts) {
            pthread_rwlock_unlock(&board_lock);
            break;
        }

        ghost_t* ghost = &global_board->ghosts[ghost_index];

        if (ghost->n_moves == 0) {
            pthread_rwlock_unlock(&board_lock);
            sleep_ms(200);
            continue;
        }

        command_t* play =
            &ghost->moves[ghost->current_move % ghost->n_moves];

        pthread_rwlock_unlock(&board_lock);

        // Adquire lock de escrita para modificar o tabuleiro
        pthread_rwlock_wrlock(&board_lock);
        move_ghost(global_board, ghost_index, play);
        pthread_rwlock_unlock(&board_lock);

        sleep_ms(200);
    }

    return NULL;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        printf("Usage: %s <level_directory>\n", argv[0]);
        return 1;
    }

    // Random seed for any random movements
    srand((unsigned int)time(NULL));

    open_debug_file("debug.log");

    if (init_levels(argv[1]) != 0) { 
        printf("Error: could not load levels from directory '%s'\n", argv[1]);
        close_debug_file();
        return 1;
    }

    terminal_init();

    int accumulated_points = 0;
    bool end_game = false;
    board_t game_board;

    while (!end_game) {
        if (load_level(&game_board, accumulated_points) != 0) {
            // sem mais níveis ou erro a carregar - sai do ciclo
            break;
        }

        global_board = &game_board;
        // Inicializa o lock para este nível
        pthread_rwlock_init(&board_lock, NULL);
        ghosts_should_run = 1;

        // apenas uma thread por ghost
        pthread_t ghost_threads[game_board.n_ghosts];
        for (int i = 0; i < game_board.n_ghosts; i++) {
            int *idx = malloc(sizeof(int));
            if (!idx) {
                // em caso de erro de malloc, o mais simples é parar o jogo
                fprintf(stderr, "Erro a alocar idx do ghost %d\n", i);
                ghosts_should_run = 0;
                game_board.n_ghosts = i; // só dá join aos ghosts que já foram criados
                break;
            }
            *idx = i;
            pthread_create(&ghost_threads[i], NULL, ghost_worker, idx);
        }

        // Desenha estado inicial
        pthread_rwlock_rdlock(&board_lock);
        draw_board(&game_board, DRAW_MENU);
        refresh_screen();
        pthread_rwlock_unlock(&board_lock);

        while (true) {
            int result = play_board(&game_board);

            // Lógica de Quicksave (Backup) usando fork()
            // O processo pai espera que o filho termine.
            // O processo filho continua o jogo.
            // Se o filho morrer (Pacman morre), retorna erro e o pai retoma o jogo do ponto de backup.
            if (result == CREATE_BACKUP) {
                if (!backup) {          // só é possível ter um estado guardado
                    pid_t pid = fork();
                    if (pid < 0) {
                        // ser tivermos um erro no fork, ignoramos o backup
                    } else if (pid > 0) {
                        // precesso pai
                        backup = 1;

                        int status;
                        waitpid(pid, &status, 0);

                        if (WIFEXITED(status) && WEXITSTATUS(status) == 1) {
                            end_game = true;
                            break;
                        }

                        backup = 0;

                        pthread_rwlock_rdlock(&board_lock);
                        screen_refresh(&game_board, DRAW_MENU);
                        pthread_rwlock_unlock(&board_lock);
                        continue;
                    } else {
                        // processo filho
                        // IMPORTANTE: Threads não sobrevivem ao fork.
                        // É necessário recriar as threads dos fantasmas e o lock no processo filho.
                        backup = 1;  // o filho sabe que já existe backup;
                        pthread_rwlock_destroy(&board_lock);
                        pthread_rwlock_init(&board_lock, NULL);

                        ghosts_should_run = 1;
                        for (int i = 0; i < game_board.n_ghosts; i++) {
                            int *idx = malloc(sizeof(int));
                            *idx = i;
                            pthread_create(&ghost_threads[i], NULL, ghost_worker, idx);
                        }
                    }
                }
                // se já havia backup, a tecla G não faz nada
                pthread_rwlock_rdlock(&board_lock);
                screen_refresh(&game_board, DRAW_MENU);
                pthread_rwlock_unlock(&board_lock);
                continue;
            }

            if (result == LOAD_BACKUP) {
                // estamos no processo filho e se o pacman morrer, volta ao processo pai para que este retome o quicksave
                exit(0);
            }

            if (result == NEXT_LEVEL) {
                pthread_rwlock_rdlock(&board_lock);
                screen_refresh(&game_board, DRAW_WIN);
                pthread_rwlock_unlock(&board_lock);
                sleep_ms(game_board.tempo);
                break;
            }

            if (result == QUIT_GAME) {
                pthread_rwlock_rdlock(&board_lock);
                screen_refresh(&game_board, DRAW_GAME_OVER);
                pthread_rwlock_unlock(&board_lock);
                sleep_ms(game_board.tempo);

                if (backup) {
                    exit(1); //hardquit
                }

                end_game = true;
                break;
            }

            // Redesenha o tabuleiro após a jogada
            pthread_rwlock_rdlock(&board_lock);
            screen_refresh(&game_board, DRAW_MENU);
            pthread_rwlock_unlock(&board_lock);

            accumulated_points = game_board.pacmans[0].points;
        }

        // termina as threads dos fantasmas do nivel em questão
        ghosts_should_run = 0;
        for (int i = 0; i < game_board.n_ghosts; i++) {
            pthread_join(ghost_threads[i], NULL);
        }
        pthread_rwlock_destroy(&board_lock);

        print_board(&game_board);
        unload_level(&game_board);
    }

    terminal_cleanup();
    close_debug_file();

    return 0;
}