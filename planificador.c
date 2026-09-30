#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

#define MAX_ACTIVIDADES 10000
#define MAX_NOMBRE 128
#define MAX_DEPS 100

typedef struct {
    int id;
    char nombre[MAX_NOMBRE];
    int tiempo_ms;
    int dependencias[MAX_DEPS];
    int num_deps;
    int estado; // 0 = Pendiente, 1 = En ejecución, 2 = Terminada, -1 = Abortada/Fallida
    pid_t pid;  // Guardar el PID para poder enviar señales
} Actividad;

Actividad plan[MAX_ACTIVIDADES];
int total_actividades = 0;
int procesos_activos = 0;

// Variable global para indicar si llegó la Seremi
volatile sig_atomic_t seremi_detectada = 0;

// Manejador de señal para SIGINT (Ctrl+C)
void manejador_sigint(int sig) {
    (void)sig; // Suprimir warning de variable no usada
    seremi_detectada = 1;
}

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

// Retorna 1 si están listas, 0 si faltan, y -1 si alguna dependencia falló
int dependencias_listas(Actividad *act) {
    for (int i = 0; i < act->num_deps; i++) {
        int dep_id = act->dependencias[i];
        int dep_terminada = 0;
        for (int j = 0; j < total_actividades; j++) {
            if (plan[j].id == dep_id) {
                if (plan[j].estado == -1) return -1; // Una dependencia falló, esta también debe abortar
                if (plan[j].estado == 2) dep_terminada = 1;
                break;
            }
        }
        if (!dep_terminada) return 0; // Todavía falta que termine
    }
    return 1;
}

void ejecutar_plan(int limite_k) {
    int actividades_completadas = 0;
    int fd_pipe[2];
    
    // Crear la tubería (Pipe) para la comunicación IPC
    if (pipe(fd_pipe) == -1) {
        perror("Error creando el pipe");
        exit(EXIT_FAILURE);
    }

    // Configurar la captura de la señal Ctrl+C (Seremi)
    struct sigaction sa;
    sa.sa_handler = manejador_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    printf("\n--- INICIANDO PLANIFICADOR DIECIOCHERO (K=%d) ---\n", limite_k);
    printf("Presiona Ctrl+C en cualquier momento para simular la inspeccion de la Seremi.\n\n");

    while (actividades_completadas < total_actividades && !seremi_detectada) {
        
        // Buscar actividades que puedan comenzar
        for (int i = 0; i < total_actividades; i++) {
            if (seremi_detectada) break;

            if (plan[i].estado == 0 && procesos_activos < limite_k) {
                int estado_deps = dependencias_listas(&plan[i]);
                
                // Si una dependencia falló, se aborta esta rama
                if (estado_deps == -1) {
                    plan[i].estado = -1;
                    actividades_completadas++;
                    printf("[ABORTADA] %s (ID: %d) cancelada en cascada por fallo previo.\n", plan[i].nombre, plan[i].id);
                    continue;
                }

                if (estado_deps == 1) {
                    plan[i].estado = 1; 
                    procesos_activos++;

                    pid_t pid = fork();
                    if (pid == 0) {
                        // Proceso hijo
                        close(fd_pipe[0]); // El hijo no lee del pipe, solo escribe
                        printf("[INICIO] %s (ID: %d) - Duracion: %d ms\n", plan[i].nombre, plan[i].id, plan[i].tiempo_ms);
                        
                        usleep(plan[i].tiempo_ms * 1000); 
                        
                        // Simulador de fallo aleatorio (5% de probabilidad)
                        if (rand() % 100 < 5) {
                            close(fd_pipe[1]);
                            exit(255); // Salida con error
                        }

                        // Enviar mensaje al pipe notificando el insumo
                        char msj[256];
                        snprintf(msj, sizeof(msj), "Insumo de '%s' (ID: %d) listo para sus dependientes.", plan[i].nombre, plan[i].id);
                        write(fd_pipe[1], msj, strlen(msj) + 1);
                        
                        close(fd_pipe[1]);
                        exit(plan[i].id); 
                    } else if (pid > 0) {
                        plan[i].pid = pid; // El padre guarda el PID del hijo
                    } else {
                        perror("Error en fork");
                        exit(EXIT_FAILURE);
                    }
                }
            }
        }

        // Revisar si algún proceso terminó (usando WNOHANG para que no se bloquee y permita detectar el Ctrl+C)
        if (procesos_activos > 0) {
            int status;
            pid_t pid_terminado = waitpid(-1, &status, WNOHANG);
            
            if (pid_terminado > 0) {
                if (WIFEXITED(status)) {
                    int exit_code = WEXITSTATUS(status);
                    
                    if (exit_code == 255) {
                        // Fallo interno de la actividad
                        for (int i = 0; i < total_actividades; i++) {
                            if (plan[i].pid == pid_terminado) {
                                plan[i].estado = -1;
                                actividades_completadas++;
                                procesos_activos--;
                                printf("[FALLO INTERNO] %s (ID: %d) colapso.\n", plan[i].nombre, plan[i].id);
                                break;
                            }
                        }
                    } else {
                        // Actividad exitosa
                        for (int i = 0; i < total_actividades; i++) {
                            if (plan[i].id == exit_code) {
                                plan[i].estado = 2;
                                actividades_completadas++;
                                procesos_activos--;
                                
                                // Leer el mensaje del pipe propagado por el hijo
                                char buffer[256];
                                read(fd_pipe[0], buffer, sizeof(buffer));
                                printf("[MENSAJE IPC] %s\n", buffer);
                                printf("[FIN] %s (ID: %d) completado exitosamente.\n", plan[i].nombre, plan[i].id);
                                break;
                            }
                        }
                    }
                }
            } else {
                usleep(50000); // Pequeña pausa para no saturar la CPU si nadie ha terminado aún
            }
        }
    }

    // Clausura por Seremi (Ctrl+C)
    if (seremi_detectada) {
        printf("\n\n[!] LLEGO LA SEREMI (Ctrl+C detectado). Clausurando...\n");
        for (int i = 0; i < total_actividades; i++) {
            if (plan[i].estado == 1) { // Matar solo procesos en ejecución
                kill(plan[i].pid, SIGKILL);
                printf("[ABORTANDO] Proceso PID %d (%s) terminado a la fuerza.\n", plan[i].pid, plan[i].nombre);
            }
        }
        printf("--- SIMULACION CLAUSURADA POR AUTORIDAD ---\n");
    } else {
        printf("\n--- SIMULACION COMPLETADA EXITOSAMENTE ---\n");
    }

    // Cerrar el pipe al terminar
    close(fd_pipe[0]);
    close(fd_pipe[1]);
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