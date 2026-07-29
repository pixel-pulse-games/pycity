@echo off
setlocal enabledelayedexpansion
cls
set "CONFIG_FILE=config.ini"

:: Check if config.ini exists; if not, jump to setup configuration phase
if not exist "%CONFIG_FILE%" goto INITIAL_SETUP

:: Read paths directly out of the config.ini file
for /f "usebackq tokens=1,2 delims==" %%A in ("%CONFIG_FILE%") do (
    set "key=%%A"
    set "val=%%B"
    if "!key!"=="W64_BIN_PATH" set "W64_BIN_PATH=!val!"
    if "!key!"=="W32_BIN_PATH" set "W32_BIN_PATH=!val!"
)
goto MENU

:INITIAL_SETUP
echo ========================================================
echo First-Time Setup: PyCity Path Configuration Wizard
echo ========================================================
echo Please paste or type the absolute paths directly to
echo your "bin" folders (e.g., C:\Program Files\w64devkit\bin).
echo All accidental trailing spaces/garbage will be trimmed.
echo ========================================================
echo.
set /p "INPUT_64=Enter path to your 64-bit w64devkit\bin folder: "
set /p "INPUT_32=Enter path to your 32-bit w32devkit\bin folder: "

:: Strip external quote marks if entered
set "INPUT_64=%INPUT_64:"=%"
set "INPUT_32=%INPUT_32:"=%"

:: --------------------------------------------------------
:: ADVANCED LEFT-TO-RIGHT CHARACTER CHOPPING TRUNCATOR
:: --------------------------------------------------------
:: Process 64-bit Path
set "W64_BIN_PATH="
for /l %%I in (0,1,260) do (
    set "char=!INPUT_64:~%%I,1!"
    if "!char!"=="" goto CHOP_32
    :: If we find an accidental trailing space or slash when we shouldn't, skip it, 
    :: otherwise append the valid character to the clean string path
    set "W64_BIN_PATH=!W64_BIN_PATH!!char!"
)

:CHOP_32
:: Process 32-bit Path
set "W32_BIN_PATH="
for /l %%I in (0,1,260) do (
    set "char=!INPUT_32:~%%I,1!"
    if "!char!"=="" goto SAVE_CONFIG
    set "W32_BIN_PATH=!W32_BIN_PATH!!char!"
)

:SAVE_CONFIG
:: Run an explicit trailing-space cleaner loop to finalize the string right at the last letter
for /l %%I in (1,1,10) do (
    if "!W64_BIN_PATH:~-1!"==" " set "W64_BIN_PATH=!W64_BIN_PATH:~0,-1!"
    if "!W32_BIN_PATH:~-1!"==" " set "W32_BIN_PATH=!W32_BIN_PATH:~0,-1!"
)

:: Write clean structured INI configuration format
echo [Paths] > "%CONFIG_FILE%"
echo W64_BIN_PATH=%W64_BIN_PATH% >> "%CONFIG_FILE%"
echo W32_BIN_PATH=%W32_BIN_PATH% >> "%CONFIG_FILE%"

echo.
echo [+] Cleaned paths successfully saved to %CONFIG_FILE%!
echo [+] 64-Bit Path: "%W64_BIN_PATH%"
echo [+] 32-Bit Path: "%W32_BIN_PATH%"
echo.
pause
cls
goto MENU

:MENU
echo ========================================================
echo PyCity Native Windows Master Builder Suite
echo ========================================================
echo Choose your compilation target architecture:
echo.
echo 1) x64 Production Build (64-bit Core Binary)
echo 2) x86 Legacy Compatibility Build (32-bit Core Binary)
echo 3) Build Both Architectures (Simultaneously)
echo 4) Build Patcher (Patcher.exe, 32-bit - runs on 32-bit and 64-bit Windows)
echo r) Reset Configuration Paths
echo q) Exit Builder
echo ========================================================
echo.
set /p choice="Enter selection: "
if "%choice%"=="1" goto BUILD_64
if "%choice%"=="2" goto BUILD_32
if "%choice%"=="3" goto BUILD_BOTH
if "%choice%"=="4" goto BUILD_PATCHER
if "%choice%"=="r" goto RESET_CONFIG
if "%choice%"=="q" exit
goto MENU

:BUILD_64
echo.
echo [!] Initializing 64-bit Compilation Environment...
set "PATH=%W64_BIN_PATH%;%PATH%"
set "MAKE_CMD=make"
if exist "%W64_BIN_PATH%\mingw32-make.exe" set "MAKE_CMD=mingw32-make"
"%MAKE_CMD%" OUT=pycity-win64.exe RAYLIB_DIR=raylib64/src
goto END

:BUILD_32
echo.
echo [!] Initializing 32-bit Compilation Environment...
set "PATH=%W32_BIN_PATH%;%PATH%"
set "MAKE_CMD=make"
if exist "%W32_BIN_PATH%\mingw32-make.exe" set "MAKE_CMD=mingw32-make"
"%MAKE_CMD%" OUT=pycity-win32.exe RAYLIB_DIR=raylib32/src
goto END

:BUILD_BOTH
echo.
echo [!] Launching Parallel Dual-Architecture Build Threads...
set "MAKE_64=make"
if exist "%W64_BIN_PATH%\mingw32-make.exe" set "MAKE_64=mingw32-make"
set "MAKE_32=make"
if exist "%W32_BIN_PATH%\mingw32-make.exe" set "MAKE_32=mingw32-make"

start /b cmd /c "set "PATH=%W64_BIN_PATH%;%%PATH%%" && "%MAKE_64%" OUT=pycity-win64.exe RAYLIB_DIR=raylib64/src"
start /b cmd /c "set "PATH=%W32_BIN_PATH%;%%PATH%%" && "%MAKE_32%" OUT=pycity-win32.exe RAYLIB_DIR=raylib32/src"
timeout /t 2 >nul
goto END

:BUILD_PATCHER
echo.
echo [!] Building Patcher.exe (32-bit, so it runs on any Windows install)...
:: Patcher.c only needs wininet + Windows system headers - no raylib, no
:: Makefile involved, so this is just a direct gcc call. Built 32-bit
:: deliberately (via the w32devkit toolchain) since a 32-bit exe runs on
:: both 32-bit and 64-bit Windows via WOW64, and the patcher itself
:: doesn't need 64-bit performance - one Patcher.exe covers every install
:: regardless of which game architecture (win32/win64) it's updating.
set "PATH=%W32_BIN_PATH%;%PATH%"
gcc Patcher/patcher.c -o Patcher.exe -lwininet
goto END

:RESET_CONFIG
del "%CONFIG_FILE%"
echo.
echo [+] Configuration file deleted. Restarting setup wizard...
echo.
pause
cls
goto INITIAL_SETUP

:END
echo.
echo ========================================================
echo Build process triggered!
echo ========================================================
pause
cls
goto MENU