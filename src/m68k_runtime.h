#ifndef M68K_RUNTIME_H
#define M68K_RUNTIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define D0 0
#define D1 1
#define D2 2
#define D3 3
#define D4 4
#define D5 5
#define D6 6
#define D7 7

#define A0 0
#define A1 1
#define A2 2
#define A3 3
#define A4 4
#define A5 5
#define A6 6
#define A7 7

#define M68K_INT_PORTS 0x0008u
#define M68K_INT_VERTB 0x0020u

#define M68K_VEC_IRQ2 0x68u
#define M68K_VEC_IRQ3 0x6Cu

#define M68K_LABEL_TRACE_DEPTH 5

typedef struct {
    uint32_t d[8];
    uint32_t a[8];
    uint32_t pcVirtual;
    uint8_t x;
    uint8_t n;
    uint8_t z;
    uint8_t v;
    uint8_t c;
    uint8_t* mem;
    uint32_t memSize;
    uint8_t ciaRegs[0x10000];
    uint8_t customRegs[0x1000];
    uint32_t agaColorRegs[256];
    uint8_t beamFramePhase;
    uint8_t beamReadCounter;
    uint8_t faulted;
    uint8_t faultIsWrite;
    uint8_t faultAccessSize;
    uint8_t blitDebugEnabled;
    uint8_t labelTracePrintEnabled;
    uint16_t intenaEnabled;
    uint16_t intreqPending;
    uint8_t keyboardInterruptPending;
    uint8_t keyboardPendingCode;
    uint32_t faultAddr;
    uint32_t lastAsmLine;
    char* lastAsmText;
    uint32_t lastLabelPc;
    char* lastLabelName;
    uint32_t labelTracePc[M68K_LABEL_TRACE_DEPTH];
    char* labelTraceName[M68K_LABEL_TRACE_DEPTH];
    uint8_t labelTraceCount;
    uint8_t labelTraceNext;
} M68k;

void m68kInit(M68k* cpu, uint8_t* mem, uint32_t memSize);
void m68kAmigaFrameTick(M68k* cpu);

uint16_t m68kAgaColor12(M68k* cpu, int index);
uint32_t m68kAgaColorArgb(M68k* cpu, int index);

/*
 * Read up to maxBytes from a host file (binary) into the emulated
 * buffer at addr.  Intended for >EXTERN in SEKA: the string there is
 * the file name, and a matching label+org in the same source fixes the
 * load address.  Missing or unreadable files: prints a one-line message
 * to stderr and does nothing.  Return value: bytes read, or 0.
 */
int m68kLoadFile(M68k* cpu, char* fileName, uint32_t addr, uint32_t maxBytes);

/*
 * Like m68kLoadFile but prepends baseFolder (with a path separator) when
 * baseFolder is non-NULL and non-empty.  Pass NULL for standalone/cwd builds.
 */
int m68kLoadFileBase(M68k* cpu, char* baseFolder, char* fileName, uint32_t addr, uint32_t maxBytes);

uint8_t  read8 (M68k* cpu, uint32_t addr);
uint16_t read16(M68k* cpu, uint32_t addr);
uint32_t read32(M68k* cpu, uint32_t addr);

/*
 * If addr lies in the linear emulated RAM backing store, returns a host
 * pointer to that byte; otherwise NULL (chip registers, CIA stub, OOB).
 * Intended for transpiler CALL (aN) operands: host reads bytes from there.
 */
uint8_t* m68kLinearRamBytePtr(M68k* cpu, uint32_t addr);

void write8 (M68k* cpu, uint32_t addr, uint8_t  value);
void write16(M68k* cpu, uint32_t addr, uint16_t value);
void write32(M68k* cpu, uint32_t addr, uint32_t value);

void push32(M68k* cpu, uint32_t value);
void push16(M68k* cpu, uint16_t value);
uint32_t pop32(M68k* cpu);
uint16_t pop16(M68k* cpu);
void m68kTraceLabel(M68k* cpu, uint32_t pc, char* labelName);

/*
 * When labelTracePrintEnabled is set (or env M68K_TRACE_LABELS is non-zero),
 * m68kTraceLabel prints each label entry to stderr as the dispatcher runs.
 */
void m68kSetLabelTracePrint(M68k* cpu, uint8_t enabled);

/* Overrides M68K_TRACE_LABELS and --trace-labels for this process. */
void m68kDisableLabelTracePrint(void);

typedef void (*M68kHostFrameBoundaryHookFn)(M68k* cpu);
void m68kSetHostFrameBoundaryHook(M68kHostFrameBoundaryHookFn fn);

/*
 * Cooperative frame hook from transpiled call 'hostFrameBoundary'.
 * Raises VERTB when enabled in INTENA and dispatches vector $6C if a handler
 * is installed. resumePc is the synthetic PC to resume after RTE (continuation).
 * Returns 1 if the interrupt was dispatched (caller should break out of switch).
 */
int m68kHostFrameBoundary(M68k* cpu, uint32_t resumePc);

/*
 * Keyboard edge from the SDL host: stage CIA-A data and dispatch vector $68
 * when PORTS is enabled in INTENA. Returns 1 if dispatched.
 */
int m68kHostKeyboardInterrupt(M68k* cpu, uint8_t amigaKeyCode);

/*
 * Stage a PORTS keyboard interrupt (SDL keydown). Dispatch happens on the
 * next transpiled dispatch-loop iteration via m68kHostPollPendingKeyboardInterrupt.
 */
void m68kHostStageKeyboardInterrupt(M68k* cpu, uint8_t amigaKeyCode);

/*
 * Returns 1 if a staged keyboard interrupt was dispatched (continue switch).
 */
int m68kHostPollPendingKeyboardInterrupt(M68k* cpu);

/*
 * Partial-register writes.
 *
 * Fundamental to 68000 semantics, not a helper:
 *   move.b #n,d0  only touches the low 8 bits of d0
 *   move.w #n,d0  only touches the low 16 bits of d0
 *   the upper bits of the data register are always preserved.
 *
 * Every instruction helper that writes a byte or word into a data
 * register must funnel through these.
 */

uint32_t setLow8 (uint32_t reg, uint8_t  value);
uint32_t setLow16(uint32_t reg, uint16_t value);

/*
 * Instruction helpers.
 *
 * Naming carve-out: uppercase-with-underscores to stay shaped like
 * 68000 opcodes when sitting next to the original asm line comment.
 *
 * Suffix encoding:
 *   _B / _W / _L   byte / word / long operand size
 *   _imm           immediate source operand
 *   _D / _A        data / address register destination
 */

void MOVE_W_imm_D(M68k* cpu, uint16_t value, int regIdx);
void MOVE_B_imm_D(M68k* cpu, uint8_t value, int regIdx);
void MOVE_B_imm_abs(M68k* cpu, uint8_t value, uint32_t address);
void MOVE_B_imm_dA(M68k* cpu, uint8_t value, int32_t disp, int baseAddrReg);
void MOVE_B_imm_indA(M68k* cpu, uint8_t value, int baseAddrReg);
void MOVE_B_imm_piA(M68k* cpu, uint8_t value, int addrReg);
void MOVE_W_imm_abs(M68k* cpu, uint16_t value, uint32_t address);
void MOVE_W_imm_dA(M68k* cpu, uint16_t value, int32_t disp, int baseAddrReg);
void MOVE_W_imm_indA(M68k* cpu, uint16_t value, int baseAddrReg);
void MOVE_W_imm_piA(M68k* cpu, uint16_t value, int addrReg);
void MOVE_W_imm_A(M68k* cpu, uint16_t value, int dstAddrReg);
void MOVE_L_imm_D(M68k* cpu, uint32_t value, int regIdx);
void MOVE_L_imm_abs(M68k* cpu, uint32_t value, uint32_t address);
void MOVE_L_imm_dA(M68k* cpu, uint32_t value, int32_t disp, int baseAddrReg);
void MOVE_L_imm_indA(M68k* cpu, uint32_t value, int baseAddrReg);
void MOVE_L_imm_piA(M68k* cpu, uint32_t value, int baseAddrReg);
void MOVE_L_imm_A(M68k* cpu, uint32_t value, int regIdx);
void MOVEQ_imm_D(M68k* cpu, uint32_t value, int regIdx);

void MOVE_W_abs_D(M68k* cpu, uint32_t address, int regIdx);
void MOVE_W_abs_abs(M68k* cpu, uint32_t srcAddress, uint32_t dstAddress);
void MOVE_W_abs_indA(M68k* cpu, uint32_t srcAddress, int dstAddrReg);
void MOVE_W_abs_piA(M68k* cpu, uint32_t srcAddress, int dstAddrReg);
void MOVE_W_abs_dA(M68k* cpu, uint32_t srcAddress, int32_t disp, int baseAddrReg);
void MOVE_W_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int regIdx);
void MOVE_W_dA_A(M68k* cpu, int32_t disp, int baseAddrReg, int dstAddrReg);
void MOVE_W_dA_abs(M68k* cpu, int32_t disp, int baseAddrReg, uint32_t dstAddress);
void MOVE_W_dAIx_D(M68k* cpu,
                   int32_t disp,
                   int baseAddrReg,
                   int indexReg,
                   int indexIsAddr,
                   int indexSize, int indexScale,
                   int regIdx);
void MOVE_W_indA_D(M68k* cpu, int baseAddrReg, int regIdx);
void MOVE_W_piA_D(M68k* cpu, int baseAddrReg, int regIdx);
void MOVE_W_pdA_D(M68k* cpu, int srcAddrReg, int dstDataReg);
void MOVE_W_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void MOVE_W_D_abs(M68k* cpu, int srcDataReg, uint32_t address);
void MOVE_W_D_dA(M68k* cpu, int srcDataReg, int32_t disp, int baseAddrReg);
void MOVE_W_dA_dA(M68k* cpu,
                  int32_t srcDisp,
                  int srcBaseAddrReg,
                  int32_t dstDisp,
                  int dstBaseAddrReg);
void MOVE_W_indA_indA(M68k* cpu, int srcAddrReg, int dstAddrReg);
void MOVE_W_indA_piA(M68k* cpu, int srcAddrReg, int dstAddrReg);
void MOVE_W_piA_indA(M68k* cpu, int srcAddrReg, int dstAddrReg);
void MOVE_W_piA_piA(M68k* cpu, int srcAddrReg, int dstAddrReg);
void MOVE_W_dA_indA(M68k* cpu, int32_t disp, int srcBaseAddrReg, int dstAddrReg);
void MOVE_W_dA_piA(M68k* cpu, int32_t disp, int srcBaseAddrReg, int dstAddrReg);
void MOVE_W_indA_dA(M68k* cpu, int srcAddrReg, int32_t dstDisp, int dstBaseAddrReg);
void MOVE_W_indA_abs(M68k* cpu, int srcAddrReg, uint32_t dstAddress);
void MOVE_W_D_pdA(M68k* cpu, int srcDataReg, int dstAddrReg);
void MOVE_W_D_dAIx(M68k* cpu,
                   int srcDataReg,
                   int32_t disp,
                   int baseAddrReg,
                   int indexReg,
                   int indexIsAddr,
                   int indexSize, int indexScale);
void MOVE_W_abs_pdA(M68k* cpu, uint32_t srcAddress, int dstAddrReg);
void MOVE_W_A_abs(M68k* cpu, int srcAddrReg, uint32_t dstAddress);
void MOVE_W_dAIx_dA(M68k* cpu,
                    int32_t srcDisp,
                    int srcBaseAddrReg,
                    int indexReg,
                    int indexIsAddr,
                    int indexSize, int indexScale,
                    int32_t dstDisp,
                    int dstBaseAddrReg);
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
                      int dstIndexSize, int dstIndexScale);
void MOVE_W_abs_dAIx(M68k* cpu,
                     uint32_t srcAddress,
                     int32_t dstDisp,
                     int dstBaseAddrReg,
                     int indexReg,
                     int indexIsAddr,
                     int indexSize, int indexScale);
void MOVE_W_imm_dAIx(M68k* cpu,
                     uint16_t value,
                     int32_t dstDisp,
                     int dstBaseAddrReg,
                     int indexReg,
                     int indexIsAddr,
                     int indexSize, int indexScale);
void MOVE_W_dAIx_abs(M68k* cpu,
                     int32_t srcDisp,
                     int srcBaseAddrReg,
                     int indexReg,
                     int indexIsAddr,
                     int indexSize, int indexScale,
                     uint32_t dstAddress);
void MOVE_W_pdA_indA(M68k* cpu, int srcAddrReg, int dstAddrReg);
void MOVE_W_D_indA(M68k* cpu, int srcDataReg, int addrReg);
void MOVE_W_D_piA(M68k* cpu, int srcDataReg, int addrReg);
void MOVE_W_piA_dA(M68k* cpu, int srcAddrReg, int32_t disp, int dstBaseAddrReg);
void MOVE_W_piA_abs(M68k* cpu, int srcAddrReg, uint32_t dstAddress);
void MOVE_W_dAIx_indA(M68k* cpu,
                      int32_t disp,
                      int baseAddrReg,
                      int indexReg,
                      int indexIsAddr,
                      int indexSize, int indexScale,
                      int dstAddrReg);
void MOVE_B_abs_D(M68k* cpu, uint32_t address, int regIdx);
void MOVE_B_abs_abs(M68k* cpu, uint32_t srcAddress, uint32_t dstAddress);
void MOVE_B_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int regIdx);
void MOVE_B_dAIx_D(M68k* cpu,
                   int32_t disp,
                   int baseAddrReg,
                   int indexReg,
                   int indexIsAddr,
                   int indexSize, int indexScale,
                   int regIdx);
void MOVE_B_absIx_D(M68k* cpu,
                    uint32_t baseAddress,
                    int indexReg,
                    int indexIsAddr,
                    int indexSize, int indexScale,
                    int regIdx);
void MOVE_B_indA_D(M68k* cpu, int baseAddrReg, int regIdx);
void MOVE_B_piA_D(M68k* cpu, int baseAddrReg, int regIdx);
void MOVE_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void MOVE_B_D_abs(M68k* cpu, int srcDataReg, uint32_t address);
void MOVE_B_D_dA(M68k* cpu, int srcDataReg, int32_t disp, int baseAddrReg);
void MOVE_B_D_indA(M68k* cpu, int srcDataReg, int addrReg);
void MOVE_B_D_piA(M68k* cpu, int srcDataReg, int addrReg);
void MOVE_B_D_pdA(M68k* cpu, int srcDataReg, int addrReg);
void MOVE_B_indA_indA(M68k* cpu, int srcAddrReg, int dstAddrReg);
void MOVE_B_indA_piA(M68k* cpu, int srcAddrReg, int dstAddrReg);
void MOVE_B_piA_indA(M68k* cpu, int srcAddrReg, int dstAddrReg);
void MOVE_B_piA_piA(M68k* cpu, int srcAddrReg, int dstAddrReg);
void MOVE_B_dA_dA(M68k* cpu,
                  int32_t srcDisp,
                  int srcBaseAddrReg,
                  int32_t dstDisp,
                  int dstBaseAddrReg);
void MOVE_B_dA_piA(M68k* cpu, int32_t disp, int srcBaseAddrReg, int dstAddrReg);
void MOVE_B_indA_dA(M68k* cpu, int srcAddrReg, int32_t dstDisp, int dstBaseAddrReg);
void MOVE_B_piA_dA(M68k* cpu, int srcAddrReg, int32_t disp, int dstBaseAddrReg);
void MOVE_B_dAIx_indA(M68k* cpu,
                      int32_t disp,
                      int baseAddrReg,
                      int indexReg,
                      int indexIsAddr,
                      int indexSize, int indexScale,
                      int dstAddrReg);
void MOVE_B_dAIx_abs(M68k* cpu,
                     int32_t disp,
                     int baseAddrReg,
                     int indexReg,
                     int indexIsAddr,
                     int indexSize, int indexScale,
                     uint32_t dstAddress);
void MOVE_B_D_dAIx(M68k* cpu,
                   int srcDataReg,
                   int32_t disp,
                   int baseAddrReg,
                   int indexReg,
                   int indexIsAddr,
                   int indexSize, int indexScale);
void MOVE_L_abs_D(M68k* cpu, uint32_t address, int regIdx);
void MOVE_L_abs_A(M68k* cpu, uint32_t address, int regIdx);
void ADD_L_abs_A(M68k* cpu, uint32_t address, int regIdx);
void ADD_L_imm_D(M68k* cpu, uint32_t value, int dstDataReg);
void ADD_L_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void ADD_L_A_D(M68k* cpu, int srcAddrReg, int dstDataReg);
void ADD_L_D_abs(M68k* cpu, int srcDataReg, uint32_t address);
void ADD_L_D_dA(M68k* cpu, int srcDataReg, int32_t disp, int baseAddrReg);
void ADD_L_imm_abs(M68k* cpu, uint32_t value, uint32_t address);
void ADD_L_imm_dA(M68k* cpu, uint32_t value, int32_t disp, int baseAddrReg);
void ADD_L_imm_piA(M68k* cpu, uint32_t value, int addrReg);
void ADD_L_D_indA(M68k* cpu, int srcDataReg, int addrReg);
void ADD_L_abs_D(M68k* cpu, uint32_t address, int dstDataReg);
void ADD_L_indA_D(M68k* cpu, int srcAddrReg, int dstDataReg);
void ADD_L_piA_D(M68k* cpu, int srcAddrReg, int dstDataReg);
void ADD_L_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int dstDataReg);
void ADD_L_dAIx_D(M68k* cpu,
                  int32_t disp,
                  int baseAddrReg,
                  int indexReg,
                  int indexIsAddr,
                  int indexSize, int indexScale,
                  int dstDataReg);
void ADD_L_D_A(M68k* cpu, int srcDataReg, int dstAddrReg);
void ADD_L_A_A(M68k* cpu, int srcAddrReg, int dstAddrReg);
void ADD_L_indA_A(M68k* cpu, int srcAddrReg, int dstAddrReg);
void ADD_L_piA_A(M68k* cpu, int srcAddrReg, int dstAddrReg);
void ADD_L_dA_A(M68k* cpu, int32_t disp, int baseAddrReg, int dstAddrReg);
void ADD_L_dAIx_A(M68k* cpu,
                  int32_t disp,
                  int baseAddrReg,
                  int indexReg,
                  int indexIsAddr,
                  int indexSize, int indexScale,
                  int dstAddrReg);
void ADD_L_imm_A(M68k* cpu, uint32_t value, int dstAddrReg);
void SUB_L_imm_A(M68k* cpu, uint32_t value, int regIdx);
void SUBA_L_D_A(M68k* cpu, int srcDataReg, int dstAddrReg);
void SUB_L_imm_D(M68k* cpu, uint32_t value, int dstDataReg);
void SUB_L_imm_abs(M68k* cpu, uint32_t value, uint32_t address);
void SUB_L_imm_dA(M68k* cpu, uint32_t value, int32_t disp, int baseAddrReg);
void SUB_L_imm_indA(M68k* cpu, uint32_t value, int baseAddrReg);
void SUB_L_imm_piA(M68k* cpu, uint32_t value, int addrReg);
void SUB_L_A_D(M68k* cpu, int srcAddrReg, int dstDataReg);
void SUB_L_piA_D(M68k* cpu, int srcAddrReg, int dstDataReg);
void SUB_L_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int dstDataReg);
void SUB_L_dAIx_D(M68k* cpu,
                  int32_t disp,
                  int baseAddrReg,
                  int indexReg,
                  int indexIsAddr,
                  int indexSize, int indexScale,
                  int dstDataReg);
void SUB_L_abs_D(M68k* cpu, uint32_t address, int dstDataReg);
void SUB_L_indA_D(M68k* cpu, int baseAddrReg, int dstDataReg);
void SUB_L_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void SUB_L_D_abs(M68k* cpu, int srcDataReg, uint32_t address);
void SUB_L_D_dA(M68k* cpu, int srcDataReg, int32_t disp, int baseAddrReg);
void SUB_L_D_indA(M68k* cpu, int srcDataReg, int addrReg);
void SUB_L_D_A(M68k* cpu, int srcDataReg, int dstAddrReg);
void SUB_L_abs_A(M68k* cpu, uint32_t address, int dstAddrReg);
void SUB_L_indA_A(M68k* cpu, int srcAddrReg, int dstAddrReg);
void SUB_L_piA_A(M68k* cpu, int srcAddrReg, int dstAddrReg);
void SUB_L_dA_A(M68k* cpu, int32_t disp, int baseAddrReg, int dstAddrReg);
void SUB_L_dAIx_A(M68k* cpu,
                  int32_t disp,
                  int baseAddrReg,
                  int indexReg,
                  int indexIsAddr,
                  int indexSize, int indexScale,
                  int dstAddrReg);
void MOVE_L_abs_abs(M68k* cpu, uint32_t srcAddress, uint32_t dstAddress);
void MOVE_L_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int regIdx);
void MOVE_L_dA_dA(M68k* cpu,
                  int32_t srcDisp,
                  int srcBaseAddrReg,
                  int32_t dstDisp,
                  int dstBaseAddrReg);
void MOVE_L_dAIx_D(M68k* cpu,
                   int32_t disp,
                   int baseAddrReg,
                   int indexReg,
                   int indexIsAddr,
                   int indexSize, int indexScale,
                   int regIdx);
void MOVE_L_indA_D(M68k* cpu, int baseAddrReg, int regIdx);
void MOVE_L_piA_D(M68k* cpu, int baseAddrReg, int regIdx);
void MOVE_L_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void MOVE_L_D_abs(M68k* cpu, int srcDataReg, uint32_t address);
void MOVE_L_A_abs(M68k* cpu, int srcAddrReg, uint32_t address);
void MOVE_L_A_dA(M68k* cpu, int srcAddrReg, int32_t disp, int baseAddrReg);
void MOVE_L_D_dA(M68k* cpu, int srcDataReg, int32_t disp, int baseAddrReg);
void MOVE_L_D_indA(M68k* cpu, int srcDataReg, int addrReg);
void MOVE_L_A_D(M68k* cpu, int srcAddrReg, int dstDataReg);
void MOVE_L_A_indA(M68k* cpu, int srcAddrReg, int dstAddrReg);
void MOVE_L_D_piA(M68k* cpu, int srcDataReg, int addrReg);
void MOVE_L_dA_A(M68k* cpu, int32_t disp, int baseAddrReg, int dstAddrReg);
void MOVE_L_piA_dA(M68k* cpu, int srcAddrReg, int32_t disp, int dstBaseAddrReg);
void MOVE_L_dA_abs(M68k* cpu, int32_t disp, int baseAddrReg, uint32_t dstAddress);
void MOVE_L_piA_abs(M68k* cpu, int srcAddrReg, uint32_t dstAddress);
void MOVE_L_piA_piA(M68k* cpu, int srcAddrReg, int dstAddrReg);
void MOVE_L_pdA_D(M68k* cpu, int srcAddrReg, int dstDataReg);
void MOVE_L_dA_indA(M68k* cpu, int32_t disp, int srcBaseAddrReg, int dstAddrReg);
void MOVE_L_indA_indA(M68k* cpu, int srcAddrReg, int dstAddrReg);
void MOVE_L_indA_abs(M68k* cpu, int srcAddrReg, uint32_t dstAddress);
void MOVE_L_indA_dA(M68k* cpu, int srcAddrReg, int32_t dstDisp, int dstBaseAddrReg);
void MOVE_L_piA_A(M68k* cpu, int srcAddrReg, int dstAddrReg);
void MOVE_L_D_A(M68k* cpu, int srcDataReg, int dstAddrReg);
void MOVE_L_A_A(M68k* cpu, int srcAddrReg, int dstAddrReg);
void MOVE_L_D_pdA(M68k* cpu, int srcDataReg, int dstAddrReg);
void MOVE_L_A_pdA(M68k* cpu, int srcAddrReg, int dstAddrReg);
void MOVE_L_abs_dA(M68k* cpu, uint32_t srcAddress, int32_t dstDisp, int dstBaseAddrReg);
void MOVE_L_abs_pdA(M68k* cpu, uint32_t srcAddress, int dstAddrReg);
void MOVE_L_indA_A(M68k* cpu, int srcAddrReg, int dstAddrReg);
void MOVE_L_indA_piA(M68k* cpu, int srcAddrReg, int dstAddrReg);
void MOVE_L_piA_indA(M68k* cpu, int srcAddrReg, int dstAddrReg);
void MOVE_L_abs_indA(M68k* cpu, uint32_t srcAddress, int dstAddrReg);
void MOVE_L_dAIx_dA(M68k* cpu,
                    int32_t srcDisp,
                    int srcBaseAddrReg,
                    int indexReg,
                    int indexIsAddr,
                    int indexSize, int indexScale,
                    int32_t dstDisp,
                    int dstBaseAddrReg);
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
                      int dstIndexSize, int dstIndexScale);
void MOVE_L_abs_dAIx(M68k* cpu,
                     uint32_t srcAddress,
                     int32_t dstDisp,
                     int dstBaseAddrReg,
                     int indexReg,
                     int indexIsAddr,
                     int indexSize, int indexScale);
void MOVE_L_imm_dAIx(M68k* cpu,
                     uint32_t value,
                     int32_t dstDisp,
                     int dstBaseAddrReg,
                     int indexReg,
                     int indexIsAddr,
                     int indexSize, int indexScale);
void MOVE_L_dAIx_abs(M68k* cpu,
                     int32_t srcDisp,
                     int srcBaseAddrReg,
                     int indexReg,
                     int indexIsAddr,
                     int indexSize, int indexScale,
                     uint32_t dstAddress);
void MOVE_L_dAIx_indA(M68k* cpu,
                      int32_t disp,
                      int baseAddrReg,
                      int indexReg,
                      int indexIsAddr,
                      int indexSize, int indexScale,
                      int dstAddrReg);
void MOVE_L_abs_piA(M68k* cpu, uint32_t srcAddress, int dstAddrReg);
void MOVE_L_dA_piA(M68k* cpu, int32_t disp, int srcBaseAddrReg, int dstAddrReg);
void MOVE_L_pdA_indA(M68k* cpu, int srcAddrReg, int dstAddrReg);
void MOVE_L_D_dAIx(M68k* cpu,
                   int srcDataReg,
                   int32_t disp,
                   int baseAddrReg,
                   int indexReg,
                   int indexIsAddr,
                   int indexSize, int indexScale);
void MOVE_L_A_piA(M68k* cpu, int srcAddrReg, int dstAddrReg);

/*
 * MOVE_L with source d(An,Rn.size) -> destination An.
 * This is movea.l semantics, so no flags are affected.
 *
 * indexIsAddr: 0 = data register index (Dn), 1 = address register index (An)
 * indexSize:   'w' for sign-extended word index, 'l' for 32-bit index
 */
void MOVE_L_dAIx_A(M68k* cpu,
                   int32_t disp,
                   int baseAddrReg,
                   int indexReg,
                   int indexIsAddr,
                   int indexSize, int indexScale,
                   int dstAddrReg);

void CMP_W_imm_D (M68k* cpu, uint16_t value, int regIdx);
void CMP_L_imm_D(M68k* cpu, uint32_t value, int regIdx);
void CMP_L_imm_A(M68k* cpu, uint32_t value, int regIdx);
void CMP_L_imm_abs(M68k* cpu, uint32_t value, uint32_t address);
void CMP_L_imm_indA(M68k* cpu, uint32_t value, int baseAddrReg);
void CMP_L_imm_dA(M68k* cpu, uint32_t value, int32_t disp, int baseAddrReg);
void CMP_L_imm_dAIx(M68k* cpu,
                    uint32_t value,
                    int32_t disp,
                    int baseAddrReg,
                    int indexReg,
                    int indexIsAddr,
                    int indexSize, int indexScale);
void CMP_L_imm_piA(M68k* cpu, uint32_t value, int baseAddrReg);
void CMP_L_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int dstDataReg);
void CMP_L_abs_D(M68k* cpu, uint32_t address, int dstDataReg);
void CMP_L_abs_A(M68k* cpu, uint32_t address, int dstAddrReg);
void CMP_L_indA_D(M68k* cpu, int srcAddrReg, int dstDataReg);
void CMP_L_piA_D(M68k* cpu, int srcAddrReg, int dstDataReg);
void CMP_L_dAIx_D(M68k* cpu,
                  int32_t disp,
                  int baseAddrReg,
                  int indexReg,
                  int indexIsAddr,
                  int indexSize, int indexScale,
                  int dstDataReg);
void CMP_L_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void CMP_L_A_A(M68k* cpu, int srcAddrReg, int dstAddrReg);
void CMP_W_imm_abs(M68k* cpu, uint16_t value, uint32_t address);
void CMP_W_imm_indA(M68k* cpu, uint16_t value, int baseAddrReg);
void CMP_W_imm_dA(M68k* cpu, uint16_t value, int32_t disp, int baseAddrReg);
void CMP_W_imm_dAIx(M68k* cpu,
                    uint16_t value,
                    int32_t disp,
                    int baseAddrReg,
                    int indexReg,
                    int indexIsAddr,
                    int indexSize, int indexScale);
void CMP_W_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int dstDataReg);
void CMP_W_abs_D(M68k* cpu, uint32_t address, int dstDataReg);
void CMP_W_indA_D(M68k* cpu, int srcAddrReg, int dstDataReg);
void CMP_W_piA_D(M68k* cpu, int srcAddrReg, int dstDataReg);
void CMP_W_imm_piA(M68k* cpu, uint16_t value, int baseAddrReg);
void CMP_W_D_D   (M68k* cpu, int srcDataReg, int dstDataReg);
void CMP_B_imm_D(M68k* cpu, uint8_t value, int regIdx);
void CMP_B_imm_abs(M68k* cpu, uint8_t value, uint32_t address);
void CMP_B_imm_dA(M68k* cpu, uint8_t value, int32_t disp, int baseAddrReg);
void CMP_B_imm_indA(M68k* cpu, uint8_t value, int baseAddrReg);
void CMP_B_imm_piA(M68k* cpu, uint8_t value, int baseAddrReg);
void CMP_B_imm_pdA(M68k* cpu, uint8_t value, int baseAddrReg);
void CMP_B_imm_dAIx(M68k* cpu,
                    uint8_t value,
                    int32_t disp,
                    int baseAddrReg,
                    int indexReg,
                    int indexIsAddr,
                    int indexSize, int indexScale);
void CMP_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void CMP_B_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int dstDataReg);
void CMP_B_abs_D(M68k* cpu, uint32_t address, int dstDataReg);
void CMP_B_indA_D(M68k* cpu, int srcAddrReg, int dstDataReg);
void CMP_B_piA_D(M68k* cpu, int srcAddrReg, int dstDataReg);
void CMP_B_dAIx_D(M68k* cpu,
                  int32_t disp,
                  int baseAddrReg,
                  int indexReg,
                  int indexIsAddr,
                  int indexSize, int indexScale,
                  int dstDataReg);

void TST_W_D     (M68k* cpu, int regIdx);
void TST_W_abs(M68k* cpu, uint32_t address);
void TST_W_indA(M68k* cpu, int baseAddrReg);
void TST_W_piA(M68k* cpu, int baseAddrReg);
void TST_W_dA(M68k* cpu, int32_t disp, int baseAddrReg);
void TST_W_dAIx(M68k* cpu,
                int32_t disp,
                int baseAddrReg,
                int indexReg,
                int indexIsAddr,
                int indexSize, int indexScale);

void TST_B_D(M68k* cpu, int regIdx);
void TST_B_abs(M68k* cpu, uint32_t address);
void TST_B_indA(M68k* cpu, int baseAddrReg);
void TST_B_dA(M68k* cpu, int32_t disp, int baseAddrReg);

void TST_L_D(M68k* cpu, int regIdx);
void TST_L_abs(M68k* cpu, uint32_t address);
void TST_L_indA(M68k* cpu, int baseAddrReg);
void TST_L_dA(M68k* cpu, int32_t disp, int baseAddrReg);
void TST_L_piA(M68k* cpu, int baseAddrReg);

/*
 * 68k condition-code predicates.
 *
 * One function per 68k conditional suffix. Each reads only the flag
 * fields of cpu and returns 1 if the condition is true, 0 otherwise.
 *
 * These are the single authoritative source of truth for the
 * Bcc family. The code generator emits calls of the shape
 *
 *     if (CC_GT(cpu)) { cpu->pcVirtual = PC_target; break; }
 *
 * instead of inlining flag expressions, so that:
 *   - the condition logic is unit-testable in one place
 *   - the emitted C reads in the same instruction-shaped style as
 *     the rest of the helpers
 *
 * Synonyms on 68k:
 *   CC_CS == CC_LO   (lower, unsigned)
 *   CC_CC == CC_HS   (same-or-higher, unsigned)
 * Aliases are implemented as separate functions so call-site names
 * mirror whichever spelling the original assembly used.
 */
int CC_EQ(M68k* cpu);
int CC_NE(M68k* cpu);
int CC_CS(M68k* cpu);
int CC_LO(M68k* cpu);
int CC_CC(M68k* cpu);
int CC_HS(M68k* cpu);
int CC_MI(M68k* cpu);
int CC_PL(M68k* cpu);
int CC_VS(M68k* cpu);
int CC_VC(M68k* cpu);
int CC_GE(M68k* cpu);
int CC_LT(M68k* cpu);
int CC_GT(M68k* cpu);
int CC_LE(M68k* cpu);
int CC_HI(M68k* cpu);
int CC_LS(M68k* cpu);

void ADDQ_W_imm_D(M68k* cpu, uint16_t value, int regIdx);
void SUBQ_W_imm_D(M68k* cpu, uint16_t value, int regIdx);
void ADDQ_W_imm_A(M68k* cpu, uint16_t value, int regIdx);
void ADDQ_L_imm_A(M68k* cpu, uint32_t value, int regIdx);
void ADDQ_L_imm_D(M68k* cpu, uint32_t value, int regIdx);
void SUBQ_W_imm_A(M68k* cpu, uint16_t value, int regIdx);
void SUBQ_L_imm_A(M68k* cpu, uint32_t value, int regIdx);
void SUBQ_L_imm_D(M68k* cpu, uint32_t value, int regIdx);
void ADDQ_W_imm_abs(M68k* cpu, uint16_t value, uint32_t address);
void SUBQ_W_imm_abs(M68k* cpu, uint16_t value, uint32_t address);
void ADDQ_W_imm_dA(M68k* cpu, uint16_t value, int32_t disp, int baseAddrReg);
void SUBQ_W_imm_dA(M68k* cpu, uint16_t value, int32_t disp, int baseAddrReg);
void ADDQ_W_imm_indA(M68k* cpu, uint16_t value, int baseAddrReg);
void SUBQ_W_imm_indA(M68k* cpu, uint16_t value, int baseAddrReg);

void ADD_W_imm_D(M68k* cpu, uint16_t value, int dstDataReg);
void ADD_W_imm_abs(M68k* cpu, uint16_t value, uint32_t address);
void ADD_W_imm_dA(M68k* cpu, uint16_t value, int32_t disp, int baseAddrReg);
void ADD_W_imm_indA(M68k* cpu, uint16_t value, int baseAddrReg);
void ADD_W_imm_piA(M68k* cpu, uint16_t value, int baseAddrReg);
void ADD_W_imm_A(M68k* cpu, uint16_t value, int dstAddrReg);
void ADD_W_D_D  (M68k* cpu, int srcDataReg, int dstDataReg);
void ADD_W_D_A(M68k* cpu, int srcDataReg, int dstAddrReg);
void ADD_W_abs_D(M68k* cpu, uint32_t address, int dstDataReg);
void ADD_W_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int dstDataReg);
void ADD_W_indA_D(M68k* cpu, int baseAddrReg, int dstDataReg);
void ADD_W_piA_D(M68k* cpu, int baseAddrReg, int dstDataReg);
void ADD_W_dAIx_D(M68k* cpu,
                  int32_t disp,
                  int baseAddrReg,
                  int indexReg,
                  int indexIsAddr,
                  int indexSize, int indexScale,
                  int dstDataReg);
void ADD_W_D_dA(M68k* cpu, int srcDataReg, int32_t disp, int baseAddrReg);
void ADD_W_D_indA(M68k* cpu, int srcDataReg, int addrReg);
void ADD_W_D_piA(M68k* cpu, int srcDataReg, int addrReg);
void ADD_W_D_abs(M68k* cpu, int srcDataReg, uint32_t address);
void ADD_W_imm_dAIx(M68k* cpu,
                    uint16_t value,
                    int32_t disp,
                    int baseAddrReg,
                    int indexReg,
                    int indexIsAddr,
                    int indexSize, int indexScale);

void ADD_B_imm_D(M68k* cpu, uint8_t value, int dstDataReg);
void ADD_B_imm_abs(M68k* cpu, uint8_t value, uint32_t address);
void ADD_B_imm_dA(M68k* cpu, uint8_t value, int32_t disp, int baseAddrReg);
void ADD_B_imm_indA(M68k* cpu, uint8_t value, int baseAddrReg);
void ADD_B_imm_piA(M68k* cpu, uint8_t value, int baseAddrReg);
void ADD_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void ADD_B_abs_D(M68k* cpu, uint32_t address, int dstDataReg);
void ADD_B_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int dstDataReg);
void ADD_B_indA_D(M68k* cpu, int baseAddrReg, int dstDataReg);
void ADD_B_pdA_D(M68k* cpu, int baseAddrReg, int dstDataReg);
void ADD_B_D_dA(M68k* cpu, int srcDataReg, int32_t disp, int baseAddrReg);
void ADD_B_D_indA(M68k* cpu, int srcDataReg, int addrReg);
void ADD_B_D_pdA(M68k* cpu, int srcDataReg, int addrReg);
void ADD_B_D_abs(M68k* cpu, int srcDataReg, uint32_t address);

void SUB_W_imm_D(M68k* cpu, uint16_t value, int dstDataReg);
void SUB_W_imm_abs(M68k* cpu, uint16_t value, uint32_t address);
void SUB_W_imm_dA(M68k* cpu, uint16_t value, int32_t disp, int baseAddrReg);
void SUB_W_imm_indA(M68k* cpu, uint16_t value, int baseAddrReg);
void SUB_W_imm_piA(M68k* cpu, uint16_t value, int addrReg);
void SUB_W_imm_dAIx(M68k* cpu,
                    uint16_t value,
                    int32_t disp,
                    int baseAddrReg,
                    int indexReg,
                    int indexIsAddr,
                    int indexSize, int indexScale);
void SUB_W_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void SUB_W_abs_D(M68k* cpu, uint32_t address, int dstDataReg);
void SUB_W_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int dstDataReg);
void SUB_W_dAIx_D(M68k* cpu,
                  int32_t disp,
                  int baseAddrReg,
                  int indexReg,
                  int indexIsAddr,
                  int indexSize, int indexScale,
                  int dstDataReg);
void SUB_W_indA_D(M68k* cpu, int baseAddrReg, int dstDataReg);
void SUB_W_piA_D(M68k* cpu, int baseAddrReg, int dstDataReg);
void SUB_W_D_abs(M68k* cpu, int srcDataReg, uint32_t address);
void SUB_W_D_dA(M68k* cpu, int srcDataReg, int32_t disp, int baseAddrReg);
void SUB_W_D_indA(M68k* cpu, int srcDataReg, int addrReg);
void SUB_W_D_piA(M68k* cpu, int srcDataReg, int addrReg);

void SUB_B_imm_D(M68k* cpu, uint8_t value, int dstDataReg);
void SUB_B_imm_abs(M68k* cpu, uint8_t value, uint32_t address);
void SUB_B_imm_dA(M68k* cpu, uint8_t value, int32_t disp, int baseAddrReg);
void SUB_B_imm_indA(M68k* cpu, uint8_t value, int baseAddrReg);
void SUB_B_imm_dAIx(M68k* cpu,
                    uint8_t value,
                    int32_t disp,
                    int baseAddrReg,
                    int indexReg,
                    int indexIsAddr,
                    int indexSize, int indexScale);
void SUB_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void SUB_B_abs_D(M68k* cpu, uint32_t address, int dstDataReg);
void SUB_B_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int dstDataReg);
void SUB_B_indA_D(M68k* cpu, int baseAddrReg, int dstDataReg);
void SUB_B_pdA_D(M68k* cpu, int baseAddrReg, int dstDataReg);
void SUB_B_D_abs(M68k* cpu, int srcDataReg, uint32_t address);
void SUB_B_D_dA(M68k* cpu, int srcDataReg, int32_t disp, int baseAddrReg);
void SUB_B_D_indA(M68k* cpu, int srcDataReg, int addrReg);
void SUB_B_D_pdA(M68k* cpu, int srcDataReg, int addrReg);

void AND_W_imm_D(M68k* cpu, uint16_t value, int dstDataReg);
void AND_W_imm_abs(M68k* cpu, uint16_t value, uint32_t address);
void AND_W_imm_dA(M68k* cpu, uint16_t value, int32_t disp, int baseAddrReg);
void AND_W_imm_indA(M68k* cpu, uint16_t value, int baseAddrReg);
void AND_W_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void AND_W_abs_D(M68k* cpu, uint32_t address, int dstDataReg);
void AND_W_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int dstDataReg);
void AND_W_indA_D(M68k* cpu, int baseAddrReg, int dstDataReg);
void AND_W_imm_dAIx(M68k* cpu,
                    uint16_t value,
                    int32_t disp,
                    int baseAddrReg,
                    int indexReg,
                    int indexIsAddr,
                    int indexSize, int indexScale);
void AND_B_imm_D(M68k* cpu, uint8_t value, int dstDataReg);
void AND_B_imm_abs(M68k* cpu, uint8_t value, uint32_t address);
void AND_B_imm_dA(M68k* cpu, uint8_t value, int32_t disp, int baseAddrReg);
void AND_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void AND_B_abs_D(M68k* cpu, uint32_t address, int dstDataReg);
void AND_B_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int dstDataReg);
void AND_B_indA_D(M68k* cpu, int baseAddrReg, int dstDataReg);
void AND_B_D_indA(M68k* cpu, int srcDataReg, int addrReg);
void AND_L_imm_D(M68k* cpu, uint32_t value, int dstDataReg);
void AND_L_imm_abs(M68k* cpu, uint32_t value, uint32_t address);
void AND_L_imm_dA(M68k* cpu, uint32_t value, int32_t disp, int baseAddrReg);
void OR_W_imm_D(M68k* cpu, uint16_t value, int dstDataReg);
void OR_W_imm_abs(M68k* cpu, uint16_t value, uint32_t address);
void OR_W_imm_dA(M68k* cpu, uint16_t value, int32_t disp, int baseAddrReg);
void OR_W_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void OR_W_D_indA(M68k* cpu, int srcDataReg, int addrReg);
void OR_W_D_dA(M68k* cpu, int srcDataReg, int32_t disp, int baseAddrReg);
void OR_W_D_abs(M68k* cpu, int srcDataReg, uint32_t address);
void OR_W_abs_D(M68k* cpu, uint32_t address, int dstDataReg);
void OR_W_D_dAIx(M68k* cpu,
                 int srcDataReg,
                 int32_t disp,
                 int baseAddrReg,
                 int indexReg,
                 int indexIsAddr,
                 int indexSize, int indexScale);
void OR_B_imm_D(M68k* cpu, uint8_t value, int dstDataReg);
void OR_B_imm_abs(M68k* cpu, uint8_t value, uint32_t address);
void OR_B_imm_dA(M68k* cpu, uint8_t value, int32_t disp, int baseAddrReg);
void OR_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void OR_B_abs_D(M68k* cpu, uint32_t address, int dstDataReg);
void OR_B_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int dstDataReg);
void OR_B_indA_D(M68k* cpu, int baseAddrReg, int dstDataReg);
void OR_B_D_abs(M68k* cpu, int srcDataReg, uint32_t address);
void OR_B_D_dA(M68k* cpu, int srcDataReg, int32_t disp, int baseAddrReg);
void OR_B_D_indA(M68k* cpu, int srcDataReg, int addrReg);
void EOR_W_imm_D(M68k* cpu, uint16_t value, int dstDataReg);
void EOR_W_imm_abs(M68k* cpu, uint16_t value, uint32_t address);
void EOR_W_imm_dA(M68k* cpu, uint16_t value, int32_t disp, int baseAddrReg);
void EOR_W_imm_piA(M68k* cpu, uint16_t value, int baseAddrReg);
void EOR_W_imm_indA(M68k* cpu, uint16_t value, int baseAddrReg);
void EOR_W_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void EOR_B_imm_D(M68k* cpu, uint8_t value, int dstDataReg);
void EOR_B_imm_abs(M68k* cpu, uint8_t value, uint32_t address);
void EOR_B_imm_dA(M68k* cpu, uint8_t value, int32_t disp, int baseAddrReg);
void EOR_B_imm_indA(M68k* cpu, uint8_t value, int baseAddrReg);
void EOR_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void EOR_L_imm_D(M68k* cpu, uint32_t value, int dstDataReg);
void EOR_L_imm_abs(M68k* cpu, uint32_t value, uint32_t address);
void EOR_L_imm_dA(M68k* cpu, uint32_t value, int32_t disp, int baseAddrReg);
void EOR_L_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void NEG_W_D(M68k* cpu, int dstDataReg);
void NEG_W_dA(M68k* cpu, int32_t disp, int baseAddrReg);
void NEG_W_abs(M68k* cpu, uint32_t address);
void NEG_L_D(M68k* cpu, int dstDataReg);
void NEG_L_dA(M68k* cpu, int32_t disp, int baseAddrReg);
void NEG_L_abs(M68k* cpu, uint32_t address);
void NOT_W_D(M68k* cpu, int dstDataReg);

void CLR_B_abs(M68k* cpu, uint32_t address);
void CLR_B_dA(M68k* cpu, int32_t disp, int baseAddrReg);
void CLR_B_indA(M68k* cpu, int baseAddrReg);
void CLR_B_dAIx(M68k* cpu,
                int32_t disp,
                int baseAddrReg,
                int indexReg,
                int indexIsAddr,
                int indexSize, int indexScale);

void CLR_W_abs(M68k* cpu, uint32_t address);
void CLR_W_D(M68k* cpu, int dstDataReg);
void CLR_W_dA(M68k* cpu, int32_t disp, int baseAddrReg);
void CLR_W_indA(M68k* cpu, int baseAddrReg);
void CLR_W_piA(M68k* cpu, int baseAddrReg);
void CLR_W_dAIx(M68k* cpu,
                int32_t disp,
                int baseAddrReg,
                int indexReg,
                int indexIsAddr,
                int indexSize, int indexScale);

void CLR_L_abs(M68k* cpu, uint32_t address);
void CLR_L_D(M68k* cpu, int dstDataReg);
void CLR_L_dA(M68k* cpu, int32_t disp, int baseAddrReg);
void CLR_L_indA(M68k* cpu, int baseAddrReg);
void CLR_L_piA(M68k* cpu, int baseAddrReg);
void CLR_L_dAIx(M68k* cpu,
                int32_t disp,
                int baseAddrReg,
                int indexReg,
                int indexIsAddr,
                int indexSize, int indexScale);

void BTST_B_imm_abs(M68k* cpu, uint16_t bit, uint32_t address);
void BTST_B_imm_D(M68k* cpu, uint16_t bit, int dstDataReg);
void BTST_B_imm_dA(M68k* cpu, uint16_t bit, int32_t disp, int baseAddrReg);
void BCLR_B_imm_abs(M68k* cpu, uint16_t bit, uint32_t address);
void BCLR_B_imm_D(M68k* cpu, uint16_t bit, int dstDataReg);
void BCLR_B_imm_dA(M68k* cpu, uint16_t bit, int32_t disp, int baseAddrReg);
void BSET_B_imm_abs(M68k* cpu, uint16_t bit, uint32_t address);
void BSET_B_imm_D(M68k* cpu, uint16_t bit, int dstDataReg);
void BSET_B_imm_dA(M68k* cpu, uint16_t bit, int32_t disp, int baseAddrReg);
void BTST_B_imm_indA(M68k* cpu, uint16_t bit, int baseAddrReg);
void BCLR_B_imm_indA(M68k* cpu, uint16_t bit, int baseAddrReg);
void BSET_B_imm_indA(M68k* cpu, uint16_t bit, int baseAddrReg);
void BTST_B_D_abs(M68k* cpu, int srcDataReg, uint32_t address);
void BTST_B_D_dA(M68k* cpu, int srcDataReg, int32_t disp, int baseAddrReg);
void BTST_B_D_indA(M68k* cpu, int srcDataReg, int baseAddrReg);
void BTST_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void BCLR_B_D_abs(M68k* cpu, int srcDataReg, uint32_t address);
void BCLR_B_D_dA(M68k* cpu, int srcDataReg, int32_t disp, int baseAddrReg);
void BCLR_B_D_indA(M68k* cpu, int srcDataReg, int baseAddrReg);
void BCLR_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void BSET_B_D_abs(M68k* cpu, int srcDataReg, uint32_t address);
void BSET_B_D_dA(M68k* cpu, int srcDataReg, int32_t disp, int baseAddrReg);
void BSET_B_D_indA(M68k* cpu, int srcDataReg, int baseAddrReg);
void BSET_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void BCHG_B_imm_abs(M68k* cpu, uint16_t bit, uint32_t address);
void BCHG_B_imm_dA(M68k* cpu, uint16_t bit, int32_t disp, int baseAddrReg);
void BCHG_B_imm_indA(M68k* cpu, uint16_t bit, int baseAddrReg);
void BCHG_B_imm_D(M68k* cpu, uint16_t bit, int dstDataReg);
void BCHG_B_D_abs(M68k* cpu, int srcDataReg, uint32_t address);
void BCHG_B_D_dA(M68k* cpu, int srcDataReg, int32_t disp, int baseAddrReg);
void BCHG_B_D_indA(M68k* cpu, int srcDataReg, int baseAddrReg);
void BCHG_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg);

void LEA_imm_A  (M68k* cpu, uint32_t value, int aReg);
void PEA_abs(M68k* cpu, uint32_t address);
void LEA_indA_A(M68k* cpu, int srcAddrReg, int dstAddrReg);
void LEA_dA_A(M68k* cpu, int32_t disp, int baseAddrReg, int dstAddrReg);
void LEA_dAIx_A(M68k* cpu,
                int32_t disp,
                int baseAddrReg,
                int indexReg,
                int indexIsAddr,
                int indexSize, int indexScale,
                int dstAddrReg);
void SWAP_D(M68k* cpu, int regIdx);
void EXT_W_D(M68k* cpu, int regIdx);
void EXT_L_D(M68k* cpu, int regIdx);

void ASR_W_imm_D(M68k* cpu, uint16_t count, int dstDataReg);
void ASR_W_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void ASR_W_imm_abs(M68k* cpu, uint16_t count, uint32_t dstAddress);
void ASR_W_imm_indA(M68k* cpu, uint16_t count, int dstAddrReg);
void ASR_W_imm_dA(M68k* cpu, uint16_t count, int32_t disp, int dstBaseAddrReg);
void ASR_B_imm_D(M68k* cpu, uint16_t count, int dstDataReg);
void ASR_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void ASL_B_imm_D(M68k* cpu, uint16_t count, int dstDataReg);
void ASL_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void LSR_B_imm_D(M68k* cpu, uint16_t count, int dstDataReg);
void LSR_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void LSL_B_imm_D(M68k* cpu, uint16_t count, int dstDataReg);
void LSL_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void ROR_B_imm_D(M68k* cpu, uint16_t count, int dstDataReg);
void ROR_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void ROL_B_imm_D(M68k* cpu, uint16_t count, int dstDataReg);
void ROL_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void ASR_L_imm_D(M68k* cpu, uint16_t count, int dstDataReg);
void ASL_L_imm_D(M68k* cpu, uint16_t count, int dstDataReg);
void LSR_L_imm_D(M68k* cpu, uint16_t count, int dstDataReg);
void LSL_L_imm_D(M68k* cpu, uint16_t count, int dstDataReg);
void ASL_W_imm_D(M68k* cpu, uint16_t count, int dstDataReg);
void ASL_W_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void ASL_W_imm_abs(M68k* cpu, uint16_t count, uint32_t dstAddress);
void ASL_W_imm_indA(M68k* cpu, uint16_t count, int dstAddrReg);
void ASL_W_imm_dA(M68k* cpu, uint16_t count, int32_t disp, int dstBaseAddrReg);
void LSR_W_imm_D(M68k* cpu, uint16_t count, int dstDataReg);
void LSR_W_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void LSL_W_imm_D(M68k* cpu, uint16_t count, int dstDataReg);
void LSL_W_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void ROR_W_imm_D(M68k* cpu, uint16_t count, int dstDataReg);
void ROR_W_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void ROR_W_imm_abs(M68k* cpu, uint16_t count, uint32_t dstAddress);
void ROR_W_imm_indA(M68k* cpu, uint16_t count, int dstAddrReg);
void ROR_W_imm_dA(M68k* cpu, uint16_t count, int32_t disp, int dstBaseAddrReg);
void ROL_W_imm_D(M68k* cpu, uint16_t count, int dstDataReg);
void ROL_W_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void ROL_W_imm_abs(M68k* cpu, uint16_t count, uint32_t dstAddress);
void ROL_W_imm_indA(M68k* cpu, uint16_t count, int dstAddrReg);
void ROL_W_imm_dA(M68k* cpu, uint16_t count, int32_t disp, int dstBaseAddrReg);
void ROR_L_imm_D(M68k* cpu, uint16_t count, int dstDataReg);
void ROR_L_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void ROL_L_imm_D(M68k* cpu, uint16_t count, int dstDataReg);
void ROL_L_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void ROXL_B_imm_D(M68k* cpu, uint16_t count, int dstDataReg);
void ROXL_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void ROXL_L_imm_D(M68k* cpu, uint16_t count, int dstDataReg);
void ROXL_L_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void ROXR_B_imm_D(M68k* cpu, uint16_t count, int dstDataReg);
void ROXR_B_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void ROXR_L_imm_D(M68k* cpu, uint16_t count, int dstDataReg);
void ROXR_L_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void MOVEM_L_rlist_pdA(M68k* cpu, uint16_t rlistMask, int baseAddrReg);
void MOVEM_L_rlist_dA(M68k* cpu, uint16_t rlistMask, int32_t disp, int baseAddrReg);
void MOVEM_L_piA_rlist(M68k* cpu, int baseAddrReg, uint16_t rlistMask);
void MOVEM_W_rlist_pdA(M68k* cpu, uint16_t rlistMask, int baseAddrReg);
void MOVEM_W_rlist_dA(M68k* cpu, uint16_t rlistMask, int32_t disp, int baseAddrReg);
void MOVEM_W_piA_rlist(M68k* cpu, int baseAddrReg, uint16_t rlistMask);
void EXG_DD(M68k* cpu, int reg0, int reg1);
void EXG_AA(M68k* cpu, int reg0, int reg1);
void EXG_D_A(M68k* cpu, int dataReg, int addrReg);
void RTE(M68k* cpu);
void MULU_W_imm_D(M68k* cpu, uint16_t value, int dstDataReg);
void MULU_W_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void MULU_W_abs_D(M68k* cpu, uint32_t address, int dstDataReg);
void MULU_W_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int dstDataReg);
void MULU_W_indA_D(M68k* cpu, int baseAddrReg, int dstDataReg);
void MULU_W_piA_D(M68k* cpu, int baseAddrReg, int dstDataReg);
void MULS_W_imm_D(M68k* cpu, uint16_t value, int dstDataReg);
void MULS_W_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void MULS_W_abs_D(M68k* cpu, uint32_t address, int dstDataReg);
void MULS_W_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int dstDataReg);
void MULS_W_indA_D(M68k* cpu, int baseAddrReg, int dstDataReg);
void MULS_W_piA_D(M68k* cpu, int baseAddrReg, int dstDataReg);
void DIVU_W_imm_D(M68k* cpu, uint16_t value, int dstDataReg);
void DIVU_W_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void DIVU_W_abs_D(M68k* cpu, uint32_t address, int dstDataReg);
void DIVU_W_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int dstDataReg);
void DIVU_W_indA_D(M68k* cpu, int baseAddrReg, int dstDataReg);
void DIVS_W_imm_D(M68k* cpu, uint16_t value, int dstDataReg);
void DIVS_W_D_D(M68k* cpu, int srcDataReg, int dstDataReg);
void DIVS_W_abs_D(M68k* cpu, uint32_t address, int dstDataReg);
void DIVS_W_dA_D(M68k* cpu, int32_t disp, int baseAddrReg, int dstDataReg);
void DIVS_W_indA_D(M68k* cpu, int baseAddrReg, int dstDataReg);

/*
 * DBF Dn,<label> semantics (68k "decrement and branch if false"):
 *   - decrement the low 16 bits of Dn as a signed 16-bit value
 *   - if the decremented counter is -1, fall through
 *   - otherwise, branch is taken
 *
 * Returns 1 if the caller should branch, 0 otherwise. Does not touch flags.
 */
int DBF_D(M68k* cpu, int dataReg);

/*
 * Debug aid used by the generated dispatcher's default case when it
 * hits an unhandled pcVirtual value. Dumps every register, every flag,
 * and the current pcVirtual to stderr.
 */
void dumpCpuState(M68k* cpu);

#ifdef __cplusplus
}
#endif

#endif
