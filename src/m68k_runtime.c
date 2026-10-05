#include "m68k_runtime.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static uint32_t kCustomBase = 0x00DFF000u;
static uint32_t kCustomSize = 0x00001000u;
static uint32_t kCiaBase = 0x00BF0000u;
static uint32_t kCiaSize = 0x00010000u;
static uint32_t kBeamPosOffset = 0x00000006u;
static uint32_t kDmaconrOffset = 0x00000002u;
static uint32_t kBltcon0Offset = 0x00000040u;
static uint32_t kBltcon1Offset = 0x00000042u;
static uint32_t kBltafwmOffset = 0x00000044u;
static uint32_t kBltalwmOffset = 0x00000046u;
static uint32_t kBltcpthOffset = 0x00000048u;
static uint32_t kBltbpthOffset = 0x0000004Cu;
static uint32_t kBltapthOffset = 0x00000050u;
static uint32_t kBltdpthOffset = 0x00000054u;
static uint32_t kBltsizeOffset = 0x00000058u;
static uint32_t kCiaADataOffset = 0xEC01u;  /* $bfec01 keyboard data */
static uint32_t kCiaAIcrOffset = 0xED01u;  /* $bfed01 interrupt control */
static uint32_t kBltcmodOffset = 0x00000060u;
static uint32_t kBltbmodOffset = 0x00000062u;
static uint32_t kBltamodOffset = 0x00000064u;
static uint32_t kBltdmodOffset = 0x00000066u;
static uint32_t kBltbdatOffset = 0x00000072u;
static uint32_t kBltadatOffset = 0x00000074u;

static void memoryTrap(M68k* cpu,
                       uint32_t addr,
                       int accessSize,
                       int isWrite) {
    cpu->faulted = 1;
    cpu->faultAddr = addr;
    cpu->faultIsWrite = isWrite ? 1 : 0;
    cpu->faultAccessSize = (uint8_t)accessSize;

    fprintf(stderr,
            "m68k memory trap: %s%d at 0x%08x (RAM size 0x%08x)\n",
            isWrite ? "write" : "read",
            accessSize,
            addr,
            cpu->memSize);
    if (cpu->lastAsmLine != 0u) {
        fprintf(stderr, "  last asm line: %u\n", cpu->lastAsmLine);
    }
    if (cpu->lastAsmText != NULL && cpu->lastAsmText[0] != '\0') {
        fprintf(stderr, "  last asm text: %s\n", cpu->lastAsmText);
    }
    if (cpu->lastLabelName != NULL && cpu->lastLabelName[0] != '\0') {
        fprintf(stderr, "  last label: %s (0x%08x)\n",
                cpu->lastLabelName,
                cpu->lastLabelPc);
    }
    dumpCpuState(cpu);
    abort();
}

static int fitsRam(M68k* cpu, uint32_t addr, int accessSize) {
    uint32_t endOffset = (uint32_t)(accessSize - 1);
    if (addr > 0xFFFFFFFFu - endOffset) return 0;
    if (addr >= cpu->memSize) return 0;
    if (addr + endOffset >= cpu->memSize) return 0;
    return 1;
}

static int fitsCustom(uint32_t addr, int accessSize) {
    uint32_t endOffset = (uint32_t)(accessSize - 1);
    uint32_t customEnd = kCustomBase + kCustomSize;
    if (addr < kCustomBase) return 0;
    if (addr > 0xFFFFFFFFu - endOffset) return 0;
    if (addr + endOffset >= customEnd) return 0;
    return 1;
}

static int fitsCia(uint32_t addr, int accessSize) {
    uint32_t endOffset = (uint32_t)(accessSize - 1);
    uint32_t ciaEnd = kCiaBase + kCiaSize;
    if (addr < kCiaBase) return 0;
    if (addr > 0xFFFFFFFFu - endOffset) return 0;
    if (addr + endOffset >= ciaEnd) return 0;
    return 1;
}

static uint8_t readCustomByte(M68k* cpu, uint32_t off) {
    if (off == kBeamPosOffset) {
        uint8_t beamPos = (uint8_t)(cpu->beamFramePhase + cpu->beamReadCounter);
        cpu->beamReadCounter++;
        return beamPos;
    }
    return cpu->customRegs[off];
}

static void writeCustomByte(M68k* cpu, uint32_t off, uint8_t value) {
    if (off == kBeamPosOffset) {
        return;
    }
    cpu->customRegs[off] = value;
}

static uint16_t readCustomWordRaw(M68k* cpu, uint32_t off) {
    if (off == 0x001au) {
        return (uint16_t)(cpu->intreqPending & 0x7FFFu);
    }
    if (off == 0x001cu) {
        return (uint16_t)(cpu->intenaEnabled & 0x7FFFu);
    }
    uint16_t hi = cpu->customRegs[off];
    uint16_t lo = cpu->customRegs[off + 1u];
    return (uint16_t)((hi << 8) | lo);
}

static uint32_t readCustomLongRaw(M68k* cpu, uint32_t off) {
    uint32_t hi = readCustomWordRaw(cpu, off);
    uint32_t lo = readCustomWordRaw(cpu, off + 2u);
    return (hi << 16) | lo;
}

static void writeCustomWordRaw(M68k* cpu, uint32_t off, uint16_t value) {
    cpu->customRegs[off] = (uint8_t)((value >> 8) & 0xFFu);
    cpu->customRegs[off + 1u] = (uint8_t)(value & 0xFFu);
}

static int isCustomColorRegOffset(uint32_t off) {
    if (off < 0x180u || off > 0x1beu) {
        return 0;
    }
    if ((off & 1u) != 0u) {
        return 0;
    }
    return 1;
}

static void customColorWrite(M68k* cpu, uint32_t off, uint16_t value) {
    uint16_t bplcon3;
    int num;
    int colreg;
    int r;
    int g;
    int b;
    int cr;
    int cg;
    int cb;
    uint16_t rgb12;

    bplcon3 = readCustomWordRaw(cpu, 0x106u);
    num = (int)((off & 0x3Eu) / 2u);
    colreg = ((bplcon3 >> 13) & 7) * 32 + num;
    r = (int)((value & 0xF00u) >> 8);
    g = (int)((value & 0xF0u) >> 4);
    b = (int)(value & 0xFu);
    cr = 0;
    cg = 0;
    cb = 0;
    if (colreg >= 0 && colreg < 256) {
        uint32_t old;
        old = cpu->agaColorRegs[colreg];
        cr = (int)((old >> 16) & 0xFFu);
        cg = (int)((old >> 8) & 0xFFu);
        cb = (int)(old & 0xFFu);
        if ((bplcon3 & 0x200u) != 0u) {
            /* LOCT=1: merge into low four bits of each 8-bit gun. */
            cr = (cr & 0xF0) | r;
            cg = (cg & 0xF0) | g;
            cb = (cb & 0xF0) | b;
        } else {
            /* LOCT=0: merge into high four bits; keep existing low nibbles. */
            cr = (cr & 0x0F) | (r << 4);
            cg = (cg & 0x0F) | (g << 4);
            cb = (cb & 0x0F) | (b << 4);
        }
        cpu->agaColorRegs[colreg] =
            ((uint32_t)cr << 16) | ((uint32_t)cg << 8) | (uint32_t)cb;
    }
    rgb12 = (uint16_t)(((cr >> 4) << 8) | ((cg >> 4) << 4) | (cb >> 4));
    writeCustomWordRaw(cpu, off, rgb12);
}

uint16_t m68kAgaColor12(M68k* cpu, int index) {
    int cr;
    int cg;
    int cb;
    uint32_t packed;

    if (cpu == NULL || index < 0 || index >= 256) {
        return 0u;
    }
    packed = cpu->agaColorRegs[index];
    cr = (int)((packed >> 16) & 0xFFu);
    cg = (int)((packed >> 8) & 0xFFu);
    cb = (int)(packed & 0xFFu);
    return (uint16_t)(((cr >> 4) << 8) | ((cg >> 4) << 4) | (cb >> 4));
}

uint32_t m68kAgaColorArgb(M68k* cpu, int index) {
    uint32_t packed;

    if (cpu == NULL || index < 0 || index >= 256) {
        return 0xFF000000u;
    }
    packed = cpu->agaColorRegs[index];
    return 0xFF000000u | (packed & 0x00FFFFFFu);
}

static void writeCustomIntena(M68k* cpu, uint16_t value) {
    uint16_t mask = (uint16_t)(value & 0x7FFFu);
    if ((value & 0x8000u) != 0u) {
        cpu->intenaEnabled = (uint16_t)(cpu->intenaEnabled | mask);
    } else {
        cpu->intenaEnabled = (uint16_t)(cpu->intenaEnabled & (uint16_t)~mask);
    }
}

static void writeCustomIntreq(M68k* cpu, uint16_t value) {
    uint16_t mask = (uint16_t)(value & 0x7FFFu);
    if ((value & 0x8000u) != 0u) {
        cpu->intreqPending = (uint16_t)(cpu->intreqPending | mask);
    } else {
        cpu->intreqPending = (uint16_t)(cpu->intreqPending & (uint16_t)~mask);
    }
}

static uint16_t m68kBuildInterruptSr(M68k* cpu) {
    uint16_t sr = 0x0200u;
    sr |= (uint16_t)(cpu->c & 1u);
    sr |= (uint16_t)((cpu->v & 1u) << 1);
    sr |= (uint16_t)((cpu->z & 1u) << 2);
    sr |= (uint16_t)((cpu->n & 1u) << 3);
    sr |= (uint16_t)((cpu->x & 1u) << 4);
    return sr;
}

static int m68kTryDispatchInterrupt(M68k* cpu,
                                    uint32_t vectorAddr,
                                    uint16_t intBit,
                                    uint32_t resumePc) {
    uint32_t handler;
    if ((cpu->intenaEnabled & intBit) == 0u) {
        return 0;
    }
    if ((cpu->intreqPending & intBit) == 0u) {
        return 0;
    }
    handler = read32(cpu, vectorAddr);
    if (handler == 0u) {
        return 0;
    }
    push32(cpu, resumePc);
    push16(cpu, m68kBuildInterruptSr(cpu));
    cpu->pcVirtual = handler;
    return 1;
}

static void setBlitterBusy(M68k* cpu, int busy) {
    uint8_t dmaconr = cpu->customRegs[kDmaconrOffset];
    if (busy) {
        dmaconr |= 0x40u;
    } else {
        dmaconr &= (uint8_t)~0x40u;
    }
    cpu->customRegs[kDmaconrOffset] = dmaconr;
}

static uint16_t readWordSafe(M68k* cpu, uint32_t addr) {
    if (!fitsRam(cpu, addr, 2)) {
        return 0u;
    }
    return (uint16_t)((((uint16_t)cpu->mem[addr]) << 8) | (uint16_t)cpu->mem[addr + 1u]);
}

static void writeWordSafe(M68k* cpu, uint32_t addr, uint16_t value) {
    if (!fitsRam(cpu, addr, 2)) {
        return;
    }
    cpu->mem[addr] = (uint8_t)((value >> 8) & 0xFFu);
    cpu->mem[addr + 1u] = (uint8_t)(value & 0xFFu);
}

static uint16_t applyMinterm(uint16_t a, uint16_t b, uint16_t c, uint8_t minterm) {
    uint16_t out = 0u;
    uint16_t term;
    if (minterm & 0x01u) {
        term = (uint16_t)(~a & ~b & ~c);
        out |= term;
    }
    if (minterm & 0x02u) {
        term = (uint16_t)(~a & ~b & c);
        out |= term;
    }
    if (minterm & 0x04u) {
        term = (uint16_t)(~a & b & ~c);
        out |= term;
    }
    if (minterm & 0x08u) {
        term = (uint16_t)(~a & b & c);
        out |= term;
    }
    if (minterm & 0x10u) {
        term = (uint16_t)(a & ~b & ~c);
        out |= term;
    }
    if (minterm & 0x20u) {
        term = (uint16_t)(a & ~b & c);
        out |= term;
    }
    if (minterm & 0x40u) {
        term = (uint16_t)(a & b & ~c);
        out |= term;
    }
    if (minterm & 0x80u) {
        term = (uint16_t)(a & b & c);
        out |= term;
    }
    return out;
}

static void blitLineStepX(uint32_t* ptrInOut, int16_t* bitIndexInOut, int xDir) {
    int16_t bitIndex = *bitIndexInOut;
    uint32_t ptr = *ptrInOut;
    if (xDir > 0) {
        bitIndex--;
        if (bitIndex < 0) {
            bitIndex = 15;
            ptr += 2u;
        }
    } else {
        bitIndex++;
        if (bitIndex > 15) {
            bitIndex = 0;
            ptr -= 2u;
        }
    }
    *bitIndexInOut = bitIndex;
    *ptrInOut = ptr;
}

static void blitLineStepY(uint32_t* ptrInOut, int yDir, int16_t rowStepBytes) {
    uint32_t ptr = *ptrInOut;
    if (yDir > 0) {
        ptr += (uint32_t)(int32_t)rowStepBytes;
    } else {
        ptr -= (uint32_t)(int32_t)rowStepBytes;
    }
    *ptrInOut = ptr;
}

static void runBlitterLineMode(M68k* cpu,
                               uint16_t bltcon0,
                               uint16_t bltcon1,
                               uint16_t bltbdat,
                               uint16_t bltsize,
                               uint32_t bltapt,
                               uint32_t bltdpt,
                               int16_t bltbmod,
                               int16_t bltamod,
                               int16_t bltdmod) {
    int octant;
    int xMajor;
    int xDir;
    int yDir;
    int pixels;
    int i;
    int32_t errorTerm;
    int16_t bitIndex;
    uint32_t dstPtr;
    uint16_t pattern;

    octant = (int)((bltcon1 >> 2) & 0x7u);
    xMajor = octant >= 4 ? 1 : 0;
    xDir = ((octant == 2) || (octant == 3) || (octant == 5) || (octant == 7)) ? -1 : 1;
    yDir = ((octant == 1) || (octant == 3) || (octant == 6) || (octant == 7)) ? -1 : 1;

    pixels = (int)((bltsize >> 6) & 0x03FFu);
    if (pixels <= 0) {
        pixels = 1024;
    }

    errorTerm = (int32_t)(int16_t)(bltapt & 0xFFFFu);
    bitIndex = (int16_t)(15 - (int16_t)((bltcon0 >> 12) & 0x0Fu));
    dstPtr = bltdpt & 0xFFFFFFFEu;
    pattern = bltbdat;

    for (i = 0; i < pixels; i++) {
        uint16_t dstWord = readWordSafe(cpu, dstPtr);
        uint16_t bitMask = (uint16_t)(1u << bitIndex);
        int textureBit = (int)((pattern >> (15 - (i & 15))) & 1u);
        if (textureBit != 0) {
            dstWord |= bitMask;
        } else {
            dstWord &= (uint16_t)~bitMask;
        }
        writeWordSafe(cpu, dstPtr, dstWord);

        if (errorTerm >= 0) {
            errorTerm += (int32_t)bltamod;
            if (xMajor) {
                blitLineStepX(&dstPtr, &bitIndex, xDir);
                blitLineStepY(&dstPtr, yDir, bltdmod);
            } else {
                blitLineStepY(&dstPtr, yDir, bltdmod);
                blitLineStepX(&dstPtr, &bitIndex, xDir);
            }
        } else {
            errorTerm += (int32_t)bltbmod;
            if (xMajor) {
                blitLineStepX(&dstPtr, &bitIndex, xDir);
            } else {
                blitLineStepY(&dstPtr, yDir, bltdmod);
            }
        }
    }
}

static void blitDebugDescribeDRange(uint32_t startPtr,
                                    int widthWords,
                                    int height,
                                    int descending,
                                    int16_t bltdmod,
                                    uint32_t* minAddrOut,
                                    uint32_t* maxAddrOut) {
    int y;
    int64_t rowPtr;
    int64_t minAddr;
    int64_t maxAddr;
    int64_t rowStart;
    int64_t rowEnd;

    rowPtr = (int64_t)(uint32_t)startPtr;
    minAddr = rowPtr;
    maxAddr = rowPtr;
    for (y = 0; y < height; y++) {
        rowStart = rowPtr;
        if (descending) {
            rowEnd = rowStart - (int64_t)(widthWords - 1) * 2;
        } else {
            rowEnd = rowStart + (int64_t)(widthWords - 1) * 2;
        }
        if (rowStart < minAddr) minAddr = rowStart;
        if (rowStart > maxAddr) maxAddr = rowStart;
        if (rowEnd < minAddr) minAddr = rowEnd;
        if (rowEnd > maxAddr) maxAddr = rowEnd;

        if (!descending) {
            rowPtr += (int64_t)widthWords * 2 + (int64_t)bltdmod;
        } else {
            rowPtr -= (int64_t)widthWords * 2 - (int64_t)bltdmod;
        }
    }
    *minAddrOut = (uint32_t)minAddr;
    *maxAddrOut = (uint32_t)maxAddr;
}

static void blitDebugLog(M68k* cpu,
                         uint16_t bltcon0,
                         uint16_t bltcon1,
                         uint16_t bltafwm,
                         uint16_t bltalwm,
                         uint16_t bltsize,
                         uint32_t bltapt,
                         uint32_t bltbpt,
                         uint32_t bltcpt,
                         uint32_t bltdpt,
                         int16_t bltamod,
                         int16_t bltbmod,
                         int16_t bltcmod,
                         int16_t bltdmod,
                         int widthWords,
                         int height,
                         int descending) {
    uint32_t dMin;
    uint32_t dMax;
    char* modeName;

    if (!cpu->blitDebugEnabled) {
        return;
    }
    modeName = (bltcon1 & 0x0001u) ? "line" : "area";
    blitDebugDescribeDRange(bltdpt, widthWords, height, descending, bltdmod,
                            &dMin, &dMax);
    fprintf(stderr,
            "blitdbg mode=%s line=%u asm=\"%s\"\n",
            modeName,
            cpu->lastAsmLine,
            cpu->lastAsmText ? cpu->lastAsmText : "");
    fprintf(stderr,
            "  con0=%04x con1=%04x size=%04x w=%d h=%d desc=%d fwm=%04x lwm=%04x\n",
            (unsigned)bltcon0,
            (unsigned)bltcon1,
            (unsigned)bltsize,
            widthWords,
            height,
            descending,
            (unsigned)bltafwm,
            (unsigned)bltalwm);
    fprintf(stderr,
            "  apt=%08x bpt=%08x cpt=%08x dpt=%08x amod=%d bmod=%d cmod=%d dmod=%d\n",
            (unsigned)bltapt,
            (unsigned)bltbpt,
            (unsigned)bltcpt,
            (unsigned)bltdpt,
            (int)bltamod,
            (int)bltbmod,
            (int)bltcmod,
            (int)bltdmod);
    fprintf(stderr,
            "  dRange=[%08x..%08x] memSize=%08x\n",
            (unsigned)dMin,
            (unsigned)dMax,
            (unsigned)cpu->memSize);
}

static void runBlitter(M68k* cpu) {
    uint16_t bltcon0 = readCustomWordRaw(cpu, kBltcon0Offset);
    uint16_t bltcon1 = readCustomWordRaw(cpu, kBltcon1Offset);
    uint16_t bltafwm = readCustomWordRaw(cpu, kBltafwmOffset);
    uint16_t bltalwm = readCustomWordRaw(cpu, kBltalwmOffset);
    uint16_t bltbdat = readCustomWordRaw(cpu, kBltbdatOffset);
    uint16_t bltadat = readCustomWordRaw(cpu, kBltadatOffset);
    uint16_t bltsize = readCustomWordRaw(cpu, kBltsizeOffset);
    uint32_t bltcpt = readCustomLongRaw(cpu, kBltcpthOffset);
    uint32_t bltbpt = readCustomLongRaw(cpu, kBltbpthOffset);
    uint32_t bltapt = readCustomLongRaw(cpu, kBltapthOffset);
    uint32_t bltdpt = readCustomLongRaw(cpu, kBltdpthOffset);
    int16_t bltcmod = (int16_t)readCustomWordRaw(cpu, kBltcmodOffset);
    int16_t bltbmod = (int16_t)readCustomWordRaw(cpu, kBltbmodOffset);
    int16_t bltamod = (int16_t)readCustomWordRaw(cpu, kBltamodOffset);
    int16_t bltdmod = (int16_t)readCustomWordRaw(cpu, kBltdmodOffset);
    uint8_t minterm = (uint8_t)(bltcon0 & 0x00FFu);
    int ash = (int)((bltcon0 >> 12) & 0x0Fu);
    int bsh = (int)((bltcon1 >> 12) & 0x0Fu);
    int descending = (bltcon1 & 0x0002u) ? 1 : 0;
    int widthWords = (int)(bltsize & 0x003Fu);
    int height = (int)((bltsize >> 6) & 0x03FFu);
    int y;
    int x;

    /*
     * Diagnostic: warn (only when the game has enabled BLITDEBUG) about
     * any pointer arriving here with bit 0 set on a channel that BLTCON0
     * actually enables. The hardware silently masks bit 0 below, which
     * means the blit lands one byte BEFORE the address the game thought
     * it was writing to. This nearly always means the source has an odd
     * data label, which is itself a transpiler artefact we now warn about
     * at codegen time too.
     */
    if (cpu->blitDebugEnabled) {
        int useA = (bltcon0 & 0x0800u) ? 1 : 0;
        int useB = (bltcon0 & 0x0400u) ? 1 : 0;
        int useC = (bltcon0 & 0x0200u) ? 1 : 0;
        int useD = (bltcon0 & 0x0100u) ? 1 : 0;
        int aBad = useA && (bltapt & 1u);
        int bBad = useB && (bltbpt & 1u);
        int cBad = useC && (bltcpt & 1u);
        int dBad = useD && (bltdpt & 1u);
        if (aBad || bBad || cBad || dBad) {
            fprintf(stderr,
                    "blitdbg WARNING odd pointer(s) before bit-0 mask"
                    " (line=%u asm=\"%s\"):",
                    cpu->lastAsmLine,
                    cpu->lastAsmText ? cpu->lastAsmText : "");
            if (aBad) fprintf(stderr, " A=%08x", (unsigned)bltapt);
            if (bBad) fprintf(stderr, " B=%08x", (unsigned)bltbpt);
            if (cBad) fprintf(stderr, " C=%08x", (unsigned)bltcpt);
            if (dBad) fprintf(stderr, " D=%08x", (unsigned)bltdpt);
            fprintf(stderr,
                    " - hardware will mask bit 0;"
                    " blit will land 1 byte BEFORE the labelled address.\n");
        }
    }

    /*
     * OCS/ECS blitter channels are word based. Hardware ignores address bit 0
     * on BLTxPT fetch/store, so mirror that behavior here to avoid odd/even
     * jitter when game code builds byte-wise pointer deltas.
     */
    bltapt &= 0xFFFFFFFEu;
    bltbpt &= 0xFFFFFFFEu;
    bltcpt &= 0xFFFFFFFEu;
    bltdpt &= 0xFFFFFFFEu;
    (void)bltadat;

    if (widthWords == 0) {
        widthWords = 64;
    }
    if (height == 0) {
        height = 1024;
    }

    blitDebugLog(cpu, bltcon0, bltcon1, bltafwm, bltalwm, bltsize,
                 bltapt, bltbpt, bltcpt, bltdpt,
                 bltamod, bltbmod, bltcmod, bltdmod,
                 widthWords, height, descending);

    setBlitterBusy(cpu, 1);

    if ((bltcon1 & 0x0001u) != 0u) {
        runBlitterLineMode(cpu,
                           bltcon0,
                           bltcon1,
                           bltbdat,
                           bltsize,
                           bltapt,
                           bltdpt,
                           bltbmod,
                           bltamod,
                           bltdmod);
        setBlitterBusy(cpu, 0);
        return;
    }

    for (y = 0; y < height; y++) {
        uint32_t rowA = bltapt;
        uint32_t rowB = bltbpt;
        uint32_t rowC = bltcpt;
        uint32_t rowD = bltdpt;
        uint16_t prevA = 0u;
        uint16_t prevB = 0u;

        for (x = 0; x < widthWords; x++) {
            uint16_t a = readWordSafe(cpu, rowA);
            uint16_t b = readWordSafe(cpu, rowB);
            uint16_t c = readWordSafe(cpu, rowC);
            uint16_t mask = 0xFFFFu;
            uint32_t step = descending ? (uint32_t)-2 : (uint32_t)2;
            uint16_t shiftedA;
            uint16_t shiftedB;
            uint16_t d;

            if (x == 0) {
                mask &= bltafwm;
            }
            if (x == widthWords - 1) {
                mask &= bltalwm;
            }
            a &= mask;

            /*
             * Agnus funnel: 32-bit concat then >> shift. (WinUAE blitter_loadadat /
             * blitter_dofast.) The old (a>>n)|(prev<<(16-n)) in 16-bit truncated
             * prev<<(16-n) and reversed smooth 0..15 subpixel order.
             */
            if (!descending) {
                uint32_t wa;
                uint32_t wb;
                wa = ((uint32_t)prevA << 16) | (uint32_t)a;
                wb = ((uint32_t)prevB << 16) | (uint32_t)b;
                shiftedA = (uint16_t)(wa >> ash);
                shiftedB = (uint16_t)(wb >> bsh);
            } else {
                int descA;
                int descB;
                uint32_t wa;
                uint32_t wb;
                descA = 16 - ash;
                descB = 16 - bsh;
                wa = ((uint32_t)a << 16) | (uint32_t)prevA;
                wb = ((uint32_t)b << 16) | (uint32_t)prevB;
                shiftedA = (uint16_t)(wa >> descA);
                shiftedB = (uint16_t)(wb >> descB);
            }

            d = applyMinterm(shiftedA, shiftedB, c, minterm);
            writeWordSafe(cpu, rowD, d);

            prevA = a;
            prevB = b;
            rowA += step;
            rowB += step;
            rowC += step;
            rowD += step;
        }

        if (!descending) {
            bltapt += (uint32_t)((int32_t)widthWords * 2 + bltamod);
            bltbpt += (uint32_t)((int32_t)widthWords * 2 + bltbmod);
            bltcpt += (uint32_t)((int32_t)widthWords * 2 + bltcmod);
            bltdpt += (uint32_t)((int32_t)widthWords * 2 + bltdmod);
        } else {
            bltapt -= (uint32_t)((int32_t)widthWords * 2 - bltamod);
            bltbpt -= (uint32_t)((int32_t)widthWords * 2 - bltbmod);
            bltcpt -= (uint32_t)((int32_t)widthWords * 2 - bltcmod);
            bltdpt -= (uint32_t)((int32_t)widthWords * 2 - bltdmod);
        }
    }

    writeCustomWordRaw(cpu, kBltdpthOffset, (uint16_t)((bltdpt >> 16) & 0xFFFFu));
    writeCustomWordRaw(cpu, kBltdpthOffset + 2u, (uint16_t)(bltdpt & 0xFFFFu));
    setBlitterBusy(cpu, 0);
}

void m68kInit(M68k* cpu, uint8_t* mem, uint32_t memSize) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->mem = mem;
    cpu->memSize = memSize;
}

void m68kAmigaFrameTick(M68k* cpu) {
    cpu->beamFramePhase += 64u;
    cpu->beamReadCounter = 0;
}

uint8_t* m68kLinearRamBytePtr(M68k* cpu, uint32_t addr) {
    if (fitsCustom(addr, 1) || fitsCia(addr, 1)) {
        return (uint8_t*)0;
    }
    if (fitsRam(cpu, addr, 1)) {
        return cpu->mem + addr;
    }
    return (uint8_t*)0;
}

static uint8_t labelTracePrintDisabled = 0u;
static M68kHostFrameBoundaryHookFn hostFrameBoundaryHook;

void m68kSetHostFrameBoundaryHook(M68kHostFrameBoundaryHookFn fn) {
    hostFrameBoundaryHook = fn;
}

static int labelTracePrintActive(M68k* cpu) {
    if (labelTracePrintDisabled != 0u) {
        return 0;
    }
    if (cpu->labelTracePrintEnabled != 0u) {
        return 1;
    }
    static uint8_t envReady = 0u;
    static uint8_t envOn = 0u;
    if (envReady == 0u) {
        char* value = getenv("M68K_TRACE_LABELS");
        envReady = 1u;
        if (value != NULL && value[0] != '\0' && value[0] != '0') {
            envOn = 1u;
        }
    }
    return envOn;
}

void m68kSetLabelTracePrint(M68k* cpu, uint8_t enabled) {
    cpu->labelTracePrintEnabled = enabled;
}

void m68kDisableLabelTracePrint(void) {
    labelTracePrintDisabled = 1u;
}

int m68kHostFrameBoundary(M68k* cpu, uint32_t resumePc) {
    if (hostFrameBoundaryHook != NULL) {
        hostFrameBoundaryHook(cpu);
    }
    cpu->intreqPending = (uint16_t)(cpu->intreqPending | M68K_INT_VERTB);
    return m68kTryDispatchInterrupt(cpu, M68K_VEC_IRQ3, M68K_INT_VERTB, resumePc);
}

void m68kHostStageKeyboardInterrupt(M68k* cpu, uint8_t amigaKeyCode) {
    cpu->keyboardPendingCode = amigaKeyCode;
    cpu->keyboardInterruptPending = 1u;
    cpu->ciaRegs[kCiaADataOffset] = amigaKeyCode;
    cpu->ciaRegs[kCiaAIcrOffset] =
        (uint8_t)(cpu->ciaRegs[kCiaAIcrOffset] | 0x08u);
    cpu->intreqPending = (uint16_t)(cpu->intreqPending | M68K_INT_PORTS);
}

int m68kHostPollPendingKeyboardInterrupt(M68k* cpu) {
    if (cpu->keyboardInterruptPending == 0u) {
        return 0;
    }
    if ((cpu->intenaEnabled & M68K_INT_PORTS) == 0u) {
        return 0;
    }
    cpu->keyboardInterruptPending = 0u;
    return m68kTryDispatchInterrupt(cpu, M68K_VEC_IRQ2, M68K_INT_PORTS, cpu->pcVirtual);
}

int m68kHostKeyboardInterrupt(M68k* cpu, uint8_t amigaKeyCode) {
    m68kHostStageKeyboardInterrupt(cpu, amigaKeyCode);
    return m68kHostPollPendingKeyboardInterrupt(cpu);
}

void m68kTraceLabel(M68k* cpu, uint32_t pc, char* labelName) {
    cpu->lastLabelPc = pc;
    cpu->lastLabelName = labelName;

    if (labelTracePrintActive(cpu)) {
        if (labelName == NULL || labelName[0] == '\0') {
            labelName = "(unknown)";
        }
        fprintf(stderr, "label: %s (0x%08x)\n", labelName, pc);
    }

    uint32_t slot = cpu->labelTraceNext;
    if (slot >= M68K_LABEL_TRACE_DEPTH) {
        slot = 0;
        cpu->labelTraceNext = 0;
    }
    cpu->labelTracePc[slot] = pc;
    cpu->labelTraceName[slot] = labelName;
    cpu->labelTraceNext = (uint8_t)((slot + 1u) % M68K_LABEL_TRACE_DEPTH);
    if (cpu->labelTraceCount < M68K_LABEL_TRACE_DEPTH) {
        cpu->labelTraceCount++;
    }
}

int m68kLoadFile(M68k* cpu, char* fileName, uint32_t addr, uint32_t maxBytes) {
    if (addr >= cpu->memSize) {
        fprintf(stderr, "m68kLoadFile: address 0x%08x out of gMem (size 0x%08x)\n",
            addr, cpu->memSize);
        return 0;
    }
    uint32_t cap = cpu->memSize - addr;
    if (maxBytes > cap) maxBytes = cap;

    FILE* f = fopen(fileName, "rb");
    if (f == NULL) {
        fprintf(stderr, "m68kLoadFile: cannot open '%s' (emulated at 0x%08x)\n",
            fileName, addr);
        return 0;
    }
    size_t total = 0;
    while (total < (size_t)maxBytes) {
        size_t n = fread(&cpu->mem[addr + (uint32_t)total], 1,
            (size_t)maxBytes - total, f);
        if (n == 0) {
            if (ferror(f)) {
                fprintf(stderr, "m68kLoadFile: read error for '%s'\n", fileName);
            }
            break;
        }
        total += n;
    }
    fclose(f);
    return (int)total;
}

int m68kLoadFileBase(M68k* cpu, char* baseFolder, char* fileName, uint32_t addr, uint32_t maxBytes) {
    if (baseFolder == NULL || baseFolder[0] == '\0') {
        return m68kLoadFile(cpu, fileName, addr, maxBytes);
    }
    size_t baseLen = strlen(baseFolder);
    size_t nameLen = strlen(fileName);
    /* separator between folder and filename if baseFolder doesn't end with one */
    int needSep = (baseFolder[baseLen - 1] != '/' && baseFolder[baseLen - 1] != '\\') ? 1 : 0;
    char* fullPath = (char*)malloc(baseLen + (size_t)needSep + nameLen + 1u);
    if (fullPath == NULL) {
        fprintf(stderr, "m68kLoadFileBase: out of memory\n");
        return 0;
    }
    memcpy(fullPath, baseFolder, baseLen);
    if (needSep) {
        fullPath[baseLen] = '/';
    }
    memcpy(fullPath + baseLen + (size_t)needSep, fileName, nameLen + 1u);
    int result = m68kLoadFile(cpu, fullPath, addr, maxBytes);
    free(fullPath);
    return result;
}

/*
 * Memory access.
 *
 * Byte-shift construction makes the 68000's big-endian layout explicit
 * without caring what the host CPU endianness is. Bounds are asserted
 * by silent wrap for now; Phase 0 tests never cross the buffer edge.
 */

uint8_t read8(M68k* cpu, uint32_t addr) {
    if (fitsCustom(addr, 1)) {
        uint32_t off = addr - kCustomBase;
        return readCustomByte(cpu, off);
    }
    if (fitsCia(addr, 1)) {
        uint32_t off = addr - kCiaBase;
        return cpu->ciaRegs[off];
    }
    if (fitsRam(cpu, addr, 1)) {
        return cpu->mem[addr];
    }
    memoryTrap(cpu, addr, 1, 0);
    return cpu->mem[addr];
}

uint16_t read16(M68k* cpu, uint32_t addr) {
    uint16_t hi;
    uint16_t lo;
    if (fitsCustom(addr, 2)) {
        uint32_t off = addr - kCustomBase;
        hi = readCustomByte(cpu, off);
        lo = readCustomByte(cpu, off + 1u);
        return (uint16_t)((hi << 8) | lo);
    }
    if (fitsCia(addr, 2)) {
        uint32_t off = addr - kCiaBase;
        hi = cpu->ciaRegs[off];
        lo = cpu->ciaRegs[off + 1u];
        return (uint16_t)((hi << 8) | lo);
    }
    if (fitsRam(cpu, addr, 2)) {
        hi = cpu->mem[addr];
        lo = cpu->mem[addr + 1];
        return (uint16_t)((hi << 8) | lo);
    }
    memoryTrap(cpu, addr, 2, 0);
    hi = 0;
    lo = 0;
    return (uint16_t)((hi << 8) | lo);
}

uint32_t read32(M68k* cpu, uint32_t addr) {
    uint32_t b0;
    uint32_t b1;
    uint32_t b2;
    uint32_t b3;
    if (fitsCustom(addr, 4)) {
        uint32_t off = addr - kCustomBase;
        b0 = readCustomByte(cpu, off);
        b1 = readCustomByte(cpu, off + 1u);
        b2 = readCustomByte(cpu, off + 2u);
        b3 = readCustomByte(cpu, off + 3u);
        return (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
    }
    if (fitsCia(addr, 4)) {
        uint32_t off = addr - kCiaBase;
        b0 = cpu->ciaRegs[off];
        b1 = cpu->ciaRegs[off + 1u];
        b2 = cpu->ciaRegs[off + 2u];
        b3 = cpu->ciaRegs[off + 3u];
        return (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
    }
    if (fitsRam(cpu, addr, 4)) {
        b0 = cpu->mem[addr];
        b1 = cpu->mem[addr + 1];
        b2 = cpu->mem[addr + 2];
        b3 = cpu->mem[addr + 3];
        return (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
    }
    memoryTrap(cpu, addr, 4, 0);
    b0 = 0;
    b1 = 0;
    b2 = 0;
    b3 = 0;
    return (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
}

void write8(M68k* cpu, uint32_t addr, uint8_t value) {
    if (fitsCustom(addr, 1)) {
        uint32_t off = addr - kCustomBase;
        writeCustomByte(cpu, off, value);
        return;
    }
    if (fitsCia(addr, 1)) {
        uint32_t off = addr - kCiaBase;
        cpu->ciaRegs[off] = value;
        return;
    }
    if (fitsRam(cpu, addr, 1)) {
        cpu->mem[addr] = value;
        return;
    }
    memoryTrap(cpu, addr, 1, 1);
    cpu->mem[addr] = value;
}

void write16(M68k* cpu, uint32_t addr, uint16_t value) {
    if (fitsCustom(addr, 2)) {
        uint32_t off = addr - kCustomBase;
        if (off == 0x009au) {
            writeCustomIntena(cpu, value);
            return;
        }
        if (off == 0x009cu) {
            writeCustomIntreq(cpu, value);
            return;
        }
        if (isCustomColorRegOffset(off)) {
            customColorWrite(cpu, off, value);
            return;
        }
        writeCustomByte(cpu, off, (uint8_t)((value >> 8) & 0xFF));
        writeCustomByte(cpu, off + 1u, (uint8_t)(value & 0xFF));
        if (off == kBltsizeOffset) {
            runBlitter(cpu);
        }
        return;
    }
    if (fitsCia(addr, 2)) {
        uint32_t off = addr - kCiaBase;
        cpu->ciaRegs[off] = (uint8_t)((value >> 8) & 0xFF);
        cpu->ciaRegs[off + 1u] = (uint8_t)(value & 0xFF);
        return;
    }
    if (fitsRam(cpu, addr, 2)) {
        cpu->mem[addr]     = (uint8_t)((value >> 8) & 0xFF);
        cpu->mem[addr + 1] = (uint8_t)(value & 0xFF);
        return;
    }
    memoryTrap(cpu, addr, 2, 1);
    cpu->mem[addr]     = (uint8_t)((value >> 8) & 0xFF);
    cpu->mem[addr + 1] = (uint8_t)(value & 0xFF);
}

void write32(M68k* cpu, uint32_t addr, uint32_t value) {
    if (fitsCustom(addr, 4)) {
        uint32_t off = addr - kCustomBase;
        writeCustomByte(cpu, off, (uint8_t)((value >> 24) & 0xFF));
        writeCustomByte(cpu, off + 1u, (uint8_t)((value >> 16) & 0xFF));
        writeCustomByte(cpu, off + 2u, (uint8_t)((value >> 8)  & 0xFF));
        writeCustomByte(cpu, off + 3u, (uint8_t)(value & 0xFF));
        return;
    }
    if (fitsCia(addr, 4)) {
        uint32_t off = addr - kCiaBase;
        cpu->ciaRegs[off] = (uint8_t)((value >> 24) & 0xFF);
        cpu->ciaRegs[off + 1u] = (uint8_t)((value >> 16) & 0xFF);
        cpu->ciaRegs[off + 2u] = (uint8_t)((value >> 8)  & 0xFF);
        cpu->ciaRegs[off + 3u] = (uint8_t)(value & 0xFF);
        return;
    }
    if (fitsRam(cpu, addr, 4)) {
        cpu->mem[addr]     = (uint8_t)((value >> 24) & 0xFF);
        cpu->mem[addr + 1] = (uint8_t)((value >> 16) & 0xFF);
        cpu->mem[addr + 2] = (uint8_t)((value >> 8)  & 0xFF);
        cpu->mem[addr + 3] = (uint8_t)(value & 0xFF);
        return;
    }
    memoryTrap(cpu, addr, 4, 1);
    cpu->mem[addr]     = (uint8_t)((value >> 24) & 0xFF);
    cpu->mem[addr + 1] = (uint8_t)((value >> 16) & 0xFF);
    cpu->mem[addr + 2] = (uint8_t)((value >> 8)  & 0xFF);
    cpu->mem[addr + 3] = (uint8_t)(value & 0xFF);
}

void push32(M68k* cpu, uint32_t value) {
    cpu->a[7] -= 4;
    write32(cpu, cpu->a[7], value);
}

void push16(M68k* cpu, uint16_t value) {
    cpu->a[7] -= 2;
    write16(cpu, cpu->a[7], value);
}

uint32_t pop32(M68k* cpu) {
    uint32_t value = read32(cpu, cpu->a[7]);
    cpu->a[7] += 4;
    return value;
}

uint16_t pop16(M68k* cpu) {
    uint16_t value = read16(cpu, cpu->a[7]);
    cpu->a[7] += 2;
    return value;
}

uint32_t setLow8(uint32_t reg, uint8_t value) {
    return (reg & 0xFFFFFF00u) | (uint32_t)value;
}

uint32_t setLow16(uint32_t reg, uint16_t value) {
    return (reg & 0xFFFF0000u) | (uint32_t)value;
}

static void moveWSetFlags(M68k* cpu, uint16_t value) {
    cpu->n = (value & 0x8000u) ? 1 : 0;
    cpu->z = (value == 0) ? 1 : 0;
    cpu->v = 0;
    cpu->c = 0;
    /* X is not affected by MOVE. */
}

static void moveBSetFlags(M68k* cpu, uint8_t value) {
    cpu->n = (value & 0x80u) ? 1 : 0;
    cpu->z = (value == 0) ? 1 : 0;
    cpu->v = 0;
    cpu->c = 0;
    /* X is not affected by MOVE. */
}

static void moveLSetFlags(M68k* cpu, uint32_t value) {
    cpu->n = (value & 0x80000000u) ? 1 : 0;
    cpu->z = (value == 0) ? 1 : 0;
    cpu->v = 0;
    cpu->c = 0;
    /* X is not affected by MOVE. */
}

void MOVE_W_imm_D(M68k* cpu, uint16_t value, int regIdx) {
    cpu->d[regIdx] = setLow16(cpu->d[regIdx], value);
    moveWSetFlags(cpu, value);
}

void MOVE_B_imm_D(M68k* cpu, uint8_t value, int regIdx) {
    cpu->d[regIdx] = setLow8(cpu->d[regIdx], value);
    moveBSetFlags(cpu, value);
}

void MOVE_B_imm_abs(M68k* cpu, uint8_t value, uint32_t address) {
    write8(cpu, address, value);
    moveBSetFlags(cpu, value);
}

void MOVE_B_imm_dA(M68k* cpu, uint8_t value, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    write8(cpu, ea, value);
    moveBSetFlags(cpu, value);
}

void MOVE_B_imm_indA(M68k* cpu, uint8_t value, int baseAddrReg) {
    write8(cpu, cpu->a[baseAddrReg], value);
    moveBSetFlags(cpu, value);
}

void MOVE_B_imm_piA(M68k* cpu, uint8_t value, int addrReg) {
    uint32_t ea = cpu->a[addrReg];
    write8(cpu, ea, value);
    cpu->a[addrReg] = ea + 1u;
    moveBSetFlags(cpu, value);
}

void MOVE_W_imm_abs(M68k* cpu, uint16_t value, uint32_t address) {
    write16(cpu, address, value);
    moveWSetFlags(cpu, value);
}

void MOVE_W_imm_dA(M68k* cpu, uint16_t value, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    write16(cpu, ea, value);
    moveWSetFlags(cpu, value);
}

void MOVE_W_imm_indA(M68k* cpu, uint16_t value, int baseAddrReg) {
    write16(cpu, cpu->a[baseAddrReg], value);
    moveWSetFlags(cpu, value);
}

void MOVE_W_imm_piA(M68k* cpu, uint16_t value, int addrReg) {
    uint32_t ea = cpu->a[addrReg];
    write16(cpu, ea, value);
    cpu->a[addrReg] = ea + 2u;
    moveWSetFlags(cpu, value);
}

void MOVE_L_imm_A(M68k* cpu, uint32_t value, int regIdx) {
    /* MOVEA does not affect any flags. */
    cpu->a[regIdx] = value;
}

void MOVE_L_imm_D(M68k* cpu, uint32_t value, int regIdx) {
    cpu->d[regIdx] = value;
    moveLSetFlags(cpu, value);
}

void MOVE_L_imm_abs(M68k* cpu, uint32_t value, uint32_t address) {
    write32(cpu, address, value);
    moveLSetFlags(cpu, value);
}

void MOVE_L_imm_dA(M68k* cpu, uint32_t value, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    write32(cpu, ea, value);
    moveLSetFlags(cpu, value);
}

void MOVE_L_imm_indA(M68k* cpu, uint32_t value, int baseAddrReg) {
    write32(cpu, cpu->a[baseAddrReg], value);
    moveLSetFlags(cpu, value);
}

void MOVE_L_imm_piA(M68k* cpu, uint32_t value, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg];
    write32(cpu, ea, value);
    cpu->a[baseAddrReg] = ea + 4u;
    moveLSetFlags(cpu, value);
}

void MOVEQ_imm_D(M68k* cpu, uint32_t value, int regIdx) {
    int8_t imm8 = (int8_t)(uint8_t)(value & 0xFFu);
    uint32_t result = (uint32_t)(int32_t)imm8;
    cpu->d[regIdx] = result;
    cpu->n = (result & 0x80000000u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    cpu->v = 0;
    cpu->c = 0;
    /* X is not affected by MOVEQ. */
}

static uint32_t indexExtend(M68k* cpu,
                            int indexReg,
                            int indexIsAddr,
                            int indexSize,
                            int indexScale) {
    uint32_t raw = indexIsAddr ? cpu->a[indexReg] : cpu->d[indexReg];
    uint32_t idx;
    if (indexSize == 'l') idx = raw;
    else {
        int16_t low = (int16_t)(uint16_t)(raw & 0xFFFFu);
        idx = (uint32_t)(int32_t)low;
    }
    return idx * (uint32_t)indexScale;
}

void MOVE_L_dAIx_A(M68k* cpu,
                   int32_t disp,
                   int baseAddrReg,
                   int indexReg,
                   int indexIsAddr,
                   int indexSize, int indexScale,
                   int dstAddrReg) {
    uint32_t base = cpu->a[baseAddrReg];
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t ea = base + (uint32_t)disp + idx;
    cpu->a[dstAddrReg] = read32(cpu, ea);
}

void MOVE_W_abs_D(M68k* cpu, uint32_t address, int regIdx) {
    uint16_t value = read16(cpu, address);
    cpu->d[regIdx] = setLow16(cpu->d[regIdx], value);
    moveWSetFlags(cpu, value);
}

void MOVE_W_abs_abs(M68k* cpu, uint32_t srcAddress, uint32_t dstAddress) {
    uint16_t value = read16(cpu, srcAddress);
    write16(cpu, dstAddress, value);
    moveWSetFlags(cpu, value);
}

void MOVE_W_abs_indA(M68k* cpu, uint32_t srcAddress, int dstAddrReg) {
    uint16_t value = read16(cpu, srcAddress);
    write16(cpu, cpu->a[dstAddrReg], value);
    moveWSetFlags(cpu, value);
}

void MOVE_W_abs_dA(M68k* cpu, uint32_t srcAddress, int32_t disp, int baseAddrReg) {
    uint16_t value = read16(cpu, srcAddress);
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    write16(cpu, ea, value);
    moveWSetFlags(cpu, value);
}

void MOVE_W_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int regIdx) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint16_t value = read16(cpu, ea);
    cpu->d[regIdx] = setLow16(cpu->d[regIdx], value);
    moveWSetFlags(cpu, value);
}

void MOVE_W_dA_abs(M68k* cpu, int32_t disp, int baseAddrReg, uint32_t dstAddress) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint16_t value = read16(cpu, ea);
    write16(cpu, dstAddress, value);
    moveWSetFlags(cpu, value);
}

void MOVE_W_dAIx_D(M68k* cpu,
                   int32_t disp,
                   int baseAddrReg,
                   int indexReg,
                   int indexIsAddr,
                   int indexSize, int indexScale,
                   int regIdx) {
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp + idx;
    uint16_t value = read16(cpu, ea);
    cpu->d[regIdx] = setLow16(cpu->d[regIdx], value);
    moveWSetFlags(cpu, value);
}

void MOVE_W_indA_D(M68k* cpu, int baseAddrReg, int regIdx) {
    uint16_t value = read16(cpu, cpu->a[baseAddrReg]);
    cpu->d[regIdx] = setLow16(cpu->d[regIdx], value);
    moveWSetFlags(cpu, value);
}

void MOVE_W_piA_D(M68k* cpu, int baseAddrReg, int regIdx) {
    uint32_t ea = cpu->a[baseAddrReg];
    uint16_t value = read16(cpu, ea);
    cpu->a[baseAddrReg] = ea + 2u;
    cpu->d[regIdx] = setLow16(cpu->d[regIdx], value);
    moveWSetFlags(cpu, value);
}

void MOVE_W_pdA_D(M68k* cpu, int srcAddrReg, int dstDataReg) {
    uint32_t newEa = cpu->a[srcAddrReg] - 2u;
    cpu->a[srcAddrReg] = newEa;
    uint16_t value = read16(cpu, newEa);
    cpu->d[dstDataReg] = setLow16(cpu->d[dstDataReg], value);
    moveWSetFlags(cpu, value);
}

void MOVE_W_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint16_t value = (uint16_t)(cpu->d[srcDataReg] & 0xFFFFu);
    cpu->d[dstDataReg] = setLow16(cpu->d[dstDataReg], value);
    moveWSetFlags(cpu, value);
}

void MOVE_W_D_abs(M68k* cpu, int srcDataReg, uint32_t address) {
    uint16_t value = (uint16_t)(cpu->d[srcDataReg] & 0xFFFFu);
    write16(cpu, address, value);
    moveWSetFlags(cpu, value);
}

void MOVE_W_D_dA(M68k* cpu, int srcDataReg, int32_t disp, int baseAddrReg) {
    uint16_t value = (uint16_t)(cpu->d[srcDataReg] & 0xFFFFu);
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    write16(cpu, ea, value);
    moveWSetFlags(cpu, value);
}

void MOVE_W_D_dAIx(M68k* cpu,
                   int srcDataReg,
                   int32_t disp,
                   int baseAddrReg,
                   int indexReg,
                   int indexIsAddr,
                   int indexSize, int indexScale) {
    uint16_t value = (uint16_t)(cpu->d[srcDataReg] & 0xFFFFu);
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t dstEa = cpu->a[baseAddrReg] + (uint32_t)disp + idx;
    write16(cpu, dstEa, value);
    moveWSetFlags(cpu, value);
}

void MOVE_W_dA_dA(M68k* cpu,
                  int32_t srcDisp,
                  int srcBaseAddrReg,
                  int32_t dstDisp,
                  int dstBaseAddrReg) {
    uint32_t sea = cpu->a[srcBaseAddrReg] + (uint32_t)srcDisp;
    uint32_t dea = cpu->a[dstBaseAddrReg] + (uint32_t)dstDisp;
    uint16_t value = read16(cpu, sea);
    write16(cpu, dea, value);
    moveWSetFlags(cpu, value);
}

void MOVE_W_indA_indA(M68k* cpu, int srcAddrReg, int dstAddrReg) {
    uint16_t value = read16(cpu, cpu->a[srcAddrReg]);
    write16(cpu, cpu->a[dstAddrReg], value);
    moveWSetFlags(cpu, value);
}

void MOVE_W_indA_piA(M68k* cpu, int srcAddrReg, int dstAddrReg) {
    uint16_t value = read16(cpu, cpu->a[srcAddrReg]);
    uint32_t dea = cpu->a[dstAddrReg];
    write16(cpu, dea, value);
    cpu->a[dstAddrReg] = dea + 2u;
    moveWSetFlags(cpu, value);
}

void MOVE_W_piA_indA(M68k* cpu, int srcAddrReg, int dstAddrReg) {
    uint32_t sea = cpu->a[srcAddrReg];
    uint16_t value = read16(cpu, sea);
    cpu->a[srcAddrReg] = sea + 2u;
    write16(cpu, cpu->a[dstAddrReg], value);
    moveWSetFlags(cpu, value);
}

void MOVE_W_piA_piA(M68k* cpu, int srcAddrReg, int dstAddrReg) {
    uint32_t sea = cpu->a[srcAddrReg];
    uint16_t value = read16(cpu, sea);
    cpu->a[srcAddrReg] = sea + 2u;
    uint32_t dea = cpu->a[dstAddrReg];
    write16(cpu, dea, value);
    cpu->a[dstAddrReg] = dea + 2u;
    moveWSetFlags(cpu, value);
}

void MOVE_W_dA_indA(M68k* cpu, int32_t disp, int srcBaseAddrReg, int dstAddrReg) {
    uint32_t sea = cpu->a[srcBaseAddrReg] + (uint32_t)disp;
    uint16_t value = read16(cpu, sea);
    write16(cpu, cpu->a[dstAddrReg], value);
    moveWSetFlags(cpu, value);
}

void MOVE_W_dA_piA(M68k* cpu, int32_t disp, int srcBaseAddrReg, int dstAddrReg) {
    uint32_t sea = cpu->a[srcBaseAddrReg] + (uint32_t)disp;
    uint16_t value = read16(cpu, sea);
    uint32_t dea = cpu->a[dstAddrReg];
    write16(cpu, dea, value);
    cpu->a[dstAddrReg] = dea + 2u;
    moveWSetFlags(cpu, value);
}

void MOVE_W_indA_dA(M68k* cpu, int srcAddrReg, int32_t dstDisp, int dstBaseAddrReg) {
    uint16_t value = read16(cpu, cpu->a[srcAddrReg]);
    uint32_t dea = cpu->a[dstBaseAddrReg] + (uint32_t)dstDisp;
    write16(cpu, dea, value);
    moveWSetFlags(cpu, value);
}

void MOVE_W_indA_abs(M68k* cpu, int srcAddrReg, uint32_t dstAddress) {
    uint16_t value = read16(cpu, cpu->a[srcAddrReg]);
    write16(cpu, dstAddress, value);
    moveWSetFlags(cpu, value);
}

void MOVE_W_D_pdA(M68k* cpu, int srcDataReg, int dstAddrReg) {
    uint16_t value = (uint16_t)(cpu->d[srcDataReg] & 0xFFFFu);
    uint32_t newEa = cpu->a[dstAddrReg] - 2u;
    cpu->a[dstAddrReg] = newEa;
    write16(cpu, newEa, value);
    moveWSetFlags(cpu, value);
}

void MOVE_W_abs_pdA(M68k* cpu, uint32_t srcAddress, int dstAddrReg) {
    uint16_t value = read16(cpu, srcAddress);
    uint32_t newEa = cpu->a[dstAddrReg] - 2u;
    cpu->a[dstAddrReg] = newEa;
    write16(cpu, newEa, value);
    moveWSetFlags(cpu, value);
}

void MOVE_W_A_abs(M68k* cpu, int srcAddrReg, uint32_t dstAddress) {
    uint16_t value = (uint16_t)(cpu->a[srcAddrReg] & 0xFFFFu);
    write16(cpu, dstAddress, value);
    moveWSetFlags(cpu, value);
}

void MOVE_W_dAIx_dA(M68k* cpu,
                    int32_t srcDisp,
                    int srcBaseAddrReg,
                    int indexReg,
                    int indexIsAddr,
                    int indexSize, int indexScale,
                    int32_t dstDisp,
                    int dstBaseAddrReg) {
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t sea = cpu->a[srcBaseAddrReg] + (uint32_t)srcDisp + idx;
    uint16_t value = read16(cpu, sea);
    uint32_t dea = cpu->a[dstBaseAddrReg] + (uint32_t)dstDisp;
    write16(cpu, dea, value);
    moveWSetFlags(cpu, value);
}

void MOVE_W_dAIx_dAIx(M68k* cpu,
                      int32_t srcDisp,
                      int srcBaseAddrReg,
                      int srcIndexReg,
                      int srcIndexIsAddr,
                      int srcIndexSize, int srcIndexScale,
                      int32_t dstDisp,
                      int dstBaseAddrReg,
                      int dstIndexReg,
                      int dstIndexIsAddr,
                      int dstIndexSize, int dstIndexScale) {
    uint32_t srcIdx = indexExtend(cpu, srcIndexReg, srcIndexIsAddr, srcIndexSize, srcIndexScale);
    uint32_t sea = cpu->a[srcBaseAddrReg] + (uint32_t)srcDisp + srcIdx;
    uint16_t value = read16(cpu, sea);
    uint32_t dstIdx = indexExtend(cpu, dstIndexReg, dstIndexIsAddr, dstIndexSize, dstIndexScale);
    uint32_t dea = cpu->a[dstBaseAddrReg] + (uint32_t)dstDisp + dstIdx;
    write16(cpu, dea, value);
    moveWSetFlags(cpu, value);
}

void MOVE_W_abs_dAIx(M68k* cpu,
                     uint32_t srcAddress,
                     int32_t dstDisp,
                     int dstBaseAddrReg,
                     int indexReg,
                     int indexIsAddr,
                     int indexSize, int indexScale) {
    uint16_t value = read16(cpu, srcAddress);
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t dea = cpu->a[dstBaseAddrReg] + (uint32_t)dstDisp + idx;
    write16(cpu, dea, value);
    moveWSetFlags(cpu, value);
}

void MOVE_W_imm_dAIx(M68k* cpu,
                     uint16_t value,
                     int32_t dstDisp,
                     int dstBaseAddrReg,
                     int indexReg,
                     int indexIsAddr,
                     int indexSize, int indexScale) {
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t dea = cpu->a[dstBaseAddrReg] + (uint32_t)dstDisp + idx;
    write16(cpu, dea, value);
    moveWSetFlags(cpu, value);
}

void MOVE_W_dAIx_abs(M68k* cpu,
                     int32_t srcDisp,
                     int srcBaseAddrReg,
                     int indexReg,
                     int indexIsAddr,
                     int indexSize, int indexScale,
                     uint32_t dstAddress) {
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t sea = cpu->a[srcBaseAddrReg] + (uint32_t)srcDisp + idx;
    uint16_t value = read16(cpu, sea);
    write16(cpu, dstAddress, value);
    moveWSetFlags(cpu, value);
}

void MOVE_W_imm_A(M68k* cpu, uint16_t value, int dstAddrReg) {
    int32_t extended = (int32_t)(int16_t)value;
    cpu->a[dstAddrReg] = (uint32_t)extended;
    /* MOVEA.W does not affect condition codes. */
}

void MOVE_W_dA_A(M68k* cpu, int32_t disp, int baseAddrReg, int dstAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint16_t raw = read16(cpu, ea);
    int32_t extended = (int32_t)(int16_t)raw;
    cpu->a[dstAddrReg] = (uint32_t)extended;
    /* MOVEA.W does not affect condition codes. */
}

void MOVE_W_abs_piA(M68k* cpu, uint32_t srcAddress, int dstAddrReg) {
    uint16_t value = read16(cpu, srcAddress);
    uint32_t dea = cpu->a[dstAddrReg];
    write16(cpu, dea, value);
    cpu->a[dstAddrReg] = dea + 2u;
    moveWSetFlags(cpu, value);
}

void MOVE_W_pdA_indA(M68k* cpu, int srcAddrReg, int dstAddrReg) {
    uint32_t newEa = cpu->a[srcAddrReg] - 2u;
    cpu->a[srcAddrReg] = newEa;
    uint16_t value = read16(cpu, newEa);
    write16(cpu, cpu->a[dstAddrReg], value);
    moveWSetFlags(cpu, value);
}

void MOVE_B_abs_D(M68k* cpu, uint32_t address, int regIdx) {
    uint8_t value = read8(cpu, address);
    cpu->d[regIdx] = setLow8(cpu->d[regIdx], value);
    moveBSetFlags(cpu, value);
}

void MOVE_B_abs_abs(M68k* cpu, uint32_t srcAddress, uint32_t dstAddress) {
    uint8_t value = read8(cpu, srcAddress);
    write8(cpu, dstAddress, value);
    moveBSetFlags(cpu, value);
}

void MOVE_B_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int regIdx) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint8_t value = read8(cpu, ea);
    cpu->d[regIdx] = setLow8(cpu->d[regIdx], value);
    moveBSetFlags(cpu, value);
}

void MOVE_B_dAIx_D(M68k* cpu,
                   int32_t disp,
                   int baseAddrReg,
                   int indexReg,
                   int indexIsAddr,
                   int indexSize, int indexScale,
                   int regIdx) {
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp + idx;
    uint8_t value = read8(cpu, ea);
    cpu->d[regIdx] = setLow8(cpu->d[regIdx], value);
    moveBSetFlags(cpu, value);
}

void MOVE_B_absIx_D(M68k* cpu,
                    uint32_t baseAddress,
                    int indexReg,
                    int indexIsAddr,
                    int indexSize, int indexScale,
                    int regIdx) {
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t ea = baseAddress + idx;
    uint8_t value = read8(cpu, ea);
    cpu->d[regIdx] = setLow8(cpu->d[regIdx], value);
    moveBSetFlags(cpu, value);
}

void MOVE_B_indA_D(M68k* cpu, int baseAddrReg, int regIdx) {
    uint8_t value = read8(cpu, cpu->a[baseAddrReg]);
    cpu->d[regIdx] = setLow8(cpu->d[regIdx], value);
    moveBSetFlags(cpu, value);
}

void MOVE_B_piA_D(M68k* cpu, int baseAddrReg, int regIdx) {
    uint32_t ea = cpu->a[baseAddrReg];
    uint8_t value = read8(cpu, ea);
    cpu->a[baseAddrReg] = ea + 1u;
    cpu->d[regIdx] = setLow8(cpu->d[regIdx], value);
    moveBSetFlags(cpu, value);
}

void MOVE_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint8_t value = (uint8_t)(cpu->d[srcDataReg] & 0xFFu);
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], value);
    moveBSetFlags(cpu, value);
}

void MOVE_B_D_abs(M68k* cpu, int srcDataReg, uint32_t address) {
    uint8_t value = (uint8_t)(cpu->d[srcDataReg] & 0xFFu);
    write8(cpu, address, value);
    moveBSetFlags(cpu, value);
}

void MOVE_B_D_dA(M68k* cpu, int srcDataReg, int32_t disp, int baseAddrReg) {
    uint8_t value = (uint8_t)(cpu->d[srcDataReg] & 0xFFu);
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    write8(cpu, ea, value);
    moveBSetFlags(cpu, value);
}

void MOVE_B_D_indA(M68k* cpu, int srcDataReg, int addrReg) {
    uint8_t value = (uint8_t)(cpu->d[srcDataReg] & 0xFFu);
    write8(cpu, cpu->a[addrReg], value);
    moveBSetFlags(cpu, value);
}

void MOVE_B_D_piA(M68k* cpu, int srcDataReg, int addrReg) {
    uint8_t value = (uint8_t)(cpu->d[srcDataReg] & 0xFFu);
    uint32_t ea = cpu->a[addrReg];
    write8(cpu, ea, value);
    cpu->a[addrReg] = ea + 1u;
    moveBSetFlags(cpu, value);
}

void MOVE_B_D_pdA(M68k* cpu, int srcDataReg, int addrReg) {
    uint8_t value = (uint8_t)(cpu->d[srcDataReg] & 0xFFu);
    uint32_t newEa = cpu->a[addrReg] - 1u;
    cpu->a[addrReg] = newEa;
    write8(cpu, newEa, value);
    moveBSetFlags(cpu, value);
}

void MOVE_B_indA_indA(M68k* cpu, int srcAddrReg, int dstAddrReg) {
    uint8_t value = read8(cpu, cpu->a[srcAddrReg]);
    write8(cpu, cpu->a[dstAddrReg], value);
    moveBSetFlags(cpu, value);
}

void MOVE_B_indA_piA(M68k* cpu, int srcAddrReg, int dstAddrReg) {
    uint8_t value = read8(cpu, cpu->a[srcAddrReg]);
    uint32_t dea = cpu->a[dstAddrReg];
    write8(cpu, dea, value);
    cpu->a[dstAddrReg] = dea + 1u;
    moveBSetFlags(cpu, value);
}

void MOVE_B_piA_indA(M68k* cpu, int srcAddrReg, int dstAddrReg) {
    uint32_t sea = cpu->a[srcAddrReg];
    uint8_t value = read8(cpu, sea);
    cpu->a[srcAddrReg] = sea + 1u;
    write8(cpu, cpu->a[dstAddrReg], value);
    moveBSetFlags(cpu, value);
}

void MOVE_B_dA_dA(M68k* cpu,
                  int32_t srcDisp,
                  int srcBaseAddrReg,
                  int32_t dstDisp,
                  int dstBaseAddrReg) {
    uint32_t sea = cpu->a[srcBaseAddrReg] + (uint32_t)srcDisp;
    uint32_t dea = cpu->a[dstBaseAddrReg] + (uint32_t)dstDisp;
    uint8_t value = read8(cpu, sea);
    write8(cpu, dea, value);
    moveBSetFlags(cpu, value);
}

void MOVE_B_dA_piA(M68k* cpu, int32_t disp, int srcBaseAddrReg, int dstAddrReg) {
    uint32_t sea = cpu->a[srcBaseAddrReg] + (uint32_t)disp;
    uint8_t value = read8(cpu, sea);
    uint32_t dea = cpu->a[dstAddrReg];
    write8(cpu, dea, value);
    cpu->a[dstAddrReg] = dea + 1u;
    moveBSetFlags(cpu, value);
}

void MOVE_B_indA_dA(M68k* cpu, int srcAddrReg, int32_t dstDisp, int dstBaseAddrReg) {
    uint8_t value = read8(cpu, cpu->a[srcAddrReg]);
    uint32_t dea = cpu->a[dstBaseAddrReg] + (uint32_t)dstDisp;
    write8(cpu, dea, value);
    moveBSetFlags(cpu, value);
}

void MOVE_B_piA_dA(M68k* cpu, int srcAddrReg, int32_t disp, int dstBaseAddrReg) {
    uint32_t sea = cpu->a[srcAddrReg];
    uint8_t value = read8(cpu, sea);
    cpu->a[srcAddrReg] = sea + 1u;
    uint32_t dea = cpu->a[dstBaseAddrReg] + (uint32_t)disp;
    write8(cpu, dea, value);
    moveBSetFlags(cpu, value);
}

void MOVE_B_piA_piA(M68k* cpu, int srcAddrReg, int dstAddrReg) {
    uint32_t srcEa = cpu->a[srcAddrReg];
    uint8_t value = read8(cpu, srcEa);
    cpu->a[srcAddrReg] = srcEa + 1u;

    uint32_t dstEa = cpu->a[dstAddrReg];
    write8(cpu, dstEa, value);
    cpu->a[dstAddrReg] = dstEa + 1u;
    moveBSetFlags(cpu, value);
}

void MOVE_B_dAIx_indA(M68k* cpu,
                      int32_t disp,
                      int baseAddrReg,
                      int indexReg,
                      int indexIsAddr,
                      int indexSize, int indexScale,
                      int dstAddrReg) {
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t srcEa = cpu->a[baseAddrReg] + (uint32_t)disp + idx;
    uint8_t value = read8(cpu, srcEa);
    write8(cpu, cpu->a[dstAddrReg], value);
    moveBSetFlags(cpu, value);
}

void MOVE_B_dAIx_abs(M68k* cpu,
                     int32_t disp,
                     int baseAddrReg,
                     int indexReg,
                     int indexIsAddr,
                     int indexSize, int indexScale,
                     uint32_t dstAddress) {
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t srcEa = cpu->a[baseAddrReg] + (uint32_t)disp + idx;
    uint8_t value = read8(cpu, srcEa);
    write8(cpu, dstAddress, value);
    moveBSetFlags(cpu, value);
}

void MOVE_B_D_dAIx(M68k* cpu,
                   int srcDataReg,
                   int32_t disp,
                   int baseAddrReg,
                   int indexReg,
                   int indexIsAddr,
                   int indexSize, int indexScale) {
    uint8_t value = (uint8_t)(cpu->d[srcDataReg] & 0xFFu);
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t dstEa = cpu->a[baseAddrReg] + (uint32_t)disp + idx;
    write8(cpu, dstEa, value);
    moveBSetFlags(cpu, value);
}

void MOVE_L_abs_D(M68k* cpu, uint32_t address, int regIdx) {
    uint32_t value = read32(cpu, address);
    cpu->d[regIdx] = value;
    moveLSetFlags(cpu, value);
}

void MOVE_L_abs_A(M68k* cpu, uint32_t address, int regIdx) {
    uint32_t value = read32(cpu, address);
    cpu->a[regIdx] = value;
    /* MOVEA does not affect condition codes. */
}

void ADD_L_abs_A(M68k* cpu, uint32_t address, int regIdx) {
    uint32_t src = read32(cpu, address);
    cpu->a[regIdx] = cpu->a[regIdx] + src;
    /* ADDA.L does not affect condition codes. */
}

static uint32_t addLCore(M68k* cpu, uint32_t src, uint32_t dst) {
    uint64_t wide = (uint64_t)dst + (uint64_t)src;
    uint32_t result = (uint32_t)wide;
    cpu->n = (result & 0x80000000u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    uint32_t overflowBits = (uint32_t)((~(dst ^ src)) & (dst ^ result));
    cpu->v = (overflowBits & 0x80000000u) ? 1 : 0;
    cpu->c = (wide >> 32) ? 1 : 0;
    cpu->x = cpu->c;
    return result;
}

void ADD_L_imm_D(M68k* cpu, uint32_t value, int dstDataReg) {
    uint32_t dst = cpu->d[dstDataReg];
    uint32_t result = addLCore(cpu, value, dst);
    cpu->d[dstDataReg] = result;
}

void ADD_L_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint32_t src = cpu->d[srcDataReg];
    uint32_t dst = cpu->d[dstDataReg];
    uint32_t result = addLCore(cpu, src, dst);
    cpu->d[dstDataReg] = result;
}

void ADD_L_A_D(M68k* cpu, int srcAddrReg, int dstDataReg) {
    uint32_t src = cpu->a[srcAddrReg];
    uint32_t dst = cpu->d[dstDataReg];
    uint32_t result = addLCore(cpu, src, dst);
    cpu->d[dstDataReg] = result;
}

void ADD_L_D_abs(M68k* cpu, int srcDataReg, uint32_t address) {
    uint32_t m = read32(cpu, address);
    uint32_t src = cpu->d[srcDataReg];
    uint32_t result = addLCore(cpu, src, m);
    write32(cpu, address, result);
}

void ADD_L_D_dA(M68k* cpu, int srcDataReg, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint32_t m = read32(cpu, ea);
    uint32_t src = cpu->d[srcDataReg];
    uint32_t result = addLCore(cpu, src, m);
    write32(cpu, ea, result);
}

void ADD_L_imm_abs(M68k* cpu, uint32_t value, uint32_t address) {
    uint32_t m = read32(cpu, address);
    uint32_t result = addLCore(cpu, value, m);
    write32(cpu, address, result);
}

void ADD_L_imm_dA(M68k* cpu, uint32_t value, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint32_t m = read32(cpu, ea);
    uint32_t result = addLCore(cpu, value, m);
    write32(cpu, ea, result);
}

void ADD_L_imm_piA(M68k* cpu, uint32_t value, int addrReg) {
    uint32_t ea = cpu->a[addrReg];
    uint32_t m = read32(cpu, ea);
    uint32_t result = addLCore(cpu, value, m);
    write32(cpu, ea, result);
    cpu->a[addrReg] = ea + 4u;
}

void ADD_L_D_indA(M68k* cpu, int srcDataReg, int addrReg) {
    uint32_t ea = cpu->a[addrReg];
    uint32_t m = read32(cpu, ea);
    uint32_t src = cpu->d[srcDataReg];
    uint32_t result = addLCore(cpu, src, m);
    write32(cpu, ea, result);
}

void ADD_L_abs_D(M68k* cpu, uint32_t address, int dstDataReg) {
    uint32_t src = read32(cpu, address);
    uint32_t dst = cpu->d[dstDataReg];
    uint32_t result = addLCore(cpu, src, dst);
    cpu->d[dstDataReg] = result;
}

void ADD_L_indA_D(M68k* cpu, int srcAddrReg, int dstDataReg) {
    uint32_t src = read32(cpu, cpu->a[srcAddrReg]);
    uint32_t dst = cpu->d[dstDataReg];
    uint32_t result = addLCore(cpu, src, dst);
    cpu->d[dstDataReg] = result;
}

void ADD_L_piA_D(M68k* cpu, int srcAddrReg, int dstDataReg) {
    uint32_t ea = cpu->a[srcAddrReg];
    uint32_t src = read32(cpu, ea);
    cpu->a[srcAddrReg] = ea + 4u;
    uint32_t dst = cpu->d[dstDataReg];
    uint32_t result = addLCore(cpu, src, dst);
    cpu->d[dstDataReg] = result;
}

void ADD_L_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int dstDataReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint32_t src = read32(cpu, ea);
    uint32_t dst = cpu->d[dstDataReg];
    uint32_t result = addLCore(cpu, src, dst);
    cpu->d[dstDataReg] = result;
}

void ADD_L_dAIx_D(M68k* cpu,
                  int32_t disp,
                  int baseAddrReg,
                  int indexReg,
                  int indexIsAddr,
                  int indexSize, int indexScale,
                  int dstDataReg) {
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp + idx;
    uint32_t src = read32(cpu, ea);
    uint32_t dst = cpu->d[dstDataReg];
    uint32_t result = addLCore(cpu, src, dst);
    cpu->d[dstDataReg] = result;
}

void ADD_L_D_A(M68k* cpu, int srcDataReg, int dstAddrReg) {
    cpu->a[dstAddrReg] = cpu->a[dstAddrReg] + cpu->d[srcDataReg];
    /* ADDA.L does not affect condition codes. */
}

void ADD_L_A_A(M68k* cpu, int srcAddrReg, int dstAddrReg) {
    cpu->a[dstAddrReg] = cpu->a[dstAddrReg] + cpu->a[srcAddrReg];
    /* ADDA.L does not affect condition codes. */
}

void ADD_L_indA_A(M68k* cpu, int srcAddrReg, int dstAddrReg) {
    uint32_t src = read32(cpu, cpu->a[srcAddrReg]);
    cpu->a[dstAddrReg] = cpu->a[dstAddrReg] + src;
    /* ADDA.L does not affect condition codes. */
}

void ADD_L_piA_A(M68k* cpu, int srcAddrReg, int dstAddrReg) {
    uint32_t ea = cpu->a[srcAddrReg];
    uint32_t src = read32(cpu, ea);
    cpu->a[srcAddrReg] = ea + 4u;
    cpu->a[dstAddrReg] = cpu->a[dstAddrReg] + src;
    /* ADDA.L does not affect condition codes. */
}

void ADD_L_dA_A(M68k* cpu, int32_t disp, int baseAddrReg, int dstAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint32_t src = read32(cpu, ea);
    cpu->a[dstAddrReg] = cpu->a[dstAddrReg] + src;
    /* ADDA.L does not affect condition codes. */
}

void ADD_L_dAIx_A(M68k* cpu,
                  int32_t disp,
                  int baseAddrReg,
                  int indexReg,
                  int indexIsAddr,
                  int indexSize, int indexScale,
                  int dstAddrReg) {
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp + idx;
    uint32_t src = read32(cpu, ea);
    cpu->a[dstAddrReg] = cpu->a[dstAddrReg] + src;
    /* ADDA.L does not affect condition codes. */
}

void ADD_L_imm_A(M68k* cpu, uint32_t value, int dstAddrReg) {
    cpu->a[dstAddrReg] = cpu->a[dstAddrReg] + value;
    /* ADDA.L does not affect condition codes. */
}

void SUB_L_imm_A(M68k* cpu, uint32_t value, int regIdx) {
    cpu->a[regIdx] = cpu->a[regIdx] - value;
    /* SUBA.L does not affect condition codes. */
}

void SUBA_L_D_A(M68k* cpu, int srcDataReg, int dstAddrReg) {
    cpu->a[dstAddrReg] = cpu->a[dstAddrReg] - cpu->d[srcDataReg];
    /* SUBA.L does not affect condition codes. */
}

void MOVE_L_abs_abs(M68k* cpu, uint32_t srcAddress, uint32_t dstAddress) {
    uint32_t value = read32(cpu, srcAddress);
    write32(cpu, dstAddress, value);
    moveLSetFlags(cpu, value);
}

void MOVE_L_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int regIdx) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint32_t value = read32(cpu, ea);
    cpu->d[regIdx] = value;
    moveLSetFlags(cpu, value);
}

void MOVE_L_dA_dA(M68k* cpu,
                  int32_t srcDisp,
                  int srcBaseAddrReg,
                  int32_t dstDisp,
                  int dstBaseAddrReg) {
    uint32_t sea = cpu->a[srcBaseAddrReg] + (uint32_t)srcDisp;
    uint32_t dea = cpu->a[dstBaseAddrReg] + (uint32_t)dstDisp;
    uint32_t value = read32(cpu, sea);
    write32(cpu, dea, value);
    moveLSetFlags(cpu, value);
}

void MOVE_L_dAIx_D(M68k* cpu,
                   int32_t disp,
                   int baseAddrReg,
                   int indexReg,
                   int indexIsAddr,
                   int indexSize, int indexScale,
                   int regIdx) {
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp + idx;
    uint32_t value = read32(cpu, ea);
    cpu->d[regIdx] = value;
    moveLSetFlags(cpu, value);
}

void MOVE_L_indA_D(M68k* cpu, int baseAddrReg, int regIdx) {
    uint32_t value = read32(cpu, cpu->a[baseAddrReg]);
    cpu->d[regIdx] = value;
    moveLSetFlags(cpu, value);
}

void MOVE_L_piA_D(M68k* cpu, int baseAddrReg, int regIdx) {
    uint32_t ea = cpu->a[baseAddrReg];
    uint32_t value = read32(cpu, ea);
    cpu->a[baseAddrReg] = ea + 4u;
    cpu->d[regIdx] = value;
    moveLSetFlags(cpu, value);
}

void MOVE_L_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint32_t value = cpu->d[srcDataReg];
    cpu->d[dstDataReg] = value;
    moveLSetFlags(cpu, value);
}

void MOVE_L_D_abs(M68k* cpu, int srcDataReg, uint32_t address) {
    uint32_t value = cpu->d[srcDataReg];
    write32(cpu, address, value);
    moveLSetFlags(cpu, value);
}

void MOVE_L_A_abs(M68k* cpu, int srcAddrReg, uint32_t address) {
    uint32_t value = cpu->a[srcAddrReg];
    write32(cpu, address, value);
    moveLSetFlags(cpu, value);
}

void MOVE_L_A_dA(M68k* cpu, int srcAddrReg, int32_t disp, int baseAddrReg) {
    uint32_t value = cpu->a[srcAddrReg];
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    write32(cpu, ea, value);
    moveLSetFlags(cpu, value);
}

void MOVE_L_D_dA(M68k* cpu, int srcDataReg, int32_t disp, int baseAddrReg) {
    uint32_t value = cpu->d[srcDataReg];
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    write32(cpu, ea, value);
    moveLSetFlags(cpu, value);
}

void MOVE_L_D_indA(M68k* cpu, int srcDataReg, int addrReg) {
    uint32_t value = cpu->d[srcDataReg];
    write32(cpu, cpu->a[addrReg], value);
    moveLSetFlags(cpu, value);
}

void MOVE_L_A_D(M68k* cpu, int srcAddrReg, int dstDataReg) {
    uint32_t value = cpu->a[srcAddrReg];
    cpu->d[dstDataReg] = value;
    moveLSetFlags(cpu, value);
}

void MOVE_L_A_indA(M68k* cpu, int srcAddrReg, int dstAddrReg) {
    uint32_t value = cpu->a[srcAddrReg];
    write32(cpu, cpu->a[dstAddrReg], value);
    moveLSetFlags(cpu, value);
}

void MOVE_L_D_piA(M68k* cpu, int srcDataReg, int addrReg) {
    uint32_t value = cpu->d[srcDataReg];
    uint32_t ea = cpu->a[addrReg];
    write32(cpu, ea, value);
    cpu->a[addrReg] = ea + 4u;
    moveLSetFlags(cpu, value);
}

void MOVE_L_dA_A(M68k* cpu, int32_t disp, int baseAddrReg, int dstAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    cpu->a[dstAddrReg] = read32(cpu, ea);
    /* MOVEA does not affect condition codes. */
}

void MOVE_L_piA_dA(M68k* cpu, int srcAddrReg, int32_t disp, int dstBaseAddrReg) {
    uint32_t sea = cpu->a[srcAddrReg];
    uint32_t value = read32(cpu, sea);
    cpu->a[srcAddrReg] = sea + 4u;
    uint32_t dea = cpu->a[dstBaseAddrReg] + (uint32_t)disp;
    write32(cpu, dea, value);
    moveLSetFlags(cpu, value);
}

void MOVE_L_dA_abs(M68k* cpu, int32_t disp, int baseAddrReg, uint32_t dstAddress) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint32_t value = read32(cpu, ea);
    write32(cpu, dstAddress, value);
    moveLSetFlags(cpu, value);
}

void MOVE_L_piA_abs(M68k* cpu, int srcAddrReg, uint32_t dstAddress) {
    uint32_t sea = cpu->a[srcAddrReg];
    uint32_t value = read32(cpu, sea);
    cpu->a[srcAddrReg] = sea + 4u;
    write32(cpu, dstAddress, value);
    moveLSetFlags(cpu, value);
}

void MOVE_L_piA_piA(M68k* cpu, int srcAddrReg, int dstAddrReg) {
    uint32_t sea = cpu->a[srcAddrReg];
    uint32_t value = read32(cpu, sea);
    cpu->a[srcAddrReg] = sea + 4u;
    uint32_t dea = cpu->a[dstAddrReg];
    write32(cpu, dea, value);
    cpu->a[dstAddrReg] = dea + 4u;
    moveLSetFlags(cpu, value);
}

void MOVE_L_pdA_D(M68k* cpu, int srcAddrReg, int dstDataReg) {
    uint32_t newEa = cpu->a[srcAddrReg] - 4u;
    cpu->a[srcAddrReg] = newEa;
    uint32_t value = read32(cpu, newEa);
    cpu->d[dstDataReg] = value;
    moveLSetFlags(cpu, value);
}

void MOVE_L_dA_indA(M68k* cpu, int32_t disp, int srcBaseAddrReg, int dstAddrReg) {
    uint32_t sea = cpu->a[srcBaseAddrReg] + (uint32_t)disp;
    uint32_t value = read32(cpu, sea);
    write32(cpu, cpu->a[dstAddrReg], value);
    moveLSetFlags(cpu, value);
}

void MOVE_L_indA_indA(M68k* cpu, int srcAddrReg, int dstAddrReg) {
    uint32_t value = read32(cpu, cpu->a[srcAddrReg]);
    write32(cpu, cpu->a[dstAddrReg], value);
    moveLSetFlags(cpu, value);
}

void MOVE_L_indA_abs(M68k* cpu, int srcAddrReg, uint32_t dstAddress) {
    uint32_t value = read32(cpu, cpu->a[srcAddrReg]);
    write32(cpu, dstAddress, value);
    moveLSetFlags(cpu, value);
}

void MOVE_L_indA_dA(M68k* cpu, int srcAddrReg, int32_t dstDisp, int dstBaseAddrReg) {
    uint32_t value = read32(cpu, cpu->a[srcAddrReg]);
    uint32_t dea = cpu->a[dstBaseAddrReg] + (uint32_t)dstDisp;
    write32(cpu, dea, value);
    moveLSetFlags(cpu, value);
}

void MOVE_L_piA_A(M68k* cpu, int srcAddrReg, int dstAddrReg) {
    uint32_t sea = cpu->a[srcAddrReg];
    uint32_t value = read32(cpu, sea);
    cpu->a[srcAddrReg] = sea + 4u;
    cpu->a[dstAddrReg] = value;
    /* MOVEA.L does not affect condition codes. */
}

void MOVE_L_D_A(M68k* cpu, int srcDataReg, int dstAddrReg) {
    cpu->a[dstAddrReg] = cpu->d[srcDataReg];
    /* MOVEA.L does not affect condition codes. */
}

void MOVE_L_A_A(M68k* cpu, int srcAddrReg, int dstAddrReg) {
    cpu->a[dstAddrReg] = cpu->a[srcAddrReg];
    /* MOVEA.L does not affect condition codes. */
}

void MOVE_L_D_pdA(M68k* cpu, int srcDataReg, int dstAddrReg) {
    uint32_t value = cpu->d[srcDataReg];
    uint32_t newEa = cpu->a[dstAddrReg] - 4u;
    cpu->a[dstAddrReg] = newEa;
    write32(cpu, newEa, value);
    moveLSetFlags(cpu, value);
}

void MOVE_L_A_pdA(M68k* cpu, int srcAddrReg, int dstAddrReg) {
    uint32_t value = cpu->a[srcAddrReg];
    uint32_t newEa = cpu->a[dstAddrReg] - 4u;
    cpu->a[dstAddrReg] = newEa;
    write32(cpu, newEa, value);
    moveLSetFlags(cpu, value);
}

void MOVE_L_abs_dA(M68k* cpu, uint32_t srcAddress, int32_t dstDisp, int dstBaseAddrReg) {
    uint32_t value = read32(cpu, srcAddress);
    uint32_t dea = cpu->a[dstBaseAddrReg] + (uint32_t)dstDisp;
    write32(cpu, dea, value);
    moveLSetFlags(cpu, value);
}

void MOVE_L_abs_pdA(M68k* cpu, uint32_t srcAddress, int dstAddrReg) {
    uint32_t value = read32(cpu, srcAddress);
    uint32_t newEa = cpu->a[dstAddrReg] - 4u;
    cpu->a[dstAddrReg] = newEa;
    write32(cpu, newEa, value);
    moveLSetFlags(cpu, value);
}

void MOVE_L_indA_A(M68k* cpu, int srcAddrReg, int dstAddrReg) {
    uint32_t value = read32(cpu, cpu->a[srcAddrReg]);
    cpu->a[dstAddrReg] = value;
    /* MOVEA.L does not affect condition codes. */
}

void MOVE_L_indA_piA(M68k* cpu, int srcAddrReg, int dstAddrReg) {
    uint32_t value = read32(cpu, cpu->a[srcAddrReg]);
    uint32_t dea = cpu->a[dstAddrReg];
    write32(cpu, dea, value);
    cpu->a[dstAddrReg] = dea + 4u;
    moveLSetFlags(cpu, value);
}

void MOVE_L_piA_indA(M68k* cpu, int srcAddrReg, int dstAddrReg) {
    uint32_t sea = cpu->a[srcAddrReg];
    uint32_t value = read32(cpu, sea);
    cpu->a[srcAddrReg] = sea + 4u;
    write32(cpu, cpu->a[dstAddrReg], value);
    moveLSetFlags(cpu, value);
}

void MOVE_L_abs_indA(M68k* cpu, uint32_t srcAddress, int dstAddrReg) {
    uint32_t value = read32(cpu, srcAddress);
    write32(cpu, cpu->a[dstAddrReg], value);
    moveLSetFlags(cpu, value);
}

void MOVE_L_dAIx_dA(M68k* cpu,
                    int32_t srcDisp,
                    int srcBaseAddrReg,
                    int indexReg,
                    int indexIsAddr,
                    int indexSize, int indexScale,
                    int32_t dstDisp,
                    int dstBaseAddrReg) {
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t sea = cpu->a[srcBaseAddrReg] + (uint32_t)srcDisp + idx;
    uint32_t value = read32(cpu, sea);
    uint32_t dea = cpu->a[dstBaseAddrReg] + (uint32_t)dstDisp;
    write32(cpu, dea, value);
    moveLSetFlags(cpu, value);
}

void MOVE_L_dAIx_dAIx(M68k* cpu,
                      int32_t srcDisp,
                      int srcBaseAddrReg,
                      int srcIndexReg,
                      int srcIndexIsAddr,
                      int srcIndexSize, int srcIndexScale,
                      int32_t dstDisp,
                      int dstBaseAddrReg,
                      int dstIndexReg,
                      int dstIndexIsAddr,
                      int dstIndexSize, int dstIndexScale) {
    uint32_t srcIdx = indexExtend(cpu, srcIndexReg, srcIndexIsAddr, srcIndexSize, srcIndexScale);
    uint32_t sea = cpu->a[srcBaseAddrReg] + (uint32_t)srcDisp + srcIdx;
    uint32_t value = read32(cpu, sea);
    uint32_t dstIdx = indexExtend(cpu, dstIndexReg, dstIndexIsAddr, dstIndexSize, dstIndexScale);
    uint32_t dea = cpu->a[dstBaseAddrReg] + (uint32_t)dstDisp + dstIdx;
    write32(cpu, dea, value);
    moveLSetFlags(cpu, value);
}

void MOVE_L_abs_dAIx(M68k* cpu,
                     uint32_t srcAddress,
                     int32_t dstDisp,
                     int dstBaseAddrReg,
                     int indexReg,
                     int indexIsAddr,
                     int indexSize, int indexScale) {
    uint32_t value = read32(cpu, srcAddress);
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t dea = cpu->a[dstBaseAddrReg] + (uint32_t)dstDisp + idx;
    write32(cpu, dea, value);
    moveLSetFlags(cpu, value);
}

void MOVE_L_imm_dAIx(M68k* cpu,
                     uint32_t value,
                     int32_t dstDisp,
                     int dstBaseAddrReg,
                     int indexReg,
                     int indexIsAddr,
                     int indexSize, int indexScale) {
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t dea = cpu->a[dstBaseAddrReg] + (uint32_t)dstDisp + idx;
    write32(cpu, dea, value);
    moveLSetFlags(cpu, value);
}

void MOVE_L_dAIx_abs(M68k* cpu,
                     int32_t srcDisp,
                     int srcBaseAddrReg,
                     int indexReg,
                     int indexIsAddr,
                     int indexSize, int indexScale,
                     uint32_t dstAddress) {
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t sea = cpu->a[srcBaseAddrReg] + (uint32_t)srcDisp + idx;
    uint32_t value = read32(cpu, sea);
    write32(cpu, dstAddress, value);
    moveLSetFlags(cpu, value);
}

void MOVE_L_dAIx_indA(M68k* cpu,
                      int32_t disp,
                      int baseAddrReg,
                      int indexReg,
                      int indexIsAddr,
                      int indexSize, int indexScale,
                      int dstAddrReg) {
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t sea = cpu->a[baseAddrReg] + (uint32_t)disp + idx;
    uint32_t value = read32(cpu, sea);
    write32(cpu, cpu->a[dstAddrReg], value);
    moveLSetFlags(cpu, value);
}

void MOVE_L_abs_piA(M68k* cpu, uint32_t srcAddress, int dstAddrReg) {
    uint32_t value = read32(cpu, srcAddress);
    uint32_t dea = cpu->a[dstAddrReg];
    write32(cpu, dea, value);
    cpu->a[dstAddrReg] = dea + 4u;
    moveLSetFlags(cpu, value);
}

void MOVE_L_dA_piA(M68k* cpu, int32_t disp, int srcBaseAddrReg, int dstAddrReg) {
    uint32_t sea = cpu->a[srcBaseAddrReg] + (uint32_t)disp;
    uint32_t value = read32(cpu, sea);
    uint32_t dea = cpu->a[dstAddrReg];
    write32(cpu, dea, value);
    cpu->a[dstAddrReg] = dea + 4u;
    moveLSetFlags(cpu, value);
}

void MOVE_L_pdA_indA(M68k* cpu, int srcAddrReg, int dstAddrReg) {
    uint32_t newEa = cpu->a[srcAddrReg] - 4u;
    cpu->a[srcAddrReg] = newEa;
    uint32_t value = read32(cpu, newEa);
    write32(cpu, cpu->a[dstAddrReg], value);
    moveLSetFlags(cpu, value);
}

void MOVE_L_D_dAIx(M68k* cpu,
                   int srcDataReg,
                   int32_t disp,
                   int baseAddrReg,
                   int indexReg,
                   int indexIsAddr,
                   int indexSize, int indexScale) {
    uint32_t value = cpu->d[srcDataReg];
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t dstEa = cpu->a[baseAddrReg] + (uint32_t)disp + idx;
    write32(cpu, dstEa, value);
    moveLSetFlags(cpu, value);
}

void MOVE_L_A_piA(M68k* cpu, int srcAddrReg, int dstAddrReg) {
    uint32_t value = cpu->a[srcAddrReg];
    uint32_t dea = cpu->a[dstAddrReg];
    write32(cpu, dea, value);
    cpu->a[dstAddrReg] = dea + 4u;
    moveLSetFlags(cpu, value);
}

void CMP_W_imm_D(M68k* cpu, uint16_t value, int regIdx) {
    uint16_t dst = (uint16_t)(cpu->d[regIdx] & 0xFFFF);
    uint16_t result = (uint16_t)(dst - value);

    cpu->n = (result & 0x8000) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;

    /* Signed overflow for subtraction: dst and value differ in sign,
       and the result's sign differs from dst's sign. */
    uint16_t overflowBits = (uint16_t)((dst ^ value) & (dst ^ result));
    cpu->v = (overflowBits & 0x8000) ? 1 : 0;

    /* Unsigned borrow. */
    cpu->c = (dst < value) ? 1 : 0;

    /* X is not affected by CMP. */
}

static void cmpLCore(M68k* cpu, uint32_t src, uint32_t dst) {
    uint32_t result = dst - src;
    cpu->n = (result & 0x80000000u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    uint32_t overflowBits = (uint32_t)((dst ^ src) & (dst ^ result));
    cpu->v = (overflowBits & 0x80000000u) ? 1 : 0;
    cpu->c = (dst < src) ? 1 : 0;
    /* X is not affected by CMP. */
}

void CMP_L_imm_D(M68k* cpu, uint32_t value, int regIdx) {
    cmpLCore(cpu, value, cpu->d[regIdx]);
}

void CMP_L_imm_A(M68k* cpu, uint32_t value, int regIdx) {
    cmpLCore(cpu, value, cpu->a[regIdx]);
}

void CMP_L_imm_abs(M68k* cpu, uint32_t value, uint32_t address) {
    uint32_t dst = read32(cpu, address);
    cmpLCore(cpu, value, dst);
}

void CMP_L_imm_indA(M68k* cpu, uint32_t value, int baseAddrReg) {
    uint32_t dst = read32(cpu, cpu->a[baseAddrReg]);
    cmpLCore(cpu, value, dst);
}

void CMP_L_imm_dA(M68k* cpu, uint32_t value, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint32_t dst = read32(cpu, ea);
    cmpLCore(cpu, value, dst);
}

void CMP_L_imm_dAIx(M68k* cpu,
                    uint32_t value,
                    int32_t disp,
                    int baseAddrReg,
                    int indexReg,
                    int indexIsAddr,
                    int indexSize, int indexScale) {
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp + idx;
    uint32_t dst = read32(cpu, ea);
    cmpLCore(cpu, value, dst);
}

void CMP_L_imm_piA(M68k* cpu, uint32_t value, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg];
    uint32_t dst = read32(cpu, ea);
    cmpLCore(cpu, value, dst);
    cpu->a[baseAddrReg] = ea + 4u;
}

void CMP_L_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int dstDataReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint32_t src = read32(cpu, ea);
    uint32_t dst = cpu->d[dstDataReg];
    cmpLCore(cpu, src, dst);
}

void CMP_L_abs_D(M68k* cpu, uint32_t address, int dstDataReg) {
    uint32_t src = read32(cpu, address);
    uint32_t dst = cpu->d[dstDataReg];
    cmpLCore(cpu, src, dst);
}

void CMP_L_abs_A(M68k* cpu, uint32_t address, int dstAddrReg) {
    uint32_t src = read32(cpu, address);
    uint32_t dst = cpu->a[dstAddrReg];
    cmpLCore(cpu, src, dst);
}

void CMP_L_indA_D(M68k* cpu, int srcAddrReg, int dstDataReg) {
    uint32_t src = read32(cpu, cpu->a[srcAddrReg]);
    uint32_t dst = cpu->d[dstDataReg];
    cmpLCore(cpu, src, dst);
}

void CMP_L_piA_D(M68k* cpu, int srcAddrReg, int dstDataReg) {
    uint32_t ea = cpu->a[srcAddrReg];
    uint32_t src = read32(cpu, ea);
    cpu->a[srcAddrReg] = ea + 4u;
    uint32_t dst = cpu->d[dstDataReg];
    cmpLCore(cpu, src, dst);
}

void CMP_L_dAIx_D(M68k* cpu,
                  int32_t disp,
                  int baseAddrReg,
                  int indexReg,
                  int indexIsAddr,
                  int indexSize, int indexScale,
                  int dstDataReg) {
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t sea = cpu->a[baseAddrReg] + (uint32_t)disp + idx;
    uint32_t src = read32(cpu, sea);
    uint32_t dst = cpu->d[dstDataReg];
    cmpLCore(cpu, src, dst);
}

void CMP_L_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint32_t src = cpu->d[srcDataReg];
    uint32_t dst = cpu->d[dstDataReg];
    cmpLCore(cpu, src, dst);
}

void CMP_L_A_A(M68k* cpu, int srcAddrReg, int dstAddrReg) {
    uint32_t src = cpu->a[srcAddrReg];
    uint32_t dst = cpu->a[dstAddrReg];
    cmpLCore(cpu, src, dst);
}

static void cmpWCore(M68k* cpu, uint16_t src, uint16_t dst) {
    uint16_t result = (uint16_t)(dst - src);
    cpu->n = (result & 0x8000u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    uint16_t overflowBits = (uint16_t)((dst ^ src) & (dst ^ result));
    cpu->v = (overflowBits & 0x8000u) ? 1 : 0;
    cpu->c = (dst < src) ? 1 : 0;
    /* X is not affected by CMP. */
}

void CMP_W_imm_abs(M68k* cpu, uint16_t value, uint32_t address) {
    uint16_t dst = read16(cpu, address);
    cmpWCore(cpu, value, dst);
}

void CMP_W_imm_indA(M68k* cpu, uint16_t value, int baseAddrReg) {
    uint16_t dst = read16(cpu, cpu->a[baseAddrReg]);
    cmpWCore(cpu, value, dst);
}

void CMP_W_imm_piA(M68k* cpu, uint16_t value, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg];
    uint16_t dst = read16(cpu, ea);
    cpu->a[baseAddrReg] = ea + 2u;
    cmpWCore(cpu, value, dst);
}

void CMP_W_imm_dA(M68k* cpu, uint16_t value, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint16_t dst = read16(cpu, ea);
    cmpWCore(cpu, value, dst);
}

void CMP_W_imm_dAIx(M68k* cpu,
                    uint16_t value,
                    int32_t disp,
                    int baseAddrReg,
                    int indexReg,
                    int indexIsAddr,
                    int indexSize, int indexScale) {
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp + idx;
    uint16_t dst = read16(cpu, ea);
    cmpWCore(cpu, value, dst);
}

void CMP_W_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int dstDataReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint16_t src = read16(cpu, ea);
    uint16_t dst = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    cmpWCore(cpu, src, dst);
}

void CMP_W_abs_D(M68k* cpu, uint32_t address, int dstDataReg) {
    uint16_t src = read16(cpu, address);
    uint16_t dst = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    cmpWCore(cpu, src, dst);
}

void CMP_W_indA_D(M68k* cpu, int srcAddrReg, int dstDataReg) {
    uint16_t src = read16(cpu, cpu->a[srcAddrReg]);
    uint16_t dst = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    cmpWCore(cpu, src, dst);
}

void CMP_W_piA_D(M68k* cpu, int srcAddrReg, int dstDataReg) {
    uint32_t ea = cpu->a[srcAddrReg];
    uint16_t src = read16(cpu, ea);
    cpu->a[srcAddrReg] = ea + 2u;
    uint16_t dst = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    cmpWCore(cpu, src, dst);
}

static void cmpBCore(M68k* cpu, uint8_t src, uint8_t dst) {
    uint8_t result = (uint8_t)(dst - src);
    cpu->n = (result & 0x80u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    uint8_t overflowBits = (uint8_t)((dst ^ src) & (dst ^ result));
    cpu->v = (overflowBits & 0x80u) ? 1 : 0;
    cpu->c = (dst < src) ? 1 : 0;
}

void CMP_B_imm_D(M68k* cpu, uint8_t value, int regIdx) {
    uint8_t dst = (uint8_t)(cpu->d[regIdx] & 0xFFu);
    cmpBCore(cpu, value, dst);
}

void CMP_B_imm_abs(M68k* cpu, uint8_t value, uint32_t address) {
    uint8_t dst = read8(cpu, address);
    cmpBCore(cpu, value, dst);
}

void CMP_B_imm_dA(M68k* cpu, uint8_t value, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint8_t dst = read8(cpu, ea);
    cmpBCore(cpu, value, dst);
}

void CMP_B_imm_indA(M68k* cpu, uint8_t value, int baseAddrReg) {
    uint8_t dst = read8(cpu, cpu->a[baseAddrReg]);
    cmpBCore(cpu, value, dst);
}

void CMP_B_imm_piA(M68k* cpu, uint8_t value, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg];
    uint8_t dst = read8(cpu, ea);
    cpu->a[baseAddrReg] = ea + 1u;
    cmpBCore(cpu, value, dst);
}

void CMP_B_imm_pdA(M68k* cpu, uint8_t value, int baseAddrReg) {
    cpu->a[baseAddrReg] -= 1u;
    uint8_t dst = read8(cpu, cpu->a[baseAddrReg]);
    cmpBCore(cpu, value, dst);
}

void CMP_B_imm_dAIx(M68k* cpu,
                    uint8_t value,
                    int32_t disp,
                    int baseAddrReg,
                    int indexReg,
                    int indexIsAddr,
                    int indexSize, int indexScale) {
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp + idx;
    uint8_t dst = read8(cpu, ea);
    cmpBCore(cpu, value, dst);
}

void CMP_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint8_t src = (uint8_t)(cpu->d[srcDataReg] & 0xFFu);
    uint8_t dst = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    cmpBCore(cpu, src, dst);
}

void CMP_B_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int dstDataReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint8_t src = read8(cpu, ea);
    uint8_t dst = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    cmpBCore(cpu, src, dst);
}

void CMP_B_abs_D(M68k* cpu, uint32_t address, int dstDataReg) {
    uint8_t src = read8(cpu, address);
    uint8_t dst = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    cmpBCore(cpu, src, dst);
}

void CMP_B_indA_D(M68k* cpu, int srcAddrReg, int dstDataReg) {
    uint8_t src = read8(cpu, cpu->a[srcAddrReg]);
    uint8_t dst = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    cmpBCore(cpu, src, dst);
}

void CMP_B_piA_D(M68k* cpu, int srcAddrReg, int dstDataReg) {
    uint32_t ea = cpu->a[srcAddrReg];
    uint8_t src = read8(cpu, ea);
    cpu->a[srcAddrReg] = ea + 1u;
    uint8_t dst = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    cmpBCore(cpu, src, dst);
}

void CMP_B_dAIx_D(M68k* cpu,
                  int32_t disp,
                  int baseAddrReg,
                  int indexReg,
                  int indexIsAddr,
                  int indexSize, int indexScale,
                  int dstDataReg) {
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t sea = cpu->a[baseAddrReg] + (uint32_t)disp + idx;
    uint8_t src = read8(cpu, sea);
    uint8_t dst = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    cmpBCore(cpu, src, dst);
}

void CMP_W_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    /* cmp src,dst on 68k computes (dst - src) and sets flags from
       the result without storing anything. Operand order is the
       single biggest bug magnet in this family. */
    uint16_t src    = (uint16_t)(cpu->d[srcDataReg] & 0xFFFF);
    uint16_t dst    = (uint16_t)(cpu->d[dstDataReg] & 0xFFFF);
    uint16_t result = (uint16_t)(dst - src);

    cpu->n = (result & 0x8000) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;

    uint16_t overflowBits = (uint16_t)((dst ^ src) & (dst ^ result));
    cpu->v = (overflowBits & 0x8000) ? 1 : 0;

    cpu->c = (dst < src) ? 1 : 0;

    /* X is not affected by CMP. */
}

void TST_W_D(M68k* cpu, int regIdx) {
    uint16_t value = (uint16_t)(cpu->d[regIdx] & 0xFFFF);
    cpu->n = (value & 0x8000) ? 1 : 0;
    cpu->z = (value == 0) ? 1 : 0;
    cpu->v = 0;
    cpu->c = 0;
    /* X is not affected by TST. */
}

static void tstWCore(M68k* cpu, uint16_t value) {
    cpu->n = (value & 0x8000u) ? 1 : 0;
    cpu->z = (value == 0) ? 1 : 0;
    cpu->v = 0;
    cpu->c = 0;
    /* X is not affected by TST. */
}

void TST_W_abs(M68k* cpu, uint32_t address) {
    uint16_t value = read16(cpu, address);
    tstWCore(cpu, value);
}

void TST_W_indA(M68k* cpu, int baseAddrReg) {
    uint16_t value = read16(cpu, cpu->a[baseAddrReg]);
    tstWCore(cpu, value);
}

void TST_W_piA(M68k* cpu, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg];
    uint16_t value = read16(cpu, ea);
    cpu->a[baseAddrReg] = ea + 2u;
    tstWCore(cpu, value);
}

void TST_W_dA(M68k* cpu, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint16_t value = read16(cpu, ea);
    tstWCore(cpu, value);
}

void TST_W_dAIx(M68k* cpu,
                int32_t disp,
                int baseAddrReg,
                int indexReg,
                int indexIsAddr,
                int indexSize, int indexScale) {
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp + idx;
    uint16_t value = read16(cpu, ea);
    tstWCore(cpu, value);
}

static void tstBCore(M68k* cpu, uint8_t value) {
    cpu->n = (value & 0x80u) ? 1 : 0;
    cpu->z = (value == 0) ? 1 : 0;
    cpu->v = 0;
    cpu->c = 0;
    /* X is not affected by TST. */
}

void TST_B_D(M68k* cpu, int regIdx) {
    tstBCore(cpu, (uint8_t)(cpu->d[regIdx] & 0xFFu));
}

void TST_B_abs(M68k* cpu, uint32_t address) {
    tstBCore(cpu, read8(cpu, address));
}

void TST_B_indA(M68k* cpu, int baseAddrReg) {
    tstBCore(cpu, read8(cpu, cpu->a[baseAddrReg]));
}

void TST_B_dA(M68k* cpu, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    tstBCore(cpu, read8(cpu, ea));
}

static void tstLCore(M68k* cpu, uint32_t value) {
    cpu->n = (value & 0x80000000u) ? 1 : 0;
    cpu->z = (value == 0) ? 1 : 0;
    cpu->v = 0;
    cpu->c = 0;
    /* X is not affected by TST. */
}

void TST_L_D(M68k* cpu, int regIdx) {
    tstLCore(cpu, cpu->d[regIdx]);
}

void TST_L_abs(M68k* cpu, uint32_t address) {
    tstLCore(cpu, read32(cpu, address));
}

void TST_L_indA(M68k* cpu, int baseAddrReg) {
    tstLCore(cpu, read32(cpu, cpu->a[baseAddrReg]));
}

void TST_L_dA(M68k* cpu, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    tstLCore(cpu, read32(cpu, ea));
}

void TST_L_piA(M68k* cpu, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg];
    tstLCore(cpu, read32(cpu, ea));
    cpu->a[baseAddrReg] = ea + 4u;
}

/*
 * Condition-code predicates.
 *
 * Each function returns 1 iff the named 68k condition is true
 * given the current flag state. Kept as 14+2 leaf functions (rather
 * than one switch) so each is independently call-site-readable and
 * independently testable.
 */

int CC_EQ(M68k* cpu) { return cpu->z ? 1 : 0; }
int CC_NE(M68k* cpu) { return cpu->z ? 0 : 1; }

int CC_CS(M68k* cpu) { return cpu->c ? 1 : 0; }
int CC_LO(M68k* cpu) { return cpu->c ? 1 : 0; }

int CC_CC(M68k* cpu) { return cpu->c ? 0 : 1; }
int CC_HS(M68k* cpu) { return cpu->c ? 0 : 1; }

int CC_MI(M68k* cpu) { return cpu->n ? 1 : 0; }
int CC_PL(M68k* cpu) { return cpu->n ? 0 : 1; }

int CC_VS(M68k* cpu) { return cpu->v ? 1 : 0; }
int CC_VC(M68k* cpu) { return cpu->v ? 0 : 1; }

int CC_GE(M68k* cpu) {
    return (cpu->n == cpu->v) ? 1 : 0;
}

int CC_LT(M68k* cpu) {
    return (cpu->n != cpu->v) ? 1 : 0;
}

int CC_GT(M68k* cpu) {
    int nEqV = (cpu->n == cpu->v) ? 1 : 0;
    int notZ = cpu->z ? 0 : 1;
    return (notZ && nEqV) ? 1 : 0;
}

int CC_LE(M68k* cpu) {
    int nNeV = (cpu->n != cpu->v) ? 1 : 0;
    int zSet = cpu->z ? 1 : 0;
    return (zSet || nNeV) ? 1 : 0;
}

int CC_HI(M68k* cpu) {
    int notC = cpu->c ? 0 : 1;
    int notZ = cpu->z ? 0 : 1;
    return (notC && notZ) ? 1 : 0;
}

int CC_LS(M68k* cpu) {
    int cSet = cpu->c ? 1 : 0;
    int zSet = cpu->z ? 1 : 0;
    return (cSet || zSet) ? 1 : 0;
}

void ADDQ_W_imm_D(M68k* cpu, uint16_t value, int regIdx) {
    uint16_t dst = (uint16_t)(cpu->d[regIdx] & 0xFFFF);
    uint32_t wide = (uint32_t)dst + (uint32_t)value;
    uint16_t result = (uint16_t)wide;

    cpu->d[regIdx] = setLow16(cpu->d[regIdx], result);

    cpu->n = (result & 0x8000) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;

    /* Signed overflow for addition: both operands same sign,
       result sign differs. */
    uint16_t overflowBits = (uint16_t)((~(dst ^ value)) & (dst ^ result));
    cpu->v = (overflowBits & 0x8000) ? 1 : 0;

    cpu->c = (wide & 0x10000u) ? 1 : 0;
    cpu->x = cpu->c;
}

void ADDQ_W_imm_A(M68k* cpu, uint16_t value, int regIdx) {
    cpu->a[regIdx] = cpu->a[regIdx] + (uint32_t)(int16_t)value;
    /* ADDQ to An does not affect condition codes on 68000. */
}

void ADDQ_L_imm_A(M68k* cpu, uint32_t value, int regIdx) {
    cpu->a[regIdx] = cpu->a[regIdx] + value;
    /* ADDQ to An does not affect condition codes on 68000. */
}

void ADDQ_L_imm_D(M68k* cpu, uint32_t value, int regIdx) {
    uint32_t dst = cpu->d[regIdx];
    uint32_t result = addLCore(cpu, value, dst);
    cpu->d[regIdx] = result;
}

void ADDQ_W_imm_abs(M68k* cpu, uint16_t value, uint32_t address) {
    uint16_t dst = read16(cpu, address);
    uint32_t wide = (uint32_t)dst + (uint32_t)value;
    uint16_t result = (uint16_t)wide;
    write16(cpu, address, result);
    cpu->n = (result & 0x8000u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    uint16_t overflowBits = (uint16_t)((~(dst ^ value)) & (dst ^ result));
    cpu->v = (overflowBits & 0x8000u) ? 1 : 0;
    cpu->c = (wide & 0x10000u) ? 1 : 0;
    cpu->x = cpu->c;
}

void ADDQ_W_imm_dA(M68k* cpu, uint16_t value, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    ADDQ_W_imm_abs(cpu, value, ea);
}

void ADDQ_W_imm_indA(M68k* cpu, uint16_t value, int baseAddrReg) {
    ADDQ_W_imm_abs(cpu, value, cpu->a[baseAddrReg]);
}

static uint8_t addBCore(M68k* cpu, uint8_t src, uint8_t dst) {
    uint16_t wide = (uint16_t)dst + (uint16_t)src;
    uint8_t result = (uint8_t)wide;
    cpu->n = (result & 0x80u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    uint8_t overflowBits = (uint8_t)((~(dst ^ src)) & (dst ^ result));
    cpu->v = (overflowBits & 0x80u) ? 1 : 0;
    cpu->c = (wide & 0x100u) ? 1 : 0;
    cpu->x = cpu->c;
    return result;
}

void ADD_B_imm_D(M68k* cpu, uint8_t value, int dstDataReg) {
    uint8_t dst = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t result = addBCore(cpu, value, dst);
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], result);
}

void ADD_B_imm_abs(M68k* cpu, uint8_t value, uint32_t address) {
    uint8_t dst = read8(cpu, address);
    uint8_t result = addBCore(cpu, value, dst);
    write8(cpu, address, result);
}

void ADD_B_imm_dA(M68k* cpu, uint8_t value, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    ADD_B_imm_abs(cpu, value, ea);
}

void ADD_B_imm_indA(M68k* cpu, uint8_t value, int baseAddrReg) {
    ADD_B_imm_abs(cpu, value, cpu->a[baseAddrReg]);
}

void ADD_B_imm_piA(M68k* cpu, uint8_t value, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg];
    uint8_t dst = read8(cpu, ea);
    uint8_t result = addBCore(cpu, value, dst);
    write8(cpu, ea, result);
    cpu->a[baseAddrReg] = ea + 1u;
}

void ADD_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint8_t src = (uint8_t)(cpu->d[srcDataReg] & 0xFFu);
    uint8_t dst = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t result = addBCore(cpu, src, dst);
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], result);
}

void ADD_B_abs_D(M68k* cpu, uint32_t address, int dstDataReg) {
    uint8_t src = read8(cpu, address);
    uint8_t dst = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t result = addBCore(cpu, src, dst);
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], result);
}

void ADD_B_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int dstDataReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint8_t src = read8(cpu, ea);
    uint8_t dst = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t result = addBCore(cpu, src, dst);
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], result);
}

void ADD_B_indA_D(M68k* cpu, int baseAddrReg, int dstDataReg) {
    uint8_t src = read8(cpu, cpu->a[baseAddrReg]);
    uint8_t dst = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t result = addBCore(cpu, src, dst);
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], result);
}

void ADD_B_pdA_D(M68k* cpu, int baseAddrReg, int dstDataReg) {
    uint32_t ea = cpu->a[baseAddrReg] - 1u;
    cpu->a[baseAddrReg] = ea;
    uint8_t src = read8(cpu, ea);
    uint8_t dst = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t result = addBCore(cpu, src, dst);
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], result);
}

void ADD_B_D_dA(M68k* cpu, int srcDataReg, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint8_t m = read8(cpu, ea);
    uint8_t src = (uint8_t)(cpu->d[srcDataReg] & 0xFFu);
    uint8_t result = addBCore(cpu, src, m);
    write8(cpu, ea, result);
}

void ADD_B_D_indA(M68k* cpu, int srcDataReg, int addrReg) {
    uint32_t ea = cpu->a[addrReg];
    uint8_t m = read8(cpu, ea);
    uint8_t src = (uint8_t)(cpu->d[srcDataReg] & 0xFFu);
    uint8_t result = addBCore(cpu, src, m);
    write8(cpu, ea, result);
}

void ADD_B_D_pdA(M68k* cpu, int srcDataReg, int addrReg) {
    uint32_t ea = cpu->a[addrReg] - 1u;
    cpu->a[addrReg] = ea;
    uint8_t m = read8(cpu, ea);
    uint8_t src = (uint8_t)(cpu->d[srcDataReg] & 0xFFu);
    uint8_t result = addBCore(cpu, src, m);
    write8(cpu, ea, result);
}

void ADD_B_D_abs(M68k* cpu, int srcDataReg, uint32_t address) {
    uint8_t m = read8(cpu, address);
    uint8_t src = (uint8_t)(cpu->d[srcDataReg] & 0xFFu);
    uint8_t result = addBCore(cpu, src, m);
    write8(cpu, address, result);
}

static uint16_t addWCore(M68k* cpu, uint16_t src, uint16_t dst) {
    uint32_t wide = (uint32_t)dst + (uint32_t)src;
    uint16_t result = (uint16_t)wide;
    cpu->n = (result & 0x8000u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    uint16_t overflowBits = (uint16_t)((~(dst ^ src)) & (dst ^ result));
    cpu->v = (overflowBits & 0x8000u) ? 1 : 0;
    cpu->c = (wide & 0x10000u) ? 1 : 0;
    cpu->x = cpu->c;
    return result;
}

void ADD_W_imm_D(M68k* cpu, uint16_t value, int dstDataReg) {
    uint16_t dst = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    uint16_t result = addWCore(cpu, value, dst);
    cpu->d[dstDataReg] = setLow16(cpu->d[dstDataReg], result);
}

void ADD_W_imm_abs(M68k* cpu, uint16_t value, uint32_t address) {
    uint16_t dst = read16(cpu, address);
    uint16_t result = addWCore(cpu, value, dst);
    write16(cpu, address, result);
}

void ADD_W_imm_dA(M68k* cpu, uint16_t value, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint16_t dst = read16(cpu, ea);
    uint16_t result = addWCore(cpu, value, dst);
    write16(cpu, ea, result);
}

void ADD_W_imm_indA(M68k* cpu, uint16_t value, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg];
    uint16_t dst = read16(cpu, ea);
    uint16_t result = addWCore(cpu, value, dst);
    write16(cpu, ea, result);
}

void ADD_W_imm_piA(M68k* cpu, uint16_t value, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg];
    uint16_t dst = read16(cpu, ea);
    uint16_t result = addWCore(cpu, value, dst);
    write16(cpu, ea, result);
    cpu->a[baseAddrReg] = ea + 2u;
}

void ADD_W_imm_A(M68k* cpu, uint16_t value, int dstAddrReg) {
    int32_t ext = (int32_t)(int16_t)value;
    cpu->a[dstAddrReg] = cpu->a[dstAddrReg] + (uint32_t)ext;
}

void ADD_W_imm_dAIx(M68k* cpu,
                    uint16_t value,
                    int32_t disp,
                    int baseAddrReg,
                    int indexReg,
                    int indexIsAddr,
                    int indexSize, int indexScale) {
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp + idx;
    uint16_t dst = read16(cpu, ea);
    uint16_t result = addWCore(cpu, value, dst);
    write16(cpu, ea, result);
}

void MOVE_W_D_indA(M68k* cpu, int srcDataReg, int addrReg) {
    uint16_t value = (uint16_t)(cpu->d[srcDataReg] & 0xFFFF);
    write16(cpu, cpu->a[addrReg], value);
    moveWSetFlags(cpu, value);
}

void MOVE_W_D_piA(M68k* cpu, int srcDataReg, int addrReg) {
    uint16_t value = (uint16_t)(cpu->d[srcDataReg] & 0xFFFF);
    uint32_t ea = cpu->a[addrReg];
    write16(cpu, ea, value);
    cpu->a[addrReg] = ea + 2u;
    moveWSetFlags(cpu, value);
}

void ADD_W_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint16_t src = (uint16_t)(cpu->d[srcDataReg] & 0xFFFFu);
    uint16_t dst = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    uint16_t result = addWCore(cpu, src, dst);
    cpu->d[dstDataReg] = setLow16(cpu->d[dstDataReg], result);
}

void ADD_W_D_A(M68k* cpu, int srcDataReg, int dstAddrReg) {
    int32_t ext = (int32_t)(int16_t)(cpu->d[srcDataReg] & 0xFFFFu);
    cpu->a[dstAddrReg] = cpu->a[dstAddrReg] + (uint32_t)ext;
}

void ADD_W_abs_D(M68k* cpu, uint32_t address, int dstDataReg) {
    uint16_t src = read16(cpu, address);
    uint16_t dst = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    uint16_t result = addWCore(cpu, src, dst);
    cpu->d[dstDataReg] = setLow16(cpu->d[dstDataReg], result);
}

void ADD_W_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int dstDataReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint16_t src = read16(cpu, ea);
    uint16_t dst = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    uint16_t result = addWCore(cpu, src, dst);
    cpu->d[dstDataReg] = setLow16(cpu->d[dstDataReg], result);
}

void ADD_W_indA_D(M68k* cpu, int baseAddrReg, int dstDataReg) {
    uint16_t src = read16(cpu, cpu->a[baseAddrReg]);
    uint16_t dst = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    uint16_t result = addWCore(cpu, src, dst);
    cpu->d[dstDataReg] = setLow16(cpu->d[dstDataReg], result);
}

void ADD_W_piA_D(M68k* cpu, int baseAddrReg, int dstDataReg) {
    uint32_t ea = cpu->a[baseAddrReg];
    uint16_t src = read16(cpu, ea);
    cpu->a[baseAddrReg] = ea + 2u;
    uint16_t dst = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    uint16_t result = addWCore(cpu, src, dst);
    cpu->d[dstDataReg] = setLow16(cpu->d[dstDataReg], result);
}

void ADD_W_dAIx_D(M68k* cpu,
                  int32_t disp,
                  int baseAddrReg,
                  int indexReg,
                  int indexIsAddr,
                  int indexSize, int indexScale,
                  int dstDataReg) {
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp + idx;
    uint16_t src = read16(cpu, ea);
    uint16_t dst = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    uint16_t result = addWCore(cpu, src, dst);
    cpu->d[dstDataReg] = setLow16(cpu->d[dstDataReg], result);
}

void ADD_W_D_dA(M68k* cpu, int srcDataReg, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint16_t m = read16(cpu, ea);
    uint16_t src = (uint16_t)(cpu->d[srcDataReg] & 0xFFFFu);
    uint16_t result = addWCore(cpu, src, m);
    write16(cpu, ea, result);
}

void ADD_W_D_indA(M68k* cpu, int srcDataReg, int addrReg) {
    uint32_t ea = cpu->a[addrReg];
    uint16_t m = read16(cpu, ea);
    uint16_t src = (uint16_t)(cpu->d[srcDataReg] & 0xFFFFu);
    uint16_t result = addWCore(cpu, src, m);
    write16(cpu, ea, result);
}

void ADD_W_D_piA(M68k* cpu, int srcDataReg, int addrReg) {
    uint32_t ea = cpu->a[addrReg];
    uint16_t m = read16(cpu, ea);
    cpu->a[addrReg] = ea + 2u;
    uint16_t src = (uint16_t)(cpu->d[srcDataReg] & 0xFFFFu);
    uint16_t result = addWCore(cpu, src, m);
    write16(cpu, ea, result);
}

void ADD_W_D_abs(M68k* cpu, int srcDataReg, uint32_t address) {
    uint16_t m = read16(cpu, address);
    uint16_t src = (uint16_t)(cpu->d[srcDataReg] & 0xFFFFu);
    uint16_t result = addWCore(cpu, src, m);
    write16(cpu, address, result);
}

void MOVE_W_piA_dA(M68k* cpu, int srcAddrReg, int32_t disp, int dstBaseAddrReg) {
    uint32_t sea = cpu->a[srcAddrReg];
    uint16_t value = read16(cpu, sea);
    cpu->a[srcAddrReg] = sea + 2u;
    uint32_t dea = cpu->a[dstBaseAddrReg] + (uint32_t)disp;
    write16(cpu, dea, value);
    moveWSetFlags(cpu, value);
}

void MOVE_W_piA_abs(M68k* cpu, int srcAddrReg, uint32_t dstAddress) {
    uint32_t sea = cpu->a[srcAddrReg];
    uint16_t value = read16(cpu, sea);
    cpu->a[srcAddrReg] = sea + 2u;
    write16(cpu, dstAddress, value);
    moveWSetFlags(cpu, value);
}

void MOVE_W_dAIx_indA(M68k* cpu,
                      int32_t disp,
                      int baseAddrReg,
                      int indexReg,
                      int indexIsAddr,
                      int indexSize, int indexScale,
                      int dstAddrReg) {
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t sea = cpu->a[baseAddrReg] + (uint32_t)disp + idx;
    uint16_t value = read16(cpu, sea);
    write16(cpu, cpu->a[dstAddrReg], value);
    moveWSetFlags(cpu, value);
}

void LEA_imm_A(M68k* cpu, uint32_t value, int aReg) {
    /* LEA does not affect any flags. */
    cpu->a[aReg] = value;
}

void PEA_abs(M68k* cpu, uint32_t address) {
    push32(cpu, address);
}

void LEA_indA_A(M68k* cpu, int srcAddrReg, int dstAddrReg) {
    cpu->a[dstAddrReg] = cpu->a[srcAddrReg];
}

void LEA_dA_A(M68k* cpu, int32_t disp, int baseAddrReg, int dstAddrReg) {
    cpu->a[dstAddrReg] = cpu->a[baseAddrReg] + (uint32_t)disp;
}

void LEA_dAIx_A(M68k* cpu,
                int32_t disp,
                int baseAddrReg,
                int indexReg,
                int indexIsAddr,
                int indexSize, int indexScale,
                int dstAddrReg) {
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    cpu->a[dstAddrReg] = cpu->a[baseAddrReg] + (uint32_t)disp + idx;
}

void SWAP_D(M68k* cpu, int regIdx) {
    uint32_t value = cpu->d[regIdx];
    uint32_t swapped = (value << 16) | (value >> 16);
    cpu->d[regIdx] = swapped;
    cpu->n = (swapped & 0x80000000u) ? 1 : 0;
    cpu->z = (swapped == 0) ? 1 : 0;
    cpu->v = 0;
    cpu->c = 0;
    /* X is not affected by SWAP. */
}

void EXG_DD(M68k* cpu, int reg0, int reg1) {
    uint32_t t = cpu->d[reg0];
    cpu->d[reg0] = cpu->d[reg1];
    cpu->d[reg1] = t;
}

void EXG_AA(M68k* cpu, int reg0, int reg1) {
    uint32_t t = cpu->a[reg0];
    cpu->a[reg0] = cpu->a[reg1];
    cpu->a[reg1] = t;
}

void EXG_D_A(M68k* cpu, int dataReg, int addrReg) {
    uint32_t td = cpu->d[dataReg];
    uint32_t ta = cpu->a[addrReg];
    cpu->d[dataReg] = ta;
    cpu->a[addrReg] = td;
}

void EXT_W_D(M68k* cpu, int regIdx) {
    uint8_t lowByte = (uint8_t)(cpu->d[regIdx] & 0xFFu);
    int16_t extended = (int16_t)(int8_t)lowByte;
    cpu->d[regIdx] = setLow16(cpu->d[regIdx], (uint16_t)extended);
    uint16_t result = (uint16_t)extended;
    cpu->n = (result & 0x8000u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    cpu->v = 0;
    cpu->c = 0;
    /* X is not affected by EXT. */
}

void EXT_L_D(M68k* cpu, int regIdx) {
    uint16_t lowWord = (uint16_t)(cpu->d[regIdx] & 0xFFFFu);
    int32_t extended = (int32_t)(int16_t)lowWord;
    uint32_t result = (uint32_t)extended;
    cpu->d[regIdx] = result;
    cpu->n = (result & 0x80000000u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    cpu->v = 0;
    cpu->c = 0;
    /* X is not affected by EXT. */
}

static uint16_t shiftCount6(uint32_t rawCount) {
    return (uint16_t)(rawCount & 63u);
}

static void shiftSetNzWord(M68k* cpu, uint16_t value) {
    cpu->n = (value & 0x8000u) ? 1 : 0;
    cpu->z = (value == 0) ? 1 : 0;
}

static void shiftSetNzByte(M68k* cpu, uint8_t value) {
    cpu->n = (value & 0x80u) ? 1 : 0;
    cpu->z = (value == 0) ? 1 : 0;
}

static void shiftSetNzLong(M68k* cpu, uint32_t value) {
    cpu->n = (value & 0x80000000u) ? 1 : 0;
    cpu->z = (value == 0) ? 1 : 0;
}

static uint16_t asrWShiftCore(M68k* cpu, uint16_t value, uint16_t count) {
    uint16_t eff = shiftCount6(count);
    uint16_t lastOut = 0;
    if (eff == 0) {
        cpu->c = 0;
        cpu->v = 0;
        shiftSetNzWord(cpu, value);
        return value;
    }
    for (uint16_t i = 0; i < eff; i++) {
        lastOut = (uint16_t)(value & 1u);
        uint16_t msb = (uint16_t)(value & 0x8000u);
        value = (uint16_t)((value >> 1) | msb);
    }
    cpu->c = lastOut ? 1 : 0;
    cpu->x = cpu->c;
    cpu->v = 0;
    shiftSetNzWord(cpu, value);
    return value;
}

static uint16_t aslWShiftCore(M68k* cpu, uint16_t value, uint16_t count) {
    uint16_t eff = shiftCount6(count);
    uint16_t lastOut = 0;
    if (eff == 0) {
        cpu->c = 0;
        cpu->v = 0;
        shiftSetNzWord(cpu, value);
        return value;
    }
    for (uint16_t i = 0; i < eff; i++) {
        lastOut = (uint16_t)((value >> 15) & 1u);
        value = (uint16_t)(value << 1);
    }
    cpu->c = lastOut ? 1 : 0;
    cpu->x = cpu->c;
    cpu->v = 0;
    shiftSetNzWord(cpu, value);
    return value;
}

static uint16_t rorWShiftCore(M68k* cpu, uint16_t value, uint16_t count) {
    uint16_t eff = shiftCount6(count);
    uint16_t lastOut = 0;
    if (eff == 0) {
        cpu->c = 0;
        cpu->v = 0;
        shiftSetNzWord(cpu, value);
        return value;
    }
    for (uint16_t i = 0; i < eff; i++) {
        lastOut = (uint16_t)(value & 1u);
        value = (uint16_t)((value >> 1) | (lastOut << 15));
    }
    cpu->c = lastOut ? 1 : 0;
    cpu->v = 0;
    shiftSetNzWord(cpu, value);
    return value;
}

static uint16_t rolWShiftCore(M68k* cpu, uint16_t value, uint16_t count) {
    uint16_t eff = shiftCount6(count);
    uint16_t lastOut = 0;
    if (eff == 0) {
        cpu->c = 0;
        cpu->v = 0;
        shiftSetNzWord(cpu, value);
        return value;
    }
    for (uint16_t i = 0; i < eff; i++) {
        lastOut = (uint16_t)((value >> 15) & 1u);
        value = (uint16_t)((value << 1) | lastOut);
    }
    cpu->c = lastOut ? 1 : 0;
    cpu->v = 0;
    shiftSetNzWord(cpu, value);
    return value;
}

void ASR_W_imm_D(M68k* cpu, uint16_t count, int dstDataReg) {
    uint16_t value = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    value = asrWShiftCore(cpu, value, count);
    cpu->d[dstDataReg] = setLow16(cpu->d[dstDataReg], value);
}

void ASR_W_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint16_t count = (uint16_t)(cpu->d[srcDataReg] & 0x3Fu);
    ASR_W_imm_D(cpu, count, dstDataReg);
}

void ASR_W_imm_abs(M68k* cpu, uint16_t count, uint32_t dstAddress) {
    uint16_t value = read16(cpu, dstAddress);
    value = asrWShiftCore(cpu, value, count);
    write16(cpu, dstAddress, value);
}

void ASR_W_imm_indA(M68k* cpu, uint16_t count, int dstAddrReg) {
    ASR_W_imm_abs(cpu, count, cpu->a[dstAddrReg]);
}

void ASR_W_imm_dA(M68k* cpu, uint16_t count, int32_t disp, int dstBaseAddrReg) {
    uint32_t ea = cpu->a[dstBaseAddrReg] + (uint32_t)disp;
    ASR_W_imm_abs(cpu, count, ea);
}

void ASR_B_imm_D(M68k* cpu, uint16_t count, int dstDataReg) {
    uint16_t eff = shiftCount6(count);
    uint8_t value = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t lastOut = 0;
    if (eff == 0) {
        cpu->c = 0;
        cpu->v = 0;
        shiftSetNzByte(cpu, value);
        return;
    }
    for (uint16_t i = 0; i < eff; i++) {
        lastOut = (uint8_t)(value & 1u);
        uint8_t msb = (uint8_t)(value & 0x80u);
        value = (uint8_t)((value >> 1) | msb);
    }
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], value);
    cpu->c = lastOut ? 1 : 0;
    cpu->x = cpu->c;
    cpu->v = 0;
    shiftSetNzByte(cpu, value);
}

void ASR_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint16_t count = (uint16_t)(cpu->d[srcDataReg] & 0x3Fu);
    ASR_B_imm_D(cpu, count, dstDataReg);
}

void ASL_B_imm_D(M68k* cpu, uint16_t count, int dstDataReg) {
    uint16_t eff = shiftCount6(count);
    uint8_t value = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t lastOut = 0;
    if (eff == 0) {
        cpu->c = 0;
        cpu->v = 0;
        shiftSetNzByte(cpu, value);
        return;
    }
    for (uint16_t i = 0; i < eff; i++) {
        lastOut = (uint8_t)((value >> 7) & 1u);
        value = (uint8_t)((value << 1) & 0xFFu);
    }
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], value);
    cpu->c = lastOut ? 1 : 0;
    cpu->x = cpu->c;
    cpu->v = 0;
    shiftSetNzByte(cpu, value);
}

void ASL_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint16_t count = (uint16_t)(cpu->d[srcDataReg] & 0x3Fu);
    ASL_B_imm_D(cpu, count, dstDataReg);
}

void LSR_B_imm_D(M68k* cpu, uint16_t count, int dstDataReg) {
    uint16_t eff = shiftCount6(count);
    uint8_t value = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t lastOut = 0;
    if (eff == 0) {
        cpu->c = 0;
        cpu->v = 0;
        shiftSetNzByte(cpu, value);
        return;
    }
    for (uint16_t i = 0; i < eff; i++) {
        lastOut = (uint8_t)(value & 1u);
        value = (uint8_t)(value >> 1);
    }
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], value);
    cpu->c = lastOut ? 1 : 0;
    cpu->x = cpu->c;
    cpu->v = 0;
    shiftSetNzByte(cpu, value);
}

void LSR_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint16_t count = (uint16_t)(cpu->d[srcDataReg] & 0x3Fu);
    LSR_B_imm_D(cpu, count, dstDataReg);
}

void LSL_B_imm_D(M68k* cpu, uint16_t count, int dstDataReg) {
    uint16_t eff = shiftCount6(count);
    uint8_t value = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t lastOut = 0;
    if (eff == 0) {
        cpu->c = 0;
        cpu->v = 0;
        shiftSetNzByte(cpu, value);
        return;
    }
    for (uint16_t i = 0; i < eff; i++) {
        lastOut = (uint8_t)((value >> 7) & 1u);
        value = (uint8_t)((value << 1) & 0xFFu);
    }
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], value);
    cpu->c = lastOut ? 1 : 0;
    cpu->x = cpu->c;
    cpu->v = 0;
    shiftSetNzByte(cpu, value);
}

void LSL_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint16_t count = (uint16_t)(cpu->d[srcDataReg] & 0x3Fu);
    LSL_B_imm_D(cpu, count, dstDataReg);
}

static uint8_t rorBShiftCore(M68k* cpu, uint8_t value, uint16_t count) {
    uint16_t eff = shiftCount6(count);
    uint8_t lastOut = 0;
    if (eff == 0) {
        cpu->c = 0;
        cpu->v = 0;
        shiftSetNzByte(cpu, value);
        return value;
    }
    for (uint16_t i = 0; i < eff; i++) {
        lastOut = (uint8_t)(value & 1u);
        value = (uint8_t)((value >> 1) | (lastOut << 7));
    }
    cpu->c = lastOut ? 1 : 0;
    cpu->v = 0;
    shiftSetNzByte(cpu, value);
    return value;
}

static uint8_t rolBShiftCore(M68k* cpu, uint8_t value, uint16_t count) {
    uint16_t eff = shiftCount6(count);
    uint8_t lastOut = 0;
    if (eff == 0) {
        cpu->c = 0;
        cpu->v = 0;
        shiftSetNzByte(cpu, value);
        return value;
    }
    for (uint16_t i = 0; i < eff; i++) {
        lastOut = (uint8_t)((value >> 7) & 1u);
        value = (uint8_t)((value << 1) | lastOut);
    }
    cpu->c = lastOut ? 1 : 0;
    cpu->v = 0;
    shiftSetNzByte(cpu, value);
    return value;
}

void ROR_B_imm_D(M68k* cpu, uint16_t count, int dstDataReg) {
    uint8_t value = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    value = rorBShiftCore(cpu, value, count);
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], value);
}

void ROR_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint16_t count = (uint16_t)(cpu->d[srcDataReg] & 0x3Fu);
    ROR_B_imm_D(cpu, count, dstDataReg);
}

void ROL_B_imm_D(M68k* cpu, uint16_t count, int dstDataReg) {
    uint8_t value = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    value = rolBShiftCore(cpu, value, count);
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], value);
}

void ROL_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint16_t count = (uint16_t)(cpu->d[srcDataReg] & 0x3Fu);
    ROL_B_imm_D(cpu, count, dstDataReg);
}

void ASR_L_imm_D(M68k* cpu, uint16_t count, int dstDataReg) {
    uint16_t eff = shiftCount6(count);
    uint32_t value = cpu->d[dstDataReg];
    uint32_t lastOut = 0;
    if (eff == 0) {
        cpu->c = 0;
        cpu->v = 0;
        shiftSetNzLong(cpu, value);
        return;
    }
    for (uint16_t i = 0; i < eff; i++) {
        lastOut = value & 1u;
        uint32_t msb = value & 0x80000000u;
        value = (value >> 1) | msb;
    }
    cpu->d[dstDataReg] = value;
    cpu->c = lastOut ? 1 : 0;
    cpu->x = cpu->c;
    cpu->v = 0;
    shiftSetNzLong(cpu, value);
}

void ASL_L_imm_D(M68k* cpu, uint16_t count, int dstDataReg) {
    uint16_t eff = shiftCount6(count);
    uint32_t value = cpu->d[dstDataReg];
    uint32_t lastOut = 0;
    if (eff == 0) {
        cpu->c = 0;
        cpu->v = 0;
        shiftSetNzLong(cpu, value);
        return;
    }
    for (uint16_t i = 0; i < eff; i++) {
        lastOut = (value >> 31) & 1u;
        value = value << 1;
    }
    cpu->d[dstDataReg] = value;
    cpu->c = lastOut ? 1 : 0;
    cpu->x = cpu->c;
    cpu->v = 0;
    shiftSetNzLong(cpu, value);
}

void LSR_L_imm_D(M68k* cpu, uint16_t count, int dstDataReg) {
    uint16_t eff = shiftCount6(count);
    uint32_t value = cpu->d[dstDataReg];
    uint32_t lastOut = 0;
    if (eff == 0) {
        cpu->c = 0;
        cpu->v = 0;
        shiftSetNzLong(cpu, value);
        return;
    }
    for (uint16_t i = 0; i < eff; i++) {
        lastOut = value & 1u;
        value = value >> 1;
    }
    cpu->d[dstDataReg] = value;
    cpu->c = lastOut ? 1 : 0;
    cpu->x = cpu->c;
    cpu->v = 0;
    shiftSetNzLong(cpu, value);
}

void LSL_L_imm_D(M68k* cpu, uint16_t count, int dstDataReg) {
    ASL_L_imm_D(cpu, count, dstDataReg);
}

void ASL_W_imm_D(M68k* cpu, uint16_t count, int dstDataReg) {
    uint16_t value = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    value = aslWShiftCore(cpu, value, count);
    cpu->d[dstDataReg] = setLow16(cpu->d[dstDataReg], value);
}

void ASL_W_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint16_t count = (uint16_t)(cpu->d[srcDataReg] & 0x3Fu);
    ASL_W_imm_D(cpu, count, dstDataReg);
}

void ASL_W_imm_abs(M68k* cpu, uint16_t count, uint32_t dstAddress) {
    uint16_t value = read16(cpu, dstAddress);
    value = aslWShiftCore(cpu, value, count);
    write16(cpu, dstAddress, value);
}

void ASL_W_imm_indA(M68k* cpu, uint16_t count, int dstAddrReg) {
    ASL_W_imm_abs(cpu, count, cpu->a[dstAddrReg]);
}

void ASL_W_imm_dA(M68k* cpu, uint16_t count, int32_t disp, int dstBaseAddrReg) {
    uint32_t ea = cpu->a[dstBaseAddrReg] + (uint32_t)disp;
    ASL_W_imm_abs(cpu, count, ea);
}

void LSR_W_imm_D(M68k* cpu, uint16_t count, int dstDataReg) {
    uint16_t eff = shiftCount6(count);
    uint16_t value = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    uint16_t lastOut = 0;
    if (eff == 0) {
        cpu->c = 0;
        cpu->v = 0;
        shiftSetNzWord(cpu, value);
        return;
    }
    for (uint16_t i = 0; i < eff; i++) {
        lastOut = (uint16_t)(value & 1u);
        value = (uint16_t)(value >> 1);
    }
    cpu->d[dstDataReg] = setLow16(cpu->d[dstDataReg], value);
    cpu->c = lastOut ? 1 : 0;
    cpu->x = cpu->c;
    cpu->v = 0;
    shiftSetNzWord(cpu, value);
}

void LSR_W_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint16_t count = (uint16_t)(cpu->d[srcDataReg] & 0x3Fu);
    LSR_W_imm_D(cpu, count, dstDataReg);
}

void LSL_W_imm_D(M68k* cpu, uint16_t count, int dstDataReg) {
    uint16_t eff = shiftCount6(count);
    uint16_t value = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    uint16_t lastOut = 0;
    if (eff == 0) {
        cpu->c = 0;
        cpu->v = 0;
        shiftSetNzWord(cpu, value);
        return;
    }
    for (uint16_t i = 0; i < eff; i++) {
        lastOut = (uint16_t)((value >> 15) & 1u);
        value = (uint16_t)(value << 1);
    }
    cpu->d[dstDataReg] = setLow16(cpu->d[dstDataReg], value);
    cpu->c = lastOut ? 1 : 0;
    cpu->x = cpu->c;
    cpu->v = 0;
    shiftSetNzWord(cpu, value);
}

void LSL_W_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint16_t count = (uint16_t)(cpu->d[srcDataReg] & 0x3Fu);
    LSL_W_imm_D(cpu, count, dstDataReg);
}

void ROR_W_imm_D(M68k* cpu, uint16_t count, int dstDataReg) {
    uint16_t value = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    value = rorWShiftCore(cpu, value, count);
    cpu->d[dstDataReg] = setLow16(cpu->d[dstDataReg], value);
}

void ROR_W_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint16_t count = (uint16_t)(cpu->d[srcDataReg] & 0x3Fu);
    ROR_W_imm_D(cpu, count, dstDataReg);
}

void ROL_W_imm_D(M68k* cpu, uint16_t count, int dstDataReg) {
    uint16_t value = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    value = rolWShiftCore(cpu, value, count);
    cpu->d[dstDataReg] = setLow16(cpu->d[dstDataReg], value);
}

void ROL_W_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint16_t count = (uint16_t)(cpu->d[srcDataReg] & 0x3Fu);
    ROL_W_imm_D(cpu, count, dstDataReg);
}

void ROR_W_imm_abs(M68k* cpu, uint16_t count, uint32_t dstAddress) {
    uint16_t value = read16(cpu, dstAddress);
    value = rorWShiftCore(cpu, value, count);
    write16(cpu, dstAddress, value);
}

void ROR_W_imm_indA(M68k* cpu, uint16_t count, int dstAddrReg) {
    ROR_W_imm_abs(cpu, count, cpu->a[dstAddrReg]);
}

void ROR_W_imm_dA(M68k* cpu, uint16_t count, int32_t disp, int dstBaseAddrReg) {
    uint32_t ea = cpu->a[dstBaseAddrReg] + (uint32_t)disp;
    ROR_W_imm_abs(cpu, count, ea);
}

void ROL_W_imm_abs(M68k* cpu, uint16_t count, uint32_t dstAddress) {
    uint16_t value = read16(cpu, dstAddress);
    value = rolWShiftCore(cpu, value, count);
    write16(cpu, dstAddress, value);
}

void ROL_W_imm_indA(M68k* cpu, uint16_t count, int dstAddrReg) {
    ROL_W_imm_abs(cpu, count, cpu->a[dstAddrReg]);
}

void ROL_W_imm_dA(M68k* cpu, uint16_t count, int32_t disp, int dstBaseAddrReg) {
    uint32_t ea = cpu->a[dstBaseAddrReg] + (uint32_t)disp;
    ROL_W_imm_abs(cpu, count, ea);
}

void ROR_L_imm_D(M68k* cpu, uint16_t count, int dstDataReg) {
    uint16_t eff = shiftCount6(count);
    uint32_t value = cpu->d[dstDataReg];
    uint32_t lastOut = 0;
    if (eff == 0) {
        cpu->c = 0;
        cpu->v = 0;
        shiftSetNzLong(cpu, value);
        return;
    }
    for (uint16_t i = 0; i < eff; i++) {
        lastOut = value & 1u;
        value = (value >> 1) | (lastOut << 31);
    }
    cpu->d[dstDataReg] = value;
    cpu->c = lastOut ? 1 : 0;
    cpu->x = cpu->c;
    cpu->v = 0;
    shiftSetNzLong(cpu, value);
}

void ROR_L_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint16_t count = (uint16_t)(cpu->d[srcDataReg] & 0x3Fu);
    ROR_L_imm_D(cpu, count, dstDataReg);
}

void ROL_L_imm_D(M68k* cpu, uint16_t count, int dstDataReg) {
    uint16_t eff = shiftCount6(count);
    uint32_t value = cpu->d[dstDataReg];
    uint32_t lastOut = 0;
    if (eff == 0) {
        cpu->c = 0;
        cpu->v = 0;
        shiftSetNzLong(cpu, value);
        return;
    }
    for (uint16_t i = 0; i < eff; i++) {
        lastOut = (value >> 31) & 1u;
        value = (value << 1) | lastOut;
    }
    cpu->d[dstDataReg] = value;
    cpu->c = lastOut ? 1 : 0;
    cpu->x = cpu->c;
    cpu->v = 0;
    shiftSetNzLong(cpu, value);
}

void ROL_L_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint16_t count = (uint16_t)(cpu->d[srcDataReg] & 0x3Fu);
    ROL_L_imm_D(cpu, count, dstDataReg);
}

void ROXL_B_imm_D(M68k* cpu, uint16_t count, int dstDataReg) {
    uint16_t eff = shiftCount6(count);
    uint8_t value = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t lastOut = 0;
    if (eff == 0) {
        cpu->c = 0;
        cpu->v = 0;
        shiftSetNzByte(cpu, value);
        return;
    }
    uint8_t xin = cpu->x ? (uint8_t)1u : (uint8_t)0u;
    for (uint16_t i = 0; i < eff; i++) {
        lastOut = (uint8_t)((value >> 7) & 1u);
        value = (uint8_t)(((value << 1) & 0xFFu) | xin);
        xin = lastOut;
    }
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], value);
    cpu->c = lastOut ? 1 : 0;
    cpu->x = cpu->c;
    cpu->v = 0;
    shiftSetNzByte(cpu, value);
}

void ROXL_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint16_t count = (uint16_t)(cpu->d[srcDataReg] & 0x3Fu);
    ROXL_B_imm_D(cpu, count, dstDataReg);
}

void ROXL_L_imm_D(M68k* cpu, uint16_t count, int dstDataReg) {
    uint16_t eff = shiftCount6(count);
    uint32_t value = cpu->d[dstDataReg];
    uint32_t lastOut = 0;
    if (eff == 0) {
        cpu->c = 0;
        cpu->v = 0;
        shiftSetNzLong(cpu, value);
        return;
    }
    uint32_t xin = cpu->x ? 1u : 0u;
    for (uint16_t i = 0; i < eff; i++) {
        lastOut = (value >> 31) & 1u;
        value = (value << 1) | xin;
        xin = lastOut;
    }
    cpu->d[dstDataReg] = value;
    cpu->c = lastOut ? 1 : 0;
    cpu->x = cpu->c;
    cpu->v = 0;
    shiftSetNzLong(cpu, value);
}

void ROXL_L_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint16_t count = (uint16_t)(cpu->d[srcDataReg] & 0x3Fu);
    ROXL_L_imm_D(cpu, count, dstDataReg);
}

void ROXR_B_imm_D(M68k* cpu, uint16_t count, int dstDataReg) {
    uint16_t eff = shiftCount6(count);
    uint8_t value = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t lastOut = 0;
    if (eff == 0) {
        cpu->c = 0;
        cpu->v = 0;
        shiftSetNzByte(cpu, value);
        return;
    }
    uint8_t xin = cpu->x ? (uint8_t)1u : (uint8_t)0u;
    for (uint16_t i = 0; i < eff; i++) {
        lastOut = (uint8_t)(value & 1u);
        value = (uint8_t)((value >> 1) | (uint8_t)(xin << 7));
        xin = lastOut;
    }
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], value);
    cpu->c = lastOut ? 1 : 0;
    cpu->x = cpu->c;
    cpu->v = 0;
    shiftSetNzByte(cpu, value);
}

void ROXR_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint16_t count = (uint16_t)(cpu->d[srcDataReg] & 0x3Fu);
    ROXR_B_imm_D(cpu, count, dstDataReg);
}

void ROXR_L_imm_D(M68k* cpu, uint16_t count, int dstDataReg) {
    uint16_t eff = shiftCount6(count);
    uint32_t value = cpu->d[dstDataReg];
    uint32_t lastOut = 0;
    if (eff == 0) {
        cpu->c = 0;
        cpu->v = 0;
        shiftSetNzLong(cpu, value);
        return;
    }
    uint32_t xin = cpu->x ? 1u : 0u;
    for (uint16_t i = 0; i < eff; i++) {
        lastOut = value & 1u;
        value = (value >> 1) | (xin << 31);
        xin = lastOut;
    }
    cpu->d[dstDataReg] = value;
    cpu->c = lastOut ? 1 : 0;
    cpu->x = cpu->c;
    cpu->v = 0;
    shiftSetNzLong(cpu, value);
}

void ROXR_L_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint16_t count = (uint16_t)(cpu->d[srcDataReg] & 0x3Fu);
    ROXR_L_imm_D(cpu, count, dstDataReg);
}

void MOVEM_L_rlist_pdA(M68k* cpu, uint16_t rlistMask, int baseAddrReg) {
    /* rlistMask bit mapping for save direction:
       bits 15..8 => A7..A0, bits 7..0 => D7..D0.
       Walk high-to-low so visitation is A7..A0,D7..D0. */
    for (int bit = 15; bit >= 0; bit--) {
        if (((rlistMask >> bit) & 1u) == 0u) continue;
        uint32_t value;
        if (bit >= 8) {
            int reg = bit - 8;
            value = cpu->a[reg];
        } else {
            int reg = bit;
            value = cpu->d[reg];
        }
        cpu->a[baseAddrReg] -= 4u;
        write32(cpu, cpu->a[baseAddrReg], value);
    }
}

void MOVEM_L_rlist_dA(M68k* cpu, uint16_t rlistMask, int32_t disp, int baseAddrReg) {
    /* Register-to-memory at fixed EA: D0..D7, A0..A7 (low to high mask bits). */
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    for (int bit = 0; bit <= 15; bit++) {
        if (((rlistMask >> bit) & 1u) == 0u) continue;
        uint32_t value;
        if (bit >= 8) {
            int reg = bit - 8;
            value = cpu->a[reg];
        } else {
            int reg = bit;
            value = cpu->d[reg];
        }
        write32(cpu, ea, value);
        ea += 4u;
    }
}

void MOVEM_L_piA_rlist(M68k* cpu, int baseAddrReg, uint16_t rlistMask) {
    /* rlistMask bit mapping for load direction:
       bits 0..7 => D0..D7, bits 8..15 => A0..A7.
       Walk low-to-high so visitation is D0..D7,A0..A7. */
    for (int bit = 0; bit <= 15; bit++) {
        if (((rlistMask >> bit) & 1u) == 0u) continue;
        uint32_t value = read32(cpu, cpu->a[baseAddrReg]);
        cpu->a[baseAddrReg] += 4u;
        if (bit >= 8) {
            int reg = bit - 8;
            cpu->a[reg] = value;
        } else {
            int reg = bit;
            cpu->d[reg] = value;
        }
    }
}

void MOVEM_W_rlist_pdA(M68k* cpu, uint16_t rlistMask, int baseAddrReg) {
    for (int bit = 15; bit >= 0; bit--) {
        if (((rlistMask >> bit) & 1u) == 0u) continue;
        uint16_t half;
        if (bit >= 8) {
            int reg = bit - 8;
            half = (uint16_t)(cpu->a[reg] & 0xFFFFu);
        } else {
            int reg = bit;
            half = (uint16_t)(cpu->d[reg] & 0xFFFFu);
        }
        cpu->a[baseAddrReg] -= 2u;
        write16(cpu, cpu->a[baseAddrReg], half);
    }
}

void MOVEM_W_rlist_dA(M68k* cpu, uint16_t rlistMask, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    for (int bit = 0; bit <= 15; bit++) {
        if (((rlistMask >> bit) & 1u) == 0u) continue;
        uint16_t half;
        if (bit >= 8) {
            int reg = bit - 8;
            half = (uint16_t)(cpu->a[reg] & 0xFFFFu);
        } else {
            int reg = bit;
            half = (uint16_t)(cpu->d[reg] & 0xFFFFu);
        }
        write16(cpu, ea, half);
        ea += 2u;
    }
}

void MOVEM_W_piA_rlist(M68k* cpu, int baseAddrReg, uint16_t rlistMask) {
    for (int bit = 0; bit <= 15; bit++) {
        if (((rlistMask >> bit) & 1u) == 0u) continue;
        uint16_t w = read16(cpu, cpu->a[baseAddrReg]);
        cpu->a[baseAddrReg] += 2u;
        uint32_t ext = (uint32_t)(int32_t)(int16_t)w;
        if (bit >= 8) {
            int reg = bit - 8;
            cpu->a[reg] = ext;
        } else {
            int reg = bit;
            cpu->d[reg] = ext;
        }
    }
}

void RTE(M68k* cpu) {
    uint16_t sr = pop16(cpu);
    uint32_t pc = pop32(cpu);
    uint8_t ccr = (uint8_t)(sr & 0xFFu);
    cpu->c = ccr & 1u;
    cpu->v = (ccr >> 1) & 1u;
    cpu->z = (ccr >> 2) & 1u;
    cpu->n = (ccr >> 3) & 1u;
    cpu->x = (ccr >> 4) & 1u;
    cpu->pcVirtual = pc;
}

static void mulSetFlags(M68k* cpu, uint32_t result) {
    cpu->n = (result & 0x80000000u) ? 1 : 0;
    cpu->z = (result == 0u) ? 1 : 0;
    cpu->v = 0;
    cpu->c = 0;
    /* X is not affected by MUL. */
}

void MULU_W_imm_D(M68k* cpu, uint16_t value, int dstDataReg) {
    uint16_t dst = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    uint32_t result = (uint32_t)dst * (uint32_t)value;
    cpu->d[dstDataReg] = result;
    mulSetFlags(cpu, result);
}

void MULU_W_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    MULU_W_imm_D(cpu, (uint16_t)(cpu->d[srcDataReg] & 0xFFFFu), dstDataReg);
}

void MULU_W_abs_D(M68k* cpu, uint32_t address, int dstDataReg) {
    MULU_W_imm_D(cpu, read16(cpu, address), dstDataReg);
}

void MULU_W_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int dstDataReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    MULU_W_imm_D(cpu, read16(cpu, ea), dstDataReg);
}

void MULU_W_indA_D(M68k* cpu, int baseAddrReg, int dstDataReg) {
    MULU_W_imm_D(cpu, read16(cpu, cpu->a[baseAddrReg]), dstDataReg);
}

void MULU_W_piA_D(M68k* cpu, int baseAddrReg, int dstDataReg) {
    uint32_t ea = cpu->a[baseAddrReg];
    MULU_W_imm_D(cpu, read16(cpu, ea), dstDataReg);
    cpu->a[baseAddrReg] = ea + 2u;
}

void MULS_W_imm_D(M68k* cpu, uint16_t value, int dstDataReg) {
    int16_t srcS = (int16_t)value;
    int16_t dstS = (int16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    int32_t resultS = (int32_t)dstS * (int32_t)srcS;
    uint32_t result = (uint32_t)resultS;
    cpu->d[dstDataReg] = result;
    mulSetFlags(cpu, result);
}

void MULS_W_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    MULS_W_imm_D(cpu, (uint16_t)(cpu->d[srcDataReg] & 0xFFFFu), dstDataReg);
}

void MULS_W_abs_D(M68k* cpu, uint32_t address, int dstDataReg) {
    MULS_W_imm_D(cpu, read16(cpu, address), dstDataReg);
}

void MULS_W_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int dstDataReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    MULS_W_imm_D(cpu, read16(cpu, ea), dstDataReg);
}

void MULS_W_indA_D(M68k* cpu, int baseAddrReg, int dstDataReg) {
    MULS_W_imm_D(cpu, read16(cpu, cpu->a[baseAddrReg]), dstDataReg);
}

void MULS_W_piA_D(M68k* cpu, int baseAddrReg, int dstDataReg) {
    uint32_t ea = cpu->a[baseAddrReg];
    MULS_W_imm_D(cpu, read16(cpu, ea), dstDataReg);
    cpu->a[baseAddrReg] = ea + 2u;
}

static void divSetOverflow(M68k* cpu) {
    cpu->v = 1;
    cpu->c = 0;
    /* X unaffected, N/Z undefined on real 68k overflow; keep as-is. */
}

void DIVU_W_imm_D(M68k* cpu, uint16_t value, int dstDataReg) {
    if (value == 0u) {
        divSetOverflow(cpu);
        return;
    }
    uint32_t dividend = cpu->d[dstDataReg];
    uint32_t quotient = dividend / (uint32_t)value;
    uint32_t remainder = dividend % (uint32_t)value;
    if (quotient > 0xFFFFu) {
        divSetOverflow(cpu);
        return;
    }
    uint32_t packed = ((remainder & 0xFFFFu) << 16) | (quotient & 0xFFFFu);
    cpu->d[dstDataReg] = packed;
    cpu->n = (quotient & 0x8000u) ? 1 : 0;
    cpu->z = (quotient == 0u) ? 1 : 0;
    cpu->v = 0;
    cpu->c = 0;
    /* X unaffected. */
}

void DIVU_W_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    DIVU_W_imm_D(cpu, (uint16_t)(cpu->d[srcDataReg] & 0xFFFFu), dstDataReg);
}

void DIVU_W_abs_D(M68k* cpu, uint32_t address, int dstDataReg) {
    DIVU_W_imm_D(cpu, read16(cpu, address), dstDataReg);
}

void DIVU_W_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int dstDataReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    DIVU_W_imm_D(cpu, read16(cpu, ea), dstDataReg);
}

void DIVU_W_indA_D(M68k* cpu, int baseAddrReg, int dstDataReg) {
    DIVU_W_imm_D(cpu, read16(cpu, cpu->a[baseAddrReg]), dstDataReg);
}

void DIVS_W_imm_D(M68k* cpu, uint16_t value, int dstDataReg) {
    int16_t divisor = (int16_t)value;
    if (divisor == 0) {
        divSetOverflow(cpu);
        return;
    }
    int32_t dividend = (int32_t)cpu->d[dstDataReg];
    int32_t quotient = dividend / divisor;
    int32_t remainder = dividend % divisor;
    if (quotient < -32768 || quotient > 32767) {
        divSetOverflow(cpu);
        return;
    }
    uint32_t packed = (((uint32_t)(uint16_t)remainder) << 16) |
                      ((uint32_t)(uint16_t)quotient);
    cpu->d[dstDataReg] = packed;
    cpu->n = (quotient < 0) ? 1 : 0;
    cpu->z = (quotient == 0) ? 1 : 0;
    cpu->v = 0;
    cpu->c = 0;
    /* X unaffected. */
}

void DIVS_W_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    DIVS_W_imm_D(cpu, (uint16_t)(cpu->d[srcDataReg] & 0xFFFFu), dstDataReg);
}

void DIVS_W_abs_D(M68k* cpu, uint32_t address, int dstDataReg) {
    DIVS_W_imm_D(cpu, read16(cpu, address), dstDataReg);
}

void DIVS_W_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int dstDataReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    DIVS_W_imm_D(cpu, read16(cpu, ea), dstDataReg);
}

void DIVS_W_indA_D(M68k* cpu, int baseAddrReg, int dstDataReg) {
    DIVS_W_imm_D(cpu, read16(cpu, cpu->a[baseAddrReg]), dstDataReg);
}

void SUBQ_W_imm_D(M68k* cpu, uint16_t value, int regIdx) {
    uint16_t dst = (uint16_t)(cpu->d[regIdx] & 0xFFFF);
    uint16_t result = (uint16_t)(dst - value);

    cpu->d[regIdx] = setLow16(cpu->d[regIdx], result);

    cpu->n = (result & 0x8000) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;

    uint16_t overflowBits = (uint16_t)((dst ^ value) & (dst ^ result));
    cpu->v = (overflowBits & 0x8000) ? 1 : 0;

    cpu->c = (dst < value) ? 1 : 0;
    cpu->x = cpu->c;
}

void SUBQ_W_imm_A(M68k* cpu, uint16_t value, int regIdx) {
    cpu->a[regIdx] = cpu->a[regIdx] - (uint32_t)(int16_t)value;
    /* SUBQ to An does not affect condition codes on 68000. */
}

void SUBQ_L_imm_A(M68k* cpu, uint32_t value, int regIdx) {
    cpu->a[regIdx] = cpu->a[regIdx] - value;
    /* SUBQ to An does not affect condition codes on 68000. */
}

void SUBQ_W_imm_abs(M68k* cpu, uint16_t value, uint32_t address) {
    uint16_t dst = read16(cpu, address);
    uint16_t result = (uint16_t)(dst - value);
    write16(cpu, address, result);
    cpu->n = (result & 0x8000u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    uint16_t overflowBits = (uint16_t)((dst ^ value) & (dst ^ result));
    cpu->v = (overflowBits & 0x8000u) ? 1 : 0;
    cpu->c = (dst < value) ? 1 : 0;
    cpu->x = cpu->c;
}

void SUBQ_W_imm_dA(M68k* cpu, uint16_t value, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    SUBQ_W_imm_abs(cpu, value, ea);
}

void SUBQ_W_imm_indA(M68k* cpu, uint16_t value, int baseAddrReg) {
    SUBQ_W_imm_abs(cpu, value, cpu->a[baseAddrReg]);
}

static uint8_t subBCore(M68k* cpu, uint8_t src, uint8_t dst) {
    uint8_t result = (uint8_t)(dst - src);
    cpu->n = (result & 0x80u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    uint8_t overflowBits = (uint8_t)((dst ^ src) & (dst ^ result));
    cpu->v = (overflowBits & 0x80u) ? 1 : 0;
    cpu->c = (dst < src) ? 1 : 0;
    cpu->x = cpu->c;
    return result;
}

static uint16_t subWCore(M68k* cpu, uint16_t src, uint16_t dst) {
    uint16_t result = (uint16_t)(dst - src);
    cpu->n = (result & 0x8000u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    uint16_t overflowBits = (uint16_t)((dst ^ src) & (dst ^ result));
    cpu->v = (overflowBits & 0x8000u) ? 1 : 0;
    cpu->c = (dst < src) ? 1 : 0;
    cpu->x = cpu->c;
    return result;
}

void SUB_B_imm_D(M68k* cpu, uint8_t value, int dstDataReg) {
    uint8_t dst = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t result = subBCore(cpu, value, dst);
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], result);
}

void SUB_B_imm_abs(M68k* cpu, uint8_t value, uint32_t address) {
    uint8_t dst = read8(cpu, address);
    uint8_t result = subBCore(cpu, value, dst);
    write8(cpu, address, result);
}

void SUB_B_imm_dA(M68k* cpu, uint8_t value, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    SUB_B_imm_abs(cpu, value, ea);
}

void SUB_B_imm_indA(M68k* cpu, uint8_t value, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg];
    uint8_t dst = read8(cpu, ea);
    uint8_t result = subBCore(cpu, value, dst);
    write8(cpu, ea, result);
}

void SUB_B_imm_dAIx(M68k* cpu,
                    uint8_t value,
                    int32_t disp,
                    int baseAddrReg,
                    int indexReg,
                    int indexIsAddr,
                    int indexSize, int indexScale) {
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp + idx;
    uint8_t dst = read8(cpu, ea);
    uint8_t result = subBCore(cpu, value, dst);
    write8(cpu, ea, result);
}

void SUB_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint8_t src = (uint8_t)(cpu->d[srcDataReg] & 0xFFu);
    uint8_t dst = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t result = subBCore(cpu, src, dst);
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], result);
}

void SUB_B_abs_D(M68k* cpu, uint32_t address, int dstDataReg) {
    uint8_t src = read8(cpu, address);
    uint8_t dst = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t result = subBCore(cpu, src, dst);
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], result);
}

void SUB_B_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int dstDataReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint8_t src = read8(cpu, ea);
    uint8_t dst = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t result = subBCore(cpu, src, dst);
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], result);
}

void SUB_B_indA_D(M68k* cpu, int baseAddrReg, int dstDataReg) {
    uint8_t src = read8(cpu, cpu->a[baseAddrReg]);
    uint8_t dst = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t result = subBCore(cpu, src, dst);
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], result);
}

void SUB_B_pdA_D(M68k* cpu, int baseAddrReg, int dstDataReg) {
    uint32_t ea = cpu->a[baseAddrReg] - 1u;
    cpu->a[baseAddrReg] = ea;
    uint8_t src = read8(cpu, ea);
    uint8_t dst = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t result = subBCore(cpu, src, dst);
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], result);
}

void SUB_B_D_abs(M68k* cpu, int srcDataReg, uint32_t address) {
    uint8_t m = read8(cpu, address);
    uint8_t src = (uint8_t)(cpu->d[srcDataReg] & 0xFFu);
    uint8_t result = subBCore(cpu, src, m);
    write8(cpu, address, result);
}

void SUB_B_D_indA(M68k* cpu, int srcDataReg, int addrReg) {
    uint32_t ea = cpu->a[addrReg];
    uint8_t m = read8(cpu, ea);
    uint8_t src = (uint8_t)(cpu->d[srcDataReg] & 0xFFu);
    uint8_t result = subBCore(cpu, src, m);
    write8(cpu, ea, result);
}

void SUB_B_D_pdA(M68k* cpu, int srcDataReg, int addrReg) {
    uint32_t ea = cpu->a[addrReg] - 1u;
    cpu->a[addrReg] = ea;
    uint8_t m = read8(cpu, ea);
    uint8_t src = (uint8_t)(cpu->d[srcDataReg] & 0xFFu);
    uint8_t result = subBCore(cpu, src, m);
    write8(cpu, ea, result);
}

void SUB_B_D_dA(M68k* cpu, int srcDataReg, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint8_t m = read8(cpu, ea);
    uint8_t src = (uint8_t)(cpu->d[srcDataReg] & 0xFFu);
    uint8_t result = subBCore(cpu, src, m);
    write8(cpu, ea, result);
}

void SUB_W_imm_D(M68k* cpu, uint16_t value, int dstDataReg) {
    uint16_t dst = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    uint16_t result = subWCore(cpu, value, dst);
    cpu->d[dstDataReg] = setLow16(cpu->d[dstDataReg], result);
}

void SUB_W_imm_abs(M68k* cpu, uint16_t value, uint32_t address) {
    uint16_t dst = read16(cpu, address);
    uint16_t result = subWCore(cpu, value, dst);
    write16(cpu, address, result);
}

void SUB_W_imm_dA(M68k* cpu, uint16_t value, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    SUB_W_imm_abs(cpu, value, ea);
}

void SUB_W_imm_indA(M68k* cpu, uint16_t value, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg];
    uint16_t dst = read16(cpu, ea);
    uint16_t result = subWCore(cpu, value, dst);
    write16(cpu, ea, result);
}

void SUB_W_imm_piA(M68k* cpu, uint16_t value, int addrReg) {
    uint32_t ea = cpu->a[addrReg];
    uint16_t m = read16(cpu, ea);
    uint16_t result = subWCore(cpu, value, m);
    write16(cpu, ea, result);
    cpu->a[addrReg] = ea + 2u;
}

void SUB_W_imm_dAIx(M68k* cpu,
                    uint16_t value,
                    int32_t disp,
                    int baseAddrReg,
                    int indexReg,
                    int indexIsAddr,
                    int indexSize, int indexScale) {
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp + idx;
    uint16_t dst = read16(cpu, ea);
    uint16_t result = subWCore(cpu, value, dst);
    write16(cpu, ea, result);
}

void SUB_W_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint16_t src = (uint16_t)(cpu->d[srcDataReg] & 0xFFFFu);
    uint16_t dst = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    uint16_t result = subWCore(cpu, src, dst);
    cpu->d[dstDataReg] = setLow16(cpu->d[dstDataReg], result);
}

void SUB_W_abs_D(M68k* cpu, uint32_t address, int dstDataReg) {
    uint16_t src = read16(cpu, address);
    uint16_t dst = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    uint16_t result = subWCore(cpu, src, dst);
    cpu->d[dstDataReg] = setLow16(cpu->d[dstDataReg], result);
}

void SUB_W_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int dstDataReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint16_t src = read16(cpu, ea);
    uint16_t dst = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    uint16_t result = subWCore(cpu, src, dst);
    cpu->d[dstDataReg] = setLow16(cpu->d[dstDataReg], result);
}

void SUB_W_indA_D(M68k* cpu, int baseAddrReg, int dstDataReg) {
    uint16_t src = read16(cpu, cpu->a[baseAddrReg]);
    uint16_t dst = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    uint16_t result = subWCore(cpu, src, dst);
    cpu->d[dstDataReg] = setLow16(cpu->d[dstDataReg], result);
}

void SUB_W_piA_D(M68k* cpu, int baseAddrReg, int dstDataReg) {
    uint32_t ea = cpu->a[baseAddrReg];
    uint16_t src = read16(cpu, ea);
    cpu->a[baseAddrReg] = ea + 2u;
    uint16_t dst = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    uint16_t result = subWCore(cpu, src, dst);
    cpu->d[dstDataReg] = setLow16(cpu->d[dstDataReg], result);
}

void SUB_W_dAIx_D(M68k* cpu,
                  int32_t disp,
                  int baseAddrReg,
                  int indexReg,
                  int indexIsAddr,
                  int indexSize, int indexScale,
                  int dstDataReg) {
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp + idx;
    uint16_t src = read16(cpu, ea);
    uint16_t dst = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    uint16_t result = subWCore(cpu, src, dst);
    cpu->d[dstDataReg] = setLow16(cpu->d[dstDataReg], result);
}

void SUB_W_D_abs(M68k* cpu, int srcDataReg, uint32_t address) {
    uint16_t m = read16(cpu, address);
    uint16_t src = (uint16_t)(cpu->d[srcDataReg] & 0xFFFFu);
    uint16_t result = subWCore(cpu, src, m);
    write16(cpu, address, result);
}

void SUB_W_D_dA(M68k* cpu, int srcDataReg, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint16_t m = read16(cpu, ea);
    uint16_t src = (uint16_t)(cpu->d[srcDataReg] & 0xFFFFu);
    uint16_t result = subWCore(cpu, src, m);
    write16(cpu, ea, result);
}

void SUB_W_D_indA(M68k* cpu, int srcDataReg, int addrReg) {
    uint32_t ea = cpu->a[addrReg];
    uint16_t m = read16(cpu, ea);
    uint16_t src = (uint16_t)(cpu->d[srcDataReg] & 0xFFFFu);
    uint16_t result = subWCore(cpu, src, m);
    write16(cpu, ea, result);
}

void SUB_W_D_piA(M68k* cpu, int srcDataReg, int addrReg) {
    uint32_t ea = cpu->a[addrReg];
    uint16_t m = read16(cpu, ea);
    cpu->a[addrReg] = ea + 2u;
    uint16_t src = (uint16_t)(cpu->d[srcDataReg] & 0xFFFFu);
    uint16_t result = subWCore(cpu, src, m);
    write16(cpu, ea, result);
}

static uint32_t subLCore(M68k* cpu, uint32_t src, uint32_t dst) {
    uint32_t result = dst - src;
    cpu->n = (result & 0x80000000u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    uint32_t overflowBits = (uint32_t)((dst ^ src) & (dst ^ result));
    cpu->v = (overflowBits & 0x80000000u) ? 1 : 0;
    cpu->c = (dst < src) ? 1 : 0;
    cpu->x = cpu->c;
    return result;
}

void SUB_L_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint32_t src = cpu->d[srcDataReg];
    uint32_t dst = cpu->d[dstDataReg];
    uint32_t result = subLCore(cpu, src, dst);
    cpu->d[dstDataReg] = result;
}

void SUB_L_A_D(M68k* cpu, int srcAddrReg, int dstDataReg) {
    uint32_t src = cpu->a[srcAddrReg];
    uint32_t dst = cpu->d[dstDataReg];
    uint32_t result = subLCore(cpu, src, dst);
    cpu->d[dstDataReg] = result;
}

void SUB_L_imm_D(M68k* cpu, uint32_t value, int dstDataReg) {
    uint32_t dst = cpu->d[dstDataReg];
    uint32_t result = subLCore(cpu, value, dst);
    cpu->d[dstDataReg] = result;
}

void SUB_L_imm_abs(M68k* cpu, uint32_t value, uint32_t address) {
    uint32_t dst = read32(cpu, address);
    uint32_t result = subLCore(cpu, value, dst);
    write32(cpu, address, result);
}

void SUB_L_imm_dA(M68k* cpu, uint32_t value, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint32_t dst = read32(cpu, ea);
    uint32_t result = subLCore(cpu, value, dst);
    write32(cpu, ea, result);
}

void SUB_L_imm_indA(M68k* cpu, uint32_t value, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg];
    uint32_t dst = read32(cpu, ea);
    uint32_t result = subLCore(cpu, value, dst);
    write32(cpu, ea, result);
}

void SUB_L_imm_piA(M68k* cpu, uint32_t value, int addrReg) {
    uint32_t ea = cpu->a[addrReg];
    uint32_t dst = read32(cpu, ea);
    uint32_t result = subLCore(cpu, value, dst);
    write32(cpu, ea, result);
    cpu->a[addrReg] = ea + 4u;
}

void SUB_L_piA_D(M68k* cpu, int srcAddrReg, int dstDataReg) {
    uint32_t ea = cpu->a[srcAddrReg];
    uint32_t src = read32(cpu, ea);
    cpu->a[srcAddrReg] = ea + 4u;
    uint32_t dst = cpu->d[dstDataReg];
    uint32_t result = subLCore(cpu, src, dst);
    cpu->d[dstDataReg] = result;
}

void SUB_L_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int dstDataReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint32_t src = read32(cpu, ea);
    uint32_t dst = cpu->d[dstDataReg];
    uint32_t result = subLCore(cpu, src, dst);
    cpu->d[dstDataReg] = result;
}

void SUB_L_dAIx_D(M68k* cpu,
                  int32_t disp,
                  int baseAddrReg,
                  int indexReg,
                  int indexIsAddr,
                  int indexSize, int indexScale,
                  int dstDataReg) {
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp + idx;
    uint32_t src = read32(cpu, ea);
    uint32_t dst = cpu->d[dstDataReg];
    uint32_t result = subLCore(cpu, src, dst);
    cpu->d[dstDataReg] = result;
}

void SUB_L_abs_D(M68k* cpu, uint32_t address, int dstDataReg) {
    uint32_t src = read32(cpu, address);
    uint32_t dst = cpu->d[dstDataReg];
    uint32_t result = subLCore(cpu, src, dst);
    cpu->d[dstDataReg] = result;
}

void SUB_L_indA_D(M68k* cpu, int baseAddrReg, int dstDataReg) {
    uint32_t src = read32(cpu, cpu->a[baseAddrReg]);
    uint32_t dst = cpu->d[dstDataReg];
    uint32_t result = subLCore(cpu, src, dst);
    cpu->d[dstDataReg] = result;
}

void SUB_L_D_abs(M68k* cpu, int srcDataReg, uint32_t address) {
    uint32_t m = read32(cpu, address);
    uint32_t src = cpu->d[srcDataReg];
    uint32_t result = subLCore(cpu, src, m);
    write32(cpu, address, result);
}

void SUB_L_D_dA(M68k* cpu, int srcDataReg, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint32_t m = read32(cpu, ea);
    uint32_t src = cpu->d[srcDataReg];
    uint32_t result = subLCore(cpu, src, m);
    write32(cpu, ea, result);
}

void SUB_L_D_indA(M68k* cpu, int srcDataReg, int addrReg) {
    uint32_t ea = cpu->a[addrReg];
    uint32_t m = read32(cpu, ea);
    uint32_t src = cpu->d[srcDataReg];
    uint32_t result = subLCore(cpu, src, m);
    write32(cpu, ea, result);
}

void SUB_L_D_A(M68k* cpu, int srcDataReg, int dstAddrReg) {
    cpu->a[dstAddrReg] = cpu->a[dstAddrReg] - cpu->d[srcDataReg];
    /* SUBA.L does not affect condition codes. */
}

void SUB_L_abs_A(M68k* cpu, uint32_t address, int dstAddrReg) {
    uint32_t src = read32(cpu, address);
    cpu->a[dstAddrReg] = cpu->a[dstAddrReg] - src;
    /* SUBA.L does not affect condition codes. */
}

void SUB_L_indA_A(M68k* cpu, int srcAddrReg, int dstAddrReg) {
    uint32_t src = read32(cpu, cpu->a[srcAddrReg]);
    cpu->a[dstAddrReg] = cpu->a[dstAddrReg] - src;
    /* SUBA.L does not affect condition codes. */
}

void SUB_L_piA_A(M68k* cpu, int srcAddrReg, int dstAddrReg) {
    uint32_t ea = cpu->a[srcAddrReg];
    uint32_t src = read32(cpu, ea);
    cpu->a[srcAddrReg] = ea + 4u;
    cpu->a[dstAddrReg] = cpu->a[dstAddrReg] - src;
    /* SUBA.L does not affect condition codes. */
}

void SUB_L_dA_A(M68k* cpu, int32_t disp, int baseAddrReg, int dstAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint32_t src = read32(cpu, ea);
    cpu->a[dstAddrReg] = cpu->a[dstAddrReg] - src;
    /* SUBA.L does not affect condition codes. */
}

void SUB_L_dAIx_A(M68k* cpu,
                  int32_t disp,
                  int baseAddrReg,
                  int indexReg,
                  int indexIsAddr,
                  int indexSize, int indexScale,
                  int dstAddrReg) {
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp + idx;
    uint32_t src = read32(cpu, ea);
    cpu->a[dstAddrReg] = cpu->a[dstAddrReg] - src;
    /* SUBA.L does not affect condition codes. */
}

void SUBQ_L_imm_D(M68k* cpu, uint32_t value, int regIdx) {
    uint32_t dst = cpu->d[regIdx];
    uint32_t result = subLCore(cpu, value, dst);
    cpu->d[regIdx] = result;
}

static uint8_t andBCore(M68k* cpu, uint8_t src, uint8_t dst) {
    uint8_t result = (uint8_t)(dst & src);
    cpu->n = (result & 0x80u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    cpu->v = 0;
    cpu->c = 0;
    /* X is not affected by AND. */
    return result;
}

static uint16_t andWCore(M68k* cpu, uint16_t src, uint16_t dst) {
    uint16_t result = (uint16_t)(dst & src);
    cpu->n = (result & 0x8000u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    cpu->v = 0;
    cpu->c = 0;
    /* X is not affected by AND. */
    return result;
}

void AND_B_imm_D(M68k* cpu, uint8_t value, int dstDataReg) {
    uint8_t dst = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t result = andBCore(cpu, value, dst);
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], result);
}

void AND_B_imm_abs(M68k* cpu, uint8_t value, uint32_t address) {
    uint8_t dst = read8(cpu, address);
    uint8_t result = andBCore(cpu, value, dst);
    write8(cpu, address, result);
}

void AND_B_imm_dA(M68k* cpu, uint8_t value, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint8_t dst = read8(cpu, ea);
    uint8_t result = andBCore(cpu, value, dst);
    write8(cpu, ea, result);
}

void AND_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint8_t src = (uint8_t)(cpu->d[srcDataReg] & 0xFFu);
    uint8_t dst = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t result = andBCore(cpu, src, dst);
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], result);
}

void AND_B_abs_D(M68k* cpu, uint32_t address, int dstDataReg) {
    uint8_t src = read8(cpu, address);
    uint8_t dst = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t result = andBCore(cpu, src, dst);
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], result);
}

void AND_B_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int dstDataReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint8_t src = read8(cpu, ea);
    uint8_t dst = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t result = andBCore(cpu, src, dst);
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], result);
}

void AND_B_indA_D(M68k* cpu, int baseAddrReg, int dstDataReg) {
    uint8_t src = read8(cpu, cpu->a[baseAddrReg]);
    uint8_t dst = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t result = andBCore(cpu, src, dst);
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], result);
}

void AND_B_D_indA(M68k* cpu, int srcDataReg, int addrReg) {
    uint32_t ea = cpu->a[addrReg];
    uint8_t m = read8(cpu, ea);
    uint8_t r = (uint8_t)(cpu->d[srcDataReg] & 0xFFu);
    uint8_t result = andBCore(cpu, r, m);
    write8(cpu, ea, result);
}

void AND_W_imm_D(M68k* cpu, uint16_t value, int dstDataReg) {
    uint16_t dst = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    uint16_t result = andWCore(cpu, value, dst);
    cpu->d[dstDataReg] = setLow16(cpu->d[dstDataReg], result);
}

void AND_W_imm_abs(M68k* cpu, uint16_t value, uint32_t address) {
    uint16_t dst = read16(cpu, address);
    uint16_t result = andWCore(cpu, value, dst);
    write16(cpu, address, result);
}

void AND_W_imm_dA(M68k* cpu, uint16_t value, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint16_t dst = read16(cpu, ea);
    uint16_t result = andWCore(cpu, value, dst);
    write16(cpu, ea, result);
}

void AND_W_imm_indA(M68k* cpu, uint16_t value, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg];
    uint16_t dst = read16(cpu, ea);
    uint16_t result = andWCore(cpu, value, dst);
    write16(cpu, ea, result);
}

void AND_W_imm_dAIx(M68k* cpu,
                    uint16_t value,
                    int32_t disp,
                    int baseAddrReg,
                    int indexReg,
                    int indexIsAddr,
                    int indexSize, int indexScale) {
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp + idx;
    uint16_t dst = read16(cpu, ea);
    uint16_t result = andWCore(cpu, value, dst);
    write16(cpu, ea, result);
}

void AND_W_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint16_t src = (uint16_t)(cpu->d[srcDataReg] & 0xFFFFu);
    uint16_t dst = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    uint16_t result = andWCore(cpu, src, dst);
    cpu->d[dstDataReg] = setLow16(cpu->d[dstDataReg], result);
}

void AND_W_abs_D(M68k* cpu, uint32_t address, int dstDataReg) {
    uint16_t src = read16(cpu, address);
    uint16_t dst = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    uint16_t result = andWCore(cpu, src, dst);
    cpu->d[dstDataReg] = setLow16(cpu->d[dstDataReg], result);
}

void AND_W_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int dstDataReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint16_t src = read16(cpu, ea);
    uint16_t dst = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    uint16_t result = andWCore(cpu, src, dst);
    cpu->d[dstDataReg] = setLow16(cpu->d[dstDataReg], result);
}

void AND_W_indA_D(M68k* cpu, int baseAddrReg, int dstDataReg) {
    uint16_t src = read16(cpu, cpu->a[baseAddrReg]);
    uint16_t dst = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    uint16_t result = andWCore(cpu, src, dst);
    cpu->d[dstDataReg] = setLow16(cpu->d[dstDataReg], result);
}

static uint32_t andLCore(M68k* cpu, uint32_t src, uint32_t dst) {
    uint32_t result = dst & src;
    cpu->n = (result & 0x80000000u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    cpu->v = 0;
    cpu->c = 0;
    /* X is not affected by AND. */
    return result;
}

void AND_L_imm_D(M68k* cpu, uint32_t value, int dstDataReg) {
    uint32_t dst = cpu->d[dstDataReg];
    uint32_t result = andLCore(cpu, value, dst);
    cpu->d[dstDataReg] = result;
}

void AND_L_imm_abs(M68k* cpu, uint32_t value, uint32_t address) {
    uint32_t dst = read32(cpu, address);
    uint32_t result = andLCore(cpu, value, dst);
    write32(cpu, address, result);
}

void AND_L_imm_dA(M68k* cpu, uint32_t value, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    AND_L_imm_abs(cpu, value, ea);
}

void OR_W_imm_D(M68k* cpu, uint16_t value, int dstDataReg) {
    uint16_t dst = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    uint16_t result = (uint16_t)(dst | value);
    cpu->d[dstDataReg] = setLow16(cpu->d[dstDataReg], result);
    cpu->n = (result & 0x8000u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    cpu->v = 0;
    cpu->c = 0;
    /* X is not affected by OR. */
}

void OR_W_imm_abs(M68k* cpu, uint16_t value, uint32_t address) {
    uint16_t dst = read16(cpu, address);
    uint16_t result = (uint16_t)(dst | value);
    write16(cpu, address, result);
    cpu->n = (result & 0x8000u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    cpu->v = 0;
    cpu->c = 0;
}

void OR_W_imm_dA(M68k* cpu, uint16_t value, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint16_t dst = read16(cpu, ea);
    uint16_t result = (uint16_t)(dst | value);
    write16(cpu, ea, result);
    cpu->n = (result & 0x8000u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    cpu->v = 0;
    cpu->c = 0;
}

void OR_W_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint16_t src = (uint16_t)(cpu->d[srcDataReg] & 0xFFFFu);
    uint16_t dst = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    uint16_t result = (uint16_t)(dst | src);
    cpu->d[dstDataReg] = setLow16(cpu->d[dstDataReg], result);
    cpu->n = (result & 0x8000u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    cpu->v = 0;
    cpu->c = 0;
    /* X is not affected by OR. */
}

void OR_W_D_indA(M68k* cpu, int srcDataReg, int addrReg) {
    uint32_t ea = cpu->a[addrReg];
    uint16_t m = read16(cpu, ea);
    uint16_t src = (uint16_t)(cpu->d[srcDataReg] & 0xFFFFu);
    uint16_t result = (uint16_t)(m | src);
    write16(cpu, ea, result);
    cpu->n = (result & 0x8000u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    cpu->v = 0;
    cpu->c = 0;
}

void OR_W_D_dA(M68k* cpu, int srcDataReg, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint16_t m = read16(cpu, ea);
    uint16_t src = (uint16_t)(cpu->d[srcDataReg] & 0xFFFFu);
    uint16_t result = (uint16_t)(m | src);
    write16(cpu, ea, result);
    cpu->n = (result & 0x8000u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    cpu->v = 0;
    cpu->c = 0;
}

void OR_W_D_abs(M68k* cpu, int srcDataReg, uint32_t address) {
    uint16_t m = read16(cpu, address);
    uint16_t src = (uint16_t)(cpu->d[srcDataReg] & 0xFFFFu);
    uint16_t result = (uint16_t)(m | src);
    write16(cpu, address, result);
    cpu->n = (result & 0x8000u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    cpu->v = 0;
    cpu->c = 0;
}

void OR_W_abs_D(M68k* cpu, uint32_t address, int dstDataReg) {
    uint16_t src = read16(cpu, address);
    uint16_t dst = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    uint16_t result = (uint16_t)(dst | src);
    cpu->d[dstDataReg] = setLow16(cpu->d[dstDataReg], result);
    cpu->n = (result & 0x8000u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    cpu->v = 0;
    cpu->c = 0;
    /* X is not affected by OR. */
}

void OR_W_D_dAIx(M68k* cpu,
                 int srcDataReg,
                 int32_t disp,
                 int baseAddrReg,
                 int indexReg,
                 int indexIsAddr,
                 int indexSize, int indexScale) {
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp + idx;
    uint16_t m = read16(cpu, ea);
    uint16_t src = (uint16_t)(cpu->d[srcDataReg] & 0xFFFFu);
    uint16_t result = (uint16_t)(m | src);
    write16(cpu, ea, result);
    cpu->n = (result & 0x8000u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    cpu->v = 0;
    cpu->c = 0;
}

static void orBSetFlags(M68k* cpu, uint8_t result) {
    cpu->n = (result & 0x80u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    cpu->v = 0;
    cpu->c = 0;
    /* X is not affected by OR. */
}

void OR_B_imm_D(M68k* cpu, uint8_t value, int dstDataReg) {
    uint8_t dst = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t result = (uint8_t)(dst | value);
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], result);
    orBSetFlags(cpu, result);
}

void OR_B_imm_abs(M68k* cpu, uint8_t value, uint32_t address) {
    uint8_t dst = read8(cpu, address);
    uint8_t result = (uint8_t)(dst | value);
    write8(cpu, address, result);
    orBSetFlags(cpu, result);
}

void OR_B_imm_dA(M68k* cpu, uint8_t value, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint8_t dst = read8(cpu, ea);
    uint8_t result = (uint8_t)(dst | value);
    write8(cpu, ea, result);
    orBSetFlags(cpu, result);
}

void OR_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint8_t src = (uint8_t)(cpu->d[srcDataReg] & 0xFFu);
    uint8_t dst = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t result = (uint8_t)(dst | src);
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], result);
    orBSetFlags(cpu, result);
}

void OR_B_abs_D(M68k* cpu, uint32_t address, int dstDataReg) {
    uint8_t src = read8(cpu, address);
    uint8_t dst = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t result = (uint8_t)(dst | src);
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], result);
    orBSetFlags(cpu, result);
}

void OR_B_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int dstDataReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint8_t src = read8(cpu, ea);
    uint8_t dst = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t result = (uint8_t)(dst | src);
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], result);
    orBSetFlags(cpu, result);
}

void OR_B_indA_D(M68k* cpu, int baseAddrReg, int dstDataReg) {
    uint8_t src = read8(cpu, cpu->a[baseAddrReg]);
    uint8_t dst = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t result = (uint8_t)(dst | src);
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], result);
    orBSetFlags(cpu, result);
}

void OR_B_D_abs(M68k* cpu, int srcDataReg, uint32_t address) {
    uint8_t m = read8(cpu, address);
    uint8_t src = (uint8_t)(cpu->d[srcDataReg] & 0xFFu);
    uint8_t result = (uint8_t)(m | src);
    write8(cpu, address, result);
    orBSetFlags(cpu, result);
}

void OR_B_D_dA(M68k* cpu, int srcDataReg, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint8_t m = read8(cpu, ea);
    uint8_t src = (uint8_t)(cpu->d[srcDataReg] & 0xFFu);
    uint8_t result = (uint8_t)(m | src);
    write8(cpu, ea, result);
    orBSetFlags(cpu, result);
}

void OR_B_D_indA(M68k* cpu, int srcDataReg, int addrReg) {
    uint32_t ea = cpu->a[addrReg];
    uint8_t m = read8(cpu, ea);
    uint8_t src = (uint8_t)(cpu->d[srcDataReg] & 0xFFu);
    uint8_t result = (uint8_t)(m | src);
    write8(cpu, ea, result);
    orBSetFlags(cpu, result);
}

void EOR_W_imm_D(M68k* cpu, uint16_t value, int dstDataReg) {
    uint16_t dst = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    uint16_t result = (uint16_t)(dst ^ value);
    cpu->d[dstDataReg] = setLow16(cpu->d[dstDataReg], result);
    cpu->n = (result & 0x8000u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    cpu->v = 0;
    cpu->c = 0;
    /* X is not affected by EOR. */
}

void EOR_W_imm_abs(M68k* cpu, uint16_t value, uint32_t address) {
    uint16_t dst = read16(cpu, address);
    uint16_t result = (uint16_t)(dst ^ value);
    write16(cpu, address, result);
    cpu->n = (result & 0x8000u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    cpu->v = 0;
    cpu->c = 0;
}

void EOR_W_imm_dA(M68k* cpu, uint16_t value, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint16_t dst = read16(cpu, ea);
    uint16_t result = (uint16_t)(dst ^ value);
    write16(cpu, ea, result);
    cpu->n = (result & 0x8000u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    cpu->v = 0;
    cpu->c = 0;
}

void EOR_W_imm_piA(M68k* cpu, uint16_t value, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg];
    uint16_t dst = read16(cpu, ea);
    cpu->a[baseAddrReg] = ea + 2u;
    uint16_t result = (uint16_t)(dst ^ value);
    write16(cpu, ea, result);
    cpu->n = (result & 0x8000u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    cpu->v = 0;
    cpu->c = 0;
}

void EOR_W_imm_indA(M68k* cpu, uint16_t value, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg];
    uint16_t dst = read16(cpu, ea);
    uint16_t result = (uint16_t)(dst ^ value);
    write16(cpu, ea, result);
    cpu->n = (result & 0x8000u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    cpu->v = 0;
    cpu->c = 0;
}

void EOR_W_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint16_t src = (uint16_t)(cpu->d[srcDataReg] & 0xFFFFu);
    uint16_t dst = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    uint16_t result = (uint16_t)(dst ^ src);
    cpu->d[dstDataReg] = setLow16(cpu->d[dstDataReg], result);
    cpu->n = (result & 0x8000u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    cpu->v = 0;
    cpu->c = 0;
    /* X is not affected by EOR. */
}

static void eorBSetFlags(M68k* cpu, uint8_t result) {
    cpu->n = (result & 0x80u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    cpu->v = 0;
    cpu->c = 0;
    /* X is not affected by EOR. */
}

void EOR_B_imm_D(M68k* cpu, uint8_t value, int dstDataReg) {
    uint8_t dst = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t result = (uint8_t)(dst ^ value);
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], result);
    eorBSetFlags(cpu, result);
}

void EOR_B_imm_abs(M68k* cpu, uint8_t value, uint32_t address) {
    uint8_t dst = read8(cpu, address);
    uint8_t result = (uint8_t)(dst ^ value);
    write8(cpu, address, result);
    eorBSetFlags(cpu, result);
}

void EOR_B_imm_dA(M68k* cpu, uint8_t value, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint8_t dst = read8(cpu, ea);
    uint8_t result = (uint8_t)(dst ^ value);
    write8(cpu, ea, result);
    eorBSetFlags(cpu, result);
}

void EOR_B_imm_indA(M68k* cpu, uint8_t value, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg];
    uint8_t dst = read8(cpu, ea);
    uint8_t result = (uint8_t)(dst ^ value);
    write8(cpu, ea, result);
    eorBSetFlags(cpu, result);
}

void EOR_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint8_t s = (uint8_t)(cpu->d[srcDataReg] & 0xFFu);
    uint8_t d = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t result = (uint8_t)(s ^ d);
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], result);
    eorBSetFlags(cpu, result);
}

static void eorLSetFlags(M68k* cpu, uint32_t result) {
    cpu->n = (result & 0x80000000u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    cpu->v = 0;
    cpu->c = 0;
}

void EOR_L_imm_D(M68k* cpu, uint32_t value, int dstDataReg) {
    uint32_t dst = cpu->d[dstDataReg];
    uint32_t result = dst ^ value;
    cpu->d[dstDataReg] = result;
    eorLSetFlags(cpu, result);
}

void EOR_L_imm_abs(M68k* cpu, uint32_t value, uint32_t address) {
    uint32_t dst = read32(cpu, address);
    uint32_t result = dst ^ value;
    write32(cpu, address, result);
    eorLSetFlags(cpu, result);
}

void EOR_L_imm_dA(M68k* cpu, uint32_t value, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint32_t dst = read32(cpu, ea);
    uint32_t result = dst ^ value;
    write32(cpu, ea, result);
    eorLSetFlags(cpu, result);
}

void EOR_L_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint32_t s = cpu->d[srcDataReg];
    uint32_t d = cpu->d[dstDataReg];
    uint32_t result = s ^ d;
    cpu->d[dstDataReg] = result;
    eorLSetFlags(cpu, result);
}

void NEG_W_D(M68k* cpu, int dstDataReg) {
    uint16_t src = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    uint16_t result = (uint16_t)(0u - src);
    cpu->d[dstDataReg] = setLow16(cpu->d[dstDataReg], result);
    cpu->n = (result & 0x8000u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    cpu->v = (src == 0x8000u) ? 1 : 0;
    cpu->c = (src != 0u) ? 1 : 0;
    cpu->x = cpu->c;
}

void NEG_W_dA(M68k* cpu, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint16_t src = read16(cpu, ea);
    uint16_t result = (uint16_t)(0u - src);
    write16(cpu, ea, result);
    cpu->n = (result & 0x8000u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    cpu->v = (src == 0x8000u) ? 1 : 0;
    cpu->c = (src != 0u) ? 1 : 0;
    cpu->x = cpu->c;
}

void NEG_W_abs(M68k* cpu, uint32_t address) {
    uint16_t src = read16(cpu, address);
    uint16_t result = (uint16_t)(0u - src);
    write16(cpu, address, result);
    cpu->n = (result & 0x8000u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    cpu->v = (src == 0x8000u) ? 1 : 0;
    cpu->c = (src != 0u) ? 1 : 0;
    cpu->x = cpu->c;
}

void NEG_L_D(M68k* cpu, int dstDataReg) {
    uint32_t src = cpu->d[dstDataReg];
    uint32_t result = 0u - src;
    cpu->d[dstDataReg] = result;
    cpu->n = (result & 0x80000000u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    cpu->v = (src == 0x80000000u) ? 1 : 0;
    cpu->c = (src != 0u) ? 1 : 0;
    cpu->x = cpu->c;
}

void NEG_L_dA(M68k* cpu, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    uint32_t src = read32(cpu, ea);
    uint32_t result = 0u - src;
    write32(cpu, ea, result);
    cpu->n = (result & 0x80000000u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    cpu->v = (src == 0x80000000u) ? 1 : 0;
    cpu->c = (src != 0u) ? 1 : 0;
    cpu->x = cpu->c;
}

void NEG_L_abs(M68k* cpu, uint32_t address) {
    uint32_t src = read32(cpu, address);
    uint32_t result = 0u - src;
    write32(cpu, address, result);
    cpu->n = (result & 0x80000000u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    cpu->v = (src == 0x80000000u) ? 1 : 0;
    cpu->c = (src != 0u) ? 1 : 0;
    cpu->x = cpu->c;
}

void NOT_W_D(M68k* cpu, int dstDataReg) {
    uint16_t src = (uint16_t)(cpu->d[dstDataReg] & 0xFFFFu);
    uint16_t result = (uint16_t)(~src);
    cpu->d[dstDataReg] = setLow16(cpu->d[dstDataReg], result);
    cpu->n = (result & 0x8000u) ? 1 : 0;
    cpu->z = (result == 0) ? 1 : 0;
    cpu->v = 0;
    cpu->c = 0;
    /* X is not affected by NOT. */
}

static void clrSetFlags(M68k* cpu) {
    cpu->n = 0;
    cpu->z = 1;
    cpu->v = 0;
    cpu->c = 0;
    /* X is not affected by CLR. */
}

void CLR_B_abs(M68k* cpu, uint32_t address) {
    write8(cpu, address, 0u);
    clrSetFlags(cpu);
}

void CLR_B_dA(M68k* cpu, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    write8(cpu, ea, 0u);
    clrSetFlags(cpu);
}

void CLR_B_indA(M68k* cpu, int baseAddrReg) {
    write8(cpu, cpu->a[baseAddrReg], 0u);
    clrSetFlags(cpu);
}

void CLR_B_dAIx(M68k* cpu,
                int32_t disp,
                int baseAddrReg,
                int indexReg,
                int indexIsAddr,
                int indexSize, int indexScale) {
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp + idx;
    write8(cpu, ea, 0u);
    clrSetFlags(cpu);
}

void CLR_W_abs(M68k* cpu, uint32_t address) {
    write16(cpu, address, 0u);
    clrSetFlags(cpu);
}

void CLR_W_D(M68k* cpu, int dstDataReg) {
    cpu->d[dstDataReg] = setLow16(cpu->d[dstDataReg], 0u);
    clrSetFlags(cpu);
}

void CLR_W_dA(M68k* cpu, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    write16(cpu, ea, 0u);
    clrSetFlags(cpu);
}

void CLR_W_indA(M68k* cpu, int baseAddrReg) {
    write16(cpu, cpu->a[baseAddrReg], 0u);
    clrSetFlags(cpu);
}

void CLR_W_piA(M68k* cpu, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg];
    write16(cpu, ea, 0u);
    cpu->a[baseAddrReg] = ea + 2u;
    clrSetFlags(cpu);
}

void CLR_W_dAIx(M68k* cpu,
                int32_t disp,
                int baseAddrReg,
                int indexReg,
                int indexIsAddr,
                int indexSize, int indexScale) {
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp + idx;
    write16(cpu, ea, 0u);
    clrSetFlags(cpu);
}

void CLR_L_abs(M68k* cpu, uint32_t address) {
    write32(cpu, address, 0u);
    clrSetFlags(cpu);
}

void CLR_L_D(M68k* cpu, int dstDataReg) {
    cpu->d[dstDataReg] = 0u;
    clrSetFlags(cpu);
}

void CLR_L_dA(M68k* cpu, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    write32(cpu, ea, 0u);
    clrSetFlags(cpu);
}

void CLR_L_indA(M68k* cpu, int baseAddrReg) {
    write32(cpu, cpu->a[baseAddrReg], 0u);
    clrSetFlags(cpu);
}

void CLR_L_piA(M68k* cpu, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg];
    write32(cpu, ea, 0u);
    cpu->a[baseAddrReg] = ea + 4u;
    clrSetFlags(cpu);
}

void CLR_L_dAIx(M68k* cpu,
                int32_t disp,
                int baseAddrReg,
                int indexReg,
                int indexIsAddr,
                int indexSize, int indexScale) {
    uint32_t idx = indexExtend(cpu, indexReg, indexIsAddr, indexSize, indexScale);
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp + idx;
    write32(cpu, ea, 0u);
    clrSetFlags(cpu);
}

void BTST_B_imm_abs(M68k* cpu, uint16_t bit, uint32_t address) {
    uint8_t value = read8(cpu, address);
    uint8_t mask = (uint8_t)(1u << (bit & 7u));
    cpu->z = (value & mask) ? 0 : 1;
    /* BTST only affects Z. */
}

void BTST_B_imm_D(M68k* cpu, uint16_t bit, int dstDataReg) {
    uint8_t value = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t mask = (uint8_t)(1u << (bit & 7u));
    cpu->z = (value & mask) ? 0 : 1;
}

void BTST_B_imm_dA(M68k* cpu, uint16_t bit, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    BTST_B_imm_abs(cpu, bit, ea);
}

void BCLR_B_imm_abs(M68k* cpu, uint16_t bit, uint32_t address) {
    uint8_t value = read8(cpu, address);
    uint8_t mask = (uint8_t)(1u << (bit & 7u));
    cpu->z = (value & mask) ? 0 : 1;
    value = (uint8_t)(value & (uint8_t)(~mask));
    write8(cpu, address, value);
    /* BCLR only affects Z. */
}

void BCLR_B_imm_D(M68k* cpu, uint16_t bit, int dstDataReg) {
    uint8_t value = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t mask = (uint8_t)(1u << (bit & 7u));
    cpu->z = (value & mask) ? 0 : 1;
    value = (uint8_t)(value & (uint8_t)(~mask));
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], value);
}

void BCLR_B_imm_dA(M68k* cpu, uint16_t bit, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    BCLR_B_imm_abs(cpu, bit, ea);
}

void BSET_B_imm_abs(M68k* cpu, uint16_t bit, uint32_t address) {
    uint8_t value = read8(cpu, address);
    uint8_t mask = (uint8_t)(1u << (bit & 7u));
    cpu->z = (value & mask) ? 0 : 1;
    value = (uint8_t)(value | mask);
    write8(cpu, address, value);
    /* BSET only affects Z. */
}

void BSET_B_imm_D(M68k* cpu, uint16_t bit, int dstDataReg) {
    uint8_t value = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t mask = (uint8_t)(1u << (bit & 7u));
    cpu->z = (value & mask) ? 0 : 1;
    value = (uint8_t)(value | mask);
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], value);
}

void BSET_B_imm_dA(M68k* cpu, uint16_t bit, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    BSET_B_imm_abs(cpu, bit, ea);
}

void BTST_B_imm_indA(M68k* cpu, uint16_t bit, int baseAddrReg) {
    BTST_B_imm_abs(cpu, bit, cpu->a[baseAddrReg]);
}

void BCLR_B_imm_indA(M68k* cpu, uint16_t bit, int baseAddrReg) {
    BCLR_B_imm_abs(cpu, bit, cpu->a[baseAddrReg]);
}

void BSET_B_imm_indA(M68k* cpu, uint16_t bit, int baseAddrReg) {
    BSET_B_imm_abs(cpu, bit, cpu->a[baseAddrReg]);
}

static uint16_t dynamicBitByte(M68k* cpu, int srcDataReg) {
    return (uint16_t)(cpu->d[srcDataReg] % 8u);
}

void BTST_B_D_abs(M68k* cpu, int srcDataReg, uint32_t address) {
    BTST_B_imm_abs(cpu, dynamicBitByte(cpu, srcDataReg), address);
}

void BTST_B_D_dA(M68k* cpu, int srcDataReg, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    BTST_B_imm_abs(cpu, dynamicBitByte(cpu, srcDataReg), ea);
}

void BTST_B_D_indA(M68k* cpu, int srcDataReg, int baseAddrReg) {
    BTST_B_imm_abs(cpu, dynamicBitByte(cpu, srcDataReg), cpu->a[baseAddrReg]);
}

void BTST_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint16_t bit = dynamicBitByte(cpu, srcDataReg);
    uint8_t value = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t mask = (uint8_t)(1u << (bit & 7u));
    cpu->z = (value & mask) ? 0 : 1;
}

void BCLR_B_D_abs(M68k* cpu, int srcDataReg, uint32_t address) {
    BCLR_B_imm_abs(cpu, dynamicBitByte(cpu, srcDataReg), address);
}

void BCLR_B_D_dA(M68k* cpu, int srcDataReg, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    BCLR_B_imm_abs(cpu, dynamicBitByte(cpu, srcDataReg), ea);
}

void BCLR_B_D_indA(M68k* cpu, int srcDataReg, int baseAddrReg) {
    BCLR_B_imm_abs(cpu, dynamicBitByte(cpu, srcDataReg), cpu->a[baseAddrReg]);
}

void BCLR_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint16_t bit = dynamicBitByte(cpu, srcDataReg);
    uint8_t value = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t mask = (uint8_t)(1u << (bit & 7u));
    cpu->z = (value & mask) ? 0 : 1;
    value = (uint8_t)(value & (uint8_t)(~mask));
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], value);
}

void BSET_B_D_abs(M68k* cpu, int srcDataReg, uint32_t address) {
    BSET_B_imm_abs(cpu, dynamicBitByte(cpu, srcDataReg), address);
}

void BSET_B_D_dA(M68k* cpu, int srcDataReg, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    BSET_B_imm_abs(cpu, dynamicBitByte(cpu, srcDataReg), ea);
}

void BSET_B_D_indA(M68k* cpu, int srcDataReg, int baseAddrReg) {
    BSET_B_imm_abs(cpu, dynamicBitByte(cpu, srcDataReg), cpu->a[baseAddrReg]);
}

void BSET_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    uint16_t bit = dynamicBitByte(cpu, srcDataReg);
    uint8_t value = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t mask = (uint8_t)(1u << (bit & 7u));
    cpu->z = (value & mask) ? 0 : 1;
    value = (uint8_t)(value | mask);
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], value);
}

void BCHG_B_imm_abs(M68k* cpu, uint16_t bit, uint32_t address) {
    uint8_t value = read8(cpu, address);
    uint8_t mask = (uint8_t)(1u << (bit & 7u));
    cpu->z = (value & mask) ? 0 : 1;
    value = (uint8_t)(value ^ mask);
    write8(cpu, address, value);
}

void BCHG_B_imm_dA(M68k* cpu, uint16_t bit, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    BCHG_B_imm_abs(cpu, bit, ea);
}

void BCHG_B_imm_indA(M68k* cpu, uint16_t bit, int baseAddrReg) {
    BCHG_B_imm_abs(cpu, bit, cpu->a[baseAddrReg]);
}

void BCHG_B_imm_D(M68k* cpu, uint16_t bit, int dstDataReg) {
    uint8_t value = (uint8_t)(cpu->d[dstDataReg] & 0xFFu);
    uint8_t mask = (uint8_t)(1u << (bit & 7u));
    cpu->z = (value & mask) ? 0 : 1;
    value = (uint8_t)(value ^ mask);
    cpu->d[dstDataReg] = setLow8(cpu->d[dstDataReg], value);
}

void BCHG_B_D_abs(M68k* cpu, int srcDataReg, uint32_t address) {
    BCHG_B_imm_abs(cpu, dynamicBitByte(cpu, srcDataReg), address);
}

void BCHG_B_D_dA(M68k* cpu, int srcDataReg, int32_t disp, int baseAddrReg) {
    uint32_t ea = cpu->a[baseAddrReg] + (uint32_t)disp;
    BCHG_B_imm_abs(cpu, dynamicBitByte(cpu, srcDataReg), ea);
}

void BCHG_B_D_indA(M68k* cpu, int srcDataReg, int baseAddrReg) {
    BCHG_B_imm_abs(cpu, dynamicBitByte(cpu, srcDataReg), cpu->a[baseAddrReg]);
}

void BCHG_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg) {
    BCHG_B_imm_D(cpu, dynamicBitByte(cpu, srcDataReg), dstDataReg);
}

int DBF_D(M68k* cpu, int dataReg) {
    int16_t counter = (int16_t)(cpu->d[dataReg] & 0xFFFF);
    counter = (int16_t)(counter - 1);
    cpu->d[dataReg] = setLow16(cpu->d[dataReg], (uint16_t)counter);
    /* DBF/DBRA: if counter is now -1, branch is NOT taken; otherwise taken.
       Flags are untouched, matching 68k behavior. */
    return (counter != -1) ? 1 : 0;
}

void dumpCpuState(M68k* cpu) {
    fprintf(stderr, "--- 68k cpu state ---\n");
    fprintf(stderr, "pcVirtual = 0x%08x\n", cpu->pcVirtual);
    for (int i = 0; i < 8; i++) {
        fprintf(stderr, "  d%d = 0x%08x   a%d = 0x%08x\n",
                i, cpu->d[i], i, cpu->a[i]);
    }
    fprintf(stderr, "  flags: X=%u N=%u Z=%u V=%u C=%u\n",
            (unsigned)cpu->x, (unsigned)cpu->n, (unsigned)cpu->z,
            (unsigned)cpu->v, (unsigned)cpu->c);
    if (cpu->lastAsmLine != 0u) {
        fprintf(stderr, "  last asm line: %u\n", cpu->lastAsmLine);
    }
    if (cpu->lastAsmText != NULL && cpu->lastAsmText[0] != '\0') {
        fprintf(stderr, "  last asm text: %s\n", cpu->lastAsmText);
    }
    if (cpu->lastLabelName != NULL && cpu->lastLabelName[0] != '\0') {
        fprintf(stderr, "  last label: %s (0x%08x)\n",
                cpu->lastLabelName,
                cpu->lastLabelPc);
    }
    if (cpu->labelTraceCount > 0u) {
        fprintf(stderr, "  recent labels (oldest -> newest):\n");
        uint32_t oldest = (uint32_t)((cpu->labelTraceNext +
            M68K_LABEL_TRACE_DEPTH - cpu->labelTraceCount) %
            M68K_LABEL_TRACE_DEPTH);
        for (uint32_t i = 0; i < cpu->labelTraceCount; i++) {
            uint32_t idx = (oldest + i) % M68K_LABEL_TRACE_DEPTH;
            char* name = cpu->labelTraceName[idx];
            if (name == NULL) name = "(unknown)";
            fprintf(stderr, "    %u) %s (0x%08x)\n",
                    i + 1u, name, cpu->labelTracePc[idx]);
        }
    }
    if (cpu->faulted) {
        fprintf(stderr, "  last fault: %s%u at 0x%08x\n",
                cpu->faultIsWrite ? "write" : "read",
                (unsigned)cpu->faultAccessSize,
                cpu->faultAddr);
    }
    fprintf(stderr, "  a7 (sp) = 0x%08x,  memSize = 0x%08x\n",
            cpu->a[7], cpu->memSize);
    if (fitsRam(cpu, cpu->a[7], 16)) {
        uint32_t w0 = (uint32_t)cpu->mem[cpu->a[7]] << 24 |
                      (uint32_t)cpu->mem[cpu->a[7] + 1u] << 16 |
                      (uint32_t)cpu->mem[cpu->a[7] + 2u] << 8 |
                      (uint32_t)cpu->mem[cpu->a[7] + 3u];
        uint32_t w1 = (uint32_t)cpu->mem[cpu->a[7] + 4u] << 24 |
                      (uint32_t)cpu->mem[cpu->a[7] + 5u] << 16 |
                      (uint32_t)cpu->mem[cpu->a[7] + 6u] << 8 |
                      (uint32_t)cpu->mem[cpu->a[7] + 7u];
        uint32_t w2 = (uint32_t)cpu->mem[cpu->a[7] + 8u] << 24 |
                      (uint32_t)cpu->mem[cpu->a[7] + 9u] << 16 |
                      (uint32_t)cpu->mem[cpu->a[7] + 10u] << 8 |
                      (uint32_t)cpu->mem[cpu->a[7] + 11u];
        uint32_t w3 = (uint32_t)cpu->mem[cpu->a[7] + 12u] << 24 |
                      (uint32_t)cpu->mem[cpu->a[7] + 13u] << 16 |
                      (uint32_t)cpu->mem[cpu->a[7] + 14u] << 8 |
                      (uint32_t)cpu->mem[cpu->a[7] + 15u];
        fprintf(stderr, "  stack[+0..+15]: %08x %08x %08x %08x\n",
                w0, w1, w2, w3);
    }
    fprintf(stderr, "---------------------\n");
}
