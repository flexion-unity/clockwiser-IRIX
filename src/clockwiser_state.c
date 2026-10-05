#include "clockwiser_state.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#elif __has_include(<SDL.h>)
#include <SDL.h>
#else
#error "SDL headers not found"
#endif

static int clockwiserStatePathReady;
static char clockwiserStateFilePath[1024];
static int clockwiserStateTimeEndless;
static int clockwiserStateMusicCd;

static int clockwiserStateEnsureFilePath(void) {
    char* prefPath;
    int n;

    if (clockwiserStatePathReady) {
        return 1;
    }
    prefPath = SDL_GetPrefPath("ClockwiserPortable", "ClockwiserPortable");
    if (prefPath == NULL) {
        n = snprintf(clockwiserStateFilePath, sizeof(clockwiserStateFilePath), "clockwiser_state.txt");
        if (n <= 0 || (size_t)n >= sizeof(clockwiserStateFilePath)) {
            return 0;
        }
        clockwiserStatePathReady = 1;
        return 1;
    }

    n = snprintf(clockwiserStateFilePath, sizeof(clockwiserStateFilePath), "%sclockwiser_state.txt", prefPath);
    SDL_free(prefPath);
    if (n <= 0 || (size_t)n >= sizeof(clockwiserStateFilePath)) {
        return 0;
    }
    clockwiserStatePathReady = 1;
    return 1;
}

static void clockwiserStateTrimLine(char* line) {
    size_t len;

    assert(line);
    len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t')) {
        line[len - 1] = '\0';
        len--;
    }
}

int clockwiserStateGetTimeEndless(void) {
    return clockwiserStateTimeEndless;
}

void clockwiserStateSetTimeEndless(int enabled) {
    clockwiserStateTimeEndless = enabled != 0 ? 1 : 0;
}

int clockwiserStateGetMusicCd(void) {
    return clockwiserStateMusicCd;
}

void clockwiserStateSetMusicCd(int enabled) {
    clockwiserStateMusicCd = enabled != 0 ? 1 : 0;
}

int clockwiserStateLoad(int* fullscreenEnabledOut, int* scalerModeOut, int* timeEndlessOut,
                        int* musicCdOut) {
    FILE* f;
    char line[256];
    int fullscreenEnabled;
    int scalerMode;
    int timeEndless;

    int musicCd;

    assert(fullscreenEnabledOut && scalerModeOut && timeEndlessOut && musicCdOut);
    fullscreenEnabled = 0;
    scalerMode = 0;
    timeEndless = 0;
    musicCd = 0;
    clockwiserStateTimeEndless = 0;
    clockwiserStateMusicCd = 0;

    if (!clockwiserStateEnsureFilePath()) {
        *fullscreenEnabledOut = fullscreenEnabled;
        *scalerModeOut = scalerMode;
        *timeEndlessOut = timeEndless;
        *musicCdOut = musicCd;
        return 0;
    }
    f = fopen(clockwiserStateFilePath, "rb");
    if (f == NULL) {
        *fullscreenEnabledOut = fullscreenEnabled;
        *scalerModeOut = scalerMode;
        *timeEndlessOut = timeEndless;
        *musicCdOut = musicCd;
        return 0;
    }

    while (fgets(line, sizeof(line), f) != NULL) {
        char* colon;
        char* key;
        char* value;

        clockwiserStateTrimLine(line);
        if (line[0] == '\0') {
            continue;
        }
        colon = strchr(line, ':');
        if (colon == NULL) {
            continue;
        }
        *colon = '\0';
        key = line;
        value = colon + 1;

        if (strcmp(key, "fullscreen") == 0) {
            if (strcmp(value, "yes") == 0) {
                fullscreenEnabled = 1;
            } else if (strcmp(value, "no") == 0) {
                fullscreenEnabled = 0;
            }
            continue;
        }
        if (strcmp(key, "scaler") == 0) {
            if (strcmp(value, "xbr") == 0) {
                scalerMode = 1;
            } else if (strcmp(value, "xbrz") == 0) {
                scalerMode = 2;
            } else {
                scalerMode = 0;
            }
            continue;
        }
        if (strcmp(key, "time") == 0) {
            if (strcmp(value, "endless") == 0) {
                timeEndless = 1;
            } else {
                timeEndless = 0;
            }
            continue;
        }
        if (strcmp(key, "music") == 0) {
            if (strcmp(value, "cd") == 0) {
                musicCd = 1;
            } else {
                musicCd = 0;
            }
        }
    }
    fclose(f);

    clockwiserStateTimeEndless = timeEndless;
    clockwiserStateMusicCd = musicCd;
    *fullscreenEnabledOut = fullscreenEnabled;
    *scalerModeOut = scalerMode;
    *timeEndlessOut = timeEndless;
    *musicCdOut = musicCd;
    return 1;
}

int clockwiserStateSave(int fullscreenEnabled, int scalerMode, int timeEndless,
                        int musicCd) {
    FILE* f;
    char* scalerText;
    char* timeText;

    char* musicText;

    clockwiserStateTimeEndless = timeEndless != 0 ? 1 : 0;
    clockwiserStateMusicCd = musicCd != 0 ? 1 : 0;
    if (!clockwiserStateEnsureFilePath()) {
        return 0;
    }
    f = fopen(clockwiserStateFilePath, "wb");
    if (f == NULL) {
        return 0;
    }

    if (scalerMode == 1) {
        scalerText = "xbr";
    } else if (scalerMode == 2) {
        scalerText = "xbrz";
    } else {
        scalerText = "normal";
    }
    timeText = clockwiserStateTimeEndless != 0 ? "endless" : "normal";
    musicText = clockwiserStateMusicCd != 0 ? "cd" : "tracker";

    fprintf(f, "fullscreen:%s\n", fullscreenEnabled != 0 ? "yes" : "no");
    fprintf(f, "scaler:%s\n", scalerText);
    fprintf(f, "time:%s\n", timeText);
    fprintf(f, "music:%s\n", musicText);
    fclose(f);
    return 1;
}
