// PyCity - alpha
// A top-down tile-grid city sim: place roads and buildings, trucks path
// between buildings automatically along the road network, driven by a
// simple supply/demand economy.
//
// Controls:
//   1 = Road tool     2 = House tool     3 = Factory tool   4 = Farm tool   5 = Bulldoze
//   Left click        = place/remove on hovered tile
//   Arrows / WASD     = pan camera
//   Space             = pause/unpause simulation
//   F5                = save     F9 = load
//   Esc               = quit
//
// Money: building costs money (road $10, house $50, factory $150, farm
// $100), houses pay a flat tax over time regardless of how well they're
// served, and there's no debt - insufficient funds just blocks placement.
// Bulldozing refunds 50% of a tile's build cost.
//
// Economy chain: farms grow food (supply) -> trucks haul it to factories that
// need it (demand) -> factories turn it into goods (supply) -> trucks haul
// THOSE to houses that need them (demand). Two truck routes, not one.
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
#define SAVE_MAGIC 0x50434954 // "PCIT"

// ---- Money tuning ----
#define STARTING_MONEY 500.0
#define COST_ROAD     10.0
#define COST_HOUSE    50.0
#define COST_FACTORY 150.0
#define COST_FARM    100.0
#define TAX_PER_HOUSE_PER_FRAME 0.02 // flat per house, regardless of how well it's served
#define BULLDOZE_REFUND_PERCENT 0.5  // partial refund only, so build-then-bulldoze isn't free money

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

typedef enum { TILE_EMPTY = 0, TILE_ROAD, TILE_HOUSE, TILE_FACTORY, TILE_FARM, TOTAL_TILE_TYPES } TileType;
// TILE_FARM is appended after TILE_FACTORY (not inserted earlier) so the
// integer value of every existing tile type is unchanged - old savegame.dat
// files still load correctly.
typedef enum { TOOL_ROAD = 0, TOOL_HOUSE, TOOL_FACTORY, TOOL_FARM, TOOL_BULLDOZE } Tool;

// Cost to place one tile of a given type. Also used to compute the
// bulldoze refund (see BULLDOZE_REFUND_PERCENT).
static double GetBuildCost(TileType type) {
    switch (type) {
        case TILE_ROAD:    return COST_ROAD;
        case TILE_HOUSE:   return COST_HOUSE;
        case TILE_FACTORY: return COST_FACTORY;
        case TILE_FARM:    return COST_FARM;
        default:           return 0.0;
    }
}

#define TOTAL_GRASS_VARIANTS 3

static TileType grid[MAP_ROWS][MAP_COLS];

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

// Camera: top-left tile currently shown on screen.
static int camR = 0, camC = 0;

// ---- helpers ----

static Vector2 CellCenterWorld(int r, int c) {
    // Position in "world" pixel space (before camera offset is applied)
    Vector2 v = { c*TILE + TILE/2.0f, r*TILE + TILE/2.0f };
    return v;
}

static Vector2 WorldToScreen(Vector2 world) {
    Vector2 s = { world.x - camC*TILE, world.y - camR*TILE + TOP_BAR };
    return s;
}

static bool InBounds(int r, int c) {
    return r >= 0 && r < MAP_ROWS && c >= 0 && c < MAP_COLS;
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
// just grabbing two random buildings. Returns true if a truck was spawned.
static bool SpawnTruckLeg(TileType fromType, TileType toType, Color truckColor) {
    int fromCandidates[MAX_BUILDINGS];
    int toCandidates[MAX_BUILDINGS];
    int fromCount = 0, toCount = 0;

    for (int i = 0; i < buildingCount; i++) {
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

// Tries both legs of the chain each tick, in random order, so neither one
// starves the other of truck slots when both have work to do.
static void SpawnTruck(void) {
    if (GetRandomValue(0, 1) == 0) {
        if (SpawnTruckLeg(TILE_FARM, TILE_FACTORY, FARM_TRUCK_COLOR)) return;
        SpawnTruckLeg(TILE_FACTORY, TILE_HOUSE, FACTORY_TRUCK_COLOR);
    } else {
        if (SpawnTruckLeg(TILE_FACTORY, TILE_HOUSE, FACTORY_TRUCK_COLOR)) return;
        SpawnTruckLeg(TILE_FARM, TILE_FACTORY, FARM_TRUCK_COLOR);
    }
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

static void UpdateTrucks(void) {
    for (int i = 0; i < MAX_TRUCKS; i++) {
        if (!trucks[i].active) continue;
        Truck *tr = &trucks[i];
        if (tr->idx >= tr->pathLen - 1) {
            tr->active = false;
            deliveries++;
            DeliverToBuilding(tr->toBuildingIdx);
            continue;
        }
        tr->t += tr->speed;
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
        if (x < -TILE || x > SCREEN_W+TILE || y < TOP_BAR-TILE || y > SCREEN_H+TILE) continue;
        DrawCircle((int)x, (int)y, TILE*0.16f, tr->color);
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
    float x = screenPos.x - TILE*0.4f;
    float w = TILE*0.8f;
    float h = 4.0f;
    if (screenPos.x < -TILE || screenPos.x > SCREEN_W+TILE) return;
    if (screenPos.y < TOP_BAR-TILE || screenPos.y > SCREEN_H+TILE) return;

    if (b->type == TILE_FACTORY) {
        float foodPct = b->demand/100.0f;   // how hungry for food (high = urgent)
        float goodsPct = b->supply/100.0f;  // how many goods ready (high = good)
        Color foodColor  = (foodPct > 0.7f) ? (Color){220,60,50,255} : (foodPct > 0.35f) ? (Color){242,193,78,255} : (Color){90,170,90,255};
        Color goodsColor = (goodsPct > 0.5f) ? (Color){90,170,90,255} : (goodsPct > 0.2f) ? (Color){242,193,78,255} : (Color){120,120,120,255};
        DrawMeterBar(x, screenPos.y - TILE*0.62f, w, h, foodPct, foodColor);
        DrawMeterBar(x, screenPos.y - TILE*0.50f, w, h, goodsPct, goodsColor);
        return;
    }

    float y = screenPos.y - TILE*0.55f;
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
static void DrawTile(TileType type, int r, int c, int x, int y) {
    Color fallback = (Color){30,38,35,255}; // grass / empty
    switch (type) {
        case TILE_ROAD:    fallback = (Color){58,67,64,255};   break;
        case TILE_HOUSE:   fallback = (Color){76,110,156,255}; break;
        case TILE_FACTORY: fallback = (Color){255,107,53,255}; break;
        case TILE_FARM:    fallback = (Color){196,164,60,255}; break; // wheat gold
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
        Rectangle dst = { (float)x, (float)y, (float)TILE, (float)TILE };
        DrawTexturePro(tex, src, dst, (Vector2){0,0}, 0.0f, WHITE);
    } else {
        DrawRectangle(x, y, TILE-1, TILE-1, fallback);
    }
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
    for (int rr = camR; rr < camR + VIEW_ROWS && rr < MAP_ROWS; rr++) {
        for (int cc = camC; cc < camC + VIEW_COLS && cc < MAP_COLS; cc++) {
            if (grid[rr][cc] != TILE_EMPTY) continue;
            int item = aprilFoolsItem[rr][cc];
            if (item < 0) continue;

            int x = (cc - camC)*TILE;
            int y = TOP_BAR + (rr - camR)*TILE;
            Texture2D tex = aprilFoolsTextures[item];
            if (tex.id != 0) {
                Rectangle src = { 0, 0, (float)tex.width, (float)tex.height };
                Rectangle dst = { (float)x, (float)y, (float)TILE, (float)TILE };
                DrawTexturePro(tex, src, dst, (Vector2){0,0}, 0.0f, WHITE);
            } else {
                DrawCircle(x + TILE/2, y + TILE/2, TILE*0.3f, aprilFoolsFallbackColor);
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
    if (screenPos.x < -TILE || screenPos.x > SCREEN_W+TILE) return;
    if (screenPos.y < TOP_BAR-TILE || screenPos.y > SCREEN_H+TILE) return;

    float size = TILE * 0.45f;
    float x = screenPos.x + TILE*0.5f - size;
    float y = screenPos.y + TILE*0.5f - size;

    if (farmHayBaleTexture.id != 0) {
        Rectangle src = { 0, 0, (float)farmHayBaleTexture.width, (float)farmHayBaleTexture.height };
        Rectangle dst = { x, y, size, size };
        DrawTexturePro(farmHayBaleTexture, src, dst, (Vector2){0,0}, 0.0f, WHITE);
    } else {
        DrawRectangle((int)x, (int)y, (int)size, (int)size, (Color){196,164,60,255});
    }
}

static void ClampCamera(void) {
    if (camC < 0) camC = 0;
    if (camR < 0) camR = 0;
    if (camC > MAP_COLS - VIEW_COLS) camC = MAP_COLS - VIEW_COLS;
    if (camR > MAP_ROWS - VIEW_ROWS) camR = MAP_ROWS - VIEW_ROWS;
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
    ResetAprilFoolsItems();

    Tool tool = TOOL_ROAD;
    bool paused = false;
    const float camSpeed = 12.0f; // tiles-ish per second, scaled below

    while (!WindowShouldClose()) {
        // ---- input ----
        if (IsKeyPressed(KEY_ONE))   tool = TOOL_ROAD;
        if (IsKeyPressed(KEY_TWO))   tool = TOOL_HOUSE;
        if (IsKeyPressed(KEY_THREE)) tool = TOOL_FACTORY;
        if (IsKeyPressed(KEY_FOUR))  tool = TOOL_FARM;
        if (IsKeyPressed(KEY_FIVE)) tool = TOOL_BULLDOZE;
        if (IsKeyPressed(KEY_SPACE)) paused = !paused;
        if (IsKeyPressed(KEY_F5)) SaveGame();
        if (IsKeyPressed(KEY_F9)) LoadGame();

        // Camera pan - arrow keys or WASD, frame-rate independent-ish
        float dt = GetFrameTime();
        float move = camSpeed * dt * TILE * 0.2f;
        if (IsKeyDown(KEY_LEFT)  || IsKeyDown(KEY_A)) camC -= (int)fmaxf(1.0f, move);
        if (IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D)) camC += (int)fmaxf(1.0f, move);
        if (IsKeyDown(KEY_UP)    || IsKeyDown(KEY_W)) camR -= (int)fmaxf(1.0f, move);
        if (IsKeyDown(KEY_DOWN)  || IsKeyDown(KEY_S)) camR += (int)fmaxf(1.0f, move);
        ClampCamera();

        Vector2 mouse = GetMousePosition();
        int c = camC + (int)(mouse.x / TILE);
        int r = camR + (int)((mouse.y - TOP_BAR) / TILE);
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
                case TOOL_BULLDOZE:
                    if (grid[r][c] != TILE_EMPTY) {
                        money += GetBuildCost(grid[r][c]) * BULLDOZE_REFUND_PERCENT;
                        if (grid[r][c] == TILE_HOUSE || grid[r][c] == TILE_FACTORY || grid[r][c] == TILE_FARM) RemoveBuildingAt(r,c);
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

        // grid (only the visible slice, offset by camera)
        for (int rr = camR; rr < camR + VIEW_ROWS && rr < MAP_ROWS; rr++) {
            for (int cc = camC; cc < camC + VIEW_COLS && cc < MAP_COLS; cc++) {
                int x = (cc - camC)*TILE;
                int y = TOP_BAR + (rr - camR)*TILE;
                DrawTile(grid[rr][cc], rr, cc, x, y);
            }
        }

        if (aprilFools) DrawAprilFoolsItems();

        // building meters (demand/supply bars) and farm ready-to-ship icons
        for (int i = 0; i < buildingCount; i++) {
            DrawBuildingMeter(&buildings[i]);
            DrawFarmReadyIndicator(&buildings[i]);
        }

        // hover highlight
        if (hovering) {
            DrawRectangleLines((c-camC)*TILE, TOP_BAR + (r-camR)*TILE, TILE, TILE, (Color){242,193,78,255});
        }

        DrawTrucks();

        // toolbar (drawn last so it sits on top of the map)
        DrawRectangle(0, 0, SCREEN_W, TOP_BAR, (Color){28,35,33,255});
        const char *toolNames[5] = {
            TextFormat("ROAD (1) $%.0f", COST_ROAD),
            TextFormat("HOUSE (2) $%.0f", COST_HOUSE),
            TextFormat("FACTORY (3) $%.0f", COST_FACTORY),
            TextFormat("FARM (4) $%.0f", COST_FARM),
            "BULLDOZE (5)"
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
        DrawText("F5 save   F9 load   Arrows/WASD pan", SCREEN_W-260, 20, 14, (Color){160,160,150,255});

        EndDrawing();
    }

    UnloadAllAssets();
    CloseWindow();
    return 0;
}