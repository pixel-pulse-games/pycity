SRC    = main.c Mods/ModLoader.c Mods/lua/onelua.c
LIBS   = -lraylib -lgdi32 -lwinmm -lopengl32 -mwindows -lm
# -DMAKE_LIB: builds Mods/lua/onelua.c as an embeddable Lua library
#   (no `main()`, no standalone `lua`/`luac` binaries baked in) - see
#   Mods/lua/README.md.
# -I .: so ModLoader.c's `#include "lua/lua.h"` resolves relative to Src/.
CFLAGS = -DMAKE_LIB -I .

all:
	gcc $(SRC) -o $(OUT) -I$(RAYLIB_DIR) -L$(RAYLIB_DIR) $(CFLAGS) $(LIBS)
	@echo ">>> Finished Compiling Target: $(OUT)"
