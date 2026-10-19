/*
 * colector.c
 *
 * Uso:
 *   ./colector <cant_sensores> <mediciones_por_sensor> [cant_workers]
 */

#include <fcntl.h>
#include <mqueue.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "comun.h"

/* Argumentos que recibe cada worker thread al crearse con pthread_create()*/
typedef struct {
  int id_worker;
  mqd_t cola;
  memoria_compartida_t *shm;
  sem_t *mutex;
  unsigned long procesadas;
} args_worker_t;

static void limpiar_recursos_previos(void);
static void inicializar_memoria_compartida(memoria_compartida_t *shm,
                                           int num_sensores,
                                           unsigned long mediciones_por_sensor);
static void actualizar_estadisticas(memoria_compartida_t *shm,
                                    const medicion_msg_t *m);
static void *rutina_worker(void *arg);
static void imprimir_resumen(const memoria_compartida_t *shm);

int main(int argc, char *argv[]) {
  int num_sensores;
  long mediciones_temp;
  unsigned long mediciones_por_sensor;
  int num_workers;
  unsigned long total_esperadas;
  int i;
  int fd_shm;
  mqd_t cola;
  struct mq_attr attr;
  sem_t *mutex;
  memoria_compartida_t *shm;
  pid_t *pids_sensores;
  pthread_t *hilos;
  args_worker_t *args_workers;
  medicion_msg_t pildora;
  pid_t pid;
  int rc;
  int estado;
  char id_str[32];
  char cant_str[32];
  char *argv_sensor[4];

  /* ---------------------------------------------------------------
   * Validacion de argumentos de linea de comandos.
   * --------------------------------------------------------------- */
  if (argc < 3 || argc > 4) {
    fprintf(stderr,
            "Uso: %s <cant_sensores> <mediciones_por_sensor> [cant_workers]\n",
            argv[0]);
    fprintf(stderr, "  cant_sensores:           entre 1 y %d\n", MAX_SENSORES);
    fprintf(stderr, "  mediciones_por_sensor:   cantidad de mediciones que "
                    "genera cada sensor\n");
    fprintf(stderr, "  cant_workers (opcional): entre 1 y %d (default %d)\n",
            MAX_WORKERS, DEFAULT_NUM_WORKERS);
    exit(EXIT_FAILURE);
  }

  /* atoi/atol devuelven 0 si la cadena no es un numero valido */
  num_sensores = atoi(argv[1]);
  if (num_sensores < 1 || num_sensores > MAX_SENSORES) {
    fprintf(stderr, "%s: cant_sensores invalida (debe ser entre 1 y %d)\n",
            argv[0], MAX_SENSORES);
    exit(EXIT_FAILURE);
  }

  mediciones_temp = atol(argv[2]);
  if (mediciones_temp < 1) {
    fprintf(stderr, "%s: mediciones_por_sensor invalida (debe ser >= 1)\n",
            argv[0]);
    exit(EXIT_FAILURE);
  }
  mediciones_por_sensor = (unsigned long)mediciones_temp;

  num_workers = DEFAULT_NUM_WORKERS;
  if (argc == 4) {
    num_workers = atoi(argv[3]);
    if (num_workers < 1 || num_workers > MAX_WORKERS) {
      fprintf(stderr, "%s: cant_workers invalida (debe ser entre 1 y %d)\n",
              argv[0], MAX_WORKERS);
      exit(EXIT_FAILURE);
    }
  }

  total_esperadas = (unsigned long)num_sensores * mediciones_por_sensor;

  srand((unsigned int)time(NULL) ^ (unsigned int)getpid());

  printf("=== Colector iniciado (PID %ld) ===\n", (long)getpid());
  printf("Sensores: %d | Mediciones por sensor: %lu | Total esperado: %lu | "
         "Workers: %d\n\n",
         num_sensores, mediciones_por_sensor, total_esperadas, num_workers);

  /* ---------------------------------------------------------------
   * Limpieza de recursos IPC
   * --------------------------------------------------------------- */
  limpiar_recursos_previos();

  /* ---------------------------------------------------------------
   * Creacion de la cola de mensajes POSIX (mq_mediciones).
   * --------------------------------------------------------------- */
  attr.mq_flags = 0;
  attr.mq_maxmsg = 10;
  attr.mq_msgsize = (long)sizeof(medicion_msg_t);
  attr.mq_curmsgs = 0;

  cola = mq_open(MQ_NOMBRE, O_CREAT | O_EXCL | O_RDWR, 0600, &attr);
  if (cola == (mqd_t)-1) {
    fallar("mq_open (colector)");
  }

  /* ---------------------------------------------------------------
   * Creacion del segmento de memoria compartida de estadisticas.
   * (shm_open -> ftruncate -> mmap -> close(fd) -> ... -> munmap -> shm_unlink)
   * --------------------------------------------------------------- */
  fd_shm = shm_open(SHM_NOMBRE, O_CREAT | O_EXCL | O_RDWR, 0644);
  if (fd_shm == -1) {
    fallar("shm_open (colector)");
  }
  if (ftruncate(fd_shm, (off_t)sizeof(memoria_compartida_t)) == -1) {
    fallar("ftruncate");
  }
  shm = (memoria_compartida_t *)mmap(NULL, sizeof(memoria_compartida_t),
                                     PROT_READ | PROT_WRITE, MAP_SHARED, fd_shm,
                                     0);
  if (shm == MAP_FAILED) {
    fallar("mmap");
  }
  if (close(fd_shm) == -1) {
    perror("close fd_shm");
  }

  inicializar_memoria_compartida(shm, num_sensores, mediciones_por_sensor);

  /* ---------------------------------------------------------------
   * Creacion del semaforo binario (mutex) que protege la shm.
   * Valor inicial = 1 (libre)
   * --------------------------------------------------------------- */
  mutex = sem_open(SEM_NOMBRE, O_CREAT | O_EXCL, 0644, 1);
  if (mutex == SEM_FAILED) {
    fallar("sem_open (colector)");
  }

  /* ---------------------------------------------------------------
   * Creacion de los procesos sensores (fork + exec).
   * --------------------------------------------------------------- */
  pids_sensores = (pid_t *)malloc((size_t)num_sensores * sizeof(pid_t));
  if (pids_sensores == NULL) {
    fallar("malloc pids_sensores");
  }

  for (i = 0; i < num_sensores; i++) {
    pid = fork();
    if (pid < 0) {
      fallar("fork");
    } else if (pid == 0) {
      /* Proceso hijo: se reemplaza a si mismo por el binario "sensor" (fork +
       * exec es el patron clasico de Unix para crear un nuevo proceso que corre
       * otro programa). */
      sprintf(id_str, "%d", i + 1);
      sprintf(cant_str, "%lu", mediciones_por_sensor);
      argv_sensor[0] = (char *)"./sensor";
      argv_sensor[1] = id_str;
      argv_sensor[2] = cant_str;
      argv_sensor[3] = NULL;

      execv("./sensor", argv_sensor);
      /* Si execv() retorna, es porque fallo. */
      perror("execv sensor");
      _exit(EXIT_FAILURE);
    } else {
      pids_sensores[i] = pid;
    }
  }

  /* ---------------------------------------------------------------
   * Creacion del pool de worker threads (fan-out sobre la cola).
   *
   * Todos los workers comparten el MISMO descriptor de cola: POSIX
   * garantiza que mq_receive() es atomico, asi que el kernel reparte
   * los mensajes entre los hilos sin que haga falta un mutex propio
   * para el "dequeue" (el mutex de la shm protege otra cosa: la
   * ACTUALIZACION de las estadisticas, no la lectura de la cola).
   * --------------------------------------------------------------- */
  hilos = (pthread_t *)malloc((size_t)num_workers * sizeof(pthread_t));
  args_workers =
      (args_worker_t *)malloc((size_t)num_workers * sizeof(args_worker_t));
  if (hilos == NULL || args_workers == NULL) {
    fallar("malloc workers");
  }

  for (i = 0; i < num_workers; i++) {
    args_workers[i].id_worker = i + 1;
    args_workers[i].cola = cola;
    args_workers[i].shm = shm;
    args_workers[i].mutex = mutex;
    args_workers[i].procesadas = 0;

    rc = pthread_create(&hilos[i], NULL, rutina_worker, &args_workers[i]);
    if (rc != 0) {
      fprintf(stderr, "Error al crear worker %d: %s\n", i + 1, strerror(rc));
      exit(EXIT_FAILURE);
    }
  }

  /* ---------------------------------------------------------------
   * Esperar a que los sensores terminen de enviar sus mediciones.
   *
   * Esto NO bloquea el procesamiento: los workers ya estan
   * consumiendo la cola en paralelo, en otros hilos, mientras el
   * hilo principal espera aca a los procesos hijo con waitpid().
   * --------------------------------------------------------------- */
  for (i = 0; i < num_sensores; i++) {
    if (waitpid(pids_sensores[i], &estado, 0) == -1) {
      perror("waitpid");
      continue;
    }
    if (!WIFEXITED(estado) || WEXITSTATUS(estado) != 0) {
      fprintf(stderr, "Advertencia: el sensor %d termino de forma anormal\n",
              i + 1);
    }
  }
  printf("\nTodos los sensores (%d) terminaron de enviar sus mediciones.\n",
         num_sensores);

  /* ---------------------------------------------------------------
   * Enviar una "poison pill" por cada worker.
   * --------------------------------------------------------------- */
  pildora.sensor_id = ID_PILDORA_VENENO;
  pildora.valor = 0.0;
  pildora.timestamp = (long)time(NULL);

  for (i = 0; i < num_workers; i++) {
    if (mq_send(cola, (const char *)&pildora, sizeof(pildora), 0) == -1) {
      fallar("mq_send (pildora)");
    }
  }
  /* ---------------------------------------------------------------
   * Esperar a que todos los workers terminen.
   * --------------------------------------------------------------- */
  for (i = 0; i < num_workers; i++) {
    pthread_join(hilos[i], NULL);
    printf("Worker %d proceso %lu mediciones.\n", i + 1,
           args_workers[i].procesadas);
  }

  /* ---------------------------------------------------------------
   * Marcar el procesamiento como completo y mostrar el resumen.
   * --------------------------------------------------------------- */
  if (sem_wait(mutex) == -1) {
    fallar("sem_wait (colector resumen)");
  }
  shm->global.procesamiento_completo = 1;
  if (sem_post(mutex) == -1) {
    fallar("sem_post (colector resumen)");
  }

  imprimir_resumen(shm);
  /* ---------------------------------------------------------------
   * Terminacion normal: liberar TODOS los recursos IPC.
   * --------------------------------------------------------------- */
  free(pids_sensores);
  free(hilos);
  free(args_workers);

  if (munmap(shm, sizeof(memoria_compartida_t)) == -1) {
    perror("munmap (colector)");
  }
  if (shm_unlink(SHM_NOMBRE) == -1) {
    perror("shm_unlink (colector)");
  }

  if (mq_close(cola) == -1) {
    perror("mq_close (colector)");
  }
  if (mq_unlink(MQ_NOMBRE) == -1) {
    perror("mq_unlink (colector)");
  }

  if (sem_close(mutex) == -1) {
    perror("sem_close (colector)");
  }
  if (sem_unlink(SEM_NOMBRE) == -1) {
    perror("sem_unlink (colector)");
  }

  printf("\nRecursos IPC liberados. Colector finalizado normalmente.\n");
  return 0;
}

static void limpiar_recursos_previos(void) {
  mq_unlink(MQ_NOMBRE);
  shm_unlink(SHM_NOMBRE);
  sem_unlink(SEM_NOMBRE);
}

static void
inicializar_memoria_compartida(memoria_compartida_t *shm, int num_sensores,
                               unsigned long mediciones_por_sensor) {
  int i;
  memset(shm, 0, sizeof(memoria_compartida_t));

  shm->global.num_sensores_activos = num_sensores;
  shm->global.mediciones_por_sensor = mediciones_por_sensor;
  shm->global.total_esperadas =
      (unsigned long)num_sensores * mediciones_por_sensor;
  shm->global.total_procesadas = 0;
  shm->global.suma_global = 0.0;
  shm->global.minimo_global = 0.0;
  shm->global.maximo_global = 0.0;
  shm->global.procesamiento_completo = 0;

  for (i = 0; i < num_sensores; i++) {
    shm->sensores[i].sensor_id = i + 1;
    shm->sensores[i].cantidad = 0;
    shm->sensores[i].suma = 0.0;
    shm->sensores[i].minimo = 0.0;
    shm->sensores[i].maximo = 0.0;
    shm->sensores[i].ultimo_valor = 0.0;
  }
}

static void actualizar_estadisticas(memoria_compartida_t *shm,
                                    const medicion_msg_t *m) {
  estadistica_sensor_t *es;
  int idx;

  if (m->sensor_id < 1 || m->sensor_id > shm->global.num_sensores_activos) {
    return;
  }

  idx = m->sensor_id - 1;
  es = &shm->sensores[idx];

  if (es->cantidad == 0) {
    es->minimo = m->valor;
    es->maximo = m->valor;
  } else {
    if (m->valor < es->minimo) {
      es->minimo = m->valor;
    }
    if (m->valor > es->maximo) {
      es->maximo = m->valor;
    }
  }
  es->cantidad++;
  es->suma += m->valor;
  es->ultimo_valor = m->valor;

  if (shm->global.total_procesadas == 0) {
    shm->global.minimo_global = m->valor;
    shm->global.maximo_global = m->valor;
  } else {
    if (m->valor < shm->global.minimo_global) {
      shm->global.minimo_global = m->valor;
    }
    if (m->valor > shm->global.maximo_global) {
      shm->global.maximo_global = m->valor;
    }
  }
  shm->global.total_procesadas++;
  shm->global.suma_global += m->valor;
}

/* Cuerpo de cada worker thread: consume mensajes de la cola en un
 * bucle infinito hasta recibir su poison pill. */
static void *rutina_worker(void *arg) {
  args_worker_t *a;
  medicion_msg_t msg;
  ssize_t leidos;

  a = (args_worker_t *)arg;

  for (;;) {
    leidos = mq_receive(a->cola, (char *)&msg, sizeof(msg), NULL);
    if (leidos == -1) {
      perror("mq_receive (worker)");
      continue;
    }

    if (msg.sensor_id == ID_PILDORA_VENENO) {
      break; /* No hay mas datos: terminar el hilo. */
    }

    if (sem_wait(a->mutex) == -1) {
      perror("sem_wait (worker)");
      continue;
    }
    actualizar_estadisticas(a->shm, &msg);
    if (sem_post(a->mutex) == -1) {
      perror("sem_post (worker)");
    }

    a->procesadas++;
  }

  return NULL;
}

static void imprimir_resumen(const memoria_compartida_t *shm) {
  int i;
  double promedio_global;
  const estadistica_sensor_t *es;
  double promedio;

  printf("\n=== Resumen final de estadisticas ===\n");
  for (i = 0; i < shm->global.num_sensores_activos; i++) {
    es = &shm->sensores[i];
    promedio = (es->cantidad > 0) ? (es->suma / (double)es->cantidad) : 0.0;

    printf("  Sensor %2d | cant=%5lu | prom=%8.3f | min=%8.3f | max=%8.3f | "
           "ultimo=%8.3f\n",
           es->sensor_id, es->cantidad, promedio, es->minimo, es->maximo,
           es->ultimo_valor);
  }

  promedio_global =
      (shm->global.total_procesadas > 0)
          ? (shm->global.suma_global / (double)shm->global.total_procesadas)
          : 0.0;
  printf("  ---------------------------------------------------------------\n");
  printf("  GLOBAL    | cant=%5lu | prom=%8.3f | min=%8.3f | max=%8.3f\n",
         shm->global.total_procesadas, promedio_global,
         shm->global.minimo_global, shm->global.maximo_global);
}
