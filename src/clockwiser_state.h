#ifndef CLOCKWISER_STATE_H
#define CLOCKWISER_STATE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

int clockwiserStateLoad(int* fullscreenEnabledOut, int* scalerModeOut, int* timeEndlessOut,
                        int* musicCdOut);
int clockwiserStateSave(int fullscreenEnabled, int scalerMode, int timeEndless,
                        int musicCd);
int clockwiserStateGetTimeEndless(void);
void clockwiserStateSetTimeEndless(int enabled);
int clockwiserStateGetMusicCd(void);
void clockwiserStateSetMusicCd(int enabled);

#ifdef __cplusplus
}
#endif

#endif
