#include "amigaHostSdl.h"
#include "amigaHostMusic.h"
#include "clockagaHostFiles.h"
#include "clockwiserDrawText.h"
#include "clockwiser_movie_player.h"
#include "clockwiser_state.h"
#include "m68k_runtime.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#if defined(__APPLE__)
#include <unistd.h>
#include <limits.h>
#include <mach-o/dyld.h>
#endif

#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#elif __has_include(<SDL.h>)
#include <SDL.h>
#include <SDL_image.h>
#else
#error "SDL headers not found"
#endif

void clockwiserGameInit(M68k* cpu, char* baseFolder);
int  clockwiserGameTick(M68k* cpu);

typedef enum {
    gameStateInvalid = -1,
    gameStateIntroMovie,
    gameStateGame
} GameState;

static MovieFrameSoundCue gIntroGifSoundCues[] = {
    { 17, "data/music/introsnd1.wav" },
    { 38, "data/music/introsnd2.wav" },
    { 75, "data/music/introsnd3.wav" },
};
#define CLOCKWISER_INTRO_GIF_SOUND_CUE_COUNT \
    ((int)(sizeof(gIntroGifSoundCues) / sizeof(gIntroGifSoundCues[0])))

static void nextState(AmigaHostSdl* host, M68k* cpu, GameState newState);
static void gameShellTick(AmigaHostSdl* host,
                            M68k* cpu,
                            int shellAdvanceLatch,
                            int shellBackLatch,
                            int shellPauseMenuLatch,
                            int menuUp,
                            int menuDown,
                            int shellMouseFbX,
                            int shellMouseFbY);

static GameState shellState = gameStateInvalid;
static int shellFullscreenEnabled;
static int shellFullscreenApplied;
static int shellScalerMode;
static int shellScalerApplied;
static int shellTimeEndless;
static int shellMusicCd;
static int settingsMenuRowIndex;
static uint8_t gameShellCharset[CLOCKWISER_FONT_BYTES];
static int gameShellCharsetReady;
static uint32_t* gameplayPauseSnapshotPixels;
static int gameplayPauseSnapshotReady;
static int gameplayPauseActive;
static int gameplayPauseRowIndex;
static int gameplayPauseSubmenuSettings;
static int gameplaySettingsHintActive;
static int gameplaySettingsHintFrameCounter;
static uint64_t gameplaySettingsHintPulseStartMs;
static MoviePlayer* introMoviePlayer;
static int introMovieGoToGame;

#define GAMEPLAY_SETTINGS_HINT_START_DELAY_FRAMES 240
#define GAMEPLAY_SETTINGS_HINT_PULSE_HZ 1.0f
#define GAMEPLAY_SETTINGS_HINT_PULSE_CYCLES 6
#define GAMEPLAY_SETTINGS_HINT_TOTAL_MS \
    ((uint64_t)((1000.0f * (float)GAMEPLAY_SETTINGS_HINT_PULSE_CYCLES) \
                / GAMEPLAY_SETTINGS_HINT_PULSE_HZ))

#if defined(__APPLE__)
static void clockwiserSetWorkingDirFromAppBundle(void) {
    uint32_t bufSize;
    char exePath[PATH_MAX];
    char resolvedPath[PATH_MAX];
    char* marker;
    size_t rootLen;
    char resourcesPath[PATH_MAX];
    char macosPath[PATH_MAX];
    char resourcesDataPath[PATH_MAX];
    char macosDataPath[PATH_MAX];

    bufSize = (uint32_t)sizeof(exePath);
    if (_NSGetExecutablePath(exePath, &bufSize) != 0) {
        return;
    }
    if (realpath(exePath, resolvedPath) == NULL) {
        return;
    }

    marker = strstr(resolvedPath, "/Contents/MacOS/");
    if (marker == NULL) {
        return;
    }
    rootLen = (size_t)(marker - resolvedPath);
    if (rootLen + strlen("/Contents/Resources") + 1 >= sizeof(resourcesPath)) {
        return;
    }
    if (rootLen + strlen("/Contents/MacOS") + 1 >= sizeof(macosPath)) {
        return;
    }
    memcpy(resourcesPath, resolvedPath, rootLen);
    resourcesPath[rootLen] = '\0';
    memcpy(macosPath, resolvedPath, rootLen);
    macosPath[rootLen] = '\0';
    strcat(resourcesPath, "/Contents/Resources");
    strcat(macosPath, "/Contents/MacOS");

    if (snprintf(resourcesDataPath, sizeof(resourcesDataPath), "%s/data", resourcesPath) > 0
        && access(resourcesDataPath, F_OK) == 0) {
        (void)chdir(resourcesPath);
        return;
    }
    if (snprintf(macosDataPath, sizeof(macosDataPath), "%s/data", macosPath) > 0
        && access(macosDataPath, F_OK) == 0) {
        (void)chdir(macosPath);
        return;
    }

    (void)chdir(resourcesPath);
}
#endif

static float clamp01(float v) {
    if (v < 0.0f) {
        return 0.0f;
    }
    if (v > 1.0f) {
        return 1.0f;
    }
    return v;
}

static void introMovieDoneCb(int naturalEnd, void* userData) {
    (void)naturalEnd;
    (void)userData;
    introMovieGoToGame = 1;
}

static void discardIntroMovie(void) {
    MoviePlayer* dead;

    dead               = introMoviePlayer;
    introMoviePlayer   = NULL;
    introMovieGoToGame = 0;
    if (dead != NULL) {
        moviePlayerDestroy(dead);
    }
}

static int finishIntroMovieIfPending(AmigaHostSdl* host, M68k* cpu) {
    if (!introMovieGoToGame) {
        return 0;
    }
    introMovieGoToGame = 0;
    discardIntroMovie();
    nextState(host, cpu, gameStateGame);
    return 1;
}

static void fillFramebufferSolid(AmigaHostSdl* host, uint32_t argb) {
    size_t i;
    size_t n;

    assert(host->frameBuffer && host->width > 0 && host->height > 0);
    n = (size_t)host->width * (size_t)host->height;
    for (i = 0; i < n; i++) {
        host->frameBuffer[i] = argb;
    }
}

static void freePixelCache(uint32_t** pixels, int* ready) {
    if (pixels == NULL || ready == NULL) {
        return;
    }
    if (*pixels != NULL) {
        free(*pixels);
        *pixels = NULL;
    }
    *ready = 0;
}

static void gameShellDrawText(AmigaHostSdl* host,
                               int x,
                               int y,
                               int pixelScale,
                               DrawTextColor color,
                               float brightness,
                               float opacity,
                               char* text) {
    assert(host && host->frameBuffer && text);
    if (!gameShellCharsetReady) {
        return;
    }
    drawText(host->frameBuffer,
             host->width,
             host->height,
             gameShellCharset,
             x,
             y,
             pixelScale,
             color,
             brightness,
             opacity,
             text);
}

static void dimFramebufferRgb(AmigaHostSdl* host, float rgbMult) {
    size_t i;
    size_t n;

    assert(host && host->frameBuffer);
    rgbMult = clamp01(rgbMult);
    n = (size_t)host->width * (size_t)host->height;
    for (i = 0; i < n; i++) {
        uint32_t p;
        int r;
        int g;
        int b;

        p = host->frameBuffer[i];
        r = (int)((float)((p >> 16) & 0xFFu) * rgbMult);
        g = (int)((float)((p >> 8) & 0xFFu) * rgbMult);
        b = (int)((float)(p & 0xFFu) * rgbMult);
        host->frameBuffer[i] =
            0xFF000000u | ((uint32_t)r << 16u) | ((uint32_t)g << 8u) | (uint32_t)b;
    }
}

static void fillShellOverlayBackdrop(AmigaHostSdl* host) {
    fillFramebufferSolid(host, 0xFF202020u);
}

static int shellMenuThreeRowEdges[] = {80, 124, 158, 210};

static int shellMenuRowHit(int fbY, int* edges, int rowCount) {
    int i;

    for (i = 0; i < rowCount; i++) {
        if (fbY >= edges[i] && fbY < edges[i + 1]) {
            return i;
        }
    }
    return -1;
}

static int shellSettingsMenuRowEdges[] = {56, 88, 120, 152, 184, 216};

static int shellSettingsMenuRowHit(int fbY) {
    return shellMenuRowHit(fbY, shellSettingsMenuRowEdges, 5);
}

static void applyShellFullscreenSetting(AmigaHostSdl* host) {
    uint32_t flags;
    int rc;

    assert(host && host->window);
    if (shellFullscreenApplied == shellFullscreenEnabled) {
        return;
    }
    flags = shellFullscreenEnabled != 0 ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0u;
    rc = SDL_SetWindowFullscreen((SDL_Window*)host->window, flags);
    if (rc != 0) {
        fprintf(stderr, "Could not switch fullscreen mode: %s\n", SDL_GetError());
        return;
    }
    shellFullscreenApplied = shellFullscreenEnabled;
}

static void applyShellScaleModeSetting(AmigaHostSdl* host) {
    assert(host);
    if (shellScalerMode < amigaHostScaleModeNormal || shellScalerMode > amigaHostScaleModeXbrz) {
        shellScalerMode = amigaHostScaleModeNormal;
    }
    if (shellScalerApplied == shellScalerMode) {
        return;
    }
    amigaHostSdlSetScaleMode(host, shellScalerMode);
    shellScalerApplied = shellScalerMode;
}

static int settingsRowY[5] = { 56, 88, 120, 152, 184 };

static void presentMenuSettings(AmigaHostSdl* host, int overlayOnPauseSnapshot) {
    int scale;
    char* lines[5];
    int i;
    float tSec;
    float selectedPulse;
    size_t n;

    if (overlayOnPauseSnapshot != 0) {
        assert(host && host->frameBuffer);
        if (!gameplayPauseSnapshotReady || gameplayPauseSnapshotPixels == NULL) {
            return;
        }
        n = (size_t)host->width * (size_t)host->height;
        memcpy(host->frameBuffer, gameplayPauseSnapshotPixels, n * sizeof(uint32_t));
        dimFramebufferRgb(host, 0.15f);
    } else {
        fillShellOverlayBackdrop(host);
    }

    scale = 2;
    if (shellFullscreenEnabled == 0) {
        lines[0] = "FULLSCREEN: NO";
    } else {
        lines[0] = "FULLSCREEN: YES";
    }
    if (shellScalerMode == amigaHostScaleModeXbr) {
        lines[1] = "SCALER: XBR";
    } else if (shellScalerMode == amigaHostScaleModeXbrz) {
        lines[1] = "SCALER: XBRZ";
    } else {
        lines[1] = "SCALER: NORMAL";
    }
    if (shellTimeEndless == 0) {
        lines[2] = "TIME: NORMAL";
    } else {
        lines[2] = "TIME: ENDLESS";
    }
    if (shellMusicCd == 0) {
        lines[3] = "MUSIC: TRACKER";
    } else {
        lines[3] = "MUSIC: CD";
    }
    lines[4] = "RETURN";

    tSec = (float)SDL_GetTicks64() / 1000.0f;
    selectedPulse = 0.5f * sinf(2.0f * 3.14159265f * 1.0f * tSec);

    for (i = 0; i < 5; i++) {
        int w;
        int x;

        w = (int)strlen(lines[i]) * 8 * scale;
        x = (host->width - w) / 2;
        gameShellDrawText(host,
                          x,
                          settingsRowY[i],
                          scale,
                          drawTextColorBlue,
                          settingsMenuRowIndex == i ? selectedPulse : 0.0f,
                          1.0f,
                          lines[i]);
    }

    amigaHostSdlPresentFramebufferOnly(host);
}

static int activateSettingsRow(AmigaHostSdl* host, M68k* cpu, int row) {
    (void)cpu;
    if (row == 0) {
        shellFullscreenEnabled = shellFullscreenEnabled == 0 ? 1 : 0;
        applyShellFullscreenSetting(host);
    } else if (row == 1) {
        shellScalerMode = (shellScalerMode + 1) % 3;
        applyShellScaleModeSetting(host);
    } else if (row == 2) {
        shellTimeEndless = shellTimeEndless == 0 ? 1 : 0;
        clockwiserStateSetTimeEndless(shellTimeEndless);
    } else if (row == 3) {
        shellMusicCd = shellMusicCd == 0 ? 1 : 0;
        clockwiserStateSetMusicCd(shellMusicCd);
        amigaHostMusicSetCdMode(shellMusicCd);
    } else if (row == 4) {
        return 1;
    } else {
        return 0;
    }
    (void)clockwiserStateSave(shellFullscreenEnabled, shellScalerMode, shellTimeEndless,
                              shellMusicCd);
    return 0;
}

static void gameplayPauseEnsureSnapshotBuffer(AmigaHostSdl* host) {
    size_t n;

    assert(host && host->frameBuffer && host->width > 0 && host->height > 0);
    if (gameplayPauseSnapshotPixels != NULL) {
        return;
    }
    n = (size_t)host->width * (size_t)host->height;
    gameplayPauseSnapshotPixels = (uint32_t*)malloc(n * sizeof(uint32_t));
    if (gameplayPauseSnapshotPixels == NULL) {
        fprintf(stderr, "Out of memory for gameplay pause snapshot\n");
    }
}

static void gameplayPauseCaptureSnapshot(AmigaHostSdl* host) {
    size_t n;

    gameplayPauseEnsureSnapshotBuffer(host);
    if (gameplayPauseSnapshotPixels == NULL || host == NULL || host->frameBuffer == NULL) {
        gameplayPauseSnapshotReady = 0;
        return;
    }
    n = (size_t)host->width * (size_t)host->height;
    memcpy(gameplayPauseSnapshotPixels, host->frameBuffer, n * sizeof(uint32_t));
    gameplayPauseSnapshotReady = 1;
}

static void gameplaySettingsHintOverlay(AmigaHostSdl* host, void* userData) {
    char* hintText;
    int scale;
    int x;
    int y;
    uint64_t elapsedMs;
    float tSec;
    float pulseOpacity;

    (void)userData;
    if (!gameplaySettingsHintActive) {
        return;
    }
    if (gameplaySettingsHintFrameCounter >= GAMEPLAY_SETTINGS_HINT_START_DELAY_FRAMES) {
        hintText = "PRESS TAB FOR SETTINGS";
        scale = 1;
        x = 8;
        y = 4;
        if (gameplaySettingsHintFrameCounter == GAMEPLAY_SETTINGS_HINT_START_DELAY_FRAMES) {
            gameplaySettingsHintPulseStartMs = SDL_GetTicks64();
        }
        elapsedMs = SDL_GetTicks64() - gameplaySettingsHintPulseStartMs;
        if (elapsedMs >= GAMEPLAY_SETTINGS_HINT_TOTAL_MS) {
            gameplaySettingsHintActive = 0;
            gameplaySettingsHintFrameCounter++;
            return;
        }
        tSec = (float)elapsedMs / 1000.0f;
        pulseOpacity = 0.5f
                       * (sinf(2.0f * 3.14159265f * GAMEPLAY_SETTINGS_HINT_PULSE_HZ * tSec
                              - 1.57079632f)
                          + 1.0f);
        gameShellDrawText(host,
                          x,
                          y,
                          scale,
                          drawTextColorWhite,
                          0.0f,
                          pulseOpacity,
                          hintText);
    }
    gameplaySettingsHintFrameCounter++;
}

static void gameplayHudOverlay(AmigaHostSdl* host, void* userData) {
    char buf[64];

    if (host->showPerfHud) {
        snprintf(buf, sizeof(buf), "SCALE %.2f  FRAME %.2f MS",
                 host->lastScaleMs, host->lastFrameMs);
        gameShellDrawText(host, 4, 4, 1, drawTextColorGreen, 0.0f, 1.0f, buf);
    }
    gameplaySettingsHintOverlay(host, userData);
}

static void presentGameplayPauseMenu(AmigaHostSdl* host) {
    size_t n;
    float tSec;
    float selectedPulse;
    int scale;
    char* lineResume;
    char* lineSettings;
    char* lineExit;
    int wResume;
    int wSettings;
    int wExit;
    int xResume;
    int xSettings;
    int xExit;

    assert(host && host->frameBuffer);
    if (!gameplayPauseSnapshotReady || gameplayPauseSnapshotPixels == NULL) {
        return;
    }
    n = (size_t)host->width * (size_t)host->height;
    memcpy(host->frameBuffer, gameplayPauseSnapshotPixels, n * sizeof(uint32_t));
    dimFramebufferRgb(host, 0.3f);

    scale        = 2;
    lineResume   = "RESUME";
    lineSettings = "SETTINGS";
    lineExit     = "EXIT APP";
    wResume      = (int)strlen(lineResume) * 8 * scale;
    wSettings    = (int)strlen(lineSettings) * 8 * scale;
    wExit        = (int)strlen(lineExit) * 8 * scale;
    xResume      = (host->width - wResume) / 2;
    xSettings    = (host->width - wSettings) / 2;
    xExit        = (host->width - wExit) / 2;

    tSec = (float)SDL_GetTicks64() / 1000.0f;
    selectedPulse = 0.5f * sinf(2.0f * 3.14159265f * 1.0f * tSec);

    gameShellDrawText(host,
                      xResume,
                      96,
                      scale,
                      drawTextColorBlue,
                      gameplayPauseRowIndex == 0 ? selectedPulse : 0.0f,
                      1.0f,
                      lineResume);
    gameShellDrawText(host,
                      xSettings,
                      132,
                      scale,
                      drawTextColorBlue,
                      gameplayPauseRowIndex == 1 ? selectedPulse : 0.0f,
                      1.0f,
                      lineSettings);
    gameShellDrawText(host,
                      xExit,
                      168,
                      scale,
                      drawTextColorBlue,
                      gameplayPauseRowIndex == 2 ? selectedPulse : 0.0f,
                      1.0f,
                      lineExit);
    amigaHostSdlPresentFramebufferOnly(host);
}

static void nextState(AmigaHostSdl* host, M68k* cpu, GameState newState) {
    GameState oldState;

    (void)host;
    oldState = shellState;
    if (newState == oldState) {
        return;
    }

    if (oldState == gameStateGame && newState != gameStateGame) {
        gameplayPauseActive = 0;
        gameplayPauseRowIndex = 0;
        gameplayPauseSubmenuSettings = 0;
        freePixelCache(&gameplayPauseSnapshotPixels, &gameplayPauseSnapshotReady);
    }
    if (oldState == gameStateIntroMovie && newState != gameStateIntroMovie) {
        discardIntroMovie();
    }

    shellState = newState;

    switch (shellState) {
    case gameStateIntroMovie:
        introMovieGoToGame = 0;
        introMoviePlayer   = moviePlayerCreate();
        if (introMoviePlayer == NULL) {
            fprintf(stderr, "movie: out of memory\n");
            break;
        }
        if (!moviePlayerPlayFile(introMoviePlayer,
                                 "data/intro.gif",
                                 introMovieDoneCb,
                                 NULL,
                                 host->width,
                                 host->height,
                                 gIntroGifSoundCues,
                                 CLOCKWISER_INTRO_GIF_SOUND_CUE_COUNT)) {
            fprintf(stderr,
                    "Intro animation failed to load (see \"movie:\" lines above). "
                    "Click / gamepad A to continue.\n");
            moviePlayerDestroy(introMoviePlayer);
            introMoviePlayer = NULL;
        }
        break;
    case gameStateGame:
        amigaHostMusicSetBasePath("data/music");
        clockwiserGameInit(cpu, "data");
        gameplaySettingsHintActive = 1;
        gameplaySettingsHintFrameCounter = 0;
        gameplaySettingsHintPulseStartMs = 0;
        break;
    case gameStateInvalid:
        break;
    }
}

static void gameShellTick(AmigaHostSdl* host,
                            M68k* cpu,
                            int shellAdvanceLatch,
                            int shellBackLatch,
                            int shellPauseMenuLatch,
                            int menuUp,
                            int menuDown,
                            int shellMouseFbX,
                            int shellMouseFbY) {
    switch (shellState) {
    case gameStateIntroMovie:
        if (finishIntroMovieIfPending(host, cpu)) {
            break;
        }
        if (shellAdvanceLatch) {
            if (introMoviePlayer != NULL) {
                moviePlayerStop(introMoviePlayer, 0);
            } else {
                nextState(host, cpu, gameStateGame);
                break;
            }
        }
        if (finishIntroMovieIfPending(host, cpu)) {
            break;
        }
        if (introMoviePlayer != NULL && moviePlayerIsPlaying(introMoviePlayer)) {
            moviePlayerUpdate(introMoviePlayer,
                              (uint8_t*)host->frameBuffer,
                              host->width,
                              host->height,
                              host->width * (int)sizeof(uint32_t));
        }
        if (finishIntroMovieIfPending(host, cpu)) {
            break;
        }
        if (introMoviePlayer == NULL || !moviePlayerIsPlaying(introMoviePlayer)) {
            fillFramebufferSolid(host, 0xFF000000u);
        }
        amigaHostSdlPresentFramebufferOnly(host);
        break;
    case gameStateGame:
        if (gameplayPauseActive) {
            if (gameplayPauseSubmenuSettings) {
                if (shellBackLatch || shellPauseMenuLatch) {
                    gameplayPauseSubmenuSettings = 0;
                    presentGameplayPauseMenu(host);
                    break;
                }
                if (menuUp) {
                    settingsMenuRowIndex = (settingsMenuRowIndex + 4) % 5;
                }
                if (menuDown) {
                    settingsMenuRowIndex = (settingsMenuRowIndex + 1) % 5;
                }
                if (shellAdvanceLatch) {
                    int selectedRow;
                    int activateResult;

                    if (shellMouseFbX >= 0) {
                        selectedRow = shellSettingsMenuRowHit(shellMouseFbY);
                    } else {
                        selectedRow = settingsMenuRowIndex;
                    }
                    if (selectedRow >= 0) {
                        activateResult = activateSettingsRow(host, cpu, selectedRow);
                        if (activateResult == 1) {
                            gameplayPauseSubmenuSettings = 0;
                            presentGameplayPauseMenu(host);
                            break;
                        }
                    }
                }
                presentMenuSettings(host, 1);
                break;
            }
            if (shellBackLatch || shellPauseMenuLatch) {
                gameplayPauseActive = 0;
                gameplayPauseSubmenuSettings = 0;
                break;
            }
            if (menuUp) {
                gameplayPauseRowIndex = (gameplayPauseRowIndex + 2) % 3;
            }
            if (menuDown) {
                gameplayPauseRowIndex = (gameplayPauseRowIndex + 1) % 3;
            }
            if (shellAdvanceLatch) {
                if (shellMouseFbX >= 0) {
                    int rowHit;

                    rowHit = shellMenuRowHit(shellMouseFbY, shellMenuThreeRowEdges, 3);
                    if (rowHit == 0) {
                        gameplayPauseActive = 0;
                    } else if (rowHit == 1) {
                        gameplayPauseSubmenuSettings = 1;
                        settingsMenuRowIndex = 0;
                        presentMenuSettings(host, 1);
                    } else if (rowHit == 2) {
                        host->running = 0;
                    }
                } else {
                    if (gameplayPauseRowIndex == 0) {
                        gameplayPauseActive = 0;
                    } else if (gameplayPauseRowIndex == 1) {
                        gameplayPauseSubmenuSettings = 1;
                        settingsMenuRowIndex = 0;
                        presentMenuSettings(host, 1);
                    } else if (gameplayPauseRowIndex == 2) {
                        host->running = 0;
                    }
                }
            }
            if (gameplayPauseActive) {
                if (gameplayPauseSubmenuSettings) {
                    presentMenuSettings(host, 1);
                } else {
                    presentGameplayPauseMenu(host);
                }
            }
            break;
        }
        if (shellPauseMenuLatch) {
            gameplaySettingsHintActive = 0;
            gameplayPauseCaptureSnapshot(host);
            if (gameplayPauseSnapshotReady) {
                gameplayPauseActive   = 1;
                gameplayPauseRowIndex = 0;
                presentGameplayPauseMenu(host);
            }
            break;
        }
        if (!clockwiserGameTick(cpu)) {
            host->running = 0;
            break;
        }
        amigaHostSdlPresentFrameEx(host, gameplayHudOverlay, NULL);
        break;
    case gameStateInvalid:
        break;
    }
}

int main(void) {
#if defined(__APPLE__)
    clockwiserSetWorkingDirFromAppBundle();
#endif

    M68k cpu;
    AmigaHostSdl host;
    uint64_t perfFreq;
    uint64_t frameStart;
    uint64_t frameEnd;
    uint64_t frameElapsed;
    uint64_t targetTicks;
    int shellAdvanceLatch;
    int shellBackLatch;
    int shellPauseMenuLatch;
    int shellOtherKeyLatch;
    int shellTextBackspace;
    int shellTextSubmit;
    char shellTextChar;
    int menuUp;
    int menuDown;
    int mouseFbX;
    int mouseFbY;

    memset(&cpu, 0, sizeof(cpu));
    shellFullscreenEnabled = 0;
    shellFullscreenApplied = 0;
    shellScalerMode = amigaHostScaleModeNormal;
    shellScalerApplied = amigaHostScaleModeNormal;
    shellTimeEndless = 0;
    shellMusicCd = 0;
    settingsMenuRowIndex = 0;
    gameplayPauseSnapshotPixels = NULL;
    gameplayPauseSnapshotReady = 0;
    gameplayPauseActive = 0;
    gameplayPauseRowIndex = 0;
    gameplayPauseSubmenuSettings = 0;
    gameplaySettingsHintActive = 0;
    gameplaySettingsHintFrameCounter = 0;
    gameplaySettingsHintPulseStartMs = 0;
    introMoviePlayer   = NULL;
    introMovieGoToGame = 0;

    if (!amigaHostSdlInit(&host, &cpu, 320, 256, 3)) {
        fprintf(stderr, "Failed to init SDL host\n");
        return 1;
    }

    clockagaHostSeedPackFilesFromData("data");

    amigaHostSdlEnableMouseJoy0(&host);

    if ((IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG) == 0) {
        fprintf(stderr, "IMG_Init PNG failed: %s\n", IMG_GetError());
        amigaHostSdlShutdown(&host);
        return 1;
    }

    if (!loadCharsetFromFile("data/chars", gameShellCharset)) {
        fprintf(stderr, "Could not load data/chars (8x8 font)\n");
        gameShellCharsetReady = 0;
    } else {
        gameShellCharsetReady = 1;
    }

    (void)clockwiserStateLoad(&shellFullscreenEnabled, &shellScalerMode, &shellTimeEndless,
                              &shellMusicCd);
    clockwiserStateSetTimeEndless(shellTimeEndless);
    clockwiserStateSetMusicCd(shellMusicCd);
    amigaHostMusicSetCdMode(shellMusicCd);
    applyShellFullscreenSetting(&host);
    applyShellScaleModeSetting(&host);

    nextState(&host, &cpu, gameStateGame);

    perfFreq    = SDL_GetPerformanceFrequency();
    targetTicks = perfFreq / 50u;
    if (targetTicks == 0u) {
        targetTicks = 1u;
    }

    while (host.running) {
        frameStart = SDL_GetPerformanceCounter();

        shellAdvanceLatch = 0;
        shellBackLatch    = 0;
        shellPauseMenuLatch = 0;
        shellOtherKeyLatch = 0;
        shellTextBackspace = 0;
        shellTextSubmit = 0;
        shellTextChar = '\0';
        menuUp            = 0;
        menuDown          = 0;
        mouseFbX          = -1;
        mouseFbY          = -1;
        amigaHostSdlPollEvents(&host,
                               &shellAdvanceLatch,
                               &shellBackLatch,
                               &shellPauseMenuLatch,
                               &shellOtherKeyLatch,
                               &shellTextBackspace,
                               &shellTextSubmit,
                               &shellTextChar,
                               0,
                               &menuUp,
                               &menuDown,
                               &mouseFbX,
                               &mouseFbY);

        gameShellTick(&host,
                      &cpu,
                      shellAdvanceLatch,
                      shellBackLatch,
                      shellPauseMenuLatch,
                      menuUp,
                      menuDown,
                      mouseFbX,
                      mouseFbY);

        frameEnd     = SDL_GetPerformanceCounter();
        frameElapsed = frameEnd - frameStart;
        host.lastFrameMs = (double)frameElapsed * 1000.0 / (double)perfFreq;
        if (frameElapsed < targetTicks) {
            uint64_t remainTicks = targetTicks - frameElapsed;
            uint64_t remainMs    = (remainTicks * 1000u) / perfFreq;
            if (remainMs > 0u) {
                SDL_Delay((uint32_t)remainMs);
            }
        }
    }

    discardIntroMovie();
    freePixelCache(&gameplayPauseSnapshotPixels, &gameplayPauseSnapshotReady);
    IMG_Quit();
    amigaHostSdlShutdown(&host);
    return 0;
}
