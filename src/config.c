/* config.c — minimal INI-ish loader. One [filter] block per filter.
 * Designed to be hand-edited; unknown keys are ignored so it stays forgiving.
 */
#include "config.h"
#include "generator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int biome_from_name(const char *s);   /* fwd */
static int struct_from_name(const char *s);
static int mc_from_name(const char *s);
static uint64_t loot_mask_from_list(const char *s);
static int add_biomes_list(const char *s, int *arr, int *n, int max);

static char *trim(char *s) {
    while (*s==' '||*s=='\t') s++;
    char *e = s + strlen(s);
    while (e>s && (e[-1]=='\n'||e[-1]=='\r'||e[-1]==' '||e[-1]=='\t')) *--e=0;
    return s;
}

static void default_cost(Filter *f) {
    switch (f->kind) {
        case F_QUAD_HUT:     f->cost = 0;   break;
        case F_BIOME_AT:     f->cost = 10;  break;
        case F_STRUCTURE:    f->cost = 30;  break;
        case F_BIOME_RADIUS: f->cost = 50;  break;
        case F_BIOME_AREA:   f->cost = 60;  break; /* grid scan, spawn optional */
        case F_BIOME_SIZE:   f->cost = 70;  break; /* flood fill, precise+heavy */
        case F_SPAWN_BIOME:  f->cost = 90;  break;
        case F_CHEESE_CAVE:  f->cost = 100; break; /* noise init + flood fill  */
        case F_LOOT:         f->cost = 120; break; /* structure pos + loot sim */
    }
}

int load_profile(const char *path, SearchProfile *p) {
    FILE *fp = fopen(path, "r");
    if (!fp) { fprintf(stderr, "cannot open %s\n", path); return -1; }

    memset(p, 0, sizeof(*p));
    p->mc_version = MC_NEWEST;
    p->dim = DIM_OVERWORLD;
    p->seed_start = 0;
    p->seed_end = 1ULL << 20;
    p->num_gpus = 0;
    strcpy(p->out_path, "hits.txt");

    Filter *cur = NULL;
    char line[1024];
    while (fgets(line, sizeof line, fp)) {
        char *s = trim(line);
        if (!*s || *s=='#' || *s==';') continue;

        if (strcmp(s, "[filter]") == 0) {
            if (p->num_filters >= SF_MAX_FILTERS) {
                fprintf(stderr, "too many filters\n"); break;
            }
            cur = &p->filters[p->num_filters++];
            memset(cur, 0, sizeof(*cur));
            cur->biome = -1; cur->structure = -1; cur->y = 63;
            cur->step = 16; cur->want_count = 1; cur->cost = -1;
            cur->loot_step = -1; cur->loot_salt = -1; cur->loot_pre = -1;
            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(s), *v = trim(eq+1);

        if (!cur) { /* global section */
            if      (!strcmp(k,"mc"))         p->mc_version = mc_from_name(v);
            else if (!strcmp(k,"dim"))        p->dim = atoi(v);
            else if (!strcmp(k,"seed_start")) p->seed_start = strtoull(v,0,0);
            else if (!strcmp(k,"seed_end"))   p->seed_end   = strtoull(v,0,0);
            else if (!strcmp(k,"out"))        { strncpy(p->out_path,v,511); }
            else if (!strcmp(k,"verbose"))    p->verbose = atoi(v);
            else if (!strcmp(k,"top_n") || !strcmp(k,"keep")) p->top_n = atoi(v);
            else if (!strcmp(k,"gpus")) {
                char *tok = strtok(v, ",");
                while (tok && p->num_gpus < 8) {
                    p->gpus[p->num_gpus++] = atoi(tok);
                    tok = strtok(NULL, ",");
                }
            }
        } else {
            if      (!strcmp(k,"kind")) {
                if      (!strcmp(v,"biome_at"))     cur->kind=F_BIOME_AT;
                else if (!strcmp(v,"biome_radius")) cur->kind=F_BIOME_RADIUS;
                else if (!strcmp(v,"structure"))    cur->kind=F_STRUCTURE;
                else if (!strcmp(v,"spawn_biome"))  cur->kind=F_SPAWN_BIOME;
                else if (!strcmp(v,"quad_hut"))     cur->kind=F_QUAD_HUT;
                else if (!strcmp(v,"loot"))         cur->kind=F_LOOT;
                else if (!strcmp(v,"biome_area"))   cur->kind=F_BIOME_AREA;
                else if (!strcmp(v,"biome_size"))   cur->kind=F_BIOME_SIZE;
                else if (!strcmp(v,"cheese_cave"))  {
                    cur->kind=F_CHEESE_CAVE;
                    /* Sentinel: threshold<0 → use default 0.5 in eval_filter.
                     * Override the y=63 global default to something underground. */
                    cur->cave_threshold = -1.0;
                    cur->y = -40;
                }
            }
            else if (!strcmp(k,"x"))        cur->x=atoi(v);
            else if (!strcmp(k,"z"))        cur->z=atoi(v);
            else if (!strcmp(k,"y"))        cur->y=atoi(v);
            else if (!strcmp(k,"radius"))   cur->radius=atoi(v);
            else if (!strcmp(k,"step"))     cur->step=atoi(v);
            else if (!strcmp(k,"count"))    cur->want_count=atoi(v);
            else if (!strcmp(k,"cost"))     cur->cost=atoi(v);
            else if (!strcmp(k,"biome"))    cur->biome=biome_from_name(v);
            else if (!strcmp(k,"min_area") || !strcmp(k,"min_blocks"))
                cur->min_area = atoi(v);
            /* biome CATEGORY / set: accepts a comma list mixing biome names and
             * category keywords (mountains, oceans, forests, jungles, taigas,
             * badlands, peaks). Lets one filter target a whole biome group. */
            else if (!strcmp(k,"biomes") || !strcmp(k,"category"))
                add_biomes_list(v, cur->match_biomes,
                                &cur->num_match_biomes, 16);
            else if (!strcmp(k,"structure"))cur->structure=struct_from_name(v);
            else if (!strcmp(k,"spawn_biomes")) {
                char *tok = strtok(v, ",");
                while (tok && cur->num_spawn_biomes < 16) {
                    int b = biome_from_name(trim(tok));
                    if (b >= 0)
                        cur->spawn_biomes[cur->num_spawn_biomes++] = b;
                    tok = strtok(NULL, ",");
                }
            }
            else if (!strcmp(k,"loot_items") || !strcmp(k,"loot_item"))
                cur->loot_mask |= loot_mask_from_list(v);
            else if (!strcmp(k,"loot_min") || !strcmp(k,"loot_count"))
                cur->loot_min = atoi(v);
            else if (!strcmp(k,"loot_step"))  cur->loot_step = atoi(v);
            else if (!strcmp(k,"loot_salt"))  cur->loot_salt = atoi(v);
            else if (!strcmp(k,"loot_pre"))   cur->loot_pre  = atoi(v);
            else if (!strcmp(k,"center_spawn") || !strcmp(k,"at_spawn"))
                cur->center_spawn = atoi(v);
            else if (!strcmp(k,"cave_threshold"))
                cur->cave_threshold = atof(v);
        }
    }
    fclose(fp);

    for (int i=0;i<p->num_filters;i++)
        if (p->filters[i].cost < 0) default_cost(&p->filters[i]);

    /* stable insertion sort by cost: cheap filters first */
    for (int i=1;i<p->num_filters;i++) {
        Filter t=p->filters[i]; int j=i-1;
        while (j>=0 && p->filters[j].cost>t.cost){p->filters[j+1]=p->filters[j];j--;}
        p->filters[j+1]=t;
    }
    return 0;
}

void print_profile(const SearchProfile *p) {
    printf("MC=%d dim=%d seeds=[%llu,%llu) filters=%d gpus=%d\n",
        p->mc_version, p->dim,
        (unsigned long long)p->seed_start,(unsigned long long)p->seed_end,
        p->num_filters, p->num_gpus);
    for (int i=0;i<p->num_filters;i++) {
        const Filter*f=&p->filters[i];
        printf("  [%d] kind=%d cost=%d xz=(%d,%d) r=%d biome=%d struct=%d\n",
            i,f->kind,f->cost,f->x,f->z,f->radius,f->biome,f->structure);
    }
}

/* --- tiny name tables (extend as needed) --- */
static int mc_from_name(const char *s){
    if(!strcmp(s,"1.21"))return MC_1_21; if(!strcmp(s,"1.20"))return MC_1_20;
    if(!strcmp(s,"1.21.1"))return MC_1_21_1; if(!strcmp(s,"1.21.3"))return MC_1_21_3;
    return MC_NEWEST;
}
static int biome_from_name(const char *s){
    /* Full overworld biome set (1.18+ names; legacy aliases accepted too).
     * Keep in sync with the UI biome list in ui/index.html. */
    struct{const char*n;int v;} t[]={
        /* oceans & rivers */
        {"ocean",ocean},{"deep_ocean",deep_ocean},
        {"warm_ocean",warm_ocean},{"lukewarm_ocean",lukewarm_ocean},
        {"cold_ocean",cold_ocean},{"frozen_ocean",frozen_ocean},
        {"deep_lukewarm_ocean",deep_lukewarm_ocean},
        {"deep_cold_ocean",deep_cold_ocean},
        {"deep_frozen_ocean",deep_frozen_ocean},
        {"river",river},{"frozen_river",frozen_river},
        /* flat / warm land */
        {"plains",plains},{"sunflower_plains",sunflower_plains},
        {"desert",desert},{"savanna",savanna},
        {"savanna_plateau",savanna_plateau},
        {"windswept_savanna",windswept_savanna},
        {"snowy_plains",snowy_plains},{"snowy_tundra",snowy_tundra},
        {"ice_spikes",ice_spikes},
        /* forests */
        {"forest",forest},{"flower_forest",flower_forest},
        {"birch_forest",birch_forest},
        {"old_growth_birch_forest",old_growth_birch_forest},
        {"dark_forest",dark_forest},
        {"taiga",taiga},{"snowy_taiga",snowy_taiga},
        {"old_growth_pine_taiga",old_growth_pine_taiga},
        {"old_growth_spruce_taiga",old_growth_spruce_taiga},
        {"grove",grove},
        /* jungles */
        {"jungle",jungle},{"sparse_jungle",sparse_jungle},
        {"bamboo_jungle",bamboo_jungle},
        /* mountains / peaks */
        {"mountains",mountains},{"windswept_hills",windswept_hills},
        {"windswept_forest",windswept_forest},
        {"windswept_gravelly_hills",windswept_gravelly_hills},
        {"gravelly_mountains",gravelly_mountains},
        {"wooded_mountains",wooded_mountains},
        {"meadow",meadow},{"snowy_slopes",snowy_slopes},
        {"jagged_peaks",jagged_peaks},{"frozen_peaks",frozen_peaks},
        {"stony_peaks",stony_peaks},{"stony_shore",stony_shore},
        /* swamp / mushroom / badlands / special */
        {"swamp",swamp},{"mangrove_swamp",mangrove_swamp},
        {"mushroom_fields",mushroom_fields},
        {"badlands",badlands},{"wooded_badlands",wooded_badlands},
        {"eroded_badlands",eroded_badlands},
        {"cherry_grove",cherry_grove},{"pale_garden",pale_garden},
        {"deep_dark",deep_dark},
        {"dripstone_caves",dripstone_caves},{"lush_caves",lush_caves},
        {"beach",beach},{"snowy_beach",snowy_beach},
        /* nether (for dim=-1 searches) */
        {"nether_wastes",nether_wastes},{"soul_sand_valley",soul_sand_valley},
        {"crimson_forest",crimson_forest},{"warped_forest",warped_forest},
        {"basalt_deltas",basalt_deltas},
        {0,0}};
    for(int i=0;t[i].n;i++) if(!strcmp(s,t[i].n)) return t[i].v;
    /* allow raw numeric ids too */
    if(s[0]>='0'&&s[0]<='9') return atoi(s);
    return -1;
}

/* Expand a category keyword (e.g. "mountains") into its member biome ids,
 * appending to arr (n is bounded by max). Returns the number appended, or -1 if
 * the keyword is not a known category. Singular and plural both accepted. */
static int expand_category(const char *name, int *arr, int *n, int max){
    struct { const char *cat; int ids[16]; int k; } C[] = {
        /* The 1.18+ mountain terrain group — what "biggest mountain biome"
         * really means, since the peaks/slopes/meadow/grove all border and
         * flow into one another and read as one massive mountain range. */
        {"mountains", {mountains, wooded_mountains, gravelly_mountains,
                       meadow, grove, snowy_slopes, jagged_peaks,
                       frozen_peaks, stony_peaks}, 9},
        {"peaks",     {jagged_peaks, frozen_peaks, stony_peaks}, 3},
        {"oceans",    {ocean, deep_ocean, warm_ocean, lukewarm_ocean,
                       cold_ocean, frozen_ocean, deep_lukewarm_ocean,
                       deep_cold_ocean, deep_frozen_ocean}, 9},
        {"forests",   {forest, flower_forest, birch_forest,
                       old_growth_birch_forest, dark_forest}, 5},
        {"jungles",   {jungle, sparse_jungle, bamboo_jungle}, 3},
        {"taigas",    {taiga, snowy_taiga, old_growth_pine_taiga,
                       old_growth_spruce_taiga}, 4},
        {"badlands",  {badlands, wooded_badlands, eroded_badlands}, 3},
        {0,{0},0}
    };
    char plural[64];
    snprintf(plural, sizeof plural, "%ss", name);   /* mountain -> mountains */
    for (int i=0; C[i].cat; i++) {
        if (!strcmp(name, C[i].cat) || !strcmp(plural, C[i].cat)) {
            int added = 0;
            for (int j=0; j<C[i].k && *n<max; j++) {
                arr[(*n)++] = C[i].ids[j];
                added++;
            }
            return added;
        }
    }
    return -1;
}

/* Parse a comma list that may mix biome names and category keywords into the
 * filter's match set. Returns the new total count. */
static int add_biomes_list(const char *orig, int *arr, int *n, int max){
    char buf[512]; strncpy(buf, orig, 511); buf[511]=0;
    char *tok = strtok(buf, ",");
    while (tok && *n < max) {
        char *t = trim(tok);
        if (expand_category(t, arr, n, max) < 0) {
            int b = biome_from_name(t);
            if (b >= 0 && *n < max) arr[(*n)++] = b;
        }
        tok = strtok(NULL, ",");
    }
    return *n;
}

static int struct_from_name(const char *s){
    struct{const char*n;int v;} t[]={
        {"village",Village},{"desert_pyramid",Desert_Pyramid},
        {"jungle_temple",Jungle_Temple},{"swamp_hut",Swamp_Hut},
        {"monument",Monument},{"mansion",Mansion},{"outpost",Outpost},
        {"ruined_portal",Ruined_Portal},{"ancient_city",Ancient_City},
        {"trail_ruins",Trail_Ruins},{"igloo",Igloo},{"shipwreck",Shipwreck},
        {0,0}};
    for(int i=0;t[i].n;i++) if(!strcmp(s,t[i].n)) return t[i].v;
    if(s[0]>='0'&&s[0]<='9') return atoi(s);
    return -1;
}

/* include loot item name table here to avoid circular header deps */
#include "loot.h"
static int loot_item_from_name(const char *s) {
    struct{const char*n;int v;} t[]={
        {"golden_apple",            LI_GOLDEN_APPLE},
        {"enchanted_golden_apple",  LI_ENCHANTED_GOLDEN_APPLE},
        {"diamond",                 LI_DIAMOND},
        {"emerald",                 LI_EMERALD},
        {"gold_ingot",              LI_GOLD_INGOT},
        {"iron_ingot",              LI_IRON_INGOT},
        {"gold_nugget",             LI_GOLD_NUGGET},
        {"iron_nugget",             LI_IRON_NUGGET},
        {"saddle",                  LI_SADDLE},
        {"golden_horse_armor",      LI_GOLD_HORSE_ARMOR},
        {"iron_horse_armor",        LI_IRON_HORSE_ARMOR},
        {"diamond_horse_armor",     LI_DIAMOND_HORSE_ARMOR},
        {"rotten_flesh",            LI_ROTTEN_FLESH},
        {"bone",                    LI_BONE},
        {"spider_eye",              LI_SPIDER_EYE},
        {"gunpowder",               LI_GUNPOWDER},
        {"coal",                    LI_COAL},
        {"bread",                   LI_BREAD},
        {"wheat",                   LI_WHEAT},
        {"paper",                   LI_PAPER},
        {"feather",                 LI_FEATHER},
        {"clock",                   LI_CLOCK},
        {"compass",                 LI_COMPASS},
        {"map",                     LI_MAP},
        {"carrot",                  LI_CARROT},
        {"tnt",                     LI_TNT},
        {"enchanted_book",          LI_ENCHANTED_BOOK},
        {"leather_chestplate",      LI_LEATHER_CHESTPLATE},
        {"iron_sword",              LI_IRON_SWORD},
        {"experience_bottle",       LI_EXPERIENCE_BOTTLE},
        {"echo_shard",              LI_ECHO_SHARD},
        {"music_disc_otherside",    LI_DISC_OTHERSIDE},
        {"sculk_catalyst",          LI_SCULK_CATALYST},
        {"amethyst_shard",          LI_AMETHYST_SHARD},
        {"bundle",                  LI_BUNDLE},
        {"glow_berries",            LI_GLOW_BERRIES},
        {"candle",                  LI_CANDLE},
        {0,0}};
    for(int i=0;t[i].n;i++) if(!strcmp(s,t[i].n)) return t[i].v;
    return -1;
}
static uint64_t loot_mask_from_list(const char *orig) {
    char buf[512]; strncpy(buf, orig, 511); buf[511]=0;
    uint64_t mask = 0;
    char *tok = strtok(buf, ",");
    while (tok) {
        int id = loot_item_from_name(trim(tok));
        if (id >= 0 && id < 64) mask |= (1ULL << id);
        tok = strtok(NULL, ",");
    }
    return mask;
}
