#ifndef AMIGA_HOST_MUSIC_H
#define AMIGA_HOST_MUSIC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void amigaHostMusicInit(void);
void amigaHostMusicShutdown(void);
void amigaHostMusicSetBasePath(char* path);
void amigaHostMusicSetCdMode(int cdEnabled);

void amigaHostMusicPlayTitleTheme(void);
void amigaHostMusicStopTitleTheme(void);
void amigaHostMusicPlayMenuTheme(void);
void amigaHostMusicStopMenuTheme(void);
void amigaHostMusicFadeOut(int ms);
void amigaHostMusicPlayGameOverTheme(void);
void amigaHostMusicStopGameOverTheme(void);
void amigaHostMusicPlayLevelSelectTheme(void);
void amigaHostMusicStopLevelSelectTheme(void);

void amigaHostMusicStopEverything(void);
void amigaHostMusicPlayOneShotWav(char* path);
void amigaHostMusicHaltOneShotSamples(void);
void startMusic(uint32_t trackNr);
void stopMusic(void);
void playLoopingMusic(uint32_t trackNr);
void playSample(uint32_t sampleNr);

#ifdef __cplusplus
}
#endif

#endif
