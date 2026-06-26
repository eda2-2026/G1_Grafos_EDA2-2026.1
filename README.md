# SeedForge

GPU-accelerated Minecraft (Java) seed finder built on
[cubiomes](https://github.com/Cubitect/cubiomes). One filter engine, compiled
for **both** CPU (reference / no-GPU) and CUDA (multi-GPU). Searches are
described entirely in a hand-editable config file — no recompiling to change
what you look for.

Inspired by cubiomes-viewer, GodsVictory's seed finder, and tomudding's
CUDA-Seed-Finder. cubiomes is MIT; this project is GPL-3.0 to match.

## Why it's fast

- **Shared `eval.h`** runs identically on host and device, so the CUDA path is
  correct by construction (validate on CPU, run on GPU).
- **Cost-sorted filters**: the loader sorts conditions cheapest-first, so the
  expensive biome/spawn sampling only touches seeds that already survived the
  cheap rejects. This is the single biggest throughput lever.
- **Grid-stride kernel** keeps every SM saturated; `setupGenerator` runs once
  per thread (it's the costly setup), `applySeed` runs per seed.
- **Multi-GPU**: the seed range is split across all selected devices, each on
  its own host thread + stream. Your RTX 5060 Ti + RTX 3060 grind disjoint
  sub-ranges in parallel; throughput ≈ the sum of both cards.

## Build

```bash
make lib     # build the cubiomes static lib
make cpu     # CPU finder (gcc + OpenMP) — works with no GPU
make gpu     # CUDA finder (needs nvcc); arches preset for sm_89 + sm_86
```

## Run

```bash
./seedforge_cpu example.cfg      # reference / portable
./seedforge_gpu example.cfg      # uses all visible GPUs, or `gpus=` from cfg
```

Hits are written to the `out=` path as `seed<TAB>x<TAB>z<TAB>size`, sorted by `size` (measured biome area in blocks, from a `biome_size`/`biome_area` filter; `0` otherwise) descending — so a biggest-biome hunt puts the winner on the first line.

## Tuning knobs

**Runtime (config file)** — see `example.cfg`. Globals: `mc`, `dim`,
`seed_start`, `seed_end`, `gpus` (e.g. `gpus = 0,1`), `out`, `verbose`.
Each `[filter]` block adds an AND condition.

**Filter kinds**

| kind           | meaning                                        | key fields                          |
|----------------|------------------------------------------------|-------------------------------------|
| `biome_at`     | exact biome at one point                       | `biome,x,y,z`                       |
| `biome_radius` | biome appears within radius                    | `biome,x,z,radius,step,count`       |
| `structure`    | viable structure within radius                 | `structure,x,z,radius,count`        |
| `spawn_biome`  | world spawn is one of a set (expensive)        | `spawn_biomes=plains,desert,...`    |
| `quad_hut`     | cheap pre-filter hook (extend as needed)       | —                                   |

Bump `step` (≥16) on radius filters to trade accuracy for speed. Override a
filter's auto-assigned `cost` to force ordering.

**Build-time (nvcc `-D`)** — `SF_THREADS_PER_BLOCK` (128/256/512),
`SF_SEEDS_PER_THREAD`, `SF_MAX_HITS`, `SF_MAX_FILTERS`. Sweep
`SF_THREADS_PER_BLOCK` per card to find peak occupancy.

## Notes / limits

- cubiomes can't see block-level terrain, so desert pyramids / jungle temples /
  mansions in 1.18+ may yield occasional false positives — same caveat as every
  cubiomes-based tool.
- The device hit buffer is capped at `SF_MAX_HITS`; if it fills, tighten filters
  or raise the cap. The CPU runner has no such cap.
- Full 2^48 sweeps are huge; start with a small `seed_end` and widen.
