#include "amigaHostSdl.h"
#include "amigaHostMusic.h"
#include "clockwiser_state.h"

#include <stdlib.h>

#if defined(AMIGA_HOST_HAVE_SDL2_MIXER)
static int hostSdlAudioSubsystemOpened;
#endif
/* Active host for transpiled CALL hooks (getMouseXY, ...). */
static AmigaHostSdl* callHookHost;

static void wheelEnqueue(AmigaHostSdl* host, int direction) {
    if (host == NULL || direction == 0) {
        return;
    }
    if (host->wheelQueueCount >= AMIGA_HOST_WHEEL_QUEUE_MAX) {
        return;
    }
    host->wheelQueue[host->wheelQueueWrite] = direction;
    host->wheelQueueWrite =
        (host->wheelQueueWrite + 1) % AMIGA_HOST_WHEEL_QUEUE_MAX;
    host->wheelQueueCount++;
}

static void wheelOnVblank(M68k* cpu) {
    AmigaHostSdl* host;

    (void)cpu;
    host = callHookHost;
    if (host == NULL) {
        return;
    }

    if (host->wheelQueueCount > 0) {
        host->wheelVblank = host->wheelQueue[host->wheelQueueRead];
        host->wheelQueueRead =
            (host->wheelQueueRead + 1) % AMIGA_HOST_WHEEL_QUEUE_MAX;
        host->wheelQueueCount--;
    } else {
        host->wheelVblank = 0;
    }
}

#include <string.h>
#include <time.h>
#include <stdio.h>

#include <assert.h>

#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#elif __has_include(<SDL.h>)
#include <SDL.h>
#else
#error "SDL headers not found"
#endif

/*
 * Game controller state. We hold one active SDL_GameController at a time
 * (whichever was plugged in first or hot-plugged in last). This mirrors the
 * keyboard handling: continuous polling for gameplay (CIA registers) plus
 * key-down style latches for shell navigation.
 *
 * gameControllerInstanceId is stored so SDL_CONTROLLERDEVICEREMOVED can be
 * matched against the controller we currently hold.
 */
static SDL_GameController* hostSdlGameController;
static SDL_JoystickID hostSdlGameControllerInstanceId;
static int hostSdlGameControllerSubsystemOpened;
static int hostSdlGameControllerStickDeadzone = 12000;

/*
 * Minimal controller-driven text entry: D-pad up/down cycles a candidate
 * character; A button commits the candidate as if it had been typed on the
 * keyboard; B button is backspace; auto-submit fires when the commit count
 * reaches the maximum name length (10).
 *
 * hostSdlTextCharset is the alphabet, deliberately matching the set that the
 * keyboard branch in this file accepts (A..Z, 0..9, space, '-').
 */
static char hostSdlTextCharset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -";
static int hostSdlTextCharsetLen = 38;
static int hostSdlTextNameMaxLen = 10;
static char hostSdlTextCandidate = 'A';
static int hostSdlTextCommitCount;
static int hostSdlTextEntryWasActive;

/*
 * Gamepad input feeds CIA joystick state. Keyboard is forwarded to the game
 * separately for text entry and other in-game key handling.
 */
static int hostSdlTextCharsetIndexOf(char c) {
    int i;
    for (i = 0; i < hostSdlTextCharsetLen; i++) {
        if (hostSdlTextCharset[i] == c) {
            return i;
        }
    }
    return 0;
}

static void hostSdlTextResetForNewSession(void) {
    hostSdlTextCandidate = 'A';
    hostSdlTextCommitCount = 0;
}

static void hostSdlTextCycleCandidate(int delta) {
    int idx;
    idx = hostSdlTextCharsetIndexOf(hostSdlTextCandidate);
    idx += delta;
    while (idx < 0) {
        idx += hostSdlTextCharsetLen;
    }
    while (idx >= hostSdlTextCharsetLen) {
        idx -= hostSdlTextCharsetLen;
    }
    hostSdlTextCandidate = hostSdlTextCharset[idx];
}

char amigaHostSdlGetTextEntryCandidate(void) {
    if (hostSdlGameController == NULL) {
        return '\0';
    }
    if (!hostSdlTextEntryWasActive) {
        return '\0';
    }
    return hostSdlTextCandidate;
}

static void hostSdlOpenFirstGameController(void) {
    int i;
    int count;

    if (hostSdlGameController != NULL) {
        return;
    }
    count = SDL_NumJoysticks();
    for (i = 0; i < count; i++) {
        if (SDL_IsGameController(i)) {
            SDL_GameController* gc;
            gc = SDL_GameControllerOpen(i);
            if (gc != NULL) {
                SDL_Joystick* joy;
                hostSdlGameController = gc;
                joy = SDL_GameControllerGetJoystick(gc);
                hostSdlGameControllerInstanceId = (joy != NULL)
                    ? SDL_JoystickInstanceID(joy)
                    : -1;
                fprintf(stderr,
                        "controller: opened '%s' (instance %d)\n",
                        SDL_GameControllerName(gc),
                        (int)hostSdlGameControllerInstanceId);
                return;
            }
        }
    }
}

static void hostSdlCloseGameController(void) {
    if (hostSdlGameController != NULL) {
        SDL_GameControllerClose(hostSdlGameController);
        hostSdlGameController = NULL;
        hostSdlGameControllerInstanceId = -1;
    }
}

static int hostSdlControllerButtonHeld(int sdlControllerButton) {
    if (hostSdlGameController == NULL) {
        return 0;
    }
    return SDL_GameControllerGetButton(hostSdlGameController,
                                       (SDL_GameControllerButton)sdlControllerButton)
           ? 1 : 0;
}

static int hostSdlControllerAxisInt(int sdlControllerAxis) {
    if (hostSdlGameController == NULL) {
        return 0;
    }
    return (int)SDL_GameControllerGetAxis(hostSdlGameController,
                                          (SDL_GameControllerAxis)sdlControllerAxis);
}

static int hostSdlControllerLeftStickUp(void) {
    return hostSdlControllerAxisInt(SDL_CONTROLLER_AXIS_LEFTY) < -hostSdlGameControllerStickDeadzone;
}

static int hostSdlControllerLeftStickDown(void) {
    return hostSdlControllerAxisInt(SDL_CONTROLLER_AXIS_LEFTY) > hostSdlGameControllerStickDeadzone;
}

static int hostSdlControllerLeftStickLeft(void) {
    return hostSdlControllerAxisInt(SDL_CONTROLLER_AXIS_LEFTX) < -hostSdlGameControllerStickDeadzone;
}

static int hostSdlControllerLeftStickRight(void) {
    return hostSdlControllerAxisInt(SDL_CONTROLLER_AXIS_LEFTX) > hostSdlGameControllerStickDeadzone;
}


static uint32_t copperListStartPc(M68k* cpu);
static int copperListLooksValid(M68k* cpu, uint32_t pc);

static uint16_t readCustomWord(M68k* cpu, uint32_t offset) {
    uint16_t hi = cpu->customRegs[offset];
    uint16_t lo = cpu->customRegs[offset + 1u];
    return (uint16_t)((hi << 8) | lo);
}

static uint32_t readCustomLong(M68k* cpu, uint32_t offset) {
    uint32_t hi = readCustomWord(cpu, offset);
    uint32_t lo = readCustomWord(cpu, offset + 2u);
    return (hi << 16) | lo;
}

static uint32_t color12ToArgb(uint16_t rgb12) {
    uint32_t r = (uint32_t)((rgb12 >> 8) & 0x0Fu);
    uint32_t g = (uint32_t)((rgb12 >> 4) & 0x0Fu);
    uint32_t b = (uint32_t)(rgb12 & 0x0Fu);
    r = (r << 4) | r;
    g = (g << 4) | g;
    b = (b << 4) | b;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

#define AMIGA_HOST_MAX_PLANES 8
#define AMIGA_HOST_MAX_PALETTE 256

static void buildPalette(M68k* cpu, uint32_t* paletteOut, int count) {
    int i;
    int allZero;
    static int haveRandomFallback;
    static uint32_t randomFallbackPalette[32];

    if (count > AMIGA_HOST_MAX_PALETTE) {
        count = AMIGA_HOST_MAX_PALETTE;
    }
    allZero = 1;
    for (i = 0; i < count; i++) {
        paletteOut[i] = m68kAgaColorArgb(cpu, i);
        if ((paletteOut[i] & 0x00FFFFFFu) != 0u) {
            allZero = 0;
        }
    }

    /*
     * When the game never wrote COLORxx into our custom stub (common until
     * copper lists are mirrored), every entry is zero and the whole screen
     * maps to black. A one-time random 12-bit table makes bitplane data
     * visible for bring-up debugging (OCS-sized screens only).
     */
    if (allZero && count <= 32) {
        if (!haveRandomFallback) {
            unsigned seed;
            seed = (unsigned)time(NULL);
            if (seed == 0u) {
                seed = 1u;
            }
            srand(seed);
            for (i = 0; i < 32; i++) {
                uint16_t junk;
                junk = (uint16_t)((unsigned)rand() & 0xFFFu);
                randomFallbackPalette[i] = color12ToArgb(junk);
            }
            haveRandomFallback = 1;
        }
        for (i = 0; i < count; i++) {
            paletteOut[i] = randomFallbackPalette[i];
        }
    }
}

static uint32_t planeBasePointer(M68k* cpu, int planeIdx) {
    uint32_t pthOffset = 0x000000E0u + (uint32_t)(planeIdx * 4);
    return readCustomLong(cpu, pthOffset);
}

static int readPlaneBitSigned(M68k* cpu, int64_t address, int bitShift) {
    if (address < 0 || address >= (int64_t)cpu->memSize) {
        return 0;
    }
    return (cpu->mem[(uint32_t)address] >> bitShift) & 1;
}

/*
 * OCS sprites after playfield: custom PTH-PTL at 0x120+4*spr, POS+CTL at
 * 0x140+8*spr. Vertical decode follows OCS: VSTART = SPOS[15:8] + CTL bit2;
 * VSTOP = SCTL[15:8] + CTL bit1. Horizontal decode: HSTART is 9 bits from
 * SPOS low byte plus CTL bit0, then adjusted by the 0x7f hardware bias.
 * hoilevel sprites S0+S1 and S2+S3 are attached: 2 words/chan/line, 4 bits/px,
 * COLOR17–31 (0x1a2+2*(c-1)).
 * Draw pair 2+3 first, then 0+1 (S0 on top in hardware).
 */
static void decodeOcsSpriteVRange(uint32_t sposW, uint32_t sctlW, int* vStartOut, int* vStopOut) {
    int vStart;
    int vStop;
    vStart = (int)((sposW >> 8) & 0xFFu);
    if ((sctlW & 0x0004u) != 0u) {
        vStart |= 0x100;
    }
    vStop = (int)((sctlW >> 8) & 0xFFu);
    if ((sctlW & 0x0002u) != 0u) {
        vStop |= 0x100;
    }
    *vStartOut = vStart;
    *vStopOut = vStop;
}

static int decodeOcsSpriteHStart(uint32_t sposW, uint32_t sctlW) {
    int hRaw;
    hRaw = (int)((sposW & 0x00FFu) << 1);
    hRaw |= (int)(sctlW & 0x0001u);
    return hRaw - 0x7F;
}

static int decodeDisplayWindowStartY(M68k* cpu) {
    uint16_t diwstrt;
    diwstrt = readCustomWord(cpu, 0x008Eu);
    return (int)((diwstrt >> 8) & 0x00FFu);
}

/*
 * Decode the horizontal display window from DIWSTRT/DIWSTOP into framebuffer
 * pixel coordinates.
 *
 * 1 DIW H unit = 1 lo-res pixel = 1 framebuffer pixel (host->width = 320).
 * Framebuffer x = 0 corresponds to DIW H = $81, which is the standard left
 * edge of a low-res screen.
 *
 * For DIWSTOP, this matches what WinUAE/FS-UAE actually do: bit 8 of the H
 * byte is set whenever bit 7 is set, i.e. eff = raw | 0x100 if H[7] = 1, else
 * eff = raw. This makes the standard $81/$c1 give a full 320-px window, and
 * the game's "klap" anchors line up correctly:
 *   $84/$bc  -> visible 312 px (doors fully open, ~minimal trim)
 *   $8c/$b4  -> visible 296 px (doors closed, ~24 px total trim)
 *
 * Note the OCS Hardware Reference Manual states bit 8 = NOT bit 7, but real
 * games use the bit 8 = bit 7 behaviour, which is why both major emulators
 * implement it that way.
 */
static void computeDiwHorizontalRange(M68k* cpu, int hostWidth,
                                      int* xLeftOut, int* xRightOut) {
    uint16_t diwstrt;
    uint16_t diwstop;
    int hStart;
    int hStopRaw;
    int hStopEff;
    int xLeft;
    int xRight;

    diwstrt = readCustomWord(cpu, 0x008Eu);
    diwstop = readCustomWord(cpu, 0x0090u);

    hStart = (int)(diwstrt & 0x00FFu);
    hStopRaw = (int)(diwstop & 0x00FFu);
    if ((hStopRaw & 0x80) != 0) {
        hStopEff = hStopRaw | 0x100;
    } else {
        hStopEff = hStopRaw;
    }

    xLeft = hStart - 0x81;
    xRight = hStopEff - 0x81;
    if (xLeft < 0) {
        xLeft = 0;
    }
    if (xLeft > hostWidth) {
        xLeft = hostWidth;
    }
    if (xRight < xLeft) {
        xRight = xLeft;
    }
    if (xRight > hostWidth) {
        xRight = hostWidth;
    }
    *xLeftOut = xLeft;
    *xRightOut = xRight;
}

/*
 * Decode the vertical display window from DIWSTRT/DIWSTOP. DIWSTOP.V uses
 * the same H[7] inversion trick as DIWSTOP.H to extend the stop value past
 * a single byte: V[7] = 1 -> bit 8 = 0; V[7] = 0 -> bit 8 = 1.
 */
static void computeDiwVerticalRange(M68k* cpu, int* vStartOut, int* vStopOut) {
    uint16_t diwstrt;
    uint16_t diwstop;
    int vStart;
    int vStopRaw;
    int vStopEff;

    diwstrt = readCustomWord(cpu, 0x008Eu);
    diwstop = readCustomWord(cpu, 0x0090u);

    vStart = (int)((diwstrt >> 8) & 0x00FFu);
    vStopRaw = (int)((diwstop >> 8) & 0x00FFu);
    if ((vStopRaw & 0x80) != 0) {
        vStopEff = vStopRaw;
    } else {
        vStopEff = vStopRaw + 0x100;
    }
    if (vStopEff < vStart) {
        vStopEff = vStart;
    }
    *vStartOut = vStart;
    *vStopOut = vStopEff;
}

static int clampOcsVStop(int vStart, int vStop) {
    if (vStop <= vStart) {
        return vStart;
    }
    if (vStop - vStart > 200) {
        return vStart + 32;
    }
    return vStop;
}

static uint32_t spriteColorThreePlaneArgb(M68k* cpu, int sprIndex, int twoBit) {
    int pairIndex;
    uint32_t off;
    if (twoBit < 1 || twoBit > 3) {
        return 0u;
    }
    pairIndex = sprIndex >> 1;
    off = 0x1A2u + (uint32_t)(pairIndex * 8) + (uint32_t)(2 * (twoBit - 1));
    return color12ToArgb(readCustomWord(cpu, off));
}

static uint32_t attachedNibbleToArgb(M68k* cpu, int nibble) {
    if (nibble < 1 || nibble > 15) {
        return 0u;
    }
    return color12ToArgb(readCustomWord(cpu, 0x1A2u + 2u * (uint32_t)(nibble - 1)));
}

static int spriteCanOverwritePlayfield(AmigaHostSdl* host, M68k* cpu, int sprIndex, int pixelIndex) {
    uint16_t bplcon0;
    uint16_t bplcon2;
    int playfieldIndex;
    int pairIndex;
    int priorityBits;

    if (host == NULL || host->playfieldIndexBuffer == NULL || cpu == NULL) {
        return 1;
    }
    if (pixelIndex < 0 || pixelIndex >= host->width * host->height) {
        return 0;
    }

    playfieldIndex = (int)host->playfieldIndexBuffer[pixelIndex];
    if (playfieldIndex == 0) {
        return 1;
    }

    bplcon0 = readCustomWord(cpu, 0x00000100u);
    bplcon2 = readCustomWord(cpu, 0x00000104u);
    pairIndex = sprIndex >> 1;
    if (pairIndex < 2) {
        priorityBits = (int)(bplcon2 & 0x0007u);
    } else {
        priorityBits = (int)((bplcon2 >> 3) & 0x0007u);
    }

    /*
     * Single-playfield mode: do not derive priority from color index.
     * Any nonzero priority field means sprites in front of nonzero PF pixels;
     * zero means sprites behind nonzero PF pixels.
     */
    if ((bplcon0 & 0x0400u) == 0u) {
        return priorityBits != 0;
    }

    if (priorityBits == 0) {
        return 0;
    }
    return 1;
}

/*
 * Attached 16-color line: even/odd channels can have different VSTART (chained
 * +8) while sharing one VSTOP. Each list uses its own line index; when the odd
 * channel is not active yet, its bits are zero.
 */
static void drawAttachedOcs16OneLine(
    AmigaHostSdl* host,
    M68k* cpu,
    int pairBaseSpriteIndex,
    uint32_t listA,
    uint32_t listB,
    int yScreen,
    int evenLineIdx,
    int oddLineIdx,
    int hStart) {
    uint32_t evenRowOff;
    uint32_t oddRowOff;
    uint16_t s0A;
    uint16_t s0B;
    uint16_t s1A;
    uint16_t s1B;
    int px;
    if (yScreen < 0 || yScreen >= host->height) {
        return;
    }
    if (evenLineIdx < 0 && oddLineIdx < 0) {
        return;
    }
    s0A = 0u;
    s0B = 0u;
    s1A = 0u;
    s1B = 0u;
    if (evenLineIdx >= 0) {
        evenRowOff = (uint32_t)evenLineIdx * 4u;
        if (listA + evenRowOff + 3u < cpu->memSize) {
            s0A = read16(cpu, listA + evenRowOff);
            s0B = read16(cpu, listA + evenRowOff + 2u);
        }
    }
    if (oddLineIdx >= 0) {
        oddRowOff = (uint32_t)oddLineIdx * 4u;
        if (listB + oddRowOff + 3u < cpu->memSize) {
            s1A = read16(cpu, listB + oddRowOff);
            s1B = read16(cpu, listB + oddRowOff + 2u);
        }
    }
    for (px = 0; px < 16; px++) {
        int sh;
        int t0;
        int t1;
        int t2;
        int t3;
        int nibble;
        int xScr;
        int idx;
        sh = 15 - px;
        t0 = (int)((s0A >> sh) & 1u);
        t1 = (int)((s0B >> sh) & 1u);
        t2 = (int)((s1A >> sh) & 1u);
        t3 = (int)((s1B >> sh) & 1u);
        nibble = t0 | (t1 << 1) | (t2 << 2) | (t3 << 3);
        if (nibble == 0) {
            continue;
        }
        xScr = hStart + px;
        if (xScr < 0 || xScr >= host->width) {
            continue;
        }
        idx = yScreen * host->width + xScr;
        if (!spriteCanOverwritePlayfield(host, cpu, pairBaseSpriteIndex, idx)) {
            continue;
        }
        host->frameBuffer[idx] = attachedNibbleToArgb(cpu, nibble);
    }
}

static void drawSingleOcsSpriteOneLine(
    AmigaHostSdl* host, M68k* cpu, int spr, uint32_t listPtr, int yScreen, int lineIdx, int hStart) {
    uint32_t rowOff;
    uint16_t dataW;
    uint16_t datbW;
    int px;
    if (yScreen < 0 || yScreen >= host->height) {
        return;
    }
    if (lineIdx < 0) {
        return;
    }
    rowOff = (uint32_t)lineIdx * 4u;
    if (listPtr + rowOff + 3u >= cpu->memSize) {
        return;
    }
    dataW = read16(cpu, listPtr + rowOff);
    datbW = read16(cpu, listPtr + rowOff + 2u);
    for (px = 0; px < 16; px++) {
        int b;
        int a;
        int twoBit;
        int sh;
        int xScr;
        int idx;
        sh = 15 - px;
        b = (int)((dataW >> sh) & 1u);
        a = (int)((datbW >> sh) & 1u);
        twoBit = (a << 1) | b;
        if (twoBit == 0) {
            continue;
        }
        xScr = hStart + px;
        if (xScr < 0 || xScr >= host->width) {
            continue;
        }
        idx = yScreen * host->width + xScr;
        if (!spriteCanOverwritePlayfield(host, cpu, spr, idx)) {
            continue;
        }
        host->frameBuffer[idx] = spriteColorThreePlaneArgb(cpu, spr, twoBit);
    }
}

static int spriteChannelDisabled(uint32_t sposW, uint32_t sctlW) {
    return (sposW == 0u && sctlW == 0u) ? 1 : 0;
}

static int spritePairAttached(uint32_t evenCtlW, uint32_t oddCtlW) {
    (void)evenCtlW;
    /* OCS attach bit lives on the odd sprite of each pair. */
    return (oddCtlW & 0x0080u) != 0u;
}

/*
 * Sprite words/pointers come from customRegs. The copper runner updates these
 * regs as it advances toward each scanline, so sprite state stays line-accurate
 * without needing a second full-list sprite pass.
 */
static uint16_t readSpriteWord(M68k* cpu, uint32_t customOff) {
    return readCustomWord(cpu, customOff);
}

static uint32_t readSpritePointer(M68k* cpu, int sprIndex) {
    return readCustomLong(cpu, 0x120u + 4u * (uint32_t)sprIndex);
}

/*
 * Denise keeps fetching sprite data on every scanline inside the vertical
 * window even when copper rewrites VSTART mid-frame (pole + areyoucop). Count
 * displayed lines per pair per frame instead of subtracting the live VSTART.
 */
static void drawSpritePairForScanline(
    AmigaHostSdl* host,
    M68k* cpu,
    int y,
    int amigaScanline,
    int displayStartY,
    int pairBase,
    int spritePairBeamLineCount[4],
    uint32_t spritePairLastEvenPtr[4]) {
    int evenSpr;
    int oddSpr;
    uint32_t evenPos;
    uint32_t oddPos;
    uint32_t evenCtl;
    uint32_t oddCtl;
    int amigaVs;
    int amigaVe;
    int evenVs;
    int evenVe;
    int pairIdx;

    evenSpr = pairBase;
    oddSpr = pairBase + 1;
    pairIdx = pairBase / 2;
    evenPos = (uint32_t)readSpriteWord(cpu, 0x140u + 8u * (uint32_t)evenSpr);
    oddPos = (uint32_t)readSpriteWord(cpu, 0x140u + 8u * (uint32_t)oddSpr);
    evenCtl = (uint32_t)readSpriteWord(cpu, 0x142u + 8u * (uint32_t)evenSpr);
    oddCtl = (uint32_t)readSpriteWord(cpu, 0x142u + 8u * (uint32_t)oddSpr);

    decodeOcsSpriteVRange(evenPos, evenCtl, &amigaVs, &amigaVe);
    amigaVe = clampOcsVStop(amigaVs, amigaVe);
    evenVs = amigaVs - displayStartY;
    evenVe = amigaVe - displayStartY;
    if (y < evenVs || y >= evenVe) {
        return;
    }
    if (spriteChannelDisabled(evenPos, evenCtl) && spriteChannelDisabled(oddPos, oddCtl)) {
        return;
    }

    if (pairIdx == 1) {
        uint32_t evenPtr;
        evenPtr = readSpritePointer(cpu, evenSpr);
        if (spritePairLastEvenPtr[pairIdx] != 0u && evenPtr != spritePairLastEvenPtr[pairIdx]) {
            spritePairBeamLineCount[pairIdx] = 0;
        }
        spritePairLastEvenPtr[pairIdx] = evenPtr;
        spritePairBeamLineCount[pairIdx]++;
    }

    if (!spriteChannelDisabled(evenPos, evenCtl) && !spriteChannelDisabled(oddPos, oddCtl) &&
        spritePairAttached(evenCtl, oddCtl)) {
        int hStart;
        int evenLineIdx;
        int oddLineIdx;
        int oddAmigaVs;
        int oddAmigaVe;
        hStart = decodeOcsSpriteHStart(evenPos, evenCtl);
        /*
         * Pole (SPR2+3): beam line count survives mid-frame VSTART bumps.
         * Cursor/button (SPR4+7): same-line attached data; use live VSTART.
         */
        if (pairIdx == 1) {
            evenLineIdx = spritePairBeamLineCount[pairIdx] - 1;
        } else {
            evenLineIdx = amigaScanline - amigaVs;
        }
        decodeOcsSpriteVRange(oddPos, oddCtl, &oddAmigaVs, &oddAmigaVe);
        oddLineIdx = evenLineIdx - (oddAmigaVs - amigaVs);
        drawAttachedOcs16OneLine(
            host,
            cpu,
            evenSpr,
            readSpritePointer(cpu, evenSpr),
            readSpritePointer(cpu, oddSpr),
            y,
            evenLineIdx,
            oddLineIdx,
            hStart);
    } else {
        int spr;
        for (spr = oddSpr; spr >= evenSpr; spr--) {
            uint32_t sposW;
            uint32_t sctlW;
            int sprAmigaVs;
            int sprAmigaVe;
            int vs;
            int ve;
            int lineIdx;
            sposW = (uint32_t)readSpriteWord(cpu, 0x140u + 8u * (uint32_t)spr);
            sctlW = (uint32_t)readSpriteWord(cpu, 0x142u + 8u * (uint32_t)spr);
            if (spriteChannelDisabled(sposW, sctlW) != 0) {
                continue;
            }
            decodeOcsSpriteVRange(sposW, sctlW, &sprAmigaVs, &sprAmigaVe);
            sprAmigaVe = clampOcsVStop(sprAmigaVs, sprAmigaVe);
            vs = sprAmigaVs - displayStartY;
            ve = sprAmigaVe - displayStartY;
            if (y < vs || y >= ve) {
                continue;
            }
            lineIdx = amigaScanline - sprAmigaVs;
            drawSingleOcsSpriteOneLine(
                host,
                cpu,
                spr,
                readSpritePointer(cpu, spr),
                y,
                lineIdx,
                decodeOcsSpriteHStart(sposW, sctlW));
        }
    }
}

/*
 * Sprite regs are sampled from customRegs after runCopperUntilLine(), so their
 * state matches the current scanline instead of "last write in whole list".
 */
static void drawSpritesOcsForScanline(
    AmigaHostSdl* host,
    M68k* cpu,
    int y,
    int amigaScanline,
    int spritePairBeamLineCount[4],
    uint32_t spritePairLastEvenPtr[4]) {
    int pairBase;
    int displayStartY;
    if (host == NULL || cpu == NULL) {
        return;
    }
    displayStartY = decodeDisplayWindowStartY(cpu);
    for (pairBase = 6; pairBase >= 0; pairBase -= 2) {
        drawSpritePairForScanline(
            host,
            cpu,
            y,
            amigaScanline,
            displayStartY,
            pairBase,
            spritePairBeamLineCount,
            spritePairLastEvenPtr);
    }
}

static int computeBitplaneBytesPerRow(M68k* cpu, int width) {
    uint16_t ddfstrt;
    uint16_t ddfstop;
    uint16_t bplcon0;
    int start;
    int stop;
    int fetchWords;
    int bytesPerRow;

    ddfstrt = readCustomWord(cpu, 0x00000092u);
    ddfstop = readCustomWord(cpu, 0x00000094u);
    bplcon0 = readCustomWord(cpu, 0x00000100u);

    start = (int)(ddfstrt & 0x00FEu);
    stop = (int)(ddfstop & 0x00FEu);
    if (stop < start) {
        return width / 8;
    }

    /*
     * HRM: lores: DDFSTRT = DDFSTOP - (8 * (word count - 1))
     *      hires: DDFSTRT = DDFSTOP - (4 * (word count - 2))
     * so word count is (span/8)+1 or (span/4)+2 respectively.
     */
    if ((bplcon0 & 0x8000u) != 0u) {
        fetchWords = (stop - start) / 4;
        fetchWords = fetchWords + 2;
    } else {
        fetchWords = (stop - start) / 8;
        fetchWords = fetchWords + 1;
    }
    if (fetchWords < 1) {
        fetchWords = 1;
    }
    if (fetchWords > 512) {
        fetchWords = 512;
    }
    bytesPerRow = fetchWords * 2;
    return bytesPerRow;
}

static int chooseBitplaneBytesPerRow(M68k* cpu, int width) {
    return computeBitplaneBytesPerRow(cpu, width);
}

static int computeDdfHorizontalOffsetPixels(M68k* cpu) {
    uint16_t ddfstrt;
    uint16_t bplcon0;
    int start;
    int delta;
    ddfstrt = readCustomWord(cpu, 0x00000092u);
    bplcon0 = readCustomWord(cpu, 0x00000100u);
    start = (int)(ddfstrt & 0x00FEu);
    delta = start - 0x30;
    if ((bplcon0 & 0x8000u) != 0u) {
        return delta * 4 - 1;
    }
    /* -1 keeps PF1H (15-nibble) scroll and standard DDFSTRT=$38 aligned at x=0. */
    return delta * 2 - 1;
}

static int copperListLooksValid(M68k* cpu, uint32_t pc) {
    if (cpu == NULL || pc == 0u || pc + 4u > cpu->memSize) {
        return 0;
    }
    if (read16(cpu, pc) == 0u && read16(cpu, pc + 2u) == 0u) {
        return 0;
    }
    return 1;
}

/*
 * HRM: vblank restarts the single Copper PC from COP1LC. Fall back to COP2LC
 * only when COP1LC is unset/empty (some hoilevel* builds park a scratch list
 * in COP1 and keep the real list in COP2).
 */
static uint32_t copperListStartPc(M68k* cpu) {
    uint32_t pc1;
    uint32_t pc2;

    if (cpu == NULL) {
        return 0u;
    }
    pc1 = readCustomLong(cpu, 0x00000080u);
    if (copperListLooksValid(cpu, pc1)) {
        return pc1;
    }
    pc2 = readCustomLong(cpu, 0x00000084u);
    if (copperListLooksValid(cpu, pc2)) {
        return pc2;
    }
    return 0u;
}

/*
 * COPJMP1/COPJMP2 strobe writes reload the Copper PC from COP1LC/COP2LC.
 * Returns 1 on jump, 0 if regOff is not a strobe, -1 if destination invalid.
 */
static int copperStrobeJump(M68k* cpu, uint32_t regOff, uint32_t* pcOut) {
    uint32_t dest;

    if (cpu == NULL || pcOut == NULL) {
        return -1;
    }
    if (regOff == 0x088u) {
        dest = readCustomLong(cpu, 0x00000080u);
    } else if (regOff == 0x08Au) {
        dest = readCustomLong(cpu, 0x00000084u);
    } else {
        return 0;
    }
    if (!copperListLooksValid(cpu, dest)) {
        return -1;
    }
    *pcOut = dest;
    return 1;
}

static int copperWaitBeamReached(uint16_t waitIr,
                                 int beamYVirtual,
                                 int displayStartY,
                                 int lowWaitMeansPostWrap) {
    int waitBeamY;

    waitBeamY = (int)((waitIr >> 8) & 0xFFu);
    if (!lowWaitMeansPostWrap) {
        int currentBeamY;
        currentBeamY = beamYVirtual & 0xFF;
        return (waitBeamY <= currentBeamY) ? 1 : 0;
    }
    {
        int waitBeamVirtual;
        waitBeamVirtual = waitBeamY;
        if (waitBeamVirtual < displayStartY) {
            waitBeamVirtual += 256;
        }
        return (waitBeamVirtual <= beamYVirtual) ? 1 : 0;
    }
}

static void runCopperUntilLine(M68k* cpu,
                               uint32_t* pcInOut,
                               int beamYVirtual,
                               int displayStartY,
                               int lowWaitMeansPostWrap,
                               int* doneInOut,
                               int* frameHighWaitSeenInOut);

/*
 * Static menu copper ends with WAIT v=$ff then WAIT v=$2c (DIWSTRT.V) and
 * BPLCON0 off; that trailing $2c is for the next vertical pass. In-game
 * copbuf also has per-line waits at v=$f0..$fe and a $ff->$00 wrap into
 * v=$01..$07 scoreboard tail — only the bare $ff->$2c shutdown must be
 * held (exact v match on both sides).
 */
static int copperWaitHeldForNextFrame(uint16_t waitIr,
                                      int displayStartY,
                                      int frameHighWaitSeen) {
    int waitRaw;

    if (!frameHighWaitSeen) {
        return 0;
    }
    waitRaw = (int)((waitIr >> 8) & 0xFFu);
    if (waitRaw == displayStartY) {
        return 1;
    }
    return 0;
}

static void copperNoteHighFrameWait(int waitRaw, int* frameHighWaitSeenInOut) {
    if (frameHighWaitSeenInOut != NULL && waitRaw == 0xff) {
        *frameHighWaitSeenInOut = 1;
    }
}

/*
 * True when this WAIT sits earlier in the list than the previous WAIT we
 * already honoured this frame, but is not the copbuf $ff->$00 byte wrap
 * Hoi uses. ClockAGA parks its scoreboard tail (v=$06,$07,...) after the
 * high-v copbuf; without this guard a catch-up burst applies the whole tail
 * on one host scanline.
 */
static int copperWaitRetrograde(uint16_t waitIr, int lastWaitRaw) {
    int waitRaw;

    waitRaw = (int)((waitIr >> 8) & 0xFFu);
    if (lastWaitRaw < 0) {
        return 0;
    }
    if (waitRaw >= lastWaitRaw) {
        return 0;
    }
    if (waitRaw == 0 && lastWaitRaw >= 0xf0) {
        return 0;
    }
    if ((lastWaitRaw - waitRaw) > 80 && waitRaw < 0x60) {
        return 1;
    }
    return 0;
}

/*
 * Advance the copper for the current host scanline until the next WAIT is
 * in the future. Hoi builds ~230 per-line waits in copbuf before the
 * scoreboard tail; one WAIT per display line never reaches that tail.
 */
static void runCopperCatchUpToBeam(M68k* cpu,
                                   uint32_t* pcInOut,
                                   int beamYVirtual,
                                   int displayStartY,
                                   int lowWaitMeansPostWrap,
                                   int* doneInOut,
                                   int* frameHighWaitSeenInOut) {
    int guard;
    int lastWaitRaw;
    int maxSections;

    if (cpu == NULL || pcInOut == NULL || doneInOut == NULL) {
        return;
    }
    if (*doneInOut) {
        return;
    }

    maxSections = lowWaitMeansPostWrap ? 96 : 512;
    lastWaitRaw = -1;
    for (guard = 0; guard < maxSections; guard++) {
        uint32_t pcBefore;
        uint16_t peekIr1;

        pcBefore = *pcInOut;
        if (pcBefore + 4u > cpu->memSize) {
            break;
        }
        peekIr1 = read16(cpu, pcBefore);
        if ((peekIr1 & 1u) != 0u) {
            if (copperWaitHeldForNextFrame(
                    peekIr1, displayStartY,
                    frameHighWaitSeenInOut != NULL ? *frameHighWaitSeenInOut : 0)) {
                break;
            }
            if (!copperWaitBeamReached(peekIr1, beamYVirtual, displayStartY, lowWaitMeansPostWrap)) {
                break;
            }
            if (copperWaitRetrograde(peekIr1, lastWaitRaw)) {
                break;
            }
            lastWaitRaw = (int)((peekIr1 >> 8) & 0xFFu);
            copperNoteHighFrameWait(lastWaitRaw, frameHighWaitSeenInOut);
        }
        runCopperUntilLine(
            cpu, pcInOut, beamYVirtual, displayStartY, lowWaitMeansPostWrap, doneInOut,
            frameHighWaitSeenInOut);
        if (*doneInOut || *pcInOut == pcBefore) {
            break;
        }
    }
}

/*
 * Advance the copper through at most one satisfied WAIT and the MOVEs that
 * follow it until the next WAIT/SKIP. Real hardware does not satisfy every
 * WAIT with v <= beam in a single vertical instant; chewing through the
 * whole list in one host step made ClockAGA's scoreboard tail fire
 * BPLCON0=$7200 and the following disable on the same synthetic line.
 */
static void runCopperUntilLine(M68k* cpu,
                               uint32_t* pcInOut,
                               int beamYVirtual,
                               int displayStartY,
                               int lowWaitMeansPostWrap,
                               int* doneInOut,
                               int* frameHighWaitSeenInOut) {
    int moveSteps;

    if (cpu == NULL || pcInOut == NULL || doneInOut == NULL) {
        return;
    }
    if (*doneInOut) {
        return;
    }

    for (moveSteps = 0; moveSteps < 4096; moveSteps++) {
        uint16_t ir1;
        uint16_t ir2;
        uint32_t pc;

        pc = *pcInOut;
        if (pc + 4u > cpu->memSize) {
            *doneInOut = 1;
            break;
        }
        ir1 = read16(cpu, pc);
        ir2 = read16(cpu, pc + 2u);
        pc += 4u;

        if (ir1 == 0xFFFFu && ir2 == 0xFFFEu) {
            *doneInOut = 1;
            break;
        }

        if ((ir1 & 1u) == 0u) {
            uint32_t regOff;

            regOff = (uint32_t)(ir1 & 0x01FEu);
            if (regOff + 1u >= sizeof(cpu->customRegs)) {
                *pcInOut = pc;
                continue;
            }
            write16(cpu, 0x00DFF000u + regOff, ir2);
            {
                int jmp;
                jmp = copperStrobeJump(cpu, regOff, pcInOut);
                if (jmp < 0) {
                    *doneInOut = 1;
                    break;
                }
                if (jmp > 0) {
                    continue;
                }
            }
            *pcInOut = pc;
            continue;
        }

        if (copperWaitHeldForNextFrame(
                ir1, displayStartY,
                frameHighWaitSeenInOut != NULL ? *frameHighWaitSeenInOut : 0)) {
            *pcInOut = pc - 4u;
            break;
        }
        if (!copperWaitBeamReached(ir1, beamYVirtual, displayStartY, lowWaitMeansPostWrap)) {
            *pcInOut = pc - 4u;
            break;
        }
        copperNoteHighFrameWait((int)((ir1 >> 8) & 0xFFu), frameHighWaitSeenInOut);
        *pcInOut = pc;

        for (moveSteps = 0; moveSteps < 4096; moveSteps++) {
            pc = *pcInOut;
            if (pc + 4u > cpu->memSize) {
                *doneInOut = 1;
                return;
            }
            ir1 = read16(cpu, pc);
            ir2 = read16(cpu, pc + 2u);
            pc += 4u;

            if (ir1 == 0xFFFFu && ir2 == 0xFFFEu) {
                *doneInOut = 1;
                *pcInOut = pc;
                return;
            }

            if ((ir1 & 1u) != 0u) {
                *pcInOut = pc - 4u;
                return;
            }

            {
                uint32_t regOff;
                regOff = (uint32_t)(ir1 & 0x01FEu);
                if (regOff + 1u >= sizeof(cpu->customRegs)) {
                    *pcInOut = pc;
                    continue;
                }
                write16(cpu, 0x00DFF000u + regOff, ir2);
                {
                    int jmp;
                    jmp = copperStrobeJump(cpu, regOff, pcInOut);
                    if (jmp < 0) {
                        *doneInOut = 1;
                        return;
                    }
                    if (jmp > 0) {
                        continue;
                    }
                }
            }
            *pcInOut = pc;
        }
        return;
    }
}

static uint32_t srcPixelClamp(uint32_t* src, int srcW, int srcH, int x, int y) {
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    if (x >= srcW) {
        x = srcW - 1;
    }
    if (y >= srcH) {
        y = srcH - 1;
    }
    return src[(size_t)y * (size_t)srcW + (size_t)x];
}

/*
 * rgbToYuvCached converts an ARGB pixel to packed YUV (Y<<16)|(U<<8)|V using the
 * exact same integer formula as before, but memoises the result per distinct RGB
 * color in a direct-mapped cache. An Amiga framebuffer only holds a handful of
 * distinct colors, so after the first few lookups every conversion is a table
 * read instead of nine multiplies plus three integer divides. The cached values
 * are bit-identical to the original computation, so scaler output is unchanged.
 *
 * Note: the cache is process-wide and single-threaded. If the scalers are ever
 * run across worker threads, give each thread its own cache (or precompute a
 * read-only table) to avoid data races on these arrays.
 */
#define yuvCacheBits 13
#define yuvCacheSize (1u << yuvCacheBits)
static uint32_t rgbToYuvCached(uint32_t color) {
    static uint32_t yuvCacheKey[yuvCacheSize];
    static uint32_t yuvCacheVal[yuvCacheSize];
    static int yuvCacheReady = 0;
    uint32_t rgb;
    uint32_t idx;
    int r;
    int g;
    int b;
    int yv;
    int uv;
    int vv;
    uint32_t packed;

    if (yuvCacheReady == 0) {
        unsigned k;
        for (k = 0; k < yuvCacheSize; k++) {
            yuvCacheKey[k] = 0xFFFFFFFFu;
        }
        yuvCacheReady = 1;
    }

    rgb = color & 0x00FFFFFFu;
    idx = (rgb * 2654435761u) >> (32 - yuvCacheBits);
    if (yuvCacheKey[idx] == rgb) {
        return yuvCacheVal[idx];
    }

    r = (int)((rgb >> 16) & 0xFFu);
    g = (int)((rgb >> 8) & 0xFFu);
    b = (int)(rgb & 0xFFu);
    yv = (299 * r + 587 * g + 114 * b) / 1000;
    uv = (-169 * r - 331 * g + 500 * b) / 1000 + 128;
    vv = (500 * r - 419 * g - 81 * b) / 1000 + 128;
    packed = ((uint32_t)yv << 16) | ((uint32_t)uv << 8) | (uint32_t)vv;

    yuvCacheKey[idx] = rgb;
    yuvCacheVal[idx] = packed;
    return packed;
}
#undef yuvCacheBits
#undef yuvCacheSize

static unsigned xbrPixelDiffInline(uint32_t x1, uint32_t y1) {
    uint32_t yuv1;
    uint32_t yuv2;
    int dy;
    int du;
    int dv;

    yuv1 = rgbToYuvCached(x1);
    yuv2 = rgbToYuvCached(y1);
    dy = (int)((yuv1 >> 16) & 0xFFu) - (int)((yuv2 >> 16) & 0xFFu);
    du = (int)((yuv1 >> 8) & 0xFFu) - (int)((yuv2 >> 8) & 0xFFu);
    dv = (int)(yuv1 & 0xFFu) - (int)(yuv2 & 0xFFu);
    if (dy < 0) {
        dy = -dy;
    }
    if (du < 0) {
        du = -du;
    }
    if (dv < 0) {
        dv = -dv;
    }
    return (unsigned)(dy + du + dv);
}

static void scaleXbr2x(uint32_t* src, int srcW, int srcH, uint32_t* dst) {
    int y;
    int x;
    int dstW;

#define xbrLbMask 0x00FEFEFEu
#define xbrRedBlueMask 0x00FF00FFu
#define xbrGreenMask 0x0000FF00u
#define xbrAlphaBlend128(a, b) ((((a) & xbrLbMask) >> 1) + (((b) & xbrLbMask) >> 1))
#define xbrAlphaBlendBase(a, b, m, s) \
    ((xbrRedBlueMask & (((a) & xbrRedBlueMask) + (((((b) & xbrRedBlueMask) - ((a) & xbrRedBlueMask)) * (m)) >> (s)))) \
     | (xbrGreenMask & (((a) & xbrGreenMask) + (((((b) & xbrGreenMask) - ((a) & xbrGreenMask)) * (m)) >> (s)))))
#define xbrAlphaBlend32(a, b) xbrAlphaBlendBase(a, b, 1, 3)
#define xbrAlphaBlend64(a, b) xbrAlphaBlendBase(a, b, 1, 2)
#define xbrAlphaBlend192(a, b) xbrAlphaBlendBase(a, b, 3, 2)
#define xbrAlphaBlend224(a, b) xbrAlphaBlendBase(a, b, 7, 3)
#define xbrPixelDiff(a, b) xbrPixelDiffInline((a), (b))
#define xbrEq(a, b) (xbrPixelDiff((a), (b)) < 155u)
#define xbrFILT2(PE, PI, PH, PF, PG, PC, PD, PB, PA, G5, C4, G0, D0, C1, B1, F4, I4, H5, I5, A0, A1, N0, N1, N2, N3) \
    do { \
        if ((PE) != (PH) && (PE) != (PF)) { \
            unsigned e; \
            unsigned i; \
            if (xbrPixelDiff((PE), (PC)) + xbrPixelDiff((PE), (PG)) + xbrPixelDiff((PI), (H5)) + xbrPixelDiff((PI), (F4)) + (xbrPixelDiff((PH), (PF)) << 2) \
                <= xbrPixelDiff((PH), (PD)) + xbrPixelDiff((PH), (I5)) + xbrPixelDiff((PF), (I4)) + xbrPixelDiff((PF), (PB)) + (xbrPixelDiff((PE), (PI)) << 2)) { \
                uint32_t px; \
                px = xbrPixelDiff((PE), (PF)) <= xbrPixelDiff((PE), (PH)) ? (PF) : (PH); \
                e = xbrPixelDiff((PE), (PC)) + xbrPixelDiff((PE), (PG)) + xbrPixelDiff((PI), (H5)) + xbrPixelDiff((PI), (F4)) + (xbrPixelDiff((PH), (PF)) << 2); \
                i = xbrPixelDiff((PH), (PD)) + xbrPixelDiff((PH), (I5)) + xbrPixelDiff((PF), (I4)) + xbrPixelDiff((PF), (PB)) + (xbrPixelDiff((PE), (PI)) << 2); \
                if (e < i && ((!xbrEq((PF), (PB)) && !xbrEq((PH), (PD))) || (xbrEq((PE), (PI)) && (!xbrEq((PF), (I4)) && !xbrEq((PH), (I5)))) || xbrEq((PE), (PG)) || xbrEq((PE), (PC)))) { \
                    unsigned ke; \
                    unsigned ki; \
                    int left; \
                    int up; \
                    ke = xbrPixelDiff((PF), (PG)); \
                    ki = xbrPixelDiff((PH), (PC)); \
                    left = (ke << 1) <= ki && (PE) != (PG) && (PD) != (PG); \
                    up = ke >= (ki << 1) && (PE) != (PC) && (PB) != (PC); \
                    if (left && up) { \
                        E[(N3)] = xbrAlphaBlend224(E[(N3)], px); \
                        E[(N2)] = xbrAlphaBlend64(E[(N2)], px); \
                        E[(N1)] = E[(N2)]; \
                    } else if (left) { \
                        E[(N3)] = xbrAlphaBlend192(E[(N3)], px); \
                        E[(N2)] = xbrAlphaBlend64(E[(N2)], px); \
                    } else if (up) { \
                        E[(N3)] = xbrAlphaBlend192(E[(N3)], px); \
                        E[(N1)] = xbrAlphaBlend64(E[(N1)], px); \
                    } else { \
                        E[(N3)] = xbrAlphaBlend128(E[(N3)], px); \
                    } \
                } else { \
                    E[(N3)] = xbrAlphaBlend128(E[(N3)], px); \
                } \
            } \
        } \
    } while (0)

    dstW = srcW * 2;
    for (y = 0; y < srcH; y++) {
        for (x = 0; x < srcW; x++) {
            uint32_t pA;
            uint32_t pB;
            uint32_t pC;
            uint32_t pD;
            uint32_t pE;
            uint32_t pF;
            uint32_t pG;
            uint32_t pH;
            uint32_t pI;
            uint32_t o0;
            uint32_t o1;
            uint32_t o2;
            uint32_t o3;
            int dx;
            int dy;

            pA = srcPixelClamp(src, srcW, srcH, x - 1, y - 1);
            pB = srcPixelClamp(src, srcW, srcH, x, y - 1);
            pC = srcPixelClamp(src, srcW, srcH, x + 1, y - 1);
            pD = srcPixelClamp(src, srcW, srcH, x - 1, y);
            pE = srcPixelClamp(src, srcW, srcH, x, y);
            pF = srcPixelClamp(src, srcW, srcH, x + 1, y);
            pG = srcPixelClamp(src, srcW, srcH, x - 1, y + 1);
            pH = srcPixelClamp(src, srcW, srcH, x, y + 1);
            pI = srcPixelClamp(src, srcW, srcH, x + 1, y + 1);
            {
                uint32_t a0;
                uint32_t a1;
                uint32_t b1;
                uint32_t c1;
                uint32_t c4;
                uint32_t d0;
                uint32_t f4;
                uint32_t g0;
                uint32_t g5;
                uint32_t h5;
                uint32_t i4;
                uint32_t i5;
                uint32_t E[4];

                a1 = srcPixelClamp(src, srcW, srcH, x - 1, y - 2);
                b1 = srcPixelClamp(src, srcW, srcH, x, y - 2);
                c1 = srcPixelClamp(src, srcW, srcH, x + 1, y - 2);
                a0 = srcPixelClamp(src, srcW, srcH, x - 2, y - 1);
                d0 = srcPixelClamp(src, srcW, srcH, x - 2, y);
                g0 = srcPixelClamp(src, srcW, srcH, x - 2, y + 1);
                c4 = srcPixelClamp(src, srcW, srcH, x + 2, y - 1);
                f4 = srcPixelClamp(src, srcW, srcH, x + 2, y);
                i4 = srcPixelClamp(src, srcW, srcH, x + 2, y + 1);
                g5 = srcPixelClamp(src, srcW, srcH, x - 1, y + 2);
                h5 = srcPixelClamp(src, srcW, srcH, x, y + 2);
                i5 = srcPixelClamp(src, srcW, srcH, x + 1, y + 2);

                E[0] = pE;
                E[1] = pE;
                E[2] = pE;
                E[3] = pE;
                xbrFILT2(pE, pI, pH, pF, pG, pC, pD, pB, pA, g5, c4, g0, d0, c1, b1, f4, i4, h5, i5, a0, a1, 0, 1, 2, 3);
                xbrFILT2(pE, pC, pF, pB, pI, pA, pH, pD, pG, i4, a1, i5, h5, a0, d0, b1, c1, f4, c4, g5, g0, 2, 0, 3, 1);
                xbrFILT2(pE, pA, pB, pD, pC, pG, pF, pH, pI, c1, g0, c4, f4, g5, h5, d0, a0, b1, a1, i4, i5, 3, 2, 1, 0);
                xbrFILT2(pE, pG, pD, pH, pA, pI, pB, pF, pC, a0, i5, a1, b1, i4, f4, h5, g5, d0, g0, c1, c4, 1, 3, 0, 2);
                o0 = E[0];
                o1 = E[1];
                o2 = E[2];
                o3 = E[3];
            }

            dx = x * 2;
            dy = y * 2;
            dst[(size_t)dy * (size_t)dstW + (size_t)dx] = o0;
            dst[(size_t)dy * (size_t)dstW + (size_t)(dx + 1)] = o1;
            dst[(size_t)(dy + 1) * (size_t)dstW + (size_t)dx] = o2;
            dst[(size_t)(dy + 1) * (size_t)dstW + (size_t)(dx + 1)] = o3;
        }
    }
#undef xbrLbMask
#undef xbrRedBlueMask
#undef xbrGreenMask
#undef xbrAlphaBlend128
#undef xbrAlphaBlendBase
#undef xbrAlphaBlend32
#undef xbrAlphaBlend64
#undef xbrAlphaBlend192
#undef xbrAlphaBlend224
#undef xbrPixelDiff
#undef xbrEq
#undef xbrFILT2
}

/*
 * xBRz: same 2x output layout and blending weights as xBR, but Hyllian/FILT3-style inner
 * edge test (FFmpeg libavfilter/vf_xbr.c FILT3) to reduce zigzag artifacts vs plain FILT2.
 */
static void scaleXbrz2x(uint32_t* src, int srcW, int srcH, uint32_t* dst) {
    int y;
    int x;
    int dstW;

#define xbrLbMask 0x00FEFEFEu
#define xbrRedBlueMask 0x00FF00FFu
#define xbrGreenMask 0x0000FF00u
#define xbrAlphaBlend128(a, b) ((((a) & xbrLbMask) >> 1) + (((b) & xbrLbMask) >> 1))
#define xbrAlphaBlendBase(a, b, m, s) \
    ((xbrRedBlueMask & (((a) & xbrRedBlueMask) + (((((b) & xbrRedBlueMask) - ((a) & xbrRedBlueMask)) * (m)) >> (s)))) \
     | (xbrGreenMask & (((a) & xbrGreenMask) + (((((b) & xbrGreenMask) - ((a) & xbrGreenMask)) * (m)) >> (s)))))
#define xbrAlphaBlend32(a, b) xbrAlphaBlendBase(a, b, 1, 3)
#define xbrAlphaBlend64(a, b) xbrAlphaBlendBase(a, b, 1, 2)
#define xbrAlphaBlend192(a, b) xbrAlphaBlendBase(a, b, 3, 2)
#define xbrAlphaBlend224(a, b) xbrAlphaBlendBase(a, b, 7, 3)
#define xbrPixelDiff(a, b) xbrPixelDiffInline((a), (b))
#define xbrEq(a, b) (xbrPixelDiff((a), (b)) < 155u)
#define xbrzFILT2(PE, PI, PH, PF, PG, PC, PD, PB, PA, G5, C4, G0, D0, C1, B1, F4, I4, H5, I5, A0, A1, N0, N1, N2, N3) \
    do { \
        if ((PE) != (PH) && (PE) != (PF)) { \
            unsigned e; \
            unsigned i; \
            if (xbrPixelDiff((PE), (PC)) + xbrPixelDiff((PE), (PG)) + xbrPixelDiff((PI), (H5)) + xbrPixelDiff((PI), (F4)) + (xbrPixelDiff((PH), (PF)) << 2) \
                <= xbrPixelDiff((PH), (PD)) + xbrPixelDiff((PH), (I5)) + xbrPixelDiff((PF), (I4)) + xbrPixelDiff((PF), (PB)) + (xbrPixelDiff((PE), (PI)) << 2)) { \
                uint32_t px; \
                px = xbrPixelDiff((PE), (PF)) <= xbrPixelDiff((PE), (PH)) ? (PF) : (PH); \
                e = xbrPixelDiff((PE), (PC)) + xbrPixelDiff((PE), (PG)) + xbrPixelDiff((PI), (H5)) + xbrPixelDiff((PI), (F4)) + (xbrPixelDiff((PH), (PF)) << 2); \
                i = xbrPixelDiff((PH), (PD)) + xbrPixelDiff((PH), (I5)) + xbrPixelDiff((PF), (I4)) + xbrPixelDiff((PF), (PB)) + (xbrPixelDiff((PE), (PI)) << 2); \
                if (e < i && ((!xbrEq((PF), (PB)) && !xbrEq((PF), (PC))) || (!xbrEq((PH), (PD)) && !xbrEq((PH), (PG))) \
                    || (xbrEq((PE), (PI)) && ((!xbrEq((PF), (F4)) && !xbrEq((PF), (I4))) || (!xbrEq((PH), (H5)) && !xbrEq((PH), (I5))))) \
                    || xbrEq((PE), (PG)) || xbrEq((PE), (PC)))) { \
                    unsigned ke; \
                    unsigned ki; \
                    int left; \
                    int up; \
                    ke = xbrPixelDiff((PF), (PG)); \
                    ki = xbrPixelDiff((PH), (PC)); \
                    left = (ke << 1) <= ki && (PE) != (PG) && (PD) != (PG); \
                    up = ke >= (ki << 1) && (PE) != (PC) && (PB) != (PC); \
                    if (left && up) { \
                        E[(N3)] = xbrAlphaBlend224(E[(N3)], px); \
                        E[(N2)] = xbrAlphaBlend64(E[(N2)], px); \
                        E[(N1)] = E[(N2)]; \
                    } else if (left) { \
                        E[(N3)] = xbrAlphaBlend192(E[(N3)], px); \
                        E[(N2)] = xbrAlphaBlend64(E[(N2)], px); \
                    } else if (up) { \
                        E[(N3)] = xbrAlphaBlend192(E[(N3)], px); \
                        E[(N1)] = xbrAlphaBlend64(E[(N1)], px); \
                    } else { \
                        E[(N3)] = xbrAlphaBlend128(E[(N3)], px); \
                    } \
                } else { \
                    E[(N3)] = xbrAlphaBlend128(E[(N3)], px); \
                } \
            } \
        } \
    } while (0)

    dstW = srcW * 2;
    for (y = 0; y < srcH; y++) {
        for (x = 0; x < srcW; x++) {
            uint32_t pA;
            uint32_t pB;
            uint32_t pC;
            uint32_t pD;
            uint32_t pE;
            uint32_t pF;
            uint32_t pG;
            uint32_t pH;
            uint32_t pI;
            uint32_t o0;
            uint32_t o1;
            uint32_t o2;
            uint32_t o3;
            int dx;
            int dy;

            pA = srcPixelClamp(src, srcW, srcH, x - 1, y - 1);
            pB = srcPixelClamp(src, srcW, srcH, x, y - 1);
            pC = srcPixelClamp(src, srcW, srcH, x + 1, y - 1);
            pD = srcPixelClamp(src, srcW, srcH, x - 1, y);
            pE = srcPixelClamp(src, srcW, srcH, x, y);
            pF = srcPixelClamp(src, srcW, srcH, x + 1, y);
            pG = srcPixelClamp(src, srcW, srcH, x - 1, y + 1);
            pH = srcPixelClamp(src, srcW, srcH, x, y + 1);
            pI = srcPixelClamp(src, srcW, srcH, x + 1, y + 1);
            {
                uint32_t a0;
                uint32_t a1;
                uint32_t b1;
                uint32_t c1;
                uint32_t c4;
                uint32_t d0;
                uint32_t f4;
                uint32_t g0;
                uint32_t g5;
                uint32_t h5;
                uint32_t i4;
                uint32_t i5;
                uint32_t E[4];

                a1 = srcPixelClamp(src, srcW, srcH, x - 1, y - 2);
                b1 = srcPixelClamp(src, srcW, srcH, x, y - 2);
                c1 = srcPixelClamp(src, srcW, srcH, x + 1, y - 2);
                a0 = srcPixelClamp(src, srcW, srcH, x - 2, y - 1);
                d0 = srcPixelClamp(src, srcW, srcH, x - 2, y);
                g0 = srcPixelClamp(src, srcW, srcH, x - 2, y + 1);
                c4 = srcPixelClamp(src, srcW, srcH, x + 2, y - 1);
                f4 = srcPixelClamp(src, srcW, srcH, x + 2, y);
                i4 = srcPixelClamp(src, srcW, srcH, x + 2, y + 1);
                g5 = srcPixelClamp(src, srcW, srcH, x - 1, y + 2);
                h5 = srcPixelClamp(src, srcW, srcH, x, y + 2);
                i5 = srcPixelClamp(src, srcW, srcH, x + 1, y + 2);

                E[0] = pE;
                E[1] = pE;
                E[2] = pE;
                E[3] = pE;
                xbrzFILT2(pE, pI, pH, pF, pG, pC, pD, pB, pA, g5, c4, g0, d0, c1, b1, f4, i4, h5, i5, a0, a1, 0, 1, 2, 3);
                xbrzFILT2(pE, pC, pF, pB, pI, pA, pH, pD, pG, i4, a1, i5, h5, a0, d0, b1, c1, f4, c4, g5, g0, 2, 0, 3, 1);
                xbrzFILT2(pE, pA, pB, pD, pC, pG, pF, pH, pI, c1, g0, c4, f4, g5, h5, d0, a0, b1, a1, i4, i5, 3, 2, 1, 0);
                xbrzFILT2(pE, pG, pD, pH, pA, pI, pB, pF, pC, a0, i5, a1, b1, i4, f4, h5, g5, d0, g0, c1, c4, 1, 3, 0, 2);
                o0 = E[0];
                o1 = E[1];
                o2 = E[2];
                o3 = E[3];
            }

            dx = x * 2;
            dy = y * 2;
            dst[(size_t)dy * (size_t)dstW + (size_t)dx] = o0;
            dst[(size_t)dy * (size_t)dstW + (size_t)(dx + 1)] = o1;
            dst[(size_t)(dy + 1) * (size_t)dstW + (size_t)dx] = o2;
            dst[(size_t)(dy + 1) * (size_t)dstW + (size_t)(dx + 1)] = o3;
        }
    }
#undef xbrLbMask
#undef xbrRedBlueMask
#undef xbrGreenMask
#undef xbrAlphaBlend128
#undef xbrAlphaBlendBase
#undef xbrAlphaBlend32
#undef xbrAlphaBlend64
#undef xbrAlphaBlend192
#undef xbrAlphaBlend224
#undef xbrPixelDiff
#undef xbrEq
#undef xbrzFILT2
}

static int ensureScaledFrameBuffer(AmigaHostSdl* host, int dstW, int dstH) {
    size_t needCount;
    uint32_t* newBuffer;

    assert(host && dstW > 0 && dstH > 0);
    if (host->scaledFrameBuffer != NULL
        && host->scaledFrameWidth == dstW
        && host->scaledFrameHeight == dstH) {
        return 1;
    }
    if (host->scaledFrameBuffer != NULL) {
        free(host->scaledFrameBuffer);
        host->scaledFrameBuffer = NULL;
    }
    needCount = (size_t)dstW * (size_t)dstH;
    newBuffer = (uint32_t*)malloc(needCount * sizeof(uint32_t));
    if (newBuffer == NULL) {
        host->scaledFrameWidth = 0;
        host->scaledFrameHeight = 0;
        return 0;
    }
    host->scaledFrameBuffer = newBuffer;
    host->scaledFrameWidth = dstW;
    host->scaledFrameHeight = dstH;
    return 1;
}

static void choosePresentedFrame(AmigaHostSdl* host, uint32_t** frameOut, int* wOut, int* hOut) {
    int dstW;
    int dstH;
    int useScaleMode;
    uint64_t scaleStart;
    uint64_t scaleEnd;

    assert(frameOut && wOut && hOut && host);
    *frameOut = host->frameBuffer;
    *wOut = host->width;
    *hOut = host->height;
    assert(host->frameBuffer && host->width > 0 && host->height > 0);
    useScaleMode = host->scaleMode;
    if (useScaleMode == amigaHostScaleModeNormal) {
        host->lastScaleMs = 0.0;
        return;
    }
    dstW = host->width * 2;
    dstH = host->height * 2;
    if (!ensureScaledFrameBuffer(host, dstW, dstH)) {
        return;
    }
    scaleStart = SDL_GetPerformanceCounter();
    if (useScaleMode == amigaHostScaleModeXbrz) {
        scaleXbrz2x(host->frameBuffer, host->width, host->height, host->scaledFrameBuffer);
    } else if (useScaleMode == amigaHostScaleModeXbr) {
        scaleXbr2x(host->frameBuffer, host->width, host->height, host->scaledFrameBuffer);
    } else {
        scaleXbr2x(host->frameBuffer, host->width, host->height, host->scaledFrameBuffer);
    }
    scaleEnd = SDL_GetPerformanceCounter();
    host->lastScaleMs = (double)(scaleEnd - scaleStart) * 1000.0
                        / (double)SDL_GetPerformanceFrequency();
    *frameOut = host->scaledFrameBuffer;
    *wOut = dstW;
    *hOut = dstH;
}

static int ensurePresentationTexture(AmigaHostSdl* host, int frameW, int frameH) {
    assert(host && frameW > 0 && frameH > 0);
    if (host->texture != NULL && host->textureWidth == frameW && host->textureHeight == frameH) {
        return 1;
    }
    if (host->texture != NULL) {
        SDL_DestroyTexture((SDL_Texture*)host->texture);
        host->texture = NULL;
    }
    host->texture = SDL_CreateTexture((SDL_Renderer*)host->renderer,
                                      SDL_PIXELFORMAT_ARGB8888,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      frameW,
                                      frameH);
    if (host->texture == NULL) {
        host->textureWidth = 0;
        host->textureHeight = 0;
        return 0;
    }
    host->textureWidth = frameW;
    host->textureHeight = frameH;
    return 1;
}

static void computePresentationRect(AmigaHostSdl* host,
                                    int sourceW,
                                    int sourceH,
                                    int* outX,
                                    int* outY,
                                    int* outW,
                                    int* outH) {
    int outputW;
    int outputH;
    int scaledW;
    int scaledH;
    int x;
    int y;
    float sx;
    float sy;
    float scale;

    assert(host && outX && outY && outW && outH);
    outputW = 0;
    outputH = 0;
    if (host->renderer != NULL) {
        (void)SDL_GetRendererOutputSize((SDL_Renderer*)host->renderer, &outputW, &outputH);
    }
    if (outputW <= 0 || outputH <= 0) {
        SDL_Window* window;

        window = (SDL_Window*)host->window;
        if (window != NULL) {
            SDL_GetWindowSize(window, &outputW, &outputH);
        }
    }
    if (outputW <= 0) {
        outputW = sourceW;
    }
    if (outputH <= 0) {
        outputH = sourceH;
    }
    if (sourceW <= 0 || sourceH <= 0) {
        *outX = 0;
        *outY = 0;
        *outW = outputW;
        *outH = outputH;
        return;
    }

    sx = (float)outputW / (float)sourceW;
    sy = (float)outputH / (float)sourceH;
    scale = sx;
    if (sy < scale) {
        scale = sy;
    }
    if (scale <= 0.0f) {
        scale = 1.0f;
    }
    scaledW = (int)((float)sourceW * scale + 0.5f);
    scaledH = (int)((float)sourceH * scale + 0.5f);
    if (scaledW < 1) {
        scaledW = 1;
    }
    if (scaledH < 1) {
        scaledH = 1;
    }
    if (scaledW > outputW) {
        scaledW = outputW;
    }
    if (scaledH > outputH) {
        scaledH = outputH;
    }
    x = (outputW - scaledW) / 2;
    y = (outputH - scaledH) / 2;

    *outX = x;
    *outY = y;
    *outW = scaledW;
    *outH = scaledH;
}

static void mapWindowMouseToGameCoords(AmigaHostSdl* host, int winX, int winY,
                                       int clipToViewport,
                                       int* gameXOut, int* gameYOut) {
    int dstX;
    int dstY;
    int dstW;
    int dstH;
    int localX;
    int localY;
    int gameX;
    int gameY;

    assert(host && gameXOut && gameYOut);
    *gameXOut = -1;
    *gameYOut = -1;

    /*
     * Mouse events report window-point coordinates, but the presentation rect is
     * computed in renderer-output (physical) pixels. On a HighDPI display those
     * differ by the backing-scale factor, so convert points -> output pixels here.
     */
    {
        int outputW;
        int outputH;
        int windowW;
        int windowH;

        outputW = 0;
        outputH = 0;
        windowW = 0;
        windowH = 0;
        if (host->renderer != NULL) {
            SDL_GetRendererOutputSize((SDL_Renderer*)host->renderer, &outputW, &outputH);
        }
        if (host->window != NULL) {
            SDL_GetWindowSize((SDL_Window*)host->window, &windowW, &windowH);
        }
        if (windowW > 0 && outputW > 0 && windowW != outputW) {
            winX = winX * outputW / windowW;
        }
        if (windowH > 0 && outputH > 0 && windowH != outputH) {
            winY = winY * outputH / windowH;
        }
    }

    computePresentationRect(host, host->width, host->height, &dstX, &dstY, &dstW,
                            &dstH);
    if (dstW <= 0 || dstH <= 0 || host->width <= 0 || host->height <= 0) {
        return;
    }

    localX = winX - dstX;
    localY = winY - dstY;
    if (clipToViewport != 0) {
        if (localX < 0) {
            localX = 0;
        } else if (localX >= dstW) {
            localX = dstW - 1;
        }
        if (localY < 0) {
            localY = 0;
        } else if (localY >= dstH) {
            localY = dstH - 1;
        }
    } else if (localX < 0 || localY < 0 || localX >= dstW || localY >= dstH) {
        return;
    }

    gameX = localX * host->width / dstW;
    gameY = localY * host->height / dstH;
    if (gameX < 0) {
        gameX = 0;
    }
    if (gameY < 0) {
        gameY = 0;
    }
    if (gameX >= host->width) {
        gameX = host->width - 1;
    }
    if (gameY >= host->height) {
        gameY = host->height - 1;
    }
    *gameXOut = gameX;
    *gameYOut = gameY;
}

/*
 * $dff00a/$dff00b (JOY0DAT): Amiga mouse port — byte counters the game diffs
 * for movement (ClockAGA movsprite). SDL relative motion accumulates here.
 *
 * $dff00c/$dff00d (JOY1DAT): keyboard / joystick quadrature encodings for
 * shell remappable keys and gamepad.
 * $bfe001 is CIAB PRB (offset 0xe001 in our stub): active-low fire/waitgo lines.
 */
static void updateMouseJoydat(AmigaHostSdl* host) {
    M68k* cpu;
    int dx;
    int dy;

    if (host->mouseJoy0Enabled == 0) {
        return;
    }

    cpu = host->cpu;
    if (cpu == NULL) {
        return;
    }

    (void)SDL_GetRelativeMouseState(&dx, &dy);
    host->mouseXCounter = (uint8_t)(host->mouseXCounter + (uint8_t)dx);
    host->mouseYCounter = (uint8_t)(host->mouseYCounter + (uint8_t)dy);

    cpu->customRegs[0x0Au] = host->mouseYCounter;
    cpu->customRegs[0x0Bu] = host->mouseXCounter;
}

static void updateCiaInput(AmigaHostSdl* host) {
    M68k* cpu;
    uint8_t ciaValue;
    uint8_t bfePrb;
    uint32_t mouseButtons;
    int up;
    int down;
    int left;
    int right;
    uint16_t joy1Word;

    cpu = host->cpu;
    if (cpu == NULL) {
        return;
    }

    ciaValue = 0xFFu;
    bfePrb = 0xFFu;
    up = 0;
    down = 0;
    left = 0;
    right = 0;

    if (hostSdlControllerButtonHeld(SDL_CONTROLLER_BUTTON_DPAD_UP)
        || hostSdlControllerLeftStickUp()) {
        up = 1;
    }
    if (hostSdlControllerButtonHeld(SDL_CONTROLLER_BUTTON_DPAD_DOWN)
        || hostSdlControllerLeftStickDown()) {
        down = 1;
    }
    if (hostSdlControllerButtonHeld(SDL_CONTROLLER_BUTTON_DPAD_LEFT)
        || hostSdlControllerLeftStickLeft()) {
        left = 1;
    }
    if (hostSdlControllerButtonHeld(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)
        || hostSdlControllerLeftStickRight()) {
        right = 1;
    }

    if (up) {
        ciaValue &= (uint8_t)~(1u << 0);
    }
    if (down) {
        ciaValue &= (uint8_t)~(1u << 1);
    }
    if (left) {
        ciaValue &= (uint8_t)~(1u << 2);
    }
    if (right) {
        ciaValue &= (uint8_t)~(1u << 3);
    }
    if (hostSdlControllerButtonHeld(SDL_CONTROLLER_BUTTON_A)) {
        ciaValue &= (uint8_t)~(1u << 4);
        bfePrb   &= (uint8_t)~(1u << 6);
        bfePrb   &= (uint8_t)~(1u << 7);
    }

    if (host->mouseJoy0Enabled != 0) {
        mouseButtons = SDL_GetMouseState(NULL, NULL);
        if ((mouseButtons & SDL_BUTTON_LMASK) != 0) {
            bfePrb &= (uint8_t)~(1u << 6);
        }
        if ((mouseButtons & SDL_BUTTON_RMASK) != 0) {
            bfePrb &= (uint8_t)~(1u << 7);
        }
    }

    updateMouseJoydat(host);

    joy1Word = 0u;
    if (up && left) {
        joy1Word = 0x0200u;
    } else if (up && right) {
        joy1Word = 0x0103u;
    } else if (down && left) {
        joy1Word = 0x0301u;
    } else if (down && right) {
        joy1Word = 0x0002u;
    } else if (down) {
        joy1Word = 0x0001u;
    } else if (up) {
        joy1Word = 0x0100u;
    } else if (left) {
        joy1Word = 0x0300u;
    } else if (right) {
        joy1Word = 0x0003u;
    }

    cpu->customRegs[0x0Cu] = (uint8_t)((joy1Word >> 8) & 0xFFu);
    cpu->customRegs[0x0Du] = (uint8_t)(joy1Word & 0xFFu);

    cpu->ciaRegs[0xE001u] = bfePrb;
    cpu->ciaRegs[0xDC00u] = ciaValue;
    cpu->ciaRegs[0xDC01u] = ciaValue;
}

static int bitplaneCountFromChipRegs(M68k* cpu) {
    uint16_t bplcon0;
    uint16_t bplcon4;
    uint16_t ddfstrt;
    int bpu;
    int count;

    bplcon0 = readCustomWord(cpu, 0x00000100u);
    bplcon4 = readCustomWord(cpu, 0x0000010Cu);
    ddfstrt = readCustomWord(cpu, 0x00000092u);
    if (bplcon0 == 0u && ddfstrt == 0u) {
        return 0;
    }
    /*
     * AGA BPU<3:0>: bits 14-12 (BPU2..0) plus bit 4 (BPU3 extension).
     * BPU<3:0> directly encodes the plane count (0000 = display off,
     * 0001 = 1 plane, ..., 1000 = 8 planes; 1001-1111 are illegal).
     * No +1 here — that off-by-one only happened to look right for
     * BPU<3:0> = 1000 because the result clamped back to 8.
     */
    bpu = (int)((bplcon0 >> 12) & 0x7u);
    if ((bplcon0 & 0x0010u) != 0u) {
        bpu |= 8;
    }
    bpu |= (int)((bplcon4 >> 13) & 0x4u);
    count = bpu;
    if (count > AMIGA_HOST_MAX_PLANES) {
        count = AMIGA_HOST_MAX_PLANES;
    }
    return count;
}

/*
 * Pre-AGA compositor plane count (hoilevel* tested behaviour): BPLCON0
 * bits 14-12 used directly as the number of planes to fetch, not BPU+1.
 */
static int bitplaneCountOcsLegacy(M68k* cpu, uint16_t bplcon0) {
    uint16_t ddfstrt;

    ddfstrt = readCustomWord(cpu, 0x00000092u);
    if (bplcon0 == 0u && ddfstrt == 0u) {
        return 0;
    }
    return (int)((bplcon0 >> 12) & 0x7u);
}

static int countActiveBitplanePointers(M68k* cpu) {
    int i;
    int last;
    last = 0;
    for (i = 0; i < AMIGA_HOST_MAX_PLANES; i++) {
        if (planeBasePointer(cpu, i) != 0u) {
            last = i + 1;
        }
    }
    return last;
}

static int countConsecutiveInterleaved40PlaneSlices(uint32_t* planeBase) {
    int i;

    if (planeBase[0] == 0u) {
        return 0;
    }
    for (i = 1; i < AMIGA_HOST_MAX_PLANES; i++) {
        if (planeBase[i] != planeBase[i - 1] + 40u) {
            break;
        }
    }
    return i;
}

/*
 * AGA titles set BPLCON0 BPU3 and/or non-zero BPLCON4 ($8011, $0011, ...).
 * OCS/ECS games (hoilevel*) leave both at zero for the whole copper list.
 */
static int chipsetAgaVideoActive(uint16_t bplcon0, uint16_t bplcon4) {
    if ((bplcon0 & 0x0010u) != 0u) {
        return 1;
    }
    if ((bplcon4 & 0x8000u) != 0u) {
        return 1;
    }
    if ((bplcon4 & 0x0010u) != 0u) {
        return 1;
    }
    if (((bplcon4 >> 13) & 0x4u) != 0u) {
        return 1;
    }
    return 0;
}

static int bitplaneCountForScanline(M68k* cpu,
                                    uint16_t bplcon0,
                                    uint16_t bplcon4,
                                    int agaActive) {
    int planeCount;

    (void)bplcon4;
    if (agaActive) {
        planeCount = bitplaneCountFromChipRegs(cpu);
        if (planeCount < 1) {
            int fromPtr;
            fromPtr = countActiveBitplanePointers(cpu);
            if (fromPtr > 0) {
                planeCount = fromPtr;
            }
        }
        {
            uint32_t planeBase[AMIGA_HOST_MAX_PLANES];
            int16_t bpl1mod;
            int slices;
            int x;
            bpl1mod = (int16_t)readCustomWord(cpu, 0x00000108u);
            for (x = 0; x < AMIGA_HOST_MAX_PLANES; x++) {
                planeBase[x] = planeBasePointer(cpu, x);
            }
            slices = countConsecutiveInterleaved40PlaneSlices(planeBase);
            if (slices >= 5 && bpl1mod == 232) {
                if (planeCount < slices) {
                    planeCount = slices;
                }
                if (planeCount > 7) {
                    planeCount = 7;
                }
            }
        }
    } else {
        planeCount = bitplaneCountOcsLegacy(cpu, bplcon0);
    }
    return planeCount;
}

static int applyBplamIndex(M68k* cpu, int colorIdx) {
    uint16_t bplcon4;
    int masked;

    bplcon4 = readCustomWord(cpu, 0x0000010Cu);
    masked = colorIdx ^ (int)((bplcon4 >> 8) & 0xFFu);
    return masked & 255;
}

static uint32_t lookupAgaPlayfieldColorDirect(M68k* cpu, int colorIdx) {
    return m68kAgaColorArgb(cpu, colorIdx & 255);
}

/*
 * AGA dual-playfield colour register selection.
 *
 *   pf1Reg = BPLAM XOR pf1Idx
 *   pf2Reg = pf2Idx OR (1 << PF2OF)
 *
 * BPLAM is the high byte of BPLCON4, PF2OF is BPLCON3 bits 12..10. Each
 * playfield's index 0 stays transparent and falls through to COLOR00 (or
 * to the other playfield when that one is opaque).
 *
 * This generalises what was previously hardcoded to PF1 -> reg 16+idx,
 * PF2 -> reg 24+idx, which only happened to match the first ClockAGA port
 * (BPLAM=$10, PF2OF=011b giving +8). Other AGA games use different banks,
 * e.g. clockwiser's game area pings PF2OF between 110b (+64) and 101b
 * (+32) to ping-pong palette banks per scanline.
 */
static uint32_t lookupAgaDualPfColor(M68k* cpu, int pf1Idx, int pf2Idx, int pf2Pri) {
    uint16_t bplcon3;
    uint16_t bplcon4;
    int bplam;
    int pf2of;
    int pf2BaseBit;
    int regIdx;

    bplcon3 = readCustomWord(cpu, 0x00000106u);
    bplcon4 = readCustomWord(cpu, 0x0000010Cu);
    bplam = (int)((bplcon4 >> 8) & 0xFFu);
    pf2of = (int)((bplcon3 >> 10) & 0x7u);
    pf2BaseBit = 1 << pf2of;

    regIdx = 0;
    if (pf2Pri) {
        if (pf2Idx != 0) {
            regIdx = pf2Idx | pf2BaseBit;
        } else if (pf1Idx != 0) {
            regIdx = bplam ^ pf1Idx;
        }
    } else {
        if (pf1Idx != 0) {
            regIdx = bplam ^ pf1Idx;
        } else if (pf2Idx != 0) {
            regIdx = pf2Idx | pf2BaseBit;
        }
    }
    return m68kAgaColorArgb(cpu, regIdx & 0xFF);
}

/*
 * AGA dual-playfield with the chip-side interleave that ClockAGA's
 * "bluelopt" routine and clockwiser's game/text area both use: PF1 planes
 * (0/2/4 and optionally 6) live in one chip block, PF2 planes (1/3/5 and
 * optionally 7) in another, each block laid out as N 40-byte slices per
 * row with a 160-byte total row pitch (4 slots reserved per row regardless
 * of whether the 4th plane is active). Handles both the 6-plane DPF case
 * and the 8-plane DPF case clockwiser uses for its top text band and
 * middle game area.
 *
 * With this layout the Denise per-plane modulo (typically 112) yields a
 * naive stride of 40+112 = 152, which is 8 bytes short of the real 160.
 * Without this detector firing, the image shears by 8 bytes per scanline.
 */
static int detectAgaDualPfInterleaved160(uint16_t bplcon0, uint32_t* planeBase, int planeCount) {
    uint32_t pf1Base;
    uint32_t pf2Base;
    if (planeCount != 6 && planeCount != 8) {
        return 0;
    }
    if ((bplcon0 & 0x0610u) != 0x0610u) {
        return 0;
    }
    pf1Base = planeBase[0];
    pf2Base = planeBase[1];
    if (pf1Base == 0u || pf2Base == 0u) {
        return 0;
    }
    if (planeBase[2] != pf1Base + 40u) {
        return 0;
    }
    if (planeBase[4] != pf1Base + 80u) {
        return 0;
    }
    if (planeBase[3] != pf2Base + 40u) {
        return 0;
    }
    if (planeBase[5] != pf2Base + 80u) {
        return 0;
    }
    if (planeCount > 6) {
        if (planeBase[6] != pf1Base + 120u) {
            return 0;
        }
        if (planeBase[7] != pf2Base + 120u) {
            return 0;
        }
    }
    return 1;
}

static void applyAgaDualPfInterleaved160LineBases(int y,
                                                uint32_t* planeAnchorBase,
                                                int* planeAnchorY,
                                                int planeCount,
                                                int64_t* planeLineBase) {
    int rowPitch;
    int dy0;
    int dy1;
    uint32_t pf1Row;
    uint32_t pf2Row;

    rowPitch = 160;
    dy0 = y - planeAnchorY[0];
    dy1 = y - planeAnchorY[1];
    pf1Row = planeAnchorBase[0] + (uint32_t)(dy0 * rowPitch);
    pf2Row = planeAnchorBase[1] + (uint32_t)(dy1 * rowPitch);
    planeLineBase[0] = (int64_t)pf1Row;
    planeLineBase[2] = (int64_t)(pf1Row + 40u);
    planeLineBase[4] = (int64_t)(pf1Row + 80u);
    planeLineBase[1] = (int64_t)pf2Row;
    planeLineBase[3] = (int64_t)(pf2Row + 40u);
    planeLineBase[5] = (int64_t)(pf2Row + 80u);
    if (planeCount > 6) {
        planeLineBase[6] = (int64_t)(pf1Row + 120u);
        planeLineBase[7] = (int64_t)(pf2Row + 120u);
    }
}

/*
 * Single-playfield bitmap with seven 40-byte interleaved slices per row
 * (chip rowPitch 280) and BPL1MOD = 232 — the "Denise mod gives 272/step
 * but chip layout pitch is 280" pattern. Detection is structural: BPL1MOD
 * is 232 and active BPL pointers are consecutive +40 byte slices. Some
 * AGA games leave the DPF bit set in BPLCON0 even though the fetch is
 * really single-playfield interleaved data (not a dual-playfield pair).
 */
static int detectAgaSinglePfInterleaved40(uint16_t bplcon0,
                                          int16_t bpl1mod,
                                          uint32_t* planeBase,
                                          int planeCount) {
    int slices;
    int i;

    (void)planeCount;
    if ((bplcon0 & 0x8000u) != 0u) {
        return 0;
    }
    if (bpl1mod != 232) {
        return 0;
    }
    slices = countConsecutiveInterleaved40PlaneSlices(planeBase);
    if (slices < 5) {
        return 0;
    }
    for (i = 0; i < slices; i++) {
        if (planeBase[i] == 0u) {
            return 0;
        }
    }
    return 1;
}

static void applyAgaSinglePfInterleaved40LineBases(int y,
                                                   uint32_t* planeAnchorBase,
                                                   int* planeAnchorY,
                                                   int planeCount,
                                                   int64_t* planeLineBase) {
    int dy;
    int rowPitch;
    uint32_t rowBase;
    int i;

    /*
     * Denise mod 232 gives 272 bytes/step but the bitmap row pitch in chip
     * RAM is 280 (40 bytes per plane slice, all planes interleaved).
     */
    rowPitch = 280;
    dy = y - planeAnchorY[0];
    rowBase = planeAnchorBase[0] + (uint32_t)(dy * rowPitch);
    for (i = 0; i < planeCount; i++) {
        planeLineBase[i] = (int64_t)(rowBase + (uint32_t)(i * 40));
    }
}

/*
 * 0 = generic modulo stride, 1 = AGA SPF interleaved-40, 2 = AGA DPF interleaved-160.
 */
static int agaFetchLayoutKind(uint16_t bplcon0,
                              int16_t bpl1mod,
                              uint32_t* planeBase,
                              int planeCount,
                              int dpf) {
    if (detectAgaSinglePfInterleaved40(bplcon0, bpl1mod, planeBase, planeCount)) {
        return 1;
    }
    if (dpf && detectAgaDualPfInterleaved160(bplcon0, planeBase, planeCount)) {
        return 2;
    }
    return 0;
}

static uint32_t fetchLayoutKey(uint16_t bplcon0,
                             int16_t bpl1mod,
                             int16_t bpl2mod,
                             int layoutKind) {
    uint32_t key;

    key = ((uint32_t)bplcon0 << 16) ^ (uint32_t)(uint16_t)bpl1mod;
    key ^= ((uint32_t)(uint16_t)bpl2mod << 8);
    key ^= ((uint32_t)layoutKind << 4);
    return key;
}

static void renderBitplanesLowres(AmigaHostSdl* host) {
    M68k* cpu;
    int displayStartY;
    uint32_t planeAnchorBase[AMIGA_HOST_MAX_PLANES];
    uint32_t planeLastRawBase[AMIGA_HOST_MAX_PLANES];
    int planeAnchorY[AMIGA_HOST_MAX_PLANES];
    int planeLastRowStride[AMIGA_HOST_MAX_PLANES];
    int planeAnchorValid[AMIGA_HOST_MAX_PLANES];
    uint32_t copperPc;
    int copperDone;
    int copperFrameHighWaitSeen;
    uint16_t color00;
    int bytesPerRow;
    int y;
    int x;
    int printedActiveLineDbg;
    uint16_t peakBplcon0;
    uint16_t peakBplcon4;
    int peakPlaneCount;
    uint32_t lastFetchLayoutKey;
    int fetchLayoutValid;
    int spritePairBeamLineCount[4];
    uint32_t spritePairLastEvenPtr[4];

    cpu = host->cpu;
    if (cpu == NULL) {
        return;
    }
    peakBplcon0 = 0u;
    peakBplcon4 = 0u;
    peakPlaneCount = 0;
    displayStartY = decodeDisplayWindowStartY(cpu);
    for (x = 0; x < AMIGA_HOST_MAX_PLANES; x++) {
        planeAnchorBase[x] = 0u;
        planeLastRawBase[x] = 0u;
        planeAnchorY[x] = 0;
        planeLastRowStride[x] = 0;
        planeAnchorValid[x] = 0;
    }
    for (x = 0; x < 4; x++) {
        spritePairBeamLineCount[x] = 0;
        spritePairLastEvenPtr[x] = 0u;
    }

    copperPc = copperListStartPc(cpu);
    copperDone = 0;
    copperFrameHighWaitSeen = 0;
    printedActiveLineDbg = 0;
    lastFetchLayoutKey = 0u;
    fetchLayoutValid = 0;
    runCopperCatchUpToBeam(
        cpu, &copperPc, displayStartY - 1, displayStartY, 0, &copperDone,
        &copperFrameHighWaitSeen);

    for (y = 0; y < host->height; y++) {
        uint16_t bplcon0;
        uint16_t bplcon1;
        uint16_t bplcon2;
        uint16_t bplcon4;
        int agaActive;
        int hScroll0;
        int hScrollPf2;
        int dpf;
        int ddfOffsetPixels;
        int16_t bpl1mod;
        int16_t bpl2mod;
        int planeCount;
        int maxPlanes;
        int singlePfInterleaved40;
        int fetchLayoutKind;
        uint32_t fetchLayoutKeyNow;
        int forceReanchor;
        int fetchedWidthBits;
        uint32_t palette[AMIGA_HOST_MAX_PALETTE];
        uint32_t planeBase[AMIGA_HOST_MAX_PLANES];
        int planeRowStride[AMIGA_HOST_MAX_PLANES];
        int64_t planeLineBase[AMIGA_HOST_MAX_PLANES];
        int diwXLeft;
        int diwXRight;
        int diwVTop;
        int diwVBottom;
        int amigaScanline;
        int diwVisibleY;

        runCopperCatchUpToBeam(
            cpu, &copperPc, displayStartY + y, displayStartY, 1, &copperDone,
            &copperFrameHighWaitSeen);
        bplcon0 = readCustomWord(cpu, 0x00000100u);
        bplcon1 = readCustomWord(cpu, 0x00000102u);
        bplcon2 = readCustomWord(cpu, 0x00000104u);
        bplcon4 = readCustomWord(cpu, 0x0000010Cu);
        agaActive = chipsetAgaVideoActive(bplcon0, bplcon4);
        {
            int scanPlanes;
            scanPlanes = bitplaneCountForScanline(cpu, bplcon0, bplcon4, agaActive);
            if (scanPlanes > peakPlaneCount) {
                peakPlaneCount = scanPlanes;
                peakBplcon0 = bplcon0;
                peakBplcon4 = bplcon4;
            }
        }
        computeDiwHorizontalRange(cpu, host->width, &diwXLeft, &diwXRight);
        computeDiwVerticalRange(cpu, &diwVTop, &diwVBottom);
        amigaScanline = displayStartY + y;
        diwVisibleY = (amigaScanline >= diwVTop && amigaScanline < diwVBottom);
        /*
         * Mode bits we look at every scanline so the copperlist can flip
         * playfield mode on/off mid-frame:
         *   BPLCON0 bit 10 (0x0400) = DPF (dual-playfield)
         *   BPLCON1 low nibble = PF1H (PF1 horizontal scroll 0..15)
         *   BPLCON1 high nibble = PF2H (PF2 horizontal scroll 0..15)
         *   BPLCON2 bit 6 (0x0040) = PF2PRI (PF2 in front of PF1 when set)
         * Our shifter order matches a compositor with realscroll = 15 - nibble.
         */
        dpf = (bplcon0 & 0x0400u) != 0u;
        hScroll0 = 15 - (int)(bplcon1 & 0x0Fu);
        hScrollPf2 = 15 - (int)((bplcon1 >> 4) & 0x0Fu);
        ddfOffsetPixels = computeDdfHorizontalOffsetPixels(cpu);
        color00 = m68kAgaColor12(cpu, 0);
        planeCount = bitplaneCountForScanline(cpu, bplcon0, bplcon4, agaActive);
        /*
         * areyoucop v=$cc reloads BPLxPT while BPLCON0 still says fetch-off.
         * bitplaneCountForScanline can infer planes from those stale pointers;
         * discard only that inference, not a real BPU-backed fetch ($7200 etc.).
         */
        if (agaActive) {
            int regPlaneCount;
            regPlaneCount = bitplaneCountFromChipRegs(cpu);
            if (regPlaneCount < 1 && planeCount > 0) {
                planeCount = 0;
            }
        }
        singlePfInterleaved40 = 0;
        if (planeCount < 1) {
            uint32_t bg = color12ToArgb(color00);
            /*
             * Playfield-off lines (e.g. areyoucop v=$cc) can reload BPLxPT
             * while Denise is not fetching. Do not anchor there; force a
             * fresh anchor on the first active line after the transition.
             */
            fetchLayoutValid = 0;
            for (x = 0; x < host->width; x++) {
                host->frameBuffer[y * host->width + x] = bg;
            }
            drawSpritesOcsForScanline(
                host, cpu, y, amigaScanline, spritePairBeamLineCount, spritePairLastEvenPtr);
            continue;
        }
        /*
         * OCS/ECS (hoilevel*): legacy plane count and 5/6-plane caps.
         * AGA (ClockAGA): BPU+1, up to 8 planes, BPLAM palette XOR.
         */
        if (agaActive) {
            maxPlanes = AMIGA_HOST_MAX_PLANES;
//            if (dpf && planeCount > 6) {
//                planeCount = 6;
//           }
        } else {
            maxPlanes = dpf ? 6 : 5;
        }
        if (planeCount > maxPlanes) {
            planeCount = maxPlanes;
        }
        bpl1mod = (int16_t)readCustomWord(cpu, 0x00000108u);
        bpl2mod = (int16_t)readCustomWord(cpu, 0x0000010Au);
        bytesPerRow = chooseBitplaneBytesPerRow(cpu, host->width);
        if (bytesPerRow <= 0) {
            drawSpritesOcsForScanline(
                host, cpu, y, amigaScanline, spritePairBeamLineCount, spritePairLastEvenPtr);
            continue;
        }
        fetchedWidthBits = bytesPerRow * 8;
        if (!printedActiveLineDbg && (host->frameCounter % 120u) == 0u) {
            uint32_t bplpt0;
            bplpt0 = planeBasePointer(cpu, 0);
            fprintf(stderr,
                    "video active y=%d aga=%d bplcon0=0x%04x bplcon4=0x%04x bplcon1=0x%04x bplcon2=0x%04x dpf=%d hScroll1=%d hScroll2=%d bpl1mod=%d bpl2mod=%d bytesPerRow=%d bitsRow=%d bplpt0=0x%08x planes=%d\n",
                    y,
                    agaActive,
                    (unsigned)bplcon0,
                    (unsigned)bplcon4,
                    (unsigned)bplcon1,
                    (unsigned)bplcon2,
                    dpf,
                    hScroll0,
                    hScrollPf2,
                    (int)bpl1mod,
                    (int)bpl2mod,
                    (int)bytesPerRow,
                    (int)fetchedWidthBits,
                    (unsigned)bplpt0,
                    planeCount);
            printedActiveLineDbg = 1;
        }
        /*
         * Palette population:
         *   single-PF: only 1<<planeCount entries are reachable.
         *   dual-PF:   PF1 uses palette[0..7], PF2 uses palette[8..15].
         */
        if (dpf) {
            buildPalette(cpu, palette, 16);
        } else {
            int paletteEntries = 1 << planeCount;
            if (paletteEntries > AMIGA_HOST_MAX_PALETTE) {
                paletteEntries = AMIGA_HOST_MAX_PALETTE;
            }
            buildPalette(cpu, palette, paletteEntries);
        }
        for (x = 0; x < planeCount; x++) {
            planeBase[x] = planeBasePointer(cpu, x);
        }
        fetchLayoutKind = 0;
        if (agaActive) {
            fetchLayoutKind = agaFetchLayoutKind(
                bplcon0, bpl1mod, planeBase, planeCount, dpf);
        }
        fetchLayoutKeyNow = fetchLayoutKey(
            bplcon0, bpl1mod, bpl2mod, fetchLayoutKind);
        forceReanchor = !fetchLayoutValid || fetchLayoutKeyNow != lastFetchLayoutKey;
        for (x = 0; x < planeCount; x++) {
            planeRowStride[x] = bytesPerRow;
            /*
             * BPL1MOD applies to odd-numbered planes (zero-indexed 0, 2, 4),
             * BPL2MOD to even-numbered planes (zero-indexed 1, 3, 5). This is
             * the same rule for single- and dual-playfield, so the modulo
             * split is mode-agnostic.
             */
            if ((x & 1) == 0) {
                planeRowStride[x] += (int)bpl1mod;
            } else {
                planeRowStride[x] += (int)bpl2mod;
            }
            if (forceReanchor ||
                !planeAnchorValid[x] ||
                planeBase[x] != planeLastRawBase[x] ||
                planeRowStride[x] != planeLastRowStride[x]) {
                planeAnchorValid[x] = 1;
                planeAnchorBase[x] = planeBase[x];
                planeAnchorY[x] = y;
            }
            planeLastRawBase[x] = planeBase[x];
            planeLastRowStride[x] = planeRowStride[x];
            planeLineBase[x] = (int64_t)planeAnchorBase[x] +
                               (int64_t)(y - planeAnchorY[x]) * (int64_t)planeRowStride[x];
        }
        lastFetchLayoutKey = fetchLayoutKeyNow;
        fetchLayoutValid = 1;
        if (fetchLayoutKind == 1) {
            int slices;
            slices = countConsecutiveInterleaved40PlaneSlices(planeBase);
            if (slices > planeCount) {
                planeCount = slices;
            }
            if (planeCount > 7) {
                planeCount = 7;
            }
            applyAgaSinglePfInterleaved40LineBases(
                y, planeAnchorBase, planeAnchorY, planeCount, planeLineBase);
            singlePfInterleaved40 = 1;
        } else if (fetchLayoutKind == 2) {
            applyAgaDualPfInterleaved160LineBases(
                y, planeAnchorBase, planeAnchorY, planeCount, planeLineBase);
        }

        if (!dpf || singlePfInterleaved40) {
            /*
             * Single-playfield compositor. Untouched from the original
             * implementation so DPF support is purely additive.
             */
            for (x = 0; x < host->width; x++) {
                int plane;
                int colorIdx = 0;
                int64_t pIdx;
                int byteOffset;
                int bitShift;
                pIdx = (int64_t)x + (int64_t)hScroll0 - (int64_t)ddfOffsetPixels;
                if (pIdx < 0 || pIdx >= (int64_t)fetchedWidthBits) {
                    host->frameBuffer[y * host->width + x] = color12ToArgb(color00);
                    if (host->playfieldIndexBuffer != NULL) {
                        host->playfieldIndexBuffer[y * host->width + x] = 0u;
                    }
                    continue;
                }
                byteOffset = (int)(pIdx / 8);
                bitShift = 7 - (int)(pIdx & 7);
                for (plane = 0; plane < planeCount; plane++) {
                    int64_t bitAddr;
                    bitAddr = planeLineBase[plane] + (int64_t)byteOffset;
                    int bit = readPlaneBitSigned(cpu, bitAddr, bitShift);
                    colorIdx |= (bit << plane);
                }
                if (agaActive) {
                    /*
                     * AGA single-playfield: BPLAM XORs the assembled
                     * index into the full 0..255 register space, so we
                     * cannot use the prebuilt palette[] that only has
                     * 1<<planeCount entries populated. Direct lookup
                     * into agaColorRegs covers every reachable slot
                     * (e.g. BPLAM=$80 maps index 0..127 into regs 128..255).
                     * Applied on every AGA single-playfield path, including
                     * interleaved-40 when BPLAM happens to be zero.
                     */
                    colorIdx = applyBplamIndex(cpu, colorIdx);
                    host->frameBuffer[y * host->width + x] =
                        lookupAgaPlayfieldColorDirect(cpu, colorIdx);
                } else {
                    host->frameBuffer[y * host->width + x] = palette[colorIdx];
                }
                if (host->playfieldIndexBuffer != NULL) {
                    host->playfieldIndexBuffer[y * host->width + x] = (uint8_t)colorIdx;
                }
            }
        } else {
            /*
             * Dual-playfield compositor (Pass A).
             *
             * PF1 := planes 0,2,4 (zero-indexed) -> 3-bit index 0..7,
             *        scrolled by PF1H, palette[0..7] (palette[0] = COLOR00).
             * PF2 := planes 1,3,5 (zero-indexed) -> 3-bit index 0..7,
             *        scrolled by PF2H, palette[8..15] with PF2 index 0
             *        meaning "transparent for PF2".
             *
             * BPLCON2 PF2PRI selects which playfield is in front. The
             * back playfield only shows where the front is transparent;
             * if both are transparent the pixel falls through to COLOR00.
             *
             * Sprite priority through playfieldIndexBuffer (Pass A): we
             * pack the two indices into one byte (low nibble = PF1, high
             * nibble = PF2). Existing sprite priority code only checks
             * "byte != 0", which still correctly means "some PF is
             * opaque here" in DPF mode. Pass B will unpack these for
             * per-PF sprite priority via BPLCON2[2:0]/[5:3].
             */
            int pf2Pri = (bplcon2 & 0x0040u) != 0;
            for (x = 0; x < host->width; x++) {
                int plane;
                int pf1Idx = 0;
                int pf2Idx = 0;
                int finalIdx;
                int64_t pIdxPf1;
                int64_t pIdxPf2;
                int pf1InRange;
                int pf2InRange;
                pIdxPf1 = (int64_t)x + (int64_t)hScroll0    - (int64_t)ddfOffsetPixels;
                pIdxPf2 = (int64_t)x + (int64_t)hScrollPf2 - (int64_t)ddfOffsetPixels;
                pf1InRange = (pIdxPf1 >= 0 && pIdxPf1 < (int64_t)fetchedWidthBits);
                pf2InRange = (pIdxPf2 >= 0 && pIdxPf2 < (int64_t)fetchedWidthBits);

                for (plane = 0; plane < planeCount; plane++) {
                    int isPf2 = (plane & 1);
                    int inRange = isPf2 ? pf2InRange : pf1InRange;
                    int64_t pIdx;
                    int byteOffset;
                    int bitShift;
                    int bit;
                    int pfBit;
                    int64_t bitAddr;
                    if (!inRange) {
                        continue;
                    }
                    pIdx = isPf2 ? pIdxPf2 : pIdxPf1;
                    byteOffset = (int)(pIdx / 8);
                    bitShift = 7 - (int)(pIdx & 7);
                    bitAddr = planeLineBase[plane] + (int64_t)byteOffset;
                    bit = readPlaneBitSigned(cpu, bitAddr, bitShift);
                    pfBit = plane / 2;
                    if (isPf2) {
                        pf2Idx |= (bit << pfBit);
                    } else {
                        pf1Idx |= (bit << pfBit);
                    }
                }

                if (pf2Pri) {
                    if (pf2Idx != 0) {
                        finalIdx = 8 + pf2Idx;
                    } else if (pf1Idx != 0) {
                        finalIdx = pf1Idx;
                    } else {
                        finalIdx = 0;
                    }
                } else {
                    if (pf1Idx != 0) {
                        finalIdx = pf1Idx;
                    } else if (pf2Idx != 0) {
                        finalIdx = 8 + pf2Idx;
                    } else {
                        finalIdx = 0;
                    }
                }

                if (host->playfieldIndexBuffer != NULL) {
                    host->playfieldIndexBuffer[y * host->width + x] =
                        (uint8_t)((pf2Idx << 4) | pf1Idx);
                }
                if (agaActive) {
                    host->frameBuffer[y * host->width + x] =
                        lookupAgaDualPfColor(cpu, pf1Idx, pf2Idx, pf2Pri);
                } else {
                    host->frameBuffer[y * host->width + x] = palette[finalIdx];
                }
            }
        }
        /*
         * Apply DIWSTRT/DIWSTOP clipping to the playfield. The border on
         * OCS is COLOR00; sprites are drawn afterwards and intentionally
         * remain visible across the border, matching real OCS behaviour.
         */
        {
            uint32_t bg;
            int xLeft;
            int xRight;
            bg = color12ToArgb(color00);
            if (!diwVisibleY) {
                xLeft = host->width;
                xRight = host->width;
            } else {
                xLeft = diwXLeft;
                xRight = diwXRight;
            }
            for (x = 0; x < xLeft; x++) {
                host->frameBuffer[y * host->width + x] = bg;
                if (host->playfieldIndexBuffer != NULL) {
                    host->playfieldIndexBuffer[y * host->width + x] = 0u;
                }
            }
            for (x = xRight; x < host->width; x++) {
                host->frameBuffer[y * host->width + x] = bg;
                if (host->playfieldIndexBuffer != NULL) {
                    host->playfieldIndexBuffer[y * host->width + x] = 0u;
                }
            }
        }
        drawSpritesOcsForScanline(
            host, cpu, y, amigaScanline, spritePairBeamLineCount, spritePairLastEvenPtr);
    }

    host->frameCounterPeakPlanes = (uint32_t)peakPlaneCount;
    host->frameCounterPeakBplcon0 = peakBplcon0;
    host->frameCounterPeakBplcon4 = peakBplcon4;
}

int amigaHostSdlInit(AmigaHostSdl* host, M68k* cpu, int width, int height, int scale) {
    char* title;
    int windowWidth;
    int windowHeight;

    memset(host, 0, sizeof(*host));
    host->cpu = cpu;
    host->width = width;
    host->height = height;
    host->running = 1;
    host->textureWidth = 0;
    host->textureHeight = 0;
    host->scaleMode = amigaHostScaleModeNormal;
    host->scaledFrameBuffer = NULL;
    host->scaledFrameWidth = 0;
    host->scaledFrameHeight = 0;
    host->lastScaleMs = 0.0;
    host->lastFrameMs = 0.0;
    host->showPerfHud = 0;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL: SDL_Init(VIDEO) failed: %s\n", SDL_GetError());
        return 0;
    }
    fprintf(stderr, "SDL: video driver: %s\n", SDL_GetCurrentVideoDriver());

#if defined(AMIGA_HOST_HAVE_SDL2_MIXER)
    hostSdlAudioSubsystemOpened = 0;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        hostSdlAudioSubsystemOpened = 1;
        amigaHostMusicInit();
    }
#endif

    hostSdlGameControllerSubsystemOpened = 0;
    hostSdlGameController = NULL;
    hostSdlGameControllerInstanceId = -1;
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0) {
        hostSdlGameControllerSubsystemOpened = 1;
        SDL_GameControllerEventState(SDL_ENABLE);
        hostSdlOpenFirstGameController();
    } else {
        fprintf(stderr,
                "controller: SDL_INIT_GAMECONTROLLER failed: %s\n",
                SDL_GetError());
    }


    title = "Clockwiser";
    windowWidth = width * scale;
    windowHeight = height * scale;

    host->window = SDL_CreateWindow(title,
                                    SDL_WINDOWPOS_CENTERED,
                                    SDL_WINDOWPOS_CENTERED,
                                    windowWidth,
                                    windowHeight,
                                    SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
    if (host->window == NULL) {
        fprintf(stderr, "SDL: SDL_CreateWindow failed: %s\n", SDL_GetError());
        amigaHostSdlShutdown(host);
        return 0;
    }

    /* Keep the upscale crisp: nearest-neighbour blit, no bilinear smoothing. */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

    host->renderer = SDL_CreateRenderer((SDL_Window*)host->window, -1, SDL_RENDERER_ACCELERATED);
    if (host->renderer == NULL) {
        fprintf(stderr, "SDL: accelerated renderer failed: %s\n", SDL_GetError());
        /* Older systems (e.g. IRIX OpenGL 1.x) may lack accelerated GL renderer */
        host->renderer = SDL_CreateRenderer((SDL_Window*)host->window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (host->renderer == NULL) {
        fprintf(stderr, "SDL: SDL_CreateRenderer failed: %s\n", SDL_GetError());
        amigaHostSdlShutdown(host);
        return 0;
    }
    {
        SDL_RendererInfo rendererInfo;

        if (SDL_GetRendererInfo((SDL_Renderer*)host->renderer, &rendererInfo) == 0) {
            fprintf(stderr, "SDL: renderer: %s\n", rendererInfo.name);
        }
    }

    host->texture = SDL_CreateTexture((SDL_Renderer*)host->renderer,
                                      SDL_PIXELFORMAT_ARGB8888,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      width,
                                      height);
    if (host->texture == NULL) {
        fprintf(stderr, "SDL: SDL_CreateTexture failed: %s\n", SDL_GetError());
        amigaHostSdlShutdown(host);
        return 0;
    }
    host->textureWidth = width;
    host->textureHeight = height;

    host->frameBuffer = (uint32_t*)malloc((size_t)width * (size_t)height * sizeof(uint32_t));
    if (host->frameBuffer == NULL) {
        fprintf(stderr, "SDL: out of memory for frame buffer\n");
        amigaHostSdlShutdown(host);
        return 0;
    }
    host->playfieldIndexBuffer = (uint8_t*)malloc((size_t)width * (size_t)height);
    if (host->playfieldIndexBuffer == NULL) {
        fprintf(stderr, "SDL: out of memory for playfield buffer\n");
        amigaHostSdlShutdown(host);
        return 0;
    }
    memset(host->playfieldIndexBuffer, 0, (size_t)width * (size_t)height);

    fprintf(stderr, "SDL host: press F12 to dump the active copper list to copper_dump_<frame>.txt\n");

    callHookHost = host;
    m68kSetHostFrameBoundaryHook(wheelOnVblank);
    return 1;
}

/*
 * Transpiled hook: CALL 'getMouseXY'
 * SDL window mouse position scaled to lo-res playfield (host->width x host->height,
 * typically 320x256). Result in emulated d0 (x) and d1 (y).
 */
void getMouseXY(void) {
    AmigaHostSdl* host;
    M68k* cpu;
    int mouseX;
    int mouseY;
    int gameX;
    int gameY;

    host = callHookHost;
    if (host == NULL || host->cpu == NULL || host->window == NULL) {
        return;
    }
    cpu = host->cpu;

    (void)SDL_GetMouseState(&mouseX, &mouseY);
    mapWindowMouseToGameCoords(host, mouseX, mouseY, 1, &gameX, &gameY);
    if (gameX < 0 || gameY < 0) {
        gameX = 0;
        gameY = 0;
    }

    cpu->d[0] = (uint32_t)gameX;
    cpu->d[1] = (uint32_t)gameY;
}

/*
 * Transpiled hook: CALL 'getPreference', d0
 * Preference 0: 0 = normal time, 1 = endless time.
 */
void getPreference(uint32_t prefNr) {
    AmigaHostSdl* host;
    M68k* cpu;

    host = callHookHost;
    if (host == NULL || host->cpu == NULL) {
        return;
    }
    cpu = host->cpu;
    if (prefNr == 0u) {
        cpu->d[0] = clockwiserStateGetTimeEndless() != 0 ? 1u : 0u;
    } else {
        cpu->d[0] = 0u;
    }
}

/*
 * Transpiled hook: CALL 'mouseWheel'
 * d0 = wheel for this vblank: 1 up, -1 down, 0 none (latched at hostFrameBoundary).
 */
void mouseWheel(void) {
    AmigaHostSdl* host;
    M68k* cpu;
    int32_t value;

    host = callHookHost;
    if (host == NULL || host->cpu == NULL) {
        return;
    }
    cpu = host->cpu;
    value = (int32_t)host->wheelVblank;
    cpu->d[0] = (uint32_t)value;
}

void amigaHostSdlEnableMouseJoy0(AmigaHostSdl* host) {
    if (host != NULL) {
        host->mouseJoy0Enabled = 1;
    }
}

static char* copperRegLabel(uint32_t regOff) {
    switch (regOff) {
        case 0x080u: return "COP1LCH/L";
        case 0x084u: return "COP2LCH/L";
        case 0x088u: return "COPJMP1";
        case 0x08Au: return "COPJMP2";
        case 0x08Eu: return "DIWSTRT";
        case 0x090u: return "DIWSTOP";
        case 0x092u: return "DDFSTRT";
        case 0x094u: return "DDFSTOP";
        case 0x100u: return "BPLCON0";
        case 0x102u: return "BPLCON1";
        case 0x104u: return "BPLCON2";
        case 0x106u: return "BPLCON3";
        case 0x108u: return "BPL1MOD";
        case 0x10Au: return "BPL2MOD";
        case 0x10Cu: return "BPLCON4";
        default: break;
    }
    if (regOff >= 0x0E0u && regOff <= 0x0FEu && (regOff & 2u) == 0u) {
        return "BPLxPTH/L";
    }
    if (regOff >= 0x180u && regOff <= 0x1BEu && (regOff & 2u) == 0u) {
        return "COLORxx";
    }
    return NULL;
}

static void writeCopperChipSnapshot(M68k* cpu, FILE* out) {
    int plane;
    int col;

    if (cpu == NULL || out == NULL) {
        return;
    }

    fprintf(out, "=== chip registers (custom stub) ===\n");
    fprintf(out,
            "COP1LC=0x%08x COP2LC=0x%08x copperListStartPc=0x%08x\n",
            (unsigned)readCustomLong(cpu, 0x080u),
            (unsigned)readCustomLong(cpu, 0x084u),
            (unsigned)copperListStartPc(cpu));
    fprintf(out,
            "BPLCON0=0x%04x BPLCON1=0x%04x BPLCON2=0x%04x BPLCON3=0x%04x BPLCON4=0x%04x\n",
            (unsigned)readCustomWord(cpu, 0x100u),
            (unsigned)readCustomWord(cpu, 0x102u),
            (unsigned)readCustomWord(cpu, 0x104u),
            (unsigned)readCustomWord(cpu, 0x106u),
            (unsigned)readCustomWord(cpu, 0x10Cu));
    fprintf(out,
            "BPL1MOD=%d BPL2MOD=%d DDFSTRT=0x%04x DDFSTOP=0x%04x\n",
            (int)(int16_t)readCustomWord(cpu, 0x108u),
            (int)(int16_t)readCustomWord(cpu, 0x10Au),
            (unsigned)readCustomWord(cpu, 0x092u),
            (unsigned)readCustomWord(cpu, 0x094u));
    fprintf(out, "DIWSTRT=0x%04x DIWSTOP=0x%04x\n",
            (unsigned)readCustomWord(cpu, 0x08Eu),
            (unsigned)readCustomWord(cpu, 0x090u));
    fprintf(out, "bitplane pointers:\n");
    for (plane = 0; plane < 8; plane++) {
        uint32_t pt;
        pt = planeBasePointer(cpu, plane);
        if (pt != 0u || plane < 6) {
            fprintf(out, "  BPL%dPT=0x%08x\n", plane + 1, (unsigned)pt);
        }
    }
    /*
     * Dump the full AGA palette window. Always print regs 0..31 (the
     * always-relevant low slots), and print any non-zero reg from 32..255
     * so we can diagnose per-line ping-pong banks (PF2 in DPF lands at
     * 32..47 or 64..79 in clockwiser's middle band) and high-bank palettes
     * (clockwiser's bottom strip uses BPLAM=$80 to land in regs 128..255).
     */
    fprintf(out, "agaColorRegs (24-bit, indices 0..255):\n");
    for (col = 0; col < 256; col++) {
        uint32_t argb;
        argb = m68kAgaColorArgb(cpu, col);
        if ((argb & 0x00FFFFFFu) != 0u || col < 32) {
            fprintf(out, "  [%3d]=0x%06x\n", col, (unsigned)(argb & 0x00FFFFFFu));
        }
    }
    fprintf(out, "\n");
}

static void dumpCopperListRange(M68k* cpu, FILE* out, uint32_t startPc, char* label, int maxOps) {
    int op;
    uint32_t pc;
    int done;

    if (cpu == NULL || out == NULL || startPc == 0u) {
        return;
    }

    fprintf(out, "--- copper list: %s (start 0x%08x) ---\n", label, (unsigned)startPc);
    pc = startPc;
    done = 0;
    for (op = 0; op < maxOps && !done; op++) {
        uint16_t ir1;
        uint16_t ir2;
        uint32_t regOff;
        char* regName;

        if (pc + 4u > cpu->memSize) {
            fprintf(out, "0x%08x: (past end of chip mem)\n", (unsigned)pc);
            break;
        }
        ir1 = read16(cpu, pc);
        ir2 = read16(cpu, pc + 2u);
        if (ir1 == 0xFFFFu && ir2 == 0xFFFEu) {
            fprintf(out, "0x%08x: ffff fffe  ; END\n", (unsigned)pc);
            done = 1;
        } else if ((ir1 & 1u) == 0u) {
            regOff = (uint32_t)(ir1 & 0x01FEu);
            regName = copperRegLabel(regOff);
            if (regName != NULL) {
                fprintf(out, "0x%08x: %04x %04x  ; MOVE %s = $%04x\n",
                        (unsigned)pc, (unsigned)ir1, (unsigned)ir2,
                        regName, (unsigned)ir2);
            } else {
                fprintf(out, "0x%08x: %04x %04x  ; MOVE $%03x = $%04x\n",
                        (unsigned)pc, (unsigned)ir1, (unsigned)ir2,
                        (unsigned)regOff, (unsigned)ir2);
            }
        } else {
            fprintf(out, "0x%08x: %04x %04x  ; WAIT v=%02x h=%02x\n",
                    (unsigned)pc, (unsigned)ir1, (unsigned)ir2,
                    (unsigned)((ir1 >> 8) & 0xFFu), (unsigned)(ir1 & 0xFEu));
        }
        pc += 4u;
    }
    if (!done) {
        fprintf(out, "; stopped after %d instructions (no END seen)\n", maxOps);
    }
    fprintf(out, "\n");
}

int amigaHostSdlDumpCopperToFile(AmigaHostSdl* host, char* path) {
    M68k* cpu;
    FILE* out;
    char defaultPath[64];
    uint32_t cop1lc;
    uint32_t cop2lc;
    uint32_t activePc;

    if (host == NULL || host->cpu == NULL) {
        return 0;
    }
    cpu = host->cpu;
    if (path == NULL || path[0] == '\0') {
        snprintf(defaultPath, sizeof(defaultPath), "copper_dump_%u.txt",
                 (unsigned)host->frameCounter);
        path = defaultPath;
    }
    out = fopen(path, "w");
    if (out == NULL) {
        fprintf(stderr, "copper dump: could not open %s for writing\n", path);
        return 0;
    }

    cop1lc = readCustomLong(cpu, 0x080u);
    cop2lc = readCustomLong(cpu, 0x084u);
    activePc = copperListStartPc(cpu);

    fprintf(out, "ClockAGA / SDL host copper dump\n");
    fprintf(out, "frame=%u\n", (unsigned)host->frameCounter);
    fprintf(out, "peak planes=%u peak BPLCON0=0x%04x peak BPLCON4=0x%04x\n\n",
            (unsigned)host->frameCounterPeakPlanes,
            (unsigned)host->frameCounterPeakBplcon0,
            (unsigned)host->frameCounterPeakBplcon4);

    writeCopperChipSnapshot(cpu, out);

    dumpCopperListRange(cpu, out, cop1lc, "COP1LC", 8192);
    if (cop2lc != 0u && cop2lc != cop1lc) {
        dumpCopperListRange(cpu, out, cop2lc, "COP2LC", 8192);
    }
    if (activePc != 0u && activePc != cop1lc && activePc != cop2lc) {
        dumpCopperListRange(cpu, out, activePc, "copperListStartPc", 8192);
    }

    fclose(out);
    fprintf(stderr, "copper dump: wrote %s (COP1LC=0x%08x COP2LC=0x%08x active=0x%08x)\n",
            path, (unsigned)cop1lc, (unsigned)cop2lc, (unsigned)activePc);
    return 1;
}

static void mapWindowMouseToFramebuffer(AmigaHostSdl* host, int winX, int winY,
                                        int* fbXOut, int* fbYOut) {
    mapWindowMouseToGameCoords(host, winX, winY, 0, fbXOut, fbYOut);
}

/*
 * ClockAGA (and similar games) read $bfec01 in int2 and store the byte in
 * toets. Character keys are looked up via NOT/ROR into the acodes table.
 */
static uint8_t amigaRawKeyFromAcodesIndex(uint8_t index) {
    uint8_t rolled = (uint8_t)((index << 1) | (index >> 7));
    return (uint8_t)(~rolled);
}

static int acodesIndexForLetter(char letter) {
    switch (letter) {
        case 'A': return 0x20;
        case 'B': return 0x35;
        case 'C': return 0x33;
        case 'D': return 0x22;
        case 'E': return 0x12;
        case 'F': return 0x23;
        case 'G': return 0x24;
        case 'H': return 0x25;
        case 'I': return 0x17;
        case 'J': return 0x26;
        case 'K': return 0x27;
        case 'L': return 0x28;
        case 'M': return 0x37;
        case 'N': return 0x36;
        case 'O': return 0x18;
        case 'P': return 0x19;
        case 'Q': return 0x10;
        case 'R': return 0x13;
        case 'S': return 0x21;
        case 'T': return 0x14;
        case 'U': return 0x16;
        case 'V': return 0x34;
        case 'W': return 0x11;
        case 'X': return 0x32;
        case 'Y': return 0x15;
        case 'Z': return 0x31;
        default: return -1;
    }
}

static int acodesIndexForDigit(char digit) {
    if (digit >= '1' && digit <= '9') {
        return digit - '0';
    }
    if (digit == '0') {
        return 0x0F;
    }
    return -1;
}

static int sdlKeyToAmigaRawCode(SDL_Keycode sym) {
    int index;

    if (sym >= SDLK_a && sym <= SDLK_z) {
        index = acodesIndexForLetter((char)('A' + (sym - SDLK_a)));
        if (index >= 0) {
            return (int)amigaRawKeyFromAcodesIndex((uint8_t)index);
        }
    }
    if (sym >= SDLK_0 && sym <= SDLK_9) {
        index = acodesIndexForDigit((char)sym);
        if (index >= 0) {
            return (int)amigaRawKeyFromAcodesIndex((uint8_t)index);
        }
    }

    switch (sym) {
        case SDLK_KP_0:
            return (int)amigaRawKeyFromAcodesIndex(0x0Fu);
        case SDLK_KP_1:
            return (int)amigaRawKeyFromAcodesIndex(0x01u);
        case SDLK_KP_2:
            return (int)amigaRawKeyFromAcodesIndex(0x02u);
        case SDLK_KP_3:
            return (int)amigaRawKeyFromAcodesIndex(0x03u);
        case SDLK_KP_4:
            return (int)amigaRawKeyFromAcodesIndex(0x04u);
        case SDLK_KP_5:
            return (int)amigaRawKeyFromAcodesIndex(0x05u);
        case SDLK_KP_6:
            return (int)amigaRawKeyFromAcodesIndex(0x06u);
        case SDLK_KP_7:
            return (int)amigaRawKeyFromAcodesIndex(0x07u);
        case SDLK_KP_8:
            return (int)amigaRawKeyFromAcodesIndex(0x08u);
        case SDLK_KP_9:
            return (int)amigaRawKeyFromAcodesIndex(0x09u);
        case SDLK_BACKSPACE:
            return 0x7D;
        case SDLK_RETURN:
            return 0x77;
        case SDLK_KP_ENTER:
            return 0x79;
        case SDLK_DELETE:
            return 0x73;
        case SDLK_LEFT:
            return 0x61;
        case SDLK_RIGHT:
            return 0x63;
        default:
            return -1;
    }
}

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
                            int* shellMouseFbYOut) {
    SDL_Event event;
    if (shellAdvanceLatchOut != NULL) {
        *shellAdvanceLatchOut = 0;
    }
    if (shellBackLatchOut != NULL) {
        *shellBackLatchOut = 0;
    }
    if (shellPauseMenuLatchOut != NULL) {
        *shellPauseMenuLatchOut = 0;
    }
    if (shellOtherKeyLatchOut != NULL) {
        *shellOtherKeyLatchOut = 0;
    }
    if (shellTextBackspaceOut != NULL) {
        *shellTextBackspaceOut = 0;
    }
    if (shellTextSubmitOut != NULL) {
        *shellTextSubmitOut = 0;
    }
    if (shellTextCharOut != NULL) {
        *shellTextCharOut = '\0';
    }
    if (shellMenuUpOut != NULL) {
        *shellMenuUpOut = 0;
    }
    if (shellMenuDownOut != NULL) {
        *shellMenuDownOut = 0;
    }
    if (shellMouseFbXOut != NULL) {
        *shellMouseFbXOut = -1;
    }
    if (shellMouseFbYOut != NULL) {
        *shellMouseFbYOut = -1;
    }

    if (textEntryMode && !hostSdlTextEntryWasActive) {
        hostSdlTextResetForNewSession();
    }
    hostSdlTextEntryWasActive = textEntryMode;

    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            host->running = 0;
        }
        if (event.type == SDL_KEYDOWN) {
            if (event.key.repeat == 0 && event.key.keysym.sym == SDLK_F12) {
                amigaHostSdlDumpCopperToFile(host, NULL);
            }
            if (event.key.repeat == 0 && event.key.keysym.sym == SDLK_F1) {
                host->showPerfHud = host->showPerfHud == 0 ? 1 : 0;
            }
            if (host->cpu != NULL && event.key.repeat == 0
                && event.key.keysym.sym != SDLK_TAB) {
                int rawCode;

                rawCode = sdlKeyToAmigaRawCode(event.key.keysym.sym);
                if (rawCode >= 0) {
                    m68kHostStageKeyboardInterrupt(host->cpu, (uint8_t)rawCode);
                    m68kHostPollPendingKeyboardInterrupt(host->cpu);
                }
            }
            if (shellAdvanceLatchOut != NULL
                && (event.key.keysym.sym == SDLK_SPACE || event.key.keysym.sym == SDLK_RETURN
                    || event.key.keysym.sym == SDLK_KP_ENTER)
                && !textEntryMode) {
                *shellAdvanceLatchOut = 1;
            }
            if (shellBackLatchOut != NULL
                && event.key.keysym.sym == SDLK_ESCAPE) {
                *shellBackLatchOut = 1;
            }
            if (shellPauseMenuLatchOut != NULL
                && event.key.keysym.sym == SDLK_TAB) {
                *shellPauseMenuLatchOut = 1;
            }
            if (shellMenuUpOut != NULL && event.key.keysym.sym == SDLK_UP) {
                *shellMenuUpOut = 1;
            }
            if (shellMenuDownOut != NULL && event.key.keysym.sym == SDLK_DOWN) {
                *shellMenuDownOut = 1;
            }
            if (textEntryMode) {
                if (shellTextSubmitOut != NULL
                    && (event.key.keysym.sym == SDLK_RETURN || event.key.keysym.sym == SDLK_KP_ENTER)) {
                    *shellTextSubmitOut = 1;
                } else if (shellTextBackspaceOut != NULL
                           && (event.key.keysym.sym == SDLK_BACKSPACE
                               || event.key.keysym.sym == SDLK_DELETE)) {
                    *shellTextBackspaceOut = 1;
                    if (hostSdlTextCommitCount > 0) {
                        hostSdlTextCommitCount--;
                    }
                } else if (shellTextCharOut != NULL) {
                    SDL_Keycode sym;
                    char emittedChar;

                    sym = event.key.keysym.sym;
                    emittedChar = '\0';
                    if (sym >= SDLK_a && sym <= SDLK_z) {
                        emittedChar = (char)('A' + (int)(sym - SDLK_a));
                    } else if (sym >= SDLK_0 && sym <= SDLK_9) {
                        emittedChar = (char)('0' + (int)(sym - SDLK_0));
                    } else if (sym == SDLK_SPACE) {
                        emittedChar = ' ';
                    } else if (sym == SDLK_MINUS) {
                        emittedChar = '-';
                    }
                    if (emittedChar != '\0' && hostSdlTextCommitCount < hostSdlTextNameMaxLen) {
                        *shellTextCharOut = emittedChar;
                        hostSdlTextCommitCount++;
                    }
                }
            }
            if (shellOtherKeyLatchOut != NULL
                && event.key.keysym.sym != SDLK_SPACE
                && event.key.keysym.sym != SDLK_RETURN
                && event.key.keysym.sym != SDLK_KP_ENTER
                && event.key.keysym.sym != SDLK_ESCAPE
                && event.key.keysym.sym != SDLK_TAB
                && event.key.keysym.sym != SDLK_UP
                && event.key.keysym.sym != SDLK_DOWN
                && event.key.keysym.sym != SDLK_BACKSPACE
                && event.key.keysym.sym != SDLK_DELETE) {
                *shellOtherKeyLatchOut = 1;
            }
        }
        if (event.type == SDL_MOUSEBUTTONUP) {
            if (shellAdvanceLatchOut != NULL && event.button.button == SDL_BUTTON_LEFT) {
                *shellAdvanceLatchOut = 1;
                if (shellMouseFbXOut != NULL && shellMouseFbYOut != NULL) {
                    mapWindowMouseToFramebuffer(host,
                                                (int)event.button.x,
                                                (int)event.button.y,
                                                shellMouseFbXOut,
                                                shellMouseFbYOut);
                }
            }
            if (shellBackLatchOut != NULL && event.button.button == SDL_BUTTON_RIGHT) {
                *shellBackLatchOut = 1;
                if (shellMouseFbXOut != NULL && shellMouseFbYOut != NULL) {
                    mapWindowMouseToFramebuffer(host,
                                                (int)event.button.x,
                                                (int)event.button.y,
                                                shellMouseFbXOut,
                                                shellMouseFbYOut);
                }
            }
        }
        if (event.type == SDL_CONTROLLERDEVICEADDED) {
            /*
             * event.cdevice.which on ADDED is a joystick *device index* into
             * SDL_NumJoysticks(), not an instance id. We just open the first
             * available controller on demand.
             */
            if (hostSdlGameController == NULL) {
                hostSdlOpenFirstGameController();
            }
        }
        if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
            if (hostSdlGameController != NULL
                && event.cdevice.which == hostSdlGameControllerInstanceId) {
                fprintf(stderr,
                        "controller: removed instance %d\n",
                        (int)hostSdlGameControllerInstanceId);
                hostSdlCloseGameController();
                hostSdlOpenFirstGameController();
            }
        }
        if (event.type == SDL_CONTROLLERBUTTONDOWN) {
            int btn;
            btn = (int)event.cbutton.button;

            if (textEntryMode) {
                /*
                 * Controller text entry overrides the normal latch mappings:
                 *   D-pad Up/Down  -> cycle candidate character
                 *   A              -> commit candidate as next char; auto-submit on the last slot
                 *   B              -> backspace
                 *   Start          -> still acts as cancel (back to menu)
                 */
                if (btn == SDL_CONTROLLER_BUTTON_DPAD_UP) {
                    hostSdlTextCycleCandidate(+1);
                } else if (btn == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
                    hostSdlTextCycleCandidate(-1);
                } else if (btn == SDL_CONTROLLER_BUTTON_A) {
                    if (shellTextCharOut != NULL
                        && hostSdlTextCommitCount < hostSdlTextNameMaxLen) {
                        *shellTextCharOut = hostSdlTextCandidate;
                        hostSdlTextCommitCount++;
                        if (hostSdlTextCommitCount >= hostSdlTextNameMaxLen
                            && shellTextSubmitOut != NULL) {
                            *shellTextSubmitOut = 1;
                        }
                    }
                } else if (btn == SDL_CONTROLLER_BUTTON_B) {
                    if (shellTextBackspaceOut != NULL) {
                        *shellTextBackspaceOut = 1;
                    }
                    if (hostSdlTextCommitCount > 0) {
                        hostSdlTextCommitCount--;
                    }
                } else if (btn == SDL_CONTROLLER_BUTTON_START) {
                    if (shellBackLatchOut != NULL) {
                        *shellBackLatchOut = 1;
                    }
                }
            } else {
                if (shellAdvanceLatchOut != NULL
                    && btn == SDL_CONTROLLER_BUTTON_A) {
                    *shellAdvanceLatchOut = 1;
                }
                if (shellBackLatchOut != NULL
                    && (btn == SDL_CONTROLLER_BUTTON_B
                        || btn == SDL_CONTROLLER_BUTTON_START)) {
                    *shellBackLatchOut = 1;
                }
                if (shellMenuUpOut != NULL && btn == SDL_CONTROLLER_BUTTON_DPAD_UP) {
                    *shellMenuUpOut = 1;
                }
                if (shellMenuDownOut != NULL && btn == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
                    *shellMenuDownOut = 1;
                }
                if (shellOtherKeyLatchOut != NULL
                    && btn != SDL_CONTROLLER_BUTTON_A
                    && btn != SDL_CONTROLLER_BUTTON_B
                    && btn != SDL_CONTROLLER_BUTTON_START
                    && btn != SDL_CONTROLLER_BUTTON_DPAD_UP
                    && btn != SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
                    *shellOtherKeyLatchOut = 1;
                }
            }
        }
        if (event.type == SDL_MOUSEWHEEL) {
            if (event.wheel.y > 0) {
                wheelEnqueue(host, 1);
            } else if (event.wheel.y < 0) {
                wheelEnqueue(host, -1);
            }
        }
    }
    updateCiaInput(host);
}

void amigaHostSdlPresentFrameEx(AmigaHostSdl* host,
                                AmigaHostSdlFrameOverlayFn overlayFn,
                                void* overlayUserData) {
    int y;
    int pitch;
    uint8_t* pixels;
    SDL_Rect dstRect;
    int dstX;
    int dstY;
    int dstW;
    int dstH;
    uint32_t* presentedFrame;
    int presentedW;
    int presentedH;

    if (host->texture == NULL || host->frameBuffer == NULL) {
        return;
    }

    renderBitplanesLowres(host);

    if (overlayFn != NULL) {
        overlayFn(host, overlayUserData);
    }

    if (host->cpu != NULL && (host->frameCounter % 120u) == 0u) {
        uint16_t bplcon0;
        uint16_t bplcon4;
        uint16_t color00;
        int16_t bpl1mod;
        int16_t bpl2mod;
        uint16_t ddfstrt;
        uint16_t ddfstop;
        int bytesPerRow;
        uint32_t cop1lc;
        uint32_t bplpt0;
        uint16_t cop1w0;
        uint16_t cop1w1;
        cop1lc = readCustomLong(host->cpu, 0x00000080u);
        bplcon0 = readCustomWord(host->cpu, 0x00000100u);
        bplcon4 = readCustomWord(host->cpu, 0x0000010Cu);
        color00 = m68kAgaColor12(host->cpu, 0);
        bpl1mod = (int16_t)readCustomWord(host->cpu, 0x00000108u);
        bpl2mod = (int16_t)readCustomWord(host->cpu, 0x0000010Au);
        ddfstrt = readCustomWord(host->cpu, 0x00000092u);
        ddfstop = readCustomWord(host->cpu, 0x00000094u);
        bytesPerRow = chooseBitplaneBytesPerRow(host->cpu, host->width);
        bplpt0 = planeBasePointer(host->cpu, 0);
        cop1w0 = (cop1lc + 2u <= host->cpu->memSize) ? read16(host->cpu, cop1lc) : 0u;
        cop1w1 = (cop1lc + 4u <= host->cpu->memSize) ? read16(host->cpu, cop1lc + 2u) : 0u;
        fprintf(stderr,
            "video dbg frame=%u end bplcon0=0x%04x bplcon4=0x%04x planes=%d "
            "peak planes=%u peak bplcon0=0x%04x peak bplcon4=0x%04x "
            "bplpt0=0x%08x color00=0x%04x bpl1mod=%d bpl2mod=%d "
            "ddf=%04x..%04x bytesPerRow=%d cop1lc=0x%08x [%04x %04x]\n",
            (unsigned)host->frameCounter,
            (unsigned)bplcon0,
            (unsigned)bplcon4,
            bitplaneCountFromChipRegs(host->cpu),
            (unsigned)host->frameCounterPeakPlanes,
            (unsigned)host->frameCounterPeakBplcon0,
            (unsigned)host->frameCounterPeakBplcon4,
            (unsigned)bplpt0,
            (unsigned)color00,
            (int)bpl1mod,
            (int)bpl2mod,
            (unsigned)ddfstrt,
            (unsigned)ddfstop,
            (int)bytesPerRow,
            (unsigned)cop1lc,
            (unsigned)cop1w0,
            (unsigned)cop1w1);
    }

    choosePresentedFrame(host, &presentedFrame, &presentedW, &presentedH);
    if (!ensurePresentationTexture(host, presentedW, presentedH)) {
        return;
    }
    if (SDL_LockTexture((SDL_Texture*)host->texture, NULL, (void**)&pixels, &pitch) == 0) {
        for (y = 0; y < presentedH; y++) {
            memcpy(pixels + (size_t)y * (size_t)pitch,
                   presentedFrame + (size_t)y * (presentedW),
                   (size_t)presentedW * sizeof(uint32_t));
        }
        SDL_UnlockTexture((SDL_Texture*)host->texture);
    }

    computePresentationRect(host, presentedW, presentedH, &dstX, &dstY, &dstW, &dstH);
    dstRect.x = dstX;
    dstRect.y = dstY;
    dstRect.w = dstW;
    dstRect.h = dstH;
    SDL_SetRenderDrawColor((SDL_Renderer*)host->renderer, 0, 0, 0, 255);
    SDL_RenderClear((SDL_Renderer*)host->renderer);
    SDL_RenderCopy((SDL_Renderer*)host->renderer, (SDL_Texture*)host->texture, NULL, &dstRect);
    SDL_RenderPresent((SDL_Renderer*)host->renderer);

    if (host->cpu != NULL) {
        m68kAmigaFrameTick(host->cpu);
    }
    host->frameCounter++;
}

void amigaHostSdlPresentFrame(AmigaHostSdl* host) {
    amigaHostSdlPresentFrameEx(host, NULL, NULL);
}

void amigaHostSdlSetScaleMode(AmigaHostSdl* host, int scaleMode) {
    assert(host);
    if (scaleMode < amigaHostScaleModeNormal || scaleMode > amigaHostScaleModeXbrz) {
        scaleMode = amigaHostScaleModeNormal;
    }
    host->scaleMode = scaleMode;
}

int amigaHostSdlGetScaleMode(AmigaHostSdl* host) {
    assert(host);
    return host->scaleMode;
}

void amigaHostSdlPresentFramebufferOnly(AmigaHostSdl* host) {
    int y;
    int pitch;
    uint8_t* pixels;
    SDL_Rect dstRect;
    int dstX;
    int dstY;
    int dstW;
    int dstH;
    uint32_t* presentedFrame;
    int presentedW;
    int presentedH;

    assert(host->frameBuffer);

    choosePresentedFrame(host, &presentedFrame, &presentedW, &presentedH);
    if (!ensurePresentationTexture(host, presentedW, presentedH)) {
        return;
    }
    if (SDL_LockTexture((SDL_Texture*)host->texture, NULL, (void**)&pixels, &pitch) == 0) {
        for (y = 0; y < presentedH; y++) {
            memcpy(pixels + (size_t)y * (size_t)pitch,
                   presentedFrame + (size_t)y * (size_t)presentedW,
                   (size_t)presentedW * sizeof(uint32_t));
        }
        SDL_UnlockTexture((SDL_Texture*)host->texture);
    }

    computePresentationRect(host, presentedW, presentedH, &dstX, &dstY, &dstW, &dstH);
    dstRect.x = dstX;
    dstRect.y = dstY;
    dstRect.w = dstW;
    dstRect.h = dstH;
    SDL_SetRenderDrawColor((SDL_Renderer*)host->renderer, 0, 0, 0, 255);
    SDL_RenderClear((SDL_Renderer*)host->renderer);
    SDL_RenderCopy((SDL_Renderer*)host->renderer, (SDL_Texture*)host->texture, NULL, &dstRect);
    SDL_RenderPresent((SDL_Renderer*)host->renderer);
}

void amigaHostSdlShutdown(AmigaHostSdl* host) {
    if (callHookHost == host) {
        callHookHost = NULL;
    }
    hostSdlCloseGameController();
    if (hostSdlGameControllerSubsystemOpened) {
        SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
        hostSdlGameControllerSubsystemOpened = 0;
    }
    amigaHostMusicShutdown();
#if defined(AMIGA_HOST_HAVE_SDL2_MIXER)
    if (hostSdlAudioSubsystemOpened) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        hostSdlAudioSubsystemOpened = 0;
    }
#endif
    if (host->frameBuffer != NULL) {
        free(host->frameBuffer);
        host->frameBuffer = NULL;
    }
    if (host->scaledFrameBuffer != NULL) {
        free(host->scaledFrameBuffer);
        host->scaledFrameBuffer = NULL;
        host->scaledFrameWidth = 0;
        host->scaledFrameHeight = 0;
    }
    if (host->playfieldIndexBuffer != NULL) {
        free(host->playfieldIndexBuffer);
        host->playfieldIndexBuffer = NULL;
    }
    if (host->texture != NULL) {
        SDL_DestroyTexture((SDL_Texture*)host->texture);
        host->texture = NULL;
    }
    if (host->renderer != NULL) {
        SDL_DestroyRenderer((SDL_Renderer*)host->renderer);
        host->renderer = NULL;
    }
    if (host->window != NULL) {
        SDL_DestroyWindow((SDL_Window*)host->window);
        host->window = NULL;
    }
    SDL_Quit();
}
