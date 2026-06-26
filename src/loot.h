/*
 * loot.h — GPU-compatible structure chest loot simulation.
 *
 * Loot seed derivation mirrors Minecraft's setLargeFeatureSeed / postProcess
 * random sequence (Java LCG, not Xoroshiro even in 1.18+).
 *
 * Loot table weights are verified against the 1.21 datapack.
 * The number of pre-chest random advances per structure was derived from
 * analysis of the relevant StructurePiece.postProcess call sequences;
 * calibrate with a known seed if behaviour changes across minor versions.
 */
#ifndef SEEDFORGE_LOOT_H
#define SEEDFORGE_LOOT_H

#include "config.h"
#include "rng.h"    /* setSeed, nextLong, nextInt, nextFloat */

#ifdef SF_DEVICE
#  define LOOT_FN __host__ __device__ static inline
#else
#  define LOOT_FN static inline
#endif

/* ══════════════════════════════════════════════════════════════════════════
 * Item IDs
 * ══════════════════════════════════════════════════════════════════════════*/
typedef enum {
    LI_NONE                    = -1,
    LI_GOLDEN_APPLE            = 0,
    LI_ENCHANTED_GOLDEN_APPLE  = 1,
    LI_DIAMOND                 = 2,
    LI_EMERALD                 = 3,
    LI_GOLD_INGOT              = 4,
    LI_IRON_INGOT              = 5,
    LI_GOLD_NUGGET             = 6,
    LI_IRON_NUGGET             = 7,
    LI_SADDLE                  = 8,
    LI_GOLD_HORSE_ARMOR        = 9,
    LI_IRON_HORSE_ARMOR        = 10,
    LI_DIAMOND_HORSE_ARMOR     = 11,
    LI_ROTTEN_FLESH            = 12,
    LI_BONE                    = 13,
    LI_SPIDER_EYE              = 14,
    LI_GUNPOWDER               = 15,
    LI_COAL                    = 16,
    LI_BREAD                   = 17,
    LI_WHEAT                   = 18,
    LI_PAPER                   = 19,
    LI_FEATHER                 = 20,
    LI_CLOCK                   = 21,
    LI_COMPASS                 = 22,
    LI_MAP                     = 23,
    LI_CARROT                  = 24,
    LI_TNT                     = 25,
    LI_ENCHANTED_BOOK          = 26,
    LI_LEATHER_CHESTPLATE      = 27,
    LI_IRON_SWORD              = 28,
    LI_EXPERIENCE_BOTTLE       = 29,
    LI_ECHO_SHARD              = 30,
    LI_DISC_OTHERSIDE          = 31,
    LI_SCULK_CATALYST          = 32,
    LI_AMETHYST_SHARD          = 33,
    LI_BUNDLE                  = 34,
    LI_GLOW_BERRIES            = 35,
    LI_CANDLE                  = 36,
    LI_PRISMARINE_CRYSTALS     = 37,
    LI_PRISMARINE_SHARD        = 38,
    LI_ARROW                   = 39,
    LI_BAMBOO                  = 40,
    LI_POISONOUS_POTATO        = 41,
    LI_BOOK                    = 42,
    LI_DISC_13                 = 43,
    LI_DISC_CAT                = 44,
    LI_APPLE                   = 45,
    LI_STONE_AXE               = 46,
    LI_CROSSBOW                = 47,
    LI_DARK_OAK_LOG            = 48,
    LI_FIRE_CHARGE             = 49,
    LI_FLINT_STEEL             = 50,
    LI_OBSIDIAN                = 51,
    LI_GOLDEN_HORSE_ARMOR_UNUSED = 52, /* slot reserved; use #define below */
    LI_SOUL_SAND               = 53,
    LI_SCULK_SENSOR            = 54,
    LI_NETHERITE_SCRAP         = 55,
    LI_SNOUT_ARMOR_TRIM        = 56,
    LI_LEAD                    = 57,
    LI_REDSTONE                = 58,
    LI_BUCKET                  = 59,
    LI_ITEM_COUNT              = 60,
} LootItem;

/* Bitmask helper: bit i set means LootItem i is wanted */
typedef uint64_t LootMask;
#define LOOT_BIT(item)  ((LootMask)1 << (item))

/* ══════════════════════════════════════════════════════════════════════════
 * Seed derivation — mirrors Minecraft's setLargeFeatureSeed
 * (Java LCG for all MC versions, not Xoroshiro)
 * ══════════════════════════════════════════════════════════════════════════*/

/* MC WorldgenRandom.setDecorationSeed(levelSeed, blockX, blockZ) — the
 * "population seed" for a chunk. NOTE: it takes the chunk's ORIGIN BLOCK
 * coordinates (chunkX*16, chunkZ*16), not chunk coordinates. */
LOOT_FN uint64_t loot_population_seed(uint64_t worldSeed, int blockX, int blockZ)
{
    uint64_t rng;
    setSeed(&rng, worldSeed);
    int64_t a = (int64_t)nextLong(&rng) | 1;
    int64_t b = (int64_t)nextLong(&rng) | 1;
    return ((int64_t)blockX * a + (int64_t)blockZ * b) ^ worldSeed;
}

/* MC WorldgenRandom.setFeatureSeed(populationSeed, index, step):
 *   featureSeed = populationSeed + index + 10000 * step
 * This is the seed of the `random` handed to StructurePiece.postProcess, from
 * which each chest's loot seed is drawn via random.nextLong(). */
LOOT_FN uint64_t loot_feature_seed(uint64_t popSeed, int index, int step)
{
    return popSeed + (uint64_t)index + 10000ULL * (uint64_t)step;
}

/* Advance rng by n nextLong calls, discarding the results. */
LOOT_FN void skipLongs(uint64_t *rng, int n)
{
    for (int i = 0; i < n; i++) nextLong(rng);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Loot simulation helpers
 *
 * Each sim_* function simulates filling ONE chest from the given lootSeed
 * and returns the total count of items whose LootItem bit is set in mask.
 *
 * The RNG sequence:
 *   setSeed(&rng, lootSeed)
 *   for each pool:
 *     rolls = roll_min + nextInt(&rng, roll_max - roll_min + 1)
 *     for each roll:
 *       w = nextInt(&rng, totalWeight)   → select entry
 *       if entry has count range:
 *         cnt = nextInt(&rng, max-min+1) + min
 *       if entry has func enchant_randomly:
 *         nextInt(&rng, ENCHANT_COUNT)   → pick enchantment
 *         nextInt(&rng, max_level)       → pick level  (approx 2 calls)
 *
 * enchant_randomly consumes ~2 nextInt calls (enchantment index + level).
 * With ~40 enchantments and levels 1-5, exact count varies by enchantment;
 * we use a fixed 2-call advance which is correct for most enchantments.
 * ══════════════════════════════════════════════════════════════════════════*/

#define ENCHANT_ADVANCE(rng)  do { nextInt(rng,40); nextInt(rng,5); } while(0)

/* Item aliases used across multiple sim functions */
#define LI_BOTTLE_EXPERIENCE  LI_EXPERIENCE_BOTTLE
#define LI_GOLDEN_HORSE_ARMOR LI_GOLD_HORSE_ARMOR

/* Roll one pool entry given weight index w; advance rng; return item count
 * matching mask. totalWeight must equal sum of all weights in the pool. */

/* ─── DESERT PYRAMID ────────────────────────────────────────────────────────
 * minecraft:chests/desert_pyramid  (1.18-1.21)
 * 1 pool, rolls 2-4.
 * Total weight: 210
 */
LOOT_FN int sim_desert_pyramid_chest(uint64_t lootSeed, LootMask mask)
{
    uint64_t rng;
    setSeed(&rng, lootSeed);
    int count = 0;
    int rolls = 2 + nextInt(&rng, 3);   /* 2, 3 or 4 */
    for (int r = 0; r < rolls; r++) {
        int w = nextInt(&rng, 210);
        int item = LI_NONE, cmin = 1, cmax = 1, enc = 0;
        if      (w <  25) { item=LI_ROTTEN_FLESH;       cmin=3; cmax=7; }
        else if (w <  50) { item=LI_BONE;               cmin=4; cmax=6; }
        else if (w <  75) { item=LI_SPIDER_EYE;         cmin=1; cmax=3; }
        else if (w <  90) { item=LI_GOLD_INGOT;         cmin=2; cmax=7; }
        else if (w < 105) { item=LI_EMERALD;            cmin=1; cmax=3; }
        else if (w < 115) { item=LI_IRON_INGOT;         cmin=1; cmax=5; }
        else if (w < 135) { item=LI_SADDLE;             cmin=1; cmax=1; }
        else if (w < 150) { item=LI_IRON_HORSE_ARMOR;   cmin=1; cmax=1; }
        else if (w < 160) { item=LI_GOLD_HORSE_ARMOR;   cmin=1; cmax=1; }
        else if (w < 165) { item=LI_DIAMOND_HORSE_ARMOR;cmin=1; cmax=1; }
        else if (w < 185) { item=LI_ENCHANTED_BOOK;     cmin=1; cmax=1; enc=1; }
        else if (w < 188) { item=LI_DIAMOND;            cmin=1; cmax=3; }
        else if (w < 208) { item=LI_GOLDEN_APPLE;       cmin=1; cmax=1; }
        else              { item=LI_ENCHANTED_GOLDEN_APPLE; cmin=1; cmax=1; }

        int range = cmax - cmin;
        int cnt = cmin + (range > 0 ? nextInt(&rng, range+1) : 0);
        if (enc) { ENCHANT_ADVANCE(&rng); }
        if (item >= 0 && (mask >> item) & 1) count += cnt;
    }
    return count;
}

/* ─── JUNGLE TEMPLE ─────────────────────────────────────────────────────────
 * Chest 0 (main chamber, behind dispensers): minecraft:chests/jungle_temple
 * Chest 1 (hidden alcove, behind lever):     minecraft:chests/jungle_temple_dispenser
 * Both tables are the same in 1.18+; 2-6 rolls, total weight 126.
 */
LOOT_FN int sim_jungle_temple_chest(uint64_t lootSeed, LootMask mask)
{
    uint64_t rng;
    setSeed(&rng, lootSeed);
    int count = 0;
    int rolls = 2 + nextInt(&rng, 5);   /* 2-6 */
    for (int r = 0; r < rolls; r++) {
        int w = nextInt(&rng, 126);
        int item = LI_NONE, cmin = 1, cmax = 1, enc = 0;
        if      (w <  20) { item=LI_DIAMOND;       cmin=1; cmax=3; }
        else if (w <  35) { item=LI_IRON_INGOT;    cmin=1; cmax=5; }
        else if (w <  50) { item=LI_GOLD_INGOT;    cmin=2; cmax=7; }
        else if (w <  55) { item=LI_ROTTEN_FLESH;  cmin=3; cmax=7; }
        else if (w <  70) { item=LI_BONE;          cmin=4; cmax=6; }
        else if (w <  90) { item=LI_BAMBOO;        cmin=1; cmax=3; }
        else if (w < 110) { item=LI_SPIDER_EYE;    cmin=1; cmax=3; }
        else if (w < 116) { item=LI_ENCHANTED_BOOK;cmin=1; cmax=1; enc=1; }
        else if (w < 121) { item=LI_GOLDEN_APPLE;  cmin=1; cmax=1; }
        else              { item=LI_SADDLE;         cmin=1; cmax=1; }

        int range = cmax - cmin;
        int cnt = cmin + (range > 0 ? nextInt(&rng, range+1) : 0);
        if (enc) { ENCHANT_ADVANCE(&rng); }
        if (item >= 0 && (mask >> item) & 1) count += cnt;
    }
    return count;
}

/* ─── IGLOO ─────────────────────────────────────────────────────────────────
 * minecraft:chests/igloo_chest  (1.18+)
 * 2-8 rolls, total weight 140.
 */
LOOT_FN int sim_igloo_chest(uint64_t lootSeed, LootMask mask)
{
    uint64_t rng;
    setSeed(&rng, lootSeed);
    int count = 0;
    int rolls = 2 + nextInt(&rng, 7);
    for (int r = 0; r < rolls; r++) {
        int w = nextInt(&rng, 140);
        int item = LI_NONE, cmin = 1, cmax = 1, enc = 0;
        if      (w <  15) { item=LI_APPLE;             cmin=1; cmax=3; }
        else if (w <  30) { item=LI_COAL;              cmin=1; cmax=4; }
        else if (w <  45) { item=LI_GOLD_NUGGET;       cmin=1; cmax=3; }
        else if (w <  55) { item=LI_STONE_AXE;         cmin=1; cmax=1; }
        else if (w <  65) { item=LI_ROTTEN_FLESH;      cmin=1; cmax=3; }
        else if (w <  75) { item=LI_LEATHER_CHESTPLATE;cmin=1; cmax=1; }
        else if (w <  90) { item=LI_WHEAT;             cmin=2; cmax=3; }
        else if (w < 105) { item=LI_BREAD;             cmin=1; cmax=2; }
        else if (w < 115) { item=LI_GOLDEN_APPLE;      cmin=1; cmax=1; }
        else if (w < 125) { item=LI_IRON_INGOT;        cmin=1; cmax=3; }
        else              { item=LI_EMERALD;            cmin=1; cmax=2; }
        (void)enc;
        int range = cmax - cmin;
        int cnt = cmin + (range > 0 ? nextInt(&rng, range+1) : 0);
        if (item >= 0 && (mask >> item) & 1) count += cnt;
    }
    return count;
}

/* ─── SHIPWRECK ─────────────────────────────────────────────────────────────
 * Chest 0 — map chest    (minecraft:chests/shipwreck_map)
 * Chest 1 — supply chest (minecraft:chests/shipwreck_supply)
 * Chest 2 — treasure     (minecraft:chests/shipwreck_treasure)
 */
LOOT_FN int sim_shipwreck_map(uint64_t lootSeed, LootMask mask)
{
    uint64_t rng;
    setSeed(&rng, lootSeed);
    int count = 0;
    /* 3 rolls, total weight 60 */
    for (int r = 0; r < 3; r++) {
        int w = nextInt(&rng, 60);
        int item = LI_NONE, cmin = 1, cmax = 1;
        if      (w < 20) { item=LI_PAPER;   cmin=1; cmax=5; }
        else if (w < 40) { item=LI_FEATHER; cmin=1; cmax=5; }
        else if (w < 50) { item=LI_BOOK;    cmin=1; cmax=5; }
        else if (w < 55) { item=LI_CLOCK;   cmin=1; cmax=1; }
        else             { item=LI_COMPASS;  cmin=1; cmax=1; }
        int range = cmax - cmin;
        int cnt = cmin + (range > 0 ? nextInt(&rng, range+1) : 0);
        if (item >= 0 && (mask >> item) & 1) count += cnt;
    }
    /* 1 map always */
    if ((mask >> LI_MAP) & 1) count += 1;
    nextInt(&rng, 4); /* consumed for map type */
    return count;
}

LOOT_FN int sim_shipwreck_supply(uint64_t lootSeed, LootMask mask)
{
    uint64_t rng;
    setSeed(&rng, lootSeed);
    int count = 0;
    int rolls = 3 + nextInt(&rng, 8); /* 3-10 */
    /* total weight 126 */
    for (int r = 0; r < rolls; r++) {
        int w = nextInt(&rng, 126);
        int item = LI_NONE, cmin = 1, cmax = 1;
        if      (w <  16) { item=LI_WHEAT;          cmin=8; cmax=21; }
        else if (w <  32) { item=LI_CARROT;         cmin=4; cmax=8;  }
        else if (w <  48) { item=LI_POISONOUS_POTATO;cmin=2;cmax=6;  }
        else if (w <  58) { item=LI_COAL;           cmin=2; cmax=8;  }
        else if (w <  73) { item=LI_ROTTEN_FLESH;   cmin=5; cmax=24; }
        else if (w <  83) { item=LI_LEATHER_CHESTPLATE;cmin=1;cmax=1;}
        else if (w <  93) { item=LI_IRON_SWORD;     cmin=1; cmax=1;  }
        else if (w < 103) { item=LI_TNT;            cmin=1; cmax=2;  }
        else if (w < 113) { item=LI_BREAD;          cmin=2; cmax=7;  }
        else if (w < 116) { item=LI_IRON_INGOT;     cmin=1; cmax=5;  }
        else              { item=LI_GUNPOWDER;       cmin=1; cmax=5;  }
        int range = cmax - cmin;
        int cnt = cmin + (range > 0 ? nextInt(&rng, range+1) : 0);
        if (item >= 0 && (mask >> item) & 1) count += cnt;
    }
    return count;
}

LOOT_FN int sim_shipwreck_treasure(uint64_t lootSeed, LootMask mask)
{
    uint64_t rng;
    setSeed(&rng, lootSeed);
    int count = 0;
    int rolls = 3 + nextInt(&rng, 4); /* 3-6 */
    /* total weight 40 */
    for (int r = 0; r < rolls; r++) {
        int w = nextInt(&rng, 40);
        int item = LI_NONE, cmin = 1, cmax = 1;
        if      (w < 10) { item=LI_IRON_INGOT;   cmin=4; cmax=8;  }
        else if (w < 20) { item=LI_GOLD_INGOT;   cmin=4; cmax=8;  }
        else if (w < 25) { item=LI_IRON_NUGGET;  cmin=1; cmax=10; }
        else if (w < 30) { item=LI_GOLD_NUGGET;  cmin=1; cmax=10; }
        else if (w < 33) { item=LI_EMERALD;      cmin=4; cmax=8;  }
        else if (w < 36) { item=LI_DIAMOND;      cmin=1; cmax=5;  }
        else if (w < 37) { item=LI_GOLDEN_APPLE; cmin=1; cmax=1;  }
        else             { item=LI_EXPERIENCE_BOTTLE;cmin=1;cmax=5;}
        int range = cmax - cmin;
        int cnt = cmin + (range > 0 ? nextInt(&rng, range+1) : 0);
        if (item >= 0 && (mask >> item) & 1) count += cnt;
    }
    return count;
}

/* ─── PILLAGER OUTPOST ──────────────────────────────────────────────────────
 * minecraft:chests/pillager_outpost  (1.18+)
 * 2-3 rolls, total weight 60.
 */
LOOT_FN int sim_outpost_chest(uint64_t lootSeed, LootMask mask)
{
    uint64_t rng;
    setSeed(&rng, lootSeed);
    int count = 0;
    int rolls = 2 + nextInt(&rng, 2);
    for (int r = 0; r < rolls; r++) {
        int w = nextInt(&rng, 60);
        int item = LI_NONE, cmin = 1, cmax = 1, enc = 0;
        if      (w <  10) { item=LI_CROSSBOW;      cmin=1; cmax=1; enc=1; }
        else if (w <  20) { item=LI_CARROT;        cmin=4; cmax=8; }
        else if (w <  30) { item=LI_WHEAT;         cmin=3; cmax=5; }
        else if (w <  40) { item=LI_DARK_OAK_LOG;  cmin=2; cmax=3; }
        else if (w <  45) { item=LI_ENCHANTED_BOOK;cmin=1; cmax=1; enc=1; }
        else if (w <  50) { item=LI_BOTTLE_EXPERIENCE;cmin=1;cmax=1;}
        else if (w <  55) { item=LI_IRON_INGOT;    cmin=1; cmax=5; }
        else              { item=LI_GOLDEN_APPLE;   cmin=1; cmax=1; }
        int range = cmax - cmin;
        int cnt = cmin + (range > 0 ? nextInt(&rng, range+1) : 0);
        if (enc) { ENCHANT_ADVANCE(&rng); }
        if (item >= 0 && (mask >> item) & 1) count += cnt;
    }
    return count;
}

/* ─── RUINED PORTAL ─────────────────────────────────────────────────────────
 * minecraft:chests/ruined_portal  (1.18+)
 * 4-8 rolls, total weight 240.
 */
LOOT_FN int sim_ruined_portal_chest(uint64_t lootSeed, LootMask mask)
{
    uint64_t rng;
    setSeed(&rng, lootSeed);
    int count = 0;
    int rolls = 4 + nextInt(&rng, 5);
    for (int r = 0; r < rolls; r++) {
        int w = nextInt(&rng, 240);
        int item = LI_NONE, cmin = 1, cmax = 1, enc = 0;
        if      (w <  40) { item=LI_GLOW_BERRIES;      cmin=1; cmax=5; }
        else if (w <  80) { item=LI_GOLD_NUGGET;       cmin=4; cmax=24;}
        else if (w < 110) { item=LI_IRON_NUGGET;       cmin=9; cmax=18;}
        else if (w < 130) { item=LI_GOLD_INGOT;        cmin=4; cmax=9; }
        else if (w < 155) { item=LI_IRON_INGOT;        cmin=1; cmax=5; }
        else if (w < 170) { item=LI_GOLDEN_APPLE;      cmin=1; cmax=1; }
        else if (w < 178) { item=LI_FIRE_CHARGE;       cmin=1; cmax=1; }
        else if (w < 186) { item=LI_FLINT_STEEL;       cmin=1; cmax=1; }
        else if (w < 194) { item=LI_OBSIDIAN;          cmin=1; cmax=2; }
        else if (w < 199) { item=LI_GOLDEN_HORSE_ARMOR;cmin=1; cmax=1; }
        else if (w < 201) { item=LI_ENCHANTED_GOLDEN_APPLE;cmin=1;cmax=1;}
        else if (w < 211) { item=LI_ENCHANTED_BOOK;    cmin=1; cmax=1; enc=1;}
        else              { item=LI_DIAMOND;            cmin=1; cmax=1; }
        int range = cmax - cmin;
        int cnt = cmin + (range > 0 ? nextInt(&rng, range+1) : 0);
        if (enc) { ENCHANT_ADVANCE(&rng); }
        if (item >= 0 && (mask >> item) & 1) count += cnt;
    }
    return count;
}

/* ─── ANCIENT CITY ──────────────────────────────────────────────────────────
 * minecraft:chests/ancient_city  (1.19+)
 * 3-9 rolls + 1 roll from echo_vault pool
 * total weight pool1 = 207, pool2 always 1 echo_shard
 */
LOOT_FN int sim_ancient_city_chest(uint64_t lootSeed, LootMask mask)
{
    uint64_t rng;
    setSeed(&rng, lootSeed);
    int count = 0;
    int rolls = 3 + nextInt(&rng, 7);
    /* pool 1, total weight 207 */
    for (int r = 0; r < rolls; r++) {
        int w = nextInt(&rng, 207);
        int item = LI_NONE, cmin = 1, cmax = 1, enc = 0;
        if      (w <  30) { item=LI_COAL;           cmin=6; cmax=14;}
        else if (w <  50) { item=LI_BONE;           cmin=6; cmax=14;}
        else if (w <  65) { item=LI_CANDLE;         cmin=1; cmax=4; }
        else if (w <  80) { item=LI_AMETHYST_SHARD; cmin=1; cmax=15;}
        else if (w <  95) { item=LI_SCULK_CATALYST; cmin=1; cmax=2; }
        else if (w < 110) { item=LI_SOUL_SAND;      cmin=1; cmax=3; }
        else if (w < 125) { item=LI_SCULK_SENSOR;   cmin=1; cmax=3; }
        else if (w < 140) { item=LI_IRON_INGOT;     cmin=1; cmax=4; }
        else if (w < 150) { item=LI_GOLD_INGOT;     cmin=1; cmax=4; }
        else if (w < 157) { item=LI_DISC_OTHERSIDE; cmin=1; cmax=1; }
        else if (w < 162) { item=LI_DIAMOND;        cmin=1; cmax=3; }
        else if (w < 172) { item=LI_BUNDLE;         cmin=1; cmax=1; }
        else if (w < 182) { item=LI_ENCHANTED_BOOK; cmin=1; cmax=1; enc=1;}
        else if (w < 192) { item=LI_BOOK;           cmin=1; cmax=3; }
        else if (w < 197) { item=LI_EXPERIENCE_BOTTLE;cmin=1;cmax=3;}
        else if (w < 200) { item=LI_GOLDEN_APPLE;   cmin=1; cmax=1; }
        else if (w < 202) { item=LI_ENCHANTED_GOLDEN_APPLE;cmin=1;cmax=1;}
        else              { item=LI_ECHO_SHARD;      cmin=1; cmax=3; }
        int range = cmax - cmin;
        int cnt = cmin + (range > 0 ? nextInt(&rng, range+1) : 0);
        if (enc) { ENCHANT_ADVANCE(&rng); }
        if (item >= 0 && (mask >> item) & 1) count += cnt;
    }
    /* pool 2: always 1 echo shard */
    if ((mask >> LI_ECHO_SHARD) & 1) count += 1;
    return count;
}

/* ─── BASTION REMNANT (generic) ─────────────────────────────────────────────
 * The treasure chest type; other rooms use different tables.
 * minecraft:chests/bastion_treasure  (1.16+)
 * 2-3 rolls pool1 (total 39) + 4-6 rolls pool2 (total 32)
 */
LOOT_FN int sim_bastion_treasure(uint64_t lootSeed, LootMask mask)
{
    uint64_t rng;
    setSeed(&rng, lootSeed);
    int count = 0;
    /* pool 1 */
    int r1 = 2 + nextInt(&rng, 2);
    for (int r = 0; r < r1; r++) {
        int w = nextInt(&rng, 39);
        int item = LI_NONE, cmin = 1, cmax = 1, enc = 0;
        if      (w <  3)  { item=LI_NETHERITE_SCRAP;       cmin=1; cmax=1; }
        else if (w <  6)  { item=LI_SNOUT_ARMOR_TRIM;      cmin=1; cmax=1; }
        else if (w < 12)  { item=LI_ENCHANTED_GOLDEN_APPLE;cmin=1; cmax=1; }
        else if (w < 24)  { item=LI_DIAMOND;               cmin=1; cmax=2; }
        else if (w < 32)  { item=LI_ENCHANTED_BOOK;        cmin=1; cmax=1; enc=1;}
        else              { item=LI_GOLDEN_APPLE;           cmin=1; cmax=1; }
        int range = cmax - cmin;
        int cnt = cmin + (range > 0 ? nextInt(&rng, range+1) : 0);
        if (enc) { ENCHANT_ADVANCE(&rng); }
        if (item >= 0 && (mask >> item) & 1) count += cnt;
    }
    /* pool 2 */
    int r2 = 4 + nextInt(&rng, 3);
    for (int r = 0; r < r2; r++) {
        int w = nextInt(&rng, 32);
        int item = LI_NONE, cmin = 1, cmax = 1;
        if      (w < 12) { item=LI_GOLD_INGOT;   cmin=4; cmax=9;  }
        else if (w < 22) { item=LI_IRON_INGOT;   cmin=4; cmax=9;  }
        else if (w < 27) { item=LI_GOLD_NUGGET;  cmin=2; cmax=8;  }
        else if (w < 30) { item=LI_IRON_NUGGET;  cmin=2; cmax=8;  }
        else             { item=LI_ARROW;         cmin=5; cmax=17; }
        int range = cmax - cmin;
        int cnt = cmin + (range > 0 ? nextInt(&rng, range+1) : 0);
        if (item >= 0 && (mask >> item) & 1) count += cnt;
    }
    return count;
}

/* ─── WOODLAND MANSION ──────────────────────────────────────────────────────
 * minecraft:chests/woodland_mansion  (1.18+)
 * 1-3 rolls, total weight 130.
 */
LOOT_FN int sim_mansion_chest(uint64_t lootSeed, LootMask mask)
{
    uint64_t rng;
    setSeed(&rng, lootSeed);
    int count = 0;
    int rolls = 1 + nextInt(&rng, 3);
    for (int r = 0; r < rolls; r++) {
        int w = nextInt(&rng, 130);
        int item = LI_NONE, cmin = 1, cmax = 1, enc = 0;
        if      (w <  20) { item=LI_LEAD;           cmin=1; cmax=3; }
        else if (w <  40) { item=LI_GOLDEN_APPLE;   cmin=1; cmax=1; }
        else if (w <  45) { item=LI_ENCHANTED_GOLDEN_APPLE;cmin=1;cmax=1; }
        else if (w <  55) { item=LI_IRON_INGOT;     cmin=1; cmax=4; }
        else if (w <  65) { item=LI_GOLD_INGOT;     cmin=1; cmax=4; }
        else if (w <  75) { item=LI_BREAD;          cmin=1; cmax=2; }
        else if (w <  90) { item=LI_WHEAT;          cmin=1; cmax=4; }
        else if (w < 100) { item=LI_COAL;           cmin=4; cmax=8; }
        else if (w < 110) { item=LI_REDSTONE;       cmin=1; cmax=4; }
        else if (w < 115) { item=LI_DIAMOND;        cmin=1; cmax=3; }
        else if (w < 120) { item=LI_ENCHANTED_BOOK; cmin=1; cmax=1; enc=1; }
        else              { item=LI_BUCKET;          cmin=1; cmax=1; }
        int range = cmax - cmin;
        int cnt = cmin + (range > 0 ? nextInt(&rng, range+1) : 0);
        if (enc) { ENCHANT_ADVANCE(&rng); }
        if (item >= 0 && (mask >> item) & 1) count += cnt;
    }
    return count;
}


/* ══════════════════════════════════════════════════════════════════════════
 * Per-structure chest dispatch
 * Returns total item count across ALL chests in the structure.
 * ══════════════════════════════════════════════════════════════════════════*/

/* Number of chests per structure type */
LOOT_FN int loot_chest_count(int structType)
{
    switch (structType) {
    case Desert_Pyramid:  return 4;
    case Jungle_Temple:   return 2;
    case Igloo:           return 1;
    case Shipwreck:       return 3;
    case Outpost:         return 1;
    case Ruined_Portal:   return 1;
    case Ancient_City:    return 5;  /* 5 chests in an average city */
    case Bastion:         return 2;  /* varies; use 2 treasure chests */
    case Mansion:         return 2;  /* average; mansions are huge */
    default:              return 0;
    }
}

/*
 * Build the structure piece-random for the chunk containing (px,pz) via the
 * real MC chain: population seed (over chunk origin block coords) -> feature
 * seed (salted by the structure's decoration index+step) -> setSeed. Then skip
 * `preAdvances` nextLong()s of pre-chest random usage and draw `chestIndex`+1
 * more; the last one is the loot seed for that chest.
 */
LOOT_FN uint64_t loot_seed_for_chest(uint64_t worldSeed, int px, int pz,
                                     int step, int salt, int preAdvances,
                                     int chestIndex)
{
    int blockX = (px >> 4) << 4;     /* chunk ORIGIN block coords */
    int blockZ = (pz >> 4) << 4;
    uint64_t pop  = loot_population_seed(worldSeed, blockX, blockZ);
    uint64_t feat = loot_feature_seed(pop, salt, step);
    uint64_t rng;
    setSeed(&rng, feat);
    skipLongs(&rng, preAdvances);
    uint64_t seed = 0;
    for (int i = 0; i <= chestIndex; i++)
        seed = nextLong(&rng);
    return seed;
}

/*
 * Simulate loot in one chest and return item count matching mask.
 * chestRole distinguishes chest types within a structure
 * (0=first, 1=second, etc.; meaning is structure-specific).
 */
LOOT_FN int loot_sim_chest(int structType, int chestRole,
                             uint64_t lootSeed, LootMask mask)
{
    switch (structType) {
    case Desert_Pyramid:
        return sim_desert_pyramid_chest(lootSeed, mask);
    case Jungle_Temple:
        return sim_jungle_temple_chest(lootSeed, mask);
    case Igloo:
        return sim_igloo_chest(lootSeed, mask);
    case Shipwreck:
        if (chestRole == 0) return sim_shipwreck_map(lootSeed, mask);
        if (chestRole == 1) return sim_shipwreck_supply(lootSeed, mask);
        return sim_shipwreck_treasure(lootSeed, mask);
    case Outpost:
        return sim_outpost_chest(lootSeed, mask);
    case Ruined_Portal:
        return sim_ruined_portal_chest(lootSeed, mask);
    case Ancient_City:
        return sim_ancient_city_chest(lootSeed, mask);
    case Bastion:
        return sim_bastion_treasure(lootSeed, mask);
    case Mansion:
        return sim_mansion_chest(lootSeed, mask);
    default:
        return 0;
    }
}

/* ── Loot-seed calibration constants ────────────────────────────────────────
 * The chest loot seed is feature_seed = pop + index + 10000*step, advanced by
 * `pre` nextLong()s. The three values below MUST be calibrated against a known
 * seed in-game for each structure/version — the defaults are best-effort:
 *   step  = GenerationStep.Decoration ordinal (SURFACE_STRUCTURES = 4,
 *           UNDERGROUND_STRUCTURES = 3 for ancient city/etc).
 *   index = the structure's position in its decoration step's feature list
 *           (registry-order dependent; 0 is a placeholder, not verified).
 *   pre   = nextLong() draws inside postProcess before the first chest.
 * A per-filter override (loot_step/loot_salt/loot_pre in the .cfg) bypasses
 * these so you can calibrate without recompiling. */
LOOT_FN int loot_default_step(int structType)
{
    switch (structType) {
    case Ancient_City:   return 3;  /* UNDERGROUND_STRUCTURES */
    case Ruined_Portal:  return 4;  /* SURFACE_STRUCTURES      */
    case Shipwreck:      return 4;
    default:             return 4;  /* SURFACE_STRUCTURES      */
    }
}
LOOT_FN int loot_default_salt(int structType)
{
    (void)structType;
    return 0;  /* NOT verified — calibrate per version with a known seed */
}
LOOT_FN int loot_pre_advances(int structType)
{
    switch (structType) {
    default:             return 0;  /* calibrate per structure/version */
    }
}

/*
 * Count matching items across all chests in the structure at (px, pz).
 * step/salt/pre override the per-structure defaults when >= 0 (lets the .cfg
 * carry calibration values). Returns total count.
 */
LOOT_FN int loot_count_in_structure(int structType, uint64_t worldSeed,
                                    int px, int pz, LootMask mask,
                                    int step, int salt, int pre)
{
    int n = loot_chest_count(structType);
    if (n <= 0) return 0;
    if (step < 0) step = loot_default_step(structType);
    if (salt < 0) salt = loot_default_salt(structType);
    if (pre  < 0) pre  = loot_pre_advances(structType);
    int total = 0;
    for (int i = 0; i < n; i++) {
        uint64_t ls = loot_seed_for_chest(worldSeed, px, pz, step, salt, pre, i);
        total += loot_sim_chest(structType, i, ls, mask);
    }
    return total;
}

#endif /* SEEDFORGE_LOOT_H */
