// PyCity Auto-Updater
//
// Checks the latest GitHub release, and if it's newer than what's recorded
// locally in version.txt, downloads it, extracts it into a staging folder,
// verifies the extraction actually worked, and only THEN replaces the old
// files - so a failed download or a bad zip can never leave the player with
// a half-deleted game.
//
// Build (from an MSYS2/MinGW or Visual Studio dev shell):
//   gcc patcher.c -o patcher.exe -lwininet
//
// Expects to sit next to pycity-win64.exe or pycity-win32.exe in the
// game's install folder (whichever one is actually installed - the
// patcher detects which by checking which file is present).

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <wininet.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#pragma comment(lib, "wininet.lib")

// ---- Configuration ----
#define TARGET_PROCESS_64 "pycity-win64.exe"
#define TARGET_PROCESS_32 "pycity-win32.exe"
#define VERSION_FILE     "version.txt"
#define ZIP_PACKAGE      "update_package.zip"
#define STAGING_DIR      "update_staging"
#define RELEASE_API_URL  "https://api.github.com/repos/pixel-pulse-games/pycity/releases/latest"
#define RELEASE_ZIP_URL_64 "https://github.com/pixel-pulse-games/pycity/releases/latest/download/pycity-win64.zip"
#define RELEASE_ZIP_URL_32 "https://github.com/pixel-pulse-games/pycity/releases/latest/download/pycity-win32.zip"
#define USER_AGENT       "PyCity-AutoUpdater/1.0"

// ---- Process check ----
static bool IsGameRunning(const char *processName) {
    bool isRunning = false;
    PROCESSENTRY32 entry;
    entry.dwSize = sizeof(PROCESSENTRY32);

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    if (Process32First(snapshot, &entry)) {
        do {
            if (_stricmp(entry.szExeFile, processName) == 0) {
                isRunning = true;
                break;
            }
        } while (Process32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return isRunning;
}

// ---- Small helper: fetch a URL fully into a heap buffer ----
// InternetReadFile can return partial reads, so this loops until the
// server signals EOF (bytesRead == 0), growing the buffer as needed,
// instead of trusting everything to arrive in one fixed-size chunk.
static char *FetchUrlToBuffer(HINTERNET hInternet, const char *url, const char *headers, size_t *outLen) {
    HINTERNET hUrl = InternetOpenUrlA(hInternet, url, headers, headers ? (DWORD)strlen(headers) : 0,
                                       INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!hUrl) return NULL;

    size_t capacity = 8192;
    size_t used = 0;
    char *buffer = (char *)malloc(capacity);
    if (!buffer) { InternetCloseHandle(hUrl); return NULL; }

    char chunk[4096];
    DWORD bytesRead = 0;
    while (InternetReadFile(hUrl, chunk, sizeof(chunk), &bytesRead) && bytesRead > 0) {
        if (used + bytesRead + 1 > capacity) {
            capacity *= 2;
            char *grown = (char *)realloc(buffer, capacity);
            if (!grown) { free(buffer); InternetCloseHandle(hUrl); return NULL; }
            buffer = grown;
        }
        memcpy(buffer + used, chunk, bytesRead);
        used += bytesRead;
    }
    buffer[used] = '\0';
    InternetCloseHandle(hUrl);

    if (outLen) *outLen = used;
    return buffer;
}

// ---- Tiny JSON field grabber ----
// Not a real JSON parser - just enough to pull "tag_name": "vX.Y.Z" out of
// the GitHub API response, which is all we need it for.
static bool ExtractJsonStringField(const char *json, const char *key, char *out, size_t outCap) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *keyPos = strstr(json, pattern);
    if (!keyPos) return false;

    const char *colon = strchr(keyPos, ':');
    if (!colon) return false;

    const char *valueStart = strchr(colon, '\"');
    if (!valueStart) return false;
    valueStart++; // skip opening quote

    const char *valueEnd = strchr(valueStart, '\"');
    if (!valueEnd) return false;

    size_t len = (size_t)(valueEnd - valueStart);
    if (len >= outCap) len = outCap - 1;
    memcpy(out, valueStart, len);
    out[len] = '\0';
    return true;
}

static bool ReadLocalVersion(char *out, size_t outCap) {
    FILE *f = fopen(VERSION_FILE, "r");
    if (!f) return false;
    bool ok = (fgets(out, (int)outCap, f) != NULL);
    fclose(f);
    if (ok) {
        // strip trailing newline
        size_t len = strlen(out);
        while (len > 0 && (out[len-1] == '\n' || out[len-1] == '\r')) out[--len] = '\0';
    }
    return ok;
}

static void WriteLocalVersion(const char *version) {
    FILE *f = fopen(VERSION_FILE, "w");
    if (!f) return;
    fputs(version, f);
    fclose(f);
}

// Runs a command and waits for it, returning its exit code (or -1 if it
// couldn't even be launched). Window hidden so there's no console flash.
static int RunCommandAndWait(char *commandLine) {
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    if (!CreateProcessA(NULL, commandLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)exitCode;
}

static bool FileExists(const char *path) {
    DWORD attrib = GetFileAttributesA(path);
    return (attrib != INVALID_FILE_ATTRIBUTES) && !(attrib & FILE_ATTRIBUTE_DIRECTORY);
}

// ---- Architecture detection ----
// Which zip to grab depends on whether the player's *installed* game is
// the 32-bit or 64-bit build. The build's own filename already tells us
// this (pycity-win64.exe vs pycity-win32.exe), so we just check which
// one is actually sitting on disk next to the patcher - no need to go
// digging through PE headers.
typedef enum { ARCH_UNKNOWN, ARCH_X86, ARCH_X64 } GameArch;

static GameArch DetectInstalledArchitecture(void) {
    if (FileExists(TARGET_PROCESS_64)) return ARCH_X64;
    if (FileExists(TARGET_PROCESS_32)) return ARCH_X86;
    return ARCH_UNKNOWN;
}

// Best-effort fallback for the rare case neither exe is found on disk
// (e.g. this is a from-scratch install with only the patcher present).
// Guesses off the OS's native architecture - not perfect, but reasonable.
static GameArch GuessArchitectureFromOS(void) {
    SYSTEM_INFO sysInfo;
    GetNativeSystemInfo(&sysInfo);
    return (sysInfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64) ? ARCH_X64 : ARCH_X86;
}

static const char *TargetExeNameFor(GameArch arch) {
    return (arch == ARCH_X64) ? TARGET_PROCESS_64 : TARGET_PROCESS_32;
}

static void DeleteDirectoryRecursive(const char *path) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "cmd.exe /c rmdir /s /q \"%s\"", path);
    RunCommandAndWait(cmd);
}

// ---- Main update flow ----
static bool RunUpdate(GameArch arch, const char *targetExeName) {
    printf("[*] Checking latest release...\n");
    HINTERNET hInternet = InternetOpenA(USER_AGENT, INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (!hInternet) {
        printf("[ERROR] Failed to initialize internet interface.\n");
        return false;
    }

    // GitHub's API requires a User-Agent header on every request, or it
    // returns 403 - this is the #1 reason a "works in the browser" URL
    // fails from a bare WinINet call.
    char apiHeaders[128];
    snprintf(apiHeaders, sizeof(apiHeaders), "User-Agent: %s\r\n", USER_AGENT);

    size_t jsonLen = 0;
    char *json = FetchUrlToBuffer(hInternet, RELEASE_API_URL, apiHeaders, &jsonLen);
    if (!json) {
        printf("[ERROR] Failed to reach GitHub releases API.\n");
        InternetCloseHandle(hInternet);
        return false;
    }

    char latestVersion[64] = { 0 };
    if (!ExtractJsonStringField(json, "tag_name", latestVersion, sizeof(latestVersion))) {
        printf("[ERROR] Could not find a version tag in the release response.\n");
        free(json);
        InternetCloseHandle(hInternet);
        return false;
    }
    free(json);

    char localVersion[64] = { 0 };
    bool haveLocalVersion = ReadLocalVersion(localVersion, sizeof(localVersion));

    printf("[*] Latest version: %s\n", latestVersion);
    printf("[*] Installed version: %s\n", haveLocalVersion ? localVersion : "(unknown)");

    if (haveLocalVersion && strcmp(localVersion, latestVersion) == 0) {
        printf("[OK] Already up to date.\n");
        InternetCloseHandle(hInternet);
        return true;
    }

    const char *releaseZipUrl = (arch == ARCH_X64) ? RELEASE_ZIP_URL_64 : RELEASE_ZIP_URL_32;

    // ---- Download ----
    printf("[*] Downloading %s ...\n", releaseZipUrl);
    HINTERNET hDownload = InternetOpenUrlA(hInternet, releaseZipUrl, NULL, 0,
                                            INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!hDownload) {
        printf("[ERROR] Failed to start download.\n");
        InternetCloseHandle(hInternet);
        return false;
    }

    FILE *zipFile = fopen(ZIP_PACKAGE, "wb");
    if (!zipFile) {
        printf("[ERROR] Failed to open '%s' for writing.\n", ZIP_PACKAGE);
        InternetCloseHandle(hDownload);
        InternetCloseHandle(hInternet);
        return false;
    }

    char streamBuffer[8192];
    DWORD chunkBytesRead = 0;
    long long totalBytes = 0;
    while (InternetReadFile(hDownload, streamBuffer, sizeof(streamBuffer), &chunkBytesRead) && chunkBytesRead > 0) {
        fwrite(streamBuffer, 1, chunkBytesRead, zipFile);
        totalBytes += chunkBytesRead;
    }
    fclose(zipFile);
    InternetCloseHandle(hDownload);
    InternetCloseHandle(hInternet);

    // A 0-byte download means something went wrong upstream (redirect
    // not followed, connection dropped, etc.) - bail out before we touch
    // anything on disk.
    if (totalBytes == 0) {
        printf("[ERROR] Downloaded file is empty - aborting, nothing was touched.\n");
        DeleteFileA(ZIP_PACKAGE);
        return false;
    }
    printf("[OK] Downloaded %lld bytes.\n", totalBytes);

    // ---- Extract to a staging folder first ----
    // Nothing about the live install is touched yet. If any of this
    // fails, we bail out and the player's existing game is untouched.
    DeleteDirectoryRecursive(STAGING_DIR); // clean slate in case of a leftover from a previous failed run
    CreateDirectoryA(STAGING_DIR, NULL);

    printf("[*] Extracting update...\n");
    char tarCmd[256];
    snprintf(tarCmd, sizeof(tarCmd), "tar.exe -xf %s -C %s", ZIP_PACKAGE, STAGING_DIR);
    int tarExit = RunCommandAndWait(tarCmd);
    if (tarExit != 0) {
        printf("[ERROR] Extraction failed (tar exit code %d). Update aborted, nothing was touched.\n", tarExit);
        DeleteFileA(ZIP_PACKAGE);
        DeleteDirectoryRecursive(STAGING_DIR);
        return false;
    }

    // Sanity check: make sure the thing we're about to install as "the
    // game" actually contains the game.
    char stagedExePath[MAX_PATH];
    snprintf(stagedExePath, sizeof(stagedExePath), "%s\\%s", STAGING_DIR, targetExeName);
    if (!FileExists(stagedExePath)) {
        printf("[ERROR] Extracted update doesn't contain %s. Update aborted, nothing was touched.\n", targetExeName);
        DeleteFileA(ZIP_PACKAGE);
        DeleteDirectoryRecursive(STAGING_DIR);
        return false;
    }
    printf("[OK] Extraction verified.\n");

    // ---- Only now: replace the live files ----
    // xcopy /E /I /Y recursively overlays the staged files onto the
    // current directory, overwriting anything with the same name.
    printf("[*] Installing update...\n");
    char copyCmd[512];
    snprintf(copyCmd, sizeof(copyCmd), "cmd.exe /c xcopy \"%s\\*\" \".\" /E /I /Y", STAGING_DIR);
    int copyExit = RunCommandAndWait(copyCmd);

    DeleteFileA(ZIP_PACKAGE);
    DeleteDirectoryRecursive(STAGING_DIR);

    if (copyExit != 0 || !FileExists(targetExeName)) {
        printf("[ERROR] Installing the update failed. Your existing files were not deleted beforehand,\n");
        printf("        but the update did not apply cleanly - please try again or update manually.\n");
        return false;
    }

    WriteLocalVersion(latestVersion);
    printf("[SUCCESS] Updated to %s.\n", latestVersion);
    return true;
}

int main(void) {
    printf("=== PyCity Auto-Updater ===\n\n");

    GameArch arch = DetectInstalledArchitecture();
    if (arch == ARCH_UNKNOWN) {
        arch = GuessArchitectureFromOS();
        printf("[*] Neither %s nor %s found here - guessing %s from the OS.\n",
               TARGET_PROCESS_64, TARGET_PROCESS_32, (arch == ARCH_X64) ? "64-bit" : "32-bit");
    } else {
        printf("[*] Installed build detected: %s\n", (arch == ARCH_X64) ? "64-bit" : "32-bit");
    }
    const char *targetExeName = TargetExeNameFor(arch);

    if (IsGameRunning(targetExeName)) {
        printf("[ABORT] %s is currently running.\n", targetExeName);
        printf("        Please close the game before updating.\n\n");
        system("pause");
        return 1;
    }

    bool ok = RunUpdate(arch, targetExeName);

    printf("\n[*] Relaunching %s...\n", targetExeName);
    STARTUPINFOA siGame = { sizeof(siGame) };
    PROCESS_INFORMATION piGame;
    if (CreateProcessA(targetExeName, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &siGame, &piGame)) {
        CloseHandle(piGame.hProcess);
        CloseHandle(piGame.hThread);
    } else {
        printf("[ERROR] Could not relaunch %s - is it in this folder?\n", targetExeName);
    }

    if (!ok) {
        system("pause");
    }
    return ok ? 0 : 1;
}