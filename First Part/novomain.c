//Diogo Guerreiro
//André Bento
//Grupo 97



#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "constants.h"
#include "operations.h"
#include "parser.h"
#include <pthread.h>


#define MAX_PATH_LENGTH 256
// pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;


extern int barrier_flag;
extern int wait_flag;
extern unsigned global_thread_id;
extern pthread_mutex_t global_barrier_mutex;
extern unsigned int global_delay;

int max_threads_global = 0 ;
int all_waited = 0;

//mutexes globais para as variaveis globais

pthread_mutex_t all_waited_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t wait_flag_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t global_thread_id_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t global_delay_mutex = PTHREAD_MUTEX_INITIALIZER;






void* process_file(void* arg);
void process_directory(const char *jobs_directory, int max_proc,int max_threads);

// Structure to hold file descriptors
struct FileDescriptors {
    int* fd_ptr;
    int* fd_out_ptr;
    int thread_id;
    pthread_rwlock_t rw_lock;
    int* waited;

};

int main(int argc, char *argv[]) {
  unsigned int state_access_delay_ms = STATE_ACCESS_DELAY_MS;

  char *jobs_directory = argv[1];
  int max_proc = atoi(argv[2]); 
  int max_threads = atoi (argv[3]);
  

  if (argc < 4) {
      fprintf(stderr, "Usage: %s <JOBS_DIRECTORY> <MAX_PROC> <MAX_THREADS>\n", argv[0]);
      return 1;
  }

  if (argc > 4) {
  char *endptr;
  unsigned long int delay = strtoul(argv[4], &endptr, 10);

  if (*endptr != '\0' || delay > UINT_MAX) {
    fprintf(stderr, "Invalid delay value or value too large\n");
    return 1;
  }

  state_access_delay_ms = (unsigned int)delay;
  }


  if (max_proc <= 0) {
      fprintf(stderr, "MAX_PROC must be a positive integer.\n");
      return 1;
  }

  if (ems_init(state_access_delay_ms)) {
    fprintf(stderr, "Failed to initialize EMS\n");
    return 1;
  }

  process_directory(jobs_directory,max_proc,max_threads);
  ems_terminate();
  return 0;
}

// Função que é chamada por cada thread para processar um arquivo de comandos e retornar os descritores de arquivo
void* process_file(void* arg){
  struct FileDescriptors* fds = (struct FileDescriptors*)arg;
  //lock da struct
  pthread_rwlock_rdlock(&(fds->rw_lock));
  int fd = *(fds->fd_ptr);
  int fd_out = *(fds->fd_out_ptr);
  int thread_id = fds->thread_id;
  int* waited = fds->waited;
  pthread_rwlock_unlock(&(fds->rw_lock));

  

  unsigned int event_id, delay, thread_id_in;
  size_t num_rows, num_columns, num_coords;
  size_t xs[MAX_RESERVATION_SIZE], ys[MAX_RESERVATION_SIZE];

  fflush(stdout);

  enum Command cmd;
  //lock da barrier

  while (1) {
    //verificar a flag da barreira para sair do ciclo sem o ficheiro ter terminado
    pthread_mutex_lock(&global_barrier_mutex);
    if (barrier_flag == 1){
      pthread_mutex_unlock(&global_barrier_mutex);
      break;
    }
    pthread_mutex_unlock(&global_barrier_mutex);

    //lock do thread_id
   

    //verifica a flag do wait foi ativada e faz todo o processo de espera do mesmo
    //Verifica se foi levantada uma flag de wait sem thread_id
    pthread_mutex_lock(&wait_flag_mutex);
    int aux_wait_flag = wait_flag;
    pthread_mutex_unlock(&wait_flag_mutex);
    if (aux_wait_flag == 1){

      if (waited[thread_id -1] == 0){
        ems_wait(global_delay);
        waited[thread_id - 1] = 1;
        //lock do all_waited
        pthread_mutex_lock(&all_waited_mutex);
        int auxi = ++all_waited;
        pthread_mutex_unlock(&all_waited_mutex);
        if (auxi == max_threads_global){
          pthread_mutex_lock(&wait_flag_mutex);
          wait_flag = 0;
          pthread_mutex_unlock(&wait_flag_mutex);
          all_waited = 0;
          for (int i = 0; i < max_threads_global; i++){
            waited[i] = 0;
          }
        }
      }
    }

    pthread_mutex_lock(&wait_flag_mutex);
    int aux_wait_flag_2 = wait_flag;
    pthread_mutex_unlock(&wait_flag_mutex);
    //verifica se foi levantada uma flag de wait com thread_id
    if (aux_wait_flag_2 == 2){
      //lock do global_thread_id
      pthread_mutex_lock(&global_thread_id_mutex);
      unsigned int aux_thread_id = global_thread_id;
      pthread_mutex_unlock(&global_thread_id_mutex);
      if (thread_id == (int)aux_thread_id){
        //lock do global_delay
        pthread_mutex_lock(&global_delay_mutex);
        ems_wait(global_delay);
        pthread_mutex_unlock(&global_delay_mutex);
        //lock do wait_flag
        pthread_mutex_lock(&wait_flag_mutex);
        wait_flag = 0;
        pthread_mutex_unlock(&wait_flag_mutex);
        //lock do global_thread_id
        pthread_mutex_lock(&global_thread_id_mutex);
        global_thread_id = 0;
        pthread_mutex_unlock(&global_thread_id_mutex);
      }
    }
    //caso tenha chegado o EOC, sai antecipadamente do ciclo  
    cmd = get_next(fd);
    if (cmd == EOC) {
        break;
    }
    switch (cmd) {
      case CMD_CREATE:
        if (parse_create(fd, &event_id, &num_rows, &num_columns) != 0) {
          fprintf(stderr, "Invalid command. See HELP for usage\n");
          continue;
        }
        if (ems_create(event_id, num_rows, num_columns)) {
          fprintf(stderr, "Failed to create event\n");
        }
        break;

      case CMD_RESERVE:
        num_coords = parse_reserve(fd, MAX_RESERVATION_SIZE, &event_id, xs, ys);

        if (num_coords == 0) {
          fprintf(stderr, "Invalid command. See HELP for usage\n");
          continue;
        }
        if (ems_reserve(event_id, num_coords, xs, ys)) {
          fprintf(stderr, "Failed to reserve seats\n");
        }
        break;

      case CMD_SHOW:
        if (parse_show(fd, &event_id) != 0) {
          fprintf(stderr, "Invalid command. See HELP for usage\n");
          continue;
        }
        
        if (ems_show(event_id,fd_out)) {
          fprintf(stderr, "Failed to show event\n");
        }
        break;

      case CMD_LIST_EVENTS:
        printf ("list\n");
        if (ems_list_events(fd_out)) {
          fprintf(stderr, "Failed to list events\n");
        }
        break;

      case CMD_WAIT:
        if (parse_wait(fd, &delay, &thread_id_in) == -1) {  // thread_id is not implemented
          fprintf(stderr, "Invalid command. See HELP for usage\n");
          continue;
        }
        if (delay > 0) {
          printf("Waiting...\n");

        }

        break;

      case CMD_INVALID:
        fprintf(stderr, "Invalid command. See HELP for usage\n");
        break;

      case CMD_HELP:
        printf(
            "Available commands:\n"
            "  CREATE <event_id> <num_rows> <num_columns>\n"
            "  RESERVE <event_id> [(<x1>,<y1>) (<x2>,<y2>) ...]\n"
            "  SHOW <event_id>\n"
            "  LIST\n"
            "  WAIT <delay_ms> [thread_id]\n"  // thread_id is not implemented
            "  BARRIER\n"                      // Not implemented
            "  HELP\n");

        break;

      case CMD_BARRIER:  // Not implemented
        // ems_barrier();
        break;

      case CMD_EMPTY:
        break;

      case EOC:
        break;
      }
    }

  // Caso a flag da barreira tenha sido levantada, retorna os descritores de arquivo. Caso tenha chegado ao fim do ficheiro, retorna NULL
  pthread_mutex_lock(&global_barrier_mutex);
  int aux = barrier_flag;
  pthread_mutex_unlock(&global_barrier_mutex);
  if (aux == 1) {
      struct FileDescriptors* result_fds = malloc(sizeof(struct FileDescriptors));
      if (!result_fds) {
          perror("Erro ao alocar memória para os descritores de arquivo de retorno");
          return NULL;
      }
      //lock da struct
      pthread_rwlock_rdlock(&(fds->rw_lock));
      result_fds->fd_ptr = fds->fd_ptr;
      result_fds->fd_out_ptr = fds->fd_out_ptr;
      pthread_rwlock_unlock(&(fds->rw_lock));
      return result_fds;
  }

  return NULL;

}


void process_directory(const char *jobs_directory, int max_proc,int max_threads) {
  DIR *dir;
  struct dirent *entry;
  if ((dir = opendir(jobs_directory)) == NULL) {
      perror("opendir");
      return;
  }

  int active_procs = 0;
  // Ler todos os arquivos no diretório
  while ((entry = readdir(dir)) != NULL) {
    if (strstr(entry->d_name, ".jobs") != NULL) {
      char filepath[MAX_PATH_LENGTH];

      strncpy(filepath, jobs_directory, MAX_PATH_LENGTH);
      strncat(filepath, "/", MAX_PATH_LENGTH - strlen(filepath) - 1);
      strncat(filepath, entry->d_name, MAX_PATH_LENGTH - strlen(filepath) - 1);
      
      while (active_procs >= max_proc) {
          pid_t child_pid = wait(NULL);
          if (child_pid == -1) {
              perror("wait");
              break;
          }
          active_procs--;
      }
      
      // Criar um novo processo filho
      pid_t pid = fork();

      if (pid == -1) {
          perror("fork error");
          break;  // Terminar o loop se não for possível criar um novo processo
      } else if (pid == 0) {
          // Processo filho
            pthread_t threads[max_threads];

          int fd = open(filepath, O_RDONLY);
          char *outfilepath = malloc(strlen(filepath) + 5);
          strcpy(outfilepath, filepath);
          char *dot = strrchr(outfilepath, '.');
          strcpy(dot, ".out");

          int fd_out = open(outfilepath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
          
          if (fd == -1) {
            perror("Error opening file");
            exit(1);
          }

          int *waited = (int *)malloc((long unsigned int)max_threads * sizeof(int));
          if (waited == NULL) {
            perror("malloc");
            exit(EXIT_FAILURE);
          }
          
          for (int i = 0; i < max_threads; ++i) {
            waited[i] = 0;
          }
          //struct que vai ser passada como argumento para a thread
          struct FileDescriptors* fds = malloc(sizeof(struct FileDescriptors));
            fds->fd_ptr = &fd;
            fds->fd_out_ptr = &fd_out;
            fds->waited = waited;

            // Inicializar a read-write lock
              if (pthread_rwlock_init(&(fds->rw_lock), NULL) != 0) {
                  perror("pthread_rwlock_init");
                  exit(EXIT_FAILURE);
              }
          
          max_threads_global = max_threads;


          int ended = 0;
          //ciclo que vai criar as threads até o ficheiro acabar. Em caso de barreira, espera que todas as threads acabem e cria novas
          while (ended == 0){

            for (int i = 0; i < max_threads; i++) {
                //lock da struct
                pthread_rwlock_wrlock(&(fds->rw_lock));
                fds->thread_id = i + 1;
                pthread_rwlock_unlock(&(fds->rw_lock));
                //cria a thread
                int thread_create_status = pthread_create(&threads[i], NULL, process_file, (void*)fds);

                if (thread_create_status != 0) {
                    fprintf(stderr, "Falha ao criar a thread %d\n", i);
                    perror("wait");
                    break;
                }
            }

            for (int j = 0; j < max_threads; ++j) {
              void* thread_retval;
              pthread_join(threads[j], &thread_retval);
              //verifica se o ficheiro acabou e se não acabou, retorna os descritores de arquivo 
              if (thread_retval != NULL) {
                  struct FileDescriptors* fds_result = (struct FileDescriptors*)thread_retval;
                  pthread_rwlock_wrlock(&(fds->rw_lock));
                  fds->fd_ptr = fds_result->fd_ptr;
                  fds->fd_out_ptr = fds_result->fd_out_ptr;
                  pthread_rwlock_unlock(&(fds->rw_lock));
                  free(fds_result);
              }else{
                ended = 1;
              }
            }
            pthread_mutex_lock(&global_barrier_mutex);
            barrier_flag = 0;
            pthread_mutex_unlock(&global_barrier_mutex);
          }
          

        exit(EXIT_SUCCESS);
      } else {
          // Processo pai
          active_procs++;
          printf("Processo filho com PID %d criado.\n", pid);
      }
    }
  }

  closedir(dir);

  // Aguardar todos os processos filhos restantes terminarem
  while (active_procs > 0) {
    pid_t child_pid = wait(NULL);

    if (child_pid == -1) {
        // Adicionar tratamento de erro, se necessário
        perror("wait");
        break;
    }
    active_procs--;
    printf("Processo filho com PID %d terminou.\n", child_pid);
  }
}