// Mods/ModLoader.h - v1.0.1 modding support: Lua economy overrides,
// the mid-game on_broke hook (ModLoader_TriggerOnBroke below), and
// pycity.message() for in-game toast feedback (ModLoader_PopMessage
// below).
//
// Scope: mods can retune the game's *economy numbers* (build costs,
// tax, crime/theft, road congestion, demand/supply growth) at startup,
// react at one specific mid-game moment (on_broke), and show the player
// toast messages. Mods CANNOT touch map layout, the resource-chain
// assignment system, save format, or rendering, and can't add new
// building/tool types (only place *existing* ones, via on_broke's
// auto_place) - that's all a much bigger surface (and bigger risk of
// breaking saves) than what this covers.
//
// Status: verified via a standalone Lua-loading test harness that
// bypasses raylib/the game entirely (real mod scripts, real Lua VM,
// confirmed load-order/clamping/sandboxing/bytecode-rejection/on_broke/
// message-queue behavior all work as documented). NOT yet built with
// the real Windows toolchain or played in an actual game session - see
// HANDOFF.md.
#ifndef MOD_LOADER_H
#define MOD_LOADER_H

#include <stddef.h>

// Shared buffer size for mod -> player messages (see
// ModLoader_PopMessage below) - both ModLoader.c (writing into the
// queue) and Main.c (reading out of it, sizing its own toast buffers)
// need to agree on this, hence it living in the shared header rather
// than as a private constant in either .c file.
#define MOD_MESSAGE_MAX_LEN 200

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

// Mid-game hook, separate from the startup-only balance overrides
// above. A mod can define a Lua function `pycity.on_broke(cost, money)`
// in its top-level script; if it does, its Lua state is kept alive for
// the rest of the session specifically so this can call back into it.
//
// Call this from the game loop at the exact moment a placement would
// otherwise fail purely for lack of money (see TryPlaceBuilding() in
// Main.c) - not on a timer, not every frame. `cost` is what the
// placement needs; `money` is what the player currently has (less than
// cost, or this wouldn't be getting called). Every loaded hook mod's
// on_broke is called, in load order; each may set `pycity.grant_money`
// (a number) and/or `pycity.auto_place` (a boolean) before returning -
// neither is required, and doing nothing means "this mod doesn't want
// to help this time" (mods can roll their own dice for a "sometimes"
// feel, e.g. via math.random). Results across all hook mods are
// combined: grant_money amounts are summed (then clamped to a sane
// range - see ModLoader.c), and auto_place is true if ANY mod set it.
//
// Always writes both output params, even if no hook mods are loaded
// (0.0 / 0 in that case) - the caller doesn't need to check
// g_hookModCount or anything like that first.
void ModLoader_TriggerOnBroke(double cost, double money, double *moneyGranted, int *autoPlace);

// Closes every hook mod's kept-alive Lua state. Call once, at game
// shutdown (after the main loop ends, alongside other cleanup) - not
// required for correctness before that (the OS reclaims everything on
// exit regardless), but avoids relying on that.
void ModLoader_Shutdown(void);

// A mod can call pycity.message("some text") from either its startup
// script or from inside on_broke, to show the player something in-game
// (print() alone isn't visible during normal play - there's no console
// window unless the exe was launched from a terminal). Messages queue
// up here rather than being drawn directly, since ModLoader.c has no
// concept of drawing - Main.c should call this once per frame (in a
// loop, since more than one message can queue up between frames) and
// render whatever comes back as a toast/notification, then let it fade.
//
// Copies the next queued message into outBuf (truncated to outBufSize,
// always NUL-terminated) and returns 1, or returns 0 if the queue is
// empty (outBuf is left untouched in that case). Call in a `while
// (ModLoader_PopMessage(buf, sizeof(buf)))` loop to drain everything
// queued since the last call, not just one message per frame.
int ModLoader_PopMessage(char *outBuf, size_t outBufSize);

#endif
