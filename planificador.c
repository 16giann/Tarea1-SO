#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#define MAX_ACTIVIDADES 10000
#define MAX_NOMBRE 128
#define MAX_DEPS 100

typedef struct {
    int id;
    char nombre[MAX_NOMBRE];
    int tiempo_ms;
    int dependencias[MAX_DEPS];
    int num_deps;
    int estado; // 0 = Pendiente, 1 = En ejecución, 2 = Terminada
} Actividad;

Actividad plan[MAX_ACTIVIDADES];
int total_actividades = 0;

void cargar_plan(const char *archivo) {
    FILE *f = fopen(archivo, "r");
    if (!f) {
        perror("Error abriendo el archivo");
        exit(EXIT_FAILURE);
    }

    char linea[256];
    while (fgets(linea, sizeof(linea), f)) {
        if (linea[0] == '\n' || linea[0] == '\r') continue;

        Actividad *act = &plan[total_actividades];
        act->num_deps = 0;
        act->estado = 0;

        char *token = strtok(linea, ":");
        if (!token) continue;
        act->id = atoi(token);

        token = strtok(NULL, ":");
        if (token) sscanf(token, " %127[^:]", act->nombre);

        token = strtok(NULL, ":");
        if (token) {
            int tiempo = atoi(token);
            if (tiempo <= 0) {
                act->tiempo_ms = 100 + rand() % 4901; 
            } else {
                act->tiempo_ms = tiempo;
            }
        } else {
             act->tiempo_ms = 100 + rand() % 4901; 
        }

        token = strtok(NULL, ":\n\r");
        if (token) {
            char *dep_token = strtok(token, ", ");
            while (dep_token) {
                act->dependencias[act->num_deps++] = atoi(dep_token);
                dep_token = strtok(NULL, ", ");
            }
        }
        total_actividades++;
    }
    fclose(f);
}

// Verifica si todas las dependencias de una actividad están en estado 2 (Terminada)
int dependencias_listas(Actividad *act) {
    for (int i = 0; i < act->num_deps; i++) {
        int dep_id = act->dependencias[i];
        int dep_terminada = 0;
        for (int j = 0; j < total_actividades; j++) {
            if (plan[j].id == dep_id && plan[j].estado == 2) {
                dep_terminada = 1;
                break;
            }
        }
        if (!dep_terminada) return 0; // Falta una dependencia
    }
    return 1;
}

void ejecutar_plan(int limite_k) {
    int actividades_terminadas = 0;
    int procesos_activos = 0;

    printf("\n--- INICIANDO PLANIFICADOR DIECIOCHERO (K=%d) ---\n", limite_k);

    while (actividades_terminadas < total_actividades) {
        // Buscar actividades listas para ejecutar
        for (int i = 0; i < total_actividades; i++) {
            if (plan[i].estado == 0 && procesos_activos < limite_k) {
                if (dependencias_listas(&plan[i])) {
                    plan[i].estado = 1; // Marcar en ejecución
                    procesos_activos++;

                    pid_t pid = fork();
                    if (pid == 0) {
                        // Código del proceso hijo (Actividad)
                        printf("[INICIO] %s (ID: %d) - Duracion: %d ms\n", plan[i].nombre, plan[i].id, plan[i].tiempo_ms);
                        usleep(plan[i].tiempo_ms * 1000); // usleep usa microsegundos
                        exit(plan[i].id); // Retornar el ID para que el padre sepa quién terminó
                    } else if (pid < 0) {
                        perror("Error en fork");
                        exit(EXIT_FAILURE);
                    }
                }
            }
        }

        // Si hay procesos corriendo, el padre espera a que uno termine
        if (procesos_activos > 0) {
            int status;
            pid_t pid_terminado = wait(&status);
            if (pid_terminado > 0) {
                if (WIFEXITED(status)) {
                    int id_terminado = WEXITSTATUS(status);
                    // Actualizar el estado de la actividad a Terminada (2)
                    for (int i = 0; i < total_actividades; i++) {
                        if (plan[i].id == id_terminado) {
                            plan[i].estado = 2;
                            actividades_terminadas++;
                            procesos_activos--;
                            printf("[FIN] %s (ID: %d) ha terminado.\n", plan[i].nombre, plan[i].id);
                            break;
                        }
                    }
                }
            }
        }
    }
    printf("--- SIMULACION COMPLETADA ---\n");
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Uso correcto: %s plan.txt K\n", argv[0]);
        return EXIT_FAILURE;
    }

    srand(time(NULL)); 
    int limite_k = atoi(argv[2]); 
    
    cargar_plan(argv[1]);
    ejecutar_plan(limite_k);

    return EXIT_SUCCESS;
}