/*
 * comun.h
 *
 * Header compartido por los tres ejecutables del sistema (colector, sensor y
 consulta).

 * Concentra en un unico lugar:
 *   - Los nombres de los recursos IPC POSIX (para que los tres programas se
 refieran exactamente al mismo objeto del kernel).
 *   - Los limites/constantes de configuracion del sistema.
 *   - El formato del mensaje que viaja por la cola de mediciones.
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

#define MQ_NOMBRE "/mq_mediciones_ap3"
#define SHM_NOMBRE "/shm_estadisticas_ap3"
#define SEM_NOMBRE "/sem_estadisticas_ap3"

/* Identificador especial de sensor que el colector usa para armar las
 * "pildoras de veneno" (poison pills): un mensaje de control que le
 * indica a un worker que ya no hay mas mediciones y debe terminar. */
#define ID_PILDORA_VENENO (-1)

/* ------------------------------------------------------------------ */
/* Mensaje que viaja por la cola de mensajes (sensores -> colector)    */
/* ------------------------------------------------------------------ */
typedef struct {
  int sensor_id;  /* 1..N para mediciones reales, o ID_PILDORA_VENENO */
  double valor;   /* medicion generada por el sensor                 */
  long timestamp; /* segundos desde epoch (time(NULL))                */
} medicion_msg_t;

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
} estadistica_sensor_t;

/* ------------------------------------------------------------------ */
/* Estadisticas globales (agregado de todos los sensores)               */
/* ------------------------------------------------------------------ */
typedef struct {
  int num_sensores_activos;
  unsigned long mediciones_por_sensor;
  unsigned long total_esperadas;
  unsigned long total_procesadas;
  double suma_global;
  double minimo_global;
  double maximo_global;
  int procesamiento_completo; /* 0 = en curso, 1 = colector termino */
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

#endif
