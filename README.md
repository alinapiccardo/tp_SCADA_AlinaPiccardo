# Trabajo Integrador — Parte 1
Sistema de monitoreo de sensores (SCADA simplificado)

## Contenido del proyecto

```
comun.h            Constantes, nombres de recursos IPC y structs compartidos
comun.c            Funciones utilitarias compartidas (fallar, valor_aleatorio)
sensor.c           Proceso sensor: genera mediciones y las envia por la cola
colector.c         Proceso principal: IPC, fork de sensores, pool de workers
consulta.c         Cliente de solo lectura sobre la memoria compartida
Makefile           Compilacion con flags C89 estrictos
README.md          Este archivo
```

## Requisitos

- **Linux** con soporte de colas de mensajes POSIX (`/dev/mqueue` montado, presente por defecto en cualquier distribucion moderna), `gcc`, `make`, `librt` y `libpthread`.
- **macOS**: Como Darwin no implementa colas de mensajes POSIX (`mqueue.h`), se debe ejecutar mediante **Docker** o **Docker Compose** (incluidos en el repositorio).

## Compilacion y Ejecucion en macOS (con Docker)

Asegurate de que Docker Desktop este abierto.

### Opcion A: Con Docker Compose (colector + consulta automatizado)

```bash
docker compose up
```

Esto compila la imagen, arranca el colector (`./colector 3 500 4`) y lanza la consulta (`./consulta 1`) compartiendo el espacio IPC. Podras ver las actualizaciones de las mediciones segundo a segundo hasta la finalizacion. Para limpiar los contenedores:

```bash
docker compose down
```

### Opcion B: Ejecucion manual en terminales separadas (Docker)

1. Compilar la imagen:
   ```bash
   make docker-build
   # o: docker build -t tp-scada .
   ```

2. En la **Terminal 1** (colector):
   ```bash
   docker run --rm -it --ipc=host tp-scada ./colector 5 500 4
   ```

3. En la **Terminal 2** (consulta):
   ```bash
   docker run --rm -it --ipc=host tp-scada ./consulta 1
   ```

4. Para una shell interactiva en el contenedor Linux:
   ```bash
   make docker-shell
   # o: docker run --rm -it --ipc=host tp-scada bash
   ```

---

## Compilacion en Linux nativo

```bash
make
```

Esto genera tres ejecutables en la misma carpeta: `colector`, `sensor` y `consulta`. Se compila con:

```
gcc -std=c89 -D_XOPEN_SOURCE=700 -D_DEFAULT_SOURCE -pedantic -Wall -Werror -pthread ...
```

Para borrar los binarios y los `.o`:

```bash
make clean
```

## Ejecucion

**Importante:** `colector` y `sensor` deben estar en la **misma carpeta** (el colector arranca cada sensor con `execv("./sensor", ...)` en forma relativa al directorio desde el que se lo ejecuta). Correr siempre parado en la carpeta del proyecto.

### 1) Correr el colector

```bash
./colector <cant_sensores> <mediciones_por_sensor> [cant_workers]
```

- `cant_sensores`: entre 1 y 32.
- `mediciones_por_sensor`: cuantas mediciones genera cada sensor.
- `cant_workers` (opcional): entre 1 y 16. Por defecto 4.

Ejemplo simple (termina en menos de un segundo):

```bash
./colector 3 5
```

Ejemplo mas largo, para tener tiempo de usar `consulta` mientras corre (cada sensor espera hasta 40ms entre medicion y medicion):

```bash
./colector 5 500 4
```

### 2) Consultar las estadisticas mientras el colector esta corriendo

En **otra terminal**, mientras el `colector` sigue corriendo:

```bash
./consulta
```

Muestra una unica "foto" de las estadisticas actuales (por sensor y globales) y termina. Tambien se puede pasar un intervalo, en segundos, para que se refresque solo (modo watch) hasta `Ctrl+C`:

```bash
./consulta 1
```

Si se ejecuta `consulta` **sin** que haya un `colector` corriendo (o despues de que ya termino), falla con un mensaje explicativo, porque la memoria compartida ya no existe:

```
No se pudo conectar a la memoria compartida (/shm_estadisticas_ap3). 
Verificar que el colector este corriendo.
shm_open (consulta): No such file or directory
```

Esto es el comportamiento esperado: `consulta` demuestra que la memoria compartida es un recurso real del sistema operativo, independiente de los procesos que la usan, y que existe solo mientras el colector no la libera.

### 3) Inspeccionar los recursos IPC "a mano" (opcional)

Mientras el colector esta corriendo se puede ver, desde otra terminal, que los recursos realmente existen como archivos del sistema:

```bash
ls -l /dev/shm/ | grep ap3
ls -l /dev/mqueue/ | grep ap3
```

Y una vez que el colector termina, esos archivos ya no estan (fueron liberados con `shm_unlink`/`mq_unlink`/`sem_unlink`).

### 4) Limpieza manual de recursos huerfanos

Si el `colector` se corta de forma abrupta (por ejemplo `kill -9`) antes de liberar sus recursos, la proxima vez que se lo ejecute los va a limpiar el solo al arrancar. Si se prefiere limpiarlos a mano:

```bash
make limpiar_ipc
```

## Ejemplo de salida esperada

```
$ ./colector 3 5
=== Colector iniciado (PID 12345) ===
Sensores: 3 | Mediciones por sensor: 5 | Total esperado: 15 | Workers: 4

Todos los sensores (3) terminaron de enviar sus mediciones.
Worker 1 proceso 4 mediciones.
Worker 2 proceso 4 mediciones.
Worker 3 proceso 4 mediciones.
Worker 4 proceso 3 mediciones.

=== Resumen final de estadisticas ===
  Sensor  1 | cant=    5 | prom=  70.209 | min=  49.748 | max=  99.384 | ultimo=  86.726
  Sensor  2 | cant=    5 | prom=  54.605 | min=  45.279 | max=  69.516 | ultimo=  45.279
  Sensor  3 | cant=    5 | prom=  60.150 | min=  32.225 | max=  89.785 | ultimo=  76.147
  ---------------------------------------------------------------
  GLOBAL    | cant=   15 | prom=  61.655 | min=  32.225 | max=  99.384

Recursos IPC liberados. Colector finalizado normalmente.
```
