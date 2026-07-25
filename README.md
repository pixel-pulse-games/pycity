# 🛠️ How to Compile PyCity on Windows

This project features a native Windows compilation script (`build.bat`). You can build both 64-bit and legacy 32-bit versions of the game directly on your computer without installing heavy Linux environments or complex IDEs.

---

## 📋 Prerequisites

Before running the builder, you only need one portable tool:

1. Download **w64devkit** (a minimal, portable C/C++ compiler suite for Windows).
   * Get it from the official GitHub releases: `https://github.com/skeeto/w64devkit/releases`
2. Unzip the archive to a clean directory on your machine (e.g., `C:\w64devkit`).

---

## 🔨 Step-by-Step Compilation

1. **Download the Source**: Download this repository as a ZIP file and extract it into a folder.
2. **Launch the Builder**: Double-click the `build.bat` file in the main project folder.
3. **Run First-Time Setup**: The script will automatically open a setup configuration wizard:
   * Paste the absolute path to your 64-bit compiler bin folder (e.g., `C:\w64devkit\bin`).
   * If compiling 32-bit, paste the path to your 32-bit compiler bin folder.
4. **Choose Your Target**: The master menu will appear. Enter your choice:
   * Press `1` for a modern **64-bit Production Build** (`pycity-win64.exe`).
   * Press `2` for a legacy **32-bit Compatibility Build** (`pycity-win32.exe`).
   * Press `3` to trigger **Parallel Builds** and compile both architectures at the exact same moment.

The builder handles the system pathways and uses `mingw32-make` to compile the source code for 32 bit. Once finished, your executable will drop straight into the main folder, ready to play!
