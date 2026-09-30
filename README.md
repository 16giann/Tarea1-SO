# Planificador Dieciochero — Tarea 1 Sistemas Operativos

Simulador y planificador de actividades modeladas como un DAG. Cada actividad se
ejecuta en un proceso hijo (fork), con un máximo de K procesos vivos,
se comunican por pipes y el planificador reacciona a señales (SIGCHLD,
SIGINT). No se usan hilos ni primitivas de sincronización de hilos.

## Compilación

```bash
make
# equivale a:
gcc -Wall -Wextra -std=c17 planificador.c -o planificador -lpthread
```

(`-lpthread` se incluye por exigencia de la rúbrica; el código no usa hilos.)

## Uso

```bash
./planificador plan.txt K
```

- plan.txt: archivo del plan. K: máximo de procesos simultáneos (entero >= 1).
- Ctrl+C simula la inspección de la Seremi: se abortan todas las actividades.
- Código de salida: 0 todo OK, 1 hubo fallos o error de plan, 130 Seremi.

Variables de entorno opcionales (para pruebas; no cambian la invocación):

| Variable | Efecto |
|---|---|
| `FALLAR=4,9` | Las actividades con esos IDs fallan siempre (demuestra aislamiento de errores). |
| `FALLO_PCT=5` | Cada actividad falla con esa probabilidad (0-100, por defecto 0). |
| `QUIET=1` | Solo imprime el resumen final (útil en planes grandes). |

Ejemplos:

```bash
./planificador plan.txt 3
FALLAR=4 ./planificador plan.txt 3
python3 gen_plan.py 10000 7 5 > plan_grande.txt
QUIET=1 ./planificador plan_grande.txt 64
```

## Formato del plan

```
ID : Nombre : tiempo_ms : dep1, dep2, ...
```

- El ID es alfanumérico (`[A-Za-z0-9_.-]`).
- Si `tiempo_ms` está vacío (o es inválido, con aviso) se asigna un valor aleatorio entre 100 y 5000 ms.
- Se ignoran líneas vacías y las que comienzan con `#`. Las dependencias aceptan corchetes opcionales.
- Se rechazan con mensaje claro: IDs duplicados, dependencias inexistentes, auto-dependencias y **ciclos**.

## Funciones principales (planificador.c)

- cargar_plan(): lee con getline (líneas de largo arbitrario), separa los 4 campos conservando los vacíos y reserva memoria dinámica (sin límites fijos de actividades ni de dependencias).
- construir_grafo(): ordena los IDs, resuelve dependencias con búsqueda binaria, arma la lista de dependientes (formato CSR) y detecta ciclos con el algoritmo de Kahn.
- lanzar(): crea los pipes, entrega los insumos al hijo y hace fork.
- hijo(): recibe insumos, simula el trabajo (nanosleep), decide si falla y envía su mensaje.
- procesar_fin(): interpreta el estado de salida del hijo, lee su mensaje, libera dependientes o propaga el aborto.
- propagar_aborto(): marca como abortadas todas las actividades descendientes de una fallida.
- clausurar(): maneja la Seremi (SIGKILL a los hijos vivos, waitpid a cada uno, marca todo como abortado).
- ejecutar_plan(): bucle principal del planificador.

## Decisiones de diseño y justificación

**DAG con contador de dependencias pendientes (2.1, 2.4).** Cada nodo guarda cuántas
dependencias le faltan y la lista de sus dependientes. Al terminar una actividad solo se
recorren sus dependientes, y los que llegan a 0 entran a una cola FIFO. El costo total es
O(N + E); no se re-escanea el plan completo en cada evento, por eso escala a 10000 actividades.

**Concurrencia K sin busy-waiting ni race conditions (2.1).** El padre bloquea SIGINT y
SIGCHLD con sigprocmask y espera con sigsuspend, que las desbloquea de forma atómica.
Así el proceso duerme sin consumir CPU y no puede perderse una señal entre revisar el flag y
quedarse dormido. Tras despertar se recolectan todos los hijos terminados con waitpid(WNOHANG).
Nunca hay más de K hijos vivos (verificado con pgrep -P).

**Un pipe por actividad (2.2).** Cada hijo tiene dos pipes: uno padre→hijo, por el que recibe
los mensajes de sus dependencias (insumos), y otro hijo→padre, por el que envía su mensaje al
terminar. Los mensajes son de tamaño fijo (64 bytes), de modo que las lecturas nunca se mezclan
ni se cortan. El padre solo conserva un descriptor por hijo vivo y lo cierra al recolectar al hijo, por
lo que no se agotan los descriptores con 10000 actividades. Además se sube RLIMIT_NOFILE, y
si aun así pipe() o fork() fallan por recursos, el planificador espera a que termine otro
hijo y reintenta.

**Identificación del hijo por PID, no por código de salida.** El código de salida solo tiene
8 bits (no alcanza para IDs de hasta 10000 ni para IDs alfanuméricos), por lo que se usa
exit(0) para éxito, exit(1) para fallo y una tabla de procesos vivos indexada por PID.

**Aislamiento de errores (2.3).** Un hijo que termina con código distinto de 0, o muere por
señal, marca su actividad como fallida. Solo se abortan sus descendientes (recorrido iterativo
con pila); las ramas independientes siguen ejecutándose.

**Seremi (SIGINT).** El manejador solo escribe un volatile sig_atomic_t (async-signal-safe).
El bucle principal detecta el flag, envía SIGKILL a los hijos vivos, hace waitpid a cada
uno (sin zombies) y termina con un resumen. Los hijos restauran la acción por defecto de las
señales y la máscara original tras el fork.

**Otros detalles.** fflush(stdout) antes de cada fork, _exit en los hijos y salida de los
hijos con dprintf para evitar buffers duplicados; verificación de retorno de read/write
con reintento ante `EINTR`.

## Pruebas realizadas

- Plan de ejemplo con K=3, con fallos forzados (FALLAR) y con SIGINT en mitad de la ejecución.
- Planes inválidos: ciclo, ID duplicado, dependencia inexistente, K inválido.
- 10000 actividades generadas con gen_plan.py con K = 4, 64 y 500 (unos segundos en total), y con FALLO_PCT=2.
- Máximo de hijos vivos observado igual a K; sin procesos zombie; ejecución con ulimit -n 64 y K=200.
- 
## Verificación realizada
Se ejecutaron los siguientes casos en Ubuntu (VMware), confirmando el
comportamiento esperado del planificador:

- `tests/tests_ciclo.txt`: rechazado con "el plan contiene un ciclo".
- `tests/tests_dup.txt`: rechazado con "ID duplicado".
- `tests/tests_dep.txt`: rechazado con "depende de '99', que no existe".
- `FALLAR=4 ./planificador plan.txt 3`: la actividad 4 falla y se abortan
  únicamente sus 4 dependientes directos/indirectos (6, 9, 10, 11); las
  6 actividades independientes terminan con éxito. Resumen final:
  Completadas 6, Fallidas 1, Abortadas 4, Total 11 — aísla correctamente
  la falla sin afectar ramas no relacionadas del DAG.
## Autores

Gianfranco Caleni 
Gabriel Guaiquipan
