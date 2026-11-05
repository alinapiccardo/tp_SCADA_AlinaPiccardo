/*
 * comun.c
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "comun.h"

void fallar(const char *contexto) {
  perror(contexto);
  exit(EXIT_FAILURE);
}

double valor_aleatorio(double minimo, double maximo) {
  double proporcion;

  proporcion = (double)rand() / (double)RAND_MAX;
  return minimo + proporcion * (maximo - minimo);
}

/* Una senial (SIGALRM, SIGUSR1, SIGINT...) puede llegar mientras un thread
 * esta bloqueado en sem_wait(). En ese caso la llamada puede volver con -1 y
 * errno = EINTR sin haber tomado el semaforo: no es un error real, hay que
 * volver a intentar. */
int tomar_semaforo(sem_t *sem) {
  while (sem_wait(sem) == -1) {
    if (errno != EINTR) {
      return -1;
    }
  }
  return 0;
}
