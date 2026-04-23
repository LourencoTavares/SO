#include "board.h"
#include "display.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>
#include "protocol.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <ctype.h>
#include <semaphore.h>       
#include <errno.h>          
#include <signal.h>         

#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2
#define BUF_SIZE 32

static void* host_thread(void *arg);
static void* worker_thread(void *arg);
static void* ghost_thread_session(void *arg);
static void* pacman_thread(void *arg);
static void run_session(int slot, const char *levels_dir, int req_fd, int notif_fd);

static int parse_client_id(const char *req_path);  
static void write_top5_file(void);      

int thread_shutdown = 0;

typedef struct {            
    board_t *board;
    int notif_fd;
    volatile int *shutdown_flag;
    volatile int *victory_flag;
    volatile int *game_over_flag;
    int slot;
} sender_arg_t;

typedef struct {
    board_t *board;
    int ghost_index;
    volatile int *shutdown_flag;
} ghost_thread_arg_t;

typedef struct {
    board_t *board;
    int req_fd;
    volatile int *shutdown_flag;
} pacman_thread_arg_t;

typedef struct {
  char req_path[MAX_PIPE_PATH_LENGTH];
  char notif_path[MAX_PIPE_PATH_LENGTH];
} connect_req_t;                

typedef struct {
    const char *fifo_registo;
} host_arg_t;                

typedef struct {
    const char *levels_dir;
    int worker_id;
} worker_arg_t;                

typedef struct{             
    int active;
    int client_id;
    board_t *board;
    int points;
    size_t notif_bytes;
} session_info_t;

// estrutura auxiliar para ranking
typedef struct {    
    int client_id;
    int points;
} rank_entry_t;

static int cmp_rank_desc(const void *a, const void *b) {
    const rank_entry_t *ra = (const rank_entry_t*)a;
    const rank_entry_t *rb = (const rank_entry_t*)b;
    return (rb->points - ra->points); 
}

static session_info_t *sessions = NULL;     
static int max_games_global = 0;
static pthread_mutex_t sessions_mtx = PTHREAD_MUTEX_INITIALIZER;

static volatile sig_atomic_t got_sigusr1 = 0;       

static void write_top5_file(void) {
    if (max_games_global == 0) return;
    rank_entry_t *entries = malloc(sizeof(rank_entry_t) * max_games_global);
    if (!entries) return;

    int n = 0;

    pthread_mutex_lock(&sessions_mtx);
    for (int i = 0; i < max_games_global; i++) {
        if (!sessions[i].active) continue;

        entries[n].client_id = sessions[i].client_id;
        entries[n].points    = sessions[i].points;
        n++;
    }
    pthread_mutex_unlock(&sessions_mtx);

    qsort(entries, n, sizeof(entries[0]), cmp_rank_desc);

    FILE *f = fopen("top5_clients.txt", "w");
    debug("HOST: writing top5 file with %d entries\n", n);
    if (f) {
        int limit = (n < 5) ? n : 5;
        for (int i = 0; i < limit; i++) {
            fprintf(f, "client_id=%d points=%d\n", entries[i].client_id, entries[i].points);
        }
        fclose(f);
    }
    free(entries);
}

static connect_req_t q[BUF_SIZE];                
static int q_head = 0;                
static int q_tail = 0;                

static pthread_mutex_t q_mtx = PTHREAD_MUTEX_INITIALIZER;                
static sem_t *sem_items; // nº de pedidos disponíveis                
static sem_t *sem_slots; // nº de lugares livres               

static int active_players = 0;
static pthread_mutex_t active_mtx = PTHREAD_MUTEX_INITIALIZER;


static void sigusr1_handler(int sig){          
    (void)sig;
    const char *msg = "DEBUG: SIGUSR1 received in handler!\n";      //debug
    write(STDOUT_FILENO, msg, strlen(msg));
    got_sigusr1 = 1;
}

// extrai client_id do caminho do FIFO ("..._3" -> 3)
static int parse_client_id(const char *req_path) {
    if (!req_path) return -1;   

    int len = (int)strlen(req_path);
    int i = len - 1;

    while (i >= 0 && !isdigit((unsigned char)req_path[i])) i--;
    if (i < 0) return -1;

    while (i >= 0 && isdigit((unsigned char)req_path[i])) i--;

    return atoi(&req_path[i + 1]);
}


static void enqueue_req(const connect_req_t *r) {               
    sem_wait(sem_slots);
    pthread_mutex_lock(&q_mtx);

    q[q_tail] = *r;
    q_tail = (q_tail + 1) % BUF_SIZE;

    pthread_mutex_unlock(&q_mtx);
    sem_post(sem_items);
}

static connect_req_t dequeue_req(void) {               
    sem_wait(sem_items);
    pthread_mutex_lock(&q_mtx);

    connect_req_t r = q[q_head];
    q_head = (q_head + 1) % BUF_SIZE;

    pthread_mutex_unlock(&q_mtx);
    sem_post(sem_slots);
    return r;
}

static void* host_thread(void *arg) {
    host_arg_t *h = (host_arg_t*)arg;
    const char *fifo_registo = h->fifo_registo;

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);

    unlink(fifo_registo);
    if (mkfifo(fifo_registo, 0666) < 0 && errno != EEXIST) {
        perror("mkfifo fifo_registo");
        pthread_exit(NULL);
    }

    int reg_fd = open(fifo_registo, O_RDWR);
    if (reg_fd < 0) {
        perror("open fifo_registo");
        pthread_exit(NULL);
    }

    while (1) {

        if (got_sigusr1) {
            got_sigusr1 = 0;
            write_top5_file();
        }

        char op;
        ssize_t r = read(reg_fd, &op, 1);

        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // nada para ler, continua (nao bloqueia)
                usleep(10000); 
                continue;
            }
            if (errno == EINTR) {
                continue;
            }
            perror("read opcode");
            continue;
        }

        if (op != (char)OP_CODE_CONNECT) continue;

        connect_req_t req;
        memset(&req, 0, sizeof(req));

        if (read(reg_fd, req.req_path, MAX_PIPE_PATH_LENGTH) != MAX_PIPE_PATH_LENGTH)
            continue;
        if (read(reg_fd, req.notif_path, MAX_PIPE_PATH_LENGTH) != MAX_PIPE_PATH_LENGTH)
            continue;

        req.req_path[MAX_PIPE_PATH_LENGTH - 1] = '\0';
        req.notif_path[MAX_PIPE_PATH_LENGTH - 1] = '\0';

        enqueue_req(&req);
    }

    close(reg_fd);
    return NULL;
}

//HELPER PARA A FUNÇÃO RUN_SESSION
static char* build_board_data(board_t *board) {             
    int w = board->width;
    int h = board->height;
    int size = w * h;

    char *out = (char*)malloc(size);
    if (!out) return NULL;

    int k = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int idx = y * w + x;
            char ch = board->board[idx].content;

            int ghost_charged = 0;
            for (int g = 0; g < board->n_ghosts; g++) {
                ghost_t *gh = &board->ghosts[g];
                if (gh->pos_x == x && gh->pos_y == y) {
                    if (gh->charged) ghost_charged = 1;
                    break;
                }
            }

            switch (ch) {
                case 'W': out[k++] = '#'; break;         // wall
                case 'P': out[k++] = 'C'; break;         // pacman
                case 'M': out[k++] = ghost_charged ? 'G' : 'M'; break; // ghost
                case ' ':
                    if (board->board[idx].has_portal) out[k++] = '@';
                    else if (board->board[idx].has_dot) out[k++] = '.';
                    else out[k++] = ' ';
                    break;
                default:
                    out[k++] = ch;
                    break;
            }
        }
    }
    return out;
}

static void* sender_thread(void *arg) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    sender_arg_t *s = (sender_arg_t*)arg;
    board_t *board = s->board;
    int fd = s->notif_fd;

    while (!*(s->shutdown_flag)) {
        sleep_ms(board->tempo);

        pthread_rwlock_rdlock(&board->state_lock);

        int w = board->width;
        int h = board->height;
        int tempo = board->tempo;
        int victory = *(s->victory_flag);
        int game_over = *(s->game_over_flag);
        int points = board->pacmans[0].points;

        pthread_mutex_lock(&sessions_mtx);
        if (s->slot >= 0 && s->slot < max_games_global && sessions[s->slot].active) {
            sessions[s->slot].points = points;
        }
        pthread_mutex_unlock(&sessions_mtx);


        char *data = build_board_data(board);

        pthread_rwlock_unlock(&board->state_lock);

        if (!data) {
            *(s->shutdown_flag) = 1;
            break;
        }

        int size = w * h;
        char op = (char)OP_CODE_BOARD;

        if (write(fd, &op, 1) != 1 ||
            write(fd, &w, sizeof(int)) != (ssize_t)sizeof(int) ||
            write(fd, &h, sizeof(int)) != (ssize_t)sizeof(int) ||
            write(fd, &tempo, sizeof(int)) != (ssize_t)sizeof(int) ||
            write(fd, &victory, sizeof(int)) != (ssize_t)sizeof(int) ||
            write(fd, &game_over, sizeof(int)) != (ssize_t)sizeof(int) ||
            write(fd, &points, sizeof(int)) != (ssize_t)sizeof(int) ||
            write(fd, data, size) != size) {

            free(data);
            *(s->shutdown_flag) = 1;
            break;
        }

        free(data);
    }

    free(s);
    return NULL;
}


void run_session(int slot, const char *levels_dir, int req_fd, int notif_fd) {       
    DIR *level_dir = opendir(levels_dir);
    if (!level_dir) {
        fprintf(stderr, "Failed to open directory: %s\n", levels_dir);
        return;
    }

    int accumulated_points = 0;
    int end_session = 0;
    struct dirent *entry;

    while ((entry = readdir(level_dir)) != NULL && !end_session) {

        if (entry->d_name[0] == '.') continue;
        char *dot = strrchr(entry->d_name, '.');
        if (!dot) continue;
        if (strcmp(dot, ".lvl") != 0) continue;

        board_t game_board;
        load_level(&game_board, entry->d_name, (char*)levels_dir, accumulated_points);
        pthread_mutex_lock(&sessions_mtx);
        sessions[slot].board = &game_board;
        pthread_mutex_unlock(&sessions_mtx);

        // flags por nível/sessão
        volatile int shutdown_flag = 0;
        volatile int victory_flag = 0;
        volatile int game_over_flag = 0;

        // cria ghosts
        pthread_t *ghost_tids = (pthread_t*)malloc(game_board.n_ghosts * sizeof(pthread_t));
        for (int i = 0; i < game_board.n_ghosts; i++) {
            ghost_thread_arg_t *garg = (ghost_thread_arg_t*)malloc(sizeof(*garg));
            garg->board = &game_board;
            garg->ghost_index = i;
            garg->shutdown_flag = &shutdown_flag;
            pthread_create(&ghost_tids[i], NULL, ghost_thread_session, garg);
        }

        pthread_t sender_tid;
        sender_arg_t *sarg = (sender_arg_t*)malloc(sizeof(*sarg));
        sarg->board = &game_board;
        sarg->notif_fd = notif_fd;
        sarg->shutdown_flag = &shutdown_flag;
        sarg->victory_flag = &victory_flag;
        sarg->game_over_flag = &game_over_flag;
        sarg->slot = slot;
        pthread_create(&sender_tid, NULL, sender_thread, sarg);

        // cria pacman 
        pthread_t pacman_tid;
        pacman_thread_arg_t *parg = (pacman_thread_arg_t*)malloc(sizeof(*parg));
        parg->board = &game_board;
        parg->req_fd = req_fd;
        parg->shutdown_flag = &shutdown_flag;
        pthread_create(&pacman_tid, NULL, pacman_thread, parg);

        // espera pacman terminar (NEXT_LEVEL / QUIT_GAME)
        int *retval = NULL;
        pthread_join(pacman_tid, (void**)&retval);

        int result = QUIT_GAME;
        if (retval) {
            result = *retval;
            free(retval);
        }

        shutdown_flag = 1;

        if (result == NEXT_LEVEL) {
            victory_flag = 1;
        } else if (result == QUIT_GAME){
            game_over_flag = 1;
        }

        // dá chance ao sender de enviar 1 último frame final
        sleep_ms(game_board.tempo);

        // join sender
        pthread_join(sender_tid, NULL);

        // join ghosts
        for (int i = 0; i < game_board.n_ghosts; i++) {
            pthread_join(ghost_tids[i], NULL);
        }
        free(ghost_tids);

        // atualizar pontos e descarregar nível
        accumulated_points = game_board.pacmans[0].points;
        unload_level(&game_board);

        if (result == QUIT_GAME) {
            end_session = 1;
        }
        // se NEXT_LEVEL, continua para o próximo .lvl
    }

    pthread_mutex_lock(&sessions_mtx);
    sessions[slot].active = 0;
    sessions[slot].board = NULL;
    pthread_mutex_unlock(&sessions_mtx);
    closedir(level_dir);
}

static void* worker_thread(void *arg) {
    worker_arg_t *w = (worker_arg_t*)arg;
    const char *levels_dir = w->levels_dir;
    int slot = w->worker_id; 

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    debug("WORKER[%d]: started (levels_dir='%s')\n", slot, levels_dir);

    while (1) {
        debug("WORKER[%d]: waiting request\n", slot);

        connect_req_t req = dequeue_req();
        debug("WORKER[%d]: got req='%s' notif='%s'\n", slot, req.req_path, req.notif_path);

        int client_id = parse_client_id(req.req_path);

        pthread_mutex_lock(&sessions_mtx);
        sessions[slot].active = 1;
        sessions[slot].client_id = client_id;
        sessions[slot].board = NULL;
        sessions[slot].points = 0;
        sessions[slot].notif_bytes = 0;
        pthread_mutex_unlock(&sessions_mtx);

        // abre notif FIFO
        int notif_fd = open(req.notif_path, O_WRONLY);
        if (notif_fd < 0) {
            perror("open notif_fd");
            pthread_mutex_lock(&sessions_mtx);
            sessions[slot].active = 0;
            sessions[slot].board = NULL;
            pthread_mutex_unlock(&sessions_mtx);
            continue;
        }

        // ACK
        char ack = (char)OP_CODE_CONNECT;
        char result = 0;
        write(notif_fd, &ack, 1);
        write(notif_fd, &result, 1);

        // abre req FIFO
        int req_fd = open(req.req_path, O_RDONLY);
        if (req_fd < 0) {
            perror("open req_fd");
            close(notif_fd);
            pthread_mutex_lock(&sessions_mtx);
            sessions[slot].active = 0;
            sessions[slot].board = NULL;
            pthread_mutex_unlock(&sessions_mtx);
            continue;
        }

        pthread_mutex_lock(&active_mtx);
        active_players++;
        pthread_mutex_unlock(&active_mtx);

        debug("WORKER[%d]: starting run_session\n", slot);
        run_session(slot, levels_dir, req_fd, notif_fd);
        debug("WORKER[%d]: run_session returned\n", slot);

        pthread_mutex_lock(&active_mtx);
        active_players--;
        int remaining = active_players;
        pthread_mutex_unlock(&active_mtx);

        close(req_fd);
        close(notif_fd);

        debug("WORKER[%d]: session closed; remaining=%d\n", slot, remaining);

        if (remaining == 0) {
            exit(0); 
        }
    }

    return NULL;
}

void* pacman_thread(void *arg) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    pacman_thread_arg_t *parg = (pacman_thread_arg_t*) arg;
    board_t *board = parg->board;
    int req_fd = parg->req_fd;
    volatile int *shutdown_flag = parg->shutdown_flag;
    free(parg);

    pacman_t* pacman = &board->pacmans[0];
    int *retval = malloc(sizeof(int));
    if (!retval) pthread_exit(NULL);

    while (true) {
        sleep_ms(board->tempo * (1 + pacman->passo));

        if (*shutdown_flag) {       //força a encerrar o servidor no caso dos clientes todos morrerem
            *retval = QUIT_GAME;
            return (void*)retval;
        }

        command_t* play = NULL;
        command_t c;
        memset(&c, 0, sizeof(c));

        if (pacman->n_moves == 0) {
            char op = 0;
            ssize_t r = read(req_fd, &op, 1);
            if (r != 1) {
                *retval = QUIT_GAME;
                return (void*)retval;
            }

            if (op == (char)OP_CODE_DISCONNECT) {
                *retval = QUIT_GAME;
                return (void*)retval;
            }

            if (op != (char)OP_CODE_PLAY) {
                continue;
            }

            r = read(req_fd, &c.command, 1);
            if (r != 1) {
                *retval = QUIT_GAME;
                return (void*)retval;
            }

            c.command = (char)toupper((unsigned char)c.command);
            if (c.command == '\0') continue;

            c.turns = 1;
            play = &c;
        } else {
            play = &pacman->moves[pacman->current_move % pacman->n_moves];
        }

        if (play->command == 'Q') {
            *retval = QUIT_GAME;
            return (void*) retval;
        }
        
        pthread_rwlock_rdlock(&board->state_lock);
        int result = move_pacman(board, 0, play);
        pthread_rwlock_unlock(&board->state_lock);

        if (result == REACHED_PORTAL) {
            *retval = NEXT_LEVEL;
            return (void*) retval;
        }

        if (result == DEAD_PACMAN) {
            *retval = QUIT_GAME;
            return (void*) retval;
        }
    }
}

static void* ghost_thread_session(void *arg) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    ghost_thread_arg_t *ghost_arg = (ghost_thread_arg_t*) arg;
    board_t *board = ghost_arg->board;
    int ghost_ind = ghost_arg->ghost_index;
    volatile int *shutdown_flag = ghost_arg->shutdown_flag;

    free(ghost_arg);

    ghost_t* ghost = &board->ghosts[ghost_ind];

    while (true) {
        sleep_ms(board->tempo * (1 + ghost->passo));

        pthread_rwlock_rdlock(&board->state_lock);
        if (*shutdown_flag) {
            pthread_rwlock_unlock(&board->state_lock);
            pthread_exit(NULL);
        }

        move_ghost(board, ghost_ind, &ghost->moves[ghost->current_move % ghost->n_moves]);
        pthread_rwlock_unlock(&board->state_lock);
    }
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr,
          "Usage: %s <levels_dir> <max_games> <fifo_registo>\n", argv[0]);
        return 1;
    }

    const char *levels_dir = argv[1];
    int max_games = atoi(argv[2]);
    const char *fifo_registo = argv[3];

    max_games_global = max_games;
    sessions = calloc(max_games, sizeof(session_info_t));
    if (!sessions) {
        perror("calloc sessions");
        return 1;
    }

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigusr1_handler;
    sa.sa_flags = 0; // garante que SA_RESTART não está ativo para interromper o read()
    if (sigaction(SIGUSR1, &sa, NULL) < 0) {
        perror("sigaction");
        return 1;
    }

    srand((unsigned int)time(NULL));
    open_debug_file("debug.log");

    sem_unlink("/sem_items");       // no MAC é necessário fazer isto, os semáforos não funcionam de igual forma no MAC e no linux
    sem_unlink("/sem_slots");
    sem_items = sem_open("/sem_items", O_CREAT, 0644, 0);
    if (sem_items == SEM_FAILED) {
        perror("sem_open items");
        return 1;
    }
    sem_slots = sem_open("/sem_slots", O_CREAT, 0644, BUF_SIZE);
    if (sem_slots == SEM_FAILED) {
        perror("sem_open slots");
        return 1;
    }

    // Host thread
    pthread_t host_tid;
    host_arg_t harg = { .fifo_registo = fifo_registo };
    pthread_create(&host_tid, NULL, host_thread, &harg);

    // pool de workers (cada worker com worker_id próprio)
    pthread_t *worker_tids = malloc(sizeof(pthread_t) * max_games);
    worker_arg_t *wargs = malloc(sizeof(worker_arg_t) * max_games);
    if (!worker_tids || !wargs) {
        perror("malloc workers");
        return 1;
    }

    for (int i = 0; i < max_games; i++) {
        wargs[i].levels_dir = levels_dir;
        wargs[i].worker_id = i;
        pthread_create(&worker_tids[i], NULL, worker_thread, &wargs[i]);
    }

    // servidor continua ligado até ordem contrario (ctrnl + c)
    pthread_join(host_tid, NULL);

    return 0;
}