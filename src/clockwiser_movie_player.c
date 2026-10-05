#include "clockwiser_movie_player.h"

#include "amigaHostMusic.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#elif __has_include(<SDL.h>)
#include <SDL.h>
#include <SDL_image.h>
#else
#error "SDL headers not found"
#endif

#define CLOCKWISER_MIN(a, b) ((a) < (b) ? (a) : (b))

struct MoviePlayer {
    IMG_Animation* anim;
    int targetWidth;
    int targetHeight;
    uint32_t* scaledScratch;
    int frameIndex;
    uint64_t nextSwitchTickMs;
    int lastDisplayedFrameIndex;
    MovieFrameSoundCue* frameSoundCues;
    int frameSoundCueCount;
    MovieDoneCallback doneCallback;
    void* doneUserData;
    int playing;
};

static void invokeDoneCallback(MoviePlayer* mp, int naturalEnd) {
    MovieDoneCallback cb;
    void* ud;

    cb = mp->doneCallback;
    ud = mp->doneUserData;
    mp->doneCallback = NULL;
    mp->doneUserData = NULL;
    if (cb != NULL) {
        cb(naturalEnd, ud);
    }
}

static void clearAnimation(MoviePlayer* mp) {
    if (mp->anim != NULL) {
        IMG_FreeAnimation(mp->anim);
        mp->anim = NULL;
    }
    if (mp->scaledScratch != NULL) {
        free(mp->scaledScratch);
        mp->scaledScratch = NULL;
    }
    mp->frameIndex           = 0;
    mp->nextSwitchTickMs     = 0u;
    mp->playing              = 0;
    mp->frameSoundCues       = NULL;
    mp->frameSoundCueCount   = 0;
    amigaHostMusicHaltOneShotSamples();
}

static int gifDelayMs(IMG_Animation* anim, int idx) {
    int d;

    if (anim == NULL || anim->delays == NULL || idx < 0 || idx >= anim->count) {
        return 100;
    }
    d = anim->delays[idx];
    if (d <= 0) {
        return 100;
    }
    return d;
}

static void nearestScaleArgb(uint32_t* src, int sw, int sh, uint32_t* dst, int dw, int dh) {
    int x;
    int y;

    assert(src && dst && sw >= 1 && sh >= 1 && dw >= 1 && dh >= 1);
    for (y = 0; y < dh; y++) {
        int sy;

        sy = (int)((int64_t)y * (int64_t)sh / (int64_t)dh);
        if (sy >= sh) {
            sy = sh - 1;
        }
        for (x = 0; x < dw; x++) {
            int sx;

            sx = (int)((int64_t)x * (int64_t)sw / (int64_t)dw);
            if (sx >= sw) {
                sx = sw - 1;
            }
            dst[y * dw + x] = src[sy * sw + sx];
        }
    }
}

static int surfaceToArgbPixels(SDL_Surface* surf, uint32_t** outPixels, int* outW, int* outH) {
    SDL_Surface* conv;
    int y;
    uint32_t* buf;

    assert(surf && outPixels && outW && outH);
    *outPixels = NULL;
    *outW      = 0;
    *outH      = 0;
    conv = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_ARGB8888, 0);
    if (conv == NULL) {
        fprintf(stderr, "movie: SDL_ConvertSurfaceFormat: %s\n", SDL_GetError());
        return 0;
    }
    if (conv->w < 1 || conv->h < 1) {
        SDL_FreeSurface(conv);
        return 0;
    }
    buf = (uint32_t*)malloc((size_t)conv->w * (size_t)conv->h * sizeof(uint32_t));
    if (buf == NULL) {
        SDL_FreeSurface(conv);
        return 0;
    }
    if (SDL_LockSurface(conv) != 0) {
        fprintf(stderr, "movie: SDL_LockSurface: %s\n", SDL_GetError());
        free(buf);
        SDL_FreeSurface(conv);
        return 0;
    }
    for (y = 0; y < conv->h; y++) {
        uint8_t* srcRow;

        srcRow = (uint8_t*)conv->pixels + (size_t)y * (size_t)conv->pitch;
        memcpy(buf + (size_t)y * (size_t)conv->w, srcRow, (size_t)conv->w * sizeof(uint32_t));
    }
    SDL_UnlockSurface(conv);
    *outPixels = buf;
    *outW      = conv->w;
    *outH      = conv->h;
    SDL_FreeSurface(conv);
    return 1;
}

static void maybePlayFrameSoundCues(MoviePlayer* mp, int prevDisplayed, int newDisplayed) {
    int i;

    if (prevDisplayed == newDisplayed) {
        return;
    }
    if (mp->frameSoundCues == NULL || mp->frameSoundCueCount < 1) {
        return;
    }
    for (i = 0; i < mp->frameSoundCueCount; i++) {
        if (mp->frameSoundCues[i].path != NULL
            && mp->frameSoundCues[i].frameIndex == newDisplayed) {
            amigaHostMusicPlayOneShotWav(mp->frameSoundCues[i].path);
        }
    }
}

static void presentCurrentFrame(MoviePlayer* mp, uint8_t* pixels, int width, int height, int pitch) {
    SDL_Surface* fr;
    uint32_t* srcPx;
    int sw;
    int sh;
    int copyWidth;
    int copyHeight;
    int xOffset;
    int yOffset;
    int y;

    if (mp->anim == NULL || mp->anim->frames == NULL || mp->frameIndex < 0
        || mp->frameIndex >= mp->anim->count) {
        return;
    }
    if (mp->scaledScratch == NULL || mp->targetWidth < 1 || mp->targetHeight < 1) {
        return;
    }
    fr = mp->anim->frames[mp->frameIndex];
    if (fr == NULL) {
        return;
    }
    srcPx = NULL;
    sw    = 0;
    sh    = 0;
    if (!surfaceToArgbPixels(fr, &srcPx, &sw, &sh)) {
        return;
    }
    nearestScaleArgb(srcPx, sw, sh, mp->scaledScratch, mp->targetWidth, mp->targetHeight);
    free(srcPx);

    memset(pixels, 0, (size_t)pitch * (size_t)height);

    copyWidth  = CLOCKWISER_MIN(width, mp->targetWidth);
    copyHeight = CLOCKWISER_MIN(height, mp->targetHeight);
    xOffset    = (width - copyWidth) / 2;
    yOffset    = (height - copyHeight) / 2;

    for (y = 0; y < copyHeight; y++) {
        uint8_t* dst;

        dst = pixels + (size_t)(y + yOffset) * (size_t)pitch + (size_t)xOffset * 4u;
        memcpy(dst,
               mp->scaledScratch + (size_t)y * (size_t)mp->targetWidth,
               (size_t)copyWidth * sizeof(uint32_t));
    }
    {
        int prev;

        prev = mp->lastDisplayedFrameIndex;
        mp->lastDisplayedFrameIndex = mp->frameIndex;
        maybePlayFrameSoundCues(mp, prev, mp->lastDisplayedFrameIndex);
    }
}

MoviePlayer* moviePlayerCreate(void) {
    MoviePlayer* mp;

    mp = (MoviePlayer*)calloc(1u, sizeof(MoviePlayer));
    if (mp != NULL) {
        mp->lastDisplayedFrameIndex = -1;
    }
    return mp;
}

void moviePlayerClose(MoviePlayer* mp) {
    if (mp == NULL) {
        return;
    }
    mp->doneCallback = NULL;
    mp->doneUserData = NULL;
    clearAnimation(mp);
}

void moviePlayerDestroy(MoviePlayer* mp) {
    if (mp == NULL) {
        return;
    }
    moviePlayerClose(mp);
    free(mp);
}

void moviePlayerStop(MoviePlayer* mp, int naturalEnd) {
    if (mp == NULL) {
        return;
    }
    if (!mp->playing && mp->anim == NULL) {
        return;
    }
    clearAnimation(mp);
    invokeDoneCallback(mp, naturalEnd);
}

int moviePlayerIsPlaying(MoviePlayer* mp) {
    if (mp == NULL) {
        return 0;
    }
    return mp->playing;
}

int moviePlayerGetDisplayedFrameIndex(MoviePlayer* mp) {
    if (mp == NULL) {
        return -1;
    }
    return mp->lastDisplayedFrameIndex;
}

int moviePlayerPlayFile(MoviePlayer* mp,
                        char* filePath,
                        MovieDoneCallback doneCallback,
                        void* userData,
                        int targetWidth,
                        int targetHeight,
                        MovieFrameSoundCue* frameSoundCues,
                        int frameSoundCueCount) {
    uint64_t nowMs;
    size_t scratchBytes;

    if (mp == NULL || filePath == NULL) {
        return 0;
    }

    amigaHostMusicStopEverything();
    moviePlayerClose(mp);
    mp->lastDisplayedFrameIndex = -1;

    mp->doneCallback = doneCallback;
    mp->doneUserData = userData;
    if (frameSoundCues != NULL && frameSoundCueCount > 0) {
        mp->frameSoundCues     = frameSoundCues;
        mp->frameSoundCueCount = frameSoundCueCount;
    } else {
        mp->frameSoundCues     = NULL;
        mp->frameSoundCueCount = 0;
    }

    if (targetWidth < 1 || targetHeight < 1) {
        mp->targetWidth  = 640;
        mp->targetHeight = 480;
    } else {
        mp->targetWidth  = targetWidth;
        mp->targetHeight = targetHeight;
    }

    mp->anim = IMG_LoadAnimation(filePath);
    if (mp->anim == NULL) {
        fprintf(stderr,
                "movie: could not load \"%s\": %s (need SDL_image 2.6+ with GIF animation)\n",
                filePath,
                IMG_GetError());
        moviePlayerClose(mp);
        return 0;
    }
    if (mp->anim->count < 1 || mp->anim->frames == NULL) {
        fprintf(stderr, "movie: no frames in \"%s\"\n", filePath);
        moviePlayerClose(mp);
        return 0;
    }

    scratchBytes = (size_t)mp->targetWidth * (size_t)mp->targetHeight * sizeof(uint32_t);
    mp->scaledScratch = (uint32_t*)malloc(scratchBytes);
    if (mp->scaledScratch == NULL) {
        fprintf(stderr, "movie: out of memory for \"%s\"\n", filePath);
        moviePlayerClose(mp);
        return 0;
    }

    mp->frameIndex       = 0;
    mp->playing          = 1;
    nowMs                = SDL_GetTicks64();
    mp->nextSwitchTickMs = nowMs + (uint64_t)gifDelayMs(mp->anim, 0);

    return 1;
}

void moviePlayerUpdate(MoviePlayer* mp, uint8_t* pixels, int width, int height, int pitch) {
    uint64_t nowMs;

    if (mp == NULL || !mp->playing || mp->anim == NULL) {
        return;
    }

    nowMs = SDL_GetTicks64();

    for (;;) {
        if (nowMs < mp->nextSwitchTickMs) {
            break;
        }
        if (mp->frameIndex >= mp->anim->count - 1) {
            presentCurrentFrame(mp, pixels, width, height, pitch);
            moviePlayerStop(mp, 1);
            return;
        }
        mp->frameIndex++;
        presentCurrentFrame(mp, pixels, width, height, pitch);
        mp->nextSwitchTickMs += (uint64_t)gifDelayMs(mp->anim, mp->frameIndex);
    }

    presentCurrentFrame(mp, pixels, width, height, pitch);
}
