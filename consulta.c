/*
 * consulta.c
 *
 * Segundo proceso del sistema, totalmente independiente del colector,
 * que se ADJUNTA a la memoria compartida ya existente y muestra las
 * estadisticas actuales sin modificarlas y sin frenar al colector
 * (toma el semaforo el tiempo minimo indispensable: solo para copiar
 * la foto de los datos, no para imprimirla en pantalla).
 *
 * Uso:
 *   ./consulta                 -> una unica foto y termina
 *   ./consulta <intervalo_seg> -> modo "watch" cada N segundos
 */
#include <fcntl.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include "comun.h"

static void mostrar_estadisticas(const memoria_compartida_t *snapshot);

int main(int argc, char *argv[]) {
  int fd_shm;
  memoria_compartida_t *shm;
  memoria_compartida_t snapshot;
  sem_t *mutex;
  int intervalo;

  intervalo = 0;
  if (argc == 2) {
    intervalo = atoi(argv[1]);
    if (intervalo < 1) {
      fprintf(stderr, "%s: intervalo invalido (debe ser >= 1 segundos)\n",
              argv[0]);
      exit(EXIT_FAILURE);
    }
  } else if (argc > 2) {
    fprintf(stderr, "Uso: %s [intervalo_segundos]\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  /* Se adjunta a la memoria compartida EXISTENTE. */
  fd_shm = shm_open(SHM_NOMBRE, O_RDONLY, 0);
  if (fd_shm == -1) {
    fprintf(stderr, "No se pudo conectar a la memoria compartida (%s).\n",
            SHM_NOMBRE);
    fprintf(stderr, "Verificar que el colector este corriendo.\n");
    fallar("shm_open (consulta)");
  }

  shm = (memoria_compartida_t *)mmap(NULL, sizeof(memoria_compartida_t),
                                     PROT_READ, MAP_SHARED, fd_shm, 0);
  if (shm == MAP_FAILED) {
    fallar("mmap (consulta)");
  }
  if (close(fd_shm) == -1) {
    perror("close fd_shm (consulta)");
  }

  /* Se adjunta tambien al semaforo existente */
  mutex = sem_open(SEM_NOMBRE, 0);
  if (mutex == SEM_FAILED) {
    fallar("sem_open (consulta)");
  }

  do {
    /* Seccion critica: solo se copia el struct completo a una variable local*/
    if (sem_wait(mutex) == -1) {
      fallar("sem_wait (consulta)");
    }
    snapshot = *shm;
    if (sem_post(mutex) == -1) {
      fallar("sem_post (consulta)");
    }

    mostrar_estadisticas(&snapshot);

    if (intervalo > 0 && !snapshot.global.procesamiento_completo) {
      printf("\n(actualizando cada %d segundo/s, Ctrl+C para salir)\n",
             intervalo);
      sleep((unsigned int)intervalo);
    }
  } while (intervalo > 0 && !snapshot.global.procesamiento_completo);

  if (sem_close(mutex) == -1) {
    perror("sem_close (consulta)");
  }
  if (munmap(shm, sizeof(memoria_compartida_t)) == -1) {
    perror("munmap (consulta)");
  }

  return 0;
}

static void mostrar_estadisticas(const memoria_compartida_t *snapshot) {
  int i;
  double promedio_global;
  const estadistica_sensor_t *es;
  double promedio;

  printf("=== Estadisticas actuales (consulta) ===\n");
  printf("Estado: %s | Procesadas: %lu / %lu\n",
         snapshot->global.procesamiento_completo ? "COMPLETO" : "EN CURSO",
         snapshot->global.total_procesadas, snapshot->global.total_esperadas);

  for (i = 0; i < snapshot->global.num_sensores_activos; i++) {
    es = &snapshot->sensores[i];
    promedio = (es->cantidad > 0) ? (es->suma / (double)es->cantidad) : 0.0;

    printf("  Sensor %2d | cant=%5lu | prom=%8.3f | min=%8.3f | max=%8.3f | "
           "ultimo=%8.3f\n",
           es->sensor_id, es->cantidad, promedio, es->minimo, es->maximo,
           es->ultimo_valor);
  }

  promedio_global = (snapshot->global.total_procesadas > 0)
                        ? (snapshot->global.suma_global /
                           (double)snapshot->global.total_procesadas)
                        : 0.0;
  printf("  ---------------------------------------------------------------\n");
  printf("  GLOBAL    | cant=%5lu | prom=%8.3f | min=%8.3f | max=%8.3f\n",
         snapshot->global.total_procesadas, promedio_global,
         snapshot->global.minimo_global, snapshot->global.maximo_global);
}
