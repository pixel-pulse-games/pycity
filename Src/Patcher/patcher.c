// PyCity Auto-Updater
//
// Checks the latest GitHub release, and if it's newer than what's recorded
// locally in version.txt, downloads it, verifies its SHA256 against the
// checksums.txt asset published alongside the release, extracts it into a
// staging folder, verifies the extraction actually worked, and only THEN
// replaces the old files - so a failed download, a corrupted/tampered zip,
// or a bad extraction can never leave the player with a half-deleted game.
//
// Note: version.txt (local, patcher-maintained, tracks installed version)
// and checksums.txt (remote release asset, holds per-arch SHA256 hashes)
// are two different files with two different jobs - don't confuse them.
//
// Build (from an MSYS2/MinGW or Visual Studio dev shell):
//   windres patcher.rc -O coff -o patcher_res.o
//   gcc patcher.c patcher_res.o miniz.c -o patcher.exe ^
//       -lwininet -ladvapi32
//
// Zip extraction uses miniz (public domain, MIT-equivalent - see
// miniz.h for the license) instead of shelling out to tar.exe,
// since tar.exe is only guaranteed present on Windows 10 1803 and newer.
//
// The windres step embeds patcher.manifest (asInvoker) into the exe.
// Skipping it means Windows' installer-detection heuristic will silently
// auto-elevate this exe via UAC, since it's a 32-bit binary whose name
// contains "patch" - see patcher.manifest for details.
//
// Expects to sit next to pycity-win64.exe or pycity-win32.exe in the
// game's install folder (whichever one is actually installed - the
// patcher detects which by checking which file is present).

#define _CRT_SECURE_NO_WARNINGS
// CALG_SHA_256 is a Vista-era constant. MinGW's wincrypt.h only exposes it
// when the target Windows version is declared as Vista or newer, so these
// must be defined before windows.h (and therefore wincrypt.h) is pulled in.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef WINVER
#define WINVER 0x0600
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x06000000
#endif
#include <windows.h>
#include <wininet.h>
#include <wincrypt.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "advapi32.lib")

// Zip extraction via miniz instead of shelling out to tar.exe. tar.exe is
// only guaranteed present on Windows 10 1803+ - miniz ships with the exe
// so extraction works on any supported Windows version regardless of what
// system tools happen to be on PATH.
#include "miniz.h"

// ---- Configuration ----
#define TARGET_PROCESS_64 "pycity-win64.exe"
#define TARGET_PROCESS_32 "pycity-win32.exe"
#define VERSION_FILE     "version.txt"
#define ZIP_PACKAGE      "update_package.zip"
#define STAGING_DIR      "update_staging"
#define RELEASE_API_URL  "https://api.github.com/repos/pixel-pulse-games/pycity/releases/latest"
#define RELEASE_ZIP_URL_64 "https://github.com/pixel-pulse-games/pycity/releases/latest/download/pycity-win64.zip"
#define RELEASE_ZIP_URL_32 "https://github.com/pixel-pulse-games/pycity/releases/latest/download/pycity-win32.zip"
#define CHECKSUMS_URL    "https://github.com/pixel-pulse-games/pycity/releases/latest/download/checksums.txt"
#define ARCH_KEY_64      "win64"
#define ARCH_KEY_32      "win32"
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

// ---- SHA256 verification ----
// Uses Windows' built-in CryptoAPI (advapi32) so no external hashing
// library needs to ship with the patcher.

// Hashes a file on disk, writing a lowercase 64-char hex digest (+ NUL)
// into out_hex. Returns false on any failure (missing file, API error).
static bool Sha256File(const char *path, char out_hex[65]) {
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    BYTE buffer[8192];
    DWORD bytesRead;
    BYTE hash[32];
    DWORD hashLen = sizeof(hash);
    bool ok = false;

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    if (!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        goto cleanup;
    if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash))
        goto cleanup;

    while ((bytesRead = (DWORD)fread(buffer, 1, sizeof(buffer), f)) > 0) {
        if (!CryptHashData(hHash, buffer, bytesRead, 0))
            goto cleanup;
    }

    if (!CryptGetHashParam(hHash, HP_HASHVAL, hash, &hashLen, 0))
        goto cleanup;

    for (DWORD i = 0; i < hashLen; i++)
        sprintf(out_hex + i * 2, "%02x", hash[i]);
    out_hex[hashLen * 2] = '\0';
    ok = true;

cleanup:
    if (hHash) CryptDestroyHash(hHash);
    if (hProv) CryptReleaseContext(hProv, 0);
    fclose(f);
    return ok;
}

// Parses "key=hexhash" lines (as published in the checksums.txt release
// asset) looking for archKey (e.g. "win64"). Returns false if the file
// can't be read or the key isn't present.
static bool GetExpectedHashForArch(const char *checksumsText, const char *archKey, char out_hex[65]) {
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "%s=", archKey);

    const char *line = checksumsText;
    size_t patLen = strlen(pattern);
    while (line && *line) {
        if (strncmp(line, pattern, patLen) == 0) {
            const char *valueStart = line + patLen;
            size_t len = 0;
            while (valueStart[len] && valueStart[len] != '\r' && valueStart[len] != '\n' && len < 64) len++;
            memcpy(out_hex, valueStart, len);
            out_hex[len] = '\0';
            return len == 64; // a SHA256 hex digest is always exactly 64 chars
        }
        const char *next = strchr(line, '\n');
        if (!next) break;
        line = next + 1;
    }
    return false;
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

// ---- Zip extraction (miniz) ----

// Creates every directory along `path` that doesn't already exist, e.g.
// given "update_staging\\pycity-win64\\assets\\base" it creates
// "update_staging", then "...\\pycity-win64", then "...\\assets", then
// "...\\base". Needed because zip entries can be several folders deep and
// CreateDirectoryA only ever makes one level at a time.
static void CreateDirectoriesRecursive(const char *path) {
    char buffer[MAX_PATH];
    size_t len = strlen(path);
    if (len >= sizeof(buffer)) len = sizeof(buffer) - 1;
    memcpy(buffer, path, len);
    buffer[len] = '\0';

    for (size_t i = 0; i < len; i++) {
        if (buffer[i] == '/') buffer[i] = '\\';
    }

    for (size_t i = 1; i < len; i++) {
        if (buffer[i] == '\\') {
            buffer[i] = '\0';
            CreateDirectoryA(buffer, NULL); // ignore failure - "already exists" is fine
            buffer[i] = '\\';
        }
    }
    CreateDirectoryA(buffer, NULL);
}

// Extracts every entry in zipPath into destDir, recreating the folder
// structure stored in the zip. Returns false on any failure (bad archive,
// a file that can't be written, etc.) - the caller treats that exactly
// like the old tar-exit-code check: abort, don't touch the live install.
static bool ExtractZipToDirectory(const char *zipPath, const char *destDir) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));

    if (!mz_zip_reader_init_file(&zip, zipPath, 0)) {
        return false;
    }

    bool ok = true;
    mz_uint fileCount = mz_zip_reader_get_num_files(&zip);

    for (mz_uint i = 0; i < fileCount && ok; i++) {
        mz_zip_archive_file_stat fileStat;
        if (!mz_zip_reader_file_stat(&zip, i, &fileStat)) {
            ok = false;
            break;
        }

        char destPath[MAX_PATH];
        snprintf(destPath, sizeof(destPath), "%s\\%s", destDir, fileStat.m_filename);
        for (char *p = destPath; *p; p++) {
            if (*p == '/') *p = '\\';
        }

        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            CreateDirectoriesRecursive(destPath);
            continue;
        }

        // Make sure the parent folder exists before writing the file into it.
        char parentDir[MAX_PATH];
        strncpy(parentDir, destPath, sizeof(parentDir) - 1);
        parentDir[sizeof(parentDir) - 1] = '\0';
        char *lastSlash = strrchr(parentDir, '\\');
        if (lastSlash) {
            *lastSlash = '\0';
            CreateDirectoriesRecursive(parentDir);
        }

        if (!mz_zip_reader_extract_to_file(&zip, i, destPath, 0)) {
            ok = false;
            break;
        }
    }

    mz_zip_reader_end(&zip);
    return ok;
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

    // ---- Verify checksum before touching anything else ----
    // A mismatch here (corrupted download, or a tampered/MITM'd zip) is
    // treated exactly like a failed extraction below: bail out, leave the
    // live install untouched, don't advance version.txt.
    {
        printf("[*] Verifying checksum...\n");
        HINTERNET hVerify = InternetOpenA(USER_AGENT, INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
        if (!hVerify) {
            printf("[ERROR] Failed to initialize internet interface for checksum fetch.\n");
            DeleteFileA(ZIP_PACKAGE);
            return false;
        }
        char verifyHeaders[128];
        snprintf(verifyHeaders, sizeof(verifyHeaders), "User-Agent: %s\r\n", USER_AGENT);

        size_t checksumsLen = 0;
        char *checksumsText = FetchUrlToBuffer(hVerify, CHECKSUMS_URL, verifyHeaders, &checksumsLen);
        InternetCloseHandle(hVerify);
        if (!checksumsText) {
            printf("[ERROR] Failed to download checksums.txt - aborting, nothing was touched.\n");
            DeleteFileA(ZIP_PACKAGE);
            return false;
        }

        const char *archKey = (arch == ARCH_X64) ? ARCH_KEY_64 : ARCH_KEY_32;
        char expectedHash[65] = { 0 };
        bool haveExpected = GetExpectedHashForArch(checksumsText, archKey, expectedHash);
        free(checksumsText);

        if (!haveExpected) {
            printf("[ERROR] No checksum entry for '%s' in checksums.txt - aborting, nothing was touched.\n", archKey);
            DeleteFileA(ZIP_PACKAGE);
            return false;
        }

        char actualHash[65] = { 0 };
        if (!Sha256File(ZIP_PACKAGE, actualHash)) {
            printf("[ERROR] Could not hash the downloaded file - aborting, nothing was touched.\n");
            DeleteFileA(ZIP_PACKAGE);
            return false;
        }

        if (_stricmp(expectedHash, actualHash) != 0) {
            printf("[ERROR] Checksum mismatch! Expected %s, got %s.\n", expectedHash, actualHash);
            printf("        Download may be corrupted or tampered with - aborting, nothing was touched.\n");
            DeleteFileA(ZIP_PACKAGE);
            return false;
        }
        printf("[OK] Checksum verified.\n");
    }

    // ---- Extract to a staging folder first ----
    // Nothing about the live install is touched yet. If any of this
    // fails, we bail out and the player's existing game is untouched.
    DeleteDirectoryRecursive(STAGING_DIR); // clean slate in case of a leftover from a previous failed run
    CreateDirectoryA(STAGING_DIR, NULL);

    printf("[*] Extracting update...\n");
    if (!ExtractZipToDirectory(ZIP_PACKAGE, STAGING_DIR)) {
        printf("[ERROR] Extraction failed. Update aborted, nothing was touched.\n");
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