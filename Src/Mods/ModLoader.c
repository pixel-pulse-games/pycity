// Mods/ModLoader.c - see ModLoader.h for scope/status notes.
//
// Embeds Lua via the official amalgamated single-file build
// (Mods/lua/onelua.c, compiled with -DMAKE_LIB so it's a library, not a
// standalone `lua` interpreter binary - see Mods/lua/README.md for where
// this came from and how to update it).
#include "ModLoader.h"

#include "lua/lua.h"
#include "lua/lauxlib.h"
#include "lua/lualib.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ---- field table: drives both "push current values into Lua" and
// "read Lua back into econ" off one list, so the two directions can't
// drift apart from each other. Add a new moddable number by adding one
// line here (and the matching field in ModLoader.h) - nothing else
// needs to change. ----
typedef struct {
    const char *luaField;
    size_t offset;
    enum { FIELD_DOUBLE, FIELD_FLOAT, FIELD_INT } kind;
    double minVal, maxVal; // clamp applied when reading a script's value back
} FieldDesc;

#define OFS(f) offsetof(EconomyTunables, f)

static const FieldDesc FIELDS[] = {
    { "starting_money",                 OFS(startingMoney),                 FIELD_DOUBLE, 0.0,   1000000.0 },
    { "cost_road",                      OFS(costRoad),                      FIELD_DOUBLE, 0.0,   100000.0  },
    { "cost_house",                     OFS(costHouse),                     FIELD_DOUBLE, 0.0,   100000.0  },
    { "cost_factory",                   OFS(costFactory),                   FIELD_DOUBLE, 0.0,   100000.0  },
    { "cost_farm",                      OFS(costFarm),                      FIELD_DOUBLE, 0.0,   100000.0  },
    { "cost_police",                    OFS(costPolice),                    FIELD_DOUBLE, 0.0,   100000.0  },
    { "tax_per_house_per_frame",        OFS(taxPerHousePerFrame),           FIELD_DOUBLE, 0.0,   1000.0    },
    { "bulldoze_refund_percent",        OFS(bulldozeRefundPercent),         FIELD_DOUBLE, 0.0,   1.0       },
    { "police_coverage_radius",         OFS(policeCoverageRadius),          FIELD_INT,    0,     1000      },
    { "crime_growth_per_frame",         OFS(crimeGrowthPerFrame),           FIELD_FLOAT,  0.0,   1000.0    },
    { "crime_decay_per_frame",          OFS(crimeDecayPerFrame),            FIELD_FLOAT,  0.0,   1000.0    },
    { "theft_crime_threshold",          OFS(theftCrimeThreshold),           FIELD_FLOAT,  0.0,   100.0     },
    // Never 0 - Main.c uses this as GetRandomValue(0, N), and a mod
    // trying to set "theft happens every frame" should get "very often"
    // (1-in-1), not a meaningless/degenerate 1-in-0.
    { "theft_chance_per_frame",         OFS(theftChancePerFrame),           FIELD_INT,    1,     1000000   },
    { "theft_amount",                   OFS(theftAmount),                   FIELD_DOUBLE, 0.0,   1000000.0 },
    { "congestion_per_truck_per_frame", OFS(congestionPerTruckPerFrame),    FIELD_FLOAT,  0.0,   10000.0   },
    { "congestion_decay_per_frame",     OFS(congestionDecayPerFrame),       FIELD_FLOAT,  0.0,   10000.0   },
    { "congestion_max",                 OFS(congestionMax),                 FIELD_FLOAT,  1.0,   1000000.0 },
    { "congestion_slowdown_threshold",  OFS(congestionSlowdownThreshold),   FIELD_FLOAT,  0.0,   1000000.0 },
    // Capped below 1.0 - Main.c computes speed as (1 - pct * this), and
    // trucks fully stopping (or reversing) would hang the delivery loop
    // rather than just "slow down a lot."
    { "congestion_max_slowdown",        OFS(congestionMaxSlowdown),         FIELD_FLOAT,  0.0,   0.99      },
    { "demand_growth_per_frame",        OFS(demandGrowthPerFrame),          FIELD_FLOAT,  0.0,   1000.0    },
    { "supply_growth_per_frame",        OFS(supplyGrowthPerFrame),          FIELD_FLOAT,  0.0,   1000.0    },
    { "food_demand_growth_per_frame",   OFS(foodDemandGrowthPerFrame),      FIELD_FLOAT,  0.0,   1000.0    },
    { "delivery_amount",                OFS(deliveryAmount),                FIELD_FLOAT,  0.01,  1000000.0 },
    { "min_demand_to_serve",            OFS(minDemandToServe),              FIELD_FLOAT,  0.0,   100.0     },
    { "min_supply_to_pickup",           OFS(minSupplyToPickup),             FIELD_FLOAT,  0.0,   100.0     },
};
#define NUM_FIELDS (sizeof(FIELDS) / sizeof(FIELDS[0]))

static double GetFieldValue(const EconomyTunables *econ, const FieldDesc *f) {
    const char *base = (const char *)econ;
    switch (f->kind) {
        case FIELD_DOUBLE: return *(const double *)(base + f->offset);
        case FIELD_FLOAT:  return (double)*(const float *)(base + f->offset);
        case FIELD_INT:    return (double)*(const int *)(base + f->offset);
    }
    return 0.0;
}

static void SetFieldValue(EconomyTunables *econ, const FieldDesc *f, double v) {
    if (v < f->minVal) v = f->minVal;
    if (v > f->maxVal) v = f->maxVal;
    char *base = (char *)econ;
    switch (f->kind) {
        case FIELD_DOUBLE: *(double *)(base + f->offset) = v; break;
        case FIELD_FLOAT:  *(float *)(base + f->offset) = (float)v; break;
        case FIELD_INT:    *(int *)(base + f->offset) = (int)v; break;
    }
}

// Builds the `pycity` global table, pre-populated with econ's current
// values, so a script can read a default (e.g. `pycity.cost_house * 2`)
// as well as overwrite it outright.
static void PushEconomyTable(lua_State *L, const EconomyTunables *econ) {
    lua_newtable(L);
    for (size_t i = 0; i < NUM_FIELDS; i++) {
        lua_pushnumber(L, GetFieldValue(econ, &FIELDS[i]));
        lua_setfield(L, -2, FIELDS[i].luaField);
    }
    lua_setglobal(L, "pycity");
}

// Reads whatever the script left in the `pycity` table back into econ.
// A field the script didn't touch, or set to something non-numeric,
// keeps whatever value it already had going in.
static void PullEconomyTable(lua_State *L, EconomyTunables *econ) {
    lua_getglobal(L, "pycity");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
    for (size_t i = 0; i < NUM_FIELDS; i++) {
        lua_getfield(L, -1, FIELDS[i].luaField);
        if (lua_isnumber(L, -1)) {
            SetFieldValue(econ, &FIELDS[i], lua_tonumber(L, -1));
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1); // pop pycity table
}

// Every Lua binary chunk (i.e. precompiled bytecode, the output of
// string.dump() or the `luac` compiler) starts with this same 4-byte
// header - see LUA_SIGNATURE in Mods/lua/lua.h / lundump.c. Source
// files never start with these bytes (ESC is not valid at the start of
// Lua source).
static const unsigned char LUA_BYTECODE_HEADER[4] = { 0x1B, 'L', 'u', 'a' };

// Loading Lua *bytecode* from an untrusted source is a real,
// well-documented risk - unlike source, a binary chunk is loaded
// straight into the VM's internal structures without going through the
// lexer/parser's validation, so a hand-crafted one can corrupt the VM
// directly (out-of-bounds reads/writes, type confusion) rather than
// just fail to parse. The Lua manual itself calls this out. mods/ is
// exactly "files from someone we haven't audited," so a mod shipping
// bytecode instead of source is treated as an attack attempt, not a
// format mistake - this fails loudly and stops the whole game rather
// than quietly skipping just that one mod.
static int LooksLikeLuaBytecode(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0; // can't read it - RunOneMod's own fopen call will surface the real error
    unsigned char header[4] = {0};
    size_t n = fread(header, 1, sizeof(header), f);
    fclose(f);
    return (n == sizeof(header)) && (memcmp(header, LUA_BYTECODE_HEADER, sizeof(header)) == 0);
}

// Appends one line to security_violation.log (created next to wherever
// the game's working directory is, alongside mods/ and any save file),
// then terminates the process immediately. Deliberately NOT a
// "log and continue" - see LooksLikeLuaBytecode()'s comment for why a
// bytecode mod is treated as a security event rather than a bad file to
// skip past.
static void FailSecurityViolation(const char *path, const char *reason) {
    FILE *log = fopen("security_violation.log", "a");
    if (log) {
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        char timestamp[32];
        if (t && strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", t) > 0) {
            fprintf(log, "[%s] SECURITY VIOLATION: %s: %s\n", timestamp, path, reason);
        } else {
            fprintf(log, "[unknown time] SECURITY VIOLATION: %s: %s\n", path, reason);
        }
        fclose(log);
    }
    // Also to stderr in case security_violation.log itself couldn't be
    // written (e.g. installed to a read-only location) - the player
    // should get *some* explanation for why the game just quit.
    fprintf(stderr, "SECURITY VIOLATION: %s: %s\nSee security_violation.log. Exiting.\n", path, reason);
    exit(1);
}

static int RunOneMod(const char *path, EconomyTunables *econ) {
    // Hard stop before Lua is even involved: reject compiled bytecode
    // outright based on its raw file header.
    if (LooksLikeLuaBytecode(path)) {
        FailSecurityViolation(path, "file is compiled Lua bytecode, not Lua source - refusing to load");
    }

    lua_State *L = luaL_newstate();
    if (!L) {
        fprintf(stderr, "[mods] %s: could not create Lua state (out of memory?)\n", path);
        return 0;
    }

    // Deliberately NOT luaL_openlibs(). That would also hand every mod
    // io/os/package (i.e. require(), arbitrary file reads/writes,
    // os.execute()) - way more than "compute some balance numbers"
    // needs. Base + string/math/table is enough for real tuning logic
    // (conditionals, arithmetic, string formatting for print()
    // debugging) without giving a mod filesystem or process access.
    luaL_requiref(L, "_G", luaopen_base, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1); lua_pop(L, 1);

    PushEconomyTable(L, econ);

    int ok = 1;
    // mode = "t": text chunks only. This is defense-in-depth on top of
    // LooksLikeLuaBytecode() above - Lua's own loader refuses a binary
    // chunk here too, in case something slipped past the raw header
    // check (a truncated file, an unusual wrapper, etc.). Either path
    // catching it is treated as the same security violation.
    if (luaL_loadfilex(L, path, "t") != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        int isBinaryChunkError = err && (strstr(err, "binary chunk") != NULL);
        if (isBinaryChunkError) {
            lua_close(L);
            FailSecurityViolation(path, "Lua loader rejected file as a binary chunk");
        }
        fprintf(stderr, "[mods] %s: %s\n", path, err ? err : "(unknown error)");
        ok = 0;
    } else if (lua_pcall(L, 0, LUA_MULTRET, 0) != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        fprintf(stderr, "[mods] %s: %s\n", path, err ? err : "(unknown error)");
        ok = 0;
    } else {
        PullEconomyTable(L, econ);
        printf("[mods] loaded %s\n", path);
    }

    lua_close(L);
    return ok;
}

// Small strdup replacement - `strdup` is a POSIX/MSVCRT extension, not
// standard C, so it's not reliably declared under -std=c99 across
// toolchains (this file gets compiled with the same flags as Main.c).
// Returns NULL on allocation failure, same contract as strdup.
static char *DupString(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = (char *)malloc(len);
    if (copy) memcpy(copy, s, len);
    return copy;
}

static int HasLuaExtension(const char *name) {
    size_t len = strlen(name);
    if (len < 4) return 0;
    const char *ext = name + len - 4;
    // Windows filenames are effectively case-insensitive, so accept
    // .LUA/.Lua too rather than silently skipping a mod over casing.
    return (ext[0] == '.')
        && (ext[1] == 'l' || ext[1] == 'L')
        && (ext[2] == 'u' || ext[2] == 'U')
        && (ext[3] == 'a' || ext[3] == 'A');
}

// Cap on how many mod files one folder can contain. Startup-only path,
// generous enough for any realistic mods/ folder without an unbounded
// alloc for something that just runs once before the window opens.
#define MAX_MOD_FILES 256

int ModLoader_LoadMods(EconomyTunables *econ, const char *modsDir) {
    DIR *dir = opendir(modsDir);
    if (!dir) return 0; // no mods/ folder - nothing to load, not an error

    char *names[MAX_MOD_FILES];
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < MAX_MOD_FILES) {
        if (HasLuaExtension(entry->d_name)) {
            names[count] = DupString(entry->d_name);
            if (names[count]) count++;
        }
    }
    closedir(dir);

    // Sort so load order is deterministic (readdir() order isn't
    // guaranteed alphabetical on every platform) - see header comment
    // on "later filename wins" when two mods touch the same field.
    // Plain insertion sort: count is capped at MAX_MOD_FILES and this
    // only runs once at startup, so O(n^2) is not worth avoiding here.
    for (int i = 1; i < count; i++) {
        char *key = names[i];
        int j = i - 1;
        while (j >= 0 && strcmp(names[j], key) > 0) {
            names[j + 1] = names[j];
            j--;
        }
        names[j + 1] = key;
    }

    int loaded = 0;
    for (int i = 0; i < count; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", modsDir, names[i]);
        if (RunOneMod(path, econ)) loaded++;
        free(names[i]);
    }

    return loaded;
}
