#ifndef AMIGA_HOST_SDL_H
#define AMIGA_HOST_SDL_H

#include <stdint.h>

#include "m68k_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    amigaHostScaleModeNormal = 0,
    amigaHostScaleModeXbr    = 1,
    amigaHostScaleModeXbrz   = 2
} AmigaHostScaleMode;

#define AMIGA_HOST_WHEEL_QUEUE_MAX 32

typedef struct {
    void* window;
    void* renderer;
    void* texture;
    uint32_t* frameBuffer;
    uint8_t* playfieldIndexBuffer;
    int width;
    int height;
    int running;
    uint32_t frameCounter;
    uint32_t frameCounterPeakPlanes;
    uint16_t frameCounterPeakBplcon0;
    uint16_t frameCounterPeakBplcon4;
    uint8_t mouseXCounter;
    uint8_t mouseYCounter;
    int mouseJoy0Enabled;
    M68k* cpu;
    int textureWidth;
    int textureHeight;
    int scaleMode;
    uint32_t* scaledFrameBuffer;
    int scaledFrameWidth;
    int scaledFrameHeight;
    double lastScaleMs;
    double lastFrameMs;
    int showPerfHud;
    int wheelVblank;
    int wheelQueue[AMIGA_HOST_WHEEL_QUEUE_MAX];
    int wheelQueueRead;
    int wheelQueueWrite;
    int wheelQueueCount;
} AmigaHostSdl;

int amigaHostSdlInit(AmigaHostSdl* host, M68k* cpu, int width, int height, int scale);
void amigaHostSdlEnableMouseJoy0(AmigaHostSdl* host);
void amigaHostSdlPollEvents(AmigaHostSdl* host,
                            int* shellAdvanceLatchOut,
                            int* shellBackLatchOut,
                            int* shellPauseMenuLatchOut,
                            int* shellOtherKeyLatchOut,
    int* shellTextBackspaceOut,
    int* shellTextSubmitOut,
    char* shellTextCharOut,
    int textEntryMode,
    int* shellMenuUpOut,
    int* shellMenuDownOut,
    int* shellMouseFbXOut,
    int* shellMouseFbYOut);
void getMouseXY(void);
void getPreference(uint32_t prefNr);
void mouseWheel(void);
typedef void (*AmigaHostSdlFrameOverlayFn)(AmigaHostSdl* host, void* userData);
void amigaHostSdlPresentFrame(AmigaHostSdl* host);
void amigaHostSdlPresentFrameEx(AmigaHostSdl* host,
                                AmigaHostSdlFrameOverlayFn overlayFn,
                                void* overlayUserData);
void amigaHostSdlPresentFramebufferOnly(AmigaHostSdl* host);
void amigaHostSdlSetScaleMode(AmigaHostSdl* host, int scaleMode);
int amigaHostSdlGetScaleMode(AmigaHostSdl* host);
char amigaHostSdlGetTextEntryCandidate(void);
void amigaHostSdlShutdown(AmigaHostSdl* host);
int amigaHostSdlDumpCopperToFile(AmigaHostSdl* host, char* path);

#ifdef __cplusplus
}
#endif

#endif
