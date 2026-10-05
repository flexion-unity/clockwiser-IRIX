#ifndef CLOCKWISER_MOVIE_PLAYER_H
#define CLOCKWISER_MOVIE_PLAYER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MoviePlayer MoviePlayer;

typedef void (*MovieDoneCallback)(int naturalEnd, void* userData);

typedef struct MovieFrameSoundCue {
    int   frameIndex;
    char* path;
} MovieFrameSoundCue;

MoviePlayer* moviePlayerCreate(void);
void moviePlayerDestroy(MoviePlayer* mp);
void moviePlayerClose(MoviePlayer* mp);

int moviePlayerPlayFile(MoviePlayer* mp,
                        char* filePath,
                        MovieDoneCallback doneCallback,
                        void* userData,
                        int targetWidth,
                        int targetHeight,
                        MovieFrameSoundCue* frameSoundCues,
                        int frameSoundCueCount);

void moviePlayerStop(MoviePlayer* mp, int naturalEnd);

int moviePlayerIsPlaying(MoviePlayer* mp);

void moviePlayerUpdate(MoviePlayer* mp, uint8_t* pixels, int width, int height, int pitch);
int moviePlayerGetDisplayedFrameIndex(MoviePlayer* mp);

#ifdef __cplusplus
}
#endif

#endif
