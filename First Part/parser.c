#include "parser.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "constants.h"

static pthread_mutex_t mutex_get_next = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t global_barrier_mutex = PTHREAD_MUTEX_INITIALIZER;
extern pthread_mutex_t all_waited_mutex;
extern pthread_mutex_t wait_flag_mutex;
extern pthread_mutex_t global_thread_id_mutex;
extern pthread_mutex_t global_delay_mutex;

int barrier_flag = 0;

int wait_flag = 0;
unsigned int global_delay = 0;
unsigned int global_thread_id = 0;

static int read_uint(int fd, unsigned int *value, char *next) {
  char buf[16];

  int i = 0;
  while (1) {
    if (read(fd, buf + i, 1) == 0) {
      *next = '\0';
      break;
    }

    *next = buf[i];

    if (buf[i] > '9' || buf[i] < '0') {
      buf[i] = '\0';
      break;
    }

    i++;
  }

  unsigned long ul = strtoul(buf, NULL, 10);

  if (ul > UINT_MAX) {
    return 1;
  }

  *value = (unsigned int)ul;

  return 0;
}

static void cleanup(int fd) {
  char ch;
  while (read(fd, &ch, 1) == 1 && ch != '\n');
}

enum Command get_next(int fd) {
  pthread_mutex_lock(&mutex_get_next);
  char buf[16];
  

  if (read(fd, buf, 1) != 1) {
    pthread_mutex_unlock(&mutex_get_next);
    return EOC;
  }

  switch (buf[0]) {
    case 'C':
      if (read(fd, buf + 1, 6) != 6 || strncmp(buf, "CREATE ", 7) != 0) {
        cleanup(fd);
        pthread_mutex_unlock(&mutex_get_next);
        return CMD_INVALID;
      }
      pthread_mutex_unlock(&mutex_get_next);
      return CMD_CREATE;

    case 'R':
      if (read(fd, buf + 1, 7) != 7 || strncmp(buf, "RESERVE ", 8) != 0) {
        cleanup(fd);
        pthread_mutex_unlock(&mutex_get_next);
        return CMD_INVALID;
      }
      pthread_mutex_unlock(&mutex_get_next);
      return CMD_RESERVE;

    case 'S':
      if (read(fd, buf + 1, 4) != 4 || strncmp(buf, "SHOW ", 5) != 0) {
        cleanup(fd);
        pthread_mutex_unlock(&mutex_get_next);
        return CMD_INVALID;
      }
      pthread_mutex_unlock(&mutex_get_next);
      return CMD_SHOW;

    case 'L':
      if (read(fd, buf + 1, 3) != 3 || strncmp(buf, "LIST", 4) != 0) {
        cleanup(fd);
        pthread_mutex_unlock(&mutex_get_next);
        return CMD_INVALID;
      }

      if (read(fd, buf + 4, 1) != 0 && buf[4] != '\n') {
        cleanup(fd);
        pthread_mutex_unlock(&mutex_get_next);
        return CMD_INVALID;
      }
      pthread_mutex_unlock(&mutex_get_next);
      return CMD_LIST_EVENTS;

    case 'B':
      if (read(fd, buf + 1, 6) != 6 || strncmp(buf, "BARRIER", 7) != 0) {
        cleanup(fd);
        pthread_mutex_unlock(&mutex_get_next);
        return CMD_INVALID;
      }

      if (read(fd, buf + 7, 1) != 0 && buf[7] != '\n') {
        cleanup(fd);
        pthread_mutex_unlock(&mutex_get_next);
        return CMD_INVALID;
      }
      pthread_mutex_unlock(&mutex_get_next);
      //lock da barrier
      pthread_mutex_lock(&global_barrier_mutex);
      barrier_flag = 1;
      pthread_mutex_unlock(&global_barrier_mutex);
      return CMD_BARRIER;

    case 'W':
      if (read(fd, buf + 1, 4) != 4 || strncmp(buf, "WAIT ", 5) != 0) {
        cleanup(fd);
        pthread_mutex_unlock(&mutex_get_next);
        return CMD_INVALID;
      }
      pthread_mutex_unlock(&mutex_get_next);
      return CMD_WAIT;

    case 'H':
      if (read(fd, buf + 1, 3) != 3 || strncmp(buf, "HELP", 4) != 0) {
        cleanup(fd);
        pthread_mutex_unlock(&mutex_get_next);
        return CMD_INVALID;
      }

      if (read(fd, buf + 4, 1) != 0 && buf[4] != '\n') {
        cleanup(fd);
        pthread_mutex_unlock(&mutex_get_next);
        return CMD_INVALID;
      }
      pthread_mutex_unlock(&mutex_get_next);
      return CMD_HELP;

    case '#':
      cleanup(fd);
      pthread_mutex_unlock(&mutex_get_next);
      return CMD_EMPTY;

    case '\n':
    pthread_mutex_unlock(&mutex_get_next);
      return CMD_EMPTY;

    default:
      cleanup(fd);
      pthread_mutex_unlock(&mutex_get_next);
      return CMD_INVALID;
  }
}

int parse_create(int fd, unsigned int *event_id, size_t *num_rows, size_t *num_cols) {
  char ch;

  if (read_uint(fd, event_id, &ch) != 0 || ch != ' ') {
    cleanup(fd);
    return 1;
  }

  unsigned int u_num_rows;
  if (read_uint(fd, &u_num_rows, &ch) != 0 || ch != ' ') {
    cleanup(fd);
    return 1;
  }
  *num_rows = (size_t)u_num_rows;

  unsigned int u_num_cols;
  if (read_uint(fd, &u_num_cols, &ch) != 0 || (ch != '\n' && ch != '\0')) {
    cleanup(fd);
    return 1;
  }
  *num_cols = (size_t)u_num_cols;

  return 0;
}

size_t parse_reserve(int fd, size_t max, unsigned int *event_id, size_t *xs, size_t *ys) {
  char ch;

  if (read_uint(fd, event_id, &ch) != 0 || ch != ' ') {
    cleanup(fd);
    return 0;
  }

  if (read(fd, &ch, 1) != 1 || ch != '[') {
    cleanup(fd);
    return 0;
  }

  size_t num_coords = 0;
  while (num_coords < max) {
    if (read(fd, &ch, 1) != 1 || ch != '(') {
      cleanup(fd);
      return 0;
    }

    unsigned int x;
    if (read_uint(fd, &x, &ch) != 0 || ch != ',') {
      cleanup(fd);
      return 0;
    }
    xs[num_coords] = (size_t)x;

    unsigned int y;
    if (read_uint(fd, &y, &ch) != 0 || ch != ')') {
      cleanup(fd);
      return 0;
    }
    ys[num_coords] = (size_t)y;

    num_coords++;

    if (read(fd, &ch, 1) != 1 || (ch != ' ' && ch != ']')) {
      cleanup(fd);
      return 0;
    }

    if (ch == ']') {
      break;
    }
  }

  if (num_coords == max) {
    cleanup(fd);
    return 0;
  }

  if (read(fd, &ch, 1) != 1 || (ch != '\n' && ch != '\0')) {
    cleanup(fd);
    return 0;
  }

  return num_coords;
}

int parse_show(int fd, unsigned int *event_id) {
  char ch;

  if (read_uint(fd, event_id, &ch) != 0 || (ch != '\n' && ch != '\0')) {
    cleanup(fd);
    return 1;
  }

  return 0;
}

int parse_wait(int fd, unsigned int *delay, unsigned int *thread_id) {
  char ch;

  if (read_uint(fd, delay, &ch) != 0) {
    cleanup(fd);
    return -1;
  }

  if (ch == ' ') {
    if (thread_id == NULL) {
      cleanup(fd);
      return 0;
    }

    if (read_uint(fd, thread_id, &ch) != 0 || (ch != '\n' && ch != '\0')) {
      cleanup(fd);
      return -1;
    }
    //Ativa a wait flag -> Se for 2 é porque é especfico para um thread
    wait_flag = 2;
    //lock do delay
    pthread_mutex_lock(&global_delay_mutex);
    global_delay = *delay;
    pthread_mutex_unlock(&global_delay_mutex);
    global_thread_id = *thread_id;

    return 1;
  } else if (ch == '\n' || ch == '\0') {
    // Assign value to global variable
    //Ativa a wait flag -> Se for 1 é porque é global
    pthread_mutex_lock(&global_delay_mutex);
    global_delay = *delay;
    pthread_mutex_unlock(&global_delay_mutex);
    wait_flag = 1;

    return 0;
  } else {
    cleanup(fd);
    return -1;
  }
}
