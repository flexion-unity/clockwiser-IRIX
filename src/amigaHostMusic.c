#include "amigaHostMusic.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(AMIGA_HOST_HAVE_SDL2_MIXER)
#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#elif __has_include(<SDL.h>)
#include <SDL.h>
#endif
#if __has_include(<SDL2/SDL_mixer.h>)
#include <SDL2/SDL_mixer.h>
#elif __has_include(<SDL_mixer.h>)
#include <SDL_mixer.h>
#endif

static int musicSubsystemReady;
static int musicBaseExplicit;
static char musicBaseDir[512];
static Mix_Music* loadedMusic;
static Mix_Music* titleThemeMusic;
static Mix_Music* menuThemeMusic;
static Mix_Music* gameOverThemeMusic;
static Mix_Music* levelSelectThemeMusic;
static int levelSelectThemeActive;
static int gameOverThemeActive;
static int musicCdMode;

#define HOI_MIXER_SFX_CHANNEL_FIRST 0
#define HOI_MIXER_SFX_CHANNEL_LAST 31
#define SAMPLE_CACHE_MAX 128

typedef struct {
    uint32_t sampleNr;
    Mix_Chunk* chunk;
} SampleCacheEntry;

static SampleCacheEntry sampleCache[SAMPLE_CACHE_MAX];
static int sampleCacheCount;

static int sampleCacheFind(uint32_t sampleNr) {
    int i;

    for (i = 0; i < sampleCacheCount; i++) {
        if (sampleCache[i].sampleNr == sampleNr) {
            return i;
        }
    }
    return -1;
}

static int sampleChunkIsCached(Mix_Chunk* ch) {
    int i;

    if (ch == NULL) {
        return 0;
    }
    for (i = 0; i < sampleCacheCount; i++) {
        if (sampleCache[i].chunk == ch) {
            return 1;
        }
    }
    return 0;
}

static void freeSampleCache(void) {
    int i;

    for (i = 0; i < sampleCacheCount; i++) {
        if (sampleCache[i].chunk != NULL) {
            Mix_FreeChunk(sampleCache[i].chunk);
            sampleCache[i].chunk = NULL;
        }
    }
    sampleCacheCount = 0;
}

static void shellSfxChannelFinished(int channel) {
    Mix_Chunk* ch;

    ch = Mix_GetChunk(channel);
    if (ch != NULL && !sampleChunkIsCached(ch)) {
        Mix_FreeChunk(ch);
    }
}

static int pathLooksAbsolute(char* p) {
    if (p == NULL || p[0] == '\0') {
        return 0;
    }
    if (p[0] == '/') {
        return 1;
    }
#if defined(_WIN32) || defined(__CYGWIN__)
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) &&
        p[1] == ':') {
        return 1;
    }
#endif
    return 0;
}

static void refreshExplicitBaseFromEnv(void) {
    char* env;

    env = getenv("AMIGA_MUSIC_DIR");
    if (env != NULL && env[0] != '\0') {
        strncpy(musicBaseDir, env, sizeof(musicBaseDir) - 1u);
        musicBaseDir[sizeof(musicBaseDir) - 1u] = '\0';
        musicBaseExplicit = 1;
    } else {
        musicBaseDir[0] = '\0';
        musicBaseExplicit = 0;
    }
}

void amigaHostMusicSetBasePath(char* path) {
    if (path != NULL && path[0] != '\0') {
        strncpy(musicBaseDir, path, sizeof(musicBaseDir) - 1u);
        musicBaseDir[sizeof(musicBaseDir) - 1u] = '\0';
        musicBaseExplicit = 1;
    } else {
        refreshExplicitBaseFromEnv();
    }
}

void amigaHostMusicInit(void) {
    int flags;
    int mixOpenRc;

    if (musicSubsystemReady) {
        return;
    }
    refreshExplicitBaseFromEnv();

    flags = Mix_Init(MIX_INIT_OGG);
    if ((flags & MIX_INIT_OGG) == 0) {
        fprintf(stderr, "amigaHostMusicInit: Mix_Init(OGG) failed: %s\n",
                Mix_GetError());
    }

    mixOpenRc = Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048);
    if (mixOpenRc != 0) {
        fprintf(stderr, "amigaHostMusicInit: Mix_OpenAudio failed: %s\n",
                Mix_GetError());
        Mix_Quit();
        return;
    }

    (void)Mix_AllocateChannels(32);
    Mix_ChannelFinished(shellSfxChannelFinished);

    musicSubsystemReady = 1;
}

void amigaHostMusicShutdown(void) {
    int c;

    if (!musicSubsystemReady) {
        return;
    }
    for (c = HOI_MIXER_SFX_CHANNEL_FIRST; c <= HOI_MIXER_SFX_CHANNEL_LAST; c++) {
        Mix_HaltChannel(c);
    }
    Mix_ChannelFinished(NULL);
    Mix_HaltMusic();
    if (loadedMusic != NULL) {
        Mix_FreeMusic(loadedMusic);
        loadedMusic = NULL;
    }
    if (titleThemeMusic != NULL) {
        Mix_FreeMusic(titleThemeMusic);
        titleThemeMusic = NULL;
    }
    if (menuThemeMusic != NULL) {
        Mix_FreeMusic(menuThemeMusic);
        menuThemeMusic = NULL;
    }
    if (gameOverThemeMusic != NULL) {
        Mix_FreeMusic(gameOverThemeMusic);
        gameOverThemeMusic = NULL;
    }
    if (levelSelectThemeMusic != NULL) {
        Mix_FreeMusic(levelSelectThemeMusic);
        levelSelectThemeMusic = NULL;
    }
    gameOverThemeActive      = 0;
    levelSelectThemeActive = 0;
    freeSampleCache();
    Mix_CloseAudio();
    Mix_Quit();
    musicSubsystemReady = 0;
}

static void unloadCurrentMusic(void) {
    Mix_HaltMusic();
    if (loadedMusic != NULL) {
        Mix_FreeMusic(loadedMusic);
        loadedMusic = NULL;
    }
}

void amigaHostMusicSetCdMode(int cdEnabled) {
    int newMode;

    newMode = cdEnabled != 0 ? 1 : 0;
    if (musicCdMode == newMode) {
        return;
    }
    musicCdMode = newMode;
    if (musicSubsystemReady) {
        unloadCurrentMusic();
    }
}

static int snprintfTrackOggFile(char* buf, size_t bufSize, uint32_t trackNr) {
    if (musicCdMode != 0) {
        return snprintf(buf, bufSize, "%ucd.ogg", (unsigned)trackNr);
    }
    return snprintf(buf, bufSize, "%u.ogg", (unsigned)trackNr);
}

static void freeTitleThemeMusicOnly(void) {
    if (titleThemeMusic != NULL) {
        Mix_FreeMusic(titleThemeMusic);
        titleThemeMusic = NULL;
    }
}

static void freeMenuThemeMusicOnly(void) {
    if (menuThemeMusic != NULL) {
        Mix_FreeMusic(menuThemeMusic);
        menuThemeMusic = NULL;
    }
}

static void freeGameOverThemeMusicOnly(void) {
    if (gameOverThemeMusic != NULL) {
        Mix_FreeMusic(gameOverThemeMusic);
        gameOverThemeMusic = NULL;
    }
    gameOverThemeActive = 0;
}

static void freeLevelSelectThemeMusicOnly(void) {
    if (levelSelectThemeMusic != NULL) {
        Mix_FreeMusic(levelSelectThemeMusic);
        levelSelectThemeMusic = NULL;
    }
    levelSelectThemeActive = 0;
}

void amigaHostMusicStopTitleTheme(void) {
    if (!musicSubsystemReady) {
        return;
    }
    if (titleThemeMusic == NULL) {
        return;
    }
    Mix_HaltMusic();
    freeTitleThemeMusicOnly();
}

void amigaHostMusicStopMenuTheme(void) {
    if (!musicSubsystemReady) {
        return;
    }
    if (menuThemeMusic == NULL) {
        return;
    }
    Mix_HaltMusic();
    freeMenuThemeMusicOnly();
}

void amigaHostMusicStopGameOverTheme(void) {
    if (!musicSubsystemReady) {
        return;
    }
    if (gameOverThemeMusic == NULL && !gameOverThemeActive) {
        return;
    }
    Mix_HaltMusic();
    freeGameOverThemeMusicOnly();
}

void amigaHostMusicStopLevelSelectTheme(void) {
    if (!musicSubsystemReady) {
        return;
    }
    if (levelSelectThemeMusic == NULL && !levelSelectThemeActive) {
        return;
    }
    Mix_HaltMusic();
    freeLevelSelectThemeMusicOnly();
}

void amigaHostMusicFadeOut(int ms) {
    if (!musicSubsystemReady) {
        return;
    }
    if (ms <= 0) {
        Mix_HaltMusic();
        return;
    }
    (void)Mix_FadeOutMusic(ms);
}

#if SDL_MIXER_VERSION_ATLEAST(2, 6, 0)
static void logLoadedMusicLoopMeta(Mix_Music* mus, char* label) {
    /* debug output disabled for now
    double loopStart;
    double loopEnd;
    double loopLen;
    double duration;

    if (mus == NULL) {
        return;
    }
    loopStart = Mix_GetMusicLoopStartTime(mus);
    loopEnd = Mix_GetMusicLoopEndTime(mus);
    loopLen = Mix_GetMusicLoopLengthTime(mus);
    duration = Mix_MusicDuration(mus);
    fprintf(stderr,
            "amigaHostMusic: %s SDL_mixer loop query: "
            "Mix_GetMusicLoopStartTime=%.6f Mix_GetMusicLoopEndTime=%.6f "
            "Mix_GetMusicLoopLengthTime=%.6f Mix_MusicDuration=%.6f (seconds; "
            "-1.0 means unsupported or not set for this stream)\n",
            label, loopStart, loopEnd, loopLen, duration);
            */
}
#endif

static Mix_Chunk* loadWavTryPaths(char* primaryPath) {
    Mix_Chunk* ch;
    char* exeBase;
    char buf[768];
    int n;

    ch = Mix_LoadWAV(primaryPath);
    if (ch != NULL) {
        return ch;
    }
    if (pathLooksAbsolute(primaryPath)) {
        return NULL;
    }
    exeBase = SDL_GetBasePath();
    if (exeBase == NULL) {
        return NULL;
    }
    n = snprintf(buf, sizeof(buf), "%s%s", exeBase, primaryPath);
    SDL_free(exeBase);
    if (n <= 0 || (size_t)n >= sizeof(buf)) {
        return NULL;
    }
    return Mix_LoadWAV(buf);
}

void amigaHostMusicPlayOneShotWav(char* path) {
    Mix_Chunk* ch;
    int c;
    int chNum;

    if (!musicSubsystemReady || path == NULL || path[0] == '\0') {
        return;
    }
    ch = loadWavTryPaths(path);
    if (ch == NULL) {
        fprintf(stderr,
                "amigaHostMusic: one-shot WAV: Mix_LoadWAV failed for \"%s\": %s\n",
                path,
                Mix_GetError());
        return;
    }
    for (c = HOI_MIXER_SFX_CHANNEL_FIRST; c <= HOI_MIXER_SFX_CHANNEL_LAST; c++) {
        if (Mix_Playing(c) == 0) {
            chNum = Mix_PlayChannel(c, ch, 0);
            if (chNum == c) {
                return;
            }
        }
    }
    (void)Mix_HaltChannel(HOI_MIXER_SFX_CHANNEL_FIRST);
    chNum = Mix_PlayChannel(HOI_MIXER_SFX_CHANNEL_FIRST, ch, 0);
    if (chNum < 0) {
        fprintf(stderr,
                "amigaHostMusic: one-shot WAV: Mix_PlayChannel failed for \"%s\": %s\n",
                path,
                Mix_GetError());
        Mix_FreeChunk(ch);
    }
}

void amigaHostMusicHaltOneShotSamples(void) {
    int c;

    if (!musicSubsystemReady) {
        return;
    }
    for (c = HOI_MIXER_SFX_CHANNEL_FIRST; c <= HOI_MIXER_SFX_CHANNEL_LAST; c++) {
        Mix_HaltChannel(c);
    }
}

static Mix_Music* loadMusTryPaths(char* primaryPath, char* exeRelativeSubpath) {
    Mix_Music* mus;
    char* exeBase;

    mus = Mix_LoadMUS(primaryPath);
    if (mus != NULL) {
        return mus;
    }
    exeBase = SDL_GetBasePath();
    if (exeBase == NULL) {
        return NULL;
    }
    {
        char buf[768];
        int n;

        n = snprintf(buf, sizeof(buf), "%s%s", exeBase, exeRelativeSubpath);
        SDL_free(exeBase);
        if (n <= 0 || (size_t)n >= sizeof(buf)) {
            return NULL;
        }
        return Mix_LoadMUS(buf);
    }
}

void amigaHostMusicPlayTitleTheme(void) {
    freeTitleThemeMusicOnly();
    amigaHostMusicStopGameOverTheme();
    amigaHostMusicStopLevelSelectTheme();

    if (!musicSubsystemReady) {
        return;
    }

    titleThemeMusic = loadMusTryPaths("data/music/title.ogg", "data/music/title.ogg");
    if (titleThemeMusic == NULL) {
        fprintf(stderr,
                "amigaHostMusic: title theme: Mix_LoadMUS failed for data/music/title.ogg: %s\n",
                Mix_GetError());
        return;
    }
#if SDL_MIXER_VERSION_ATLEAST(2, 6, 0)
    logLoadedMusicLoopMeta(titleThemeMusic, "title theme (data/music/title.ogg)");
#endif
    if (Mix_PlayMusic(titleThemeMusic, -1) != 0) {
        fprintf(stderr, "amigaHostMusic: Mix_PlayMusic(title) failed: %s\n",
                Mix_GetError());
        freeTitleThemeMusicOnly();
    }
}

void amigaHostMusicPlayMenuTheme(void) {
    if (!musicSubsystemReady) {
        return;
    }
    amigaHostMusicStopGameOverTheme();
    amigaHostMusicStopLevelSelectTheme();
    if (menuThemeMusic != NULL && Mix_PlayingMusic()) {
        return;
    }
    if (menuThemeMusic == NULL) {
        menuThemeMusic = loadMusTryPaths("data/music/menu.ogg", "data/music/menu.ogg");
        if (menuThemeMusic == NULL) {
            fprintf(stderr,
                    "amigaHostMusic: menu theme: Mix_LoadMUS failed for data/music/menu.ogg: %s\n",
                    Mix_GetError());
            return;
        }
#if SDL_MIXER_VERSION_ATLEAST(2, 6, 0)
        logLoadedMusicLoopMeta(menuThemeMusic, "menu theme (data/music/menu.ogg)");
#endif
    }
    if (Mix_PlayMusic(menuThemeMusic, -1) != 0) {
        fprintf(stderr, "amigaHostMusic: Mix_PlayMusic(menu) failed: %s\n",
                Mix_GetError());
        freeMenuThemeMusicOnly();
    }
}

void amigaHostMusicPlayGameOverTheme(void) {
    if (!musicSubsystemReady) {
        return;
    }
    amigaHostMusicStopTitleTheme();
    amigaHostMusicStopMenuTheme();
    amigaHostMusicStopLevelSelectTheme();
    unloadCurrentMusic();
    Mix_HaltMusic();
    freeGameOverThemeMusicOnly();
    gameOverThemeMusic =
        loadMusTryPaths("data/music/gameover.ogg", "data/music/gameover.ogg");
    if (gameOverThemeMusic == NULL) {
        fprintf(stderr,
                "amigaHostMusic: game over theme: Mix_LoadMUS failed for "
                "data/music/gameover.ogg: %s\n",
                Mix_GetError());
        gameOverThemeActive = 0;
        return;
    }
#if SDL_MIXER_VERSION_ATLEAST(2, 6, 0)
    logLoadedMusicLoopMeta(gameOverThemeMusic,
                           "game over theme (data/music/gameover.ogg)");
#endif
    if (Mix_PlayMusic(gameOverThemeMusic, -1) != 0) {
        fprintf(stderr, "amigaHostMusic: Mix_PlayMusic(game over) failed: %s\n",
                Mix_GetError());
        freeGameOverThemeMusicOnly();
        return;
    }
    gameOverThemeActive = 1;
}

void amigaHostMusicPlayLevelSelectTheme(void) {
    if (!musicSubsystemReady) {
        return;
    }
    if (levelSelectThemeActive && levelSelectThemeMusic != NULL && Mix_PlayingMusic()) {
        return;
    }
    amigaHostMusicStopTitleTheme();
    amigaHostMusicStopMenuTheme();
    amigaHostMusicStopGameOverTheme();
    unloadCurrentMusic();
    if (levelSelectThemeMusic == NULL) {
        levelSelectThemeMusic =
            loadMusTryPaths("data/music/levelselection.ogg", "data/music/levelselection.ogg");
        if (levelSelectThemeMusic == NULL) {
            fprintf(stderr,
                    "amigaHostMusic: level select theme: Mix_LoadMUS failed for "
                    "data/music/levelselection.ogg: %s\n",
                    Mix_GetError());
            return;
        }
#if SDL_MIXER_VERSION_ATLEAST(2, 6, 0)
        logLoadedMusicLoopMeta(levelSelectThemeMusic,
                               "level select theme (data/music/levelselection.ogg)");
#endif
    }
    if (Mix_PlayMusic(levelSelectThemeMusic, -1) != 0) {
        fprintf(stderr, "amigaHostMusic: Mix_PlayMusic(level select) failed: %s\n",
                Mix_GetError());
        freeLevelSelectThemeMusicOnly();
        return;
    }
    levelSelectThemeActive = 1;
}

static int buildPathUnderExe(char* out, size_t outSize, char* exeBase,
                             char* subDir, char* fileName) {
    size_t bl;

    if (exeBase == NULL || fileName == NULL) {
        return -1;
    }
    bl = strlen(exeBase);
    if (bl == 0u) {
        return -1;
    }
    if (exeBase[bl - 1u] == '/' || exeBase[bl - 1u] == '\\') {
        return snprintf(out, outSize, "%s%s/%s", exeBase, subDir, fileName);
    }
    return snprintf(out, outSize, "%s/%s/%s", exeBase, subDir, fileName);
}

static Mix_Music* loadTrackFileAtPrefix(char* folder, uint32_t trackNr,
                                        char* pathOut, size_t pathOutSize) {
    int n;
    Mix_Music* mus;
    char* exeBase;
    char fileName[32];

    n = snprintfTrackOggFile(fileName, sizeof(fileName), trackNr);
    if (n <= 0 || (size_t)n >= sizeof(fileName)) {
        return NULL;
    }
    n = snprintf(pathOut, pathOutSize, "%s/%s", folder, fileName);
    if (n <= 0 || (size_t)n >= pathOutSize) {
        return NULL;
    }
    mus = Mix_LoadMUS(pathOut);
    if (mus != NULL) {
        return mus;
    }
    if (pathLooksAbsolute(folder)) {
        return NULL;
    }
    exeBase = SDL_GetBasePath();
    if (exeBase == NULL) {
        return NULL;
    }
    n = buildPathUnderExe(pathOut, pathOutSize, exeBase, folder, fileName);
    SDL_free(exeBase);
    if (n <= 0 || (size_t)n >= pathOutSize) {
        return NULL;
    }
    return Mix_LoadMUS(pathOut);
}

static Mix_Music* loadTrackFileExplicit(uint32_t trackNr, char* pathOut,
                                        size_t pathOutSize) {
    int n;
    Mix_Music* mus;
    char* exeBase;
    char fileName[32];

    n = snprintfTrackOggFile(fileName, sizeof(fileName), trackNr);
    if (n <= 0 || (size_t)n >= sizeof(fileName)) {
        return NULL;
    }
    n = snprintf(pathOut, pathOutSize, "%s/%s", musicBaseDir, fileName);
    if (n <= 0 || (size_t)n >= pathOutSize) {
        return NULL;
    }
    mus = Mix_LoadMUS(pathOut);
    if (mus != NULL) {
        return mus;
    }
    if (pathLooksAbsolute(musicBaseDir)) {
        return NULL;
    }
    exeBase = SDL_GetBasePath();
    if (exeBase == NULL) {
        return NULL;
    }
    n = buildPathUnderExe(pathOut, pathOutSize, exeBase, musicBaseDir, fileName);
    SDL_free(exeBase);
    if (n <= 0 || (size_t)n >= pathOutSize) {
        return NULL;
    }
    return Mix_LoadMUS(pathOut);
}

static void logDefaultMusicLoadFailure(uint32_t trackNr) {
    char cwdTry[768];
    char exeTry[768];
    char* exeBase;
    char fileName[32];
    int ne;

    ne = snprintfTrackOggFile(fileName, sizeof(fileName), trackNr);
    if (ne <= 0 || (size_t)ne >= sizeof(fileName)) {
        fileName[0] = '\0';
    }
    ne = snprintf(cwdTry, sizeof(cwdTry), "music/%s", fileName);
    if (ne <= 0 || (size_t)ne >= sizeof(cwdTry)) {
        cwdTry[0] = '\0';
    }
    exeTry[0] = '\0';
    exeBase = SDL_GetBasePath();
    if (exeBase != NULL && fileName[0] != '\0') {
        ne = buildPathUnderExe(exeTry, sizeof(exeTry), exeBase, "music",
                               fileName);
        SDL_free(exeBase);
        if (ne <= 0 || (size_t)ne >= sizeof(exeTry)) {
            exeTry[0] = '\0';
        }
    }
    fprintf(stderr,
            "amigaHostMusic: track %u: Mix_LoadMUS failed after trying \"%s\"",
            (unsigned)trackNr, cwdTry);
    if (exeTry[0] != '\0') {
        fprintf(stderr, " and \"%s\"", exeTry);
    }
    fprintf(stderr, ": %s\n", Mix_GetError());
}

static int buildSamplePathUnderExe(char* out, size_t outSize, char* exeBase,
                                   char* subDir, uint32_t sampleNr) {
    size_t bl;

    if (exeBase == NULL) {
        return -1;
    }
    bl = strlen(exeBase);
    if (bl == 0u) {
        return -1;
    }
    if (exeBase[bl - 1u] == '/' || exeBase[bl - 1u] == '\\') {
        return snprintf(out, outSize, "%s%s/%u.wav", exeBase, subDir,
                        (unsigned)sampleNr);
    }
    return snprintf(out, outSize, "%s/%s/%u.wav", exeBase, subDir,
                    (unsigned)sampleNr);
}

static Mix_Chunk* loadSampleFileAtPrefix(uint32_t sampleNr, char* pathOut,
                                           size_t pathOutSize) {
    int n;
    Mix_Chunk* ch;
    char* exeBase;

    n = snprintf(pathOut, pathOutSize, "data/music/%u.wav",
                 (unsigned)sampleNr);
    if (n <= 0 || (size_t)n >= pathOutSize) {
        return NULL;
    }
    ch = Mix_LoadWAV(pathOut);
    if (ch != NULL) {
        return ch;
    }
    exeBase = SDL_GetBasePath();
    if (exeBase == NULL) {
        return NULL;
    }
    n = buildSamplePathUnderExe(pathOut, pathOutSize, exeBase, "data/music",
                                sampleNr);
    SDL_free(exeBase);
    if (n <= 0 || (size_t)n >= pathOutSize) {
        return NULL;
    }
    return Mix_LoadWAV(pathOut);
}

static Mix_Chunk* loadSampleFileExplicit(uint32_t sampleNr, char* pathOut,
                                         size_t pathOutSize) {
    int n;
    Mix_Chunk* ch;
    char* exeBase;

    n = snprintf(pathOut, pathOutSize, "%s/%u.wav", musicBaseDir,
                 (unsigned)sampleNr);
    if (n <= 0 || (size_t)n >= pathOutSize) {
        return NULL;
    }
    ch = Mix_LoadWAV(pathOut);
    if (ch != NULL) {
        return ch;
    }
    if (pathLooksAbsolute(musicBaseDir)) {
        return NULL;
    }
    exeBase = SDL_GetBasePath();
    if (exeBase == NULL) {
        return NULL;
    }
    n = buildSamplePathUnderExe(pathOut, pathOutSize, exeBase, musicBaseDir,
                                sampleNr);
    SDL_free(exeBase);
    if (n <= 0 || (size_t)n >= pathOutSize) {
        return NULL;
    }
    return Mix_LoadWAV(pathOut);
}

static Mix_Chunk* getOrLoadSample(uint32_t sampleNr, char* pathOut,
                                  size_t pathOutSize) {
    int idx;
    Mix_Chunk* ch;

    idx = sampleCacheFind(sampleNr);
    if (idx >= 0) {
        return sampleCache[idx].chunk;
    }
    if (sampleCacheCount >= SAMPLE_CACHE_MAX) {
        fprintf(stderr,
                "amigaHostMusic: sample cache full, cannot load sample %u\n",
                (unsigned)sampleNr);
        return NULL;
    }

    pathOut[0] = '\0';
    if (musicBaseExplicit) {
        ch = loadSampleFileExplicit(sampleNr, pathOut, pathOutSize);
    } else {
        ch = loadSampleFileAtPrefix(sampleNr, pathOut, pathOutSize);
    }
    if (ch == NULL) {
        fprintf(stderr,
                "amigaHostMusic: sample %u: Mix_LoadWAV failed (last path "
                "\"%s\"): %s\n",
                (unsigned)sampleNr, pathOut, Mix_GetError());
        return NULL;
    }

    sampleCache[sampleCacheCount].sampleNr = sampleNr;
    sampleCache[sampleCacheCount].chunk = ch;
    sampleCacheCount++;
    return ch;
}

static int tryPlayTrack(uint32_t trackNr, int loops) {
    char path[768];

    if (!musicSubsystemReady) {
        return 0;
    }
    path[0] = '\0';
    amigaHostMusicStopTitleTheme();
    amigaHostMusicStopMenuTheme();
    amigaHostMusicStopGameOverTheme();
    amigaHostMusicStopLevelSelectTheme();
    unloadCurrentMusic();

    if (musicBaseExplicit) {
        loadedMusic = loadTrackFileExplicit(trackNr, path, sizeof(path));
    } else {
        loadedMusic =
            loadTrackFileAtPrefix("music", trackNr, path, sizeof(path));
    }

    if (loadedMusic == NULL) {
        if (musicBaseExplicit) {
            fprintf(stderr,
                    "amigaHostMusic: track %u: Mix_LoadMUS failed (last path "
                    "\"%s\"): %s\n",
                    (unsigned)trackNr, path, Mix_GetError());
        } else {
            logDefaultMusicLoadFailure(trackNr);
        }
        return 0;
    }
    if (Mix_PlayMusic(loadedMusic, loops) != 0) {
        fprintf(stderr, "amigaHostMusic: Mix_PlayMusic failed: %s\n",
                Mix_GetError());
        Mix_FreeMusic(loadedMusic);
        loadedMusic = NULL;
        return 0;
    }
    return 1;
}

void startMusic(uint32_t trackNr) {
    (void)tryPlayTrack(trackNr, 0);
}

void playLoopingMusic(uint32_t trackNr) {
    (void)tryPlayTrack(trackNr, -1);
}

static void haltChannelsPlayingChunk(Mix_Chunk* chunk) {
    int c;

    if (chunk == NULL) {
        return;
    }
    for (c = HOI_MIXER_SFX_CHANNEL_FIRST; c <= HOI_MIXER_SFX_CHANNEL_LAST; c++) {
        if (Mix_Playing(c) != 0 && Mix_GetChunk(c) == chunk) {
            Mix_HaltChannel(c);
        }
    }
}

static int playSampleOnFirstFreeChannel(Mix_Chunk* chunk) {
    int c;
    int channel;

    for (c = HOI_MIXER_SFX_CHANNEL_FIRST; c <= HOI_MIXER_SFX_CHANNEL_LAST; c++) {
        if (Mix_Playing(c) == 0) {
            channel = Mix_PlayChannel(c, chunk, 0);
            if (channel >= 0) {
                return channel;
            }
        }
    }
    return -1;
}

void playSample(uint32_t sampleNr) {
    Mix_Chunk* chunk;
    char path[768];
    int channel;

    if (!musicSubsystemReady) {
        return;
    }

    chunk = getOrLoadSample(sampleNr, path, sizeof(path));
    if (chunk == NULL) {
        return;
    }

    haltChannelsPlayingChunk(chunk);

    channel = playSampleOnFirstFreeChannel(chunk);
    if (channel < 0) {
        (void)Mix_HaltChannel(HOI_MIXER_SFX_CHANNEL_FIRST);
        channel = Mix_PlayChannel(HOI_MIXER_SFX_CHANNEL_FIRST, chunk, 0);
        if (channel < 0) {
            fprintf(stderr,
                    "amigaHostMusic: sample %u: Mix_PlayChannel failed: %s\n",
                    (unsigned)sampleNr, Mix_GetError());
        }
    }
}

void stopMusic(void) {
    if (!musicSubsystemReady) {
        return;
    }
    unloadCurrentMusic();
}

void amigaHostMusicStopEverything(void) {
    if (!musicSubsystemReady) {
        return;
    }
    amigaHostMusicHaltOneShotSamples();
    unloadCurrentMusic();
    freeTitleThemeMusicOnly();
    freeMenuThemeMusicOnly();
    freeGameOverThemeMusicOnly();
    freeLevelSelectThemeMusicOnly();
    gameOverThemeActive      = 0;
    levelSelectThemeActive = 0;
}

#else

void amigaHostMusicSetBasePath(char* path) {
    (void)path;
}

void amigaHostMusicSetCdMode(int cdEnabled) {
    (void)cdEnabled;
}

void amigaHostMusicInit(void) {}

void amigaHostMusicShutdown(void) {}

void amigaHostMusicPlayTitleTheme(void) {}

void amigaHostMusicStopTitleTheme(void) {}

void amigaHostMusicPlayMenuTheme(void) {}

void amigaHostMusicStopMenuTheme(void) {}

void amigaHostMusicPlayGameOverTheme(void) {}

void amigaHostMusicStopGameOverTheme(void) {}

void amigaHostMusicPlayLevelSelectTheme(void) {}

void amigaHostMusicStopLevelSelectTheme(void) {}

void amigaHostMusicFadeOut(int ms) {
    (void)ms;
}

void startMusic(uint32_t trackNr) {
    (void)trackNr;
}

void playLoopingMusic(uint32_t trackNr) {
    (void)trackNr;
}

void stopMusic(void) {}

void amigaHostMusicStopEverything(void) {}

void amigaHostMusicPlayOneShotWav(char* path) {
    (void)path;
}

void amigaHostMusicHaltOneShotSamples(void) {}

void playSample(uint32_t sampleNr) {
    (void)sampleNr;
}

#endif
