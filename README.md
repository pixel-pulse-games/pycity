# Src/Mods/lua/

This is the official Lua 5.4.7 source, copied in as an amalgamated
"library" build so PyCity can embed a Lua interpreter without dragging
in a full separate build step or external dependency.

- **Source**: https://github.com/lua/lua, tag `v5.4.7`.
- **What's here**: every `.c`/`.h` file from that tag *except*
  `lua.c`, `luac.c`, `ltests.c`, and `ltests.h` - those are the
  standalone `lua`/`luac` command-line binaries and Lua's own internal
  test harness, none of which PyCity needs as an embedded library.
- **How it's built**: `onelua.c` is Lua's own official single-file
  amalgamation - it `#include`s all the other `.c` files in this folder
  itself. Compiling `onelua.c` alone (with `-DMAKE_LIB`, see the
  Makefile) produces the whole interpreter as one translation unit.
  `-DMAKE_LIB` skips the `#include "lua.c"` / `#include "luac.c"` blocks
  at the bottom of `onelua.c`, so no `main()` is defined here - it's a
  pure library, meant to be driven from `Mods/ModLoader.c`.
- **License**: Lua's MIT license. See `lua.h`'s header comment or
  https://www.lua.org/license.html - unmodified from upstream, not
  PyCity's own license.
- **Not modified**: nothing in this folder has been changed from the
  upstream tag. If Lua ships a security fix or a new version, the whole
  folder can just be replaced wholesale from a newer tag of
  `github.com/lua/lua` (same file list as above).
