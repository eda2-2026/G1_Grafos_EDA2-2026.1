/* config.h — SeedForge tunable surface.
 *
 * Everything a user normally wants to change lives here or in a TOML-ish
 * runtime config file (see config.c). The build-time knobs below let you
 * trade occupancy vs. register pressure without touching kernel code.
 */
#ifndef SEEDFORGE_CONFIG_H
#define SEEDFORGE_CONFIG_H

#include <stdint.h>
#include "biomes.h"   /* MC_* version enum, biome IDs */
#include "finders.h"  /* structure type enum, Pos    */

/* ---- Build-time performance knobs (override with -D on nvcc) ---- */
#ifndef SF_THREADS_PER_BLOCK
#define SF_THREADS_PER_BLOCK 256        /* 128/256/512 — sweep per GPU */
#endif
#ifndef SF_SEEDS_PER_THREAD
#define SF_SEEDS_PER_THREAD 256         /* grid-stride batch size */
#endif
#ifndef SF_MAX_HITS
#define SF_MAX_HITS 4096                /* device-side result buffer cap */
#endif
#ifndef SF_MAX_FILTERS
#define SF_MAX_FILTERS 16               /* filters in one search profile */
#endif
/* Max grid cells the F_BIOME_SIZE flood fill will track. The fill keeps a
 * per-call uint16 stack + a visited bitmap (~2.1 bytes/cell); the scan grid
 * (2*radius/step+1)^2 is CLAMPED to this, so a large radius / small step
 * silently shrinks the measured window — widen coverage with a coarser `step`.
 *
 * Host vs device caps differ on PURPOSE. On CPU each thread has a megabyte-plus
 * stack, so a big window (precise "billion block" measurement) is fine. On GPU
 * the scratch is reserved per-thread across *every* resident thread, so even a
 * few KB explodes into gigabytes of local memory and tanks throughput — so the
 * device cap is small and the GPU's real job is the cheap `biome_area` prefilter
 * (no scratch) while big precise measurement is done with the CPU binary. */
#ifndef SF_FLOOD_MAX_CELLS
#define SF_FLOOD_MAX_CELLS 65536        /* host/CPU cap (raise freely) */
#endif
#ifndef SF_FLOOD_MAX_CELLS_GPU
#define SF_FLOOD_MAX_CELLS_GPU 512      /* device cap (tiny on-thread scratch) */
#endif

/* ---- Filter model ----
 * A search profile is an ordered list of filters, ALL of which must pass
 * (logical AND) for a seed to be reported. Cheap filters are run first
 * (the loader sorts by .cost) so expensive biome sampling only runs on
 * survivors — this is the single biggest throughput win.
 */
typedef enum {
    F_BIOME_AT,        /* a specific biome must exist at/near a point      */
    F_BIOME_RADIUS,    /* biome must appear within radius of (x,z)         */
    F_STRUCTURE,       /* viable structure of type within radius           */
    F_SPAWN_BIOME,     /* world spawn must be one of a biome set           */
    F_QUAD_HUT,        /* lower-48 quad-hut style hint (very cheap prefilter)*/
    F_LOOT,            /* structure chest must contain item(s) >= min count */
    F_BIOME_AREA,      /* biome must cover >= N grid cells within radius    */
    F_BIOME_SIZE,      /* largest CONTIGUOUS biome blob (flood fill) >= area */
    F_CHEESE_CAVE,     /* largest contiguous cheese cave region at depth >= area */
} FilterKind;

typedef struct {
    FilterKind kind;
    int        cost;        /* relative cost; lower runs first */

    /* geometry */
    int   x, z;             /* anchor block coords */
    int   y;                /* for biome-at (usually 63 / 320) */
    int   radius;           /* block radius to scan (radius filters) */
    int   step;             /* sampling step in blocks (>=16 keeps it fast) */

    /* target */
    int   biome;            /* primary target biome id (or -1) */
    int   structure;        /* structure type id (or -1) */
    int   want_count;       /* min matches required inside radius */

    /* Biome CATEGORY / match set. When num_match_biomes > 0, the biome filters
     * (F_BIOME_RADIUS / F_BIOME_AREA / F_BIOME_SIZE) treat the whole set as a
     * single target — so "mountains" can mean the union of windswept_hills,
     * gravelly_mountains, meadow, grove, jagged_peaks, ... and flood fill spans
     * across them. Filled from `biomes=` / `category=` in the cfg. Falls back
     * to the single `biome` field when empty. */
    int   match_biomes[16];
    int   num_match_biomes;

    /* F_BIOME_SIZE: minimum CONTIGUOUS area, in blocks, of the largest matching
     * blob found in the scan window. The flood fill measures connected cells at
     * spacing `step`, so a cell ~= step*step blocks; min_area is compared to
     * (cells * step * step). 0 -> default (one cell). */
    int   min_area;

    /* optional biome set for spawn/biome-set filters. Stored as an explicit
     * id list (not a <128 bitset) because many 1.18+ biome ids exceed 127:
     * cherry_grove=185, deep_dark=183, mangrove_swamp=184, bamboo_jungle=168,
     * snowy_slopes=179, ice_spikes=140, flower_forest=132, ... */
    int   spawn_biomes[16];
    int   num_spawn_biomes;

    /* F_LOOT: which items to count (LootMask bitmask) and minimum total */
    uint64_t loot_mask;     /* OR of LOOT_BIT(LootItem) values */
    int      loot_min;      /* minimum total item count across all chests */

    /* F_LOOT calibration overrides (-1 = use per-structure default in loot.h).
     * Chest loot seeds come from MC's setDecorationSeed -> setFeatureSeed chain;
     * these three constants pin it down and may need tuning per version. */
    int      loot_step;     /* GenerationStep.Decoration ordinal */
    int      loot_salt;     /* structure's feature index within that step */
    int      loot_pre;      /* nextLong() advances before the first chest seed */

    /* F_BIOME_AT / F_BIOME_RADIUS / F_BIOME_AREA: anchor the scan on the world
     * spawn point instead of (x,z) when set. Lets you ask for "big mountains
     * at/near spawn" without knowing spawn ahead of time. */
    int      center_spawn;

    /* F_CHEESE_CAVE: noise value above which a block is considered cave air.
     * The raw cave_cheese DoublePerlinNoise ranges roughly [-1.39, +1.39].
     * Default -1.0 = use 0.5 (good for real cheese caves); 0.0 = any positive
     * noise (very inclusive); 0.7+ = only the most open cave centres. */
    double   cave_threshold;
} Filter;

typedef struct {
    int      mc_version;           /* MC_1_21 etc. */
    int      dim;                  /* DIM_OVERWORLD etc. */
    uint64_t seed_start;
    uint64_t seed_end;             /* exclusive */
    int      num_filters;
    Filter   filters[SF_MAX_FILTERS];

    /* runtime perf */
    int      gpus[8];              /* device ids to use; -1 terminates */
    int      num_gpus;
    int      verbose;
    int      top_n;                /* keep only the top N hits by size; 0 = SF_MAX_HITS */
    char     out_path[512];
} SearchProfile;

/* Result row written by the kernel. */
typedef struct {
    uint64_t seed;
    int      px, pz;   /* representative position of first matched feature */
    int      filter_hits;
    int      size;     /* measured area (blocks) from a size/area filter, else 0.
                          Runners sort hits by this descending so the BIGGEST
                          biome surfaces first. */
} Hit;

/* config.c */
int  load_profile(const char *path, SearchProfile *out);
void print_profile(const SearchProfile *p);

#endif
