// PyCity - alpha
// A top-down tile-grid city sim: place roads and buildings, trucks path
// between buildings automatically along the road network, driven by a
// simple supply/demand economy.
//
// Controls:
//   1 = Road tool     2 = House tool     3 = Factory tool   4 = Farm tool
//   5 = Police tool   6 = Bulldoze
//   Left click        = place/remove on hovered tile
//   Arrows / WASD     = pan camera
//   Mouse wheel       = zoom in/out (centered on screen)   -/= = zoom (keyboard alt)
//   Space             = pause/unpause simulation
//   F5                = save     F9 = load
//   Esc               = quit
//
// Money: building costs money (road $10, house $50, factory $150, farm
// $100, police station $200), houses pay a flat tax over time regardless of
// how well they're served, and there's no debt - insufficient funds just
// blocks placement. Bulldozing refunds 50% of a tile's build cost.
//
// Economy chain: farms grow food (supply) -> trucks haul it to factories that
// need it (demand) -> factories turn it into goods (supply) -> trucks haul
// THOSE to houses that need them (demand). Two truck routes, not one.
//
// Resource types (new in v0.0.9): the food and goods chains each come in two
// distinct flavors instead of one generic "food"/"goods" value - Grain/Bread
// and Timber/Furniture. Every farm, factory, and house is assigned one of
// the two chains at placement time (random, shown as a small corner dot -
// gold for Grain/Bread, teal for Timber/Furniture) and trucks will only
// move cargo between buildings on the SAME chain: a Grain farm won't feed a
// Timber factory, and a Furniture factory won't supply a Grain-hungry
// house. This means a city needs a healthy mix of both chains, not just
// "more farms" - a house wanting Furniture will starve forever if every
// factory in town is on the Bread chain.
//
// Road congestion: every road tile tracks how much truck traffic has
// recently passed over it. Heavily-used tiles slow trucks down (and are
// drawn with a red tint), which naturally discourages funneling every
// route through one choke point - a second parallel road relieves it.
//
// Police & crime: houses not within range of a police station slowly build
// up crime; crime decays back down for houses that are covered. Houses with
// high crime have a chance each frame of a theft event that drains a bit of
// money. Build police stations to keep crime (and theft) in check.
//
// April Fools: if the real-world date is April 1st, the game periodically
// scatters silly decorative items on empty grass tiles. Purely cosmetic.
//
// Build: gcc main.c -o pycity -Iraylib/src -Lraylib/src -lraylib -lm -lpthread -ldl -lrt -lX11
// Run:   ./pycity
//
// Assets folder layout:
//   assets/base/            - normal tiles (everything except farm)
//     tile_0025.png  -> road
//     tile_0100.png  -> house
//     tile_0073.png  -> factory
//     tile_0000.png, tile_0001.png, tile_0002.png -> grass variants
//   assets/farm/             - farm building art (Kenney "Tiny Farm" pack -
//     tile_0091.png             the farm building
//     tile_0097.png             haybale icon, overlaid on a farm once it
//                               has enough crops grown to send a truck
//   assets/april_fools/      - drop ANY .png props in here, no naming
//                              needed. On April 1st the game scans this
//                              folder at startup and scatters whichever
//                              images it finds on random empty tiles.
//   assets/winter/           - winter reskin tiles (Kenney "Tiny Ski" pack -
//     tile_0000.png             \
//     tile_0004.png              > winter grass variants (same 3-slot
//     tile_0005.png             /  scheme as assets/base/'s grass)
//     tile_0028.png             winter road/street tile
//                               During winter months (Dec/Jan/Feb) these
//                               replace the base grass/road art; falls
//                               back to the normal base tiles if missing.
// Every one of these gracefully falls back to a flat color / simple shape
// if its folder is empty or missing - same pattern as always in this file.

#include "raylib.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <time.h>

// The map is bigger than one screen now - the camera scrolls over it.
#define MAP_COLS 60
#define MAP_ROWS 40

// How many tiles are actually visible on screen at once.
#define VIEW_COLS 30
#define VIEW_ROWS 20

#define TILE 32
#define SCREEN_W (VIEW_COLS*TILE)
#define SCREEN_H (VIEW_ROWS*TILE + 60) // +60 for the toolbar at top
#define TOP_BAR 60
#define MAX_TRUCKS 64
#define MAX_BUILDINGS 128
#define MAX_PATH (MAP_COLS*MAP_ROWS)

#define SAVE_FILE "savegame.dat"
// Bumped again from 0x50434932 ("PCI2") because Building grew a
// `resourceType` field this release (see "Resource types" in the header
// comment above) - same reasoning as the previous bump: an old save read
// with the new struct layout would silently misalign every field after
// it, so old saves must be rejected rather than partially loaded.
// LoadGame() already treats a magic mismatch as "corrupt or incompatible
// version" and just starts fresh.
#define SAVE_MAGIC 0x50434933 // "PCI3"

// ---- Money tuning ----
// v0.0.9 tuning pass note: these were all first-guess placeholders through
// v0.0.7/8, never actually playtested. The values below are a reasoned
// first tuning pass (worked out from the math - payback times, time-to-
// threshold, etc. - not from real play sessions), aimed at fixing the most
// obviously-broken numbers (see per-constant notes). They're a better
// starting point than the old placeholders, but still need real playtesting
// per "Suggested next steps" - don't treat these as final either.
#define STARTING_MONEY 500.0
#define COST_ROAD     10.0
#define COST_HOUSE    50.0
#define COST_FACTORY 150.0
#define COST_FARM    100.0
#define COST_POLICE  200.0
// Old value (0.02) meant a house paid back its own $50 build cost in ~42
// seconds flat - unintentionally the dominant money strategy was just
// "spam houses." Lowered so a house pays itself back in a couple of
// minutes instead, closer to a slow steady trickle than a jackpot.
#define TAX_PER_HOUSE_PER_FRAME 0.006 // flat per house, regardless of how well it's served
#define BULLDOZE_REFUND_PERCENT 0.5  // partial refund only, so build-then-bulldoze isn't free money

// ---- Police & crime tuning ----
#define POLICE_COVERAGE_RADIUS 9       // tiles (Manhattan distance) a station protects
// Old growth/theft-chance combo let an uncovered house hit the theft
// threshold in under a minute and then get robbed roughly every ~7
// seconds after that (~$1.14/sec average drain) - faster than a single
// house's tax income, so one uncovered house could bleed the whole city's
// treasury. Slowed crime growth and thefts so an uncovered house is a
// real but survivable problem, not an instant money sink.
#define CRIME_GROWTH_PER_FRAME 0.015f  // how fast crime rises on an uncovered house
#define CRIME_DECAY_PER_FRAME  0.05f   // how fast crime falls on a covered house (faster than it grows)
#define THEFT_CRIME_THRESHOLD 70.0f    // crime has to be at least this high for theft to be possible
#define THEFT_CHANCE_PER_FRAME 900     // 1-in-N per frame per eligible house
#define THEFT_AMOUNT 6.0               // flat money stolen per theft event

// ---- Road congestion tuning ----
#define CONGESTION_PER_TRUCK_PER_FRAME 3.0f // added to a road tile for every truck currently on it
// Decay raised slightly (1.0 -> 1.5) so congestion drains faster once
// trucks move on - on a map this small, the old value let a single busy
// intersection stay "hot" long after traffic actually cleared it.
#define CONGESTION_DECAY_PER_FRAME 1.5f     // how fast a tile's congestion drains back to 0
#define CONGESTION_MAX 100.0f
#define CONGESTION_SLOWDOWN_THRESHOLD 30.0f // congestion below this doesn't slow trucks at all
#define CONGESTION_MAX_SLOWDOWN 0.7f        // at CONGESTION_MAX, trucks move at (1 - this) of normal speed

static double money = STARTING_MONEY;
#define DEMAND_GROWTH_PER_FRAME 0.04f      // how fast a house's demand fills up
#define SUPPLY_GROWTH_PER_FRAME 0.05f      // how fast a farm's crops grow back in
#define FOOD_DEMAND_GROWTH_PER_FRAME 0.05f // how fast a factory's need for food fills up
#define DELIVERY_AMOUNT 35.0f              // how much one truckload moves
#define MIN_DEMAND_TO_SERVE 15.0f          // house/factory needs at least this much demand to get a truck
#define MIN_SUPPLY_TO_PICKUP 15.0f         // farm/factory needs at least this much supply to send a truck

// ---- April Fools tuning ----
#define MAX_APRIL_FOOLS_TEXTURES 32      // cap on how many props we'll load from assets/april_fools/
#define APRIL_FOOLS_SPAWN_CHANCE 300     // 1-in-N per frame while active (roughly every ~5s at 60fps)

// ---- Winter tuning ----
// (no cap needed - winter uses specific named tiles, see LoadAllAssets)

// The two independent economy chains (see "Resource types" in the header
// comment). Farms/factories/houses are each assigned one at placement and
// only ever trade with buildings on the same chain.
typedef enum { RESOURCE_GRAIN = 0, RESOURCE_TIMBER, TOTAL_RESOURCE_TYPES } ResourceType;
static const char *RESOURCE_NAMES[TOTAL_RESOURCE_TYPES] = { "Grain/Bread", "Timber/Furniture" };
// Small corner-dot color used to show which chain a building belongs to.
static const Color RESOURCE_COLORS[TOTAL_RESOURCE_TYPES] = {
    (Color){222, 193, 90, 255},  // gold - Grain/Bread
    (Color){80, 200, 190, 255},  // teal - Timber/Furniture
};

typedef enum { TILE_EMPTY = 0, TILE_ROAD, TILE_HOUSE, TILE_FACTORY, TILE_FARM, TILE_POLICE, TOTAL_TILE_TYPES } TileType;
// TILE_FARM and TILE_POLICE are appended in release order (not inserted
// earlier) so the integer value of every previously-existing tile type is
// unchanged - this matters for old savegame.dat compatibility, though see
// the SAVE_MAGIC bump below: the Building struct itself changed shape this
// release, so old saves are rejected anyway rather than silently misread.
typedef enum { TOOL_ROAD = 0, TOOL_HOUSE, TOOL_FACTORY, TOOL_FARM, TOOL_POLICE, TOOL_BULLDOZE } Tool;

// Cost to place one tile of a given type. Also used to compute the
// bulldoze refund (see BULLDOZE_REFUND_PERCENT).
static double GetBuildCost(TileType type) {
    switch (type) {
        case TILE_ROAD:    return COST_ROAD;
        case TILE_HOUSE:   return COST_HOUSE;
        case TILE_FACTORY: return COST_FACTORY;
        case TILE_FARM:    return COST_FARM;
        case TILE_POLICE:  return COST_POLICE;
        default:           return 0.0;
    }
}

#define TOTAL_GRASS_VARIANTS 3

static TileType grid[MAP_ROWS][MAP_COLS];

// How much recent truck traffic has passed over each road tile, 0-100.
// Rises while trucks are on a tile, decays back toward 0 otherwise. Not
// part of the save format - it's transient traffic, not world state - so
// it always starts at 0 after a fresh load, same as trucks[] does.
static float roadCongestion[MAP_ROWS][MAP_COLS];

// Textures indexed by TileType. gameAssets[TILE_EMPTY] is left blank on
// purpose - empty tiles are drawn using grassTextures[] instead (see below),
// falling back to a flat color if those didn't load.
static Texture2D gameAssets[TOTAL_TILE_TYPES];

// A few interchangeable grass tiles so the ground isn't one flat repeating
// texture. Which variant a given tile uses is picked once and stored in
// grassVariant[][], not re-rolled every frame.
static Texture2D grassTextures[TOTAL_GRASS_VARIANTS];
static int grassVariant[MAP_ROWS][MAP_COLS];

// demand/supply now mean different things depending on where a building sits
// in the chain:
//   TILE_HOUSE:   demand = 0-100, how much it wants a goods delivery
//   TILE_FARM:    supply = 0-100, how much food it has ready to ship
//   TILE_FACTORY: demand = 0-100, how hungry it is for food from a farm
//                 supply = 0-100, how many finished goods it has ready to
//                          ship to a house (only rises when food arrives -
//                          it does NOT grow on its own anymore)
typedef struct {
    int r, c;
    TileType type;
    float demand;
    float supply;
    float crime; // TILE_HOUSE only: 0-100, see POLICE_COVERAGE_RADIUS / THEFT_* above
    // Which of the two economy chains this building belongs to. Set once
    // at AddBuilding() time and never changes. For TILE_FARM: which crop
    // it grows. For TILE_FACTORY: which food it needs AND which goods it
    // makes (always the same chain in and out). For TILE_HOUSE: which
    // goods it wants. Unused (left 0) for TILE_ROAD/TILE_POLICE.
    ResourceType resourceType;
} Building;

static Building buildings[MAX_BUILDINGS];
static int buildingCount = 0;

// Farm decoration: a haybale icon overlaid on a farm tile once it has
// enough supply built up to actually send a truck - a quick visual "ready
// to ship" cue, on top of the meter bar.
static Texture2D farmHayBaleTexture;

// ---- April Fools ----
// Purely cosmetic: on empty tiles only, never blocks or interacts with
// gameplay. -1 means "no item on this tile". Textures are whatever .png
// files happen to be sitting in assets/april_fools/ at startup - see
// LoadPngsFromFolder().
static int aprilFoolsItem[MAP_ROWS][MAP_COLS];
static Texture2D aprilFoolsTextures[MAX_APRIL_FOOLS_TEXTURES];
static int aprilFoolsTextureCount = 0;
static Color aprilFoolsFallbackColor = (Color){255, 210, 60, 255}; // used only if a loaded file somehow fails to draw

// ---- Winter ----
// Seasonal reskin: during Dec/Jan/Feb, empty tiles and roads are drawn
// from these specific tiles instead of assets/base/'s versions. Falls
// back to the normal base art if a winter file is missing.
static Texture2D winterGrassTextures[TOTAL_GRASS_VARIANTS];
static Texture2D winterRoadTexture;
static bool winterActive = false; // recomputed once per frame

typedef struct {
    int pathR[MAX_PATH];
    int pathC[MAX_PATH];
    int pathLen;
    int idx;         // current segment index
    float t;         // 0..1 progress along current segment
    float speed;     // per-frame progress
    Color color;
    bool active;
    int toBuildingIdx; // which building index gets the demand reduction on arrival
} Truck;

static Truck trucks[MAX_TRUCKS];
static int deliveries = 0;
static double totalDemandGenerated = 0.0;
static double totalDemandServed = 0.0;

// Camera: top-left corner of the viewport, in WORLD PIXEL space (not tile
// indices - that only worked back when zoom was always 1:1). camZoom is
// how many screen pixels one world pixel maps to; 1.0 = original scale.
static float camPxC = 0, camPxR = 0;
static float camZoom = 1.0f;
#define ZOOM_MIN 0.5f
#define ZOOM_MAX 2.0f
#define ZOOM_STEP 0.1f
#define CAM_PAN_TILES_PER_SEC 18.0f // world-space pan speed, independent of zoom

// ---- helpers ----

static Vector2 CellCenterWorld(int r, int c) {
    // Position in "world" pixel space (before camera offset/zoom is applied)
    Vector2 v = { c*TILE + TILE/2.0f, r*TILE + TILE/2.0f };
    return v;
}

// Effective on-screen size (px) of one tile at the current zoom. Used
// everywhere a draw call needs a screen-space size or offset derived from
// TILE, since raw TILE is only correct at camZoom == 1.
static float TS(void) { return TILE * camZoom; }

static Vector2 WorldToScreen(Vector2 world) {
    Vector2 s = { (world.x - camPxC) * camZoom, (world.y - camPxR) * camZoom + TOP_BAR };
    return s;
}

// Inverse of WorldToScreen - used for mouse-to-tile picking.
static Vector2 ScreenToWorld(Vector2 screen) {
    Vector2 w = { camPxC + screen.x / camZoom, camPxR + (screen.y - TOP_BAR) / camZoom };
    return w;
}

static bool InBounds(int r, int c) {
    return r >= 0 && r < MAP_ROWS && c >= 0 && c < MAP_COLS;
}

// Computes which grid tiles are actually visible at the current camera
// position and zoom level (end values are exclusive), so draw loops don't
// have to walk the whole map every frame regardless of zoom.
static void GetVisibleTileRange(int *startR, int *endR, int *startC, int *endC) {
    float viewW = SCREEN_W / camZoom;
    float viewH = (SCREEN_H - TOP_BAR) / camZoom;
    *startC = (int)floorf(camPxC / TILE);
    *endC   = (int)ceilf((camPxC + viewW) / TILE) + 1;
    *startR = (int)floorf(camPxR / TILE);
    *endR   = (int)ceilf((camPxR + viewH) / TILE) + 1;
    if (*startC < 0) *startC = 0;
    if (*startR < 0) *startR = 0;
    if (*endC > MAP_COLS) *endC = MAP_COLS;
    if (*endR > MAP_ROWS) *endR = MAP_ROWS;
}

static int RoadNeighbors(int r, int c, int outR[4], int outC[4]) {
    int n = 0;
    int dr[4] = {0,0,1,-1};
    int dc[4] = {1,-1,0,0};
    for (int i = 0; i < 4; i++) {
        int nr = r+dr[i], nc = c+dc[i];
        if (InBounds(nr,nc) && grid[nr][nc] == TILE_ROAD) {
            outR[n] = nr; outC[n] = nc; n++;
        }
    }
    return n;
}

// Find nearest road tile to a building (simple expanding search)
static bool NearestRoad(int br, int bc, int *outR, int *outC) {
    for (int radius = 0; radius < MAP_COLS+MAP_ROWS; radius++) {
        for (int dr = -radius; dr <= radius; dr++) {
            for (int dc = -radius; dc <= radius; dc++) {
                if (abs(dr)+abs(dc) != radius) continue;
                int nr = br+dr, nc = bc+dc;
                if (InBounds(nr,nc) && grid[nr][nc] == TILE_ROAD) {
                    *outR = nr; *outC = nc;
                    return true;
                }
            }
        }
    }
    return false;
}

// BFS pathfinding across road tiles, writes path into truck, returns success
static bool BFSPath(int sr, int sc, int er, int ec, Truck *tr) {
    static int prevR[MAP_ROWS][MAP_COLS], prevC[MAP_ROWS][MAP_COLS];
    static bool visited[MAP_ROWS][MAP_COLS];
    memset(visited, 0, sizeof(visited));

    static int queueR[MAX_PATH], queueC[MAX_PATH];
    int qHead = 0, qTail = 0;
    queueR[qTail] = sr; queueC[qTail] = sc; qTail++;
    visited[sr][sc] = true;
    prevR[sr][sc] = -1; prevC[sr][sc] = -1;

    bool found = false;
    while (qHead < qTail) {
        int r = queueR[qHead], c = queueC[qHead]; qHead++;
        if (r == er && c == ec) { found = true; break; }
        int nr[4], nc[4];
        int n = RoadNeighbors(r, c, nr, nc);
        for (int i = 0; i < n; i++) {
            if (!visited[nr[i]][nc[i]]) {
                visited[nr[i]][nc[i]] = true;
                prevR[nr[i]][nc[i]] = r;
                prevC[nr[i]][nc[i]] = c;
                queueR[qTail] = nr[i]; queueC[qTail] = nc[i]; qTail++;
                if (qTail >= MAX_PATH) break;
            }
        }
    }
    if (!found) return false;

    // Walk back from end to start
    static int tmpR[MAX_PATH], tmpC[MAX_PATH];
    int len = 0;
    int r = er, c = ec;
    while (r != -1) {
        tmpR[len] = r; tmpC[len] = c; len++;
        int pr = prevR[r][c], pc = prevC[r][c];
        r = pr; c = pc;
    }
    // reverse into truck path
    tr->pathLen = len;
    for (int i = 0; i < len; i++) {
        tr->pathR[i] = tmpR[len-1-i];
        tr->pathC[i] = tmpC[len-1-i];
    }
    return true;
}

// Picks a real trip along one leg of the chain (a source type with supply
// ready to ship, to a destination type that actually wants it) instead of
// just grabbing two random buildings. Both ends must also be on the same
// resourceType chain - a Grain farm's trucks never visit a Timber
// factory, even if the factory is starving. Returns true if a truck was
// spawned.
static bool SpawnTruckLeg(TileType fromType, TileType toType, ResourceType resType, Color truckColor) {
    int fromCandidates[MAX_BUILDINGS];
    int toCandidates[MAX_BUILDINGS];
    int fromCount = 0, toCount = 0;

    for (int i = 0; i < buildingCount; i++) {
        if (buildings[i].resourceType != resType) continue;
        if (buildings[i].type == fromType && buildings[i].supply >= MIN_SUPPLY_TO_PICKUP) {
            fromCandidates[fromCount++] = i;
        } else if (buildings[i].type == toType && buildings[i].demand >= MIN_DEMAND_TO_SERVE) {
            toCandidates[toCount++] = i;
        }
    }
    if (fromCount == 0 || toCount == 0) return false; // nothing to ship right now

    for (int i = 0; i < MAX_TRUCKS; i++) {
        if (trucks[i].active) continue;

        int from = fromCandidates[GetRandomValue(0, fromCount-1)];
        int to = toCandidates[GetRandomValue(0, toCount-1)];

        int sr, sc, er, ec;
        if (!NearestRoad(buildings[from].r, buildings[from].c, &sr, &sc)) return false;
        if (!NearestRoad(buildings[to].r, buildings[to].c, &er, &ec)) return false;

        Truck t = {0};
        if (!BFSPath(sr, sc, er, ec, &t)) return false;
        t.idx = 0;
        t.t = 0;
        t.speed = 0.02f + GetRandomValue(0, 15)/1000.0f;
        t.color = truckColor;
        t.active = true;
        t.toBuildingIdx = to;
        trucks[i] = t;

        // Cargo leaves the source now; it arrives at the destination later.
        buildings[from].supply -= DELIVERY_AMOUNT;
        if (buildings[from].supply < 0) buildings[from].supply = 0;
        return true;
    }
    return false;
}

static const Color FARM_TRUCK_COLOR    = (Color){110, 190, 90, 255};  // green - hauling food
static const Color FACTORY_TRUCK_COLOR = (Color){255, 107, 53, 255}; // orange - hauling goods (original color)

// Nudges a base leg color toward a resource type's color so trucks
// visibly read as "which chain" as well as "which leg" - Grain-chain
// trucks skew toward the base color, Timber-chain trucks are blended
// noticeably toward teal.
static Color TruckColorFor(Color base, ResourceType resType) {
    if (resType == RESOURCE_GRAIN) return base;
    Color tint = RESOURCE_COLORS[RESOURCE_TIMBER];
    Color c;
    c.r = (unsigned char)((base.r + tint.r) / 2);
    c.g = (unsigned char)((base.g + tint.g) / 2);
    c.b = (unsigned char)((base.b + tint.b) / 2);
    c.a = 255;
    return c;
}

// Tries every (leg, resourceType) combination each tick, in random order,
// so no single leg or chain starves the others of truck slots when
// several have work to do. 2 legs x 2 resource types = 4 combos.
static void SpawnTruck(void) {
    typedef struct { TileType from, to; Color color; } Leg;
    Leg legs[2] = {
        { TILE_FARM,    TILE_FACTORY, FARM_TRUCK_COLOR },
        { TILE_FACTORY, TILE_HOUSE,   FACTORY_TRUCK_COLOR },
    };
    int order[4] = {0,1,2,3};
    // Simple Fisher-Yates shuffle over the 4 combo slots.
    for (int i = 3; i > 0; i--) {
        int j = GetRandomValue(0, i);
        int tmp = order[i]; order[i] = order[j]; order[j] = tmp;
    }
    for (int k = 0; k < 4; k++) {
        int legIdx = order[k] / 2;
        ResourceType resType = (ResourceType)(order[k] % 2);
        Leg *leg = &legs[legIdx];
        if (SpawnTruckLeg(leg->from, leg->to, resType, TruckColorFor(leg->color, resType))) return;
    }
}

// True if any police station is within POLICE_COVERAGE_RADIUS (Manhattan
// distance) of the given tile. Straightforward linear scan - buildingCount
// is capped at MAX_BUILDINGS (128), so this is cheap even called once per
// house per frame.
static bool IsCoveredByPolice(int r, int c) {
    for (int i = 0; i < buildingCount; i++) {
        if (buildings[i].type != TILE_POLICE) continue;
        int dist = abs(buildings[i].r - r) + abs(buildings[i].c - c);
        if (dist <= POLICE_COVERAGE_RADIUS) return true;
    }
    return false;
}

static void UpdateBuildings(void) {
    for (int i = 0; i < buildingCount; i++) {
        if (buildings[i].type == TILE_HOUSE) {
            if (buildings[i].demand < 100.0f) {
                buildings[i].demand += DEMAND_GROWTH_PER_FRAME;
                totalDemandGenerated += DEMAND_GROWTH_PER_FRAME;
                if (buildings[i].demand > 100.0f) buildings[i].demand = 100.0f;
            }
            // Flat tax, regardless of how well this house is being served.
            money += TAX_PER_HOUSE_PER_FRAME;

            // Crime rises when a house is outside every police station's
            // coverage radius, and decays (faster than it rises) when
            // it's covered. A house with high enough crime has a small
            // per-frame chance of a theft event draining some money -
            // a concrete reason to actually build police stations rather
            // than just a cosmetic number.
            if (IsCoveredByPolice(buildings[i].r, buildings[i].c)) {
                buildings[i].crime -= CRIME_DECAY_PER_FRAME;
                if (buildings[i].crime < 0.0f) buildings[i].crime = 0.0f;
            } else {
                buildings[i].crime += CRIME_GROWTH_PER_FRAME;
                if (buildings[i].crime > 100.0f) buildings[i].crime = 100.0f;
            }
            if (buildings[i].crime >= THEFT_CRIME_THRESHOLD &&
                GetRandomValue(0, THEFT_CHANCE_PER_FRAME) == 0) {
                money -= THEFT_AMOUNT;
                if (money < 0) money = 0;
            }
        } else if (buildings[i].type == TILE_FARM) {
            if (buildings[i].supply < 100.0f) {
                buildings[i].supply += SUPPLY_GROWTH_PER_FRAME;
                if (buildings[i].supply > 100.0f) buildings[i].supply = 100.0f;
            }
        } else if (buildings[i].type == TILE_FACTORY) {
            // A factory's hunger for food grows on its own; its *goods*
            // supply does not - that only rises when a farm delivery
            // arrives (see DeliverToBuilding).
            if (buildings[i].demand < 100.0f) {
                buildings[i].demand += FOOD_DEMAND_GROWTH_PER_FRAME;
                if (buildings[i].demand > 100.0f) buildings[i].demand = 100.0f;
            }
        }
    }
}

// Unloads one truckload at its destination building. Houses just consume
// it (end of the chain). Factories convert it: the food reduces their
// hunger (demand) AND becomes finished goods (supply) ready for the next
// leg to a house.
static void DeliverToBuilding(int buildingIdx) {
    if (buildingIdx < 0 || buildingIdx >= buildingCount) return;
    Building *b = &buildings[buildingIdx];

    if (b->type == TILE_HOUSE) {
        float served = (b->demand < DELIVERY_AMOUNT) ? b->demand : DELIVERY_AMOUNT;
        b->demand -= served;
        if (b->demand < 0) b->demand = 0;
        totalDemandServed += served;
    } else if (b->type == TILE_FACTORY) {
        float served = (b->demand < DELIVERY_AMOUNT) ? b->demand : DELIVERY_AMOUNT;
        b->demand -= served;
        if (b->demand < 0) b->demand = 0;
        b->supply += served;
        if (b->supply > 100.0f) b->supply = 100.0f;
    }
}

// Converts a road tile's congestion (0..CONGESTION_MAX) into a speed
// multiplier: 1.0 below CONGESTION_SLOWDOWN_THRESHOLD, tapering linearly
// down to (1 - CONGESTION_MAX_SLOWDOWN) at full congestion.
static float CongestionSpeedFactor(int r, int c) {
    float cong = roadCongestion[r][c];
    if (cong <= CONGESTION_SLOWDOWN_THRESHOLD) return 1.0f;
    float span = CONGESTION_MAX - CONGESTION_SLOWDOWN_THRESHOLD;
    float over = cong - CONGESTION_SLOWDOWN_THRESHOLD;
    float pct = (span > 0.0f) ? (over / span) : 1.0f;
    if (pct > 1.0f) pct = 1.0f;
    return 1.0f - pct * CONGESTION_MAX_SLOWDOWN;
}

static void UpdateTrucks(void) {
    // Congestion decays everywhere first, then gets topped back up below by
    // whichever trucks are currently sitting on a given tile - so a tile
    // stays "hot" only as long as trucks keep passing through it.
    for (int r = 0; r < MAP_ROWS; r++) {
        for (int c = 0; c < MAP_COLS; c++) {
            if (roadCongestion[r][c] > 0.0f) {
                roadCongestion[r][c] -= CONGESTION_DECAY_PER_FRAME;
                if (roadCongestion[r][c] < 0.0f) roadCongestion[r][c] = 0.0f;
            }
        }
    }

    for (int i = 0; i < MAX_TRUCKS; i++) {
        if (!trucks[i].active) continue;
        Truck *tr = &trucks[i];
        if (tr->idx >= tr->pathLen - 1) {
            tr->active = false;
            deliveries++;
            DeliverToBuilding(tr->toBuildingIdx);
            continue;
        }

        int curR = tr->pathR[tr->idx], curC = tr->pathC[tr->idx];
        roadCongestion[curR][curC] += CONGESTION_PER_TRUCK_PER_FRAME;
        if (roadCongestion[curR][curC] > CONGESTION_MAX) roadCongestion[curR][curC] = CONGESTION_MAX;

        tr->t += tr->speed * CongestionSpeedFactor(curR, curC);
        if (tr->t >= 1.0f) {
            tr->t = 0;
            tr->idx++;
            if (tr->idx >= tr->pathLen - 1) {
                tr->active = false;
                deliveries++;
                DeliverToBuilding(tr->toBuildingIdx);
            }
        }
    }
    // Keep a handful of trucks running
    int activeCount = 0;
    for (int i = 0; i < MAX_TRUCKS; i++) if (trucks[i].active) activeCount++;
    if (activeCount < 6 && GetRandomValue(0, 20) == 0) SpawnTruck();
}

static void DrawTrucks(void) {
    for (int i = 0; i < MAX_TRUCKS; i++) {
        if (!trucks[i].active) continue;
        Truck *tr = &trucks[i];
        Vector2 a = WorldToScreen(CellCenterWorld(tr->pathR[tr->idx], tr->pathC[tr->idx]));
        Vector2 b = WorldToScreen(CellCenterWorld(tr->pathR[tr->idx+1], tr->pathC[tr->idx+1]));
        float x = a.x + (b.x - a.x) * tr->t;
        float y = a.y + (b.y - a.y) * tr->t;
        // Skip drawing if off-screen (cheap clip)
        float ts = TS();
        if (x < -ts || x > SCREEN_W+ts || y < TOP_BAR-ts || y > SCREEN_H+ts) continue;
        DrawCircle((int)x, (int)y, ts*0.16f, tr->color);
    }
}

// Draws one thin meter bar at a given offset above a building.
static void DrawMeterBar(float x, float yOffset, float w, float h, float pct, Color barColor) {
    DrawRectangle((int)x, (int)yOffset, (int)w, (int)h, (Color){20,20,20,180});
    DrawRectangle((int)x, (int)yOffset, (int)(w*pct), (int)h, barColor);
}

// Small bar(s) over a building showing demand (house), crop growth (farm),
// or - for factories - two bars: food need on top, goods ready below.
static void DrawBuildingMeter(Building *b) {
    Vector2 screenPos = WorldToScreen(CellCenterWorld(b->r, b->c));
    float ts = TS();
    float x = screenPos.x - ts*0.4f;
    float w = ts*0.8f;
    float h = 4.0f;
    if (screenPos.x < -ts || screenPos.x > SCREEN_W+ts) return;
    if (screenPos.y < TOP_BAR-ts || screenPos.y > SCREEN_H+ts) return;

    if (b->type == TILE_FACTORY) {
        float foodPct = b->demand/100.0f;   // how hungry for food (high = urgent)
        float goodsPct = b->supply/100.0f;  // how many goods ready (high = good)
        Color foodColor  = (foodPct > 0.7f) ? (Color){220,60,50,255} : (foodPct > 0.35f) ? (Color){242,193,78,255} : (Color){90,170,90,255};
        Color goodsColor = (goodsPct > 0.5f) ? (Color){90,170,90,255} : (goodsPct > 0.2f) ? (Color){242,193,78,255} : (Color){120,120,120,255};
        DrawMeterBar(x, screenPos.y - ts*0.62f, w, h, foodPct, foodColor);
        DrawMeterBar(x, screenPos.y - ts*0.50f, w, h, goodsPct, goodsColor);
        return;
    }

    float y = screenPos.y - ts*0.55f;
    float pct = (b->type == TILE_HOUSE) ? (b->demand/100.0f) : (b->supply/100.0f);
    Color barColor;
    if (b->type == TILE_HOUSE) {
        // For houses, high demand = bad (unmet need), so red at high.
        barColor = (pct > 0.7f) ? (Color){220,60,50,255} : (pct > 0.35f) ? (Color){242,193,78,255} : (Color){90,170,90,255};
    } else {
        // For farms, high supply = good (crops ready to ship).
        barColor = (pct > 0.5f) ? (Color){90,170,90,255} : (pct > 0.2f) ? (Color){242,193,78,255} : (Color){120,120,120,255};
    }
    DrawMeterBar(x, y, w, h, pct, barColor);

    // Second bar for houses only: crime, purple-ish so it reads as a
    // different signal from the blue/red/green demand bar above it.
    // Only drawn once there's actually some crime, so a well-policed
    // house doesn't show a second empty bar for no reason.
    if (b->type == TILE_HOUSE && b->crime > 0.5f) {
        float crimePct = b->crime / 100.0f;
        DrawMeterBar(x, y - ts*0.12f, w, h, crimePct, (Color){170,70,190,255});
    }
}

// Small colored dot in a building's top-left corner showing which
// resource chain it's on (see RESOURCE_COLORS). Only drawn for the three
// building types that actually participate in a chain.
static void DrawResourceTypeIndicator(Building *b) {
    if (b->type != TILE_FARM && b->type != TILE_FACTORY && b->type != TILE_HOUSE) return;
    Vector2 screenPos = WorldToScreen(CellCenterWorld(b->r, b->c));
    float ts = TS();
    if (screenPos.x < -ts || screenPos.x > SCREEN_W+ts) return;
    if (screenPos.y < TOP_BAR-ts || screenPos.y > SCREEN_H+ts) return;
    float cx = screenPos.x - ts*0.4f + 3.0f;
    float cy = screenPos.y - ts*0.4f + 3.0f;
    DrawCircle((int)cx, (int)cy, 3.5f, RESOURCE_COLORS[b->resourceType]);
    DrawCircleLines((int)cx, (int)cy, 3.5f, (Color){20,20,20,180});
}

static void AddBuilding(int r, int c, TileType type) {
    if (buildingCount >= MAX_BUILDINGS) return;
    buildings[buildingCount].r = r;
    buildings[buildingCount].c = c;
    buildings[buildingCount].type = type;
    buildings[buildingCount].demand = 0.0f;
    // Farms start half-grown, same as factories used to. Factories now
    // start with nothing to ship - they need a farm delivery first.
    buildings[buildingCount].supply = (type == TILE_FARM) ? 50.0f : 0.0f;
    buildings[buildingCount].crime = 0.0f;
    // Random chain assignment for the three building types that actually
    // participate in the economy. Roads/police don't use this field, but
    // it's still given a defined value (RESOURCE_GRAIN) rather than left
    // uninitialized, since Building is written straight to the save file.
    buildings[buildingCount].resourceType =
        (type == TILE_FARM || type == TILE_FACTORY || type == TILE_HOUSE)
            ? (ResourceType)GetRandomValue(0, TOTAL_RESOURCE_TYPES - 1)
            : RESOURCE_GRAIN;
    buildingCount++;
}

static void RemoveBuildingAt(int r, int c) {
    for (int i = 0; i < buildingCount; i++) {
        if (buildings[i].r == r && buildings[i].c == c) {
            buildings[i] = buildings[buildingCount-1];
            buildingCount--;
            return;
        }
    }
}

// Loads a texture for a tile type and warns (without crashing) if it's missing.
static void LoadTileAsset(TileType type, const char *path) {
    Texture2D tex = LoadTexture(path);
    if (tex.id == 0) {
        TraceLog(LOG_WARNING, "PyCity: failed to load '%s' - falling back to flat color for this tile", path);
    }
    gameAssets[type] = tex;
}

// Scans a folder for .png files and loads every one it finds (up to
// maxCount) into outTextures. Returns how many were actually loaded.
// This is what lets assets/april_fools/ "just take random pngs" - drop
// files in, no filename or code changes needed. (Winter uses specific
// named tiles instead, since it's a coherent reskin, not random props.)
static int LoadPngsFromFolder(const char *folder, Texture2D *outTextures, int maxCount) {
    FilePathList files = LoadDirectoryFilesEx(folder, ".png", false);
    int loaded = 0;
    for (unsigned int i = 0; i < files.count && loaded < maxCount; i++) {
        Texture2D tex = LoadTexture(files.paths[i]);
        if (tex.id != 0) {
            outTextures[loaded++] = tex;
        } else {
            TraceLog(LOG_WARNING, "PyCity: failed to load '%s'", files.paths[i]);
        }
    }
    UnloadDirectoryFiles(files);
    if (loaded == 0) {
        TraceLog(LOG_WARNING, "PyCity: no usable .png files found in '%s'", folder);
    }
    return loaded;
}

static void LoadAllAssets(void) {
    gameAssets[TILE_EMPTY] = (Texture2D){ 0 };
    LoadTileAsset(TILE_ROAD,    "assets/base/tile_0025.png");
    LoadTileAsset(TILE_HOUSE,   "assets/base/tile_0100.png");
    LoadTileAsset(TILE_FACTORY, "assets/base/tile_0073.png");
    // Kenney "Tiny Farm" pack also uses the tile_XXXX.png naming
    // convention - drop the pack into assets/farm/.
    LoadTileAsset(TILE_FARM, "assets/farm/tile_0091.png");

    farmHayBaleTexture = LoadTexture("assets/farm/tile_0097.png");
    if (farmHayBaleTexture.id == 0) {
        TraceLog(LOG_WARNING, "PyCity: failed to load 'assets/farm/tile_0097.png' - falling back to a colored shape for the haybale indicator");
    }

    aprilFoolsTextureCount = LoadPngsFromFolder("assets/april_fools", aprilFoolsTextures, MAX_APRIL_FOOLS_TEXTURES);
    // Kenney "Tiny Ski" pack - winter grass variants (same 3-slot scheme
    // as the base grass) plus a dedicated winter road/street tile.
    const char *winterGrassPaths[TOTAL_GRASS_VARIANTS] = {
        "assets/winter/tile_0000.png",
        "assets/winter/tile_0004.png",
        "assets/winter/tile_0005.png"
    };
    for (int i = 0; i < TOTAL_GRASS_VARIANTS; i++) {
        Texture2D tex = LoadTexture(winterGrassPaths[i]);
        if (tex.id == 0) {
            TraceLog(LOG_WARNING, "PyCity: failed to load '%s' - falling back to the normal base grass for this variant in winter", winterGrassPaths[i]);
        }
        winterGrassTextures[i] = tex;
    }
    winterRoadTexture = LoadTexture("assets/winter/tile_0028.png");
    if (winterRoadTexture.id == 0) {
        TraceLog(LOG_WARNING, "PyCity: failed to load 'assets/winter/tile_0028.png' - falling back to the normal base road in winter");
    }

    const char *grassPaths[TOTAL_GRASS_VARIANTS] = {
        "assets/base/tile_0000.png",
        "assets/base/tile_0001.png",
        "assets/base/tile_0002.png"
    };
    for (int i = 0; i < TOTAL_GRASS_VARIANTS; i++) {
        Texture2D tex = LoadTexture(grassPaths[i]);
        if (tex.id == 0) {
            TraceLog(LOG_WARNING, "PyCity: failed to load '%s' - falling back to flat color for grass", grassPaths[i]);
        }
        grassTextures[i] = tex;
    }
}

// Picks a variant per empty tile once, so the ground doesn't flicker between
// grass textures every frame. Call this once at startup (and again after a
// load, in case the loaded map has different empty tiles than before).
static void RandomizeGrassVariants(void) {
    for (int r = 0; r < MAP_ROWS; r++) {
        for (int c = 0; c < MAP_COLS; c++) {
            grassVariant[r][c] = GetRandomValue(0, TOTAL_GRASS_VARIANTS-1);
        }
    }
}

static void UnloadAllAssets(void) {
    for (int i = 0; i < TOTAL_TILE_TYPES; i++) {
        if (gameAssets[i].id != 0) UnloadTexture(gameAssets[i]);
    }
    for (int i = 0; i < TOTAL_GRASS_VARIANTS; i++) {
        if (grassTextures[i].id != 0) UnloadTexture(grassTextures[i]);
    }
    for (int i = 0; i < aprilFoolsTextureCount; i++) {
        if (aprilFoolsTextures[i].id != 0) UnloadTexture(aprilFoolsTextures[i]);
    }
    for (int i = 0; i < TOTAL_GRASS_VARIANTS; i++) {
        if (winterGrassTextures[i].id != 0) UnloadTexture(winterGrassTextures[i]);
    }
    if (winterRoadTexture.id != 0) UnloadTexture(winterRoadTexture);
    if (farmHayBaleTexture.id != 0) UnloadTexture(farmHayBaleTexture);
}

// Draws one tile: the loaded texture if it exists, otherwise a flat color fallback.
// r, c are only used to look up which grass variant an empty tile should use.
// `size` is the on-screen pixel size of one tile at the current zoom
// (see TS()) - passed in rather than read globally so this stays a pure
// function of its arguments.
static void DrawTile(TileType type, int r, int c, int x, int y, float size) {
    Color fallback = (Color){30,38,35,255}; // grass / empty
    switch (type) {
        case TILE_ROAD:    fallback = (Color){58,67,64,255};   break;
        case TILE_HOUSE:   fallback = (Color){76,110,156,255}; break;
        case TILE_FACTORY: fallback = (Color){255,107,53,255}; break;
        case TILE_FARM:    fallback = (Color){196,164,60,255}; break; // wheat gold
        case TILE_POLICE:  fallback = (Color){60,70,140,255};  break; // dark blue - no art asset yet, flat color only
        default: break;
    }

    Texture2D tex;
    if (type == TILE_EMPTY) {
        Texture2D winterTex = winterGrassTextures[grassVariant[r][c]];
        tex = (winterActive && winterTex.id != 0) ? winterTex : grassTextures[grassVariant[r][c]];
    } else if (type == TILE_ROAD && winterActive && winterRoadTexture.id != 0) {
        tex = winterRoadTexture;
    } else {
        tex = gameAssets[type];
    }

    if (tex.id != 0) {
        Rectangle src = { 0, 0, (float)tex.width, (float)tex.height };
        Rectangle dst = { (float)x, (float)y, size, size };
        DrawTexturePro(tex, src, dst, (Vector2){0,0}, 0.0f, WHITE);
    } else {
        float gap = (size > 2.0f) ? 1.0f : 0.0f; // skip the 1px grid gap once tiles get tiny
        DrawRectangle(x, y, (int)(size-gap), (int)(size-gap), fallback);
    }
}

// Tints a road tile red, proportional to its current congestion, so busy
// choke points are visible at a glance. Drawn as a translucent overlay on
// top of the tile's normal texture/fallback rather than replacing it.
static void DrawCongestionOverlay(int r, int c, int x, int y, float size) {
    if (grid[r][c] != TILE_ROAD) return;
    float cong = roadCongestion[r][c];
    if (cong <= CONGESTION_SLOWDOWN_THRESHOLD) return;
    float pct = (cong - CONGESTION_SLOWDOWN_THRESHOLD) / (CONGESTION_MAX - CONGESTION_SLOWDOWN_THRESHOLD);
    if (pct > 1.0f) pct = 1.0f;
    unsigned char alpha = (unsigned char)(pct * 140);
    DrawRectangle(x, y, (int)size, (int)size, (Color){200, 40, 30, alpha});
}

// Checks the REAL-WORLD system date, not anything in-game. Only ever
// true on April 1st.
static bool IsAprilFoolsToday(void) {
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    if (!lt) return false;
    return (lt->tm_mon == 3 && lt->tm_mday == 1); // tm_mon is 0-indexed: April = 3
}

// True during meteorological winter (Dec/Jan/Feb), real-world date.
static bool IsWinterToday(void) {
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    if (!lt) return false;
    return (lt->tm_mon == 11 || lt->tm_mon == 0 || lt->tm_mon == 1);
}

static void ResetAprilFoolsItems(void) {
    for (int r = 0; r < MAP_ROWS; r++)
        for (int c = 0; c < MAP_COLS; c++)
            aprilFoolsItem[r][c] = -1;
}

// Drops one silly prop on a random empty tile. Called sparingly (see the
// APRIL_FOOLS_SPAWN_CHANCE roll in main's update loop) so the map doesn't
// get flooded with them.
static void SpawnAprilFoolsItem(void) {
    if (aprilFoolsTextureCount == 0) return; // nothing was loadable from assets/april_fools/
    int emptyR[MAP_ROWS*MAP_COLS], emptyC[MAP_ROWS*MAP_COLS];
    int emptyCount = 0;
    for (int r = 0; r < MAP_ROWS; r++) {
        for (int c = 0; c < MAP_COLS; c++) {
            if (grid[r][c] == TILE_EMPTY && aprilFoolsItem[r][c] == -1) {
                emptyR[emptyCount] = r; emptyC[emptyCount] = c; emptyCount++;
            }
        }
    }
    if (emptyCount == 0) return;
    int pick = GetRandomValue(0, emptyCount-1);
    aprilFoolsItem[emptyR[pick]][emptyC[pick]] = GetRandomValue(0, aprilFoolsTextureCount-1);
}

// Draws whatever silly props have landed on empty tiles so far today.
// Only ever called when IsAprilFoolsToday() is true.
static void DrawAprilFoolsItems(void) {
    int startR, endR, startC, endC;
    GetVisibleTileRange(&startR, &endR, &startC, &endC);
    float size = TS();
    for (int rr = startR; rr < endR; rr++) {
        for (int cc = startC; cc < endC; cc++) {
            if (grid[rr][cc] != TILE_EMPTY) continue;
            int item = aprilFoolsItem[rr][cc];
            if (item < 0) continue;

            Vector2 screenPos = WorldToScreen((Vector2){ (float)(cc*TILE), (float)(rr*TILE) });
            int x = (int)screenPos.x;
            int y = (int)screenPos.y;
            Texture2D tex = aprilFoolsTextures[item];
            if (tex.id != 0) {
                Rectangle src = { 0, 0, (float)tex.width, (float)tex.height };
                Rectangle dst = { (float)x, (float)y, size, size };
                DrawTexturePro(tex, src, dst, (Vector2){0,0}, 0.0f, WHITE);
            } else {
                DrawCircle(x + (int)(size/2), y + (int)(size/2), size*0.3f, aprilFoolsFallbackColor);
            }
        }
    }
}

// Overlays a small haybale icon in the corner of a farm tile once it has
// enough supply stacked up to actually send a truck. Purely a visual cue -
// the meter bar above already shows the exact number.
static void DrawFarmReadyIndicator(Building *b) {
    if (b->type != TILE_FARM) return;
    if (b->supply < MIN_SUPPLY_TO_PICKUP) return;

    Vector2 screenPos = WorldToScreen(CellCenterWorld(b->r, b->c));
    float ts = TS();
    if (screenPos.x < -ts || screenPos.x > SCREEN_W+ts) return;
    if (screenPos.y < TOP_BAR-ts || screenPos.y > SCREEN_H+ts) return;

    float size = ts * 0.45f;
    float x = screenPos.x + ts*0.5f - size;
    float y = screenPos.y + ts*0.5f - size;

    if (farmHayBaleTexture.id != 0) {
        Rectangle src = { 0, 0, (float)farmHayBaleTexture.width, (float)farmHayBaleTexture.height };
        Rectangle dst = { x, y, size, size };
        DrawTexturePro(farmHayBaleTexture, src, dst, (Vector2){0,0}, 0.0f, WHITE);
    } else {
        DrawRectangle((int)x, (int)y, (int)size, (int)size, (Color){196,164,60,255});
    }
}

static void ClampCamera(void) {
    float viewW = SCREEN_W / camZoom;
    float viewH = (SCREEN_H - TOP_BAR) / camZoom;
    float mapW = MAP_COLS * TILE;
    float mapH = MAP_ROWS * TILE;
    // If zoomed out far enough that the whole map fits on screen in one
    // axis, center it on that axis instead of pinning to a corner.
    if (viewW >= mapW) camPxC = (mapW - viewW) / 2.0f;
    else {
        if (camPxC < 0) camPxC = 0;
        if (camPxC > mapW - viewW) camPxC = mapW - viewW;
    }
    if (viewH >= mapH) camPxR = (mapH - viewH) / 2.0f;
    else {
        if (camPxR < 0) camPxR = 0;
        if (camPxR > mapH - viewH) camPxR = mapH - viewH;
    }
}

// Changes zoom by `delta` (clamped to [ZOOM_MIN, ZOOM_MAX]) while keeping
// the world point currently at screen-center fixed in place, so zooming
// in/out doesn't yank the view sideways. Caller should follow with
// ClampCamera() in case the new zoom level changes the valid pan range.
static void ApplyZoom(float delta) {
    float oldZoom = camZoom;
    float newZoom = camZoom + delta;
    if (newZoom < ZOOM_MIN) newZoom = ZOOM_MIN;
    if (newZoom > ZOOM_MAX) newZoom = ZOOM_MAX;
    if (newZoom == oldZoom) return;

    float halfW = SCREEN_W / 2.0f;
    float halfH = (SCREEN_H - TOP_BAR) / 2.0f;
    float worldCenterX = camPxC + halfW / oldZoom;
    float worldCenterY = camPxR + halfH / oldZoom;
    camZoom = newZoom;
    camPxC = worldCenterX - halfW / camZoom;
    camPxR = worldCenterY - halfH / camZoom;
}

// ---- save / load ----
// Simple binary dump: magic number, grid, building count, buildings array,
// economy totals. Good enough for an alpha - not meant to be a stable
// cross-version save format yet.
static void SaveGame(void) {
    FILE *f = fopen(SAVE_FILE, "wb");
    if (!f) { TraceLog(LOG_WARNING, "PyCity: could not open %s for saving", SAVE_FILE); return; }

    int magic = SAVE_MAGIC;
    fwrite(&magic, sizeof(int), 1, f);
    fwrite(grid, sizeof(TileType), MAP_ROWS*MAP_COLS, f);
    fwrite(&buildingCount, sizeof(int), 1, f);
    fwrite(buildings, sizeof(Building), buildingCount, f);
    fwrite(&deliveries, sizeof(int), 1, f);
    fwrite(&totalDemandGenerated, sizeof(double), 1, f);
    fwrite(&totalDemandServed, sizeof(double), 1, f);
    fwrite(&money, sizeof(double), 1, f);

    fclose(f);
    TraceLog(LOG_INFO, "PyCity: game saved to %s", SAVE_FILE);
}

static bool LoadGame(void) {
    FILE *f = fopen(SAVE_FILE, "rb");
    if (!f) { TraceLog(LOG_WARNING, "PyCity: no save file found at %s", SAVE_FILE); return false; }

    int magic = 0;
    fread(&magic, sizeof(int), 1, f);
    if (magic != SAVE_MAGIC) {
        TraceLog(LOG_WARNING, "PyCity: save file is corrupt or from an incompatible version");
        fclose(f);
        return false;
    }
    fread(grid, sizeof(TileType), MAP_ROWS*MAP_COLS, f);
    fread(&buildingCount, sizeof(int), 1, f);
    if (buildingCount > MAX_BUILDINGS) buildingCount = MAX_BUILDINGS;
    fread(buildings, sizeof(Building), buildingCount, f);
    fread(&deliveries, sizeof(int), 1, f);
    fread(&totalDemandGenerated, sizeof(double), 1, f);
    fread(&totalDemandServed, sizeof(double), 1, f);

    // Money didn't exist in older saves. Read into a scratch variable and
    // only accept it if a full value was actually present, so an old,
    // shorter save file can't leave `money` partially overwritten with
    // garbage bytes.
    double loadedMoney;
    money = (fread(&loadedMoney, sizeof(double), 1, f) == 1) ? loadedMoney : STARTING_MONEY;

    fclose(f);

    // Clear any in-flight trucks - their paths reference the old world state.
    memset(trucks, 0, sizeof(trucks));
    // Congestion is transient traffic, not world state - start fresh.
    memset(roadCongestion, 0, sizeof(roadCongestion));
    // April Fools props are cosmetic-only and aren't part of the save
    // format, so just clear them out rather than leaving stale ones around.
    ResetAprilFoolsItems();

    TraceLog(LOG_INFO, "PyCity: game loaded from %s", SAVE_FILE);
    return true;
}

int main(void) {
    InitWindow(SCREEN_W, SCREEN_H, "PyCity - alpha");
    SetTargetFPS(60);

    // Textures need a GPU context, so this has to happen after InitWindow().
    LoadAllAssets();
    RandomizeGrassVariants();

    memset(grid, TILE_EMPTY, sizeof(grid));
    memset(trucks, 0, sizeof(trucks));
    memset(roadCongestion, 0, sizeof(roadCongestion));
    ResetAprilFoolsItems();

    Tool tool = TOOL_ROAD;
    bool paused = false;

    while (!WindowShouldClose()) {
        // ---- input ----
        if (IsKeyPressed(KEY_ONE))   tool = TOOL_ROAD;
        if (IsKeyPressed(KEY_TWO))   tool = TOOL_HOUSE;
        if (IsKeyPressed(KEY_THREE)) tool = TOOL_FACTORY;
        if (IsKeyPressed(KEY_FOUR))  tool = TOOL_FARM;
        if (IsKeyPressed(KEY_FIVE))  tool = TOOL_POLICE;
        if (IsKeyPressed(KEY_SIX))   tool = TOOL_BULLDOZE;
        if (IsKeyPressed(KEY_SPACE)) paused = !paused;
        if (IsKeyPressed(KEY_F5)) SaveGame();
        if (IsKeyPressed(KEY_F9)) LoadGame();

        // Camera pan - arrow keys or WASD, frame-rate independent-ish.
        // Pan speed is in constant world pixels/sec regardless of zoom, so
        // panning covers the same amount of "city" per second whether
        // zoomed in or out (it'll just look faster on screen when zoomed
        // out, same as any top-down city builder).
        float dt = GetFrameTime();
        float panPx = CAM_PAN_TILES_PER_SEC * TILE * dt;
        if (IsKeyDown(KEY_LEFT)  || IsKeyDown(KEY_A)) camPxC -= panPx;
        if (IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D)) camPxC += panPx;
        if (IsKeyDown(KEY_UP)    || IsKeyDown(KEY_W)) camPxR -= panPx;
        if (IsKeyDown(KEY_DOWN)  || IsKeyDown(KEY_S)) camPxR += panPx;

        // Zoom - mouse wheel (one step per notch) or +/- keys as a
        // keyboard-only alternative. Both go through ApplyZoom() so they
        // share the same "keep screen-center fixed" behavior.
        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) ApplyZoom(wheel * ZOOM_STEP);
        if (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD))      ApplyZoom(ZOOM_STEP);
        if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT)) ApplyZoom(-ZOOM_STEP);

        ClampCamera();

        Vector2 mouse = GetMousePosition();
        Vector2 mouseWorld = ScreenToWorld(mouse);
        int c = (int)floorf(mouseWorld.x / TILE);
        int r = (int)floorf(mouseWorld.y / TILE);
        bool hovering = InBounds(r,c) && mouse.y >= TOP_BAR;

        if (hovering && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            switch (tool) {
                case TOOL_ROAD:
                    if (grid[r][c] == TILE_EMPTY && money >= COST_ROAD) {
                        grid[r][c] = TILE_ROAD;
                        money -= COST_ROAD;
                    }
                    break;
                case TOOL_HOUSE:
                    if (grid[r][c] == TILE_EMPTY && money >= COST_HOUSE) {
                        grid[r][c] = TILE_HOUSE;
                        AddBuilding(r,c,TILE_HOUSE);
                        money -= COST_HOUSE;
                    }
                    break;
                case TOOL_FACTORY:
                    if (grid[r][c] == TILE_EMPTY && money >= COST_FACTORY) {
                        grid[r][c] = TILE_FACTORY;
                        AddBuilding(r,c,TILE_FACTORY);
                        money -= COST_FACTORY;
                    }
                    break;
                case TOOL_FARM:
                    if (grid[r][c] == TILE_EMPTY && money >= COST_FARM) {
                        grid[r][c] = TILE_FARM;
                        AddBuilding(r,c,TILE_FARM);
                        money -= COST_FARM;
                    }
                    break;
                case TOOL_POLICE:
                    if (grid[r][c] == TILE_EMPTY && money >= COST_POLICE) {
                        grid[r][c] = TILE_POLICE;
                        AddBuilding(r,c,TILE_POLICE);
                        money -= COST_POLICE;
                    }
                    break;
                case TOOL_BULLDOZE:
                    if (grid[r][c] != TILE_EMPTY) {
                        money += GetBuildCost(grid[r][c]) * BULLDOZE_REFUND_PERCENT;
                        if (grid[r][c] == TILE_HOUSE || grid[r][c] == TILE_FACTORY || grid[r][c] == TILE_FARM || grid[r][c] == TILE_POLICE) RemoveBuildingAt(r,c);
                        grid[r][c] = TILE_EMPTY;
                    }
                    break;
            }
        }

        // ---- update ----
        bool aprilFools = IsAprilFoolsToday();
        winterActive = IsWinterToday();
        if (!paused) {
            UpdateBuildings();
            UpdateTrucks();
            if (aprilFools && GetRandomValue(0, APRIL_FOOLS_SPAWN_CHANCE) == 0) {
                SpawnAprilFoolsItem();
            }
        }

        // ---- draw ----
        BeginDrawing();
        ClearBackground((Color){21,26,24,255});

        // grid (only the visible slice, offset by camera and scaled by zoom)
        {
            int startR, endR, startC, endC;
            GetVisibleTileRange(&startR, &endR, &startC, &endC);
            float tileSize = TS();
            for (int rr = startR; rr < endR; rr++) {
                for (int cc = startC; cc < endC; cc++) {
                    Vector2 screenPos = WorldToScreen((Vector2){ (float)(cc*TILE), (float)(rr*TILE) });
                    int x = (int)screenPos.x;
                    int y = (int)screenPos.y;
                    DrawTile(grid[rr][cc], rr, cc, x, y, tileSize);
                    DrawCongestionOverlay(rr, cc, x, y, tileSize);
                }
            }
        }

        if (aprilFools) DrawAprilFoolsItems();

        // building meters (demand/supply bars) and farm ready-to-ship icons
        for (int i = 0; i < buildingCount; i++) {
            DrawBuildingMeter(&buildings[i]);
            DrawFarmReadyIndicator(&buildings[i]);
            DrawResourceTypeIndicator(&buildings[i]);
        }

        // hover highlight
        if (hovering) {
            Vector2 hoverScreen = WorldToScreen((Vector2){ (float)(c*TILE), (float)(r*TILE) });
            float ts = TS();
            DrawRectangleLines((int)hoverScreen.x, (int)hoverScreen.y, (int)ts, (int)ts, (Color){242,193,78,255});
        }

        DrawTrucks();

        // toolbar (drawn last so it sits on top of the map)
        DrawRectangle(0, 0, SCREEN_W, TOP_BAR, (Color){28,35,33,255});
        const char *toolNames[6] = {
            "ROAD (1) $10", 
            "HOUSE (2) $50",
            "FACTORY (3) $150",
            "FARM (4) $100", 
            "POLICE (5) $200",
            "BULLDOZE (6)"
        };
        DrawText(TextFormat("Tool: %s", toolNames[tool]), 10, 8, 18, (Color){255,107,53,255});

        double demandMetPct = (totalDemandGenerated > 0.001) ? (totalDemandServed/totalDemandGenerated*100.0) : 100.0;
        Color moneyColor = (money < COST_ROAD) ? (Color){220,60,50,255} : (Color){237,232,222,255};
        DrawText(TextFormat("Money: $%.0f", money), 10, 52, 18, moneyColor);
        DrawText(TextFormat("Deliveries: %d   Buildings: %d   Demand met: %.0f%%   %s",
                  deliveries, buildingCount, demandMetPct, paused ? "[PAUSED]" : ""),
                  10, 32, 16, (Color){237,232,222,255});

        if (aprilFools) {
            DrawText("Happy April Fools!", SCREEN_W-260, 8, 16, (Color){255,210,60,255});
        } else if (winterActive) {
            DrawText("Winter", SCREEN_W-260, 8, 16, (Color){200,220,255,255});
        }
        DrawText("F5 save   F9 load   Arrows/WASD pan   Wheel/+- zoom", SCREEN_W-350, 20, 14, (Color){160,160,150,255});

        // Legend for the little corner dots on farms/factories/houses -
        // otherwise a gold vs teal dot means nothing to a new player.
        DrawCircle(SCREEN_W-346, 40, 4, RESOURCE_COLORS[RESOURCE_GRAIN]);
        DrawText(RESOURCE_NAMES[RESOURCE_GRAIN], SCREEN_W-338, 35, 12, (Color){160,160,150,255});
        DrawCircle(SCREEN_W-346, 54, 4, RESOURCE_COLORS[RESOURCE_TIMBER]);
        DrawText(RESOURCE_NAMES[RESOURCE_TIMBER], SCREEN_W-338, 49, 12, (Color){160,160,150,255});

        EndDrawing();
    }

    UnloadAllAssets();
    CloseWindow();
    return 0;
}