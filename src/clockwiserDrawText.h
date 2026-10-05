#ifndef CLOCKWISER_DRAW_TEXT_H
#define CLOCKWISER_DRAW_TEXT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLOCKWISER_FONT_CHAR_COUNT 80
#define CLOCKWISER_FONT_BYTES       ((size_t)CLOCKWISER_FONT_CHAR_COUNT * 8u)

typedef enum {
    drawTextColorWhite,
    drawTextColorBlue,
    drawTextColorGreen
} DrawTextColor;

int loadCharsetFromFile(char* path, uint8_t* charSetOut);
void drawText(uint32_t* frameBuffer,
              int fbWidth,
              int fbHeight,
              uint8_t* charSet,
              int x,
              int y,
              int pixelScale,
              DrawTextColor color,
              float brightness,
              float opacity,
              char* text);

#ifdef __cplusplus
}
#endif

#endif
