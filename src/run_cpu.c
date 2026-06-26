/* run_cpu.c — host-only runner. Validates the profile and serves as the
 * correctness reference for the CUDA kernel. Multithreaded with OpenMP.
 *
 * Build: gcc -O3 -fopenmp -fwrapv run_cpu.c config.c \
 *           ../cubiomes/libcubiomes.a -I../cubiomes -lm -lpthread -o seedforge_cpu
 */
#include "config.h"
#include "eval.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdatomic.h>
#include <pthread.h>
#ifdef _OPENMP
#include <omp.h>
#endif

static _Atomic uint64_t g_processed;

/* Collected hits, sorted by measured size before writing so the BIGGEST biome
 * lands at the top of the output. Shared across OpenMP threads (guarded by a
 * critical section on append). */
static Hit   *g_hits;
static size_t g_hits_len, g_hits_cap;

static void push_hit(uint64_t seed, int x, int z, int size) {
    if (g_hits_len == g_hits_cap) {
        g_hits_cap = g_hits_cap ? g_hits_cap * 2 : 1024;
        g_hits = (Hit*)realloc(g_hits, g_hits_cap * sizeof(Hit));
    }
    Hit *h = &g_hits[g_hits_len++];
    h->seed = seed; h->px = x; h->pz = z; h->size = size; h->filter_hits = 0;
}

/* Descending by size, so the largest biome is first. */
static int hit_cmp_size_desc(const void *a, const void *b) {
    int sa = ((const Hit*)a)->size, sb = ((const Hit*)b)->size;
    return (sb > sa) - (sb < sa);
}

static void print_bar(uint64_t done, uint64_t total, double secs) {
    double pct = total ? 100.0 * done / total : 0.0;
    int filled = total ? (int)(30.0 * done / total) : 0;
    char bar[31];
    for (int i = 0; i < 30; i++)
        bar[i] = (i < filled) ? '=' : (i == filled && done < total) ? '>' : ' ';
    bar[30] = '\0';
    double rate = (secs > 0.01) ? done / secs / 1e6 : 0.0;
    fprintf(stderr, "\r[%s] %5.1f%% | %.3fB/%.3fB seeds | %6.1f M/s",
            bar, pct, done / 1e9, total / 1e9, rate);
    if (secs > 0.5 && done > 0 && done < total) {
        double eta = (total - done) * secs / done;
        fprintf(stderr, " | ETA %dm%02ds", (int)(eta / 60), (int)eta % 60);
    }
    fflush(stderr);
}

typedef struct {
    uint64_t total;
    struct timespec t0;
    _Atomic int stop;
    FILE *out;
} ProgArg;

static void *progress_fn(void *arg) {
    ProgArg *a = arg;
    struct timespec iv = {0, 200000000};
    while (1) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double secs = (now.tv_sec - a->t0.tv_sec) + (now.tv_nsec - a->t0.tv_nsec) / 1e9;
        print_bar(atomic_load(&g_processed), a->total, secs);
        fflush(a->out);
        if (atomic_load(&a->stop)) break;
        nanosleep(&iv, NULL);
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr,"usage: %s profile.cfg\n",argv[0]); return 1; }
    SearchProfile p;
    if (load_profile(argv[1], &p)) return 1;
    print_profile(&p);

    FILE *out = fopen(p.out_path, "w");
    if (!out) { perror("out"); return 1; }

    uint64_t total = p.seed_end - p.seed_start;
    long found = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    atomic_store(&g_processed, 0);

    ProgArg parg;
    parg.total = total;
    parg.t0 = t0;
    parg.out  = out;
    atomic_init(&parg.stop, 0);
    pthread_t prog_th;
    pthread_create(&prog_th, NULL, progress_fn, &parg);

    #pragma omp parallel
    {
        Generator g;
        setupGenerator(&g, p.mc_version, 0);
        uint64_t local = 0;

        #pragma omp for schedule(dynamic, 4096) reduction(+:found)
        for (uint64_t i = 0; i < total; i++) {
            uint64_t seed = p.seed_start + i;
            int hx, hz, hsize;
            if (eval_seed(&p, seed, &g, &hx, &hz, &hsize)) {
                #pragma omp critical
                push_hit(seed, hx, hz, hsize);
                found++;
            }
            /* Report progress every 512 seeds per thread so the bar moves even
             * on short ranges (old 64k chunk meant ranges < 64k*nthreads showed 0%). */
            if ((++local & 0x1FF) == 0)
                atomic_fetch_add(&g_processed, (uint64_t)0x200);
        }
        atomic_fetch_add(&g_processed, local & 0xFFFF);
    }

    atomic_store(&g_processed, total);
    atomic_store(&parg.stop, 1);
    pthread_join(prog_th, NULL);
    fprintf(stderr, "\n");

    /* Rank by biggest biome, then emit "seed<TAB>x<TAB>z<TAB>size". */
    qsort(g_hits, g_hits_len, sizeof(Hit), hit_cmp_size_desc);
    size_t top_n = (p.top_n > 0) ? (size_t)p.top_n : g_hits_len;
    if (top_n > g_hits_len) top_n = g_hits_len;
    for (size_t i = 0; i < top_n; i++)
        fprintf(out, "%llu\t%d\t%d\t%d\n",
                (unsigned long long)g_hits[i].seed,
                g_hits[i].px, g_hits[i].pz, g_hits[i].size);
    free(g_hits);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    fclose(out);
    printf("checked %llu seeds, %ld hits, %.2fs, %.2fM seeds/s\n",
        (unsigned long long)total, found, secs, total / secs / 1e6);
    return 0;
}
