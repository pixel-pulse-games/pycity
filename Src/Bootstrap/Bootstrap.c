// PyCity Patcher Bootstrapper
//
// One job: fetch the latest Patcher.zip, verify it, and install it in place
// of the running Patcher.exe. This exists because Patcher.exe can't
// overwrite itself while it's the running process - Windows won't allow
// that. The bootstrapper is a separate exe, so it CAN overwrite
// Patcher.exe (it's not the thing being replaced), which is the whole
// reason this file exists instead of just adding more code to patcher.c.
//
// Flow:
//   1. Download checksums.txt and Patcher.zip from the latest release
//   2. Hash the downloaded zip (SHA256, via CryptoAPI) and compare against
//      the "patcher=" line in checksums.txt - abort if missing or mismatched
//   3. Extract Patcher.zip (via miniz) into a staging folder, confirm
//      Patcher.exe is actually in there
//   4. Wait for the currently-running Patcher.exe to fully exit
//   5. Delete the old Patcher.exe, move the staged one into its place
//   6. Relaunch Patcher.exe, then exit
//
// Nothing about the live Patcher.exe is touched until steps 1-3 all
// succeed - same "verify and stage first" principle patcher.c itself uses
// for game updates.
//
// Build (from an MSYS2/MinGW dev shell):
//   gcc bootstrap.c miniz.c -o Bootstrap.exe -lwininet -ladvapi32
//
// Requires a "patcher=<sha256>" line added to checksums.txt (see the
// PowerShell snippet in the handoff doc) alongside the existing
// "win64="/"win32=" lines - one more hash to generate at release time,
// for Patcher.zip itself.

#define _CRT_SECURE_NO_WARNINGS
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

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "advapi32.lib")
#include "miniz.h"

#define CHECKSUMS_URL "https://github.com/pixel-pulse-games/pycity/releases/latest/download/checksums.txt"
#define PATCHER_ZIP_URL "https://github.com/pixel-pulse-games/pycity/releases/latest/download/Patcher.zip"
#define USER_AGENT "PyCity-Bootstrap/1.0"

#define ZIP_PACKAGE "bootstrap_download.zip"
#define STAGING_DIR "bootstrap_staging"
#define OLD_EXE "Patcher.exe"
#define WAIT_TIMEOUT_MS 10000
#define POLL_INTERVAL_MS 200

// ---- Download helpers ----

// Downloads a URL into a heap buffer (caller frees it). Returns NULL on
// any failure. outLen receives the byte count on success.
static char *DownloadUrlToBuffer(const char *url, size_t *outLen) {
    HINTERNET hInternet = InternetOpenA(USER_AGENT, INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (!hInternet) return NULL;

    char headers[128];
    snprintf(headers, sizeof(headers), "User-Agent: %s\r\n", USER_AGENT);

    HINTERNET hUrl = InternetOpenUrlA(hInternet, url, headers, (DWORD)strlen(headers), INTERNET_FLAG_RELOAD, 0);
    if (!hUrl) {
        InternetCloseHandle(hInternet);
        return NULL;
    }

    size_t capacity = 65536, len = 0;
    char *buffer = (char *)malloc(capacity);
    if (!buffer) {
        InternetCloseHandle(hUrl);
        InternetCloseHandle(hInternet);
        return NULL;
    }

    DWORD bytesRead;
    char chunk[8192];
    while (InternetReadFile(hUrl, chunk, sizeof(chunk), &bytesRead) && bytesRead > 0) {
        if (len + bytesRead + 1 > capacity) {
            capacity *= 2;
            char *grown = (char *)realloc(buffer, capacity);
            if (!grown) {
                free(buffer);
                InternetCloseHandle(hUrl);
                InternetCloseHandle(hInternet);
                return NULL;
            }
            buffer = grown;
        }
        memcpy(buffer + len, chunk, bytesRead);
        len += bytesRead;
    }
    buffer[len] = '\0';

    InternetCloseHandle(hUrl);
    InternetCloseHandle(hInternet);

    if (outLen) *outLen = len;
    return buffer;
}

// Downloads a URL straight to a file on disk. Returns false on any failure.
static bool DownloadUrlToFile(const char *url, const char *destPath) {
    HINTERNET hInternet = InternetOpenA(USER_AGENT, INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (!hInternet) return false;

    char headers[128];
    snprintf(headers, sizeof(headers), "User-Agent: %s\r\n", USER_AGENT);

    HINTERNET hUrl = InternetOpenUrlA(hInternet, url, headers, (DWORD)strlen(headers), INTERNET_FLAG_RELOAD, 0);
    if (!hUrl) {
        InternetCloseHandle(hInternet);
        return false;
    }

    FILE *f = fopen(destPath, "wb");
    if (!f) {
        InternetCloseHandle(hUrl);
        InternetCloseHandle(hInternet);
        return false;
    }

    DWORD bytesRead;
    char chunk[8192];
    bool ok = true;
    while (InternetReadFile(hUrl, chunk, sizeof(chunk), &bytesRead) && bytesRead > 0) {
        if (fwrite(chunk, 1, bytesRead, f) != bytesRead) {
            ok = false;
            break;
        }
    }

    fclose(f);
    InternetCloseHandle(hUrl);
    InternetCloseHandle(hInternet);
    return ok;
}

// ---- SHA256 (CryptoAPI) ----

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

// Parses "key=hexhash" lines looking for "patcher". Returns false if the
// text can't be read or the key isn't present.
static bool GetExpectedPatcherHash(const char *checksumsText, char out_hex[65]) {
    const char *pattern = "patcher=";
    size_t patLen = strlen(pattern);

    const char *line = checksumsText;
    while (line && *line) {
        if (strncmp(line, pattern, patLen) == 0) {
            const char *valueStart = line + patLen;
            size_t len = 0;
            while (valueStart[len] && valueStart[len] != '\r' && valueStart[len] != '\n' && len < 64) len++;
            memcpy(out_hex, valueStart, len);
            out_hex[len] = '\0';
            return len == 64;
        }
        const char *next = strchr(line, '\n');
        if (!next) break;
        line = next + 1;
    }
    return false;
}

// ---- Zip extraction (miniz) ----

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
            CreateDirectoryA(buffer, NULL);
            buffer[i] = '\\';
        }
    }
    CreateDirectoryA(buffer, NULL);
}

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

static void DeleteDirectoryRecursive(const char *path) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "cmd.exe /c rmdir /s /q \"%s\"", path);
    system(cmd);
}

// ---- Process wait ----

static bool IsProcessRunning(const char *exeName) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32 entry;
    entry.dwSize = sizeof(PROCESSENTRY32);
    bool found = false;

    if (Process32First(snapshot, &entry)) {
        do {
            if (_stricmp(entry.szExeFile, exeName) == 0) {
                found = true;
                break;
            }
        } while (Process32Next(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return found;
}

int main(void) {
    // ---- Step 1: download checksums.txt and Patcher.zip ----
    printf("[Bootstrap] Downloading checksums.txt...\n");
    size_t checksumsLen = 0;
    char *checksumsText = DownloadUrlToBuffer(CHECKSUMS_URL, &checksumsLen);
    if (!checksumsText) {
        printf("[Bootstrap] ERROR: Could not download checksums.txt. Aborting - nothing was touched.\n");
        return 1;
    }

    char expectedHash[65] = { 0 };
    bool haveExpected = GetExpectedPatcherHash(checksumsText, expectedHash);
    free(checksumsText);

    if (!haveExpected) {
        printf("[Bootstrap] ERROR: No 'patcher=' entry in checksums.txt. Aborting - nothing was touched.\n");
        return 1;
    }

    printf("[Bootstrap] Downloading Patcher.zip...\n");
    if (!DownloadUrlToFile(PATCHER_ZIP_URL, ZIP_PACKAGE)) {
        printf("[Bootstrap] ERROR: Could not download Patcher.zip. Aborting - nothing was touched.\n");
        return 1;
    }

    // ---- Step 2: verify checksum before touching anything else ----
    printf("[Bootstrap] Verifying checksum...\n");
    char actualHash[65] = { 0 };
    if (!Sha256File(ZIP_PACKAGE, actualHash)) {
        printf("[Bootstrap] ERROR: Could not hash the downloaded file. Aborting.\n");
        DeleteFileA(ZIP_PACKAGE);
        return 1;
    }

    if (_stricmp(expectedHash, actualHash) != 0) {
        printf("[Bootstrap] ERROR: Checksum mismatch! Expected %s, got %s.\n", expectedHash, actualHash);
        printf("            Download may be corrupted or tampered with. Aborting - nothing was touched.\n");
        DeleteFileA(ZIP_PACKAGE);
        return 1;
    }
    printf("[Bootstrap] Checksum verified.\n");

    // ---- Step 3: extract to staging and confirm the exe is actually there ----
    printf("[Bootstrap] Extracting...\n");
    DeleteDirectoryRecursive(STAGING_DIR);
    if (!ExtractZipToDirectory(ZIP_PACKAGE, STAGING_DIR)) {
        printf("[Bootstrap] ERROR: Extraction failed. Aborting - nothing was touched.\n");
        DeleteFileA(ZIP_PACKAGE);
        DeleteDirectoryRecursive(STAGING_DIR);
        return 1;
    }

    char stagedExePath[MAX_PATH];
    snprintf(stagedExePath, sizeof(stagedExePath), "%s\\%s", STAGING_DIR, OLD_EXE);
    if (GetFileAttributesA(stagedExePath) == INVALID_FILE_ATTRIBUTES) {
        printf("[Bootstrap] ERROR: %s not found after extraction. Aborting - nothing was touched.\n", OLD_EXE);
        DeleteFileA(ZIP_PACKAGE);
        DeleteDirectoryRecursive(STAGING_DIR);
        return 1;
    }
    DeleteFileA(ZIP_PACKAGE);

    // ---- Step 4: wait for the running Patcher.exe to exit ----
    printf("[Bootstrap] Waiting for %s to close...\n", OLD_EXE);
    DWORD waited = 0;
    while (IsProcessRunning(OLD_EXE) && waited < WAIT_TIMEOUT_MS) {
        Sleep(POLL_INTERVAL_MS);
        waited += POLL_INTERVAL_MS;
    }
    if (IsProcessRunning(OLD_EXE)) {
        printf("[Bootstrap] ERROR: %s did not close in time. Aborting - nothing was touched.\n", OLD_EXE);
        DeleteDirectoryRecursive(STAGING_DIR);
        return 1;
    }

    // ---- Step 5: swap the files ----
    if (!DeleteFileA(OLD_EXE)) {
        DWORD err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND) {
            printf("[Bootstrap] ERROR: Could not delete old %s (error %lu). Aborting.\n", OLD_EXE, err);
            DeleteDirectoryRecursive(STAGING_DIR);
            return 1;
        }
    }

    if (!MoveFileA(stagedExePath, OLD_EXE)) {
        printf("[Bootstrap] ERROR: Could not move the new %s into place (error %lu).\n", OLD_EXE, GetLastError());
        printf("            %s is now MISSING - copy it manually from %s.\n", OLD_EXE, stagedExePath);
        return 1;
    }

    printf("[Bootstrap] Installed successfully. Relaunching...\n");
    DeleteDirectoryRecursive(STAGING_DIR);

    // ---- Step 6: relaunch ----
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (CreateProcessA(OLD_EXE, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        printf("[Bootstrap] WARNING: Installed but couldn't relaunch (error %lu). Just run %s manually.\n", GetLastError(), OLD_EXE);
    }

    return 0;
}