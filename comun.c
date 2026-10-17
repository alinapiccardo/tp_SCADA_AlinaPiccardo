/*
 * comun.c
 */
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
