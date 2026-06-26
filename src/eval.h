/* eval.h — the one evaluator, compiled for BOTH host and device.
 *
 * Include with SF_DEVICE defined (from a .cu translation unit) to get
 * __host__ __device__ qualifiers; otherwise it's plain host C. This is the
 * cubiomes-on-GPU trick: the generation math is branchy but pure, so the
 * exact same function services the CPU reference path and the CUDA kernel.
 */
#ifndef SEEDFORGE_EVAL_H
#define SEEDFORGE_EVAL_H

#include "config.h"
#include "generator.h"
#include "finders.h"
#include "loot.h"

#ifdef SF_DEVICE
  #define SF_FN __host__ __device__ static inline
#else
  #define SF_FN static inline
#endif

/* Flood-fill scratch size for the current compile pass: small on the device
 * (per-thread local memory is precious), large on the host. __CUDA_ARCH__ is
 * defined only in nvcc's device pass, so the kernel gets the small cap while
 * both gcc and nvcc's host pass get the big one. See config.h for the rationale. */
#ifdef __CUDA_ARCH__
  #define SF_FLOOD_N SF_FLOOD_MAX_CELLS_GPU
#else
  #define SF_FLOOD_N SF_FLOOD_MAX_CELLS
#endif

/* Single-point biome query: on GPU uses stack-allocated genBiomeNoiseScaled
 * (no malloc); on CPU falls back to the standard getBiomeAt. */
SF_FN int sf_getBiomeAt(const Generator *g, int x, int y, int z)
{
#ifdef __CUDA_ARCH__
    int id = -1;
    Range r = {1, x, z, 1, 1, y, 1};
    genBiomeNoiseScaled(&g->bn, &id, r, g->sha);
    return id;
#else
    return getBiomeAt(g, 1, x, y, z);
#endif
}

/* Does biome id `b` satisfy this filter's target? When a category/match set is
 * present (num_match_biomes>0) any member counts — this is what lets a single
 * filter span a whole biome group (e.g. all the mountain biomes). Otherwise it
 * falls back to the single `biome` field. */
SF_FN int sf_biome_matches(const Filter *f, int b)
{
    if (f->num_match_biomes > 0) {
        for (int i = 0; i < f->num_match_biomes; i++)
            if (f->match_biomes[i] == b) return 1;
        return 0;
    }
    return b == f->biome;
}

/* Resolve the scan center for biome filters: world spawn when center_spawn is
 * set (heavy — getSpawn), otherwise the filter's own (x,z) anchor. */
SF_FN void sf_scan_center(const Generator *g, const Filter *f, int *cx, int *cz)
{
    if (f->center_spawn) {
        Pos sp = getSpawn(g);
        *cx = sp.x; *cz = sp.z;
    } else {
        *cx = f->x; *cz = f->z;
    }
}

/* Evaluate one filter against an already-seeded generator g.
 * Returns 1 on pass, 0 on fail. On pass, hx/hz get a representative pos and
 * *hsize gets a measured area in blocks for the size/area filters (0 otherwise,
 * so seeds can be ranked by biggest-biome size). */
SF_FN int eval_filter(const Generator *g, const Filter *f,
                      int *hx, int *hz, int *hsize)
{
    *hsize = 0;
    switch (f->kind) {

    case F_BIOME_AT: {
        int cx, cz; sf_scan_center(g, f, &cx, &cz);
        int b = sf_getBiomeAt(g, cx, f->y, cz);
        if (b == f->biome) { *hx = cx; *hz = cz; return 1; }
        return 0;
    }

    case F_BIOME_RADIUS: {
        int cx, cz; sf_scan_center(g, f, &cx, &cz);
        int found = 0;
        int step = f->step > 0 ? f->step : 16;
        for (int dz = -f->radius; dz <= f->radius; dz += step) {
            for (int dx = -f->radius; dx <= f->radius; dx += step) {
                int b = sf_getBiomeAt(g, cx + dx, f->y, cz + dz);
                if (sf_biome_matches(f, b)) {
                    if (!found) { *hx = cx + dx; *hz = cz + dz; }
                    if (++found >= (f->want_count > 0 ? f->want_count : 1))
                        return 1;
                }
            }
        }
        return 0;
    }

    case F_BIOME_AREA: {
        /* "Big biome" test: count matching grid cells within radius and pass
         * only if at least want_count cells match. Each cell covers step*step
         * blocks, so want_count * step^2 is the approximate minimum area.
         * Reports the scan center (e.g. spawn) as the hit position. */
        int cx, cz; sf_scan_center(g, f, &cx, &cz);
        int step = f->step > 0 ? f->step : 16;
        int need = f->want_count > 0 ? f->want_count : 1;
        int found = 0;
        for (int dz = -f->radius; dz <= f->radius; dz += step) {
            for (int dx = -f->radius; dx <= f->radius; dx += step) {
                int b = sf_getBiomeAt(g, cx + dx, f->y, cz + dz);
                if (sf_biome_matches(f, b) && ++found >= need) {
                    *hx = cx; *hz = cz; *hsize = found * step * step; return 1;
                }
            }
        }
        return 0;
    }

    case F_BIOME_SIZE: {
        /* TRUE "biggest biome" measurement: flood fill the largest CONTIGUOUS
         * blob of the target category within the scan window. Unlike F_BIOME_AREA
         * (which just counts matching samples anywhere in the box, so scattered
         * patches inflate the count), this follows connectivity at grid spacing
         * `step` and returns the single largest connected component's area. This
         * is the technique behind record "largest biome" seed hunts: a coarse
         * box prefilter (F_BIOME_AREA) culls the seed space, then this flood fill
         * measures survivors precisely so they can be ranked by real size.
         *
         * Each grid cell is sampled at most once (visited bitmap) and stands for
         * ~step*step blocks, so area = cells * step * step. The window is clamped
         * to SF_FLOOD_MAX_CELLS cells; widen coverage with a coarser `step`. */
        int cx, cz; sf_scan_center(g, f, &cx, &cz);
        int step = f->step > 0 ? f->step : 16;
        int half = f->radius > 0 ? f->radius / step : 0;
        if (half < 0) half = 0;
        /* shrink the window until the whole grid fits the per-thread scratch */
        while (half > 0 && (long)(2*half+1)*(2*half+1) > SF_FLOOD_N)
            half--;
        int side   = 2 * half + 1;
        int ncells = side * side;

        uint16_t stk[SF_FLOOD_N];
        unsigned char seen[(SF_FLOOD_N + 7) / 8];
        int nbytes = (ncells + 7) / 8;
        for (int i = 0; i < nbytes; i++) seen[i] = 0;
        #define SF_SEEN(i)     (seen[(i) >> 3] & (1u << ((i) & 7)))
        #define SF_MARK(i)     (seen[(i) >> 3] |= (1u << ((i) & 7)))

        int best = 0, bestx = cx, bestz = cz;
        for (int s = 0; s < ncells; s++) {
            if (SF_SEEN(s)) continue;
            SF_MARK(s);
            int sxi = s % side, szi = s / side;
            int wx = cx + (sxi - half) * step;
            int wz = cz + (szi - half) * step;
            if (!sf_biome_matches(f, sf_getBiomeAt(g, wx, f->y, wz)))
                continue;
            /* iterative flood fill of this connected component */
            int top = 0; stk[top++] = (uint16_t)s;
            int count = 0; long sumx = 0, sumz = 0;
            while (top > 0) {
                int c = stk[--top];
                int cxi = c % side, czi = c / side;
                count++;
                sumx += cx + (cxi - half) * step;
                sumz += cz + (czi - half) * step;
                int nbi[4] = { (cxi+1 < side) ? c+1 : -1,
                               (cxi   > 0   ) ? c-1 : -1,
                               (czi+1 < side) ? c+side : -1,
                               (czi   > 0   ) ? c-side : -1 };
                for (int k = 0; k < 4; k++) {
                    int n = nbi[k];
                    if (n < 0 || SF_SEEN(n)) continue;
                    SF_MARK(n);
                    int nxi = n % side, nzi = n / side;
                    int nx = cx + (nxi - half) * step;
                    int nz = cz + (nzi - half) * step;
                    if (sf_biome_matches(f, sf_getBiomeAt(g, nx, f->y, nz)))
                        stk[top++] = (uint16_t)n;
                }
            }
            if (count > best) {
                best = count;
                bestx = (int)(sumx / count);
                bestz = (int)(sumz / count);
            }
        }
        #undef SF_SEEN
        #undef SF_MARK

        int area = best * step * step;
        int need = f->min_area > 0 ? f->min_area : 1;
        *hx = bestx; *hz = bestz; *hsize = area;
        return area >= need;
    }

    case F_CHEESE_CAVE: {
        /* Measures the cheese cave region at the given underground Y level using
         * the 'minecraft:cave_cheese' DoublePerlinNoise sampled from the world seed.
         *
         * Noise seed: same XOR pattern as climate noises in setBiomeSeed().
         *   lo ^ MD5("minecraft:cave_cheese")[0:8]  = seed_lo ^ 0xb159093bc7baaa50
         *   hi ^ MD5("minecraft:cave_cheese")[8:16] = seed_hi ^ 0x53abc45424417c20
         * Noise: firstOctave=-9, amplitudes=[0.5,0.25,0.125,0.0625,0.03125].
         * Sampled at block coordinates (xz_scale=1.0, y_scale=1.0).
         * Range ~[-1.39, +1.39]; cave_threshold=0.5 selects developed cave cores.
         *
         * GPU path: area count (no flood fill) over the full radius using at most
         * 33x33=1089 samples (step is doubled as needed to fit); fast prefilter,
         * does NOT measure connectivity.  CPU path: 2D flood fill for the largest
         * CONNECTED high-noise region (precise measurement). */
        Xoroshiro sxr; xSetSeed(&sxr, g->seed);
        uint64_t xlo = xNextLong(&sxr), xhi = xNextLong(&sxr);

        DoublePerlinNoise cheese_noise;
        PerlinNoise cheese_oct[10]; /* 2x5 non-zero amplitudes */
        static const double amp[] = {0.5, 0.25, 0.125, 0.0625, 0.03125};
        Xoroshiro cxr;
        cxr.lo = xlo ^ 0xb159093bc7baaa50ULL;
        cxr.hi = xhi ^ 0x53abc45424417c20ULL;
        xDoublePerlinInit(&cheese_noise, &cxr, cheese_oct, amp, -9, 5, -1);

        int cx, cz; sf_scan_center(g, f, &cx, &cz);
        int step = f->step > 0 ? f->step : 16;
        double thr = (f->cave_threshold >= 0.0) ? f->cave_threshold : 0.5;
        int fy = f->y;
        int need = f->min_area > 0 ? f->min_area : 1;

#ifdef __CUDA_ARCH__
        /* GPU: count all high-noise cells over the full radius without flood fill.
         * Coarsen step so the grid stays within 33x33=1089 samples while still
         * covering the entire requested radius — no cave at any position is missed. */
        int gpu_step = step > 0 ? step : 16;
        while (f->radius > 0 && f->radius / gpu_step > 16) gpu_step *= 2;
        int gpu_half = f->radius > 0 ? f->radius / gpu_step : 16;
        if (gpu_half > 16) gpu_half = 16;
        int count = 0;
        for (int gi = -gpu_half; gi <= gpu_half; gi++)
        for (int gj = -gpu_half; gj <= gpu_half; gj++) {
            double v = sampleDoublePerlin(&cheese_noise,
                (double)(cx + gi*gpu_step), (double)fy, (double)(cz + gj*gpu_step));
            if (v > thr) count++;
        }
        int area = count * gpu_step * gpu_step;
        *hx = cx; *hz = cz; *hsize = area;
        return area >= need;
#else
        /* CPU: flood fill for the largest connected high-noise component. */
        int half = f->radius > 0 ? f->radius / step : 0;
        while (half > 0 && (long)(2*half+1)*(2*half+1) > SF_FLOOD_N) half--;
        int side   = 2 * half + 1;
        int ncells = side * side;

        uint16_t cstk[SF_FLOOD_N];
        unsigned char cseen[(SF_FLOOD_N + 7) / 8];
        int cnbytes = (ncells + 7) / 8;
        for (int i = 0; i < cnbytes; i++) cseen[i] = 0;
        #define CC_SEEN(i)  (cseen[(i)>>3] & (1u<<((i)&7)))
        #define CC_MARK(i)  (cseen[(i)>>3] |= (1u<<((i)&7)))

        int best = 0, bestx = cx, bestz = cz;
        for (int s = 0; s < ncells; s++) {
            if (CC_SEEN(s)) continue;
            CC_MARK(s);
            int sxi = s % side, szi = s / side;
            int wx = cx + (sxi - half) * step;
            int wz = cz + (szi - half) * step;
            double v = sampleDoublePerlin(&cheese_noise, (double)wx, (double)fy, (double)wz);
            if (v <= thr) continue;
            int top = 0; cstk[top++] = (uint16_t)s;
            int cnt = 0; long sumx = 0, sumz = 0;
            while (top > 0) {
                int c = cstk[--top];
                int cxi = c % side, czi = c / side;
                cnt++;
                sumx += cx + (cxi - half) * step;
                sumz += cz + (czi - half) * step;
                int nbi[4] = { (cxi+1 < side) ? c+1     : -1,
                               (cxi   > 0   ) ? c-1     : -1,
                               (czi+1 < side) ? c+side  : -1,
                               (czi   > 0   ) ? c-side  : -1 };
                for (int k = 0; k < 4; k++) {
                    int nb = nbi[k];
                    if (nb < 0 || CC_SEEN(nb)) continue;
                    CC_MARK(nb);
                    int nxi = nb % side, nzi = nb / side;
                    int nx = cx + (nxi - half) * step;
                    int nz = cz + (nzi - half) * step;
                    double nv = sampleDoublePerlin(&cheese_noise,
                        (double)nx, (double)fy, (double)nz);
                    if (nv > thr) cstk[top++] = (uint16_t)nb;
                }
            }
            if (cnt > best) {
                best = cnt;
                bestx = (int)(sumx / cnt);
                bestz = (int)(sumz / cnt);
            }
        }
        #undef CC_SEEN
        #undef CC_MARK

        int area = best * step * step;
        *hx = bestx; *hz = bestz; *hsize = area;
        return area >= need;
#endif
    }

    case F_SPAWN_BIOME: {
        /* getSpawn is heavy; only reachable after cheaper filters pass */
        Pos sp = getSpawn(g);
        int b = sf_getBiomeAt(g, sp.x, 63, sp.z);
        for (int i = 0; i < f->num_spawn_biomes; i++) {
            if (f->spawn_biomes[i] == b) {
                *hx = sp.x; *hz = sp.z; return 1;
            }
        }
        return 0;
    }

    case F_STRUCTURE: {
        StructureConfig sc;
        if (!getStructureConfig(f->structure, g->mc, &sc)) return 0;
        int rspan = sc.regionSize * 16;
        int rxlo = (f->x - f->radius) / rspan - 1;
        int rxhi = (f->x + f->radius) / rspan + 1;
        int rzlo = (f->z - f->radius) / rspan - 1;
        int rzhi = (f->z + f->radius) / rspan + 1;
        int found = 0;
        for (int rz = rzlo; rz <= rzhi; rz++)
        for (int rx = rxlo; rx <= rxhi; rx++) {
            Pos p;
            if (!getStructurePos(f->structure, g->mc, g->seed, rx, rz, &p))
                continue;
            long ddx = p.x - f->x, ddz = p.z - f->z;
            if (ddx*ddx + ddz*ddz > (long)f->radius * f->radius) continue;
#ifndef __CUDA_ARCH__
            if (isViableStructurePos(f->structure, (Generator*)g, p.x, p.z, 0)) {
                if (!found) { *hx = p.x; *hz = p.z; }
                if (++found >= (f->want_count > 0 ? f->want_count : 1))
                    return 1;
            }
#else
            {
                /* GPU: approximate isViableStructurePos via sf_getBiomeAt.
                 * Village MC-1.18+ uses variant-specific sample points; we
                 * use chunk-centre (chunkX*16+8, 319, chunkZ*16+8) as close
                 * enough to avoid the vast majority of false positives. */
                int bx = (p.x >> 4) * 16 + 8;
                int bz = (p.z >> 4) * 16 + 8;
                int bid = sf_getBiomeAt(g, bx, 319, bz);
                if (bid < 0 || !isViableFeatureBiome(g->mc, f->structure, bid))
                    continue;
            }
            if (!found) { *hx = p.x; *hz = p.z; }
            if (++found >= (f->want_count > 0 ? f->want_count : 1))
                return 1;
#endif
        }
        return 0;
    }

    case F_QUAD_HUT:
        /* placeholder cheap prefilter hook; pass-through for now */
        *hx = f->x; *hz = f->z;
        return 1;

    case F_LOOT: {
        /* Find any structure of the right type within radius, then check loot. */
        StructureConfig sc;
        if (!getStructureConfig(f->structure, g->mc, &sc)) return 0;
        int rspan = sc.regionSize * 16;
        int rxlo = (f->x - f->radius) / rspan - 1;
        int rxhi = (f->x + f->radius) / rspan + 1;
        int rzlo = (f->z - f->radius) / rspan - 1;
        int rzhi = (f->z + f->radius) / rspan + 1;
        for (int rz = rzlo; rz <= rzhi; rz++)
        for (int rx = rxlo; rx <= rxhi; rx++) {
            Pos p;
            if (!getStructurePos(f->structure, g->mc, g->seed, rx, rz, &p))
                continue;
            long ddx = p.x - f->x, ddz = p.z - f->z;
            if (ddx*ddx + ddz*ddz > (long)f->radius * f->radius) continue;
            /* getStructurePos only returns a CANDIDATE region position; the
             * structure only really generates if the biome is viable. Without
             * this gate we simulate loot for phantom temples -> false hits.
             * Same gate F_STRUCTURE uses. */
#ifndef __CUDA_ARCH__
            if (!isViableStructurePos(f->structure, (Generator*)g, p.x, p.z, 0))
                continue;
#else
            {
                int bx = (p.x >> 4) * 16 + 8;
                int bz = (p.z >> 4) * 16 + 8;
                int bid = sf_getBiomeAt(g, bx, 319, bz);
                if (bid < 0 || !isViableFeatureBiome(g->mc, f->structure, bid))
                    continue;
            }
#endif
            int n = loot_count_in_structure(f->structure, g->seed,
                                            p.x, p.z, f->loot_mask,
                                            f->loot_step, f->loot_salt,
                                            f->loot_pre);
            if (n >= f->loot_min) {
                *hx = p.x; *hz = p.z; return 1;
            }
        }
        return 0;
    }
    }
    return 0;
}

/* Evaluate a whole profile against one seed. Filters are assumed pre-sorted
 * by ascending cost so cheap rejections happen first. *out_size receives the
 * largest measured area (blocks) reported by any size/area filter, so callers
 * can rank surviving seeds by biggest-biome size. */
SF_FN int eval_seed(const SearchProfile *p, uint64_t seed,
                    Generator *g, int *out_x, int *out_z, int *out_size)
{
    applySeed(g, p->dim, seed);
    int hx = 0, hz = 0, hsize = 0;
    int first_x = 0, first_z = 0, any = 0, best_size = 0;
    for (int i = 0; i < p->num_filters; i++) {
        if (!eval_filter(g, &p->filters[i], &hx, &hz, &hsize)) return 0;
        if (!any) { first_x = hx; first_z = hz; any = 1; }
        /* Prefer the position of the filter that actually measured a size, so
         * the reported (x,z) lands on the big-biome blob rather than filter 0. */
        if (hsize > best_size) {
            best_size = hsize;
            first_x = hx; first_z = hz;
        }
    }
    *out_x = first_x; *out_z = first_z; *out_size = best_size;
    return 1;
}

#endif
