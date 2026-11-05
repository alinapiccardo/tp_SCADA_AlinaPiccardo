# Informe — Resumen

Trabajo Práctico Integrador · Laboratorio III · Alina Piccardo (0112909)
---

## 1. Qué es el proyecto

El proyecto es un **sistema de monitoreo de sensores** que imita, en menor escala, a un SCADA
industrial: junta datos de muchos sensores, los procesa y los pone a disposición de un operador.
Está escrito en **C89 estricto** y usa exclusivamente mecanismos IPC de la **API POSIX**.

El sistema tiene tres ejecutables:

| Ejecutable | Rol |
|---|---|
| **`sensor`** | Proceso independiente que genera mediciones pseudoaleatorias (entre 0 y 100) y las envía por una cola de mensajes POSIX. Lo lanza el colector con `fork()` + `execv()`. |
| **`colector`** | Proceso principal. Crea todos los recursos IPC, lanza los sensores, arranca un pool de **worker threads** que consumen la cola de mediciones, un **thread logger** que escribe alertas en disco, y un **bucle de supervisión** que atiende señales y recoge sensores terminados. |
| **`consulta`** | Proceso aparte, de solo lectura, que se adjunta a la memoria compartida existente y muestra las estadísticas actuales sin frenar al colector. Puede correr una sola vez o en modo *watch* (refrescando cada N segundos). |

Además, el código compartido (`comun.h` / `comun.c`) concentra los nombres de los recursos IPC,
las constantes de configuración, los `typedef` de los mensajes y de la memoria compartida, y tres
funciones utilitarias: `fallar()`, `valor_aleatorio()` y `tomar_semaforo()` (un `sem_wait()` que
reintenta si lo interrumpe una señal).

### Archivos del proyecto

```
comun.h            Constantes, nombres IPC, structs de mensajes y de la memoria compartida
comun.c            fallar(), valor_aleatorio(), tomar_semaforo()
sensor.c           Proceso sensor
colector.c         Proceso principal (IPC, workers, logger, señales, terminación)
consulta.c         Cliente de solo lectura sobre la memoria compartida
Makefile           Compilación C89 estricta + targets de limpieza, verificación y Docker
Dockerfile         Imagen Linux (gcc:14) para correr en macOS
docker-compose.yml Colector + consulta en dos contenedores con IPC compartido
```

Archivos que **genera** el colector al correr:

```
monitoreo.log              Alertas del thread logger (modo append, no se pisa entre corridas)
volcado_estadisticas.txt   Volcados pedidos con SIGUSR1 (modo append)
resumen_corridas.csv       Resumen final de cada corrida: una fila por sensor + GLOBAL (modo append)
```

## 2. Cómo está armado (arquitectura)

```
                         operador (Ctrl+C, kill -USR1, alarm)
                                       |
                                       v  señales
  +------------------------------- PROCESO COLECTOR --------------------------------+
  |                                                                                   |
  |   main(): bucle de supervisión  ---- SIGTERM + waitpid ---->  sensores 1..N        |
  |   (atiende señales cada 100 ms)                              (procesos)           |
  |        |                                                         |                |
  |        | lee contadores                                          | mq_timedsend   |
  |        v                                                         v                |
  |   [mutex de threads] <-- procesadas++ --  workers 1..N  <-- [cola de mediciones]  |
  |                                            |        |                             |
  |                         si valor > umbral  |        | sem_wait / sem_post         |
  |                                            v        v                             |
  |                          [cola de alertas]      [semáforo] --- [memoria compartida]
  |                                 |                                  ^              |
  |                                 v                                  |              |
  |                              logger  --> monitoreo.log             |              |
  +--------------------------------------------------------------------|--------------+
                                                                       |
                                              consulta (otro proceso, solo lectura)
```

### Recorrido de una medición

1. Un **sensor** la genera (`valor_aleatorio()`) y la envía por la **cola de mediciones** con
   `mq_timedsend()` (timeout de 1 segundo para no quedarse bloqueado si la cola está llena y poder
   revisar si tiene que detenerse o si el colector murió).
2. Cualquier **worker** libre la saca de la cola con `mq_receive()` (POSIX garantiza que la
   recepción es atómica, así que el kernel reparte el trabajo sin necesidad de un mutex extra).
3. El worker toma el **semáforo con nombre**, actualiza las estadísticas en la **memoria
   compartida** (por sensor y globales: cantidad, suma, mínimo, máximo, último valor, alertas) y
   suelta el semáforo.
4. Si el valor supera el umbral, el worker —ya sin el semáforo— manda una alerta por la **cola de
   alertas**; el **logger** la retira y la escribe en `monitoreo.log` con fecha y hora.
5. El worker actualiza sus contadores propios (procesadas y alertas) protegidos por un **mutex de
   threads**.
6. En cualquier momento, `consulta` toma el semáforo, copia una "foto" completa de las
   estadísticas a una variable local, lo suelta y recién ahí la muestra.

## 3. Mecanismos IPC: cuáles se usan y por qué

La idea general, tomada del Módulo 9 de la materia: **colas** para lo que son *eventos* que van de
un productor a un consumidor; **memoria compartida con exclusión mutua** para lo que es *estado*
que muchos leen y escriben seguido.

| Mecanismo | Para qué se usa | Por qué éste y no otro |
|---|---|---|
| **Cola de mediciones** (`/mq_mediciones_scada`) | sensores → workers | Las mediciones son eventos entre procesos. La cola garantiza atomicidad, reparte el trabajo entre workers y frena al que va más rápido (*back-pressure*) sin gastar CPU. Hacerlo con un buffer en memoria compartida necesitaría tres semáforos y más código propenso a errores. |
| **Cola de alertas** (`/mq_alertas_scada`) | workers → logger | Muchos productores y un solo consumidor (*fan-in*). Así los workers no escriben en disco y el archivo tiene un solo dueño, sin necesitar ningún candado. |
| **Memoria compartida** (`/shm_estadisticas_scada`) | estadísticas actuales | No son eventos sino estado que otro proceso (`consulta`) quiere leer cuando quiera. Leer memoria es inmediato; pedir los datos por mensajes obligaría a que el colector atienda pedidos. |
| **Semáforo con nombre** (`/sem_estadisticas_scada`) | proteger la memoria compartida | Lo usan dos procesos distintos (colector y consulta). Valor inicial 1 (binario). |
| **Mutex de threads** (`pthread_mutex_t`) | contadores de cada worker | El `main` los lee para los reportes (SIGALRM/SIGUSR1) mientras los workers los incrementan. Son datos internos de un solo proceso: alcanza con un mutex, que es más liviano y no deja nada en el sistema. |
| **Señales** | operador → colector | Solo avisan que pasó algo (no llevan datos). |

## 4. Concurrencia: secciones críticas y condiciones de carrera

Una **sección crítica** es el código que accede a datos compartidos y no puede ejecutarse
al mismo tiempo que otro proceso. En este sistema hay cuatro:

| Sección crítica | Quién la usa | Cómo se protege |
|---|---|---|
| Actualizar las estadísticas de la memoria compartida | los workers (en `rutina_worker`) | semáforo con nombre |
| Copiar la "foto" de las estadísticas | `consulta` y los reportes del main (`tomar_foto`) | el mismo semáforo |
| Marcar que el procesamiento terminó | main (al final del cierre) | el mismo semáforo |
| Contadores de cada worker (`procesadas`, `alertas`) | el worker y el main (reportes) | mutex de threads |

Las **condiciones de carrera** que podrían pasar y cómo se evitan:

- **Se pierden actualizaciones**: dos workers suman a la vez sobre el mismo sensor y una suma se
  pisa con la otra → todo el cálculo se hace con el semáforo tomado (función
  `actualizar_estadisticas()`).
- **Foto a medias**: consulta lee la cantidad ya actualizada pero la suma todavía no, y muestra un
  promedio que nunca existió → se copia la estructura entera con el semáforo y se imprime después.
- **Apagar a los workers antes de tiempo**: si se les avisa "no hay más" antes de que lleguen todas
  las mediciones, alguna se pierde → el aviso (*poison pill*) se manda recién cuando terminaron
  todos los sensores, y va al final de la cola con la misma prioridad (0), detrás de lo pendiente.
- **Apagar al logger antes de tiempo**: se perdería una alerta → se lo apaga recién cuando
  terminaron todos los workers (después de todos los `pthread_join`).
- **Una señal en mitad de una sección crítica**: si el manejador intentara tomar el semáforo y la
  señal le llegara a un worker que ya lo tiene, se trabaría esperándose a sí mismo (*deadlock*) →
  los manejadores no hacen nada más que volver a registrarse y levantar una bandera
  `volatile sig_atomic_t`.
- **Llamadas interrumpidas**: una señal puede hacer que `sem_wait`, `mq_send`, `mq_receive` o
  `waitpid` devuelvan error (`EINTR`) sin haber hecho nada → en todos los casos se reintenta
  (funciones `tomar_semaforo()`, `enviar_mensaje()`, `recolectar_sensor()`, y los bucles de las
  rutinas de worker y logger).
- **Deadlock por dos candados**: nunca se tienen el semáforo y el mutex al mismo tiempo; la alerta
  se manda después de soltar el semáforo, y `tomar_foto()` los toma en secuencia (primero semáforo,
  lo suelta, después mutex, lo suelta).

Se verificó con `valgrind --tool=helgrind` (0 errores, mandando señales durante la corrida) y con
cuentas: lo que enviaron los sensores coincide con lo procesado, y las alertas contadas coinciden
con las líneas del log.

## 5. Manejo de señales

| Señal | Qué la produce | Qué hace el colector |
|---|---|---|
| `SIGINT` | `Ctrl+C` o `kill -INT <pid>` | Apagado ordenado |
| `SIGTERM` | `kill <pid>`, `docker stop` | Lo mismo que SIGINT |
| `SIGALRM` | el temporizador `alarm()`, cada *intervalo_reporte* segundos | Reporte breve (procesadas/%, alertas, prom/min/max, mediciones por worker) y vuelve a armar la alarma |
| `SIGUSR1` | `kill -USR1 <pid>` | Volcado completo en pantalla y agregado a `volcado_estadisticas.txt` |
| `SIGPIPE` | la salida se cerró (ej. `./colector ... | head`) | Se ignora con `SIG_IGN`, para que no muera sin limpiar |

Cómo se implementan:

- Se registran con `signal()` **apenas arranca** el programa, antes de crear nada; así un `Ctrl+C`
  durante el arranque no deja cosas a medio crear.
- Los **manejadores son mínimos**: vuelven a registrar el handler y ponen en 1 una variable
  `volatile sig_atomic_t`. No hay `printf`, archivos ni semáforos adentro, porque no son seguros
  dentro de un manejador.
- El trabajo real lo hace el `main`, en un **bucle de supervisión** que cada 100 ms
  (`PERIODO_SUPERVISION_US`) mira esas banderas. No espera la señal con `pause()` porque
  en un programa con threads la señal la puede atender *cualquier* thread, y si la atiende un worker
  el `main` no se enteraría nunca. El bucle duerme entre vueltas (`usleep`), así que no gasta CPU,
  y la demora máxima (100 ms) no se nota.
- Además de las banderas, el bucle también recoge sensores terminados con
  `waitpid(-1, &estado, WNOHANG)` para que no queden como zombies mientras el sistema corre.
- Los **sensores** manejan `SIGINT` y `SIGTERM` por su cuenta: dejan de medir, cierran la cola y
  terminan. `Ctrl+C` les llega a todos a la vez (al grupo de procesos); con `kill -INT <pid>` solo
  le llega al colector, que entonces les manda `SIGTERM`.

## 6. Terminaciones

Un **zombie** es un proceso hijo que terminó pero cuyo padre nunca "recogió" (`waitpid`). Un
**recurso huérfano** es una cola, memoria compartida o semáforo que queda en el sistema (en
`/dev/mqueue` o `/dev/shm`) porque nadie lo borró.

La salida normal y el `Ctrl+C` siguen **exactamente el mismo camino**; la única diferencia es que
con `Ctrl+C` primero se les pide a los sensores que paren:

1. Se cancela la alarma (`alarm(0)`).
2. (Solo si hubo señal) se les manda `SIGTERM` a los sensores que siguen vivos (`detener_sensores`).
3. Se espera a cada sensor con `waitpid()` → **no quedan zombies**.
4. Se manda una *poison pill* (un mensaje con `sensor_id = ID_PILDORA_VENENO`) por cada worker al
   final de la cola, con la misma prioridad (0) que las mediciones: como la cola es FIFO dentro de
   una misma prioridad, primero se procesa todo lo pendiente → **se drena el trabajo**, no se pierde
   nada enviado.
5. Se espera a que terminen todos los workers (`pthread_join`).
6. Se le manda su *poison pill* al logger (con un campo `motivo_fin` que indica si fue por señal o
   por fin normal), que escribe lo que quedaba y cierra la sesión en el log.
7. Se marca `procesamiento_completo = 1` en la memoria compartida (con el semáforo tomado), para que
   `consulta` deje de refrescar, se muestra el resumen final y se agrega ese mismo resumen a
   `resumen_corridas.csv` (ver más abajo).
8. Se liberan todos los recursos:
   - `pthread_mutex_destroy` para el mutex de contadores.
   - `free` para las estructuras dinámicas (hilos, args de workers, PIDs de sensores).
   - `munmap` + `shm_unlink` para la memoria compartida.
   - `mq_close` + `mq_unlink` ×2 para las dos colas.
   - `sem_close` + `sem_unlink` para el semáforo.
   → **no quedan huérfanos**.

Casos especiales:

- **Error grave en medio del arranque**: la función `abortar()` mata a los sensores ya lanzados con
  `SIGKILL` (porque puede que todavía no haya workers consumiendo y un sensor trabado en `mq_send()`
  nunca saldría), los recoge con `waitpid()` y borra los nombres IPC antes de salir.
- **`kill -9`**: no se puede atrapar, así que el colector no llega a limpiar. Mitigaciones:
  - La siguiente corrida borra lo que haya quedado antes de empezar (`limpiar_nombres_ipc()` al
    arrancar).
  - Los sensores detectan que su padre murió (`getppid()` cambia, porque el kernel les asigna otro
    padre) y terminan solos en vez de quedarse bloqueados.
  - `mq_timedsend()` con timeout de 1 segundo evita que un sensor quede bloqueado para siempre en
    una cola que nadie lee.
  - Existe `make limpiar_ipc` para limpiar a mano si fuera necesario.

### Registro de la corrida (`resumen_corridas.csv`)

La memoria compartida se borra al terminar (`shm_unlink`), así que el resumen final de las
estadísticas solo quedaba en la pantalla. Para que una corrida se pueda consultar después, y por
alguien que no la vio en vivo, el colector agrega al terminar su resumen a `resumen_corridas.csv`:
una fila por sensor y una fila `GLOBAL`. Cada fila repite la fecha, los parámetros de la corrida
y cómo terminó (`normal` o `senial`). Así el archivo acumula corridas y se abre directamente como
tabla, para comparar, por ejemplo, distintas cantidades de sensores o umbrales.

Decisiones de diseño:

- **Lo escribe el `main`, una sola vez y al final**, después de todos los `pthread_join`. Para ese
  momento no queda ningún thread corriendo, así que escribir el archivo no necesita ningún cerrojo
  ni cambia el análisis de concurrencia. Por la misma razón es seguro usar `localtime()` para la
  fecha: el único otro thread que la usa, el logger, ya terminó.
- **Es un resumen y no un registro de cada medición.** Escribir cada medición a disco desde los
  workers frenaría el procesamiento (la E/S es lo más lento del sistema) y obligaría a agregar otro
  cerrojo u otro thread escritor. Las alertas individuales ya quedan en `monitoreo.log`.
- **Usa solo `fopen`/`fprintf`/`fclose`** (Módulo 2). Para saber si hay que escribir la fila de
  títulos, primero se intenta abrir el archivo para lectura: si no existe, es la primera corrida.

## 7. Línea de comandos

```
./colector <cant_sensores> <mediciones_por_sensor> [cant_workers] [intervalo_reporte] [umbral_alerta]
```

| Parámetro | Rango | Default |
|---|---|---|
| `cant_sensores` | 1 a 32 (`MAX_SENSORES`) | obligatorio |
| `mediciones_por_sensor` | ≥ 1 | obligatorio |
| `cant_workers` | 1 a 16 (`MAX_WORKERS`) | 4 |
| `intervalo_reporte` | 0 a 3600 segundos (0 = sin reporte) | 5 |
| `umbral_alerta` | mayor que 0.0 y menor que 100.0 | 90.0 |

Los sensores generan valores entre 0 y 100; una medición con `valor > umbral_alerta` es una alerta.

```
./consulta              # una foto y termina
./consulta <intervalo>  # modo watch: refresca cada N segundos, hasta que el colector termine o Ctrl+C
```

## 8. Portabilidad y Docker

El sistema requiere **colas de mensajes POSIX** (`mqueue.h`), que están disponibles en cualquier
Linux moderno pero **no en macOS** (Darwin no las implementa). Por eso se incluyen un `Dockerfile`
(imagen `gcc:14`) y un `docker-compose.yml` que levantan el colector y la consulta en contenedores
con IPC compartido (`ipc: host`). `docker stop` le manda `SIGTERM` al colector, que hace el mismo
apagado ordenado que con `Ctrl+C`, con un `stop_grace_period` de 20 segundos para darle tiempo a
drenar la cola.

## 9. Pruebas

Compila sin warnings con `-std=c89 -pedantic -Wall -Werror` (gcc 13, gcc 14 y clang). Después de
cada prueba se comprobó que no quedaran procesos ni recursos IPC.

| Prueba | Resultado |
|---|---|
| Corrida normal | todo procesado; alertas en memoria = líneas en el log |
| SIGALRM y SIGUSR1 | reportes periódicos y volcados correctos |
| SIGINT a mitad de corrida | las mediciones procesadas coinciden exactamente con la suma de lo que cada sensor llegó a enviar (drenado completo) |
| Ctrl+C real (a todo el grupo de procesos) | mismo cierre ordenado |
| Ctrl+C justo al arrancar | no muere a medio crear |
| `kill -9` al colector | los sensores terminan solos; la corrida siguiente limpia los recursos |
| Salida a un pipe que se cierra (`SIGPIPE`) | termina y libera todo igual |
| Estrés: 32 sensores, 16 workers, umbral 1 | miles de alertas sin pérdidas |
| helgrind | 0 condiciones de carrera |

### Prueba manual

Además de las pruebas anteriores, el sistema se puede probar eligiendo las cantidades por
línea de comandos y abriendo una terminal para cada cosa: el colector (reportes, volcados y resumen
final), `consulta` en modo watch, `monitoreo.log` en vivo con `tail -f`, y una cuarta terminal
para ver los procesos sensor con `ps` (sus PPID muestran que son hijos del colector, y la columna
STAT permite comprobar que no queda ninguno como zombie) y mandar las señales con `kill` (en Linux)
o `docker kill --signal=...` (en macOS con Docker). Los comandos exactos están en la sección
"Prueba manual" del [`README.md`](README.md).

### Por qué los sensores no se lanzan a mano

Por diseño, **el colector es el único dueño del ciclo de vida de los sensores**: los crea con
`fork()` + `execv()`, sabe cuántos son, los recoge con `waitpid()` y les manda `SIGTERM` al
apagarse. Por eso `sensor` no está pensado para ejecutarse solo, y hacerlo rompe varias garantías
del sistema (se comprobó en una prueba):

- **Sin colector** falla enseguida, porque la cola de mediciones la crea el colector.
- **Con el colector corriendo**, un sensor lanzado desde la terminal es hijo de la shell, no del
  colector, así que el colector no lo espera ni lo detiene.
- **Sus mediciones desarman los totales**: con un id fuera del rango configurado, la memoria
  compartida las descarta, pero el worker igual las cuenta como procesadas y puede generar alertas.
  En la prueba, con 2 sensores reales y uno lanzado a mano con id 9, la memoria compartida mostró
  400 mediciones y 195 alertas, mientras que los workers contaron 572 y el log 283 (88 de ellas del
  sensor 9).
- **Queda trabado al final**: cuando el colector termina, nadie lee más la cola y el sensor queda
  reintentando el envío hasta que se lo corta con `Ctrl+C`. La detección de "mi padre murió" no lo
  salva, porque su padre es la shell.

Si se quieren más sensores o más mediciones, se cambian los argumentos del colector.

## 10. Limitaciones conocidas

- La reacción a una señal puede demorar hasta 100 ms (por el bucle de supervisión).
- Las señales no se acumulan: dos `SIGUSR1` casi simultáneas pueden dar un solo volcado.
- `SIGKILL` no se puede manejar (ver mitigaciones en la sección 6); `SIGHUP` y `SIGQUIT` no se
  manejan.
- Con un umbral muy bajo el logger puede frenar a los workers (la cola de alertas se llena y
  `mq_send` bloquea); se prefirió no perder alertas.
- No corre nativamente en macOS (no tiene colas POSIX): se usa Docker.
- Si el colector termina por un error fatal o por `kill -9`, esa corrida no queda registrada en
  `resumen_corridas.csv` (el resumen se escribe recién en el cierre ordenado).
- Un `sensor` lanzado a mano (fuera del colector) no está soportado: desarma los totales y no
  lo detiene nadie (ver "Por qué los sensores no se lanzan a mano").

## 11. Conclusión

Usar el mecanismo adecuado para cada tipo de dato (colas para eventos, memoria compartida para
estado, semáforo entre procesos, mutex entre threads) dejó pocas secciones críticas, cortas y fáciles
de razonar. Lo más delicado fue la terminación: la clave fue que los manejadores de señales solo
levanten una bandera y que todo el trabajo ocurra en un único lugar (el bucle de supervisión del
main), para que el `Ctrl+C` y la salida normal compartan el mismo camino de cierre y su orden
garantice que no se pierde nada ni queda nada abandonado.
