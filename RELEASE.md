# PyCity v1.1.0 — Game Speed

A feature update on top of v1.0.1.

## New

- **Game speed control.** Press **Tab** to cycle the simulation between
  **1x, 2x and 4x**. The current speed is shown in the top bar
  (`Speed: 2x`). Everything scales together — house demand, farm growth,
  tax income, crime, and trucks — so a city plays out the same way, just
  faster. Pause (**Space**) still works at any speed. Speed always starts
  at 1x on launch and isn't stored in saves.
- Updated the on-screen controls hint to list pause, speed, save/load,
  pan and zoom.

## Unchanged

- Balance numbers, building costs and the save format (`PCI3`) are the
  same as v1.0.1. Existing saves and mods keep working.

## Known notes

- At 4x, trucks cover four steps per frame, so they can look like they
  skip tiles on very short roads. If that bothers you, stick to 1x–2x and
  let us know.
- This build hasn't had a full Windows playtest yet. Please report
  anything odd.
