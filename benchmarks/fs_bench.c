/*
 * fs_bench.c -- Benchmark unificado de false sharing (hardware real).
 *
 * Cada hilo incrementa ITERS veces su propio contador. Se elige el layout,
 * el tipo de incremento, el numero de hilos y los CPUs donde se fijan.
 *
 *   packed  : contadores contiguos, 8 B entre ellos     -> false sharing
 *   padded  : contadores separados --stride B (64/128)  -> una linea por contador
 *   local   : acumulacion en registro, una escritura al final
 *   shared  : todos incrementan el mismo contador       -> true sharing (referencia)
 *
 *   --rmw atomic   : __atomic_fetch_add relaxed (en x86: lock add)
 *   --rmw volatile : load + add + store sobre volatile (en "shared" pierde
 *                    actualizaciones: es una carrera de datos a proposito)
 *
 * Compilar:  gcc -O2 -g -pthread fs_bench.c -o fs_bench
 * Ejemplo:   ./fs_bench -l packed -n 4 --cpus 0,2,4,6 -r 10
 *            sudo perf c2c record -- ./fs_bench -l packed -n 4 --cpus 0,2,4,6
 */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAXT 64

typedef enum { L_PACKED, L_PADDED, L_LOCAL, L_SHARED } Layout;
static const char *LAYOUT_NAME[] = { "packed", "padded", "local", "shared" };

static Layout   layout = L_PACKED;
static int      nthreads = 4;
static uint64_t iters = 100000000ULL;
static int      stride = 64;
static int      offset = 0;
static int      use_atomic = 1;
static int      reps = 5;
static int      csv = 0;
static int      cpus[MAXT];
static int      ncpus = 0;

static unsigned char *arena;          /* memoria de los contadores, alineada a 4 KiB */
static pthread_barrier_t barrier;
static struct timespec t_start;

static inline volatile uint64_t *counter(int t) {
    size_t off = (size_t)offset;
    switch (layout) {
    case L_PACKED: off += (size_t)t * 8; break;
    case L_PADDED: off += (size_t)t * (size_t)stride; break;
    case L_LOCAL:  off += (size_t)t * 8; break;
    case L_SHARED: break;
    }
    return (volatile uint64_t *)(arena + off);
}

static size_t footprint(void) {
    switch (layout) {
    case L_PACKED: return (size_t)nthreads * 8;
    case L_PADDED: return (size_t)nthreads * (size_t)stride;
    case L_LOCAL:  return (size_t)nthreads * 8;
    case L_SHARED: return 8;
    }
    return 0;
}

static double now_s(const struct timespec *a, const struct timespec *b) {
    return (double)(b->tv_sec - a->tv_sec) + (double)(b->tv_nsec - a->tv_nsec) / 1e9;
}

static void *worker(void *arg) {
    int t = (int)(intptr_t)arg;
    volatile uint64_t *c = counter(t);

    pthread_barrier_wait(&barrier);
    if (t == 0) clock_gettime(CLOCK_MONOTONIC, &t_start);
    pthread_barrier_wait(&barrier);   /* todos arrancan despues de tomar t_start */

    if (layout == L_LOCAL) {
        uint64_t acc = 0;
        for (uint64_t i = 0; i < iters; i++) {
            acc++;
            __asm__ volatile("" : "+r"(acc));   /* evita que el compilador colapse el lazo */
        }
        if (use_atomic) __atomic_fetch_add(c, acc, __ATOMIC_RELAXED);
        else *c += acc;
    } else if (use_atomic) {
        for (uint64_t i = 0; i < iters; i++)
            __atomic_fetch_add(c, 1, __ATOMIC_RELAXED);
    } else {
        for (uint64_t i = 0; i < iters; i++)
            (*c)++;
    }
    return NULL;
}

/* Ejecuta una repeticion y devuelve el tiempo en segundos. */
static double run_once(void) {
    pthread_t th[MAXT];
    memset(arena, 0, 8192);
    pthread_barrier_init(&barrier, NULL, (unsigned)nthreads);

    for (int t = 0; t < nthreads; t++) {
        pthread_attr_t at;
        pthread_attr_init(&at);
        if (ncpus > 0) {
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(cpus[t % ncpus], &set);
            pthread_attr_setaffinity_np(&at, sizeof set, &set);
        }
        int e = pthread_create(&th[t], &at, worker, (void *)(intptr_t)t);
        if (e) { fprintf(stderr, "pthread_create: %s\n", strerror(e)); exit(1); }
        pthread_attr_destroy(&at);
    }
    for (int t = 0; t < nthreads; t++) pthread_join(th[t], NULL);
    struct timespec t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    pthread_barrier_destroy(&barrier);
    return now_s(&t_start, &t_end);
}

/* Comprueba la suma: en shared+volatile se esperan perdidas. */
static uint64_t total_count(void) {
    if (layout == L_SHARED) return *counter(0);
    uint64_t s = 0;
    for (int t = 0; t < nthreads; t++) s += *counter(t);
    return s;
}

static void parse_cpus(const char *s) {
    char *dup = strdup(s), *save = NULL;
    for (char *tok = strtok_r(dup, ",", &save); tok && ncpus < MAXT; tok = strtok_r(NULL, ",", &save)) {
        char *dash = strchr(tok, '-');
        if (dash) {
            int a = atoi(tok), b = atoi(dash + 1);
            for (int c = a; c <= b && ncpus < MAXT; c++) cpus[ncpus++] = c;
        } else {
            cpus[ncpus++] = atoi(tok);
        }
    }
    free(dup);
}

static void usage(const char *p) {
    printf(
"Uso: %s [opciones]\n"
"  -l, --layout L    packed | padded | local | shared       [packed]\n"
"  -n, --threads N   numero de hilos (1..%d)                [4]\n"
"  -i, --iters N     incrementos por hilo                   [100000000]\n"
"  -s, --stride B    bytes entre contadores en padded       [64]\n"
"      --offset B    desplazamiento del arreglo (0..4095)   [0]\n"
"      --rmw K       atomic | volatile                      [atomic]\n"
"  -c, --cpus LIST   CPUs donde fijar los hilos, p. ej. 0,2,4,6 o 8-11\n"
"  -r, --reps R      repeticiones medidas (tras 1 de calentamiento) [5]\n"
"      --csv         una linea CSV por repeticion\n"
"      --header      imprime la cabecera CSV y termina\n", p, MAXT);
}

static const char *HEADER =
    "layout,threads,stride,rmw,cpus,iters,rep,seconds,ns_per_inc,mops_total,footprint_bytes,count_ok";

int main(int argc, char **argv) {
    static struct option lo[] = {
        {"layout", 1, 0, 'l'}, {"threads", 1, 0, 'n'}, {"iters", 1, 0, 'i'},
        {"stride", 1, 0, 's'}, {"offset", 1, 0, 'o'}, {"rmw", 1, 0, 'k'},
        {"cpus", 1, 0, 'c'}, {"reps", 1, 0, 'r'}, {"csv", 0, 0, 'C'},
        {"header", 0, 0, 'H'}, {"help", 0, 0, 'h'}, {0, 0, 0, 0}
    };
    char cpus_str[256] = "none";
    int o;
    while ((o = getopt_long(argc, argv, "l:n:i:s:c:r:h", lo, NULL)) != -1) {
        switch (o) {
        case 'l':
            if      (!strcmp(optarg, "packed")) layout = L_PACKED;
            else if (!strcmp(optarg, "padded")) layout = L_PADDED;
            else if (!strcmp(optarg, "local"))  layout = L_LOCAL;
            else if (!strcmp(optarg, "shared")) layout = L_SHARED;
            else { fprintf(stderr, "layout invalido: %s\n", optarg); return 1; }
            break;
        case 'n': nthreads = atoi(optarg); break;
        case 'i': iters = strtoull(optarg, NULL, 10); break;
        case 's': stride = atoi(optarg); break;
        case 'o': offset = atoi(optarg); break;
        case 'k':
            if      (!strcmp(optarg, "atomic"))   use_atomic = 1;
            else if (!strcmp(optarg, "volatile")) use_atomic = 0;
            else { fprintf(stderr, "rmw invalido: %s\n", optarg); return 1; }
            break;
        case 'c': parse_cpus(optarg); snprintf(cpus_str, sizeof cpus_str, "%s", optarg); break;
        case 'r': reps = atoi(optarg); break;
        case 'C': csv = 1; break;
        case 'H': puts(HEADER); return 0;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }
    if (nthreads < 1 || nthreads > MAXT) { fprintf(stderr, "hilos fuera de rango\n"); return 1; }
    if (stride < 8 || stride % 8) { fprintf(stderr, "stride debe ser multiplo de 8\n"); return 1; }
    if (offset < 0 || offset > 4095 || offset % 8) { fprintf(stderr, "offset debe ser multiplo de 8 en 0..4095\n"); return 1; }
    if ((size_t)offset + footprint() > 8192 - 8) { fprintf(stderr, "el arreglo no cabe en la arena\n"); return 1; }
    if (ncpus == 0 && !csv)
        fprintf(stderr, "aviso: hilos sin fijar (usa --cpus 0,2,4,6 para un hilo por P-core)\n");
    if (ncpus > 0 && ncpus < nthreads && !csv)
        fprintf(stderr, "aviso: menos CPUs que hilos; varios hilos compartiran CPU\n");

    arena = aligned_alloc(4096, 8192);
    if (!arena) { perror("aligned_alloc"); return 1; }

    run_once();   /* calentamiento: frecuencia, paginas, caches */

    double sum = 0, sum2 = 0;
    if (!csv)
        printf("layout=%s hilos=%d stride=%d rmw=%s cpus=%s iters=%llu memoria=%zu B\n",
               LAYOUT_NAME[layout], nthreads, layout == L_PADDED ? stride : 8,
               use_atomic ? "atomic" : "volatile", cpus_str, (unsigned long long)iters, footprint());
    for (int r = 0; r < reps; r++) {
        double s = run_once();
        uint64_t expect = (uint64_t)nthreads * iters;
        int ok = total_count() == expect;
        double ns = s * 1e9 / (double)iters;             /* ns por incremento, por hilo */
        double mops = (double)expect / s / 1e6;          /* incrementos totales por segundo */
        sum += s; sum2 += s * s;
        if (csv)
            printf("%s,%d,%d,%s,\"%s\",%llu,%d,%.6f,%.4f,%.2f,%zu,%d\n",
                   LAYOUT_NAME[layout], nthreads, layout == L_PADDED ? stride : 8,
                   use_atomic ? "atomic" : "volatile", cpus_str, (unsigned long long)iters,
                   r, s, ns, mops, footprint(), ok);
        else
            printf("  rep %d: %.4f s  %.3f ns/inc  %.1f Mops/s%s\n", r, s, ns, mops,
                   ok ? "" : "  (suma incorrecta: actualizaciones perdidas)");
    }
    if (!csv && reps > 1) {
        double mean = sum / reps, var = sum2 / reps - mean * mean;
        printf("  media %.4f s  desv %.4f s\n", mean, var > 0 ? __builtin_sqrt(var * reps / (reps - 1)) : 0.0);
    }
    free(arena);
    return 0;
}
