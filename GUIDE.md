# SeedForge Config File Guide

SeedForge searches Minecraft world seeds using a `.cfg` file that describes what you are looking for. Every condition in the file must be true at the same time — they are logically ANDed together. The more conditions you add, the rarer (and more useful) the seeds you find.

---

## Quick structure of a config file

```
# Comments start with #
mc = 1.21
seed_start = 0
seed_end   = 1000000000
out        = hits.txt

[filter]
kind      = structure
structure = village
x = 0
z = 0
radius = 500

[filter]
kind  = biome_at
biome = mushroom_fields
x = 0
z = 0
```

Global options come first (before any `[filter]` block). Then you write one `[filter]` block for each condition. Filters are automatically sorted by cost so cheap ones run first — you don't need to worry about order.

---

## Global options

| Key | Example | What it does |
|-----|---------|--------------|
| `mc` | `1.21` | Minecraft version. Options: `1.21`, `1.21.1`, `1.21.3`, `1.20`. Defaults to newest. |
| `dim` | `0` | Dimension: `0` = Overworld, `-1` = Nether, `1` = End. |
| `seed_start` | `0` | First seed to check (inclusive). |
| `seed_end` | `2000000000` | Last seed to check (exclusive). Max is `2^64`. |
| `out` | `hits.txt` | File where matching seeds are written (`seed<TAB>x<TAB>z`). |
| `verbose` | `1` | Print per-GPU stats when done. `0` = silent. |
| `gpus` | `0,1` | Which GPU device IDs to use. Omit to use all visible GPUs. |

---

## Filter types

### `structure` — find a generated structure

Finds seeds where a specific structure spawns within a radius of a coordinate.

```ini
[filter]
kind      = structure
structure = village
x         = 0
z         = 0
radius    = 500
count     = 1
```

| Key | Required | Notes |
|-----|----------|-------|
| `structure` | yes | See structure names below |
| `x`, `z` | yes | Center of the search area (block coordinates) |
| `radius` | yes | Search radius in blocks |
| `count` | no | Minimum number of that structure within the radius. Default: `1` |

**Structure names:**

| Name | Notes |
|------|-------|
| `village` | Plains, desert, savanna, taiga, snowy |
| `desert_pyramid` | Desert biomes only |
| `jungle_temple` | Jungle biomes only |
| `swamp_hut` | Swamp biomes only |
| `igloo` | Snowy biomes only |
| `monument` | Deep ocean biomes only |
| `mansion` | Dark forest only |
| `outpost` | Plains, desert, savanna, taiga, and mountain biomes |
| `ruined_portal` | Anywhere in Overworld or Nether |
| `ancient_city` | Deep Dark (underground) |
| `trail_ruins` | Taiga, jungle, birch forest |
| `shipwreck` | Ocean and beach biomes |

**Examples:**

```ini
# Two villages within 300 blocks of origin
[filter]
kind      = structure
structure = village
x         = 0
z         = 0
radius    = 300
count     = 2

# Ancient city within 1000 blocks of origin
[filter]
kind      = structure
structure = ancient_city
x         = 0
z         = 0
radius    = 1000

# Mansion close to spawn (rare — expect a long search)
[filter]
kind      = structure
structure = mansion
x         = 0
z         = 0
radius    = 2000
```

---

### `biome_at` — require a biome at an exact point

Checks whether a specific biome exists at a given block coordinate.

```ini
[filter]
kind  = biome_at
biome = mushroom_fields
x     = 0
z     = 0
y     = 63
```

| Key | Required | Notes |
|-----|----------|-------|
| `biome` | yes | Biome name (see list below) |
| `x`, `z` | yes | Block coordinate to check |
| `y` | no | Height. Use `63` for surface, `319` for max height. Default: `63` |

**Biome names:**

| Name | Notes |
|------|-------|
| `plains` | Very common |
| `desert` | Hot, no trees |
| `savanna` | Acacia trees |
| `jungle` | Dense trees, bamboo |
| `bamboo_jungle` | Bamboo variant |
| `badlands` | Red sand, mesa |
| `mushroom_fields` | Very rare, no hostile mobs |
| `cherry_grove` | Pink cherry trees |
| `flower_forest` | Dense flowers |
| `ice_spikes` | Rare frozen biome |
| `mangrove_swamp` | Mangrove trees |
| `deep_dark` | Underground, ancient cities |
| `snowy_slopes` | Mountain snow |

You can also pass a raw numeric biome ID if the biome name is not in the list, e.g. `biome = 5`.

**Examples:**

```ini
# Mushroom island at coordinate (1000, 63, 1000)
[filter]
kind  = biome_at
biome = mushroom_fields
x     = 1000
z     = 1000

# Cherry grove right near spawn
[filter]
kind  = biome_at
biome = cherry_grove
x     = 200
z     = -100
```

---

### `biome_radius` — find a biome anywhere in an area

Scans a circle of radius `radius` around `(x, z)` and requires the biome to appear at least `count` times within it.

```ini
[filter]
kind   = biome_radius
biome  = mushroom_fields
x      = 0
z      = 0
radius = 2000
step   = 32
count  = 1
```

| Key | Required | Notes |
|-----|----------|-------|
| `biome` | yes | Biome name or numeric ID |
| `x`, `z` | yes | Center of the scan |
| `radius` | yes | Search radius in blocks |
| `step` | no | Sampling interval in blocks. Smaller = more accurate but slower. Default: `16`. Use `32` or `64` for large radii. |
| `count` | no | How many matching samples required. Default: `1`. Increase to require a larger patch. |

**Tip on `step`:** Each sample costs a full biome noise evaluation. For a 2000-block radius with step=16, that's ~50,000 samples per seed — very slow. Use step=32 or step=64 for rare biomes over large areas, and only tighten the step if you need precise borders.

**Examples:**

```ini
# Mushroom island anywhere within 3000 blocks of spawn, sampled every 64 blocks
[filter]
kind   = biome_radius
biome  = mushroom_fields
x      = 0
z      = 0
radius = 3000
step   = 64

# Ice spikes patch that's at least a medium size (5 samples)
[filter]
kind   = biome_radius
biome  = ice_spikes
x      = 0
z      = 0
radius = 1500
step   = 32
count  = 5
```

---

### `biome_size` — find the BIGGEST biome (flood fill)

This is the filter for hunting record-breaking mega-biomes — the technique behind
"largest mushroom island / biggest mountains" seed finds. Where `biome_area` just
counts matching samples anywhere in a box (so scattered patches inflate the
number), `biome_size` runs a **flood fill** from each matching cell and measures
the single largest *contiguous* blob. Every passing seed is then **ranked by that
measured size**, so the top line of your output file is the biggest biome found.

It also understands biome **categories**, so "mountains" means the whole 1.18+
mountain group at once and the flood fill flows across all of them as one range.

```ini
[filter]
kind     = biome_size
biomes   = mountains      # a category, or a comma list of biome names
x        = 0
z        = 0
radius   = 1500
step     = 48
min_area = 4000000        # largest contiguous blob must be >= 4,000,000 blocks
```

| Key | Required | Notes |
|-----|----------|-------|
| `biomes` / `category` | yes | Category keyword **or** comma list of biome names. The flood fill matches the whole set. (Falls back to a single `biome =` if given instead.) |
| `x`, `z` | yes* | Center of the scan window (*or set `center_spawn = 1`) |
| `radius` | yes | Half-width of the scan window in blocks |
| `step` | no | Grid spacing in blocks (default 16). Each cell ≈ `step × step` blocks |
| `min_area` | no | Minimum size, in blocks, of the largest contiguous blob to pass |
| `center_spawn` | no | `1` = center the scan on world spawn instead of `x,z` |

**Categories:** `mountains` (windswept_hills, wooded/gravelly mountains, meadow,
grove, snowy_slopes, jagged/frozen/stony peaks), `peaks`, `oceans`, `forests`,
`jungles`, `taigas`, `badlands`. Singular forms work too (`mountain`). You can
mix categories and plain biome names in one `biomes =` list.

**How area is measured.** The window is a grid at spacing `step`; each matching
cell counts as `step²` blocks, and `area = contiguous_cells × step²`. So a coarse
`step` covers a wider area cheaply but rounds the measurement; a fine `step`
measures precisely but covers less ground per cell.

**Window size cap (and CPU vs GPU).** The flood fill keeps its scratch on the
per-thread stack, so the scan grid `(2·radius/step + 1)²` is **clamped** to a
cell cap — a huge `radius` with a tiny `step` silently shrinks the window. The
cap differs by target *on purpose*:

* **CPU** (`seedforge_cpu`) — `SF_FLOOD_MAX_CELLS`, default **65536** cells.
  Threads have megabyte stacks, so this is the precise, large-window path. The
  most area it can report is `cap × step²`. Crank it higher for truly massive
  "billion-block" hunts: rebuild with `-DSF_FLOOD_MAX_CELLS=262144`.
* **GPU** (`seedforge_gpu`) — `SF_FLOOD_MAX_CELLS_GPU`, default **512** cells.
  The scratch is reserved for *every* resident thread, so it must stay tiny.
  The GPU therefore measures a smaller/coarser window (max area `512 × step²`);
  pick a coarse enough `step` that your `min_area` is reachable.

So: use the **GPU** to sweep billions of seeds with a strict `biome_area`
prefilter, then re-measure the survivors **precisely on the CPU** with a fine
`step` / large window. (`seedforge_cpu` is also the correctness reference.)

**Two-phase hunting (essential for throughput).** `biome_size` is the most
expensive filter — it samples biomes over an area for *every* seed it reaches.
Gate it behind a cheap, **strict** `biome_area` prefilter (same category, small
coarse window, high `count`) so almost every seed is rejected before the flood
fill runs. SeedForge auto-sorts by cost, so just list both filters. See
`mountains.cfg` in the repo root for a ready-to-run example.

**This is a slow search by nature.** Because biome noise must be sampled across
an area per seed, biggest-biome hunts run at thousands–tens-of-thousands of
seeds/sec, not millions. Record finds take GPU-hours to days — widen the seed
range and let it grind. The output's 4th column is the measured size in blocks,
rows written biggest-first, so you can stop early and still have the best so far.

---

### `loot` — require specific items in a structure's chests

Simulates the chest loot tables for a structure and counts how many of the desired items appear across all chests. Passes if the total is at or above `loot_min`.

```ini
[filter]
kind       = loot
structure  = desert_pyramid
x          = 0
z          = 0
radius     = 1000
loot_items = diamond, enchanted_golden_apple
loot_min   = 1
```

| Key | Required | Notes |
|-----|----------|-------|
| `structure` | yes | Which structure's chests to simulate |
| `x`, `z` | yes | Center of the search area |
| `radius` | yes | Search radius in blocks |
| `loot_items` | yes | Comma-separated list of item names |
| `loot_min` | no | Minimum total count of any wanted item. Default: `1` |

**Supported structures for loot:**

| Structure | Notes |
|-----------|-------|
| `desert_pyramid` | 4 chests |
| `jungle_temple` | 2 chests |
| `igloo` | Basement chest only |
| `shipwreck` | Supply, map, and treasure chests |
| `outpost` | Chest in top floor |
| `ruined_portal` | Single chest |
| `ancient_city` | Multiple chests |
| `mansion` | Multiple chests |

**Item names for `loot_items`:**

| Name | Found in |
|------|---------|
| `enchanted_golden_apple` | Desert pyramid, ancient city |
| `golden_apple` | Desert pyramid, ruins, mansion |
| `diamond` | Desert pyramid, shipwreck, mansion |
| `netherite_scrap` | Bastion remnant chests |
| `echo_shard` | Ancient city |
| `music_disc_otherside` | Ancient city |
| `saddle` | Desert pyramid, shipwreck, outpost |
| `diamond_horse_armor` | Desert pyramid |
| `iron_horse_armor` | Desert pyramid, shipwreck |
| `golden_horse_armor` | Desert pyramid |
| `enchanted_book` | Shipwreck, outpost, ancient city |
| `gold_ingot` | Shipwreck, ruined portal |
| `iron_ingot` | Shipwreck, outpost |
| `tnt` | Desert pyramid |
| `experience_bottle` | Ancient city |

**Examples:**

```ini
# Desert pyramid with an enchanted golden apple in any chest
[filter]
kind       = loot
structure  = desert_pyramid
x          = 0
z          = 0
radius     = 500
loot_items = enchanted_golden_apple
loot_min   = 1

# Shipwreck with at least 2 diamonds total across all chests
[filter]
kind       = loot
structure  = shipwreck
x          = 0
z          = 0
radius     = 800
loot_items = diamond
loot_min   = 2

# Ancient city with either an echo shard OR the otherside disc
[filter]
kind       = loot
structure  = ancient_city
x          = 0
z          = 0
radius     = 1500
loot_items = echo_shard, music_disc_otherside
loot_min   = 1
```

---

### `spawn_biome` — require the world spawn to be in a specific biome

Checks whether the world spawn falls inside one of a set of biomes.

```ini
[filter]
kind         = spawn_biome
spawn_biomes = plains, meadow, cherry_grove
```

| Key | Required | Notes |
|-----|----------|-------|
| `spawn_biomes` | yes | Comma-separated list of biome names |

This is expensive (spawn calculation requires full biome noise). Add it last (or leave the cost unset; it will be sorted automatically).

**Example:**

```ini
# Spawn in a mushroom island or cherry grove
[filter]
kind         = spawn_biome
spawn_biomes = mushroom_fields, cherry_grove
```

---

## Combining filters — real-world examples

All filters must pass simultaneously. Stack them to narrow results to exactly what you want.

### Seed with a village AND a desert pyramid close together

```ini
mc         = 1.21
seed_start = 0
seed_end   = 2000000000
out        = combo_hits.txt

[filter]
kind      = structure
structure = village
x         = 0
z         = 0
radius    = 500

[filter]
kind      = structure
structure = desert_pyramid
x         = 0
z         = 0
radius    = 500
```

---

### Seed with a mushroom island near spawn and a village within 1000 blocks

```ini
mc         = 1.21
seed_start = 0
seed_end   = 5000000000
out        = mushroom_village.txt

[filter]
kind   = biome_radius
biome  = mushroom_fields
x      = 0
z      = 0
radius = 2000
step   = 64

[filter]
kind      = structure
structure = village
x         = 0
z         = 0
radius    = 1000
```

---

### Ancient city with an echo shard AND a village near spawn

```ini
mc         = 1.21
seed_start = 0
seed_end   = 10000000000
out        = city_village.txt

# Cheap filter first (structure position only, no biome noise)
[filter]
kind      = structure
structure = village
x         = 0
z         = 0
radius    = 400

# Ancient city with specific loot (expensive, runs last automatically)
[filter]
kind       = loot
structure  = ancient_city
x          = 0
z          = 0
radius     = 2000
loot_items = echo_shard
loot_min   = 1
```

---

### Cherry grove spawn with a mansion within 2000 blocks

```ini
mc         = 1.21
seed_start = 0
seed_end   = 50000000000
out        = cherry_mansion.txt

# Structure filter is cheap, runs first
[filter]
kind      = structure
structure = mansion
x         = 0
z         = 0
radius    = 2000

# Spawn biome is expensive, runs last
[filter]
kind         = spawn_biome
spawn_biomes = cherry_grove
```

---

### Desert pyramid with diamonds AND a second structure nearby

```ini
mc         = 1.21
seed_start = 0
seed_end   = 2000000000
out        = diamond_pyramid.txt

[filter]
kind       = loot
structure  = desert_pyramid
x          = 0
z          = 0
radius     = 600
loot_items = diamond
loot_min   = 3

[filter]
kind      = structure
structure = village
x         = 0
z         = 0
radius    = 600
```

---

## Performance tips

**Search range matters.** The more seeds you cover, the longer it takes. Common structures (village, ruined portal) appear every ~500 blocks, so 1 billion seeds is enough for most searches. Rare combinations (mansion + cherry spawn) may need 10–50 billion.

**Combine a cheap filter first.** Structure filters (position only) cost almost nothing. Biome noise filters (`biome_at`, `biome_radius`, `spawn_biome`, biome-verified structures) are much slower. Always include at least one structure filter to pre-reject seeds before biome noise kicks in — SeedForge sorts this automatically by default cost, so just leave `cost` unset.

**Reduce `step` only when needed.** For `biome_radius` with a large radius, step=64 is 16× faster than step=16 and finds any biome larger than ~64 blocks. Tighten to step=32 or step=16 only if you need a specific small patch.

**`count` on structures.** Requiring `count = 2` or more of the same structure in a radius is a strong filter that drastically reduces hits. Good for speedrun seeds where you want multiple villages or temples close together.

**`loot_min` tricks.** Asking for `loot_min = 2` on `diamond` (two diamonds across all chests) is much rarer than asking for one. Use higher counts to find genuinely stacked seeds.

**Output file.** The file at `out` grows as SeedForge runs. You can tail -f it to watch results arrive in real time.

---

## Reading the output

Each line in the output file is:

```
<seed>    <x>    <z>    <size>
```

The `x` and `z` are the block coordinates of the matched feature — for structure
filters the structure position, for biome filters the matching sample point, and
for `biome_size` the centroid of the biggest blob. `size` is the measured biome
area in blocks (from a `biome_size` / `biome_area` filter; `0` when no size was
measured). **Rows are sorted by `size`, biggest first**, so for a biggest-biome
hunt the very first line is your winner.

Example output (a mountain-size search):
```
19191    -26    -64    446464
17652    -18    6      431104
14170    -61    2      404480
```

Load any seed into Minecraft with the **Java Edition** seed field. Java seeds can be any 64-bit integer.

---

## Supported Minecraft versions

| Value | Version |
|-------|---------|
| `1.21` | 1.21 / 1.21.4 |
| `1.21.3` | 1.21.3 |
| `1.21.1` | 1.21.1 |
| `1.20` | 1.20.x |

Biome noise, structure positions, and loot tables are all version-specific. Always set `mc` to match the version you will play on.
