CC = gcc
CSTD = -std=c89 -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=700
WARN = -pedantic -Wall -Werror
CFLAGS = $(CSTD) $(WARN) -pthread -g
LDLIBS = -lpthread -lrt

BINARIOS = colector sensor consulta
OBJ_COMUN = comun.o

.PHONY: all clean limpiar_ipc limpiar_salidas ver_ipc docker-build docker-up docker-down docker-shell

all: $(BINARIOS)

colector: colector.o $(OBJ_COMUN)
	$(CC) $(CFLAGS) -o $@ colector.o $(OBJ_COMUN) $(LDLIBS)

sensor: sensor.o $(OBJ_COMUN)
	$(CC) $(CFLAGS) -o $@ sensor.o $(OBJ_COMUN) $(LDLIBS)

consulta: consulta.o $(OBJ_COMUN)
	$(CC) $(CFLAGS) -o $@ consulta.o $(OBJ_COMUN) $(LDLIBS)

%.o: %.c comun.h
	$(CC) $(CFLAGS) -c $< -o $@

# Limpia binarios y objetos generados por la compilacion.
clean:
	rm -f *.o $(BINARIOS)

# Borra los archivos que genera el colector al correr (log de alertas, volcados SIGUSR1 y resumen CSV)
limpiar_salidas:
	rm -f monitoreo.log volcado_estadisticas.txt resumen_corridas.csv

# Elimina recursos IPC que hayan quedado huerfanos si el colector se corto de forma abrupta (kill -9) antes de liberar sus recursos
limpiar_ipc:
	-rm -f /dev/shm/sem.sem_estadisticas_scada
	-rm -f /dev/shm/shm_estadisticas_scada
	-rm -f /dev/mqueue/mq_mediciones_scada
	-rm -f /dev/mqueue/mq_alertas_scada

# Muestra los recursos IPC del sistema que existen en este momento (deberia estar vacio con el colector apagado)
ver_ipc:
	@echo "--- /dev/shm ---";    ls -l /dev/shm    | grep scada || true
	@echo "--- /dev/mqueue ---"; ls -l /dev/mqueue | grep scada || true

# Targets convenientes para Docker (necesario en macOS)
docker-build:
	docker compose build

docker-up:
	docker compose up

docker-down:
	docker compose down

docker-shell:
	docker run --rm -it --ipc=host tp-scada bash
