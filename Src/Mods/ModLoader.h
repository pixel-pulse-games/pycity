// Mods/ModLoader.h - v1.0.1-nightly modding support (Lua economy overrides)
//
// Scope, deliberately kept narrow for this first pass: mods can retune
// the game's *economy numbers* (build costs, tax, crime/theft, road
// congestion, demand/supply growth) by dropping a .lua file in mods/.
// Mods CANNOT touch map layout, the resource-chain assignment system,
// save format, rendering, or add new building types - that's all a much
// bigger surface (and bigger risk of breaking saves) than "let people
// retune the balance numbers," which is the actual ask this covers.
//
// Status: fresh this session, NOT built or run yet (no Windows box
// available - see HANDOFF.md). Syntax-checked only.
#ifndef MOD_LOADER_H
#define MOD_LOADER_H

#include <stddef.h>

// Every field here mirrors one of the DEFAULT_* balance constants in
// Main.c (see the "Money tuning" / "Police & crime tuning" / "Road
// congestion tuning" / economy sections up top). Main.c owns the actual
// default values (in g_econ's initializer) - this struct is just the
// shape mods are allowed to poke at.
typedef struct {
    double startingMoney;
    double costRoad, costHouse, costFactory, costFarm, costPolice;
    double taxPerHousePerFrame;
    double bulldozeRefundPercent;

    int    policeCoverageRadius;
    float  crimeGrowthPerFrame;
    float  crimeDecayPerFrame;
    float  theftCrimeThreshold;
    int    theftChancePerFrame;
    double theftAmount;

    float congestionPerTruckPerFrame;
    float congestionDecayPerFrame;
    float congestionMax;
    float congestionSlowdownThreshold;
    float congestionMaxSlowdown;

    float demandGrowthPerFrame;
    float supplyGrowthPerFrame;
    float foodDemandGrowthPerFrame;
    float deliveryAmount;
    float minDemandToServe;
    float minSupplyToPickup;
} EconomyTunables;

// Scans `modsDir` for *.lua files (non-recursive), sorts them by
// filename, and runs each one in its own sandboxed Lua state against a
// `pycity` table pre-populated with econ's current values (see
// MODDING.md for the field names a script can read/write). Whatever a
// script leaves in that table is copied back into `econ`, clamped to a
// safe range per field (see FIELDS in ModLoader.c) so a bad or hostile
// mod can't zero out a value later used as a divisor, or set a negative
// cost. If two mods touch the same field, the one that runs later (i.e.
// later alphabetically) wins - there's no merge/priority system yet.
//
// A missing or empty modsDir is not an error - it just means nothing to
// load, and econ is left untouched. Every mod's filename, and any
// load/runtime error in it, is printed to stdout/stderr - there's no
// in-game console yet, so this is the only feedback a mod author gets.
//
// SECURITY: any file that is (or looks like) compiled Lua bytecode
// rather than Lua source is treated as an attack attempt, not a mod to
// skip - loading untrusted bytecode can corrupt the Lua VM directly,
// bypassing the parser entirely (see the Lua manual's own warning on
// this). Finding one appends an entry to security_violation.log and
// terminates the whole game immediately via exit(1) - it does not just
// skip that file and keep going, since at that point mod loading is in
// an unknown state.
//
// Returns the number of mod files that loaded and ran without error.
int ModLoader_LoadMods(EconomyTunables *econ, const char *modsDir);

#endif
