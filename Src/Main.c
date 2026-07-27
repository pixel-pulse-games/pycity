// PyCity - alpha
// A top-down tile-grid city sim: place roads and buildings, trucks path
// between buildings automatically along the road network, driven by a
// simple supply/demand economy.
//
// Controls:
//   1 = Road tool     2 = House tool     3 = Factory tool     4 = Bulldoze
//   Left click        = place/remove on hovered tile
//   Arrows / WASD     = pan camera
//   Space             = pause/unpause simulation
//   F5                = save     F9 = load
//   Esc               = quit
//
// Build: gcc main.c -o pycity -Iraylib/src -Lraylib/src -lraylib -lm -lpthread -ldl -lrt -lX11
// Run:   ./pycity
//
// Assets expected at (relative to the working directory you run ./pycity from):
//   assets/tile_0025.png  -> road
//   assets/tile_0100.png  -> house
//   assets/tile_0073.png  -> factory
//   assets/tile_0000.png, tile_0001.png, tile_0002.png -> grass variants (empty tiles)

#include "raylib.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

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

// ---- economy tuning ----
#define DEMAND_GROWTH_PER_FRAME 0.04f   // how fast a house's demand fills up
#define SUPPLY_GROWTH_PER_FRAME 0.05f   // how fast a factory restocks
#define DELIVERY_AMOUNT 35.0f           // how much one truckload moves
#define MIN_DEMAND_TO_SERVE 15.0f       // house needs at least this much demand to get a truck
#define MIN_SUPPLY_TO_PICKUP 15.0f      // factory needs at least this much supply to send a truck

typedef enum { TILE_EMPTY = 0, TILE_ROAD, TILE_HOUSE, TILE_FACTORY, TOTAL_TILE_TYPES } TileType;
typedef enum { TOOL_ROAD = 0, TOOL_HOUSE, TOOL_FACTORY, TOOL_BULLDOZE } Tool;

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

typedef struct {
    int r, c;
    TileType type;
    float demand;  // used by TILE_HOUSE: 0-100, how much it wants a delivery
    float supply;  // used by TILE_FACTORY: 0-100, how much it has ready to ship
} Building;

static Building buildings[MAX_BUILDINGS];
static int buildingCount = 0;

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

// Picks a real factory->house trip based on who actually has supply/demand
// right now, instead of just grabbing two random buildings.
static void SpawnTruck(void) {
    int factoryCandidates[MAX_BUILDINGS];
    int houseCandidates[MAX_BUILDINGS];
    int factoryCount = 0, houseCount = 0;

    for (int i = 0; i < buildingCount; i++) {
        if (buildings[i].type == TILE_FACTORY && buildings[i].supply >= MIN_SUPPLY_TO_PICKUP) {
            factoryCandidates[factoryCount++] = i;
        } else if (buildings[i].type == TILE_HOUSE && buildings[i].demand >= MIN_DEMAND_TO_SERVE) {
            houseCandidates[houseCount++] = i;
        }
    }
    if (factoryCount == 0 || houseCount == 0) return; // nothing to ship right now

    for (int i = 0; i < MAX_TRUCKS; i++) {
        if (trucks[i].active) continue;

        int from = factoryCandidates[GetRandomValue(0, factoryCount-1)];
        int to = houseCandidates[GetRandomValue(0, houseCount-1)];

        int sr, sc, er, ec;
        if (!NearestRoad(buildings[from].r, buildings[from].c, &sr, &sc)) return;
        if (!NearestRoad(buildings[to].r, buildings[to].c, &er, &ec)) return;

        Truck t = {0};
        if (!BFSPath(sr, sc, er, ec, &t)) return;
        t.idx = 0;
        t.t = 0;
        t.speed = 0.02f + GetRandomValue(0, 15)/1000.0f;
        t.color = (Color){255,107,53,255};
        t.active = true;
        t.toBuildingIdx = to;
        trucks[i] = t;

        // Goods leave the factory now; they arrive at the house later.
        buildings[from].supply -= DELIVERY_AMOUNT;
        if (buildings[from].supply < 0) buildings[from].supply = 0;
        return;
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
        } else if (buildings[i].type == TILE_FACTORY) {
            if (buildings[i].supply < 100.0f) {
                buildings[i].supply += SUPPLY_GROWTH_PER_FRAME;
                if (buildings[i].supply > 100.0f) buildings[i].supply = 100.0f;
            }
        }
    }
}

static void UpdateTrucks(void) {
    for (int i = 0; i < MAX_TRUCKS; i++) {
        if (!trucks[i].active) continue;
        Truck *tr = &trucks[i];
        if (tr->idx >= tr->pathLen - 1) {
            tr->active = false;
            deliveries++;
            if (tr->toBuildingIdx >= 0 && tr->toBuildingIdx < buildingCount) {
                Building *b = &buildings[tr->toBuildingIdx];
                if (b->type == TILE_HOUSE) {
                    float served = (b->demand < DELIVERY_AMOUNT) ? b->demand : DELIVERY_AMOUNT;
                    b->demand -= served;
                    if (b->demand < 0) b->demand = 0;
                    totalDemandServed += served;
                }
            }
            continue;
        }
        tr->t += tr->speed;
        if (tr->t >= 1.0f) {
            tr->t = 0;
            tr->idx++;
            if (tr->idx >= tr->pathLen - 1) {
                tr->active = false;
                deliveries++;
                if (tr->toBuildingIdx >= 0 && tr->toBuildingIdx < buildingCount) {
                    Building *b = &buildings[tr->toBuildingIdx];
                    if (b->type == TILE_HOUSE) {
                        float served = (b->demand < DELIVERY_AMOUNT) ? b->demand : DELIVERY_AMOUNT;
                        b->demand -= served;
                        if (b->demand < 0) b->demand = 0;
                        totalDemandServed += served;
                    }
                }
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

// Small bar over a building showing demand (house) or supply (factory).
static void DrawBuildingMeter(Building *b) {
    Vector2 screenPos = WorldToScreen(CellCenterWorld(b->r, b->c));
    float x = screenPos.x - TILE*0.4f;
    float y = screenPos.y - TILE*0.55f;
    float w = TILE*0.8f;
    float h = 4.0f;
    if (screenPos.x < -TILE || screenPos.x > SCREEN_W+TILE) return;
    if (screenPos.y < TOP_BAR-TILE || screenPos.y > SCREEN_H+TILE) return;

    float pct = (b->type == TILE_HOUSE) ? (b->demand/100.0f) : (b->supply/100.0f);
    Color barColor;
    if (b->type == TILE_HOUSE) {
        // For houses, high demand = bad (unmet need), so red at high.
        barColor = (pct > 0.7f) ? (Color){220,60,50,255} : (pct > 0.35f) ? (Color){242,193,78,255} : (Color){90,170,90,255};
    } else {
        // For factories, high supply = good (ready to ship).
        barColor = (pct > 0.5f) ? (Color){90,170,90,255} : (pct > 0.2f) ? (Color){242,193,78,255} : (Color){120,120,120,255};
    }

    DrawRectangle((int)x, (int)y, (int)w, (int)h, (Color){20,20,20,180});
    DrawRectangle((int)x, (int)y, (int)(w*pct), (int)h, barColor);
}

static void AddBuilding(int r, int c, TileType type) {
    if (buildingCount >= MAX_BUILDINGS) return;
    buildings[buildingCount].r = r;
    buildings[buildingCount].c = c;
    buildings[buildingCount].type = type;
    buildings[buildingCount].demand = 0.0f;
    buildings[buildingCount].supply = (type == TILE_FACTORY) ? 50.0f : 0.0f; // factories start half-stocked
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

static void LoadAllAssets(void) {
    gameAssets[TILE_EMPTY] = (Texture2D){ 0 };
    LoadTileAsset(TILE_ROAD,    "assets/tile_0025.png");
    LoadTileAsset(TILE_HOUSE,   "assets/tile_0100.png");
    LoadTileAsset(TILE_FACTORY, "assets/tile_0073.png");

    const char *grassPaths[TOTAL_GRASS_VARIANTS] = {
        "assets/tile_0000.png",
        "assets/tile_0001.png",
        "assets/tile_0002.png"
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
}

// Draws one tile: the loaded texture if it exists, otherwise a flat color fallback.
// r, c are only used to look up which grass variant an empty tile should use.
static void DrawTile(TileType type, int r, int c, int x, int y) {
    Color fallback = (Color){30,38,35,255}; // grass / empty
    switch (type) {
        case TILE_ROAD:    fallback = (Color){58,67,64,255};   break;
        case TILE_HOUSE:   fallback = (Color){76,110,156,255}; break;
        case TILE_FACTORY: fallback = (Color){255,107,53,255}; break;
        default: break;
    }

    Texture2D tex;
    if (type == TILE_EMPTY) {
        tex = grassTextures[grassVariant[r][c]];
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

    fclose(f);

    // Clear any in-flight trucks - their paths reference the old world state.
    memset(trucks, 0, sizeof(trucks));

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

    Tool tool = TOOL_ROAD;
    bool paused = false;
    const float camSpeed = 12.0f; // tiles-ish per second, scaled below

    while (!WindowShouldClose()) {
        // ---- input ----
        if (IsKeyPressed(KEY_ONE))   tool = TOOL_ROAD;
        if (IsKeyPressed(KEY_TWO))   tool = TOOL_HOUSE;
        if (IsKeyPressed(KEY_THREE)) tool = TOOL_FACTORY;
        if (IsKeyPressed(KEY_FOUR))  tool = TOOL_BULLDOZE;
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
                    if (grid[r][c] == TILE_EMPTY) grid[r][c] = TILE_ROAD;
                    break;
                case TOOL_HOUSE:
                    if (grid[r][c] == TILE_EMPTY) { grid[r][c] = TILE_HOUSE; AddBuilding(r,c,TILE_HOUSE); }
                    break;
                case TOOL_FACTORY:
                    if (grid[r][c] == TILE_EMPTY) { grid[r][c] = TILE_FACTORY; AddBuilding(r,c,TILE_FACTORY); }
                    break;
                case TOOL_BULLDOZE:
                    if (grid[r][c] != TILE_EMPTY) {
                        if (grid[r][c] == TILE_HOUSE || grid[r][c] == TILE_FACTORY) RemoveBuildingAt(r,c);
                        grid[r][c] = TILE_EMPTY;
                    }
                    break;
            }
        }

        // ---- update ----
        if (!paused) {
            UpdateBuildings();
            UpdateTrucks();
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

        // building meters (demand/supply bars)
        for (int i = 0; i < buildingCount; i++) {
            DrawBuildingMeter(&buildings[i]);
        }

        // hover highlight
        if (hovering) {
            DrawRectangleLines((c-camC)*TILE, TOP_BAR + (r-camR)*TILE, TILE, TILE, (Color){242,193,78,255});
        }

        DrawTrucks();

        // toolbar (drawn last so it sits on top of the map)
        DrawRectangle(0, 0, SCREEN_W, TOP_BAR, (Color){28,35,33,255});
        const char *toolNames[4] = {"ROAD (1)", "HOUSE (2)", "FACTORY (3)", "BULLDOZE (4)"};
        DrawText(TextFormat("Tool: %s", toolNames[tool]), 10, 8, 18, (Color){255,107,53,255});

        double demandMetPct = (totalDemandGenerated > 0.001) ? (totalDemandServed/totalDemandGenerated*100.0) : 100.0;
        DrawText(TextFormat("Deliveries: %d   Buildings: %d   Demand met: %.0f%%   %s",
                  deliveries, buildingCount, demandMetPct, paused ? "[PAUSED]" : ""),
                  10, 32, 16, (Color){237,232,222,255});
        DrawText("F5 save   F9 load   Arrows/WASD pan", SCREEN_W-260, 20, 14, (Color){160,160,150,255});

        EndDrawing();
    }

    UnloadAllAssets();
    CloseWindow();
    return 0;
}