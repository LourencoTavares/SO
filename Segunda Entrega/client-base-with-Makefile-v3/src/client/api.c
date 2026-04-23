#include "api.h"
#include "protocol.h"
#include "debug.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>

struct Session {
  int id;
  int req_pipe;
  int notif_pipe;
  char req_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
  char notif_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
};

static struct Session session = {.id = -1};
static Board board;

int pacman_connect(char const *req_pipe_path, char const *notif_pipe_path, char const *server_pipe_path) {
  char opcode;
  char result;
  char req_buf[MAX_PIPE_PATH_LENGTH] = {0};
  char notif_buf[MAX_PIPE_PATH_LENGTH] = {0};

  /* remover FIFOs antigos */
  unlink(req_pipe_path);
  unlink(notif_pipe_path);

  /* criar FIFOs do cliente */
  if (mkfifo(req_pipe_path, 0666) < 0) return 1;
  if (mkfifo(notif_pipe_path, 0666) < 0) return 1;

  /* abrir FIFO do servidor (registo) */
  int server_fd = open(server_pipe_path, O_WRONLY);
  if (server_fd < 0) return 1;

  /* enviar CONNECT */
  opcode = OP_CODE_CONNECT;
  write(server_fd, &opcode, sizeof(char));

  strncpy(req_buf, req_pipe_path, MAX_PIPE_PATH_LENGTH);
  strncpy(notif_buf, notif_pipe_path, MAX_PIPE_PATH_LENGTH);

  write(server_fd, req_buf, MAX_PIPE_PATH_LENGTH);
  write(server_fd, strlen(notif_buf), MAX_PIPE_PATH_LENGTH);
  
  close(server_fd);

  /* abrir FIFO de notificações */
  session.notif_pipe = open(notif_pipe_path, O_RDONLY);
  if (session.notif_pipe < 0) return 1;

  /* ler ACK do servidor */
  if (read(session.notif_pipe, &opcode, sizeof(char)) != 1) return 1;
  if (read(session.notif_pipe, &result, sizeof(char)) != 1) return 1;
  if (result != 0) return 1;

  /* abrir FIFO de pedidos */
  session.req_pipe = open(req_pipe_path, O_WRONLY);
  if (session.req_pipe < 0) return 1;

  /* guardar sessão */
  session.id = 0;
  strncpy(session.req_pipe_path, req_pipe_path, MAX_PIPE_PATH_LENGTH);
  strncpy(session.notif_pipe_path, notif_pipe_path, MAX_PIPE_PATH_LENGTH);

  board.data = NULL;

  return 0;
}

void pacman_play(char command) {
  char opcode = OP_CODE_PLAY;

  write(session.req_pipe, &opcode, sizeof(char));
  write(session.req_pipe, &command, sizeof(char));
}

int pacman_disconnect() {
  char opcode = OP_CODE_DISCONNECT;

  write(session.req_pipe, &opcode, sizeof(char));

  if (session.req_pipe >= 0) close(session.req_pipe);
  if (session.notif_pipe >= 0) close(session.notif_pipe);

  unlink(session.req_pipe_path);
  unlink(session.notif_pipe_path);

  session.req_pipe = -1;
  session.notif_pipe = -1;
  session.id = -1;

  if (board.data) {
    free(board.data);
    board.data = NULL;
  }

  return 0;
}

Board receive_board_update(void) {
  char opcode;

  if (read(session.notif_pipe, &opcode, sizeof(char)) != 1) {
    board.game_over = 1;
    return board;
  }

  if (opcode != OP_CODE_BOARD) {
    board.game_over = 1;
    return board;
  }

  /* ler campos do tabuleiro */
  if (read(session.notif_pipe, &board.width, sizeof(int)) != sizeof(int) ||
      read(session.notif_pipe, &board.height, sizeof(int)) != sizeof(int) ||
      read(session.notif_pipe, &board.tempo, sizeof(int)) != sizeof(int) ||
      read(session.notif_pipe, &board.victory, sizeof(int)) != sizeof(int) ||
      read(session.notif_pipe, &board.game_over, sizeof(int)) != sizeof(int) ||
      read(session.notif_pipe, &board.accumulated_points, sizeof(int)) != sizeof(int)) {
    board.game_over = 1;
    return board;
  }

  /* ler data */
  int size = board.width * board.height;

  if (board.data == NULL)
    board.data = malloc(size);
  else
    board.data = realloc(board.data, size);

  if (read(session.notif_pipe, board.data, size) != size) {
    board.game_over = 1;
    return board;
  }

  return board;
}
