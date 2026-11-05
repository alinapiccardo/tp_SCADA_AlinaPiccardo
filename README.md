# Trabajo Integrador — Sistema de monitoreo de sensores (SCADA simplificado)

Laboratorio III · Alina Piccardo

Este archivo explica **cómo compilar, ejecutar y probar** el sistema. La explicación del diseño
(por qué cada mecanismo, concurrencia, señales y terminación) está en [`INFORME.md`](INFORME.md).

## Contenido del proyecto

```
comun.h            Constantes, nombres de recursos IPC y structs compartidos (mensajes y shm)
comun.c            Utilidades compartidas (fallar, valor_aleatorio, tomar_semaforo)
sensor.c           Proceso sensor: genera mediciones y las envia por la cola de mediciones
colector.c         Proceso principal: IPC, sensores, workers, logger, seniales, terminacion
consulta.c         Cliente de solo lectura sobre la memoria compartida
Makefile           Compilacion con flags C89 estrictos + targets de limpieza y Docker
Dockerfile         Imagen Linux para correr en macOS
docker-compose.yml Colector + consulta en dos contenedores que comparten IPC
README.md          Este archivo (compilar y probar)
INFORME.md         Resumen del informe (diseño y justificaciones)
```

Archivos que **genera** el colector al correr (en la carpeta desde donde se lo ejecuta):

```
monitoreo.log              Alertas escritas por el thread logger (se agrega, no se pisa)
volcado_estadisticas.txt   Un volcado por cada SIGUSR1 recibido (se agrega, no se pisa)
resumen_corridas.csv       Resumen final de cada corrida, una fila por sensor + GLOBAL (se agrega, no se pisa)
```

## Requisitos

- **Linux** con colas de mensajes POSIX (`/dev/mqueue`, presente por defecto en cualquier
  distribución moderna), `gcc`, `make`, `librt` y `libpthread`.
- **macOS**: Darwin no implementa colas de mensajes POSIX (`mqueue.h`), así que hay que usar
  **Docker** (ver más abajo).

## Compilación (Linux)

```bash
make
```

Genera `colector`, `sensor` y `consulta` en la misma carpeta, compilando con:

```
gcc -std=c89 -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=700 -pedantic -Wall -Werror -pthread ... -lpthread -lrt
```

Compila sin warnings con gcc 13, gcc 14 (la versión de la imagen Docker) y clang.

| Target                 | Qué hace                                                            |
|------------------------|---------------------------------------------------------------------|
| `make`                 | Compila los tres ejecutables                                        |
| `make clean`           | Borra binarios y `.o`                                               |
| `make ver_ipc`         | Lista los recursos IPC del sistema que existen ahora                |
| `make limpiar_ipc`     | Borra a mano recursos IPC huérfanos (sólo hace falta tras `kill -9`) |
| `make limpiar_salidas` | Borra `monitoreo.log`, `volcado_estadisticas.txt` y `resumen_corridas.csv` |

## Ejecución

**Importante:** correr siempre parado en la carpeta del proyecto. El colector lanza cada sensor
con `execv("./sensor", ...)`, relativo al directorio actual.

### 1) Colector

```bash
./colector <cant_sensores> <mediciones_por_sensor> [cant_workers] [intervalo_reporte] [umbral_alerta]
```

| Parámetro               | Rango                          | Default | Parte |
|-------------------------|--------------------------------|---------|-------|
| `cant_sensores`         | 1 a 32                         | —       | I     |
| `mediciones_por_sensor` | ≥ 1                            | —       | I     |
| `cant_workers`          | 1 a 16                         | 4       | I     |
| `intervalo_reporte`     | 0 a 3600 segundos (0 = apagado) | 5       | II    |
| `umbral_alerta`         | mayor que 0 y menor que 100    | 90.0    | II    |

Los sensores generan valores entre 0 y 100; una medición con `valor > umbral_alerta` es una alerta.
Los parámetros opcionales son posicionales: para indicar el umbral hay que indicar también los
anteriores. La invocación de la Parte I (`./colector 3 5` o `./colector 3 5 4`) sigue funcionando.

Ejemplos:

```bash
./colector 3 5                 # corrida corta (menos de 1 s), defaults de la Parte II
./colector 5 500 4 2 90        # ~10 s: reporte cada 2 s, alerta si valor > 90
./colector 3 300 2 0 50        # sin reporte periodico, umbral 50 (muchas alertas)
```

Al arrancar, el colector imprime su PID y cómo mandarle señales.

### 2) Señales (desde otra terminal, mientras el colector corre)

| Señal     | Cómo enviarla                                   | Qué hace                                                                   |
|-----------|-------------------------------------------------|----------------------------------------------------------------------------|
| `SIGALRM` | automática cada `intervalo_reporte` segundos    | Reporte breve: procesadas, %, alertas, prom/min/max y mediciones por worker |
| `SIGUSR1` | `kill -USR1 <pid>`                              | Volcado completo por stdout y agregado a `volcado_estadisticas.txt`         |
| `SIGINT`  | `Ctrl+C` en la terminal del colector, o `kill -INT <pid>` | Apagado ordenado: detiene sensores, drena la cola, libera IPC     |
| `SIGTERM` | `kill <pid>` o `docker stop`                    | Igual que SIGINT                                                           |

Para obtener el PID sin copiarlo de la pantalla:

```bash
kill -USR1 $(pgrep -x colector)
kill -INT  $(pgrep -x colector)
```

### 3) Consulta

```bash
./consulta        # una foto de las estadisticas y termina
./consulta 1      # modo watch: refresca cada 1 s hasta que el colector termine (o Ctrl+C)
```

Muestra por sensor y global: cantidad, promedio, mínimo, máximo, último valor y alertas, más el
estado (`EN CURSO`, `COMPLETO` o `COMPLETO (apagado por senial)`). Si no hay un colector corriendo
falla con `No se pudo conectar a la memoria compartida`, que es lo esperado.

### 4) Log de alertas

```bash
tail -f monitoreo.log        # seguirlo en vivo mientras el colector corre
```

Formato:

```
==== [2026-11-03 13:42:26] Inicio de sesion del colector (PID 423) | umbral de alerta = 90.000 ====
[2026-11-03 13:42:26] ALERTA sensor= 1 valor= 97.732 supera umbral=90.000 (detectada por worker 1)
...
==== [2026-11-03 13:42:30] Fin de sesion (apagado por senial): 95 alerta/s registrada/s ====
```

### 5) Resumen de corridas (CSV)

Cuando una corrida termina (normal o por SIGINT/SIGTERM), el colector agrega su resumen final al
archivo `resumen_corridas.csv`: **una fila por sensor y una fila `GLOBAL`**. El archivo se abre en
modo append, así que acumula corridas, y la fila de títulos se escribe una sola vez (cuando el
archivo todavía no existe). Al terminar, el colector lo confirma por pantalla:

```
Resumen de la corrida agregado a resumen_corridas.csv (6 filas).
```

| Columna | Qué contiene |
|---|---|
| `fecha` | Fecha y hora en que terminó la corrida (en Docker, hora UTC) |
| `pid` | PID del colector (en Docker siempre es 1) |
| `sensores`, `mediciones_por_sensor`, `workers`, `umbral` | Los parámetros con los que se corrió |
| `fin` | `normal` (procesó todo) o `senial` (apagado por SIGINT/SIGTERM) |
| `fila` | `sensor 1` … `sensor N`, o `GLOBAL` |
| `cantidad`, `promedio`, `minimo`, `maximo` | Estadísticas finales de esa fila |
| `ultimo` | Último valor del sensor (vacío en la fila `GLOBAL`) |
| `alertas` | Mediciones que superaron el umbral |

Ejemplo:

```
fecha,pid,sensores,mediciones_por_sensor,workers,umbral,fin,fila,cantidad,promedio,minimo,maximo,ultimo,alertas
2026-11-05 16:03:38,256,3,20,2,80.000,normal,sensor 1,20,51.055,2.390,99.059,99.059,5
2026-11-05 16:03:38,256,3,20,2,80.000,normal,sensor 2,20,52.372,5.003,96.921,46.599,5
2026-11-05 16:03:38,256,3,20,2,80.000,normal,sensor 3,20,52.368,3.997,97.237,76.909,2
2026-11-05 16:03:38,256,3,20,2,80.000,normal,GLOBAL,60,51.932,2.390,99.059,,12
```

Para verlo como tabla:

- **Google Sheets:** Archivo → Importar → Subir, con separador "coma".
- **Excel en español:** Datos → Desde texto/CSV, con delimitador "coma". Si los decimales no se
  reconocen, en la configuración regional del asistente elegir el punto como separador decimal
  (el archivo usa `.` para los decimales).
- **En la terminal:** `cat resumen_corridas.csv`.

Para verificarlo: en cada corrida, la suma de `cantidad` y la de `alertas` de las filas de sensores
tienen que dar lo mismo que la fila `GLOBAL`. Si el colector termina por un error fatal o por
`kill -9`, esa corrida no se agrega al CSV.

## Cómo probar cada requisito de la Parte II

Todas estas pruebas terminan verificando lo mismo: **ningún proceso sensor vivo ni zombie** y
**ningún recurso IPC** en el sistema.

```bash
ps -eo pid,ppid,stat,cmd | grep -E "[s]ensor|[c]olector"   # no debe mostrar nada (ni estado Z)
make ver_ipc                                              # las dos listas deben quedar vacias
```

**Salida normal + alertas**

```bash
make limpiar_salidas
./colector 3 5 2 1 50
grep -c ALERTA monitoreo.log      # coincide con "alertas=" de la fila GLOBAL del resumen
```

**SIGALRM y SIGUSR1**

```bash
./colector 5 500 4 2 90           # terminal 1: aparece un [REPORTE SIGALRM ...] cada 2 s
kill -USR1 $(pgrep -x colector)   # terminal 2: aparece "=== VOLCADO SIGUSR1 ... ==="
cat volcado_estadisticas.txt
```

**SIGINT (apagado ordenado con drenado)**

```bash
./colector 5 500 4 2 90           # y a los pocos segundos Ctrl+C
```

Salida esperada (resumida): cada sensor informa cuántas mediciones llegó a enviar, y el total
procesado es exactamente la suma de esas cantidades, lo que prueba que la cola se drenó (los números varían en cada corrida):

```
*** Senial de terminacion recibida: apagado ordenado ***
Deteniendo 5 sensor/es activo/s...
Sensor 1: detenido por senial tras enviar 237 de 500 mediciones.
...
Todos los sensores (5) terminaron y fueron recogidos con waitpid().
Drenando la cola de mediciones y deteniendo los workers...
=== Resumen final de estadisticas ===
Procesadas: 1126 / 2500 | Umbral de alerta: 90.000
...
Apagado por senial: se procesaron 1126 de 2500 mediciones esperadas (todas las que llegaron a enviarse).
Recursos IPC liberados. Colector finalizado por senial (apagado ordenado).
```

**kill -9 (el único caso que no se puede capturar)**

```bash
./colector 3 500 & sleep 1; kill -9 $!
sleep 2; ps -eo cmd | grep "[s]ensor"   # vacio: los sensores detectan que el padre murio y terminan solos
make ver_ipc                            # aca SI quedan recursos (nadie pudo hacer unlink)...
./colector 2 3                          # ...y la proxima corrida los limpia al arrancar
make ver_ipc                            # vacio
```

**Condiciones de carrera (opcional, requiere valgrind)**

```bash
valgrind --tool=helgrind --trace-children=no ./colector 4 800 4 1 80
# desde otra terminal: kill -USR1 <pid> y luego kill -INT <pid>
# resultado esperado: ERROR SUMMARY: 0 errors
```

## Prueba manual (cantidades a elección y cada cosa en su terminal)

Para probar con los números que quieras y ver por separado el colector, la consulta, el log, los
sensores y las señales, conviene abrir **cuatro terminales**, todas paradas en la carpeta del
proyecto:

| Terminal | Qué muestra |
|---|---|
| 1 | El colector: reportes de SIGALRM, volcados de SIGUSR1, mensajes de los sensores y resumen final |
| 2 | `consulta` en modo watch (estadísticas en vivo) |
| 3 | `monitoreo.log` en vivo (cada alerta a medida que el logger la escribe) |
| 4 | Comandos: ver los procesos sensor y mandar señales |

Usá bastantes mediciones para tener tiempo de mirar: cada sensor espera en promedio 20 ms entre
medición y medición, así que `1000` mediciones son unos 20 segundos.

### En Linux

```bash
# Terminal 1: compilar y arrancar el colector con las cantidades que quieras
make
make limpiar_salidas                  # opcional: empezar con el log vacio
./colector 5 1000 4 3 85              # 5 sensores, 1000 mediciones c/u, 4 workers, reporte cada 3 s, umbral 85
```

```bash
# Terminal 2: estadisticas en vivo
./consulta 1
```

```bash
# Terminal 3: alertas en vivo
tail -f monitoreo.log
```

```bash
# Terminal 4: sensores y señales
ps -o pid,ppid,stat,cmd --ppid $(pgrep -x colector)   # los sensores, hijos del colector (STAT "Z" = zombie)
kill -USR1 $(pgrep -x colector)                       # volcado: aparece en la terminal 1
cat volcado_estadisticas.txt                          # el mismo volcado, guardado en archivo
kill -INT $(pgrep -x colector)                        # apagado ordenado (o Ctrl+C en la terminal 1)
```

Cuando el colector termina, verificar que no quedó nada:

```bash
ps -eo pid,ppid,stat,cmd | grep -E "[s]ensor|[c]olector"   # vacio: ni sensores vivos ni zombies
make ver_ipc                                              # vacio: ningun recurso IPC
cat resumen_corridas.csv                                  # el resumen de la corrida (CSV)
```

### En macOS (Docker)

Antes de empezar (y cada vez que cambies el código):

```bash
docker compose down        # que no quede el colector de compose corriendo
make docker-build          # o: docker compose build
```

No corras este colector a la vez que el de `docker compose up`: comparten los recursos IPC
(`--ipc=host`) y el segundo que arranca borra los del primero.

```bash
# Terminal 1: el colector, en un contenedor llamado "colector"
docker run -it --ipc=host --name colector tp-scada ./colector 5 1000 4 3 85
```

```bash
# Terminal 2: estadisticas en vivo (dentro del mismo contenedor)
docker exec -it colector ./consulta 1
```

```bash
# Terminal 3: alertas en vivo
docker exec -it colector tail -f monitoreo.log
```

```bash
# Terminal 4: sensores y señales
docker exec colector ps -o pid,ppid,stat,cmd -e   # los procesos sensor (STAT "Z" = zombie)
docker kill --signal=USR1 colector                # volcado: aparece en la terminal 1
docker exec colector cat volcado_estadisticas.txt # el volcado guardado en archivo
docker kill --signal=INT colector                 # apagado ordenado (o Ctrl+C en la terminal 1)
```

Dentro del contenedor el colector es el PID 1, por eso al arrancar imprime `kill -USR1 1`: ese
comando sirve desde adentro del contenedor; desde la Mac se usa `docker kill --signal=...`.
`docker exec` solo funciona mientras el colector sigue corriendo (cuando termina, el contenedor se
detiene).

Al terminar, recuperar los archivos y borrar el contenedor:

```bash
docker run --rm --ipc=host tp-scada make ver_ipc   # vacio: ningun recurso IPC quedo
docker cp colector:/app/monitoreo.log .
docker cp colector:/app/volcado_estadisticas.txt .
docker cp colector:/app/resumen_corridas.csv .
docker rm colector                                 # necesario antes de volver a usar --name colector
```

Si no hace falta guardar los archivos, agregar `--rm` al `docker run` y el contenedor se borra solo.

### Por qué no conviene lanzar sensores a mano (`./sensor ...`)

`sensor` existe como ejecutable aparte solo porque el colector lo lanza con `fork()` + `execv()`;
no está pensado para correrse solo:

- **Sin colector corriendo** falla enseguida (`mq_open (sensor): No such file or directory`),
  porque la cola de mediciones la crea el colector.
- **Con el colector corriendo**, un sensor lanzado desde la terminal no es hijo del colector:
  - el colector no lo cuenta entre sus sensores, no lo espera con `waitpid()` y no le manda
    `SIGTERM` al apagarse;
  - sus mediciones desarman los totales: si su id está entre 1 y N se suman a las de ese sensor y
    lo procesado supera lo esperado; si el id es mayor que N, la memoria compartida las descarta
    pero igual cuentan como procesadas por el worker y pueden generar alertas en el log, así que
    los contadores dejan de coincidir;
  - si el colector termina antes que él, la cola ya no tiene quién la lea: el sensor queda
    reintentando el envío hasta que se lo corte con `Ctrl+C` (no detecta la muerte de su padre,
    porque su padre es la shell y no el colector).

La forma de "ver" a los sensores es el `ps` de la terminal 4, y la forma de elegir cuántos hay y
cuántas mediciones generan es la línea de comandos del colector.

## Docker (macOS)

Con Docker Desktop abierto.

### Opción A: Docker Compose (colector + consulta automáticos)

```bash
docker compose up --build
```

Corre `./colector 3 500 4 2 90` y, dos segundos después, `./consulta 1`, compartiendo el IPC del
host. Desde otra terminal se le pueden mandar señales al colector:

```bash
docker kill --signal=USR1 scada_colector     # volcado
docker exec scada_colector cat monitoreo.log # ver el log de alertas
```

Cuando termina, antes de `docker compose down` (que borra el contenedor y sus archivos), se pueden
traer los archivos generados a la carpeta del proyecto:

```bash
docker cp scada_colector:/app/monitoreo.log .
docker cp scada_colector:/app/resumen_corridas.csv .
```

`Ctrl+C` sobre `docker compose up` le manda SIGTERM al colector, que hace el mismo apagado
ordenado que con SIGINT. Para limpiar los contenedores: `docker compose down`.

### Opción B: una shell Linux interactiva

Para probar con varias terminales separadas, ver la sección
"Prueba manual" más arriba.

```bash
make docker-build         # o: docker compose build
make docker-shell         # o: docker run --rm -it --ipc=host tp-scada bash
```

Dentro del contenedor ya están compilados los binarios y se pueden correr todas las pruebas de la
sección anterior, por ejemplo:

```bash
./colector 5 500 4 2 90 &
kill -USR1 %1
./consulta
kill -INT %1
cat monitoreo.log
make ver_ipc
```

## Problemas frecuentes

- **`mq_open (colector): Invalid argument`**: el sistema tiene un `msg_max` menor a 10
  (`cat /proc/sys/fs/mqueue/msg_max`). Es raro; la constante `MQ_MAX_MENSAJES` está en `comun.h`.
- **`mq_open ... File exists`**: hay otro colector corriendo (por diseño se usa `O_EXCL`).
- **`execv sensor: No such file or directory`**: se ejecutó el colector desde otra carpeta.
- **Recursos que quedaron tras un `kill -9`**: la próxima corrida los borra sola, o `make limpiar_ipc`.
