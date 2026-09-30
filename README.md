 Tarea 1: Planificador Dieciochero - Sistemas Operativos

 1. Funciones Implementadas

- *`cargar_plan()`**: Se encarga del parseo del archivo `plan.txt`. Lee línea por línea, extrae los ID, nombres, duraciones (asignando valores aleatorios entre 100 y 5000 ms si no se proveen) y registra las dependencias para modelar el Grafo Acíclico Dirigido (DAG).

- *`dependencias_listas()`**: Función auxiliar que evalúa si los nodos padre de una actividad ya finalizaron exitosamente (estado 2) para permitir su ejecución. También detecta si una dependencia falló (estado -1) para propagar el error.

- *`ejecutar_plan()`**: Es el motor principal del programa. Gestiona la creación de procesos (`fork`), el control de concurrencia límite (`K`), la creación de tuberías (`pipes`) para el paso de mensajes, y el monitoreo del estado de los procesos hijos.

- *`manejador_sigint()`**: Intercepta la señal `SIGINT` (Ctrl+C) enviada por el usuario para cambiar la bandera global `seremi_detectada`, permitiendo una clausura segura del sistema.

## 2. Modo de Uso
Para compilar el programa, se deben utilizar estrictamente las banderas estipuladas para C17:
```bash

gcc -Wall -Wextra -std=c17 -lpthread planificador.c -o planificador