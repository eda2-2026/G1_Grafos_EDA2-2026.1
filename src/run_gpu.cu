/* run_gpu.cu — CUDA seed search. Reuses the exact same eval.h as the CPU
 * path (compiled with SF_DEVICE), so correctness is guaranteed identical.
 *
 * Design notes for performance:
 *  - Grid-stride loop: each thread processes SF_SEEDS_PER_THREAD seeds, then
 *    strides by the whole grid. Keeps all SMs saturated regardless of range.
 *  - Per-thread Generator lives in registers/local mem; setupGenerator is
 *    called ONCE per thread, applySeed per seed. setup is the costly part.
 *  - Filters are pre-sorted host-side by cost, so the cheap branchy reject
 *    happens before any expensive biome sampling — minimizes divergence cost.
 *  - Results go to a global Hit buffer guarded by a HitCtrl spinlock struct
 *    that keeps the top SF_MAX_HITS hits by measured biome size.
 *  - Multi-GPU: uses a shared atomic work cursor so faster GPUs automatically
 *    pull more batches — no idle time on mismatched cards (e.g. 5060 Ti + 3060).
 *
 * Build (per your dual-Ada/Ampere box):
 *   nvcc -O3 -DSF_DEVICE -Xcompiler -fwrapv \
 *        -gencode arch=compute_89,code=sm_89 \   # RTX 5060 Ti (Ada)
 *        -gencode arch=compute_86,code=sm_86 \   # RTX 3060  (Ampere)
 *        run_gpu.cu config.c ../cubiomes/*.c -I../cubiomes -o seedforge_gpu
 *   (compile the cubiomes .c files with nvcc so they get device codegen,
 *    OR build a relocatable-device cubiomes; see Makefile.)
 */
#include "config.h"
#include "eval.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <atomic>
#include <cuda_runtime.h>

#define CUDA_OK(call) do { cudaError_t _e=(call); if(_e!=cudaSuccess){ \
    fprintf(stderr,"CUDA %s:%d %s\n",__FILE__,__LINE__,cudaGetErrorString(_e)); \
    exit(1);} } while(0)

static std::atomic<uint64_t> g_processed{0};

/* Dynamic work queue: GPUs pull batches from this cursor until it hits g_work_end.
 * Faster cards naturally take more batches — no idle time on mismatched hardware. */
static std::atomic<uint64_t> g_work_cursor{0};
static uint64_t g_work_end = 0;

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

struct ProgArg {
    uint64_t total;
    struct timespec t0;
    std::atomic<int> stop;
    ProgArg() : total(0), stop(0) {}
};

static void *progress_fn(void *arg) {
    ProgArg *a = (ProgArg*)arg;
    struct timespec iv = {0, 200000000};
    while (1) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double secs = (now.tv_sec - a->t0.tv_sec) + (now.tv_nsec - a->t0.tv_nsec) / 1e9;
        print_bar(g_processed.load(std::memory_order_relaxed), a->total, secs);
        if (a->stop.load(std::memory_order_relaxed)) break;
        nanosleep(&iv, NULL);
    }
    return NULL;
}

/* --------------- bounded top-N-by-size result buffer ---------------
 * The device keeps the LARGEST SF_MAX_HITS hits, not the first ones. Until the
 * buffer fills it's a plain append; once full, a new hit replaces the smallest
 * kept one if it's bigger. So the global maximum is never lost no matter how low
 * `min_area` is set or how many seeds qualify — the kernel also never stops
 * early. Replacements are serialised by a global spinlock but become rare once
 * the running threshold (the current minimum) climbs, so throughput holds. */
struct HitCtrl {
    unsigned int cap;       /* actual buffer capacity (top_n, <= SF_MAX_HITS)  */
    unsigned int fill;      /* slots claimed; grows past cap once overflowing  */
    unsigned int written;   /* fill-phase slots actually written (<= cap)      */
    unsigned int lock;      /* spinlock guarding the replacement path          */
    int          min_size;  /* smallest size currently kept (valid once full)  */
    unsigned int min_idx;   /* slot holding that minimum                       */
    int          min_valid; /* 1 once min_size/min_idx have been computed      */
};

/* Rescan the full buffer for the current minimum size. Called only under lock. */
__device__ static void hitctrl_scan_min(Hit *hits, HitCtrl *c)
{
    int m = hits[0].size; unsigned int mi = 0;
    for (unsigned int k = 1; k < c->cap; k++)
        if (hits[k].size < m) { m = hits[k].size; mi = k; }
    c->min_size = m; c->min_idx = mi;
}

__device__ static void hitctrl_insert(Hit *hits, HitCtrl *c, uint64_t seed,
                                      int hx, int hz, int hsize, int nf)
{
    unsigned int idx = atomicAdd(&c->fill, 1u);
    if (idx < c->cap) {                       /* fill phase: take a fresh slot */
        hits[idx].seed = seed; hits[idx].px = hx; hits[idx].pz = hz;
        hits[idx].size = hsize; hits[idx].filter_hits = nf;
        __threadfence();
        atomicAdd(&c->written, 1u);
        return;
    }
    /* Buffer full. Fast reject against the current threshold; a stale (low) read
     * of min_size is safe because it only ever RISES, so we never wrongly drop a
     * hit — worst case is one needless lock acquisition. */
    if (((volatile HitCtrl*)c)->min_valid &&
        hsize <= ((volatile HitCtrl*)c)->min_size)
        return;
    while (atomicCAS(&c->lock, 0u, 1u) != 0u) { /* spin (rare) */ }
    __threadfence();
    /* All fill slots must be written before we can scan for the minimum. */
    while (atomicAdd(&c->written, 0u) < c->cap) { /* spin briefly */ }
    if (!c->min_valid) { hitctrl_scan_min(hits, c); c->min_valid = 1; }
    if (hsize > c->min_size) {
        unsigned int mi = c->min_idx;
        hits[mi].seed = seed; hits[mi].px = hx; hits[mi].pz = hz;
        hits[mi].size = hsize; hits[mi].filter_hits = nf;
        hitctrl_scan_min(hits, c);            /* recompute the new minimum */
    }
    __threadfence();
    atomicExch(&c->lock, 0u);
}

/* SearchProfile in constant memory: broadcast-cached, avoids parameter-space
 * overflow (CUDA limit is 4096 bytes; the struct exceeds that when passed by
 * value). Each GPU worker copies its profile here before launching kernels. */
__constant__ SearchProfile d_profile;

/* ------------------------- kernel ------------------------- */
__global__ void search_kernel(uint64_t base, uint64_t count,
                              Hit *hits, HitCtrl *ctrl)
{
    uint64_t tid   = (uint64_t)blockIdx.x * blockDim.x + threadIdx.x;
    uint64_t stride = (uint64_t)gridDim.x * blockDim.x;

    Generator g;
    setupGenerator(&g, d_profile.mc_version, 0);   /* once per thread */

    for (uint64_t i = tid; i < count; i += stride) {
        uint64_t seed = base + i;
        int hx, hz, hsize;
        if (eval_seed(&d_profile, seed, &g, &hx, &hz, &hsize))
            hitctrl_insert(hits, ctrl, seed, hx, hz, hsize, d_profile.num_filters);
    }
}

/* ----------------- per-GPU worker thread ------------------ */
typedef struct {
    int device;
    SearchProfile prof;
    uint64_t batch_size;        /* seeds per kernel launch; pulled dynamically */
    Hit  *host_hits;
    unsigned int host_n;        /* hits kept = min(total_qual, SF_MAX_HITS)   */
    uint64_t total_qual;        /* total seeds that passed all filters        */
    uint64_t seeds_done;        /* seeds actually processed by this GPU       */
    char dev_name[256];
} GpuJob;

static void *gpu_worker(void *arg) {
    GpuJob *j = (GpuJob*)arg;
    CUDA_OK(cudaSetDevice(j->device));

    /* Upload biome-tree tables to this device's global memory. */
    biome_upload_device();

    /* Upload this GPU's profile to its own constant memory (per-device). */
    CUDA_OK(cudaMemcpyToSymbol(d_profile, &j->prof, sizeof(j->prof)));

    /* Increase per-thread stack for recursive biome-tree traversal and for the
     * ~700B frame from getStructureConfig's local StructureConfig vars. The
     * F_BIOME_SIZE / F_CHEESE_CAVE flood fills keep a uint16 stack + visited
     * bitmap in eval_filter's (un-inlined, -rdc=true) frame, sized by the small
     * *device* cap SF_FLOOD_MAX_CELLS_GPU — make room or the frame overflows.
     * F_CHEESE_CAVE also allocates 10 PerlinNoise structs (~3140 B) on-stack for
     * the full-precision cave_cheese DoublePerlinNoise. */
    size_t flood_bytes = (size_t)SF_FLOOD_MAX_CELLS_GPU * 2
                       + (SF_FLOOD_MAX_CELLS_GPU + 7) / 8;
    CUDA_OK(cudaDeviceSetLimit(cudaLimitStackSize, 32768 + flood_bytes + 6144));

    cudaDeviceProp pp; CUDA_OK(cudaGetDeviceProperties(&pp, j->device));
    int blocks = pp.multiProcessorCount * 32;   /* heavy oversubscription */
    strncpy(j->dev_name, pp.name, 255);

    /* Per-GPU buffer holds top_n seeds by size. If top_n=0 default to SF_MAX_HITS.
     * With N GPUs each keeping top_n best, the host picks the global top_n from
     * the combined (N*top_n) candidates — the global best is always preserved. */
    int top_n = j->prof.top_n > 0 ? j->prof.top_n : SF_MAX_HITS;
    if (top_n > SF_MAX_HITS) top_n = SF_MAX_HITS;

    Hit *d_hits; HitCtrl *d_ctrl;
    CUDA_OK(cudaMalloc(&d_hits, sizeof(Hit) * top_n));
    CUDA_OK(cudaMemset(d_hits, 0, sizeof(Hit) * top_n));

    HitCtrl h_ctrl;
    memset(&h_ctrl, 0, sizeof(h_ctrl));
    h_ctrl.cap = (unsigned int)top_n;
    CUDA_OK(cudaMalloc(&d_ctrl, sizeof(HitCtrl)));
    CUDA_OK(cudaMemcpy(d_ctrl, &h_ctrl, sizeof(HitCtrl), cudaMemcpyHostToDevice));

    cudaStream_t s; CUDA_OK(cudaStreamCreate(&s));

    /* Dynamic work stealing: pull batches from the shared cursor until exhausted.
     * Fast GPUs pull more batches automatically — no idle time waiting for the
     * slower card to catch up. No early stop: buffer keeps top SF_MAX_HITS by
     * size, so running the whole range only ever improves the kept set. */
    uint64_t seeds_done = 0;
    uint64_t batch = j->batch_size;
    while (1) {
        uint64_t base = g_work_cursor.fetch_add(batch, std::memory_order_relaxed);
        if (base >= g_work_end) break;
        uint64_t cnt = (base + batch <= g_work_end) ? batch : g_work_end - base;
        search_kernel<<<blocks, SF_THREADS_PER_BLOCK, 0, s>>>(
            base, cnt, d_hits, d_ctrl);
        CUDA_OK(cudaPeekAtLastError());
        CUDA_OK(cudaStreamSynchronize(s));
        g_processed.fetch_add(cnt, std::memory_order_relaxed);
        seeds_done += cnt;
    }
    j->seeds_done = seeds_done;

    HitCtrl ctrl;
    CUDA_OK(cudaMemcpy(&ctrl, d_ctrl, sizeof(HitCtrl), cudaMemcpyDeviceToHost));
    j->total_qual = ctrl.fill;
    unsigned int n = ctrl.fill < ctrl.cap ? ctrl.fill : ctrl.cap;
    j->host_hits = (Hit*)malloc(sizeof(Hit) * (n ? n : 1));
    CUDA_OK(cudaMemcpy(j->host_hits, d_hits, sizeof(Hit)*n,
                       cudaMemcpyDeviceToHost));
    j->host_n = n;

    cudaFree(d_hits); cudaFree(d_ctrl); cudaStreamDestroy(s);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr,"usage: %s profile.cfg\n",argv[0]); return 1; }
    SearchProfile p;
    if (load_profile(argv[1], &p)) return 1;

    /* default to all visible GPUs if none specified */
    if (p.num_gpus == 0) {
        int n; CUDA_OK(cudaGetDeviceCount(&n));
        for (int i=0;i<n && i<8;i++) p.gpus[p.num_gpus++]=i;
    }
    print_profile(&p);


    uint64_t total = p.seed_end - p.seed_start;

    /* Set up the shared work queue. batch_size: ~400 total batches across all GPUs
     * so the progress bar updates smoothly, but at least 100K seeds per launch so
     * kernel-launch overhead stays negligible even for cheap filters. */
    g_work_cursor.store(p.seed_start, std::memory_order_relaxed);
    g_work_end = p.seed_end;
    uint64_t batch_size = total / 400 + 1;
    if (batch_size < 100000) batch_size = 100000;

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    ProgArg parg;
    parg.total = total;
    parg.t0 = t0;
    pthread_t prog_th;
    pthread_create(&prog_th, NULL, progress_fn, &parg);

    GpuJob   jobs[8];
    pthread_t th[8];
    for (int i=0;i<p.num_gpus;i++) {
        jobs[i].device     = p.gpus[i];
        jobs[i].prof       = p;
        jobs[i].batch_size = batch_size;
        jobs[i].host_hits  = NULL; jobs[i].host_n = 0;
        jobs[i].total_qual = 0; jobs[i].seeds_done = 0;
        jobs[i].dev_name[0] = '\0';
        pthread_create(&th[i], NULL, gpu_worker, &jobs[i]);
    }

    /* Gather every GPU's hits into one array so we can rank globally by the
     * measured biome size before writing (biggest first). */
    Hit *all = NULL; long found = 0, cap = 0;
    for (int i=0;i<p.num_gpus;i++) {
        pthread_join(th[i], NULL);
        for (unsigned int k=0;k<jobs[i].host_n;k++) {
            if (found == cap) {
                cap = cap ? cap*2 : 1024;
                all = (Hit*)realloc(all, cap*sizeof(Hit));
            }
            all[found++] = jobs[i].host_hits[k];
        }
        free(jobs[i].host_hits);
    }
    qsort(all, found, sizeof(Hit), [](const void *a, const void *b)->int{
        int sa=((const Hit*)a)->size, sb=((const Hit*)b)->size;
        return (sb>sa)-(sb<sa);   /* descending by size */
    });

    /* Write only the top_n globally best hits (already sorted desc by size). */
    int top_n = p.top_n > 0 ? p.top_n : SF_MAX_HITS;
    long write_n = found < (long)top_n ? found : (long)top_n;

    FILE *out = fopen(p.out_path, "w");
    for (long k = 0; k < write_n; k++)
        fprintf(out, "%llu\t%d\t%d\t%d\n",
            (unsigned long long)all[k].seed, all[k].px, all[k].pz, all[k].size);
    fclose(out);
    free(all);

    g_processed.store(total, std::memory_order_relaxed);
    parg.stop.store(1, std::memory_order_relaxed);
    pthread_join(prog_th, NULL);
    fprintf(stderr, "\n");

    uint64_t total_qual = 0;
    for (int i=0;i<p.num_gpus;i++) total_qual += jobs[i].total_qual;

    if (p.verbose) {
        for (int i=0;i<p.num_gpus;i++)
            fprintf(stderr, "[gpu %d] %s: kept %u of %llu qualifiers over %llu seeds (%.1f%%)\n",
                jobs[i].device, jobs[i].dev_name, jobs[i].host_n,
                (unsigned long long)jobs[i].total_qual,
                (unsigned long long)jobs[i].seeds_done,
                total ? 100.0 * jobs[i].seeds_done / total : 0.0);
    }

    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("checked %llu seeds across %d GPU(s), %llu qualifiers, wrote top %ld, %.2fs\n",
        (unsigned long long)total, p.num_gpus,
        (unsigned long long)total_qual, write_n, secs);
    if (total_qual > (uint64_t)write_n)
        printf("NOTE: %llu seeds qualified; kept the %ld LARGEST by size. "
               "Raise top_n in the cfg or min_area to change what is kept.\n",
               (unsigned long long)total_qual, write_n);
    return 0;
}
