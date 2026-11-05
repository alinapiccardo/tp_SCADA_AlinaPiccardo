/*
 * colector.c
 *
 * Proceso principal del sistema de monitoreo.
 *
 * Uso:
 *   ./colector <cant_sensores> <mediciones_por_sensor> [cant_workers]
 *              [intervalo_reporte] [umbral_alerta]
 */

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
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

/* Cada cuanto el hilo principal revisa las banderas de señales y si algun
 * sensor termino. */
#define PERIODO_SUPERVISION_US 100000 /* 100 ms */

/* ------------------------------------------------------------------ */
/* Banderas modificadas por los handlers.                             */
/* ------------------------------------------------------------------ */
static volatile sig_atomic_t senial_terminar = 0; /* SIGINT / SIGTERM */
static volatile sig_atomic_t senial_reporte = 0;  /* SIGALRM          */
static volatile sig_atomic_t senial_volcado = 0;  /* SIGUSR1          */

typedef struct {
  int id_worker;
  mqd_t cola;
  mqd_t cola_alertas;
  memoria_compartida_t *shm;
  sem_t *sem_shm;
  pthread_mutex_t *mutex_contadores;
  double umbral;
  /* Contadores propios del worker. Los escribe el worker y los lee el hilo
   * principal en los reportes (SIGALRM/SIGUSR1) mientras el worker sigue
   * corriendo: por eso se protegen con mutex_contadores. */
  unsigned long procesadas;
  unsigned long alertas;
} args_worker_t;

/* Argumentos del thread logger (unico consumidor de la cola de alertas). */
typedef struct {
  mqd_t cola_alertas;
  FILE *log;
  double umbral;
  unsigned long registradas;
} args_logger_t;

typedef struct {
  memoria_compartida_t *shm;
  sem_t *sem_shm;
  pthread_mutex_t *mutex_contadores;
  args_worker_t *workers;
  int num_workers;
  time_t inicio;
} contexto_reporte_t;

/* ------------------------------------------------------------------ */
/* Estado global minimo para poder limpiar ante un error fatal.       */
/* Solo lo usa el hilo principal.                                     */
/* ------------------------------------------------------------------ */
static pid_t *g_pids_sensores = NULL;
static int *g_sensor_recogido = NULL;
static int g_sensores_lanzados = 0;

static void manejador_terminar(int sig);
static void manejador_reporte(int sig);
static void manejador_volcado(int sig);
static void registrar_seniales(void);

static void limpiar_nombres_ipc(void);
static void abortar(const char *contexto);
static int enviar_mensaje(mqd_t cola, const char *msg, size_t tam);
static int recolectar_sensor(int bloquear);
static void detener_sensores(void);

static void inicializar_memoria_compartida(memoria_compartida_t *shm,
                                           int num_sensores,
                                           unsigned long mediciones_por_sensor,
                                           double umbral);
static void actualizar_estadisticas(memoria_compartida_t *shm,
                                    const medicion_msg_t *m, int es_alerta);
static void *rutina_worker(void *arg);
static void *rutina_logger(void *arg);

static void tomar_foto(const contexto_reporte_t *ctx,
                       memoria_compartida_t *foto, unsigned long *procesadas,
                       unsigned long *alertas);
static void escribir_estadisticas(FILE *f, const char *titulo,
                                  const memoria_compartida_t *foto,
                                  const unsigned long *procesadas,
                                  const unsigned long *alertas,
                                  int num_workers);
static void imprimir_reporte_periodico(const contexto_reporte_t *ctx);
static void volcar_estadisticas(const contexto_reporte_t *ctx);
static void escribir_resumen_csv(const memoria_compartida_t *foto,
                                 int num_workers, int interrumpido);

int main(int argc, char *argv[]) {
  int num_sensores;
  long mediciones_temp;
  unsigned long mediciones_por_sensor;
  int num_workers;
  int intervalo_reporte;
  double umbral;
  unsigned long total_esperadas;
  int i;
  int fd_shm;
  mqd_t cola;
  mqd_t cola_alertas;
  struct mq_attr attr;
  sem_t *sem_shm;
  memoria_compartida_t *shm;
  pthread_t *hilos;
  pthread_t hilo_logger;
  args_worker_t *args_workers;
  args_logger_t args_logger;
  pthread_mutex_t mutex_contadores;
  contexto_reporte_t ctx;
  memoria_compartida_t foto;
  unsigned long procesadas_w[MAX_WORKERS];
  unsigned long alertas_w[MAX_WORKERS];
  medicion_msg_t pildora;
  alerta_msg_t pildora_logger;
  FILE *log;
  pid_t pid;
  int rc;
  int sensores_vivos;
  int interrumpido;
  char id_str[32];
  char cant_str[32];
  char *argv_sensor[4];

  /* ---------------------------------------------------------------
   * Validacion de argumentos de linea de comandos.
   * --------------------------------------------------------------- */
  if (argc < 3 || argc > 6) {
    fprintf(stderr,
            "Uso: %s <cant_sensores> <mediciones_por_sensor> [cant_workers] "
            "[intervalo_reporte] [umbral_alerta]\n",
            argv[0]);
    fprintf(stderr, "  cant_sensores:                entre 1 y %d\n",
            MAX_SENSORES);
    fprintf(stderr, "  mediciones_por_sensor:        cantidad de mediciones "
                    "que genera cada sensor (>= 1)\n");
    fprintf(stderr,
            "  cant_workers (opcional):      entre 1 y %d (default %d)\n",
            MAX_WORKERS, DEFAULT_NUM_WORKERS);
    fprintf(stderr,
            "  intervalo_reporte (opcional): segundos entre reportes SIGALRM, "
            "entre 0 y %d (0 = sin reporte, default %d)\n",
            MAX_INTERVALO_REPORTE, DEFAULT_INTERVALO_REPORTE);
    fprintf(stderr,
            "  umbral_alerta (opcional):     valor entre %.1f y %.1f (sin "
            "incluir) por encima del cual se genera una alerta "
            "(default %.1f)\n",
            VALOR_MINIMO, VALOR_MAXIMO, DEFAULT_UMBRAL_ALERTA);
    exit(EXIT_FAILURE);
  }

  /* atoi/atol/atof devuelven 0 si la cadena no es un numero valido, por
   * eso siempre se valida el rango DESPUES de convertir. */
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
  if (argc >= 4) {
    num_workers = atoi(argv[3]);
    if (num_workers < 1 || num_workers > MAX_WORKERS) {
      fprintf(stderr, "%s: cant_workers invalida (debe ser entre 1 y %d)\n",
              argv[0], MAX_WORKERS);
      exit(EXIT_FAILURE);
    }
  }

  intervalo_reporte = DEFAULT_INTERVALO_REPORTE;
  if (argc >= 5) {
    /* "0" es un valor valido (desactiva el reporte), asi que ademas del
     * rango se exige que el texto empiece con un digito. */
    intervalo_reporte = atoi(argv[4]);
    if (argv[4][0] < '0' || argv[4][0] > '9' || intervalo_reporte < 0 ||
        intervalo_reporte > MAX_INTERVALO_REPORTE) {
      fprintf(stderr,
              "%s: intervalo_reporte invalido (debe ser entre 0 y %d "
              "segundos)\n",
              argv[0], MAX_INTERVALO_REPORTE);
      exit(EXIT_FAILURE);
    }
  }

  umbral = DEFAULT_UMBRAL_ALERTA;
  if (argc == 6) {
    umbral = atof(argv[5]);
    if (umbral <= VALOR_MINIMO || umbral >= VALOR_MAXIMO) {
      fprintf(stderr,
              "%s: umbral_alerta invalido (debe ser mayor que %.1f y menor "
              "que %.1f)\n",
              argv[0], VALOR_MINIMO, VALOR_MAXIMO);
      exit(EXIT_FAILURE);
    }
  }

  total_esperadas = (unsigned long)num_sensores * mediciones_por_sensor;

  /* ---------------------------------------------------------------
   * Registro de los handlers de señales.
   * --------------------------------------------------------------- */
  registrar_seniales();

  srand((unsigned int)time(NULL) ^ (unsigned int)getpid());

  printf("=== Colector iniciado (PID %ld) ===\n", (long)getpid());
  printf("Sensores: %d | Mediciones por sensor: %lu | Total esperado: %lu | "
         "Workers: %d\n",
         num_sensores, mediciones_por_sensor, total_esperadas, num_workers);
  if (intervalo_reporte > 0) {
    printf("Reporte periodico (SIGALRM): cada %d s", intervalo_reporte);
  } else {
    printf("Reporte periodico (SIGALRM): desactivado");
  }
  printf(" | Umbral de alerta: %.3f | Log: %s\n", umbral, ARCHIVO_LOG);
  printf("Seniales: kill -USR1 %ld (volcado) | Ctrl+C o kill -INT %ld "
         "(apagado ordenado)\n\n",
         (long)getpid(), (long)getpid());

  /* ---------------------------------------------------------------
   * Archivo de log de alertas. Se abre antes de crear cualquier
   * recurso IPC: si falla, no hay nada que limpiar todavia.
   * Modo "a" (append): cada corrida agrega una nueva sesion al final,
   * sin borrar el historial de las anteriores.
   * --------------------------------------------------------------- */
  log = fopen(ARCHIVO_LOG, "a");
  if (log == NULL) {
    fallar("fopen " ARCHIVO_LOG);
  }

  /* ---------------------------------------------------------------
   * Limpieza de recursos IPC
   * --------------------------------------------------------------- */
  limpiar_nombres_ipc();

  /* ---------------------------------------------------------------
   * Cola de mensajes de mediciones (sensores -> workers).
   * --------------------------------------------------------------- */
  attr.mq_flags = 0;
  attr.mq_maxmsg = MQ_MAX_MENSAJES;
  attr.mq_msgsize = (long)sizeof(medicion_msg_t);
  attr.mq_curmsgs = 0;

  cola = mq_open(MQ_NOMBRE, O_CREAT | O_EXCL | O_RDWR, 0600, &attr);
  if (cola == (mqd_t)-1) {
    abortar("mq_open mediciones (colector)");
  }

  /* ---------------------------------------------------------------
   * Cola de mensajes de alertas (workers -> logger).
   * --------------------------------------------------------------- */
  attr.mq_flags = 0;
  attr.mq_maxmsg = MQ_MAX_MENSAJES;
  attr.mq_msgsize = (long)sizeof(alerta_msg_t);
  attr.mq_curmsgs = 0;

  cola_alertas =
      mq_open(MQ_ALERTAS_NOMBRE, O_CREAT | O_EXCL | O_RDWR, 0600, &attr);
  if (cola_alertas == (mqd_t)-1) {
    abortar("mq_open alertas (colector)");
  }

  /* ---------------------------------------------------------------
   * Segmento de memoria compartida de estadisticas.
   * (shm_open -> ftruncate -> mmap -> close(fd) -> ... -> munmap -> shm_unlink)
   * --------------------------------------------------------------- */
  fd_shm = shm_open(SHM_NOMBRE, O_CREAT | O_EXCL | O_RDWR, 0644);
  if (fd_shm == -1) {
    abortar("shm_open (colector)");
  }
  if (ftruncate(fd_shm, (off_t)sizeof(memoria_compartida_t)) == -1) {
    abortar("ftruncate");
  }
  shm = (memoria_compartida_t *)mmap(NULL, sizeof(memoria_compartida_t),
                                     PROT_READ | PROT_WRITE, MAP_SHARED, fd_shm,
                                     0);
  if (shm == MAP_FAILED) {
    abortar("mmap");
  }
  if (close(fd_shm) == -1) {
    perror("close fd_shm");
  }

  inicializar_memoria_compartida(shm, num_sensores, mediciones_por_sensor,
                                 umbral);

  /* ---------------------------------------------------------------
   * Semaforo binario con nombre que protege la shm (valor inicial 1).
   * --------------------------------------------------------------- */
  sem_shm = sem_open(SEM_NOMBRE, O_CREAT | O_EXCL, 0644, 1);
  if (sem_shm == SEM_FAILED) {
    abortar("sem_open (colector)");
  }

  /* ---------------------------------------------------------------
   * Mutex de threads que protege los contadores por worker
   * --------------------------------------------------------------- */
  rc = pthread_mutex_init(&mutex_contadores, NULL);
  if (rc != 0) {
    errno = rc;
    abortar("pthread_mutex_init");
  }

  /* ---------------------------------------------------------------
   * Creacion de los procesos sensores (fork + exec).
   * --------------------------------------------------------------- */
  g_pids_sensores = (pid_t *)malloc((size_t)num_sensores * sizeof(pid_t));
  g_sensor_recogido = (int *)calloc((size_t)num_sensores, sizeof(int));
  if (g_pids_sensores == NULL || g_sensor_recogido == NULL) {
    abortar("malloc pids_sensores");
  }

  /* Se vacia stdout antes de fork() para que el hijo no herede texto
   * pendiente en el buffer (se imprimiria dos veces). */
  fflush(stdout);

  for (i = 0; i < num_sensores; i++) {
    pid = fork();
    if (pid < 0) {
      abortar("fork");
    } else if (pid == 0) {
      /* Proceso hijo: se reemplaza a si mismo por el binario "sensor".
       * execv() restaura a SIG_DFL toda senial que tenia handler, asi
       * que el sensor arranca "limpio" y registra los suyos propios. */
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
      g_pids_sensores[i] = pid;
      g_sensores_lanzados++;
    }
  }

  /* ---------------------------------------------------------------
   * Thread logger (consumidor unico de la cola de alertas).
   * Se crea ANTES que los workers (productores de alertas).
   * --------------------------------------------------------------- */
  args_logger.cola_alertas = cola_alertas;
  args_logger.log = log;
  args_logger.umbral = umbral;
  args_logger.registradas = 0;

  rc = pthread_create(&hilo_logger, NULL, rutina_logger, &args_logger);
  if (rc != 0) {
    errno = rc;
    abortar("pthread_create logger");
  }

  /* ---------------------------------------------------------------
   * Pool de worker threads (fan-out sobre la cola de mediciones).
   *
   * Todos los workers comparten el MISMO descriptor de cola: POSIX
   * garantiza que mq_receive() es atomico, asi que el kernel reparte
   * los mensajes entre los hilos sin que haga falta un mutex propio
   * para el "dequeue".
   * --------------------------------------------------------------- */
  hilos = (pthread_t *)malloc((size_t)num_workers * sizeof(pthread_t));
  args_workers =
      (args_worker_t *)malloc((size_t)num_workers * sizeof(args_worker_t));
  if (hilos == NULL || args_workers == NULL) {
    abortar("malloc workers");
  }

  for (i = 0; i < num_workers; i++) {
    args_workers[i].id_worker = i + 1;
    args_workers[i].cola = cola;
    args_workers[i].cola_alertas = cola_alertas;
    args_workers[i].shm = shm;
    args_workers[i].sem_shm = sem_shm;
    args_workers[i].mutex_contadores = &mutex_contadores;
    args_workers[i].umbral = umbral;
    args_workers[i].procesadas = 0;
    args_workers[i].alertas = 0;

    rc = pthread_create(&hilos[i], NULL, rutina_worker, &args_workers[i]);
    if (rc != 0) {
      errno = rc;
      abortar("pthread_create worker");
    }
  }

  ctx.shm = shm;
  ctx.sem_shm = sem_shm;
  ctx.mutex_contadores = &mutex_contadores;
  ctx.workers = args_workers;
  ctx.num_workers = num_workers;
  ctx.inicio = time(NULL);

  if (intervalo_reporte > 0) {
    alarm((unsigned int)intervalo_reporte);
  }

  /* ---------------------------------------------------------------
   * Bucle de supervision del hilo principal.
   *
   * Mientras los workers consumen la cola en otros threads, el hilo
   * principal:
   *   1) atiende las banderas que levantaron los handlers de señales
   *      (el trabajo pesado - printf, archivos, semaforos - se hace
   *      ACA, nunca dentro del handler);
   *   2) recoge con waitpid(..., WNOHANG) a los sensores que van
   *      terminando, para que no queden como zombies.
   * --------------------------------------------------------------- */
  sensores_vivos = num_sensores;
  while (sensores_vivos > 0 && !senial_terminar) {
    if (senial_reporte) {
      senial_reporte = 0;
      imprimir_reporte_periodico(&ctx);
      if (intervalo_reporte > 0) {
        alarm((unsigned int)intervalo_reporte);
      }
    }
    if (senial_volcado) {
      senial_volcado = 0;
      volcar_estadisticas(&ctx);
    }

    while (sensores_vivos > 0 && recolectar_sensor(0)) {
      sensores_vivos--;
    }

    if (sensores_vivos > 0 && !senial_terminar) {
      usleep(PERIODO_SUPERVISION_US);
    }
  }

  /* A partir de aca no se hacen mas reportes periodicos. */
  alarm(0);
  interrumpido = (senial_terminar != 0);

  /* ---------------------------------------------------------------
   * Apagado ordenado por SIGINT/SIGTERM: detener a los sensores.
   * Se les manda SIGTERM a los que siguen vivos; cada sensor sale de
   * su bucle, cierra la cola y termina. Se los espera con waitpid()
   * BLOQUEANTE: los workers siguen consumiendo mientras tanto, asi que
   * un sensor trabado en mq_send() con la cola llena se destraba solo.
   * --------------------------------------------------------------- */
  if (interrumpido) {
    printf("\n*** Senial de terminacion recibida: apagado ordenado ***\n");
    printf("Deteniendo %d sensor/es activo/s...\n", sensores_vivos);
    fflush(stdout);
    detener_sensores();
  }
  while (sensores_vivos > 0) {
    if (recolectar_sensor(1)) {
      sensores_vivos--;
    } else {
      break; /* ECHILD: no quedan hijos (no deberia pasar) */
    }
  }
  printf("\nTodos los sensores (%d) terminaron y fueron recogidos con "
         "waitpid().\n",
         num_sensores);

  /* ---------------------------------------------------------------
   * Drenado + fin de los workers: una "poison pill" por worker.
   *
   * Se envian DESPUES de que todos los sensores terminaron, al FINAL
   * de la cola y con la misma prioridad (0) que las mediciones: como
   * la cola es FIFO dentro de una misma prioridad, cada medicion que
   * todavia estaba pendiente se procesa antes de que algun worker
   * reciba su pildora. Asi se drena el trabajo pendiente sin perder
   * ninguna medicion ya enviada, tambien ante SIGINT.
   * --------------------------------------------------------------- */
  printf("Drenando la cola de mediciones y deteniendo los workers...\n");
  pildora.sensor_id = ID_PILDORA_VENENO;
  pildora.valor = 0.0;
  pildora.timestamp = (long)time(NULL);
  for (i = 0; i < num_workers; i++) {
    if (enviar_mensaje(cola, (const char *)&pildora, sizeof(pildora)) == -1) {
      abortar("mq_send (pildora worker)");
    }
  }
  for (i = 0; i < num_workers; i++) {
    rc = pthread_join(hilos[i], NULL);
    if (rc != 0) {
      fprintf(stderr, "pthread_join worker %d: %s\n", i + 1, strerror(rc));
    }
  }

  /* ---------------------------------------------------------------
   * Fin del logger. Ya no puede llegar ninguna alerta nueva, y todas las
   * que se generaron estan en la cola ANTES que esta pildora.
   * --------------------------------------------------------------- */
  pildora_logger.sensor_id = ID_PILDORA_VENENO;
  pildora_logger.worker_id = 0;
  pildora_logger.valor = 0.0;
  pildora_logger.umbral = umbral;
  pildora_logger.timestamp = (long)time(NULL);
  pildora_logger.motivo_fin = interrumpido ? 1 : 0;
  if (enviar_mensaje(cola_alertas, (const char *)&pildora_logger,
                     sizeof(pildora_logger)) == -1) {
    abortar("mq_send (pildora logger)");
  }
  rc = pthread_join(hilo_logger, NULL);
  if (rc != 0) {
    fprintf(stderr, "pthread_join logger: %s\n", strerror(rc));
  }
  if (fclose(log) == EOF) {
    perror("fclose " ARCHIVO_LOG);
  }

  /* ---------------------------------------------------------------
   * Marcar el procesamiento como completo (consulta deja de refrescar)
   * y mostrar el resumen final.
   * --------------------------------------------------------------- */
  if (tomar_semaforo(sem_shm) == -1) {
    abortar("sem_wait (colector resumen)");
  }
  shm->global.procesamiento_completo = 1;
  shm->global.interrumpido = interrumpido;
  if (sem_post(sem_shm) == -1) {
    abortar("sem_post (colector resumen)");
  }

  tomar_foto(&ctx, &foto, procesadas_w, alertas_w);
  escribir_estadisticas(stdout, "Resumen final de estadisticas", &foto,
                        procesadas_w, alertas_w, num_workers);
  if (interrumpido) {
    printf("Apagado por senial: se procesaron %lu de %lu mediciones "
           "esperadas (todas las que llegaron a enviarse).\n",
           foto.global.total_procesadas, foto.global.total_esperadas);
  }
  printf("Alertas registradas por el logger en %s: %lu\n", ARCHIVO_LOG,
         args_logger.registradas);

  /* Registro permanente de la corrida: la memoria compartida se borra
   * enseguida, asi que el resumen final se agrega a un CSV. A esta altura
   * ya no queda ningun thread corriendo (workers y logger terminaron con
   * pthread_join), por eso escribir el archivo no necesita ningun cerrojo. */
  escribir_resumen_csv(&foto, num_workers, interrumpido);

  /* ---------------------------------------------------------------
   * Liberar TODOS los recursos (mismo camino para salida normal y
   * para SIGINT/SIGTERM).
   * --------------------------------------------------------------- */
  rc = pthread_mutex_destroy(&mutex_contadores);
  if (rc != 0) {
    fprintf(stderr, "pthread_mutex_destroy: %s\n", strerror(rc));
  }
  free(hilos);
  free(args_workers);
  free(g_pids_sensores);
  free(g_sensor_recogido);
  g_pids_sensores = NULL;
  g_sensor_recogido = NULL;
  g_sensores_lanzados = 0;

  if (munmap(shm, sizeof(memoria_compartida_t)) == -1) {
    perror("munmap (colector)");
  }
  if (shm_unlink(SHM_NOMBRE) == -1) {
    perror("shm_unlink (colector)");
  }

  if (mq_close(cola) == -1) {
    perror("mq_close mediciones (colector)");
  }
  if (mq_unlink(MQ_NOMBRE) == -1) {
    perror("mq_unlink mediciones (colector)");
  }

  if (mq_close(cola_alertas) == -1) {
    perror("mq_close alertas (colector)");
  }
  if (mq_unlink(MQ_ALERTAS_NOMBRE) == -1) {
    perror("mq_unlink alertas (colector)");
  }

  if (sem_close(sem_shm) == -1) {
    perror("sem_close (colector)");
  }
  if (sem_unlink(SEM_NOMBRE) == -1) {
    perror("sem_unlink (colector)");
  }

  if (interrumpido) {
    printf("\nRecursos IPC liberados. Colector finalizado por senial "
           "(apagado ordenado).\n");
  } else {
    printf("\nRecursos IPC liberados. Colector finalizado normalmente.\n");
  }
  return 0;
}

/* ================================================================== */
/* Señales                                                            */
/* ================================================================== */

/* Los tres handlers */
static void manejador_terminar(int sig) {
  signal(sig, manejador_terminar);
  senial_terminar = 1;
}

static void manejador_reporte(int sig) {
  signal(sig, manejador_reporte);
  senial_reporte = 1;
}

static void manejador_volcado(int sig) {
  signal(sig, manejador_volcado);
  senial_volcado = 1;
}

static void registrar_seniales(void) {
  if (signal(SIGINT, manejador_terminar) == SIG_ERR) {
    fallar("signal SIGINT");
  }
  /* SIGTERM (kill <pid>, docker stop) se trata igual que SIGINT. */
  if (signal(SIGTERM, manejador_terminar) == SIG_ERR) {
    fallar("signal SIGTERM");
  }
  if (signal(SIGALRM, manejador_reporte) == SIG_ERR) {
    fallar("signal SIGALRM");
  }
  if (signal(SIGUSR1, manejador_volcado) == SIG_ERR) {
    fallar("signal SIGUSR1");
  }
  /* Si la salida estandar es un pipe que se cierra (ej: ./colector ... |
   * head), la accion por defecto de SIGPIPE mataria al colector sin
   * liberar los recursos IPC. Ignorandola, printf simplemente falla y el
   * apagado sigue su curso normal. */
  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
    fallar("signal SIGPIPE");
  }
}

/* ================================================================== */
/* Recursos IPC y procesos hijos                                      */
/* ================================================================== */

/* Borra los NOMBRES de los cuatro recursos IPC. Se usa al arrancar (para
 * limpiar restos de una corrida anterior terminada con kill -9) y en
 * abortar(). Los errores se ignoran a proposito: si el recurso no existe,
 * no hay nada que borrar. */
static void limpiar_nombres_ipc(void) {
  mq_unlink(MQ_NOMBRE);
  mq_unlink(MQ_ALERTAS_NOMBRE);
  shm_unlink(SHM_NOMBRE);
  sem_unlink(SEM_NOMBRE);
}

/* Salida ante un error fatal DESPUES de haber creado recursos: a
 * diferencia de fallar(), no deja ni sensores huerfanos/zombies ni
 * objetos IPC en /dev/shm o /dev/mqueue. Se usa SIGKILL (y no SIGTERM)
 * porque, segun en que punto fallo, puede que todavia no haya workers
 * consumiendo y un sensor trabado en mq_send() nunca saldria. */
static void abortar(const char *contexto) {
  int i;

  perror(contexto);
  for (i = 0; i < g_sensores_lanzados; i++) {
    if (!g_sensor_recogido[i]) {
      kill(g_pids_sensores[i], SIGKILL);
    }
  }
  for (i = 0; i < g_sensores_lanzados; i++) {
    if (!g_sensor_recogido[i]) {
      waitpid(g_pids_sensores[i], NULL, 0);
      g_sensor_recogido[i] = 1;
    }
  }
  limpiar_nombres_ipc();
  fprintf(stderr, "Colector abortado: sensores recogidos y recursos IPC "
                  "eliminados.\n");
  exit(EXIT_FAILURE);
}

/* mq_send() que reintenta si una senial interrumpio el envio (EINTR). */
static int enviar_mensaje(mqd_t cola, const char *msg, size_t tam) {
  while (mq_send(cola, msg, tam, 0) == -1) {
    if (errno != EINTR) {
      return -1;
    }
  }
  return 0;
}

/* Recoge (waitpid) a UN sensor que haya terminado.
 *   bloquear = 0 -> WNOHANG: si ninguno termino todavia, vuelve enseguida.
 *   bloquear = 1 -> espera hasta que termine alguno.
 * Devuelve 1 si recogio un sensor, 0 si no habia ninguno para recoger. */
static int recolectar_sensor(int bloquear) {
  pid_t pid;
  int estado;
  int i;

  for (;;) {
    pid = waitpid(-1, &estado, bloquear ? 0 : WNOHANG);
    if (pid == -1 && errno == EINTR) {
      continue; /* una senial interrumpio la espera: reintentar */
    }
    break;
  }
  if (pid == 0) {
    return 0; /* WNOHANG: hay hijos vivos pero ninguno termino */
  }
  if (pid == -1) {
    if (errno != ECHILD) {
      perror("waitpid");
    }
    return 0;
  }

  for (i = 0; i < g_sensores_lanzados; i++) {
    if (g_pids_sensores[i] == pid) {
      g_sensor_recogido[i] = 1;
      if (WIFSIGNALED(estado)) {
        fprintf(stderr, "Advertencia: el sensor %d termino por la senial %d\n",
                i + 1, WTERMSIG(estado));
      } else if (!WIFEXITED(estado) || WEXITSTATUS(estado) != 0) {
        fprintf(stderr, "Advertencia: el sensor %d termino de forma anormal\n",
                i + 1);
      }
      break;
    }
  }
  return 1;
}

/* Envia SIGTERM a todos los sensores que todavia no fueron recogidos.
 * Solo se le manda a PIDs no recogidos: un hijo terminado pero no
 * recogido es un zombie y su PID todavia no se puede reutilizar, asi que
 * nunca se le manda la senial a un proceso ajeno. */
static void detener_sensores(void) {
  int i;

  for (i = 0; i < g_sensores_lanzados; i++) {
    if (!g_sensor_recogido[i]) {
      if (kill(g_pids_sensores[i], SIGTERM) == -1 && errno != ESRCH) {
        perror("kill SIGTERM sensor");
      }
    }
  }
}

/* ================================================================== */
/* Estadisticas                                                       */
/* ================================================================== */

static void inicializar_memoria_compartida(memoria_compartida_t *shm,
                                           int num_sensores,
                                           unsigned long mediciones_por_sensor,
                                           double umbral) {
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
  shm->global.umbral_alerta = umbral;
  shm->global.total_alertas = 0;
  shm->global.procesamiento_completo = 0;
  shm->global.interrumpido = 0;

  for (i = 0; i < num_sensores; i++) {
    shm->sensores[i].sensor_id = i + 1;
    shm->sensores[i].cantidad = 0;
    shm->sensores[i].suma = 0.0;
    shm->sensores[i].minimo = 0.0;
    shm->sensores[i].maximo = 0.0;
    shm->sensores[i].ultimo_valor = 0.0;
    shm->sensores[i].alertas = 0;
  }
}

/* SECCION CRITICA: se llama siempre con el semaforo de la shm tomado. */
static void actualizar_estadisticas(memoria_compartida_t *shm,
                                    const medicion_msg_t *m, int es_alerta) {
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

  if (es_alerta) {
    es->alertas++;
    shm->global.total_alertas++;
  }
}

/* ================================================================== */
/* Threads                                                            */
/* ================================================================== */

/* Cuerpo de cada worker thread: consume mediciones hasta recibir su
 * poison pill. Por cada medicion:
 *   1) actualiza la shm (seccion critica con el semaforo);
 *   2) si supera el umbral, manda una alerta al logger (FUERA del
 *      semaforo: mq_send puede bloquearse si la cola de alertas esta
 *      llena, y no hay que frenar a los demas workers ni a consulta);
 *   3) actualiza sus contadores propios (seccion critica con el mutex). */
static void *rutina_worker(void *arg) {
  args_worker_t *a;
  medicion_msg_t msg;
  alerta_msg_t alerta;
  ssize_t leidos;
  int es_alerta;

  a = (args_worker_t *)arg;

  for (;;) {
    leidos = mq_receive(a->cola, (char *)&msg, sizeof(msg), NULL);
    if (leidos == -1) {
      if (errno == EINTR) {
        continue; /* interrumpido por una señal: no es un error */
      }
      perror("mq_receive (worker)");
      break;
    }

    if (msg.sensor_id == ID_PILDORA_VENENO) {
      break; /* No hay mas datos: terminar el hilo. */
    }

    es_alerta = (msg.valor > a->umbral);

    if (tomar_semaforo(a->sem_shm) == -1) {
      perror("sem_wait (worker)");
      continue;
    }
    actualizar_estadisticas(a->shm, &msg, es_alerta);
    if (sem_post(a->sem_shm) == -1) {
      perror("sem_post (worker)");
    }

    if (es_alerta) {
      alerta.sensor_id = msg.sensor_id;
      alerta.worker_id = a->id_worker;
      alerta.valor = msg.valor;
      alerta.umbral = a->umbral;
      alerta.timestamp = msg.timestamp;
      alerta.motivo_fin = 0;
      if (enviar_mensaje(a->cola_alertas, (const char *)&alerta,
                         sizeof(alerta)) == -1) {
        perror("mq_send alerta (worker)");
      }
    }

    pthread_mutex_lock(a->mutex_contadores);
    a->procesadas++;
    if (es_alerta) {
      a->alertas++;
    }
    pthread_mutex_unlock(a->mutex_contadores);
  }

  return NULL;
}

/* Thread logger */
static void *rutina_logger(void *arg) {
  args_logger_t *a;
  alerta_msg_t alerta;
  ssize_t leidos;
  time_t ahora;
  time_t t;
  char fecha[32];

  a = (args_logger_t *)arg;

  /* localtime() devuelve un puntero a un buffer estatico (no es seguro
   * entre threads): en este programa SOLO el logger la llama. */
  ahora = time(NULL);
  strftime(fecha, sizeof(fecha), "%Y-%m-%d %H:%M:%S", localtime(&ahora));
  fprintf(a->log,
          "==== [%s] Inicio de sesion del colector (PID %ld) | "
          "umbral de alerta = %.3f ====\n",
          fecha, (long)getpid(), a->umbral);
  fflush(a->log);

  for (;;) {
    leidos = mq_receive(a->cola_alertas, (char *)&alerta, sizeof(alerta), NULL);
    if (leidos == -1) {
      if (errno == EINTR) {
        continue;
      }
      perror("mq_receive (logger)");
      break;
    }

    if (alerta.sensor_id == ID_PILDORA_VENENO) {
      ahora = time(NULL);
      strftime(fecha, sizeof(fecha), "%Y-%m-%d %H:%M:%S", localtime(&ahora));
      fprintf(a->log,
              "==== [%s] Fin de sesion (%s): %lu alerta/s registrada/s "
              "====\n\n",
              fecha, alerta.motivo_fin ? "apagado por senial" : "fin normal",
              a->registradas);
      fflush(a->log);
      break;
    }

    t = (time_t)alerta.timestamp;
    strftime(fecha, sizeof(fecha), "%Y-%m-%d %H:%M:%S", localtime(&t));
    fprintf(a->log,
            "[%s] ALERTA sensor=%2d valor=%7.3f supera umbral=%.3f "
            "(detectada por worker %d)\n",
            fecha, alerta.sensor_id, alerta.valor, alerta.umbral,
            alerta.worker_id);
    /* fflush tras cada linea: el log se puede seguir con "tail -f" y,
     * si el proceso muere de golpe, no se pierden alertas en el buffer. */
    fflush(a->log);
    a->registradas++;
  }

  return NULL;
}

/* ================================================================== */
/* Reportes (los llama SOLO el hilo principal, nunca un handler)      */
/* ================================================================== */

/* Copia consistente del estado actual. Igual que en consulta.c, las
 * secciones criticas solo copian datos a variables locales; imprimir
 * (lento) se hace despues, con los locks ya liberados.
 * Nunca se tienen tomados el semaforo y el mutex al mismo tiempo, asi
 * que no hay orden de adquisicion que pueda producir deadlock. */
static void tomar_foto(const contexto_reporte_t *ctx,
                       memoria_compartida_t *foto, unsigned long *procesadas,
                       unsigned long *alertas) {
  int i;

  if (tomar_semaforo(ctx->sem_shm) == -1) {
    perror("sem_wait (reporte)");
    memset(foto, 0, sizeof(*foto));
  } else {
    *foto = *(ctx->shm);
    if (sem_post(ctx->sem_shm) == -1) {
      perror("sem_post (reporte)");
    }
  }

  pthread_mutex_lock(ctx->mutex_contadores);
  for (i = 0; i < ctx->num_workers; i++) {
    procesadas[i] = ctx->workers[i].procesadas;
    alertas[i] = ctx->workers[i].alertas;
  }
  pthread_mutex_unlock(ctx->mutex_contadores);
}

static void escribir_estadisticas(FILE *f, const char *titulo,
                                  const memoria_compartida_t *foto,
                                  const unsigned long *procesadas,
                                  const unsigned long *alertas,
                                  int num_workers) {
  int i;
  double promedio_global;
  double promedio;
  const estadistica_sensor_t *es;

  fprintf(f, "\n=== %s ===\n", titulo);
  fprintf(f, "Procesadas: %lu / %lu | Umbral de alerta: %.3f\n",
          foto->global.total_procesadas, foto->global.total_esperadas,
          foto->global.umbral_alerta);
  for (i = 0; i < foto->global.num_sensores_activos; i++) {
    es = &foto->sensores[i];
    promedio = (es->cantidad > 0) ? (es->suma / (double)es->cantidad) : 0.0;

    fprintf(f,
            "  Sensor %2d | cant=%5lu | prom=%8.3f | min=%8.3f | max=%8.3f | "
            "ultimo=%8.3f | alertas=%4lu\n",
            es->sensor_id, es->cantidad, promedio, es->minimo, es->maximo,
            es->ultimo_valor, es->alertas);
  }

  promedio_global =
      (foto->global.total_procesadas > 0)
          ? (foto->global.suma_global / (double)foto->global.total_procesadas)
          : 0.0;
  fprintf(f, "  ---------------------------------------------------------------"
             "----------------\n");
  fprintf(f,
          "  GLOBAL    | cant=%5lu | prom=%8.3f | min=%8.3f | max=%8.3f | "
          "                  alertas=%4lu\n",
          foto->global.total_procesadas, promedio_global,
          foto->global.minimo_global, foto->global.maximo_global,
          foto->global.total_alertas);
  for (i = 0; i < num_workers; i++) {
    fprintf(f, "  Worker %2d proceso %lu mediciones (%lu alerta/s).\n", i + 1,
            procesadas[i], alertas[i]);
  }
}

/* SIGALRM: reporte breve y periodico, por stdout. */
static void imprimir_reporte_periodico(const contexto_reporte_t *ctx) {
  memoria_compartida_t foto;
  unsigned long procesadas[MAX_WORKERS];
  unsigned long alertas[MAX_WORKERS];
  double porcentaje;
  double promedio;
  int i;

  tomar_foto(ctx, &foto, procesadas, alertas);

  porcentaje = (foto.global.total_esperadas > 0)
                   ? (100.0 * (double)foto.global.total_procesadas /
                      (double)foto.global.total_esperadas)
                   : 0.0;
  promedio =
      (foto.global.total_procesadas > 0)
          ? (foto.global.suma_global / (double)foto.global.total_procesadas)
          : 0.0;

  printf("[REPORTE SIGALRM t=+%lds] procesadas %lu/%lu (%.1f%%) | alertas %lu "
         "| prom %.3f | min %.3f | max %.3f\n",
         (long)(time(NULL) - ctx->inicio), foto.global.total_procesadas,
         foto.global.total_esperadas, porcentaje, foto.global.total_alertas,
         promedio, foto.global.minimo_global, foto.global.maximo_global);
  printf("                          workers:");
  for (i = 0; i < ctx->num_workers; i++) {
    printf(" W%d=%lu", i + 1, procesadas[i]);
  }
  printf("\n");
  fflush(stdout);
}

/* SIGUSR1: volcado completo, por stdout y agregado al final de
 * ARCHIVO_VOLCADO (para que quede registro despues de la corrida). */
static void volcar_estadisticas(const contexto_reporte_t *ctx) {
  memoria_compartida_t foto;
  unsigned long procesadas[MAX_WORKERS];
  unsigned long alertas[MAX_WORKERS];
  char titulo[96];
  FILE *f;

  tomar_foto(ctx, &foto, procesadas, alertas);
  sprintf(titulo, "VOLCADO SIGUSR1 (PID %ld, t=+%lds)", (long)getpid(),
          (long)(time(NULL) - ctx->inicio));

  escribir_estadisticas(stdout, titulo, &foto, procesadas, alertas,
                        ctx->num_workers);
  fflush(stdout);

  f = fopen(ARCHIVO_VOLCADO, "a");
  if (f == NULL) {
    perror("fopen " ARCHIVO_VOLCADO);
    return;
  }
  escribir_estadisticas(f, titulo, &foto, procesadas, alertas,
                        ctx->num_workers);
  if (fclose(f) == EOF) {
    perror("fclose " ARCHIVO_VOLCADO);
  }
  printf("(volcado agregado a %s)\n", ARCHIVO_VOLCADO);
}

/* Fin de la corrida: agrega al final de ARCHIVO_RESUMEN_CSV una fila por
 * sensor y una fila GLOBAL con el resumen final. Cada fila repite los datos
 * de la corrida (fecha, parametros y como termino), asi el archivo acumula
 * corridas y se puede abrir directamente como tabla (Excel, Sheets).
 * Lo llama SOLO el hilo principal, despues de todos los pthread_join. */
static void escribir_resumen_csv(const memoria_compartida_t *foto,
                                 int num_workers, int interrumpido) {
  FILE *f;
  int nuevo;
  int i;
  time_t ahora;
  char fecha[32];
  const char *fin;
  const estadistica_sensor_t *es;
  double promedio;

  /* Si el archivo todavia no existe hay que escribir la fila de titulos.
   * Se averigua intentando abrirlo para lectura. */
  f = fopen(ARCHIVO_RESUMEN_CSV, "r");
  nuevo = (f == NULL);
  if (f != NULL) {
    fclose(f);
  }

  /* Modo "a": cada corrida se agrega al final, sin borrar las anteriores. */
  f = fopen(ARCHIVO_RESUMEN_CSV, "a");
  if (f == NULL) {
    perror("fopen " ARCHIVO_RESUMEN_CSV);
    return;
  }

  /* localtime() usa un buffer estatico, pero el logger (el otro thread que
   * la llama) ya termino: aca no hay ningun riesgo. */
  ahora = time(NULL);
  strftime(fecha, sizeof(fecha), "%Y-%m-%d %H:%M:%S", localtime(&ahora));
  fin = interrumpido ? "senial" : "normal";

  if (nuevo) {
    fprintf(f, "fecha,pid,sensores,mediciones_por_sensor,workers,umbral,fin,"
               "fila,cantidad,promedio,minimo,maximo,ultimo,alertas\n");
  }

  for (i = 0; i < foto->global.num_sensores_activos; i++) {
    es = &foto->sensores[i];
    promedio = (es->cantidad > 0) ? (es->suma / (double)es->cantidad) : 0.0;
    fprintf(f,
            "%s,%ld,%d,%lu,%d,%.3f,%s,sensor %d,%lu,%.3f,%.3f,%.3f,%.3f,%lu\n",
            fecha, (long)getpid(), foto->global.num_sensores_activos,
            foto->global.mediciones_por_sensor, num_workers,
            foto->global.umbral_alerta, fin, es->sensor_id, es->cantidad,
            promedio, es->minimo, es->maximo, es->ultimo_valor, es->alertas);
  }

  promedio =
      (foto->global.total_procesadas > 0)
          ? (foto->global.suma_global / (double)foto->global.total_procesadas)
          : 0.0;
  fprintf(f, "%s,%ld,%d,%lu,%d,%.3f,%s,GLOBAL,%lu,%.3f,%.3f,%.3f,,%lu\n", fecha,
          (long)getpid(), foto->global.num_sensores_activos,
          foto->global.mediciones_por_sensor, num_workers,
          foto->global.umbral_alerta, fin, foto->global.total_procesadas,
          promedio, foto->global.minimo_global, foto->global.maximo_global,
          foto->global.total_alertas);

  if (fclose(f) == EOF) {
    perror("fclose " ARCHIVO_RESUMEN_CSV);
    return;
  }
  printf("Resumen de la corrida agregado a %s (%d filas).\n",
         ARCHIVO_RESUMEN_CSV, foto->global.num_sensores_activos + 1);
}
