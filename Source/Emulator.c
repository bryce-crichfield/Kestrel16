/*
 * ISA-16 emulator -- single file, plain C99.
 *
 * Naming: ProperCase for types, functions, constants and macros;
 *         camelCase for variables, fields and parameters.
 *
 * Build:  cc -O2 -Wall -Wextra -o isa16 main.c
 * Run:    ./isa16 prog.bin        load raw little-endian binary at 0x0000
 *         ./isa16                 run the built-in demo (prints a greeting)
 *         ./isa16 -t prog.bin     trace every instruction to stderr
 *         ./isa16 -x prog.bin     dump registers when the machine stops
 *         ./isa16 -n 1000 ...     stop after 1000 steps (0 = no limit)
 *
 * ---------------------------------------------------------------------
 * Machine model
 *   - 64 KB byte-addressed, little-endian. Word accesses ignore address bit 0.
 *   - R0-R7 general purpose (R0 is NOT hardwired to zero).
 *     Convention: R0/R1 args+return, R2/R3 temps, R4/R5 callee-saved,
 *     R6 = stack pointer, R7 = link register.
 *   - Flags: Zero, Negative, Carry, Overflow.
 *     Carry means "no borrow" after subtraction (ARM style).
 *   - Reset: PC = 0x0000.  Interrupt / trap vector: 0x0002.
 *   - I/O is memory mapped at 0xFF00-0xFFFF (see below).
 *
 * Encodings (bit 15 on the left, all instructions 16 bits)
 *   Register:  opcode4 | dest3[11:9] | sourceA3[8:6] | sourceB3[5:3] | function3[2:0]
 *   Immediate: opcode4 | dest3[11:9] | sourceA3[8:6] | immediate6[5:0]  (signed)
 *   Long:      opcode4 | dest3[11:9] | 0[8]          | immediate8[7:0]
 *   Branch:    opcode4 | condition4[11:8] | offset8[7:0]   (signed, in words)
 *   Jump:      opcode4 | offset12[11:0]                    (signed, in words)
 *   System:    opcode4 | function4[11:8] | source3[7:5] | unused[4:0]
 *   Branch/jump offsets are relative to the NEXT instruction (PC + 2).
 *
 * Opcodes
 *   0 ALU      functions: ADD SUB AND OR XOR SHL SHR SAR   dest = a op b
 *              shifts shift by (b & 15)
 *   1 ALU2     functions: ADC SBC CMP MOV NOT NEG SXB (7 = illegal)
 *              CMP sets flags from a - b, writes nothing
 *              MOV leaves flags untouched
 *   2 ADDI     dest = a + immediate6        (sets flags)
 *   3 LDI      dest = immediate8            (zero extended, flags untouched)
 *   4 LHI      dest.high = immediate8       (low byte kept)
 *   5 LW       dest = memory16[a + immediate6]
 *   6 SW       memory16[a + immediate6] = dest
 *   7 LB       dest = zeroExtend(memory8[a + immediate6])
 *   8 SB       memory8[a + immediate6] = dest.low
 *   9 Bcc      if condition: PC += offset8 * 2
 *   A J        PC += offset12 * 2
 *   B JAL      R7 = PC + 2; PC += offset12 * 2
 *   C SYS      functions: 0 JR, 1 JALR, 2 RETI, 3 EI, 4 DI, 5 HALT, 6 TRAP
 *   D CMPI     flags = a - immediate6
 *   E,F        reserved (illegal for now)
 *
 * Conditions: 0 AL, 1 EQ, 2 NE, 3 LTU, 4 GEU, 5 LT, 6 GE, 7 GT, 8 LE,
 *             9 MI, 10 PL, 11 VS, 12 VC
 *
 * Interrupts: an interrupt (when enabled) or TRAP saves PC and flags plus
 * the interrupt-enable bit into hidden exception registers, disables
 * interrupts, and jumps to 0x0002. RETI restores them.
 * HALT sleeps until an interrupt; with interrupts off (or no timer) the
 * emulator treats it as "program finished" and stops.
 *
 * I/O map (word registers)
 *   0xFF00  UART data    write: print low byte to stdout, read: next stdin byte
 *   0xFF02  UART status  bit0 = receive byte ready, bit1 = transmit ready (always 1)
 *   0xFF04  Timer        write N: raise an interrupt every N steps (0 = off)
 * ---------------------------------------------------------------------
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/select.h>
#include <unistd.h>
#define HavePosixIo 1
#elif defined(_WIN32)
#include <conio.h>
#include <io.h>
#define HaveWindowsIo 1
#endif

/* ------------------------------------------------------------------ */
/* Machine constants                                                   */

enum { RegisterCount = 8, LinkRegister = 7 };
enum { MemorySize = 65536 };
enum { WordAlignMask = 0xFFFE, ByteMask = 0xFF, SignBit16 = 0x8000, CarryOut16 = 0x10000 };

enum Flag {
    FlagZero             = 1 << 0,
    FlagNegative         = 1 << 1,
    FlagCarry            = 1 << 2,
    FlagOverflow         = 1 << 3,
    FlagInterruptEnable  = 1 << 4,  /* only used in the saved exception flags */
    FlagArithmeticMask   = FlagZero | FlagNegative | FlagCarry | FlagOverflow
};

enum Vector { VectorReset = 0x0000, VectorInterrupt = 0x0002 };

enum IoAddress {
    IoBase         = 0xFF00,
    IoUartData     = 0xFF00,
    IoUartStatus   = 0xFF02,
    IoTimer        = 0xFF04
};

enum UartStatusBit { UartReceiveReady = 1 << 0, UartTransmitReady = 1 << 1 };

enum StepResult { StepRunning = 0, StepHalted, StepIllegal, StepLimitReached };

/* ------------------------------------------------------------------ */
/* Instruction set constants                                           */

enum Opcode {
    OpcodeAlu              = 0x0,
    OpcodeAlu2             = 0x1,
    OpcodeAddImmediate     = 0x2,
    OpcodeLoadImmediate    = 0x3,
    OpcodeLoadHighImmediate= 0x4,
    OpcodeLoadWord         = 0x5,
    OpcodeStoreWord        = 0x6,
    OpcodeLoadByte         = 0x7,
    OpcodeStoreByte        = 0x8,
    OpcodeBranch           = 0x9,
    OpcodeJump             = 0xA,
    OpcodeJumpAndLink      = 0xB,
    OpcodeSystem           = 0xC,
    OpcodeCompareImmediate = 0xD
};

enum AluFunction {
    AluAdd = 0, AluSubtract, AluAnd, AluOr, AluXor,
    AluShiftLeft, AluShiftRightLogical, AluShiftRightArithmetic
};

enum Alu2Function {
    Alu2AddWithCarry = 0, Alu2SubtractWithCarry, Alu2Compare, Alu2Move,
    Alu2Not, Alu2Negate, Alu2SignExtendByte
};

enum SystemFunction {
    SystemJumpRegister = 0, SystemJumpAndLinkRegister, SystemReturnFromInterrupt,
    SystemEnableInterrupts, SystemDisableInterrupts, SystemHalt, SystemTrap
};

enum Condition {
    ConditionAlways = 0, ConditionEqual, ConditionNotEqual,
    ConditionLessUnsigned, ConditionGreaterOrEqualUnsigned,
    ConditionLess, ConditionGreaterOrEqual, ConditionGreater, ConditionLessOrEqual,
    ConditionMinus, ConditionPlus, ConditionOverflowSet, ConditionOverflowClear
};

enum ShiftKind { ShiftLeft, ShiftRightLogical, ShiftRightArithmetic };

enum { ShiftAmountMask = 15, WordBits = 16 };

/* Instruction field layout */
enum {
    OpcodeShift = 12,           OpcodeMask = 0xF,
    DestinationShift = 9,       RegisterMask = 0x7,
    SourceAShift = 6,
    SourceBShift = 3,
    FunctionMask = 0x7,
    ConditionShift = 8,         ConditionMask = 0xF,
    SystemFunctionShift = 8,    SystemFunctionMask = 0xF,
    SystemSourceShift = 5,
    Immediate6Bits = 6,
    Immediate8Bits = 8,         Immediate8Mask = 0xFF,
    Offset8Bits = 8,
    Offset12Bits = 12,          Offset12Mask = 0xFFF,
    Immediate6Mask = 0x3F,
    InstructionSize = 2
};

typedef struct {
    int opcode;
    int destination;
    int sourceA;
    int sourceB;
    int function;
    int condition;
    int systemFunction;
    int systemSource;
    int immediate6;     /* sign extended */
    int immediate8;     /* zero extended */
    int offset8;        /* sign extended, in words */
    int offset12;       /* sign extended, in words */
} Instruction;

typedef struct {
    uint16_t registers[RegisterCount];
    uint16_t programCounter;
    uint16_t exceptionProgramCounter;
    uint8_t  flags;
    uint8_t  exceptionFlags;
    int interruptsEnabled;
    int halted;
    int interruptPending;
    uint16_t timerReload;
    uint16_t timerCount;
    unsigned long long steps;
} Cpu;

static uint8_t memory[MemorySize];
static Cpu cpu;

/* ------------------------------------------------------------------ */
/* UART input: non-blocking on POSIX and a Windows console; piped     */
/* input on Windows is read as the program asks for it.                */

static int receiveHaveByte, receiveEndOfFile;
static uint8_t receiveByte;
static int stopAtEndOfInput, inputExhausted;  /* --run: input ran out while the program waited */

static int ReceivePoll(void)
{
    if (receiveHaveByte) return 1;
    if (receiveEndOfFile) return 0;
#ifdef HavePosixIo
    {
        const int standardInput = 0;
        fd_set readSet;
        struct timeval noWait = {0, 0};
        FD_ZERO(&readSet);
        FD_SET(standardInput, &readSet);
        if (select(standardInput + 1, &readSet, NULL, NULL, &noWait) > 0) {
            uint8_t character;
            ssize_t bytesRead = read(standardInput, &character, 1);
            if (bytesRead == 1) { receiveByte = character; receiveHaveByte = 1; return 1; }
            if (bytesRead == 0) receiveEndOfFile = 1;
        }
    }
#elif defined(HaveWindowsIo)
    if (_isatty(_fileno(stdin))) {
        if (_kbhit()) {
            int character = _getch();
            if (character == '\r') character = '\n';
            /* a console doesn't echo for _getch, so echo like a terminal would */
            if (character == '\n' || (character >= ' ' && character < 127)) { putchar(character); fflush(stdout); }
            if (character == 26) { receiveEndOfFile = 1; return 0; }  /* Ctrl+Z */
            receiveByte = (uint8_t)character; receiveHaveByte = 1; return 1;
        }
    } else {
        int character = getchar();  /* a pipe or file: blocks only until the next byte or EOF */
        if (character != EOF) { receiveByte = (uint8_t)character; receiveHaveByte = 1; return 1; }
        receiveEndOfFile = 1;
    }
#endif
    if (receiveEndOfFile && stopAtEndOfInput) inputExhausted = 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Memory and I/O                                                      */

static uint16_t IoRead(uint16_t address)
{
    switch (address & WordAlignMask) {
    case IoUartData:
        if (ReceivePoll()) { receiveHaveByte = 0; return receiveByte; }
        return 0;
    case IoUartStatus:
        return (uint16_t)((ReceivePoll() ? UartReceiveReady : 0) | UartTransmitReady);
    case IoTimer:
        return cpu.timerReload;
    default:
        return 0;
    }
}

static void IoWrite(uint16_t address, uint16_t value)
{
    switch (address & WordAlignMask) {
    case IoUartData:
        fputc(value & ByteMask, stdout);
        fflush(stdout);
        break;
    case IoTimer:
        cpu.timerReload = value;
        cpu.timerCount = value;
        break;
    default:
        break;
    }
}

static uint16_t ReadMemoryWord(uint16_t address)
{
    address &= WordAlignMask;
    return (uint16_t)(memory[address] | (memory[address + 1] << 8));
}

static uint16_t ReadWord(uint16_t address)
{
    address &= WordAlignMask;
    if (address >= IoBase) return IoRead(address);
    return ReadMemoryWord(address);
}

static void WriteWord(uint16_t address, uint16_t value)
{
    address &= WordAlignMask;
    if (address >= IoBase) { IoWrite(address, value); return; }
    memory[address] = (uint8_t)(value & ByteMask);
    memory[address + 1] = (uint8_t)(value >> 8);
}

static uint16_t ReadByte(uint16_t address)
{
    if (address >= IoBase) {
        uint16_t value = IoRead(address);
        return (address & 1) ? (uint16_t)(value >> 8) : (uint16_t)(value & ByteMask);
    }
    return memory[address];
}

static void WriteByte(uint16_t address, uint8_t value)
{
    if (address >= IoBase) {
        if (!(address & 1)) IoWrite(address, value);
        return;
    }
    memory[address] = value;
}

/* ------------------------------------------------------------------ */
/* ALU                                                                 */

static int SignExtend(int value, int bits)
{
    int signMask = 1 << (bits - 1);
    value &= (1 << bits) - 1;
    return (value ^ signMask) - signMask;
}

static int HasFlag(uint8_t flags, int flag) { return (flags & flag) != 0; }

static void SetZeroNegative(uint16_t result)
{
    cpu.flags &= (uint8_t)~(FlagZero | FlagNegative);
    if (result == 0) cpu.flags |= FlagZero;
    if (result & SignBit16) cpu.flags |= FlagNegative;
}

/* a + b + carryIn, sets all four flags. Subtraction is a + ~b + 1. */
static uint16_t AluAddWithFlags(uint16_t a, uint16_t b, unsigned carryIn)
{
    uint32_t sum = (uint32_t)a + b + carryIn;
    uint16_t result = (uint16_t)sum;
    SetZeroNegative(result);
    cpu.flags &= (uint8_t)~(FlagCarry | FlagOverflow);
    if (sum & CarryOut16) cpu.flags |= FlagCarry;
    if (~(a ^ b) & (a ^ result) & SignBit16) cpu.flags |= FlagOverflow;
    return result;
}

static uint16_t AluSubtractWithFlags(uint16_t a, uint16_t b, unsigned carryIn)
{
    return AluAddWithFlags(a, (uint16_t)~b, carryIn);
}

static uint16_t AluLogicWithFlags(uint16_t result)
{
    SetZeroNegative(result);
    return result;
}

/* Carry = last bit shifted out (unchanged when the amount is 0). */
static uint16_t AluShiftWithFlags(enum ShiftKind kind, uint16_t value, unsigned amount)
{
    uint16_t result = value;
    if (amount) {
        int carry;
        if (kind == ShiftLeft) {
            carry = (value >> (WordBits - amount)) & 1;
            result = (uint16_t)(value << amount);
        } else {
            carry = (value >> (amount - 1)) & 1;
            result = (uint16_t)(value >> amount);
            if (kind == ShiftRightArithmetic && (value & SignBit16))
                result |= (uint16_t)(0xFFFFu << (WordBits - amount));
        }
        cpu.flags = (uint8_t)((cpu.flags & ~FlagCarry) | (carry ? FlagCarry : 0));
    }
    SetZeroNegative(result);
    return result;
}

static uint16_t SignExtendByte(uint16_t value)
{
    return (value & 0x80) ? (uint16_t)(value | 0xFF00) : (uint16_t)(value & ByteMask);
}

static int ConditionHolds(int condition, uint8_t flags)
{
    int zero = HasFlag(flags, FlagZero), negative = HasFlag(flags, FlagNegative);
    int carry = HasFlag(flags, FlagCarry), overflow = HasFlag(flags, FlagOverflow);
    switch (condition) {
    case ConditionAlways:                 return 1;
    case ConditionEqual:                  return zero;
    case ConditionNotEqual:               return !zero;
    case ConditionLessUnsigned:           return !carry;
    case ConditionGreaterOrEqualUnsigned: return carry;
    case ConditionLess:                   return negative != overflow;
    case ConditionGreaterOrEqual:         return negative == overflow;
    case ConditionGreater:                return !zero && negative == overflow;
    case ConditionLessOrEqual:            return zero || negative != overflow;
    case ConditionMinus:                  return negative;
    case ConditionPlus:                   return !negative;
    case ConditionOverflowSet:            return overflow;
    case ConditionOverflowClear:          return !overflow;
    default:                              return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Instruction decoding                                                */

static Instruction Decode(uint16_t word)
{
    Instruction instruction;
    instruction.opcode         = (word >> OpcodeShift) & OpcodeMask;
    instruction.destination    = (word >> DestinationShift) & RegisterMask;
    instruction.sourceA        = (word >> SourceAShift) & RegisterMask;
    instruction.sourceB        = (word >> SourceBShift) & RegisterMask;
    instruction.function       = word & FunctionMask;
    instruction.condition      = (word >> ConditionShift) & ConditionMask;
    instruction.systemFunction = (word >> SystemFunctionShift) & SystemFunctionMask;
    instruction.systemSource   = (word >> SystemSourceShift) & RegisterMask;
    instruction.immediate6     = SignExtend(word, Immediate6Bits);
    instruction.immediate8     = word & Immediate8Mask;
    instruction.offset8        = SignExtend(word, Offset8Bits);
    instruction.offset12       = SignExtend(word, Offset12Bits);
    return instruction;
}

static uint16_t BranchTarget(uint16_t nextAddress, int offsetInWords)
{
    return (uint16_t)(nextAddress + offsetInWords * InstructionSize);
}

/* ------------------------------------------------------------------ */
/* Disassembler (used by the tracer)                                   */

static const char *AluNames[8]  = {"add", "sub", "and", "or", "xor", "shl", "shr", "sar"};
static const char *Alu2Names[8] = {"adc", "sbc", "cmp", "mov", "not", "neg", "sxb", "???"};
static const char *ConditionNames[16] = {"al", "eq", "ne", "ltu", "geu", "lt", "ge", "gt",
                                         "le", "mi", "pl", "vs", "vc", "??", "??", "??"};

static void Disassemble(char *output, size_t size, uint16_t address, uint16_t word)
{
    Instruction in = Decode(word);
    uint16_t nextAddress = (uint16_t)(address + InstructionSize);

    switch (in.opcode) {
    case OpcodeAlu:
        snprintf(output, size, "%s r%d, r%d, r%d", AluNames[in.function],
                 in.destination, in.sourceA, in.sourceB);
        break;
    case OpcodeAlu2:
        switch (in.function) {
        case Alu2AddWithCarry:
        case Alu2SubtractWithCarry:
            snprintf(output, size, "%s r%d, r%d, r%d", Alu2Names[in.function],
                     in.destination, in.sourceA, in.sourceB);
            break;
        case Alu2Compare:
            snprintf(output, size, "cmp r%d, r%d", in.sourceA, in.sourceB);
            break;
        case Alu2Move:
        case Alu2Not:
        case Alu2Negate:
        case Alu2SignExtendByte:
            snprintf(output, size, "%s r%d, r%d", Alu2Names[in.function], in.destination, in.sourceA);
            break;
        default:
            snprintf(output, size, "???");
            break;
        }
        break;
    case OpcodeAddImmediate:
        snprintf(output, size, "addi r%d, r%d, %d", in.destination, in.sourceA, in.immediate6);
        break;
    case OpcodeLoadImmediate:
        snprintf(output, size, "ldi r%d, 0x%02X", in.destination, in.immediate8);
        break;
    case OpcodeLoadHighImmediate:
        snprintf(output, size, "lhi r%d, 0x%02X", in.destination, in.immediate8);
        break;
    case OpcodeLoadWord:
        snprintf(output, size, "lw r%d, [r%d%+d]", in.destination, in.sourceA, in.immediate6);
        break;
    case OpcodeStoreWord:
        snprintf(output, size, "sw r%d, [r%d%+d]", in.destination, in.sourceA, in.immediate6);
        break;
    case OpcodeLoadByte:
        snprintf(output, size, "lb r%d, [r%d%+d]", in.destination, in.sourceA, in.immediate6);
        break;
    case OpcodeStoreByte:
        snprintf(output, size, "sb r%d, [r%d%+d]", in.destination, in.sourceA, in.immediate6);
        break;
    case OpcodeBranch: {
        uint16_t target = BranchTarget(nextAddress, in.offset8);
        if (in.condition == ConditionAlways) snprintf(output, size, "bra 0x%04X", target);
        else snprintf(output, size, "b%s 0x%04X", ConditionNames[in.condition], target);
        break;
    }
    case OpcodeJump:
        snprintf(output, size, "j 0x%04X", BranchTarget(nextAddress, in.offset12));
        break;
    case OpcodeJumpAndLink:
        snprintf(output, size, "jal 0x%04X", BranchTarget(nextAddress, in.offset12));
        break;
    case OpcodeSystem:
        switch (in.systemFunction) {
        case SystemJumpRegister:        snprintf(output, size, "jr r%d", in.systemSource); break;
        case SystemJumpAndLinkRegister: snprintf(output, size, "jalr r%d", in.systemSource); break;
        case SystemReturnFromInterrupt: snprintf(output, size, "reti"); break;
        case SystemEnableInterrupts:    snprintf(output, size, "ei"); break;
        case SystemDisableInterrupts:   snprintf(output, size, "di"); break;
        case SystemHalt:                snprintf(output, size, "halt"); break;
        case SystemTrap:                snprintf(output, size, "trap"); break;
        default:                        snprintf(output, size, "???"); break;
        }
        break;
    case OpcodeCompareImmediate:
        snprintf(output, size, "cmpi r%d, %d", in.sourceA, in.immediate6);
        break;
    default:
        snprintf(output, size, "???");
        break;
    }
}

/* ------------------------------------------------------------------ */
/* CPU core                                                            */

static void TickTimer(void)
{
    if (cpu.timerReload && --cpu.timerCount == 0) {
        cpu.interruptPending = 1;
        cpu.timerCount = cpu.timerReload;
    }
}

static uint8_t PackFlags(void)
{
    return (uint8_t)((cpu.flags & FlagArithmeticMask) | (cpu.interruptsEnabled ? FlagInterruptEnable : 0));
}

static void EnterException(uint16_t returnAddress)
{
    cpu.exceptionProgramCounter = returnAddress;
    cpu.exceptionFlags = PackFlags();
    cpu.interruptsEnabled = 0;
}

static void TraceLine(uint16_t address, uint16_t word, const char *text)
{
    fprintf(stderr, "%04X: %04X  %-20s | r0=%04X r1=%04X r2=%04X r3=%04X "
                    "r4=%04X r5=%04X r6=%04X r7=%04X %c%c%c%c\n",
            address, word, text,
            cpu.registers[0], cpu.registers[1], cpu.registers[2], cpu.registers[3],
            cpu.registers[4], cpu.registers[5], cpu.registers[6], cpu.registers[7],
            HasFlag(cpu.flags, FlagZero) ? 'Z' : '-', HasFlag(cpu.flags, FlagNegative) ? 'N' : '-',
            HasFlag(cpu.flags, FlagCarry) ? 'C' : '-', HasFlag(cpu.flags, FlagOverflow) ? 'V' : '-');
}

/* Returns 0 for an illegal function code. */
static int ExecuteAlu(const Instruction *in)
{
    uint16_t a = cpu.registers[in->sourceA], b = cpu.registers[in->sourceB], result;
    unsigned shiftAmount = b & ShiftAmountMask;

    switch (in->function) {
    case AluAdd:                  result = AluAddWithFlags(a, b, 0); break;
    case AluSubtract:             result = AluSubtractWithFlags(a, b, 1); break;
    case AluAnd:                  result = AluLogicWithFlags(a & b); break;
    case AluOr:                   result = AluLogicWithFlags(a | b); break;
    case AluXor:                  result = AluLogicWithFlags(a ^ b); break;
    case AluShiftLeft:            result = AluShiftWithFlags(ShiftLeft, a, shiftAmount); break;
    case AluShiftRightLogical:    result = AluShiftWithFlags(ShiftRightLogical, a, shiftAmount); break;
    case AluShiftRightArithmetic: result = AluShiftWithFlags(ShiftRightArithmetic, a, shiftAmount); break;
    default:                      return 0;
    }
    cpu.registers[in->destination] = result;
    return 1;
}

/* Returns 0 for an illegal function code. */
static int ExecuteAlu2(const Instruction *in)
{
    uint16_t a = cpu.registers[in->sourceA], b = cpu.registers[in->sourceB];
    unsigned carryIn = HasFlag(cpu.flags, FlagCarry);

    switch (in->function) {
    case Alu2AddWithCarry:      cpu.registers[in->destination] = AluAddWithFlags(a, b, carryIn); break;
    case Alu2SubtractWithCarry: cpu.registers[in->destination] = AluSubtractWithFlags(a, b, carryIn); break;
    case Alu2Compare:           (void)AluSubtractWithFlags(a, b, 1); break;
    case Alu2Move:              cpu.registers[in->destination] = a; break;
    case Alu2Not:               cpu.registers[in->destination] = AluLogicWithFlags((uint16_t)~a); break;
    case Alu2Negate:            cpu.registers[in->destination] = AluSubtractWithFlags(0, a, 1); break;
    case Alu2SignExtendByte:    cpu.registers[in->destination] = AluLogicWithFlags(SignExtendByte(a)); break;
    default:                    return 0;
    }
    return 1;
}

/* Updates *nextAddress for control transfers. Returns 0 for an illegal function code. */
static int ExecuteSystem(const Instruction *in, uint16_t *nextAddress)
{
    switch (in->systemFunction) {
    case SystemJumpRegister:
        *nextAddress = cpu.registers[in->systemSource];
        break;
    case SystemJumpAndLinkRegister: {
        uint16_t target = cpu.registers[in->systemSource];
        cpu.registers[LinkRegister] = *nextAddress;
        *nextAddress = target;
        break;
    }
    case SystemReturnFromInterrupt:
        cpu.flags = cpu.exceptionFlags & FlagArithmeticMask;
        cpu.interruptsEnabled = HasFlag(cpu.exceptionFlags, FlagInterruptEnable);
        *nextAddress = cpu.exceptionProgramCounter;
        break;
    case SystemEnableInterrupts:  cpu.interruptsEnabled = 1; break;
    case SystemDisableInterrupts: cpu.interruptsEnabled = 0; break;
    case SystemHalt:              cpu.halted = 1; break;
    case SystemTrap:
        EnterException(*nextAddress);
        *nextAddress = VectorInterrupt;
        break;
    default:
        return 0;
    }
    return 1;
}

static uint16_t EffectiveAddress(const Instruction *in)
{
    return (uint16_t)(cpu.registers[in->sourceA] + in->immediate6);
}

static enum StepResult Step(int trace)
{
    uint16_t address, word, nextAddress;
    Instruction in;
    int legal = 1;
    char text[48];

    cpu.steps++;
    TickTimer();

    if (cpu.interruptPending && cpu.interruptsEnabled) {
        cpu.interruptPending = 0;
        cpu.halted = 0;
        EnterException(cpu.programCounter);
        cpu.programCounter = VectorInterrupt;
    }

    if (cpu.halted)
        return (cpu.interruptsEnabled && cpu.timerReload) ? StepRunning : StepHalted;

    address = cpu.programCounter & WordAlignMask;
    word = ReadMemoryWord(address);
    nextAddress = (uint16_t)(address + InstructionSize);
    in = Decode(word);

    if (trace) Disassemble(text, sizeof text, address, word);

    switch (in.opcode) {
    case OpcodeAlu:
        legal = ExecuteAlu(&in);
        break;
    case OpcodeAlu2:
        legal = ExecuteAlu2(&in);
        break;
    case OpcodeAddImmediate:
        cpu.registers[in.destination] = AluAddWithFlags(cpu.registers[in.sourceA], (uint16_t)in.immediate6, 0);
        break;
    case OpcodeLoadImmediate:
        cpu.registers[in.destination] = (uint16_t)in.immediate8;
        break;
    case OpcodeLoadHighImmediate:
        cpu.registers[in.destination] =
            (uint16_t)((cpu.registers[in.destination] & ByteMask) | (in.immediate8 << 8));
        break;
    case OpcodeLoadWord:
        cpu.registers[in.destination] = ReadWord(EffectiveAddress(&in));
        break;
    case OpcodeStoreWord:
        WriteWord(EffectiveAddress(&in), cpu.registers[in.destination]);
        break;
    case OpcodeLoadByte:
        cpu.registers[in.destination] = ReadByte(EffectiveAddress(&in));
        break;
    case OpcodeStoreByte:
        WriteByte(EffectiveAddress(&in), (uint8_t)(cpu.registers[in.destination] & ByteMask));
        break;
    case OpcodeBranch:
        if (ConditionHolds(in.condition, cpu.flags))
            nextAddress = BranchTarget(nextAddress, in.offset8);
        break;
    case OpcodeJump:
        nextAddress = BranchTarget(nextAddress, in.offset12);
        break;
    case OpcodeJumpAndLink:
        cpu.registers[LinkRegister] = nextAddress;
        nextAddress = BranchTarget(nextAddress, in.offset12);
        break;
    case OpcodeSystem:
        legal = ExecuteSystem(&in, &nextAddress);
        break;
    case OpcodeCompareImmediate:
        (void)AluSubtractWithFlags(cpu.registers[in.sourceA], (uint16_t)in.immediate6, 1);
        break;
    default:
        legal = 0;
        break;
    }

    if (!legal) {
        fprintf(stderr, "\nillegal instruction 0x%04X at 0x%04X\n", word, address);
        cpu.programCounter = address;
        return StepIllegal;
    }

    cpu.programCounter = nextAddress & WordAlignMask;
    if (trace) TraceLine(address, word, text);
    return StepRunning;
}

/* ------------------------------------------------------------------ */
/* Built-in demo: prints a string through the UART                     */

#define EncodeRegister(op, dest, a, b, function) \
    (((op) << OpcodeShift) | ((dest) << DestinationShift) | ((a) << SourceAShift) | \
     ((b) << SourceBShift) | (function))
#define EncodeImmediate(op, dest, a, immediate) \
    (((op) << OpcodeShift) | ((dest) << DestinationShift) | ((a) << SourceAShift) | \
     ((immediate) & Immediate6Mask))
#define EncodeLong(op, dest, immediate) \
    (((op) << OpcodeShift) | ((dest) << DestinationShift) | ((immediate) & Immediate8Mask))
#define EncodeBranch(condition, offset) \
    ((OpcodeBranch << OpcodeShift) | ((condition) << ConditionShift) | ((offset) & Immediate8Mask))
#define EncodeJump(op, offset) \
    (((op) << OpcodeShift) | ((offset) & Offset12Mask))
#define EncodeSystem(function, source) \
    ((OpcodeSystem << OpcodeShift) | ((function) << SystemFunctionShift) | ((source) << SystemSourceShift))

enum { DemoMessageAddress = 0x0100 };

static void LoadDemo(void)
{
    static const uint16_t code[] = {
        EncodeJump(OpcodeJump, 1),                              /* 0000: j start              */
        EncodeSystem(SystemReturnFromInterrupt, 0),             /* 0002: reti (interrupt stub)*/
        EncodeLong(OpcodeLoadImmediate, 0, 0x00),               /* 0004: ldi r0, 0x00         */
        EncodeLong(OpcodeLoadHighImmediate, 0, 0x01),           /* 0006: lhi r0, 0x01  (r0 = message) */
        EncodeLong(OpcodeLoadImmediate, 1, 0x00),               /* 0008: ldi r1, 0x00         */
        EncodeLong(OpcodeLoadHighImmediate, 1, 0xFF),           /* 000A: lhi r1, 0xFF  (r1 = UART)    */
        EncodeImmediate(OpcodeLoadByte, 2, 0, 0),               /* 000C: loop: lb r2, [r0+0]  */
        EncodeImmediate(OpcodeCompareImmediate, 0, 2, 0),       /* 000E: cmpi r2, 0           */
        EncodeBranch(ConditionEqual, 3),                        /* 0010: beq done             */
        EncodeImmediate(OpcodeStoreByte, 2, 1, 0),              /* 0012: sb r2, [r1+0]        */
        EncodeImmediate(OpcodeAddImmediate, 0, 0, 1),           /* 0014: addi r0, r0, 1       */
        EncodeJump(OpcodeJump, -6),                             /* 0016: j loop               */
        EncodeSystem(SystemHalt, 0),                            /* 0018: done: halt           */
    };
    size_t index;
    for (index = 0; index < sizeof code / sizeof code[0]; index++) {
        memory[index * InstructionSize] = (uint8_t)(code[index] & ByteMask);
        memory[index * InstructionSize + 1] = (uint8_t)(code[index] >> 8);
    }
    strcpy((char *)&memory[DemoMessageAddress], "Hello from ISA-16!\nIf you want to run a different program, pass its filename on the command line.\n");
}

/* ------------------------------------------------------------------ */

enum { DefaultStepLimit = 50000000 };

static void Usage(const char *programName)
{
    fprintf(stderr,
            "usage: %s [-t] [-x] [-n steps] [program.bin]\n"
            "       %s [-t] [-x] [-n steps] --run program.c|.s|.bin\n"
            "  -t        trace each instruction to stderr\n"
            "  -x        dump registers at exit\n"
            "  -n steps  stop after this many steps (default %d, 0 = unlimited)\n"
            "  no file   run the built-in demo\n"
            "  --run     build a .c or .s with the Compiler and Assembler next to this\n"
            "            executable, run it with no step limit (unless -n), and stop once\n"
            "            piped input has run out and the program waits for more\n",
            programName, programName, DefaultStepLimit);
}

static const char *StepResultName(enum StepResult result)
{
    switch (result) {
    case StepHalted:  return "halted";
    case StepIllegal: return "illegal instruction";
    default:          return "step limit";
    }
}

/* Runs a command line, sending its output to stderr so stdout is only the program's. */
static int RunTool(const char *commandLine)
{
    char quoted[4096];
#ifdef _WIN32
    snprintf(quoted, sizeof quoted, "\"%s 1>&2\"", commandLine);  /* cmd.exe strips one outer pair */
#else
    snprintf(quoted, sizeof quoted, "%s 1>&2", commandLine);
#endif
    return system(quoted);
}

/* Builds a .c or .s into a .bin beside the temp folder; returns the .bin path (static) or NULL. */
static const char *BuildProgram(const char *fileName, const char *emulatorPath)
{
    static char binary[1024];
    char toolDirectory[1024], assembly[1024], command[4096], temporary[1000];
    const char *extension = strrchr(fileName, '.'), *slash, *temp;
    size_t length;
    if (!extension || !strcmp(extension, ".bin")) return fileName;
    if (strcmp(extension, ".c") && strcmp(extension, ".s")) {
        fprintf(stderr, "%s: expected a .c, .s or .bin file\n", fileName);
        return NULL;
    }

    slash = strrchr(emulatorPath, '/');
    if (strrchr(emulatorPath, '\\') > slash) slash = strrchr(emulatorPath, '\\');
    length = slash ? (size_t)(slash - emulatorPath + 1) : 0;
    snprintf(toolDirectory, sizeof toolDirectory, "%.*s", (int)length, emulatorPath);

    temp = getenv("TEMP");
    if (!temp) temp = getenv("TMPDIR");
    if (!temp) temp = "/tmp";
    slash = strrchr(fileName, '/');
    if (strrchr(fileName, '\\') > slash) slash = strrchr(fileName, '\\');
    snprintf(temporary, sizeof temporary, "%s/isa16-emulator-%.*s", temp,
             (int)(extension - (slash ? slash + 1 : fileName)), slash ? slash + 1 : fileName);
    snprintf(binary, sizeof binary, "%s.bin", temporary);

    if (!strcmp(extension, ".c")) {
        snprintf(assembly, sizeof assembly, "%s.s", temporary);
        snprintf(command, sizeof command, "\"%sCompiler\" -o \"%s\" \"%s\"", toolDirectory, assembly, fileName);
        if (RunTool(command) != 0) { fprintf(stderr, "compiling %s failed\n", fileName); return NULL; }
    } else {
        snprintf(assembly, sizeof assembly, "%s", fileName);
    }
    snprintf(command, sizeof command, "\"%sAssembler\" -o \"%s\" \"%s\"", toolDirectory, binary, assembly);
    if (RunTool(command) != 0) { fprintf(stderr, "assembling %s failed\n", fileName); return NULL; }
    return binary;
}

static int LoadProgram(const char *fileName)
{
    FILE *file = fopen(fileName, "rb");
    size_t count;
    if (!file) { perror(fileName); return 0; }
    count = fread(memory, 1, IoBase, file);
    if (count == IoBase && fgetc(file) != EOF)
        fprintf(stderr, "warning: %s is larger than %d bytes, truncated\n", fileName, IoBase);
    fclose(file);
    return 1;
}

static void DumpRegisters(void)
{
    int index;
    for (index = 0; index < RegisterCount; index++)
        fprintf(stderr, "r%d=%04X%s", index, cpu.registers[index], index == 3 ? "\n" : "  ");
    fprintf(stderr, "\nflags: Z=%d N=%d C=%d V=%d\n",
            HasFlag(cpu.flags, FlagZero), HasFlag(cpu.flags, FlagNegative),
            HasFlag(cpu.flags, FlagCarry), HasFlag(cpu.flags, FlagOverflow));
}

int main(int argc, char **argv)
{
    const char *fileName = NULL;
    int trace = 0, dump = 0, argumentIndex;
    enum StepResult result;
    unsigned long long stepLimit = DefaultStepLimit;
    int run = 0, stepLimitGiven = 0;

    for (argumentIndex = 1; argumentIndex < argc; argumentIndex++) {
        const char *argument = argv[argumentIndex];
        if (!strcmp(argument, "-t")) trace = 1;
        else if (!strcmp(argument, "-x")) dump = 1;
        else if (!strcmp(argument, "-n") && argumentIndex + 1 < argc)
            stepLimit = strtoull(argv[++argumentIndex], NULL, 0), stepLimitGiven = 1;
        else if (!strcmp(argument, "--run") && argumentIndex + 1 < argc)
            run = 1, fileName = argv[++argumentIndex];
        else if (argument[0] == '-') { Usage(argv[0]); return 2; }
        else fileName = argument;
    }

    if (run) {
        stopAtEndOfInput = 1;
        if (!stepLimitGiven) stepLimit = 0;
        if (!(fileName = BuildProgram(fileName, argv[0]))) return 2;
    }
    if (fileName) {
        if (!LoadProgram(fileName)) return 1;
    } else {
        LoadDemo();
    }

    cpu.programCounter = VectorReset;

    while ((result = Step(trace)) == StepRunning) {
        if (stepLimit && cpu.steps >= stepLimit) { result = StepLimitReached; break; }
        if (inputExhausted) break;
    }
    fflush(stdout);

    fprintf(stderr, "[%s after %llu steps, pc=0x%04X]\n",
            inputExhausted && result == StepRunning ? "end of input" : StepResultName(result),
            cpu.steps, cpu.programCounter);

    if (dump) DumpRegisters();

    return result == StepIllegal ? 1 : 0;
}