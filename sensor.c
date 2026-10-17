/*
 * sensor.c
 *
 * Proceso "sensor": simula un sensor fisico de un sistema SCADA.
 * Genera una cantidad fija de mediciones pseudoaleatorias y las envia, una por
 * una, a traves de la cola de mensajes POSIX que ya fue creada por el colector.
 *
 * Uso:
 *   ./sensor <id_sensor> <cantidad_mediciones>
 *
 * Este ejecutable no se corre a mano en el uso normal del sistema: lo lanza el
 * colector mediante fork() + execv() (uno por cada sensor configurado por linea
 * de comandos).
 */

#include <fcntl.h>
#include <mqueue.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "comun.h"

/* Rango de valores simulados (a modo de ejemplo: temperatura en C) */
#define VALOR_MINIMO 0.0
#define VALOR_MAXIMO 100.0

/* Retardo maximo, en microsegundos, entre una medicion y la siguiente. Simula
 * el tiempo real que tardaria un sensor fisico en volver a muestrear, y ademas
 * da tiempo a que el cliente "consulta" pueda observar estadisticas parciales
 * mientras el sistema corre. */
#define RETARDO_MAX_US 40000 /* 40 ms */

int main(int argc, char *argv[]) {
  int id_sensor;
  long cant_temp;
  unsigned long cantidad;
  unsigned long i;
  mqd_t cola;
  medicion_msg_t msg;

  if (argc != 3) {
    fprintf(stderr, "Uso: %s <id_sensor> <cantidad_mediciones>\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  id_sensor = atoi(argv[1]);
  cant_temp = atol(argv[2]);
  if (id_sensor < 1 || id_sensor > MAX_SENSORES || cant_temp < 1) {
    fprintf(
        stderr,
        "%s: parametros invalidos (id_sensor entre 1 y %d, cantidad >= 1)\n",
        argv[0], MAX_SENSORES);
    exit(EXIT_FAILURE);
  }
  cantidad = (unsigned long)cant_temp;

  /* Semilla propia por proceso. Cada sensor es un proceso distinto (con su
   * propio PID) */
  srand((unsigned int)time(NULL) ^ (unsigned int)getpid());

  /* El sensor solo escribe en la cola: la abre en modo O_WRONLY.
   * La cola ya existe (la creo el colector antes de hacer fork),
   * por eso NO se usa O_CREAT */
  cola = mq_open(MQ_NOMBRE, O_WRONLY);
  if (cola == (mqd_t)-1) {
    fallar("mq_open (sensor)");
  }

  for (i = 0; i < cantidad; i++) {
    msg.sensor_id = id_sensor;
    msg.valor = valor_aleatorio(VALOR_MINIMO, VALOR_MAXIMO);
    msg.timestamp = (long)time(NULL);

    /* mq_send es una llamada al sistema atomica: el kernel es el
     * unico que garantiza que el mensaje se encola completo, sin
     * que se mezcle con el de otro sensor concurrente */
    if (mq_send(cola, (const char *)&msg, sizeof(msg), 0) == -1) {
      fallar("mq_send (sensor)");
    }

    usleep((useconds_t)(rand() % RETARDO_MAX_US));
  }

  if (mq_close(cola) == -1) {
    perror("mq_close (sensor)");
  }

  printf("Sensor %d: envio %lu mediciones.\n", id_sensor, cantidad);

  return 0;
}
