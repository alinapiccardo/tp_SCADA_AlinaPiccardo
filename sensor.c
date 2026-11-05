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
 *
 * El sensor se detiene de forma ordenada si recibe SIGINT (Ctrl+C
 * en la terminal llega a todo el grupo de procesos, incluidos los sensores) o
 * SIGTERM (el colector se lo envia con kill() al apagarse). En ambos casos
 * deja de generar mediciones, cierra su descriptor de la cola y termina con
 * exit, para que el colector lo recoja con waitpid().
 * Ademas, si el colector muere de golpe (kill -9, que no se puede capturar),
 * el sensor lo detecta (getppid() cambia) y termina solo, en vez de quedar
 * como proceso huerfano bloqueado para siempre en una cola que nadie lee.
 */

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "comun.h"

/* Retardo maximo, en microsegundos, entre una medicion y la siguiente. Simula
 * el tiempo real que tardaria un sensor fisico en volver a muestrear, y ademas
 * da tiempo a que el cliente "consulta" pueda observar estadisticas parciales
 * mientras el sistema corre. */
#define RETARDO_MAX_US 40000 /* 40 ms */

/* Tiempo maximo que el sensor espera en mq_timedsend() si la cola esta
 * llena, antes de volver a revisar si tiene que detenerse. */
#define TIMEOUT_ENVIO_S 1

/* Bandera modificada por el handler y leida por el main */
static volatile sig_atomic_t detener = 0;

/* Handler minimo: solo vuelve a registrarse (por si el sistema usa la
 * semantica System V de signal(), que restaura SIG_DFL despues de cada
 * entrega) y levanta la bandera. Todo el trabajo real se hace en el main. */
static void manejador_detener(int sig) {
  signal(sig, manejador_detener);
  detener = 1;
}

int main(int argc, char *argv[]) {
  int id_sensor;
  long cant_temp;
  unsigned long cantidad;
  unsigned long enviadas;
  mqd_t cola;
  medicion_msg_t msg;
  struct timespec vencimiento;
  pid_t padre;
  int pendiente;
  int huerfano;

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

  /* Se registran los handlers ANTES de abrir la cola y empezar a enviar. */
  if (signal(SIGINT, manejador_detener) == SIG_ERR) {
    fallar("signal SIGINT (sensor)");
  }
  if (signal(SIGTERM, manejador_detener) == SIG_ERR) {
    fallar("signal SIGTERM (sensor)");
  }

  /* PID del colector, para detectar si muere de golpe (ver el bucle). */
  padre = getppid();

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

  enviadas = 0;
  pendiente = 0;
  huerfano = 0;
  while (enviadas < cantidad && !detener) {
    /* Si el colector murio sin poder avisar (kill -9 no se puede
     * capturar), el sensor queda huerfano: el kernel le asigna otro
     * padre y getppid() cambia. En ese caso no tiene sentido seguir
     * enviando a una cola que nadie va a leer. */
    if (getppid() != padre) {
      huerfano = 1;
      break;
    }

    if (!pendiente) {
      msg.sensor_id = id_sensor;
      msg.valor = valor_aleatorio(VALOR_MINIMO, VALOR_MAXIMO);
      msg.timestamp = (long)time(NULL);
      pendiente = 1;
    }

    /* mq_timedsend es atomica igual que mq_send (el kernel garantiza
     * que el mensaje se encola completo, sin mezclarse con el de otro
     * sensor), pero si la cola esta llena espera como maximo
     * TIMEOUT_ENVIO_S segundos en vez de bloquearse para siempre. */
    vencimiento.tv_sec = time(NULL) + TIMEOUT_ENVIO_S;
    vencimiento.tv_nsec = 0;
    if (mq_timedsend(cola, (const char *)&msg, sizeof(msg), 0, &vencimiento) ==
        -1) {
      if (errno == EINTR || errno == ETIMEDOUT) {
        /* El mensaje NO se encolo (cola llena). Se vuelve a evaluar
         * si hay que detenerse o si el padre murio, y si no, se
         * reintenta el MISMO mensaje. */
        continue;
      }
      fallar("mq_timedsend (sensor)");
    }
    pendiente = 0;
    enviadas++;

    /* usleep se interrumpe si llega una señal: el while vuelve a
     * evaluar "detener" inmediatamente. */
    usleep((useconds_t)(rand() % RETARDO_MAX_US));
  }

  if (mq_close(cola) == -1) {
    perror("mq_close (sensor)");
  }

  if (huerfano) {
    fprintf(stderr,
            "Sensor %d: el colector termino de forma abrupta; se detiene "
            "tras enviar %lu de %lu mediciones.\n",
            id_sensor, enviadas, cantidad);
  } else if (detener) {
    printf("Sensor %d: detenido por senial tras enviar %lu de %lu "
           "mediciones.\n",
           id_sensor, enviadas, cantidad);
  } else {
    printf("Sensor %d: envio %lu mediciones.\n", id_sensor, enviadas);
  }

  return 0;
}
