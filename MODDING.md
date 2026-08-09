# Modding PyCity

New in v1.0.1-nightly: PyCity can load `.lua` scripts from a `mods/` folder
next to the game's `.exe` to retune the game's economy numbers - no
recompiling required.

> **Status: nightly only, not in v1.0.0** 

## Scope

Mods can override **balance numbers only**: build costs, tax, crime and
theft, road congestion, and demand/supply growth rates. Mods **cannot**:

- change map size, tile art, or building sprites
- add new building/tool types
- touch the resource-chain system (which farms/factories/houses get
  Grain vs. Timber)
- read or write files, launch other programs, or do anything outside the
  `pycity` table below - see "Sandboxing" below.

This is deliberate: retuning numbers can't corrupt a save or crash the
game, while a general-purpose scripting hook into game logic could do
either. If you want something outside this list, open an issue - it's
not off the table for later, just out of scope for this first pass.

## How it works

1. Drop a `.lua` file into `mods/` (create the folder next to the
   `.exe` if it doesn't exist).
2. Launch the game. Before the window even opens, PyCity finds every
   `.lua` file in `mods/`, sorts them alphabetically by filename, and
   runs each one in turn.
3. Each script sees a global table called `pycity`, pre-filled with the
   *current* value of every tunable (defaults, unless an earlier mod
   already changed something). Read from it, do math, and assign back
   into it - whatever's left in `pycity` when your script finishes is
   applied.
4. If two mods touch the same field, whichever mod's filename sorts
   later wins (`02_x.lua` overrides `01_y.lua`). Prefix your mod
   filenames with numbers if load order matters to you.
5. Every mod that loads (or fails to) is logged to the terminal/console
   the game was launched from. There's no in-game console yet, so
   that's the only feedback you get today - worth launching the exe
   from a terminal at least once while testing a new mod.

## Available fields

All fields live under the global `pycity` table, e.g.
`pycity.cost_house = 25`.

| Field | Default | What it controls |
|---|---|---|
| `starting_money` | 500 | Money the city starts with |
| `cost_road` | 10 | Cost to place one road tile |
| `cost_house` | 50 | Cost to place a house |
| `cost_factory` | 150 | Cost to place a factory |
| `cost_farm` | 100 | Cost to place a farm |
| `cost_police` | 200 | Cost to place a police station |
| `tax_per_house_per_frame` | 0.006 | Flat tax income per house, per frame |
| `bulldoze_refund_percent` | 0.5 | Fraction of build cost refunded on bulldoze (0-1) |
| `police_coverage_radius` | 9 | Tiles (Manhattan distance) a police station protects |
| `crime_growth_per_frame` | 0.015 | How fast crime rises on an uncovered house |
| `crime_decay_per_frame` | 0.05 | How fast crime falls on a covered house |
| `theft_crime_threshold` | 70 | Crime level (0-100) above which theft becomes possible |
| `theft_chance_per_frame` | 900 | 1-in-N chance of a theft event per eligible house per frame (never goes below 1) |
| `theft_amount` | 6 | Money stolen per theft event |
| `congestion_per_truck_per_frame` | 3 | Congestion added to a road tile per truck currently on it |
| `congestion_decay_per_frame` | 1.5 | How fast a road tile's congestion drains back to 0 |
| `congestion_max` | 100 | Congestion cap per tile |
| `congestion_slowdown_threshold` | 30 | Congestion below this doesn't slow trucks at all |
| `congestion_max_slowdown` | 0.7 | At max congestion, trucks move at `(1 - this)` of normal speed (capped below 1.0) |
| `demand_growth_per_frame` | 0.04 | How fast a house's demand for goods fills up |
| `supply_growth_per_frame` | 0.05 | How fast a farm's crops grow back in |
| `food_demand_growth_per_frame` | 0.05 | How fast a factory's hunger for food fills up |
| `delivery_amount` | 35 | How much one truckload moves |
| `min_demand_to_serve` | 15 | Minimum demand before a house/factory qualifies for a truck |
| `min_supply_to_pickup` | 15 | Minimum supply before a farm/factory can send a truck |

Every field is clamped to a safe range after your script runs (e.g. costs
can't go negative, `theft_chance_per_frame` can't hit 0 since the game
uses it as a divisor). If you set something out of range, it's silently
clamped rather than crashing the game - check the field's behavior
in-game if a value doesn't seem to be taking effect the way you expected.

## Example

See `mods/example_easy_mode.lua.disabled` (rename to drop `.disabled`
to try it) - halves every building cost, triples starting money, and
slows crime growth.

```lua
-- Building costs, halved.
pycity.cost_house = pycity.cost_house / 2
pycity.cost_factory = pycity.cost_factory / 2

-- Start with more cash in the bank.
pycity.starting_money = pycity.starting_money * 3

print("[my_mod] loaded")
```

## Sandboxing

Mod scripts run in their own Lua state with only the base language plus
`string`, `math`, and `table` available - **not** `io`, `os`, or
`package`/`require`. A mod cannot read or write files, launch other
programs, or load additional code from disk. It can only compute numbers
and assign them into the `pycity` table. This is intentional: you should
be able to drop in a mod someone else wrote without worrying about what
else it might do to your machine.

## Security: no compiled bytecode

Mods must be plain Lua **source** (readable text). A file that's
actually compiled Lua bytecode (the output of `string.dump()` or the
`luac` compiler) - even if it's named `something.lua` - is refused.

This isn't just a format check. Loading Lua bytecode from a source you
haven't audited is a genuine security risk: unlike source, which the
parser validates before anything runs, bytecode is loaded straight into
the Lua VM's internal structures - a hand-crafted binary chunk can
corrupt the VM directly. Because of that, finding one is treated as an
attack attempt rather than a bad file to politely skip: the game writes
an entry to `security_violation.log` (next to the exe) and **exits
immediately**, without loading any other mods, even ones that would
otherwise have loaded fine.

If you see the game quit with a `security_violation.log` message, it
means a `.lua` file in your `mods/` folder wasn't actually Lua
source - check where it came from before re-adding it.

## Known limitations (this first pass)

- No priority/merge system beyond "later filename wins" - if two mods
  disagree, only the constraint above decides which one sticks.
- No way for a mod to know what other mods are loaded, or to depend on
  one another.
- No hot-reload - mods are read once at startup.
- No in-game UI to see which mods are active; check the console output.
