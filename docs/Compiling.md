# 🛠️ Compiling PyCity Natively on Windows

PyCity is written in C/C++ using the ultra-lightweight **Raylib** graphics library. To keep things fast and transparent, the game features a native Windows Master Builder script (`build.bat`). 

You can build modern 64-bit production builds or legacy 32-bit compatibility builds directly on your PC without installing massive IDEs, heavy development tools, or Linux emulation layers.

---

## 📋 1. Prerequisites (Get the Compilers)

The build script relies on a portable C/C++ toolchain called **w64devkit**. Because it cuts out unnecessary bloat, you must grab the individual architecture files for the builds you want to make:

1. Head to the official compiler archive: `https://github.com/skeeto/w64devkit/releases`
2. Download the **64-bit toolchain** (`w64devkit-x64-X.X.X.zip`) for the main 64-bit executable.
3. Download the **32-bit toolchain** (`w64devkit-x86-X.X.X.zip`) if you want to build the legacy 32-bit compatibility version.
4. Unzip them somewhere safe on your storage drive (e.g., `C:\w64devkit` and `C:\w32devkit`).

---

## ⚙️ 2. The First-Time Setup Wizard

When you double-click the **`build.bat`** script for the first time, it automatically detects that no settings exist and launches the **Path Configuration Wizard**:

1. Open your extracted PyCity folder and launch `build.bat`.
2. The terminal will ask you to enter the absolute path to your compiler `bin` directories.
3. **Do not include quotation marks.** Paste the direct paths to the folders containing `gcc.exe` and `make.exe`:
   * **64-bit Prompt:** e.g., `C:\w64devkit\bin`
   * **32-bit Prompt:** e.g., `C:\w32devkit\bin`
4. Press Enter. The batch script will clean any trailing whitespaces, format the options, and save them into a permanent local file named `config.ini`.

---

## 🔨 3. Operating the Master Builder Suite

Once your configuration paths are saved, the main builder menu will load. Choose your target compilation mode:

* **`1` - x64 Production Build:** Sets the path to your 64-bit compiler, hooks into the `raylib64/src` folder, and uses `make` to compile the standard 64-bit version (`pycity-win64.exe`).
* **`2` - x86 Legacy Compatibility Build:** Temporarily changes your active system execution path to your 32-bit compiler, maps out the `raylib32/src` folder, and outputs the 32-bit version (`pycity-win32.exe`).
* **`3` - Build Both Architectures (Simultaneously):** Spawns two independent, parallel background threads (`start /b cmd /c`). This fires off both the 64-bit and 32-bit compiler engines at the exact same moment to save time.

---

## 🔄 How to Reset Configuration Paths

If you ever move your compiler directories, move your project folders, or type a path incorrectly during the first setup:
1. Launch `build.bat` to pull up the menu interface.
2. Press **`r`** to choose "Reset Configuration Paths".
3. The script will wipe out your local `config.ini` file and immediately loop you back into the step-by-step setup wizard to put in fresh paths.
