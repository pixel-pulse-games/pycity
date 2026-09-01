# Modding PyCity

New in v1.0.1: PyCity can load `.lua` scripts from a `mods/` folder next
to the game's `.exe` to retune the game's economy numbers - no
recompiling required. Mods can also react mid-game via `pycity.on_broke`,
and show the player toast messages via `pycity.message()` - see below.

> **Status.** Verified via a standalone test harness that loads real
> mod scripts through a real Lua VM, bypassing raylib/the game entirely
> - confirmed economy overrides, `on_broke`, `pycity.message()`,
> sandboxing, and bytecode rejection all behave as documented here. Not
> yet built with the real Windows toolchain or played in an actual game
> session - see HANDOFF.md.

## Scope

Mods can do two things: override **balance numbers** (build costs, tax,
crime and theft, road congestion, demand/supply growth) at startup, and
optionally react **mid-game**, at the exact moment a placement would
otherwise fail for lack of money - see "Mid-game hook: on_broke" below.
Mods still **cannot**:

- change map size, tile art, or building sprites
- add new building/tool types (they can place *existing* types via
  `auto_place` in the hook below - not invent new ones)
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

## Mid-game hook: on_broke

Everything above only runs once, at startup. This is the one exception:
a mod can define a function called `pycity.on_broke`, and the game will
call it later, *during play*, at the exact moment you try to place
something and don't have enough money for it - not on a timer, not
every frame, only right then.

```lua
function pycity.on_broke(cost, money)
    -- cost:  what the thing you're trying to place actually costs
    -- money: what you currently have (always less than cost, or this
    --        wouldn't be getting called)
end
```

Inside that function, you can set either or both of:

| Field | Type | Effect |
|---|---|---|
| `pycity.grant_money` | number | Added to your money right now |
| `pycity.auto_place` | boolean | If true, places the building anyway - even if `grant_money` didn't (fully) cover it |

Doing neither is completely normal - most calls to `on_broke` should
probably do nothing, so that when it *does* help, it feels like it
means something. Use `math.random(...)` for a "sometimes" feel (see the
example below). If more than one hook mod is installed, every one gets
called: `grant_money` amounts are added together, and `auto_place`
happens if *any* mod set it.

There's no equivalent hook for "when you have plenty of money" or "every
frame" - this one trigger point (an unaffordable placement attempt) is
the only mid-game moment mods currently get a say in.

## Talking to the player: pycity.message()

`print("...")` still works, but it only reaches stdout - during normal
play there's no console window for the player to see it in (only
someone who launched the exe from a terminal would). To actually show
the player something, call:

```lua
pycity.message("Verity: covered a $40 shortfall")
```

This shows up as a toast in the bottom-left corner of the screen for a
few seconds, then fades out. It works from a startup script (e.g. to
announce "Easy mode active" when the game begins) and from inside
`on_broke` (e.g. to explain why money or a building just appeared).

A few practical limits: messages are capped at 199 characters (longer
ones get cut off, not rejected outright), and at most 5 toasts are ever
on screen at once - if you queue up more than that in a short span, the
oldest ones get replaced rather than piling up indefinitely.

## Example

Two examples exist, but as of v1.0.1 they're a **separate download**
(the "PyCity Mods" package), not bundled inside the game install - a
fresh `PyCity.exe` ships with no `mods/` folder at all and plays
completely unmodded until you add one yourself.

- `example_easy_mode.lua` - a startup-only mod. Halves every
  building cost, triples starting money, and slows crime growth. Same
  numbers, all the time, whether you need them or not.
- `Verity.lua` - uses `on_broke` instead. Changes nothing on
  its own; only steps in when you're about to fail a placement, and
  even then only about 1 time in 3, covering the exact shortfall and
  placing the building for you:

```lua
function pycity.on_broke(cost, money)
    if math.random(1, 3) ~= 1 then return end -- most of the time, do nothing
    local shortfall = cost - money
    pycity.grant_money = shortfall
    pycity.auto_place = true
    pycity.message(string.format("Verity: covered a $%.0f shortfall", shortfall))
end
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

- No priority/merge system beyond "later filename wins" for the
  startup fields - if two mods disagree there, only the constraint
  above decides which one sticks. `on_broke` is different: every hook
  mod gets called, not just one (see above).
- No way for a mod to know what other mods are loaded, or to depend on
  one another.
- No hot-reload - mods are read once at startup (though `on_broke`
  itself can still run any number of times later, using that same
  startup read).
- No in-game UI to see which mods are active; check the console output.
- `on_broke` is the *only* mid-game hook - there's no equivalent for
  other moments (e.g. "money is running low" without an actual failed
  placement, or "a theft just happened").
