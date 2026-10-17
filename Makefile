CC = gcc
CSTD = -std=c89 -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=700
WARN = -pedantic -Wall -Werror
CFLAGS = $(CSTD) $(WARN) -pthread -g
LDLIBS = -lpthread -lrt

BINARIOS = colector sensor consulta
OBJ_COMUN = comun.o

.PHONY: all clean limpiar_ipc docker-build docker-up docker-down docker-shell

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

# Elimina recursos IPC que hayan quedado huerfanos si el colector se corto de forma abrupta (kill -9) antes de liberar sus recursos
limpiar_ipc:
	-rm -f /dev/shm/sem.sem_estadisticas_ap3
	-rm -f /dev/shm/shm_estadisticas_ap3
	-rm -f /dev/mqueue/mq_mediciones_ap3

# Targets convenientes para Docker (necesario en macOS)
docker-build:
	docker compose build

docker-up:
	docker compose up

docker-down:
	docker compose down

docker-shell:
	docker run --rm -it --ipc=host tp-scada bash
