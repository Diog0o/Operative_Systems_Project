#include "api.h"
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "common/io.h"

#define MAX_PIPE_NAME_LENGTH 40

int session_id=-1;
int request_fd;
int response_fd;

char const* global_req_pipe_path;
char const* global_resp_pipe_path;

/*A função verifica se os bytes written foram -1, caso seja -1 levanta um erro*/
int check_write_create(ssize_t bytes_written){
  if(bytes_written==-1){
    perror("Erro ao enviar pedido de criacao de evento para o servidor");
    return 1; 
  }
  return 0;
}

int check_write_reserve(ssize_t bytes_written){
  if(bytes_written==-1){
    perror("Erro ao enviar pedido de reserva de lugares para o servidor");
    return 1;
  }
  return 0;
}

int check_read(ssize_t bytes_read){
  if(bytes_read==-1){
    perror("Erro ao ler a mensagem do servidor");
    exit(EXIT_FAILURE);
  }
  return 0;
}

/*A função escreve no ficheiro .out da diretoria jobs, o equivalente ao show*/
int show_write(int out_fd, size_t num_rows, size_t num_cols, unsigned int seats[num_rows * num_cols]) {

  for (size_t i = 1; i <= num_rows; i++) {
    for (size_t j = 1; j <= num_cols; j++) {
      char buffer[16];
      sprintf(buffer, "%u",seats[(i-1)* num_cols + j-1]);
      
      if (print_str(out_fd, buffer)) {
        perror("Error writing to file descriptor");
        return 1;
      }

      if (j < num_cols) {
        if (print_str(out_fd, " ")) {
          perror("Error writing to file descriptor");
          return 1;
        }
      }
    }

    if (print_str(out_fd, "\n")) {
      perror("Error writing to file descriptor");
      return 1;
    }
  }

  return 0;
}


/*A função escreve no ficheiro .out da diretoria jobs, o equivalente ao list*/
int list_write(int out_fd, size_t num_events, unsigned int event_ids[num_events]) {
    
  if (num_events == 0) {
    char buff[] = "No events\n";
    if (print_str(out_fd, buff)) {
        perror("Error writing to file descriptor");
        return 1;
    }
    return 1;
  }

  for (size_t i = 0; i < num_events; ++i) {
    char buff[16];
    sprintf(buff, "Event: %u\n", event_ids[i]);

    if (print_str(out_fd, buff)) {
        perror("Error writing to file descriptor");
        return 1;
    }
  }
  return 0;
}


int ems_setup(char const* req_pipe_path, char const* resp_pipe_path, char const* server_pipe_path) {
  
  if ((unlink(req_pipe_path) != 0 || unlink(resp_pipe_path)) && errno != ENOENT) {
    fprintf(stderr, "Error creating named pipe: %s\n", strerror(errno));
    exit(EXIT_FAILURE);
  }
  
  if(mkfifo(req_pipe_path, 0640) != 0 || mkfifo(resp_pipe_path, 0640) != 0) {
    fprintf(stderr, "Error creating named pipe: %s\n", strerror(errno));
    exit(EXIT_FAILURE);
  }

  int server_fd=open(server_pipe_path, O_WRONLY);
  if(server_fd==-1){
    fprintf(stderr, "Error opening server pipe: %s\n", strerror(errno));
    exit(EXIT_FAILURE);
  }
  

  global_req_pipe_path = req_pipe_path;
  global_resp_pipe_path = resp_pipe_path;

  /*envia a mensagem de "setup" toda junta para o servidor*/
  char message[81];

  message[0]='1';
  memset(message + 1, '\0', MAX_PIPE_NAME_LENGTH);
  memset(message + 41, '\0', MAX_PIPE_NAME_LENGTH);


  strncpy(message+1, req_pipe_path, MAX_PIPE_NAME_LENGTH);
  strncpy(message+41, resp_pipe_path ,MAX_PIPE_NAME_LENGTH);

  ssize_t conf=write(server_fd, message, sizeof(message));
  if (conf == -1) {
      perror("Erro ao enviar pedido de criacao de sessao para o servidor");
      return 1;
  }

  /*abre o pipe de resposta*/
  response_fd = open(resp_pipe_path, O_RDONLY);
  if (response_fd == -1) {
      perror("Erro ao abrir named pipe de resposta pelo cliente");
      return 1; 
  }
  
  /*ler o session_id do servidor do pipe de resposta e atribui ao session id*/
  read(response_fd, &session_id, sizeof(int));
  
  request_fd = open(req_pipe_path, O_WRONLY);
  if (request_fd == -1) {
      perror("Erro ao abrir named pipe de pedido pelo cliente");
      return 1; 
  }
  return 0;
}

int ems_quit(void) { 
  char op_code = '2';
  ssize_t bytes_written;

  bytes_written = write(request_fd, &op_code, sizeof(char));
  if (bytes_written != sizeof(char)){
    perror("Erro ao enviar pedido de terminacao de sessao para o servidor");
    return 1;
  }
  
  if (response_fd != -1) {
    close(response_fd);
  }
  if (request_fd != -1) {
    close(request_fd);
  }

  if (unlink(global_req_pipe_path) != 0 || unlink(global_resp_pipe_path) != 0) {
    return 1; 
  }

  printf("Client terminated.\n");

  return 0;
}



int ems_create(unsigned int event_id, size_t num_rows, size_t num_cols) {

  if (request_fd == -1 || response_fd == -1) {
    perror("Erro ao abrir named pipes pelo cliente");
    return 1; 
  }

  char op_code = '3';
  ssize_t bytes_written;
  /*enviar opcode*/
  size_t written=0;
  while(written<sizeof(char)){
    bytes_written = write(request_fd, &op_code, sizeof(char));
    check_write_create(bytes_written);
    written+=(size_t)bytes_written;
  }
  /*enviar event_id*/
  written=0;
  while(written<sizeof(unsigned int)){
    bytes_written = write(request_fd, &event_id, sizeof(unsigned int));
    check_write_create(bytes_written);
    written+=(size_t)bytes_written;
  }
  /*enviar num_rows*/
  written=0;
  while(written<sizeof(size_t)){
    bytes_written = write(request_fd, &num_rows, sizeof(size_t));
    check_write_create(bytes_written);
    written+=(size_t)bytes_written;
  }

  /*enviar num_cols*/
  written=0;
  while(written<sizeof(size_t)){
    bytes_written = write(request_fd, &num_cols, sizeof(size_t));
    check_write_create(bytes_written);
    written+=(size_t)bytes_written;
  }

  /*ler resposta do cliente*/
  int response;
  size_t readed=0;
  while(readed<sizeof(int)){
    ssize_t bytes_read=read(response_fd, &response, sizeof(int));
    check_read(bytes_read);
    readed+=(size_t)bytes_read;
  }

  if(response==0){
    return 0;
  }
  else{
    return 1;
  }
}



int ems_reserve(unsigned int event_id, size_t num_seats, size_t* xs, size_t* ys) {

  char op_code = '4';

  /*envia o op_code*/
  ssize_t bytes_written = write(request_fd, &op_code, sizeof(char));
  if (bytes_written != sizeof(char)){
    perror("Erro ao enviar pedido de reserva de lugares para o servidor");
    return 1;
  }

  /*envia o event_id*/
  bytes_written = write(request_fd, &event_id, sizeof(unsigned int));
  if (bytes_written != sizeof(unsigned int)){
    perror("Erro ao enviar pedido de reserva de lugares para o servidor");
    return 1; 
  }

  /*envia o num_seats com um write*/
  bytes_written = write(request_fd, &num_seats, sizeof(size_t));
  if (bytes_written != sizeof(size_t)){
    perror("Erro ao enviar pedido de reserva de lugares para o servidor");
    return 1; 
  }

  /*envia o xs com um write*/
  write(request_fd, xs, sizeof(size_t)*num_seats);

  /*envia o ys com um write*/
  write(request_fd, ys, sizeof(size_t)*num_seats);

  int response;
  ssize_t bytes_read = read(response_fd, &response, sizeof(int));
  if (bytes_read != sizeof(int)){
    perror("Erro ao ler a mensagem do servidor");
    exit(EXIT_FAILURE); 
  }

  if(response==1){
    return 1;
  }
  else{
    return 0;
  }
}


int ems_show(int out_fd, unsigned int event_id) {
  char op_code = '5';

  /*write do op_code para o server atraves do request_fd*/
  ssize_t bytes_written = write(request_fd, &op_code, sizeof(char));
  if (bytes_written == -1) {
      perror("Erro ao enviar pedido de visualizacao de evento para o servidor");
      return 1; 
  }


  /*write do event_id para o server atraves do request_fd*/
  bytes_written = write(request_fd, &event_id, sizeof(unsigned int));
  if (bytes_written == -1) {
      perror("Erro ao enviar pedido de visualizacao de evento para o servidor");
      return 1; 
  }

  int retorno;

  ssize_t bytes_read = read(response_fd, &retorno, sizeof(int));
  if (bytes_read == -1) {
      perror("Erro ao ler a mensagem do servidor");
      exit(EXIT_FAILURE); 
  }

  if (retorno == 1) {
    return 1;
  }
  else{
    /*dar read do num_rows que vem do response_fd*/
    size_t num_rows;
    bytes_read = read(response_fd, &num_rows, sizeof(size_t));
    if (bytes_read == -1) {
        perror("Erro ao ler a mensagem do servidor");
        exit(EXIT_FAILURE); 
    }

    /*dar read do num_cols que vem do response_fd*/
    size_t num_cols;
    bytes_read = read(response_fd, &num_cols, sizeof(size_t));
    if (bytes_read == -1) {
        perror("Erro ao ler a mensagem do servidor");
        exit(EXIT_FAILURE); 
    }


    unsigned int seats[num_rows * num_cols];
    bytes_read = read(response_fd, seats, sizeof(unsigned int)*num_rows*num_cols);
    if (bytes_read == -1) {
        perror("Erro ao ler a mensagem do servidor");
        exit(EXIT_FAILURE);
    }
    int show_write_ok;

    show_write_ok=show_write(out_fd, num_rows, num_cols, seats);
    if(show_write_ok==0){
      return 0;
    }
    else{
      return 1;
    }
  }
}



int ems_list_events(int out_fd) {
  char opcode = '6';

  ssize_t bytes_written;
  size_t written=0;

  written=0;
  while(written<sizeof(char)){
    bytes_written = write(request_fd, &opcode, sizeof(char));
    check_write_create(bytes_written);
    written+=(size_t)bytes_written;
  }

  int retorno;
  size_t readed=0;
  while(readed<sizeof(int)){
    ssize_t bytes_read=read(response_fd, &retorno, sizeof(int));
    check_read(bytes_read);
    readed+=(size_t)bytes_read;
  }
  if(retorno==1){
    return 1;
  }else{
    size_t num_events;
    readed=0;
    ssize_t bytes_read;
    while(readed<sizeof(size_t)){
      bytes_read=read(response_fd, &num_events, sizeof(size_t));
      check_read(bytes_read);
      readed+=(size_t)bytes_read;
    }
    
    unsigned int event_ids[num_events];
    for(size_t i=0; i<num_events; i++){
      readed=0;
      while(readed<sizeof(unsigned int)){
        bytes_read=read(response_fd, &(event_ids[i]), sizeof(unsigned int));
        check_read(bytes_read);
        readed+=(size_t)bytes_read;
      }
    }

    if(list_write(out_fd, num_events, event_ids)){
      return 1;
    }
    return 0;
  }

}

