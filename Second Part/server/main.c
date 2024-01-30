#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include "common/constants.h"
#include "common/io.h"
#include "operations.h"
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <netinet/in.h>
#include <signal.h>

const char *fifo_server_path;

#define MAX_PIPE_NAME_LENGTH 40
#define S 1 //maximo de threads trabalhadoras
#define QUEUE_MAX_LENGTH 100 //maximo de sessoes que podem estar na queue

int server_fd=-1;
int session_number=0;

int count=0;
int consptr=0;
int prodptr=0;


pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;  
pthread_cond_t podeProd = PTHREAD_COND_INITIALIZER;  
pthread_cond_t podeCons = PTHREAD_COND_INITIALIZER;


/*cria struct session para guardar o session_id e os nomes dos named_pipes que o cliente indicou*/

typedef struct session {
  int session_id;
  char* req_pipe_path;
  char* resp_pipe_path;
 
}session_t;


session_t* queue[QUEUE_MAX_LENGTH];

volatile sig_atomic_t print_requested = 0;

void sigusr1_handler(int signo) {
  if (signo == SIGUSR1) {
    print_requested = 1;
  }
}

/*coloca a session na fila*/
void enqueue_session(session_t *session) {
  
    pthread_mutex_lock(&mutex);
    while (count == QUEUE_MAX_LENGTH)
        pthread_cond_wait(&podeProd, &mutex);

    queue[prodptr] = session;

    prodptr ++; if(prodptr==QUEUE_MAX_LENGTH) prodptr = 0;
    count++;


    pthread_cond_signal(&podeCons);
    pthread_mutex_unlock(&mutex);
}

/*retira a session da fila*/
session_t *dequeue_session() {
  session_t *session;
  
  pthread_mutex_lock(&mutex);
  while (count == 0)
      pthread_cond_wait(&podeCons, &mutex);

  session = queue[consptr];
  consptr ++; if (consptr == QUEUE_MAX_LENGTH) consptr = 0;
  count--;

  pthread_cond_signal(&podeProd);
  pthread_mutex_unlock(&mutex);

  return session;
}


void case_quit(session_t *session){
  unlink(session->req_pipe_path);
  unlink(session->resp_pipe_path);

  free(session);
  
}



void case_create(session_t *session,int request_fd,int response_fd){

  unsigned int event_id=(unsigned int) -1;
  size_t num_rows=(size_t) -1;
  size_t num_cols=(size_t) -1;

  size_t readed=0;
  while(readed < sizeof(unsigned int)){
    ssize_t bytes_read = read(request_fd, &event_id, sizeof(unsigned int));
    if (bytes_read == -1) {
      fprintf(stderr,"[ERR]: read of event_id on session id(%d) failed\n", session->session_id);
      exit(EXIT_FAILURE); 
    }
    readed+=(size_t)bytes_read;
  }


  readed=0;
  while(readed < sizeof(size_t)){
    ssize_t bytes_read = read(request_fd, &num_rows, sizeof(size_t));
    if (bytes_read == -1) {
      perror("Erro ao ler o num_rows do cliente");
      exit(EXIT_FAILURE); 
    }
    readed+=(size_t)bytes_read;
  }

  readed=0;
  while(readed < sizeof(size_t)){
    ssize_t bytes_read = read(request_fd, &num_cols, sizeof(size_t));
    if (bytes_read == -1) {
      perror("Erro ao ler o num_cols do cliente");
      exit(EXIT_FAILURE); 
    }
    readed+=(size_t)bytes_read;
  }


  int response = ems_create(event_id, num_rows, num_cols);

  size_t written=0;

  while(written < sizeof(int)){
    ssize_t bytes_written = write(response_fd, &response, sizeof(int));
    if (bytes_written == -1) {
      perror("Erro ao escrever a resposta para o cliente");
      exit(EXIT_FAILURE); 
    }
    written+=(size_t)bytes_written;
  }

}



void case_reserve(session_t *session, int request_fd,int response_fd){
  unsigned int event_id;
  ssize_t bytes_read = read(request_fd, &event_id, sizeof(unsigned int));
  if (bytes_read == -1) {
    perror("Erro ao ler o event_id do cliente");
    exit(EXIT_FAILURE);
  }
  size_t num_seats;

  bytes_read = read(request_fd, &num_seats, sizeof(size_t));
  if (bytes_read == -1) {
    perror("Erro ao ler o num_seats do cliente");
    exit(EXIT_FAILURE);
  }

  size_t xs[num_seats];
  size_t ys[num_seats];

  /*Read do xs*/
  for (size_t i = 0; i < num_seats; i++) {
    bytes_read = read(request_fd, &xs[i], sizeof(size_t));
    if (bytes_read == -1) {
      perror("Erro ao ler o xs do cliente");
      exit(EXIT_FAILURE); 
    }
  }

  /*read do xy*/
  for (size_t i = 0; i < num_seats; i++) {
    bytes_read = read(request_fd, &ys[i], sizeof(size_t));
    if (bytes_read == -1) {
      perror("Erro ao ler o ys do cliente");
      exit(EXIT_FAILURE); 
    }
  }
  
  int response = ems_reserve(event_id, num_seats, xs, ys);

  /*fazer write da response para o cliente*/
  if(write(response_fd, &response, sizeof(int)) != sizeof(int)){
    fprintf(stderr, "[ERR]: write(%s) failed: %s\n", session->resp_pipe_path, strerror(errno));
    exit(EXIT_FAILURE);
  }

}

void case_show (session_t *session ,int request_fd,int response_fd){
  /*ler o event id do read*/
  unsigned int event_id;

  size_t readed=0;
  while(readed < sizeof(unsigned int)){
    ssize_t bytes_read = read(request_fd, &event_id, sizeof(unsigned int));
    if (bytes_read == -1) {
      fprintf(stderr,"[ERR]: read on session id(%d) failed\n", session->session_id);
      exit(EXIT_FAILURE); 
    }
    readed+=(size_t)bytes_read;
  }

  ems_show(response_fd, event_id);
}



void case_list (int response_fd){

  ems_list_events(response_fd);

}


/*função dada às threads*/
void *worker_thread_function(void *arg) {
  (void)arg;
  while(1){

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    session_t *session = dequeue_session();

    int response_fd = open(session->resp_pipe_path, O_WRONLY);
  
    if (response_fd == -1) {
        fprintf(stderr, "[ERR]: open(%s) failed: %s\n", session->resp_pipe_path, strerror(errno));
        exit(EXIT_FAILURE);
    }
    
    /*enviar o session_id para o cliente*/
    size_t written=0;
    while(written < sizeof(int)){
      ssize_t bytes_written = write(response_fd, &session->session_id, sizeof(int));
      if (bytes_written == -1) {
        perror("Erro ao escrever a resposta para o cliente");
        exit(EXIT_FAILURE);
      }
      written+=(size_t)bytes_written;
    }

    int request_fd = open(session->req_pipe_path, O_RDONLY);
    if (request_fd == -1) {
        fprintf(stderr, "[ERR]: open(%s) failed: %s\n", session->req_pipe_path, strerror(errno));
        exit(EXIT_FAILURE);
    }

    while (1) {

      char buf[1];
      buf[0] = '\0';
      ssize_t bytesRead = read(request_fd, buf, sizeof(char));
      if (bytesRead == 0) {
        break;
      } else if (bytesRead != sizeof(char)) {
          fprintf(stderr, "[ERR]: read(%s) failed: %s\n", fifo_server_path, strerror(errno));
          exit(EXIT_FAILURE);
      }
      switch(buf[0]){
        case '2':
          case_quit(session);
          break;
        case '3':
          case_create(session,request_fd,response_fd);
          break;
        case '4':
          case_reserve(session,request_fd,response_fd);
          break;
        case '5':
          case_show(session,request_fd,response_fd);
          break;
        case '6':
          case_list(response_fd);
          break;
      }
    }
    close(request_fd);
    close(response_fd);
  }
}


int main(int argc, char* argv[]) {

  if (argc < 2 || argc > 3) {
    fprintf(stderr, "Usage: %s\n <pipe_path> [delay]\n", argv[0]);
    return 1;
  }

  fifo_server_path = argv[1] ;

  char* endptr;
  unsigned int state_access_delay_us = STATE_ACCESS_DELAY_US;
  if (argc == 3) {
    unsigned long int delay = strtoul(argv[2], &endptr, 10);

    if (*endptr != '\0' || delay > UINT_MAX) {
      fprintf(stderr, "Invalid delay value or value too large\n");
      return 1;
    }
    
    state_access_delay_us = (unsigned int)delay;
  }
  printf("Server started...\n");

  if (ems_init(state_access_delay_us)) {
    fprintf(stderr, "Failed to initialize EMS\n");
    return 1;
  }

  if (unlink(fifo_server_path) != 0 && errno != ENOENT) {
    fprintf(stderr, "[ERR]: unlink(%s) failed: %s\n", fifo_server_path,
            strerror(errno));
    exit(EXIT_FAILURE);
  }


  if(mkfifo(fifo_server_path, 0640) != 0) {
    fprintf(stderr, "[ERR]: mkfifo(%s) failed: %s\n",fifo_server_path,strerror(errno));
    exit(EXIT_FAILURE);
  }

  server_fd = open(fifo_server_path, O_RDONLY);
  if (server_fd == -1) {
    fprintf(stderr, "[ERR]: open(%s) failed: %s\n", fifo_server_path, strerror(errno));
    exit(EXIT_FAILURE);
  }

  for (int i = 0; i < S; i++) {
    pthread_t worker_thread;
    if (pthread_create(&worker_thread, NULL, worker_thread_function, NULL) != 0) {
        fprintf(stderr, "[ERR]: pthread_create() failed\n");
        exit(EXIT_FAILURE);
    }
  }

  while (1) {
    if (signal(SIGUSR1, sigusr1_handler) == SIG_ERR) {
      fprintf(stderr, "Erro ao configurar o tratamento para SIGUSR1\n");
    return 1;
    }

    if(print_requested==1){
      signal_show_list(); 
    }
    print_requested=0;
        char buf[1];
        buf[0] = '\0';
    
        ssize_t bytesRead = read(server_fd, buf, sizeof(char));
        if (bytesRead != 0) {
          if(buf[0] == '1') {
              session_number++;

              session_t* session = malloc(sizeof(session_t));
              if (session == NULL) {
                  fprintf(stderr, "Falha na alocação de memória para session_t.\n");
                  exit(EXIT_FAILURE);
              }
              session->session_id = session_number;
              char message[81];

              if (read(server_fd, message, 81) == -1) {
                  fprintf(stderr, "[ERR]: read(%d) failed: %s\n", server_fd, strerror(errno));
                  exit(EXIT_FAILURE);
              }
        
              char* req_pipe_path = malloc(MAX_PIPE_NAME_LENGTH);
              char* resp_pipe_path = malloc(MAX_PIPE_NAME_LENGTH);

              if (req_pipe_path == NULL || resp_pipe_path == NULL) {
                  fprintf(stderr, "Falha na alocação de memória para os caminhos dos pipes.\n");
                  exit(EXIT_FAILURE);
              }

              memcpy(req_pipe_path, message, 40);
              req_pipe_path[40] = '\0';

              memcpy(resp_pipe_path, message + 40, 40);
              resp_pipe_path[40] = '\0'; 

              session->req_pipe_path = req_pipe_path;
              session->resp_pipe_path = resp_pipe_path;

              enqueue_session(session);
      }
    }
  }
  close(server_fd);
  ems_terminate();
}