/*
 * comun.h
 *
 * Header compartido por los tres ejecutables del sistema (colector, sensor y
 * consulta).
 *
 * Concentra en un unico lugar:
 *   - Los nombres de los recursos IPC POSIX (para que los tres programas se
 *     refieran exactamente al mismo objeto del kernel).
 *   - Los limites/constantes de configuracion del sistema.
 *   - El formato de los mensajes que viajan por las dos colas
 *     (mediciones y alertas).
 *   - El layout exacto de la memoria compartida de estadisticas.
 *   - Los prototipos de las funciones utilitarias de comun.c.
 */
#ifndef COMUN_H
#define COMUN_H

#include <mqueue.h>
#include <semaphore.h>

/* Cantidad maxima de sensores que puede soportar un unico segmento de memoria
 * compartida (el arreglo de estadisticas por sensor tiene tamanio fijo) */
#define MAX_SENSORES 32

/* Cantidad de worker threads que arranca el colector si no se indica
 * explicitamente por linea de comandos, y limite maximo permitido. */
#define DEFAULT_NUM_WORKERS 4
#define MAX_WORKERS 16

/* Intervalo (en segundos) del reporte periodico disparado por
 * SIGALRM, y umbral por encima del cual una medicion genera una alerta. */
#define DEFAULT_INTERVALO_REPORTE 5
#define MAX_INTERVALO_REPORTE 3600
#define DEFAULT_UMBRAL_ALERTA 90.0

/* Rango de valores simulados por los sensores. Se usa en sensor.c para generar
 * las mediciones y en colector.c para validar que el umbral de alerta tenga
 * sentido. */
#define VALOR_MINIMO 0.0
#define VALOR_MAXIMO 100.0

/* Nombres de los recursos IPC POSIX */
#define MQ_NOMBRE "/mq_mediciones_scada"
#define MQ_ALERTAS_NOMBRE "/mq_alertas_scada"
#define SHM_NOMBRE "/shm_estadisticas_scada"
#define SEM_NOMBRE "/sem_estadisticas_scada"

/* Capacidad (cantidad de mensajes) de cada cola. */
#define MQ_MAX_MENSAJES 10

/* Archivo donde el thread logger escribe las alertas. */
#define ARCHIVO_LOG "monitoreo.log"

/* Archivo donde se agrega cada volcado de estadisticas pedido con SIGUSR1. */
#define ARCHIVO_VOLCADO "volcado_estadisticas.txt"

/* Archivo CSV donde, al terminar cada corrida, se agrega una fila por
 * sensor y una fila GLOBAL con el resumen final (se abre como tabla). */
#define ARCHIVO_RESUMEN_CSV "resumen_corridas.csv"

/* Identificador especial de sensor que el colector usa para armar las
 * "pildoras de veneno" (poison pills): un mensaje de control que le
 * indica a un worker (o al logger) que ya no hay mas datos y debe terminar. */
#define ID_PILDORA_VENENO (-1)

/* ------------------------------------------------------------------ */
/* Mensaje que viaja por la cola de mediciones (sensores -> workers)  */
/* ------------------------------------------------------------------ */
typedef struct {
  int sensor_id;  /* 1..N para mediciones reales, o ID_PILDORA_VENENO */
  double valor;   /* medicion generada por el sensor                 */
  long timestamp; /* segundos desde epoch (time(NULL))                */
} medicion_msg_t;

/* ------------------------------------------------------------------ */
/* Mensaje que viaja por la cola de alertas (workers -> logger)       */
/* Patron fan-in: muchos productores (los workers) y un unico         */
/* consumidor (el thread logger).                                     */
/* ------------------------------------------------------------------ */
typedef struct {
  int sensor_id;  /* sensor que genero la medicion, o ID_PILDORA_VENENO */
  int worker_id;  /* worker que detecto la alerta                       */
  double valor;   /* valor medido                                       */
  double umbral;  /* umbral que se supero                               */
  long timestamp; /* momento de la medicion (time(NULL) del sensor)     */
  int motivo_fin; /* solo en la pildora: 0 = fin normal, 1 = por senial */
} alerta_msg_t;

/* ------------------------------------------------------------------ */
/* Estadisticas por sensor, dentro de la memoria compartida             */
/* ------------------------------------------------------------------ */
typedef struct {
  int sensor_id;
  unsigned long cantidad;
  double suma;
  double minimo;
  double maximo;
  double ultimo_valor;
  unsigned long alertas;
} estadistica_sensor_t;

/* ------------------------------------------------------------------ */
/* Estadisticas globales                                              */
/* ------------------------------------------------------------------ */
typedef struct {
  int num_sensores_activos;
  unsigned long mediciones_por_sensor;
  unsigned long total_esperadas;
  unsigned long total_procesadas;
  double suma_global;
  double minimo_global;
  double maximo_global;
  double umbral_alerta;        /* umbral configurado por argv   */
  unsigned long total_alertas; /* alertas generadas en total    */
  int procesamiento_completo;  /* 0 = en curso, 1 = colector termino      */
  int interrumpido;            /* 1 si termino por SIGINT/TERM  */
} estadisticas_globales_t;

/* ------------------------------------------------------------------ */
/* Layout completo del segmento de memoria compartida.                */
/* ------------------------------------------------------------------ */
typedef struct {
  estadisticas_globales_t global;
  estadistica_sensor_t sensores[MAX_SENSORES];
} memoria_compartida_t;

/* ------------------------------------------------------------------ */
/* Funciones utilitarias compartidas (implementadas en comun.c)       */
/* ------------------------------------------------------------------ */
void fallar(const char *contexto);

/* Devuelve un numero pseudoaleatorio de punto flotante en [minimo,
 * maximo). Usada por cada sensor para simular una medicion fisica. */
double valor_aleatorio(double minimo, double maximo);

/* sem_wait() que reintenta si una senial interrumpio la espera
 * (errno == EINTR). Devuelve 0 si tomo el semaforo, -1 ante otro error. */
int tomar_semaforo(sem_t *sem);

#endif
