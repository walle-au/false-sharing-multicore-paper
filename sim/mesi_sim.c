/*
 * mesi_sim.c -- Simulador de coherencia MESI para estudiar false sharing.
 *
 * Modelo:
 *   - N nucleos, cada uno con una cache privada set-asociativa (LRU).
 *   - Bus de snooping unico y serializado: una transaccion a la vez.
 *   - Protocolo MESI con transacciones BusRd, BusRdX y BusUpgr.
 *   - Cada nucleo tiene su propio reloj; en cada paso avanza el nucleo con
 *     el reloj mas bajo (simulacion dirigida por tiempo, no round-robin fijo).
 *   - Las latencias son PARAMETROS DEL MODELO (estimaciones), no medidas.
 *
 * Carga de trabajo: cada hilo incrementa ITERS veces "su" contador.
 *   packed  : contadores contiguos (stride 8 B)      -> false sharing
 *   padded  : contadores separados por --stride B    -> sin false sharing si stride >= linea
 *   local   : acumulacion en un registro y una sola escritura al final
 *   shared  : todos incrementan EL MISMO contador    -> true sharing (referencia)
 *
 * Uso:  ./mesi_sim --help
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#define MAXCORES 64

typedef enum { ST_I = 0, ST_S, ST_E, ST_M } State;
static const char *STN = "ISEM";

typedef enum { L_PACKED, L_PADDED, L_LOCAL, L_SHARED } Layout;
static const char *LAYOUT_NAME[] = { "packed", "padded", "local", "shared" };

typedef enum { RMW_ATOMIC, RMW_LOADSTORE } RmwKind;

/* ------------------------------------------------------------------ */
/* Configuracion                                                       */
/* ------------------------------------------------------------------ */
typedef struct {
    int      cores;
    int      line;          /* tamano de linea en bytes (potencia de 2) */
    int      cache_kib;     /* tamano de la cache privada */
    int      ways;
    Layout   layout;
    int      stride;        /* bytes entre contadores en layout padded */
    int      offset;        /* desplazamiento del arreglo respecto a una frontera de 4 KiB */
    uint64_t iters;
    RmwKind  rmw;
    int      prefetch;      /* 1 = prefetch de la linea adyacente (par de 128 B alineado) */
    int      work;          /* ciclos de computo por iteracion fuera de memoria */
    int      jitter;        /* ruido aleatorio [0,jitter] ciclos por iteracion */
    unsigned seed;
    /* latencias del modelo (ciclos) */
    int lat_hit, lat_llc, lat_hitm, lat_mem, lat_upgr;
    int lat_atomic;        /* costo minimo de un RMW con lock aunque la linea ya este en M */
    double ghz;            /* solo para convertir ciclos a ns en la salida */
    int csv, header, verbose;
} Config;

/* ------------------------------------------------------------------ */
/* Estadisticas                                                        */
/* ------------------------------------------------------------------ */
typedef struct {
    uint64_t accesses, hits, misses;
    uint64_t bus_rd, bus_rdx, bus_upgr;
    uint64_t invalidations;   /* copias invalidadas en OTRAS caches */
    uint64_t hitm;            /* snoop encontro la linea en M en otro nucleo (c2c sucio) */
    uint64_t c2c_clean;       /* dato provisto por otra copia limpia / LLC */
    uint64_t mem_reads;
    uint64_t writebacks;
    uint64_t pf_issued, pf_hitm;
} Stats;

/* ------------------------------------------------------------------ */
/* Cache                                                               */
/* ------------------------------------------------------------------ */
typedef struct {
    uint64_t la;    /* direccion de linea (addr / line) */
    State    st;
    uint64_t lru;
} Way;

typedef struct {
    Way *w;
    int  sets, ways;
    uint64_t tick;
} Cache;

static Config C;
static Cache  cache[MAXCORES];
static Stats  G;                 /* globales */
static Stats  P[MAXCORES];       /* por nucleo */
static uint64_t bus_free = 0;    /* ciclo en que el bus queda libre */

static void cache_init(Cache *c) {
    int lines = C.cache_kib * 1024 / C.line;
    c->ways = C.ways;
    c->sets = lines / C.ways;
    if (c->sets < 1) c->sets = 1;
    c->w = calloc((size_t)c->sets * c->ways, sizeof(Way));
    if (!c->w) { perror("calloc"); exit(1); }
    c->tick = 0;
}

static Way *cache_find(Cache *c, uint64_t la) {
    Way *set = &c->w[(la % (uint64_t)c->sets) * c->ways];
    for (int i = 0; i < c->ways; i++)
        if (set[i].st != ST_I && set[i].la == la) return &set[i];
    return NULL;
}

static void touch(Cache *c, Way *w) { w->lru = ++c->tick; }

/* Reserva un hueco para la linea la; si expulsa una linea M cuenta writeback. */
static Way *cache_alloc(int core, uint64_t la) {
    Cache *c = &cache[core];
    Way *set = &c->w[(la % (uint64_t)c->sets) * c->ways];
    Way *v = &set[0];
    for (int i = 0; i < c->ways; i++) {
        if (set[i].st == ST_I) { v = &set[i]; break; }
        if (set[i].lru < v->lru) v = &set[i];
    }
    if (v->st == ST_M) { G.writebacks++; P[core].writebacks++; }
    v->la = la;
    v->st = ST_I;
    return v;
}

/* ------------------------------------------------------------------ */
/* Bus y snooping                                                      */
/* ------------------------------------------------------------------ */

/* Ocupa el bus durante lat ciclos a partir de 'now'. Devuelve el ciclo de fin. */
static uint64_t bus_use(uint64_t now, int lat) {
    uint64_t start = now > bus_free ? now : bus_free;
    bus_free = start + (uint64_t)lat;
    return bus_free;
}

/*
 * Snoop de los otros nucleos para la linea la.
 *   exclusive = 0 : BusRd  (M/E -> S)
 *   exclusive = 1 : BusRdX / BusUpgr (todo -> I)
 * Devuelve la latencia de la transaccion segun quien provee el dato.
 * need_data = 0 para BusUpgr (ya se tiene el dato).
 */
static int snoop(int req, uint64_t la, int exclusive, int need_data, int *had_copy, int is_pf) {
    int found_m = 0, found_clean = 0;
    for (int k = 0; k < C.cores; k++) {
        if (k == req) continue;
        Way *w = cache_find(&cache[k], la);
        if (!w) continue;
        if (w->st == ST_M) {
            found_m = 1;
            G.writebacks++; P[k].writebacks++;     /* flush del dato sucio */
        } else {
            found_clean = 1;
        }
        if (exclusive) {
            w->st = ST_I;
            G.invalidations++; P[req].invalidations++;
        } else {
            w->st = ST_S;
        }
    }
    *had_copy = found_m || found_clean;
    if (!need_data) return C.lat_upgr;
    if (found_m) {
        if (is_pf) G.pf_hitm++;
        else { G.hitm++; P[req].hitm++; }
        return C.lat_hitm;
    }
    if (found_clean) { G.c2c_clean++; P[req].c2c_clean++; return C.lat_llc; }
    G.mem_reads++; P[req].mem_reads++;
    return C.lat_mem;
}

/* Prefetch de la linea compañera del par alineado de 2 lineas (modelo del
 * "adjacent cache line prefetch"). Ocupa el bus pero no detiene al nucleo. */
static void adjacent_prefetch(int core, uint64_t la, uint64_t now) {
    uint64_t buddy = la ^ 1;
    if (cache_find(&cache[core], buddy)) return;
    int had;
    G.pf_issued++;
    int lat = snoop(core, buddy, 0, 1, &had, 1);
    G.bus_rd++;
    bus_use(now, lat);
    Way *w = cache_alloc(core, buddy);
    w->st = had ? ST_S : ST_E;
    touch(&cache[core], w);
}

/* ------------------------------------------------------------------ */
/* Accesos                                                             */
/* ------------------------------------------------------------------ */

/* Lectura. Devuelve el nuevo reloj del nucleo. */
static uint64_t do_read(int core, uint64_t addr, uint64_t now) {
    uint64_t la = addr / (uint64_t)C.line;
    Cache *c = &cache[core];
    G.accesses++; P[core].accesses++;
    Way *w = cache_find(c, la);
    if (w) {
        G.hits++; P[core].hits++;
        touch(c, w);
        return now + (uint64_t)C.lat_hit;
    }
    G.misses++; P[core].misses++;
    int had;
    int lat = snoop(core, la, 0, 1, &had, 0);
    G.bus_rd++;
    uint64_t end = bus_use(now, lat);
    w = cache_alloc(core, la);
    w->st = had ? ST_S : ST_E;
    touch(c, w);
    if (C.prefetch) adjacent_prefetch(core, la, end);
    return end;
}

/* Escritura (o RMW atomico, que pide la linea en exclusiva desde el inicio). */
static uint64_t do_write(int core, uint64_t addr, uint64_t now) {
    uint64_t la = addr / (uint64_t)C.line;
    Cache *c = &cache[core];
    G.accesses++; P[core].accesses++;
    Way *w = cache_find(c, la);
    if (w && (w->st == ST_M || w->st == ST_E)) {
        G.hits++; P[core].hits++;
        w->st = ST_M;                      /* E -> M es silencioso */
        touch(c, w);
        return now + (uint64_t)C.lat_hit;
    }
    int had;
    uint64_t end;
    if (w && w->st == ST_S) {
        /* Tengo el dato pero no el permiso: BusUpgr. Se cuenta como hit de dato. */
        G.hits++; P[core].hits++;
        snoop(core, la, 1, 0, &had, 0);
        G.bus_upgr++;
        end = bus_use(now, C.lat_upgr);
    } else {
        G.misses++; P[core].misses++;
        int lat = snoop(core, la, 1, 1, &had, 0);
        G.bus_rdx++;
        end = bus_use(now, lat);
        w = cache_alloc(core, la);
        if (C.prefetch) adjacent_prefetch(core, la, end);
    }
    w->st = ST_M;
    touch(c, w);
    return end;
}

/* ------------------------------------------------------------------ */
/* Generador pseudoaleatorio (xorshift) para el jitter                 */
/* ------------------------------------------------------------------ */
static uint32_t rng[MAXCORES];
static uint32_t xs(uint32_t *s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x; }

/* ------------------------------------------------------------------ */
/* Carga de trabajo                                                    */
/* ------------------------------------------------------------------ */
#define BASE_ADDR   0x100000ULL

static uint64_t counter_addr(int t) {
    uint64_t base = BASE_ADDR + (uint64_t)C.offset;
    switch (C.layout) {
    case L_PACKED: return base + (uint64_t)t * 8;
    case L_PADDED: return base + (uint64_t)t * (uint64_t)C.stride;
    case L_LOCAL:  return base + (uint64_t)t * 8;      /* resultado final, contiguo */
    case L_SHARED: return base;
    }
    return base;
}

static uint64_t footprint_bytes(void) {
    switch (C.layout) {
    case L_PACKED: return (uint64_t)C.cores * 8;
    case L_PADDED: return (uint64_t)C.cores * (uint64_t)C.stride;
    case L_LOCAL:  return (uint64_t)C.cores * 8;       /* solo el arreglo final */
    case L_SHARED: return 8;
    }
    return 0;
}

static uint64_t lines_touched(void) {
    /* lineas distintas que ocupa el arreglo de contadores */
    uint64_t first = counter_addr(0) / (uint64_t)C.line;
    uint64_t last  = (counter_addr(C.cores - 1) + 7) / (uint64_t)C.line;
    return last - first + 1;
}

/* Un paso (una iteracion) del hilo t. Devuelve su nuevo reloj. */
static uint64_t step(int t, uint64_t now) {
    uint64_t a = counter_addr(t);
    if (C.layout == L_LOCAL) {
        /* El acumulador vive en un registro (asi lo compila fs_bench.c):
         * no hay acceso a memoria por iteracion, solo el volcado final. */
    } else if (C.rmw == RMW_LOADSTORE) {
        now = do_read(t, a, now);
        now = do_write(t, a, now);
    } else {
        /* lock add: RFO + escritura. Un RMW con lock no baja de lat_atomic
         * ciclos (drena el store buffer y se serializa), aunque sea un hit. */
        uint64_t before = now;
        now = do_write(t, a, now);
        if (now - before < (uint64_t)C.lat_atomic) now = before + (uint64_t)C.lat_atomic;
    }
    now += (uint64_t)C.work;
    if (C.jitter) now += xs(&rng[t]) % (uint32_t)(C.jitter + 1);
    return now;
}

static uint64_t run(void) {
    uint64_t clk[MAXCORES] = {0};
    uint64_t left[MAXCORES];
    for (int t = 0; t < C.cores; t++) { left[t] = C.iters; rng[t] = C.seed * 2654435761u + (uint32_t)t + 1; }

    for (;;) {
        int t = -1;
        for (int k = 0; k < C.cores; k++)
            if (left[k] && (t < 0 || clk[k] < clk[t])) t = k;
        if (t < 0) break;
        clk[t] = step(t, clk[t]);
        left[t]--;
        if (left[t] == 0 && C.layout == L_LOCAL)        /* volcado final al arreglo */
            clk[t] = do_write(t, counter_addr(t), clk[t]);
    }
    uint64_t mk = 0;
    for (int k = 0; k < C.cores; k++) if (clk[k] > mk) mk = clk[k];
    return mk;
}

/* ------------------------------------------------------------------ */
/* CLI                                                                 */
/* ------------------------------------------------------------------ */
static void usage(const char *p) {
    printf(
"Uso: %s [opciones]\n"
"  -n, --cores N        nucleos/hilos (1..%d)            [4]\n"
"  -l, --layout L       packed | padded | local | shared  [packed]\n"
"  -s, --stride B       bytes entre contadores (padded)   [64]\n"
"  -L, --line B         tamano de linea                   [64]\n"
"  -i, --iters N        incrementos por hilo              [100000]\n"
"  -r, --rmw K          atomic | loadstore                [atomic]\n"
"  -p, --prefetch       activa prefetch de linea adyacente\n"
"      --offset B       desplazamiento del arreglo        [0]\n"
"      --cache-kib K    cache privada                     [48]\n"
"      --ways W         asociatividad                     [12]\n"
"      --work C         ciclos de computo por iteracion   [1]\n"
"      --jitter C       ruido por iteracion [0..C]        [0]\n"
"      --seed S         semilla del jitter                [1]\n"
"      --lat-hit/--lat-llc/--lat-hitm/--lat-mem/--lat-upgr C\n"
"                       latencias del modelo     [4/40/47/200/30]\n"
"      --lat-atomic C   costo minimo de un lock RMW       [18]\n"
"      --ghz F          frecuencia para convertir a ns    [4.7]\n"
"      --csv            salida en una linea CSV\n"
"      --header         imprime la cabecera CSV y termina\n"
"  -v, --verbose        estadisticas por nucleo\n", p, MAXCORES);
}

static const char *CSV_HEADER =
    "layout,cores,line,stride,rmw,prefetch,iters,makespan,cycles_per_op,"
    "accesses,hits,misses,bus_rd,bus_rdx,bus_upgr,invalidations,hitm,c2c_clean,"
    "mem_reads,writebacks,pf_issued,pf_hitm,footprint_bytes,lines_touched,ns_per_op";

int main(int argc, char **argv) {
    C = (Config){ .cores = 4, .line = 64, .cache_kib = 48, .ways = 12,
                  .layout = L_PACKED, .stride = 64, .offset = 0, .iters = 100000,
                  .rmw = RMW_ATOMIC, .prefetch = 0, .work = 1, .jitter = 0, .seed = 1,
                  .lat_hit = 4, .lat_llc = 40, .lat_hitm = 47, .lat_mem = 200, .lat_upgr = 30,
                  .lat_atomic = 18, .ghz = 4.7 };
    enum { O_OFF = 1000, O_KIB, O_WAYS, O_WORK, O_JIT, O_SEED,
           O_LHIT, O_LLLC, O_LHITM, O_LMEM, O_LUPG, O_LATM, O_GHZ, O_CSV, O_HDR };
    static struct option lo[] = {
        {"cores", 1, 0, 'n'}, {"layout", 1, 0, 'l'}, {"stride", 1, 0, 's'},
        {"line", 1, 0, 'L'}, {"iters", 1, 0, 'i'}, {"rmw", 1, 0, 'r'},
        {"prefetch", 0, 0, 'p'}, {"verbose", 0, 0, 'v'}, {"help", 0, 0, 'h'},
        {"offset", 1, 0, O_OFF}, {"cache-kib", 1, 0, O_KIB}, {"ways", 1, 0, O_WAYS},
        {"work", 1, 0, O_WORK}, {"jitter", 1, 0, O_JIT}, {"seed", 1, 0, O_SEED},
        {"lat-hit", 1, 0, O_LHIT}, {"lat-llc", 1, 0, O_LLLC}, {"lat-hitm", 1, 0, O_LHITM},
        {"lat-mem", 1, 0, O_LMEM}, {"lat-upgr", 1, 0, O_LUPG},
        {"lat-atomic", 1, 0, O_LATM}, {"ghz", 1, 0, O_GHZ},
        {"csv", 0, 0, O_CSV}, {"header", 0, 0, O_HDR}, {0, 0, 0, 0}
    };
    int o;
    while ((o = getopt_long(argc, argv, "n:l:s:L:i:r:pvh", lo, NULL)) != -1) {
        switch (o) {
        case 'n': C.cores = atoi(optarg); break;
        case 'l':
            if      (!strcmp(optarg, "packed")) C.layout = L_PACKED;
            else if (!strcmp(optarg, "padded")) C.layout = L_PADDED;
            else if (!strcmp(optarg, "local"))  C.layout = L_LOCAL;
            else if (!strcmp(optarg, "shared")) C.layout = L_SHARED;
            else { fprintf(stderr, "layout invalido: %s\n", optarg); return 1; }
            break;
        case 's': C.stride = atoi(optarg); break;
        case 'L': C.line = atoi(optarg); break;
        case 'i': C.iters = strtoull(optarg, NULL, 10); break;
        case 'r':
            if      (!strcmp(optarg, "atomic"))    C.rmw = RMW_ATOMIC;
            else if (!strcmp(optarg, "loadstore")) C.rmw = RMW_LOADSTORE;
            else { fprintf(stderr, "rmw invalido: %s\n", optarg); return 1; }
            break;
        case 'p': C.prefetch = 1; break;
        case 'v': C.verbose = 1; break;
        case O_OFF:  C.offset = atoi(optarg); break;
        case O_KIB:  C.cache_kib = atoi(optarg); break;
        case O_WAYS: C.ways = atoi(optarg); break;
        case O_WORK: C.work = atoi(optarg); break;
        case O_JIT:  C.jitter = atoi(optarg); break;
        case O_SEED: C.seed = (unsigned)strtoul(optarg, NULL, 10); break;
        case O_LHIT: C.lat_hit = atoi(optarg); break;
        case O_LLLC: C.lat_llc = atoi(optarg); break;
        case O_LHITM: C.lat_hitm = atoi(optarg); break;
        case O_LMEM: C.lat_mem = atoi(optarg); break;
        case O_LUPG: C.lat_upgr = atoi(optarg); break;
        case O_LATM: C.lat_atomic = atoi(optarg); break;
        case O_GHZ:  C.ghz = atof(optarg); break;
        case O_CSV:  C.csv = 1; break;
        case O_HDR:  puts(CSV_HEADER); return 0;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }
    if (C.cores < 1 || C.cores > MAXCORES) { fprintf(stderr, "cores fuera de rango\n"); return 1; }
    if (C.line < 8 || (C.line & (C.line - 1))) { fprintf(stderr, "line debe ser potencia de 2 >= 8\n"); return 1; }
    if (C.stride < 8) { fprintf(stderr, "stride debe ser >= 8\n"); return 1; }

    for (int k = 0; k < C.cores; k++) cache_init(&cache[k]);
    uint64_t mk = run();
    uint64_t ops = C.iters * (uint64_t)C.cores;
    double cpo = (double)mk / (double)C.iters;   /* ciclos por incremento, por hilo */

    if (C.csv) {
        printf("%s,%d,%d,%d,%s,%d,%llu,%llu,%.3f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.3f\n",
               LAYOUT_NAME[C.layout], C.cores, C.line, C.layout == L_PADDED ? C.stride : 8,
               C.rmw == RMW_ATOMIC ? "atomic" : "loadstore", C.prefetch,
               (unsigned long long)C.iters, (unsigned long long)mk, cpo,
               (unsigned long long)G.accesses, (unsigned long long)G.hits, (unsigned long long)G.misses,
               (unsigned long long)G.bus_rd, (unsigned long long)G.bus_rdx, (unsigned long long)G.bus_upgr,
               (unsigned long long)G.invalidations, (unsigned long long)G.hitm, (unsigned long long)G.c2c_clean,
               (unsigned long long)G.mem_reads, (unsigned long long)G.writebacks,
               (unsigned long long)G.pf_issued, (unsigned long long)G.pf_hitm,
               (unsigned long long)footprint_bytes(), (unsigned long long)lines_touched(), cpo / C.ghz);
    } else {
        printf("Layout %-7s | %d nucleos | linea %d B | stride %d B | %s | prefetch %s\n",
               LAYOUT_NAME[C.layout], C.cores, C.line, C.layout == L_PADDED ? C.stride : 8,
               C.rmw == RMW_ATOMIC ? "atomic" : "loadstore", C.prefetch ? "on" : "off");
        printf("  incrementos totales : %llu (%llu por hilo)\n", (unsigned long long)ops, (unsigned long long)C.iters);
        printf("  makespan (estimado) : %llu ciclos  (%.2f ciclos/incremento por hilo = %.2f ns a %.1f GHz)\n",
               (unsigned long long)mk, cpo, cpo / C.ghz, C.ghz);
        printf("  accesos / hits / miss: %llu / %llu / %llu\n",
               (unsigned long long)G.accesses, (unsigned long long)G.hits, (unsigned long long)G.misses);
        printf("  BusRd / BusRdX / BusUpgr: %llu / %llu / %llu\n",
               (unsigned long long)G.bus_rd, (unsigned long long)G.bus_rdx, (unsigned long long)G.bus_upgr);
        printf("  invalidaciones      : %llu\n", (unsigned long long)G.invalidations);
        printf("  HITM (c2c sucio)    : %llu\n", (unsigned long long)G.hitm);
        printf("  c2c limpio / memoria: %llu / %llu\n", (unsigned long long)G.c2c_clean, (unsigned long long)G.mem_reads);
        printf("  writebacks          : %llu\n", (unsigned long long)G.writebacks);
        if (C.prefetch)
            printf("  prefetch emitidos / con HITM: %llu / %llu\n", (unsigned long long)G.pf_issued, (unsigned long long)G.pf_hitm);
        printf("  memoria de contadores: %llu B en %llu linea(s)\n",
               (unsigned long long)footprint_bytes(), (unsigned long long)lines_touched());
        if (C.verbose) {
            printf("  %-5s %10s %10s %10s %10s %10s\n", "core", "accesos", "misses", "HITM", "inval.", "estado");
            for (int k = 0; k < C.cores; k++) {
                Way *w = cache_find(&cache[k], counter_addr(k) / (uint64_t)C.line);
                printf("  %-5d %10llu %10llu %10llu %10llu %10c\n", k,
                       (unsigned long long)P[k].accesses, (unsigned long long)P[k].misses,
                       (unsigned long long)P[k].hitm, (unsigned long long)P[k].invalidations,
                       w ? STN[w->st] : 'I');
            }
        }
    }
    return 0;
}
