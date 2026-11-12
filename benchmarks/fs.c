// Prueba rapida de false sharing: contadores juntos vs separados por 64 bytes.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define ITERS 100000000UL
#define MAXT 12

struct juntos { volatile unsigned long c; };                       // 8 bytes: 8 por linea
struct separados { volatile unsigned long c; char pad[56]; };      // 64 bytes: 1 por linea

static _Alignas(64) struct juntos J[MAXT];
static _Alignas(64) struct separados S[MAXT];

static void *wj(void *a) { struct juntos *p = a; for (unsigned long i = 0; i < ITERS; i++) p->c++; return 0; }
static void *ws(void *a) { struct separados *p = a; for (unsigned long i = 0; i < ITERS; i++) p->c++; return 0; }

static double run(int n, int sep) {
    pthread_t t[MAXT]; struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < n; i++) pthread_create(&t[i], 0, sep ? ws : wj, sep ? (void *)&S[i] : (void *)&J[i]);
    for (int i = 0; i < n; i++) pthread_join(t[i], 0);
    clock_gettime(CLOCK_MONOTONIC, &b);
    return (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
}

int main(void) {
    printf("hilos  juntos(s)  separados(s)  factor\n");
    for (int n = 1; n <= 6; n++) {
        double j = run(n, 0), s = run(n, 1);
        printf("%5d  %9.3f  %12.3f  %6.2fx\n", n, j, s, j / s);
    }
}
