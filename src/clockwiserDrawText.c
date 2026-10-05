#include "clockwiserDrawText.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static uint16_t gBlueCopperRowRgb12[8] = {
    0x006Fu,
    0x00AFu,
    0x00FFu,
    0x0FFFu,
    0x00FFu,
    0x00AFu,
    0x006Fu,
    0x000Fu
};

static uint32_t applyBrightnessToArgb(uint32_t argb, float brightness) {
    float r;
    float g;
    float b;
    float br;

    br = brightness;
    if (br < -1.0f) {
        br = -1.0f;
    }
    if (br > 1.0f) {
        br = 1.0f;
    }
    r = (float)((argb >> 16) & 0xFFu);
    g = (float)((argb >> 8) & 0xFFu);
    b = (float)(argb & 0xFFu);
    if (br <= 0.0f) {
        float dim;

        dim = 1.0f + br;
        r *= dim;
        g *= dim;
        b *= dim;
    } else {
        r += (255.0f - r) * br;
        g += (255.0f - g) * br;
        b += (255.0f - b) * br;
    }
    if (r < 0.0f) {
        r = 0.0f;
    }
    if (r > 255.0f) {
        r = 255.0f;
    }
    if (g < 0.0f) {
        g = 0.0f;
    }
    if (g > 255.0f) {
        g = 255.0f;
    }
    if (b < 0.0f) {
        b = 0.0f;
    }
    if (b > 255.0f) {
        b = 255.0f;
    }
    return 0xFF000000u | ((uint32_t)roundf(r) << 16u) | ((uint32_t)roundf(g) << 8u)
           | (uint32_t)roundf(b);
}

static uint32_t rgb12ToArgb8888(uint16_t rgb12) {
    uint32_t r;
    uint32_t g;
    uint32_t b;

    r = (uint32_t)((rgb12 >> 8) & 0x0Fu);
    g = (uint32_t)((rgb12 >> 4) & 0x0Fu);
    b = (uint32_t)(rgb12 & 0x0Fu);
    r = (r << 4u) | r;
    g = (g << 4u) | g;
    b = (b << 4u) | b;
    return 0xFF000000u | (r << 16u) | (g << 8u) | b;
}

static uint16_t rgb12ForRow(DrawTextColor color, int row) {
    int r;

    r = row;
    if (r < 0) {
        r = 0;
    }
    if (r > 7) {
        r = 7;
    }
    switch (color) {
    case drawTextColorWhite:
        return 0x0FFFu;
    case drawTextColorBlue:
        return gBlueCopperRowRgb12[r];
    case drawTextColorGreen:
        return 0x04D2u;
    default:
        return 0x0FFFu;
    }
}

static void writePixelOpaque(uint32_t* frameBuffer,
                             int fbWidth,
                             int fbHeight,
                             int px,
                             int py,
                             uint32_t argb) {
    if (px < 0 || py < 0 || px >= fbWidth || py >= fbHeight) {
        return;
    }
    frameBuffer[(size_t)py * (size_t)fbWidth + (size_t)px] = argb;
}

static void writePixelBlended(uint32_t* frameBuffer,
                              int fbWidth,
                              int fbHeight,
                              int px,
                              int py,
                              uint32_t fgArgb,
                              float opacity) {
    size_t idx;
    uint32_t bgArgb;
    float r;
    float g;
    float b;
    float a;
    float bgR;
    float bgG;
    float bgB;

    if (px < 0 || py < 0 || px >= fbWidth || py >= fbHeight) {
        return;
    }
    a = opacity;
    if (a <= 0.0f) {
        return;
    }
    if (a >= 1.0f) {
        writePixelOpaque(frameBuffer, fbWidth, fbHeight, px, py, fgArgb);
        return;
    }
    idx    = (size_t)py * (size_t)fbWidth + (size_t)px;
    bgArgb = frameBuffer[idx];
    r      = (float)((fgArgb >> 16) & 0xFFu);
    g      = (float)((fgArgb >> 8) & 0xFFu);
    b      = (float)(fgArgb & 0xFFu);
    bgR    = (float)((bgArgb >> 16) & 0xFFu);
    bgG    = (float)((bgArgb >> 8) & 0xFFu);
    bgB    = (float)(bgArgb & 0xFFu);
    r      = bgR + (r - bgR) * a;
    g      = bgG + (g - bgG) * a;
    b      = bgB + (b - bgB) * a;
    frameBuffer[idx] = 0xFF000000u | ((uint32_t)roundf(r) << 16u) | ((uint32_t)roundf(g) << 8u)
                       | (uint32_t)roundf(b);
}

int loadCharsetFromFile(char* path, uint8_t* charSetOut) {
    FILE* fp;
    size_t n;

    assert(path != NULL);
    assert(charSetOut != NULL);
    fp = fopen(path, "rb");
    if (fp == NULL) {
        return 0;
    }
    n = fread(charSetOut, 1u, CLOCKWISER_FONT_BYTES, fp);
    fclose(fp);
    if (n != CLOCKWISER_FONT_BYTES) {
        return 0;
    }
    return 1;
}

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
              char* text) {
    int penX;
    char* p;
    int scale;

    assert(frameBuffer != NULL);
    assert(fbWidth > 0);
    assert(fbHeight > 0);
    assert(charSet != NULL);
    assert(text != NULL);
    if (opacity < 0.0f) {
        return;
    }
    scale = pixelScale;
    if (scale < 1) {
        scale = 1;
    }

    penX = x;
    for (p = text; *p != '\0'; p++) {
        int code;
        int row;
        int base;
        unsigned char uc;

        uc = (unsigned char)(*p);
        code = (int)uc;
        if (code < 32) {
            code = 32;
        }
        if (code > 32 + CLOCKWISER_FONT_CHAR_COUNT - 1) {
            code = 32;
        }
        base = (code - 32);
        for (row = 0; row < 8; row++) {
            int col;
            uint8_t bits;
            uint32_t rowArgb;

            rowArgb = applyBrightnessToArgb(rgb12ToArgb8888(rgb12ForRow(color, row)), brightness);
            bits    = charSet[(size_t)base + (size_t)row * 80u];
            for (col = 0; col < 8; col++) {
                int bitMask;

                bitMask = 0x80 >> col;
                if ((bits & (uint8_t)bitMask) != 0u) {
                    int sy;
                    int sx;

                    for (sy = 0; sy < scale; sy++) {
                        for (sx = 0; sx < scale; sx++) {
                            if (opacity >= 1.0f) {
                                writePixelOpaque(frameBuffer,
                                                 fbWidth,
                                                 fbHeight,
                                                 penX + col * scale + sx,
                                                 y + row * scale + sy,
                                                 rowArgb);
                            } else {
                                writePixelBlended(frameBuffer,
                                                  fbWidth,
                                                  fbHeight,
                                                  penX + col * scale + sx,
                                                  y + row * scale + sy,
                                                  rowArgb,
                                                  opacity);
                            }
                        }
                    }
                }
            }
        }
        penX += 8 * scale;
    }
}
