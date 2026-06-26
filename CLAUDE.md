# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build commands

```bash
make lib     # build cubiomes static library (prerequisite for cpu target)
make cpu     # CPU finder: gcc + OpenMP, links against libcubiomes.a
make gpu     # CUDA finder: nvcc compiles everything together (cubiomes .c files included)
make clean   # remove seedforge_cpu and seedforge_gpu binaries + cubiomes objects
```

The GPU target compiles cubiomes `.c` files **with nvcc** (not a pre-built lib) so they get device codegen. The `-rdc=true -x cu` flags are required — don't factor them out.

GPU architecture targets in the Makefile: `sm_86` (RTX 3060, Ampere) + PTX `compute_89` (forward-compat JIT covering RTX 5060 Ti Blackwell sm_120). The RTX 5060 Ti is Blackwell, not Ada — native `sm_120` requires CUDA 12.5+; the PTX fallback handles it via JIT.

## Running

```bash
./seedforge_cpu example.cfg     # CPU reference, OpenMP multithreaded, no hit cap
./seedforge_gpu example.cfg     # GPU path, uses all visible GPUs unless gpus= set
```

Output is `seed<TAB>x<TAB>z<TAB>size` written to the `out=` path (default `hits.txt`), sorted by `size` (measured biome area in blocks) descending — `0` when no size filter is used.

## Architecture

All search logic flows through three layers:

**`src/eval.h`** — the shared evaluator, compiled for both host and device. Defines `eval_filter()` and `eval_seed()` with `SF_FN` which expands to `__host__ __device__ static inline` when `SF_DEVICE` is defined (from nvcc), or plain `static inline` for gcc. This is the key design invariant: CPU and GPU run identical code, so the CPU path is the correctness reference.

**`src/config.h` / `src/config.c`** — defines `SearchProfile` and `Filter` structs; parses the INI-style `.cfg` file. After parsing, `load_profile()` stable-sorts filters by ascending `.cost` so cheap filters reject seeds before expensive ones run. Default costs: `F_QUAD_HUT`=0, `F_BIOME_AT`=10, `F_STRUCTURE`=30, `F_BIOME_RADIUS`=50, `F_BIOME_AREA`=60, `F_BIOME_SIZE`=70, `F_SPAWN_BIOME`=90, `F_LOOT`=120.

**Biggest-biome search.** `F_BIOME_SIZE` (cfg `kind=biome_size`) flood-fills the largest *contiguous* blob of a biome category within a window and reports its area in blocks; `eval_seed` returns that size and both runners sort hits descending by it, so the output's first row is the biggest biome. A biome **category** (`biomes=`/`category=` in the cfg, e.g. `mountains`) expands to a set of biome ids stored in `Filter.match_biomes[]`; `sf_biome_matches()` in `eval.h` is the single match predicate the radius/area/size filters share. The flood-fill scratch (uint16 stack + visited bitmap) lives on the per-thread stack, so the scan grid is clamped to a cell cap that differs by compile pass via `SF_FLOOD_N` in `eval.h`: host/CPU uses `SF_FLOOD_MAX_CELLS` (default 65536 — megabyte stacks allow precise large windows), device/GPU uses `SF_FLOOD_MAX_CELLS_GPU` (default 512 — the scratch is reserved per resident thread, so it must stay tiny or it explodes into GBs of local memory and tanks throughput). Practical workflow: GPU sweeps with a strict `biome_area` prefilter, CPU re-measures survivors precisely. The filter is sampling-bound (thousands–tens-of-thousands seeds/s, not millions). See `mountains.cfg` for the two-phase pattern.

**Cheese cave search.** `F_CHEESE_CAVE` (cfg `kind=cheese_cave`) finds seeds with large cheese caves by sampling the `minecraft:cave_cheese` `DoublePerlinNoise` at a fixed underground Y level. The noise seed is derived via the same XOR pattern as climate noises: `xSetSeed` then `xNextLong×2` on the world seed, then XOR with `MD5("minecraft:cave_cheese")` (`lo=0xb159093bc7baaa50`, `hi=0x53abc45424417c20`), firstOctave=-9, amplitudes=[0.5,0.25,0.125,0.0625,0.03125], sampled at block coordinates. GPU path: area count over the full radius using at most 33×33=1089 samples (step is doubled to fit), fast prefilter at ~120k seeds/s. CPU path: 2D flood fill for the largest *connected* component (precise). Typical workflow: GPU sweeps, CPU re-measures survivors. Key fields: `y` (block Y, default -40), `cave_threshold` (noise cutoff, default 0.5, range ~0–1.4), `min_area` (blocks). See `cheese_cave.cfg`.

**`src/run_cpu.c`** / **`src/run_gpu.cu`** — entry points. CPU uses OpenMP `parallel for` with `dynamic` scheduling; GPU launches a grid-stride kernel with one `Generator` per thread (setup once, `applySeed` per seed) and splits the seed range across GPUs using pthreads.

**`src/loot.h`** — GPU-compatible loot simulation. Uses Java LCG (`rng.h`) to derive chest loot seeds via `pieceRand()` (mirrors `setLargeFeatureSeed`) and simulates loot tables for Desert Pyramid, Jungle Temple, Igloo, Shipwreck, Pillager Outpost, Ruined Portal, Ancient City, Bastion Remnant, and Woodland Mansion. All functions are `LOOT_FN` (same dual-compile pattern as `SF_FN`). Loot tables are verified against MC 1.21 datapack weights.

**`cubiomes/`** — vendored [cubiomes](https://github.com/Cubitect/cubiomes) library. Do not modify. The `cuda_compat.h` shim makes it compilable with nvcc. The GPU biome query in `eval.h` uses `genBiomeNoiseScaled` with a stack-allocated range instead of `getBiomeAt` (which mallocs) — this is required for device code.

## Config file format

The `.cfg` file is INI-style: global keys first, then `[filter]` sections (all filters are ANDed). Unknown keys are silently ignored. Filters auto-sort by cost — write them in any order.

Key filter fields: `kind`, `x`, `z`, `y`, `radius`, `step`, `biome`, `structure`, `count`, `cost` (optional override), `spawn_biomes` (comma list), `loot_items` (comma list), `loot_min`, `biomes`/`category` (comma list of biome names + category keywords → `match_biomes[]`), `min_area` (blocks, for `biome_size`/`cheese_cave`), `cave_threshold` (noise cutoff, for `cheese_cave`), `center_spawn`/`at_spawn`.

## Build-time knobs (nvcc `-D` overrides)

| Define | Default | Purpose |
|---|---|---|
| `SF_THREADS_PER_BLOCK` | 256 | Sweep 128/256/512 per GPU for peak occupancy |
| `SF_SEEDS_PER_THREAD` | 256 | Grid-stride batch size |
| `SF_MAX_HITS` | 4096 | Device-side result buffer cap |
| `SF_MAX_FILTERS` | 16 | Max filters per profile |
| `SF_FLOOD_MAX_CELLS` | 65536 | `biome_size` flood-fill grid cap on **host/CPU** (on-stack scratch); raise for huge precise windows |
| `SF_FLOOD_MAX_CELLS_GPU` | 512 | `biome_size` grid cap on **device/GPU**; keep small — scratch is reserved per resident thread |

## Extending loot tables or filter kinds

- **New loot table**: add a `sim_<name>_chest()` function in `loot.h`, add a case to `loot_chest_count()`, `loot_pre_advances()`, and `loot_sim_chest()`. Calibrate `loot_pre_advances` against a known seed.
- **New filter kind**: add to `FilterKind` enum in `config.h`, add a `case` in `eval_filter()` in `eval.h`, add parsing in `config.c` (`kind=` handler and `default_cost()`).
- **New biome/structure name**: extend the name tables in `config.c` (`biome_from_name`, `struct_from_name`).
