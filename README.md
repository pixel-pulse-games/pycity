# PyCity

A small top-down city builder for Windows. Lay roads, place farms, factories
and houses, and keep the trucks moving. Written in C with
[raylib](https://www.raylib.com/).

**Website and downloads:** <https://pycity.pixel-pulse.dev>
&nbsp;|&nbsp; **Releases:** [GitHub Releases](https://github.com/pixel-pulse-games/pycity/releases)

## Download

1. Open the [latest release](https://github.com/pixel-pulse-games/pycity/releases/latest).
2. Download `pycity-win64.zip` (most computers) or `pycity-win32.zip` (32-bit Windows).
3. Unzip it and run the `.exe`. There is no installer.

You need Windows 10 or newer. To update an existing install, use `Patcher.zip`
from the same release; it checks the download against `checksums.txt` before
replacing any files.

> If a city saved in an older version won't load, convert it with the
> [save converter](https://pycity.pixel-pulse.dev/save_convert). It runs in
> your browser and uploads nothing.

## How the game works

Every farm, factory and house belongs to one of two resource chains, grain or
timber, and only trades with buildings in its own chain. A building needs a
road next to it to take part.

- **Farms** grow food and send it by truck to a factory.
- **Factories** turn it into goods and send them to a house.
- **Houses** ask for goods and pay tax.

Roads get congested, so one road for everything jams up. Houses far from a
police station slowly attract theft; a station covers about nine tiles around
it. The map holds up to 128 buildings.

## Controls

| Key | Action |
| --- | --- |
| `1` to `6` | Pick a tool: road ($10), house ($50), factory ($150), farm ($100), police station ($200), bulldoze (refunds half) |
| Left click | Use the tool on the tile under the mouse |
| Arrow keys or `W` `A` `S` `D` | Pan the map |
| Mouse wheel, `+` or `-` | Zoom |
| `Space` | Pause or resume |
| `Tab` | Cycle game speed: 1x, 2x, 4x |
| `F5` / `F9` | Save / load |
| `Esc` | Quit |

Costs are the defaults; mods can change them.

## Modding

PyCity loads Lua scripts from a `mods/` folder next to the `.exe`. Mods can
retune costs, tax, crime, congestion and growth, react when a placement fails
for lack of money, and show messages to the player. They run in a sandbox with
no file or shell access, and compiled Lua bytecode is rejected.

See [MODDING.md](MODDING.md) for the full field reference. The game runs
unmodded when there is no `mods/` folder.

## Building from source

Windows only. You need the 64-bit and/or 32-bit
[w64devkit](https://github.com/skeeto/w64devkit/releases) toolchain
(`w64devkit-x64-*.7z.exe` and `w64devkit-x86-*.7z.exe`).

1. Run `Src\build.bat`. The first time, it asks for the path to each
   toolchain's `bin` folder (the one containing `gcc.exe`) and saves them to
   `config.ini`.
2. Pick a build from the menu: 64-bit, 32-bit, or both. Press `r` to reset the
   saved paths.
3. Run `Src\pycity-win64.exe` (or `pycity-win32.exe`) from the `Src` folder so
   it finds `assets/`.

The same script builds `Patcher.exe` and `Bootstrap.exe`. A step-by-step guide
is on the [website](https://pycity.pixel-pulse.dev/Compiling).

## Releasing

Releases are built and published by GitHub Actions
(`.github/workflows/release.yml`):

1. Set the new version in `VERSION` (for example `1.2.0`; a suffix like
   `1.2.0-rc.1` publishes as a pre-release).
2. Write the release notes in `RELEASE.md`.
3. Update the window title in `Src/Main.c` to `PyCity - v1.2.0`. The workflow
   fails if it doesn't match `VERSION`.
4. Push to `main`.

The workflow builds the 64-bit and 32-bit game, the patcher and the bootstrap,
checks each exe's architecture, zips them in the layout the patcher expects,
writes `checksums.txt`, and creates the release with `RELEASE.md` as its body.
To test a build without publishing, run it manually from the Actions tab and
leave "publish" unticked.

## Project layout

| Path | What's there |
| --- | --- |
| `Src/Main.c` | The game |
| `Src/Mods/` | Lua mod loader, plus the embedded Lua 5.4.7 source |
| `Src/Patcher/`, `Src/Bootstrap/` | The updater and the tool that downloads it |
| `Src/assets/` | Tile art, including the winter and April Fools sets |
| `Src/raylib64/`, `Src/raylib32/` | raylib headers and static libraries |
| `Src/build.bat`, `Src/Makefile` | Build scripts |
| `docs/` | The website (plain HTML, served from this folder) |
| `.github/workflows/` | Release and checksum workflows |

## Credits and license

Tile art comes from [Kenney's](https://kenney.nl) free asset packs. The game
uses [raylib](https://www.raylib.com/) and [Lua](https://www.lua.org/).

PyCity is licensed under the
[PolyForm Noncommercial License 1.0.0](LICENSE): you can use, modify and share
it for noncommercial purposes.
