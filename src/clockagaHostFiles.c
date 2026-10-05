/*
 * ClockAGA host file hooks for transpiled CALL sites:
 *   loadDirectory(buffer, maxEntries, pathSize)
 *   loadFile(path, buffer, maxBytes)
 *   saveFile(path, buffer, numBytes)
 *
 * Level packs live in the user settings folder as <name>.pack under
 * .../ClockwiserPortable/ClockwiserPortable/ (org/app pref path).
 * Host files use lowercase names; the Amiga side sees uppercase.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "clockagaHostFiles.h"

#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#elif __has_include(<SDL.h>)
#include <SDL.h>
#else
#error "SDL headers not found"
#endif

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static char settingsDir[512];
static int settingsDirReady;

static void logFileHook(char *fmt, ...) {
    va_list args;

    fprintf(stderr, "[clockagaHostFiles] ");
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    fflush(stderr);
}

static void logBufferSlots(uint8_t *buffer, uint32_t slotCount,
                           uint32_t pathSize) {
    uint32_t i;

    for (i = 0u; i < slotCount; i++) {
        char *slotText = (char *)(buffer + i * pathSize);
        logFileHook("  slot %u: \"%.*s\"", i, (int)pathSize, slotText);
    }
}

static int pathEndsWithSep(char *path) {
    size_t len;

    if (path == NULL) {
        return 0;
    }
    len = strlen(path);
    if (len == 0u) {
        return 0;
    }
    return path[len - 1u] == '/' || path[len - 1u] == '\\';
}

static void ensureSettingsDir(void) {
    char *prefPath;

    if (settingsDirReady) {
        return;
    }

    settingsDir[0] = '\0';
    prefPath = SDL_GetPrefPath("ClockwiserPortable", "ClockwiserPortable");
    if (prefPath != NULL && prefPath[0] != '\0') {
        strncpy(settingsDir, prefPath, sizeof(settingsDir) - 1u);
        settingsDir[sizeof(settingsDir) - 1u] = '\0';
        SDL_free(prefPath);
    }
    if (settingsDir[0] == '\0') {
        strncpy(settingsDir, "ClockwiserPortable/ClockwiserPortable/",
                sizeof(settingsDir) - 1u);
        settingsDir[sizeof(settingsDir) - 1u] = '\0';
    }

#if defined(_WIN32)
    _mkdir(settingsDir);
#else
    mkdir(settingsDir, 0755);
#endif

    settingsDirReady = 1;
    logFileHook("settings folder ready: %s", settingsDir);
}

static void asciiToUpperInPlace(char *text) {
    size_t i;

    if (text == NULL) {
        return;
    }
    for (i = 0u; text[i] != '\0'; i++) {
        if (text[i] >= 'a' && text[i] <= 'z') {
            text[i] = (char)(text[i] - 'a' + 'A');
        }
    }
}

static void asciiToLowerInPlace(char *text) {
    size_t i;

    if (text == NULL) {
        return;
    }
    for (i = 0u; text[i] != '\0'; i++) {
        if (text[i] >= 'A' && text[i] <= 'Z') {
            text[i] = (char)(text[i] - 'A' + 'a');
        }
    }
}

static int nameIsPackFile(char *name) {
    size_t len;
    size_t extLen = 5u;

    if (name == NULL) {
        return 0;
    }
    len = strlen(name);
    if (len < extLen + 1u) {
        return 0;
    }
#if defined(_WIN32)
    if (_strnicmp(name + len - extLen, ".pack", extLen) != 0) {
        return 0;
    }
#else
    if (strncasecmp(name + len - extLen, ".pack", extLen) != 0) {
        return 0;
    }
#endif
    return 1;
}

static void packBaseNameFromFile(char *fileName, char *out, size_t outSize) {
    size_t len;
    size_t extLen = 5u;
    size_t baseLen;

    if (outSize == 0u) {
        return;
    }
    out[0] = '\0';
    if (!nameIsPackFile(fileName)) {
        return;
    }
    len = strlen(fileName);
    baseLen = len - extLen;
    if (baseLen >= outSize) {
        baseLen = outSize - 1u;
    }
    memcpy(out, fileName, baseLen);
    out[baseLen] = '\0';
}

static void writeNameSlot(uint8_t *buffer, uint32_t slotIndex, uint32_t pathSize,
                          char *name) {
    uint8_t *dest;
    size_t nameLen;
    size_t copyLen;

    dest = buffer + slotIndex * pathSize;
    nameLen = strlen(name);
    copyLen = nameLen;
    if (pathSize == 0u) {
        return;
    }
    if (copyLen >= pathSize) {
        copyLen = (size_t)pathSize - 1u;
    }
    memset(dest, 0, (size_t)pathSize);
    if (copyLen > 0u) {
        memcpy(dest, name, copyLen);
    }
}

static void stripAmigaVolume(char *path) {
    char *colon;

    colon = strchr(path, ':');
    if (colon != NULL) {
        size_t tailLen = strlen(colon + 1);
        memmove(path, colon + 1, tailLen + 1u);
    }
}

/*
 * CLOCKWISER:sl3.potpouri -> potpourri
 * sl3.potpouri            -> potpourri
 */
static void extractPackSuffix(char *amigaPath, char *out, size_t outSize) {
    char work[512];
    char *namePart;

    if (outSize == 0u) {
        return;
    }
    out[0] = '\0';
    if (amigaPath == NULL || amigaPath[0] == '\0') {
        return;
    }

    strncpy(work, amigaPath, sizeof(work) - 1u);
    work[sizeof(work) - 1u] = '\0';
    stripAmigaVolume(work);

    namePart = work;
    if ((namePart[0] == 's' || namePart[0] == 'S') &&
        (namePart[1] == 'l' || namePart[1] == 'L')) {
        namePart += 2;
        while (*namePart >= '0' && *namePart <= '9') {
            namePart++;
        }
        if (*namePart == '.') {
            namePart++;
        }
    }

    strncpy(out, namePart, outSize - 1u);
    out[outSize - 1u] = '\0';
}

static void buildPackHostPath(char *out, size_t outSize, char *packSuffix) {
    size_t dirLen;

    ensureSettingsDir();
    if (outSize == 0u) {
        return;
    }
    out[0] = '\0';
    if (packSuffix == NULL || packSuffix[0] == '\0') {
        return;
    }

    dirLen = strlen(settingsDir);
    if (dirLen + strlen(packSuffix) + 6u >= outSize) {
        return;
    }
    memcpy(out, settingsDir, dirLen + 1u);
    if (!pathEndsWithSep(out)) {
        strncat(out, "/", outSize - strlen(out) - 1u);
    }
    strncat(out, packSuffix, outSize - strlen(out) - 1u);
    strncat(out, ".pack", outSize - strlen(out) - 1u);
}

static void joinDirAndName(char *out, size_t outSize, char *dir, char *name) {
    size_t dirLen;
    size_t nameLen;

    if (outSize == 0u) {
        return;
    }
    out[0] = '\0';
    if (dir == NULL || name == NULL || dir[0] == '\0' || name[0] == '\0') {
        return;
    }
    dirLen = strlen(dir);
    nameLen = strlen(name);
    if (dirLen + 1u + nameLen >= outSize) {
        return;
    }
    memcpy(out, dir, dirLen + 1u);
    if (!pathEndsWithSep(out)) {
        strncat(out, "/", outSize - strlen(out) - 1u);
    }
    strncat(out, name, outSize - strlen(out) - 1u);
}

static int hostFileExists(char *path) {
#if defined(_WIN32)
    return _access(path, 0) == 0;
#else
    return access(path, F_OK) == 0;
#endif
}

static int copyHostFile(char *srcPath, char *destPath) {
    FILE *inFile;
    FILE *outFile;
    uint8_t chunk[8192];
    size_t bytesRead;

    inFile = fopen(srcPath, "rb");
    if (inFile == NULL) {
        logFileHook("seedPack: cannot open source \"%s\"", srcPath);
        return 0;
    }
    outFile = fopen(destPath, "wb");
    if (outFile == NULL) {
        logFileHook("seedPack: cannot create \"%s\"", destPath);
        fclose(inFile);
        return 0;
    }

    while ((bytesRead = fread(chunk, 1u, sizeof(chunk), inFile)) > 0u) {
        if (fwrite(chunk, 1u, bytesRead, outFile) != bytesRead) {
            logFileHook("seedPack: write error for \"%s\"", destPath);
            fclose(inFile);
            fclose(outFile);
            return 0;
        }
    }
    if (ferror(inFile)) {
        logFileHook("seedPack: read error for \"%s\"", srcPath);
        fclose(inFile);
        fclose(outFile);
        return 0;
    }

    fclose(inFile);
    fclose(outFile);
    logFileHook("seedPack: copied \"%s\" -> \"%s\"", srcPath, destPath);
    return 1;
}

static void seedPackFileFromDataDir(char *dataDir, char *fileName) {
    char srcPath[768];
    char destPath[768];

    joinDirAndName(srcPath, sizeof(srcPath), dataDir, fileName);
    joinDirAndName(destPath, sizeof(destPath), settingsDir, fileName);
    if (hostFileExists(destPath)) {
        logFileHook("seedPack: skip existing \"%s\"", destPath);
        return;
    }
    (void)copyHostFile(srcPath, destPath);
}

void clockagaHostSeedPackFilesFromData(char *dataDir) {
    char scanDir[512];

    if (dataDir == NULL || dataDir[0] == '\0') {
        dataDir = "data";
    }
    strncpy(scanDir, dataDir, sizeof(scanDir) - 1u);
    scanDir[sizeof(scanDir) - 1u] = '\0';

    ensureSettingsDir();
    logFileHook("seedPack: scanning \"%s\" for *.pack", scanDir);

#if defined(_WIN32)
    {
        char searchPath[576];
        WIN32_FIND_DATAA findData;
        HANDLE handle;

        snprintf(searchPath, sizeof(searchPath), "%s\\*.pack", scanDir);
        handle = FindFirstFileA(searchPath, &findData);
        if (handle == INVALID_HANDLE_VALUE) {
            logFileHook("seedPack: no *.pack files found in \"%s\"", scanDir);
            return;
        }
        do {
            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                continue;
            }
            if (!nameIsPackFile(findData.cFileName)) {
                continue;
            }
            seedPackFileFromDataDir(scanDir, findData.cFileName);
        } while (FindNextFileA(handle, &findData));
        FindClose(handle);
    }
#else
    {
        DIR *dir = opendir(scanDir);
        struct dirent *entry;

        if (dir == NULL) {
            logFileHook("seedPack: cannot open \"%s\"", scanDir);
            return;
        }
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            if (!nameIsPackFile(entry->d_name)) {
                continue;
            }
            seedPackFileFromDataDir(scanDir, entry->d_name);
        }
        closedir(dir);
    }
#endif
}

void loadDirectory(uint8_t *buffer, uint32_t maxEntries, uint32_t pathSize) {
    uint32_t slotIndex;
    char diskName[256];
    char amigaName[256];

    logFileHook("loadDirectory called (buffer=%p maxEntries=%u pathSize=%u)",
                (void *)buffer, maxEntries, pathSize);

    if (buffer == NULL || maxEntries == 0u || pathSize == 0u) {
        logFileHook("loadDirectory: bad args, returning without changes");
        return;
    }

    ensureSettingsDir();
    logFileHook("loadDirectory: scanning %s for *.pack", settingsDir);
    slotIndex = 0u;

#if defined(_WIN32)
    {
        char searchPath[576];
        WIN32_FIND_DATAA findData;
        HANDLE handle;

        snprintf(searchPath, sizeof(searchPath), "%s*.pack", settingsDir);
        handle = FindFirstFileA(searchPath, &findData);
        if (handle == INVALID_HANDLE_VALUE) {
            logFileHook("loadDirectory: no *.pack files found (or cannot read dir)");
            logFileHook("loadDirectory: returning 0 entries");
            return;
        }
        do {
            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                logFileHook("loadDirectory: skip dir '%s'", findData.cFileName);
                continue;
            }
            if (!nameIsPackFile(findData.cFileName)) {
                logFileHook("loadDirectory: skip '%s' (not *.pack)",
                            findData.cFileName);
                continue;
            }
            packBaseNameFromFile(findData.cFileName, diskName, sizeof(diskName));
            strncpy(amigaName, diskName, sizeof(amigaName) - 1u);
            amigaName[sizeof(amigaName) - 1u] = '\0';
            asciiToUpperInPlace(amigaName);
            logFileHook("loadDirectory: pack file '%s' -> slot %u disk '%s' amiga '%s'",
                        findData.cFileName, slotIndex, diskName, amigaName);
            writeNameSlot(buffer, slotIndex, pathSize, amigaName);
            slotIndex++;
            if (slotIndex >= maxEntries) {
                logFileHook("loadDirectory: hit maxEntries (%u), stopping scan",
                            maxEntries);
                break;
            }
        } while (FindNextFileA(handle, &findData));
        FindClose(handle);
    }
#else
    {
        DIR *dir = opendir(settingsDir);
        struct dirent *entry;

        if (dir == NULL) {
            logFileHook("loadDirectory: cannot open '%s'", settingsDir);
            logFileHook("loadDirectory: returning 0 entries");
            return;
        }
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            if (!nameIsPackFile(entry->d_name)) {
                logFileHook("loadDirectory: skip '%s' (not *.pack)",
                            entry->d_name);
                continue;
            }
            packBaseNameFromFile(entry->d_name, diskName, sizeof(diskName));
            strncpy(amigaName, diskName, sizeof(amigaName) - 1u);
            amigaName[sizeof(amigaName) - 1u] = '\0';
            asciiToUpperInPlace(amigaName);
            logFileHook("loadDirectory: pack file '%s' -> slot %u disk '%s' amiga '%s'",
                        entry->d_name, slotIndex, diskName, amigaName);
            writeNameSlot(buffer, slotIndex, pathSize, amigaName);
            slotIndex++;
            if (slotIndex >= maxEntries) {
                logFileHook("loadDirectory: hit maxEntries (%u), stopping scan",
                            maxEntries);
                break;
            }
        }
        closedir(dir);
    }
#endif

    logFileHook("loadDirectory: done, wrote %u entries to buffer", slotIndex);
    if (slotIndex > 0u) {
        logBufferSlots(buffer, slotIndex, pathSize);
    }
}

void loadFile(uint8_t *pathPtr, uint8_t *bufPtr, uint32_t maxBytes) {
    char packSuffix[256];
    char hostPath[768];
    FILE *file;
    size_t bytesRead;

    logFileHook("loadFile called (pathPtr=%p bufPtr=%p maxBytes=%u)",
                (void *)pathPtr, (void *)bufPtr, maxBytes);

    if (pathPtr == NULL || bufPtr == NULL || maxBytes == 0u) {
        logFileHook("loadFile: bad args, returning");
        return;
    }

    logFileHook("loadFile: amiga path \"%s\"", (char *)pathPtr);
    extractPackSuffix((char *)pathPtr, packSuffix, sizeof(packSuffix));
    if (packSuffix[0] == '\0') {
        logFileHook("loadFile: empty pack name from \"%s\"", (char *)pathPtr);
        return;
    }
    logFileHook("loadFile: amiga pack name \"%s\"", packSuffix);
    asciiToLowerInPlace(packSuffix);
    logFileHook("loadFile: host pack name \"%s\"", packSuffix);

    buildPackHostPath(hostPath, sizeof(hostPath), packSuffix);
    logFileHook("loadFile: host path \"%s\"", hostPath);
    file = fopen(hostPath, "rb");
    if (file == NULL) {
        logFileHook("loadFile: FAILED to open \"%s\"", hostPath);
        return;
    }

    bytesRead = fread(bufPtr, 1u, (size_t)maxBytes, file);
    if (bytesRead < (size_t)maxBytes) {
        if (ferror(file)) {
            logFileHook("loadFile: read error for \"%s\"", hostPath);
        } else if (bytesRead > 0u) {
            memset(bufPtr + bytesRead, 0, (size_t)maxBytes - bytesRead);
            logFileHook("loadFile: short file (%zu bytes), zero-filled rest",
                        bytesRead);
        } else {
            logFileHook("loadFile: file is empty");
        }
    }
    fclose(file);
    logFileHook("loadFile: OK, read %zu byte(s) into buffer", bytesRead);
}

void saveFile(uint8_t *pathPtr, uint8_t *bufPtr, uint32_t numBytes) {
    char packSuffix[256];
    char hostPath[768];
    FILE *file;
    size_t bytesWritten;

    logFileHook("saveFile called (pathPtr=%p bufPtr=%p numBytes=%u)",
                (void *)pathPtr, (void *)bufPtr, numBytes);

    if (pathPtr == NULL || bufPtr == NULL || numBytes == 0u) {
        logFileHook("saveFile: bad args, returning");
        return;
    }

    logFileHook("saveFile: amiga path \"%s\"", (char *)pathPtr);
    extractPackSuffix((char *)pathPtr, packSuffix, sizeof(packSuffix));
    if (packSuffix[0] == '\0') {
        logFileHook("saveFile: empty pack name from \"%s\"", (char *)pathPtr);
        return;
    }
    logFileHook("saveFile: amiga pack name \"%s\"", packSuffix);
    asciiToLowerInPlace(packSuffix);
    logFileHook("saveFile: host pack name \"%s\"", packSuffix);

    buildPackHostPath(hostPath, sizeof(hostPath), packSuffix);
    logFileHook("saveFile: host path \"%s\"", hostPath);
    file = fopen(hostPath, "wb");
    if (file == NULL) {
        logFileHook("saveFile: FAILED to create \"%s\"", hostPath);
        return;
    }

    bytesWritten = fwrite(bufPtr, 1u, (size_t)numBytes, file);
    fclose(file);
    if (bytesWritten != (size_t)numBytes) {
        logFileHook("saveFile: short write (%zu of %u bytes) for \"%s\"",
                    bytesWritten, numBytes, hostPath);
        return;
    }
    logFileHook("saveFile: OK, wrote %zu byte(s)", bytesWritten);
}
