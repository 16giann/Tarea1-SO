#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>

#define MSG_LEN 64      /* tamano fijo de cada mensaje por pipe */
#define MAX_INSUMOS 32  /* maximo de insumos entregados a un hijo */
#define DUR_MIN 100
#define DUR_MAX 5000

enum { PENDIENTE, EN_COLA, EJECUTANDO, OK, FALLO, ABORTADA };

typedef struct {
    char *id;          /* identificador alfanumerico */
    char *nombre;
    int tiempo_ms;
    char *deps_txt;    /* dependencias en texto (solo durante el parseo) */
    int *deps;         /* indices de las dependencias */
    int ndeps;
    int pendientes;    /* dependencias que aun no terminan bien */
    int estado;
    int forzar_fallo;
    pid_t pid;
    int fd_out;        /* lectura del pipe hijo -> padre */
    int slot;          /* posicion en la tabla de procesos vivos */
    char msg[MSG_LEN]; /* mensaje producido al terminar bien */
} Nodo;

/* ---- estado global ---- */
static Nodo *nodos = NULL;
static int n = 0;
static int *orden = NULL;    /* indices ordenados por ID (busqueda binaria) */
static int *dep_off = NULL;  /* lista de dependientes en formato CSR */
static int *dep_lst = NULL;
static int *cola = NULL;     /* cola FIFO de actividades listas */
static int qh = 0, qt = 0;
static int *live = NULL;     /* actividades en ejecucion (max K) */
static int nlive = 0;
static int *pila = NULL;
static int K = 1;
static int restantes = 0;
static int nok = 0, nfallo = 0, nabort = 0;
static int quiet = 0;
static int fallo_pct = 0;
static sigset_t mascara_original;

static volatile sig_atomic_t seremi = 0;
static void on_sigint(int s) { (void)s; seremi = 1; }
static void on_sigchld(int s) { (void)s; }

#define LOG(...) do { if (!quiet) printf(__VA_ARGS__); } while (0)

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "Error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(EXIT_FAILURE);
}

static int rango_aleatorio(void) { return DUR_MIN + rand() % (DUR_MAX - DUR_MIN + 1); }

/* ------------------------------------------------------------------ */
/*  Utilidades de E/S                                                  */
/* ------------------------------------------------------------------ */
static size_t leer_hasta(int fd, void *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t r = read(fd, (char *)buf + got, len - got);
        if (r > 0) got += (size_t)r;
        else if (r == 0) break;
        else if (errno != EINTR) break;
    }
    return got;
}

static int escribir_todo(int fd, const void *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t w = write(fd, (const char *)buf + sent, len - sent);
        if (w > 0) sent += (size_t)w;
        else if (w < 0 && errno != EINTR) return -1;
    }
    return 0;
}

static void dormir_ms(int ms) {
    struct timespec req = { ms / 1000, (long)(ms % 1000) * 1000000L }, rem;
    while (nanosleep(&req, &rem) == -1 && errno == EINTR) req = rem;
}

/* ------------------------------------------------------------------ */
/*  Parseo de plan.txt                                                 */
/* ------------------------------------------------------------------ */
static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

static char *dup_or_die(const char *s) {
    char *d = strdup(s);
    if (!d) die("sin memoria");
    return d;
}

static int id_valido(const char *s) {
    if (!*s) return 0;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '_' && *s != '-' && *s != '.') return 0;
    return 1;
}

static void cargar_plan(const char *ruta) {
    FILE *f = fopen(ruta, "r");
    if (!f) { perror("Error abriendo el archivo del plan"); exit(EXIT_FAILURE); }

    char *linea = NULL;
    size_t cap_linea = 0, cap = 0;
    int lineno = 0;

    while (getline(&linea, &cap_linea, f) != -1) {
        lineno++;
        char *p = trim(linea);
        if (*p == '\0' || *p == '#') continue;

        /* separar en hasta 4 campos conservando los vacios */
        char *campos[4] = { NULL, NULL, NULL, NULL };
        char *cur = p;
        for (int i = 0; i < 4; i++) {
            campos[i] = cur;
            if (i == 3) break;
            char *sep = strchr(cur, ':');
            if (!sep) break;
            *sep = '\0';
            cur = sep + 1;
        }

        char *id = trim(campos[0]);
        if (!id_valido(id)) die("linea %d: ID invalido '%s'", lineno, id);

        if (n == (int)cap) {
            cap = cap ? cap * 2 : 256;
            Nodo *nn = realloc(nodos, cap * sizeof(Nodo));
            if (!nn) die("sin memoria");
            nodos = nn;
        }
        Nodo *a = &nodos[n];
        memset(a, 0, sizeof(*a));
        a->id = dup_or_die(id);
        a->nombre = dup_or_die(campos[1] ? trim(campos[1]) : id);
        if (!*a->nombre) { free(a->nombre); a->nombre = dup_or_die(id); }

        char *t = campos[2] ? trim(campos[2]) : "";
        if (*t == '\0') {
            a->tiempo_ms = rango_aleatorio();
        } else {
            char *end;
            long v = strtol(t, &end, 10);
            if (*end != '\0' || v <= 0 || v > 86400000L) {
                fprintf(stderr, "Aviso: linea %d: tiempo invalido '%s', se asigna aleatorio\n", lineno, t);
                a->tiempo_ms = rango_aleatorio();
            } else {
                a->tiempo_ms = (int)v;
            }
        }
        a->deps_txt = dup_or_die(campos[3] ? trim(campos[3]) : "");
        a->fd_out = -1;
        n++;
    }
    free(linea);
    fclose(f);
    if (n == 0) die("el plan no contiene actividades");
}

static int cmp_idx(const void *a, const void *b) {
    return strcmp(nodos[*(const int *)a].id, nodos[*(const int *)b].id);
}

static int buscar(const char *id) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int c = strcmp(id, nodos[orden[mid]].id);
        if (c == 0) return orden[mid];
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/*  Modelado del DAG                                                   */
/* ------------------------------------------------------------------ */
static void construir_grafo(void) {
    orden = malloc((size_t)n * sizeof(int));
    if (!orden) die("sin memoria");
    for (int i = 0; i < n; i++) orden[i] = i;
    qsort(orden, (size_t)n, sizeof(int), cmp_idx);
    for (int i = 1; i < n; i++)
        if (strcmp(nodos[orden[i - 1]].id, nodos[orden[i]].id) == 0)
            die("ID duplicado: '%s'", nodos[orden[i]].id);

    /* resolver dependencias texto -> indices */
    for (int v = 0; v < n; v++) {
        Nodo *a = &nodos[v];
        a->deps = malloc((strlen(a->deps_txt) / 2 + 1) * sizeof(int));
        if (!a->deps) die("sin memoria");
        char *sp = NULL;
        for (char *tok = strtok_r(a->deps_txt, " ,\t[]", &sp); tok; tok = strtok_r(NULL, " ,\t[]", &sp)) {
            int d = buscar(tok);
            if (d < 0) die("la actividad '%s' depende de '%s', que no existe", a->id, tok);
            if (d == v) die("la actividad '%s' depende de si misma", a->id);
            a->deps[a->ndeps++] = d;
        }
        a->pendientes = a->ndeps;
        free(a->deps_txt);
        a->deps_txt = NULL;
    }

    /* lista de dependientes (CSR): dep_lst[dep_off[u] .. dep_off[u+1]) */
    dep_off = calloc((size_t)n + 1, sizeof(int));
    if (!dep_off) die("sin memoria");
    long total = 0;
    for (int v = 0; v < n; v++)
        for (int j = 0; j < nodos[v].ndeps; j++) { dep_off[nodos[v].deps[j] + 1]++; total++; }
    for (int i = 0; i < n; i++) dep_off[i + 1] += dep_off[i];
    dep_lst = malloc((size_t)(total ? total : 1) * sizeof(int));
    int *cursor = malloc((size_t)n * sizeof(int));
    if (!dep_lst || !cursor) die("sin memoria");
    memcpy(cursor, dep_off, (size_t)n * sizeof(int));
    for (int v = 0; v < n; v++)
        for (int j = 0; j < nodos[v].ndeps; j++) dep_lst[cursor[nodos[v].deps[j]]++] = v;

    /* deteccion de ciclos (algoritmo de Kahn sobre una copia) */
    int *pend = malloc((size_t)n * sizeof(int));
    int *q = malloc((size_t)n * sizeof(int));
    if (!pend || !q) die("sin memoria");
    int h = 0, t = 0;
    for (int v = 0; v < n; v++) {
        pend[v] = nodos[v].pendientes;
        if (pend[v] == 0) q[t++] = v;
    }
    while (h < t) {
        int u = q[h++];
        for (int j = dep_off[u]; j < dep_off[u + 1]; j++)
            if (--pend[dep_lst[j]] == 0) q[t++] = dep_lst[j];
    }
    if (t != n) {
        fprintf(stderr, "Error: el plan contiene un ciclo. Actividades involucradas (max 5):");
        int mostradas = 0;
        for (int v = 0; v < n && mostradas < 5; v++)
            if (pend[v] > 0) { fprintf(stderr, " %s", nodos[v].id); mostradas++; }
        fprintf(stderr, "\n");
        exit(EXIT_FAILURE);
    }
    free(pend); free(q); free(cursor);
}

/*  Proceso hijo: simula una actividad                                 */
static void hijo(const Nodo *a, int fd_in, int fd_out) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGCHLD, &sa, NULL);
    sigprocmask(SIG_SETMASK, &mascara_original, NULL);

    /* cerrar los pipes de los otros hijos que heredamos */
    for (int i = 0; i < nlive; i++) close(nodos[live[i]].fd_out);

    /* 1) recibir los insumos de las dependencias (mensajes de tamaño fijo) */
    char buf[MSG_LEN * MAX_INSUMOS];
    size_t got = leer_hasta(fd_in, buf, sizeof(buf));
    close(fd_in);
    if (!quiet)
        for (size_t off = 0; off + MSG_LEN <= got; off += MSG_LEN)
            dprintf(STDOUT_FILENO, "    [INSUMO] %s recibe: %.*s\n", a->id, MSG_LEN, buf + off);

    /* 2) simular el trabajo */
    dormir_ms(a->tiempo_ms);

    /* 3) posible fallo interno */
    srand((unsigned)getpid() * 2654435761u ^ (unsigned)time(NULL));
    if (a->forzar_fallo || (fallo_pct > 0 && rand() % 100 < fallo_pct)) _exit(EXIT_FAILURE);

    /* 4) avisar al planificador (que lo reenvia a los dependientes) */
    char msg[MSG_LEN];
    memset(msg, 0, sizeof(msg));
    snprintf(msg, sizeof(msg), "%s:%s listo", a->id, a->nombre);
    if (escribir_todo(fd_out, msg, MSG_LEN) != 0) _exit(EXIT_FAILURE);
    close(fd_out);
    _exit(EXIT_SUCCESS);
}

/*  Planificador */
/* Devuelve 0 si lanzo, 1 si conviene reintentar cuando termine otro hijo, -1 error fatal */
static int lanzar(int v) {
    Nodo *a = &nodos[v];
    int in_p[2], out_p[2];
    char buf[MSG_LEN * MAX_INSUMOS];
    size_t blen = 0;

    for (int i = 0; i < a->ndeps && i < MAX_INSUMOS; i++) {
        memcpy(buf + blen, nodos[a->deps[i]].msg, MSG_LEN);
        blen += MSG_LEN;
    }

    if (pipe(in_p) == -1) return (errno == EMFILE || errno == ENFILE) ? 1 : -1;
    if (pipe(out_p) == -1) {
        int e = errno;
        close(in_p[0]); close(in_p[1]);
        return (e == EMFILE || e == ENFILE) ? 1 : -1;
    }
    /* los insumos caben en el buffer del pipe, asi que no bloquea */
    if ((blen && escribir_todo(in_p[1], buf, blen) != 0)) {
        close(in_p[0]); close(in_p[1]); close(out_p[0]); close(out_p[1]);
        return -1;
    }
    close(in_p[1]);

    LOG("[INICIO] %s (ID: %s) - Duracion: %d ms  [vivos %d/%d]\n", a->nombre, a->id, a->tiempo_ms, nlive + 1, K);
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        int e = errno;
        close(in_p[0]); close(out_p[0]); close(out_p[1]);
        return (e == EAGAIN || e == ENOMEM) ? 1 : -1;
    }
    if (pid == 0) {
        close(out_p[0]);
        hijo(a, in_p[0], out_p[1]);
    }
    close(in_p[0]);
    close(out_p[1]);
    a->pid = pid;
    a->fd_out = out_p[0];
    a->estado = EJECUTANDO;
    a->slot = nlive;
    live[nlive++] = v;
    return 0;
}

/* Marca como abortadas todas las actividades que dependen (directa oindirectamente) de la actividad fallida 'raiz'. */
static void propagar_aborto(int raiz) {
    int sp = 0;
    pila[sp++] = raiz;
    while (sp > 0) {
        int u = pila[--sp];
        for (int j = dep_off[u]; j < dep_off[u + 1]; j++) {
            int d = dep_lst[j];
            if (nodos[d].estado == PENDIENTE) {
                nodos[d].estado = ABORTADA;
                restantes--;
                nabort++;
                LOG("[ABORTADA] %s (ID: %s) cancelada: depende de %s que fallo.\n",
                    nodos[d].nombre, nodos[d].id, nodos[raiz].id);
                pila[sp++] = d;
            }
        }
    }
}

static void procesar_fin(pid_t pid, int st) {
    int slot = -1;
    for (int i = 0; i < nlive; i++)
        if (nodos[live[i]].pid == pid) { slot = i; break; }
    if (slot < 0) return;

    int v = live[slot];
    Nodo *a = &nodos[v];
    live[slot] = live[--nlive];
    if (slot < nlive) nodos[live[slot]].slot = slot;

    int ok = 0;
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
        ok = (leer_hasta(a->fd_out, a->msg, MSG_LEN) == MSG_LEN);
    close(a->fd_out);
    a->fd_out = -1;
    restantes--;

    if (ok) {
        a->estado = OK;
        nok++;
        LOG("[FIN] %s (ID: %s) completado. Mensaje: %s\n", a->nombre, a->id, a->msg);
        for (int j = dep_off[v]; j < dep_off[v + 1]; j++) {
            Nodo *b = &nodos[dep_lst[j]];
            if (b->estado == PENDIENTE && --b->pendientes == 0) {
                b->estado = EN_COLA;
                cola[qt++] = dep_lst[j];
            }
        }
    } else {
        a->estado = FALLO;
        nfallo++;
        if (WIFSIGNALED(st))
            LOG("[FALLO] %s (ID: %s) murio por senal %d.\n", a->nombre, a->id, WTERMSIG(st));
        else
            LOG("[FALLO] %s (ID: %s) fallo internamente.\n", a->nombre, a->id);
        propagar_aborto(v);
    }
}

static int sigint_pendiente(void) {
    sigset_t p;
    sigpending(&p);
    return sigismember(&p, SIGINT);
}

/* Inspeccion de la Seremi: se aborta todo el plan */
static void clausurar(void) {
    printf("\n[!] LLEGO LA SEREMI (Ctrl+C detectado). Clausurando...\n");
    for (int i = 0; i < nlive; i++) {
        Nodo *a = &nodos[live[i]];
        LOG("[ABORTANDO] PID %d (%s) terminado a la fuerza.\n", (int)a->pid, a->nombre);
        kill(a->pid, SIGKILL);
    }
    for (int i = 0; i < nlive; i++) {
        Nodo *a = &nodos[live[i]];
        while (waitpid(a->pid, NULL, 0) == -1 && errno == EINTR) { }
        close(a->fd_out);
        a->fd_out = -1;
        a->estado = ABORTADA;
        nabort++;
    }
    nlive = 0;
    for (int v = 0; v < n; v++)
        if (nodos[v].estado == PENDIENTE || nodos[v].estado == EN_COLA) {
            nodos[v].estado = ABORTADA;
            nabort++;
        }
    restantes = 0;
}

static void subir_limite_fds(void) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) return;
    rlim_t candidatos[2] = { 65536, 10240 };
    for (int i = 0; i < 2; i++) {
        rlim_t c = candidatos[i];
        if (rl.rlim_max != RLIM_INFINITY && c > rl.rlim_max) c = rl.rlim_max;
        if (c <= rl.rlim_cur) return;
        struct rlimit nuevo = rl;
        nuevo.rlim_cur = c;
        if (setrlimit(RLIMIT_NOFILE, &nuevo) == 0) return;
    }
}

static void ejecutar_plan(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_sigint;
    sigaction(SIGINT, &sa, NULL);
    sa.sa_handler = on_sigchld;
    sa.sa_flags = SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    /* SIGINT y SIGCHLD quedan bloqueadas y solo se entregan dentro de
     * sigsuspend(): no hay carreras entre revisar el flag y dormir. */
    sigset_t bloq, vacia;
    sigemptyset(&bloq);
    sigaddset(&bloq, SIGINT);
    sigaddset(&bloq, SIGCHLD);
    sigemptyset(&vacia);
    sigprocmask(SIG_BLOCK, &bloq, &mascara_original);

    cola = malloc((size_t)n * sizeof(int));
    live = malloc((size_t)K * sizeof(int));
    pila = malloc(((size_t)n + 1) * sizeof(int));
    if (!cola || !live || !pila) die("sin memoria");
    for (int v = 0; v < n; v++)
        if (nodos[v].pendientes == 0) { nodos[v].estado = EN_COLA; cola[qt++] = v; }
    restantes = n;

    printf("--- INICIANDO PLANIFICADOR DIECIOCHERO (actividades=%d, K=%d) ---\n", n, K);
    printf("Presiona Ctrl+C para simular la inspeccion de la Seremi.\n\n");

    while (restantes > 0) {
        /* lanzar todo lo que este listo mientras haya cupo (< K procesos) */
        while (nlive < K && qh < qt) {
            if (sigint_pendiente()) break;
            int r = lanzar(cola[qh]);
            if (r == 0) { qh++; continue; }
            if (r < 0) { perror("Error creando pipe/proceso"); clausurar(); exit(EXIT_FAILURE); }
            if (nlive == 0) { fprintf(stderr, "Error: no se pueden crear mas procesos/pipes.\n"); clausurar(); exit(EXIT_FAILURE); }
            break; /* limite del SO: esperar a que termine algun hijo */
        }
        if (nlive == 0 && !sigint_pendiente()) break;

        sigsuspend(&vacia); /* duerme hasta SIGCHLD o SIGINT (sin busy-waiting) */
        if (seremi) break;

        int st;
        pid_t p;
        while ((p = waitpid(-1, &st, WNOHANG)) > 0) procesar_fin(p, st);
    }

    if (seremi) clausurar();

    printf("\n--- RESUMEN ---\n");
    printf("Completadas: %d | Fallidas: %d | Abortadas: %d | Total: %d\n", nok, nfallo, nabort, n);
    if (seremi) printf("--- SIMULACION CLAUSURADA POR LA SEREMI ---\n");
    else if (nfallo > 0) printf("--- SIMULACION TERMINADA CON FALLOS (ramas abortadas) ---\n");
    else if (restantes > 0) printf("--- PLAN INCOMPLETO ---\n");
    else printf("--- SIMULACION COMPLETADA EXITOSAMENTE ---\n");
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Uso correcto: %s plan.txt K\n", argv[0]);
        return EXIT_FAILURE;
    }
    char *end;
    long k = strtol(argv[2], &end, 10);
    if (*end != '\0' || k < 1 || k > 1000000) die("K debe ser un entero >= 1");

    const char *e;
    if ((e = getenv("QUIET")) && *e && *e != '0') quiet = 1;
    if ((e = getenv("FALLO_PCT"))) fallo_pct = atoi(e);

    srand((unsigned)time(NULL));
    cargar_plan(argv[1]);
    construir_grafo();

    if ((e = getenv("FALLAR"))) {
        char *copia = dup_or_die(e), *sp = NULL;
        for (char *tok = strtok_r(copia, " ,;", &sp); tok; tok = strtok_r(NULL, " ,;", &sp)) {
            int v = buscar(tok);
            if (v < 0) fprintf(stderr, "Aviso: FALLAR contiene un ID inexistente: %s\n", tok);
            else nodos[v].forzar_fallo = 1;
        }
        free(copia);
    }
    K = (k > n) ? n : (int)k;
    subir_limite_fds();
    ejecutar_plan();
    if (seremi) return 130;
    return (nfallo > 0 || restantes > 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}