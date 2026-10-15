/*
 * Kestrel16 assembler -- single file, plain C99.
 *
 * Naming: ProperCase for types, functions, constants and macros;
 *         camelCase for variables, fields and parameters.
 *
 * Build:  cc -O2 -Wall -Wextra -o Assembler Assembler.c
 * Run:    ./Assembler prog.s             writes prog.bin
 *         ./Assembler -o out.bin prog.s  choose the output name
 *         ./Assembler -l prog.s          also print a listing + symbol table
 *
 * The output is a raw little-endian image starting at address 0x0000,
 * which is exactly what the emulator loads.
 *
 * ---------------------------------------------------------------------
 * Source syntax
 *
 *   label:  mnemonic  operand, operand   ; comment
 *
 *   - Mnemonics, directives and register names are case-insensitive;
 *     labels and symbols are case-sensitive.
 *   - Registers: r0-r7, plus sp (= r6) and lr (= r7).
 *   - Numbers: 42, 0x2A, 0b101010, 'A', '\n'  (escapes: \n \t \r \0 \\ \' \" \xHH)
 *   - Expressions: + - * / % & | ^ ~ << >> and parentheses, symbols,
 *     $ (address of the current statement), lo(x) and hi(x) (bytes of x).
 *   - Memory operands: [r1], [r1+4], [r1-2], [sp+CONSTANT]
 *   - Branch and jump operands are absolute targets (usually labels);
 *     the assembler works out the relative offset and checks its range.
 *
 * Directives
 *   name = expr          define a constant (same as .equ name, expr)
 *   .equ name, expr
 *   .org address         continue assembling at address (forward only)
 *   .word expr, ...      16-bit values (aligned to an even address)
 *   .byte expr|"str",... 8-bit values
 *   .ascii "str", ...    string bytes
 *   .asciz "str", ...    string bytes, each followed by a zero
 *   .space count         count zero bytes
 *   .align n             pad with zeros to a multiple of n
 *
 * Instructions are always placed at even addresses; the assembler pads
 * automatically after odd-length data, and a label just before an
 * instruction lands on the padded address.
 *
 * Real instructions (see the emulator for semantics)
 *   add sub and or xor shl shr sar adc sbc   rd, ra, rb
 *   mov not neg sxb                          rd, ra
 *   cmp                                      ra, rb
 *   addi                                     rd, ra, imm6     (-32..31)
 *   cmpi                                     ra, imm6
 *   ldi lhi                                  rd, imm8         (-128..255)
 *   lw sw lb sb                              rd, [ra+imm6]
 *   bra beq bne bltu bgeu blt bge bgt ble bmi bpl bvs bvc   target
 *     (aliases: bz=beq bnz=bne bcc=blo=bltu bcs=bhs=bgeu)
 *   j jal                                    target
 *   jr jalr                                  ra
 *   reti ei di halt trap
 *
 * Pseudo-instructions
 *   li rd, expr     load any 16-bit value       (ldi + lhi, 2 words)
 *   la rd, expr     same as li, reads better for addresses
 *   push rx         addi sp, sp, -2 ; sw rx, [sp]  (2 words, sets flags)
 *   pop rx          lw rx, [sp] ; addi sp, sp, 2   (2 words, sets flags)
 *   call target     jal target
 *   ret             jr lr
 *   subi rd, ra, n  addi rd, ra, -n
 *   nop             mov r0, r0                  (touches nothing)
 * ---------------------------------------------------------------------
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Instruction set constants (must match the emulator)                 */

enum Opcode {
    OpcodeAlu               = 0x0,
    OpcodeAlu2              = 0x1,
    OpcodeAddImmediate      = 0x2,
    OpcodeLoadImmediate     = 0x3,
    OpcodeLoadHighImmediate = 0x4,
    OpcodeLoadWord          = 0x5,
    OpcodeStoreWord         = 0x6,
    OpcodeLoadByte          = 0x7,
    OpcodeStoreByte         = 0x8,
    OpcodeBranch            = 0x9,
    OpcodeJump              = 0xA,
    OpcodeJumpAndLink       = 0xB,
    OpcodeSystem            = 0xC,
    OpcodeCompareImmediate  = 0xD
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

enum { StackPointer = 6, LinkRegister = 7, RegisterCount = 8 };

enum {
    OpcodeShift = 12, DestinationShift = 9, SourceAShift = 6, SourceBShift = 3,
    ConditionShift = 8, SystemFunctionShift = 8, SystemSourceShift = 5,
    Immediate6Bits = 6, Immediate8Bits = 8, Offset8Bits = 8, Offset12Bits = 12,
    InstructionSize = 2
};

static int Encode3Registers(int opcode, int destination, int sourceA, int sourceB, int function)
{
    return (opcode << OpcodeShift) | (destination << DestinationShift) |
           (sourceA << SourceAShift) | (sourceB << SourceBShift) | function;
}

static int EncodeImmediate(int opcode, int destination, int sourceA, int immediate6)
{
    return (opcode << OpcodeShift) | (destination << DestinationShift) |
           (sourceA << SourceAShift) | (immediate6 & ((1 << Immediate6Bits) - 1));
}

static int EncodeLong(int opcode, int destination, int immediate8)
{
    return (opcode << OpcodeShift) | (destination << DestinationShift) |
           (immediate8 & ((1 << Immediate8Bits) - 1));
}

static int EncodeBranch(int condition, int offset8)
{
    return (OpcodeBranch << OpcodeShift) | (condition << ConditionShift) |
           (offset8 & ((1 << Offset8Bits) - 1));
}

static int EncodeJump(int opcode, int offset12)
{
    return (opcode << OpcodeShift) | (offset12 & ((1 << Offset12Bits) - 1));
}

static int EncodeSystem(int function, int source)
{
    return (OpcodeSystem << OpcodeShift) | (function << SystemFunctionShift) |
           (source << SystemSourceShift);
}

/* ------------------------------------------------------------------ */
/* Instruction tables                                                  */

typedef enum {
    FormThreeRegisters,     /* op rd, ra, rb   */
    FormTwoRegisters,       /* op rd, ra       */
    FormCompareRegisters,   /* op ra, rb       */
    FormRegisterImmediate,  /* op rd, ra, imm6 */
    FormCompareImmediate,   /* op ra, imm6     */
    FormLoadImmediate,      /* op rd, imm8     */
    FormMemory,             /* op rd, [ra+imm6] */
    FormJump,               /* op target       */
    FormSystemRegister,     /* op ra           */
    FormSystemBare          /* op              */
} InstructionForm;

typedef struct {
    const char *mnemonic;
    InstructionForm form;
    int opcode;
    int function;
} InstructionInfo;

static const InstructionInfo Instructions[] = {
    {"add",  FormThreeRegisters,    OpcodeAlu,  AluAdd},
    {"sub",  FormThreeRegisters,    OpcodeAlu,  AluSubtract},
    {"and",  FormThreeRegisters,    OpcodeAlu,  AluAnd},
    {"or",   FormThreeRegisters,    OpcodeAlu,  AluOr},
    {"xor",  FormThreeRegisters,    OpcodeAlu,  AluXor},
    {"shl",  FormThreeRegisters,    OpcodeAlu,  AluShiftLeft},
    {"shr",  FormThreeRegisters,    OpcodeAlu,  AluShiftRightLogical},
    {"sar",  FormThreeRegisters,    OpcodeAlu,  AluShiftRightArithmetic},
    {"adc",  FormThreeRegisters,    OpcodeAlu2, Alu2AddWithCarry},
    {"sbc",  FormThreeRegisters,    OpcodeAlu2, Alu2SubtractWithCarry},
    {"cmp",  FormCompareRegisters,  OpcodeAlu2, Alu2Compare},
    {"mov",  FormTwoRegisters,      OpcodeAlu2, Alu2Move},
    {"not",  FormTwoRegisters,      OpcodeAlu2, Alu2Not},
    {"neg",  FormTwoRegisters,      OpcodeAlu2, Alu2Negate},
    {"sxb",  FormTwoRegisters,      OpcodeAlu2, Alu2SignExtendByte},
    {"addi", FormRegisterImmediate, OpcodeAddImmediate, 0},
    {"cmpi", FormCompareImmediate,  OpcodeCompareImmediate, 0},
    {"ldi",  FormLoadImmediate,     OpcodeLoadImmediate, 0},
    {"lhi",  FormLoadImmediate,     OpcodeLoadHighImmediate, 0},
    {"lw",   FormMemory,            OpcodeLoadWord, 0},
    {"sw",   FormMemory,            OpcodeStoreWord, 0},
    {"lb",   FormMemory,            OpcodeLoadByte, 0},
    {"sb",   FormMemory,            OpcodeStoreByte, 0},
    {"j",    FormJump,              OpcodeJump, 0},
    {"jal",  FormJump,              OpcodeJumpAndLink, 0},
    {"jr",   FormSystemRegister,    OpcodeSystem, SystemJumpRegister},
    {"jalr", FormSystemRegister,    OpcodeSystem, SystemJumpAndLinkRegister},
    {"reti", FormSystemBare,        OpcodeSystem, SystemReturnFromInterrupt},
    {"ei",   FormSystemBare,        OpcodeSystem, SystemEnableInterrupts},
    {"di",   FormSystemBare,        OpcodeSystem, SystemDisableInterrupts},
    {"halt", FormSystemBare,        OpcodeSystem, SystemHalt},
    {"trap", FormSystemBare,        OpcodeSystem, SystemTrap},
};

typedef struct {
    const char *suffix;     /* mnemonic is "b" + suffix */
    int condition;
} BranchInfo;

static const BranchInfo Branches[] = {
    {"ra",  ConditionAlways},
    {"eq",  ConditionEqual},       {"z",   ConditionEqual},
    {"ne",  ConditionNotEqual},    {"nz",  ConditionNotEqual},
    {"ltu", ConditionLessUnsigned}, {"lo", ConditionLessUnsigned}, {"cc", ConditionLessUnsigned},
    {"geu", ConditionGreaterOrEqualUnsigned}, {"hs", ConditionGreaterOrEqualUnsigned},
    {"cs",  ConditionGreaterOrEqualUnsigned},
    {"lt",  ConditionLess},
    {"ge",  ConditionGreaterOrEqual},
    {"gt",  ConditionGreater},
    {"le",  ConditionLessOrEqual},
    {"mi",  ConditionMinus},
    {"pl",  ConditionPlus},
    {"vs",  ConditionOverflowSet},
    {"vc",  ConditionOverflowClear},
};

#define CountOf(array) ((int)(sizeof(array) / sizeof((array)[0])))

/* ------------------------------------------------------------------ */
/* Assembler state                                                     */

enum {
    ImageSize = 0xFF00,         /* everything above is I/O space */
    MaxSymbols = 4096,
    MaxNameLength = 64,
    MaxLineLength = 1024,
    MaxOperands = 32,
    MaxPendingLabels = 64,
    MaxStringLength = 1024,
    ListingBytesShown = 8
};

typedef struct {
    char name[MaxNameLength];
    int value;
    int definedOnLine;
} Symbol;

typedef struct {
    char name[MaxNameLength];
    int line;
} PendingLabel;

static Symbol symbols[MaxSymbols];
static int symbolCount;

static uint8_t image[ImageSize];
static int highestAddress;          /* one past the last byte written */
static int locationCounter;         /* where the next byte goes */
static int statementAddress;        /* start of the current statement, for $ and branches */
static int pass;                    /* 1 = measure and collect labels, 2 = emit */
static int errorCount;
static const char *sourceName;
static int lineNumber;
static int expressionUnresolved;
static int outOfSpaceReported;

static PendingLabel pendingLabels[MaxPendingLabels];
static int pendingLabelCount;

static FILE *listingFile;
static uint8_t lineBytes[ListingBytesShown];
static int lineByteCount;
static int lineFirstAddress;

/* ------------------------------------------------------------------ */
/* Error reporting                                                     */

static void ReportError(const char *format, va_list arguments)
{
    fprintf(stderr, "%s:%d: error: ", sourceName, lineNumber);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    errorCount++;
}

/* Most problems are reported only in pass 2, so each appears exactly once. */
static void Error(const char *format, ...)
{
    va_list arguments;
    if (pass != 2) return;
    va_start(arguments, format);
    ReportError(format, arguments);
    va_end(arguments);
}

/* Problems that would make the two passes disagree are reported immediately. */
static void ErrorNow(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    ReportError(format, arguments);
    va_end(arguments);
}

/* ------------------------------------------------------------------ */
/* Text helpers                                                        */

static const char *SkipSpaces(const char *text)
{
    while (*text == ' ' || *text == '\t') text++;
    return text;
}

static void TrimRight(char *text)
{
    size_t length = strlen(text);
    while (length && isspace((unsigned char)text[length - 1])) text[--length] = 0;
}

static int IsIdentifierStart(int character)
{
    return isalpha(character) || character == '_' || character == '.';
}

static int IsIdentifierCharacter(int character)
{
    return isalnum(character) || character == '_' || character == '.';
}

static int EqualsIgnoringCase(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

/* Reads an identifier at *text into name and advances past it. Returns 0 if there is none. */
static int ReadIdentifier(const char **text, char *name)
{
    const char *cursor = *text;
    int length = 0;
    if (!IsIdentifierStart((unsigned char)*cursor)) return 0;
    while (IsIdentifierCharacter((unsigned char)*cursor)) {
        if (length < MaxNameLength - 1) name[length++] = *cursor;
        cursor++;
    }
    name[length] = 0;
    *text = cursor;
    return 1;
}

/* Cuts the line at the first ';' that is not inside quotes. */
static void StripComment(char *line)
{
    char quote = 0;
    for (; *line; line++) {
        if (quote) {
            if (*line == '\\' && line[1]) line++;
            else if (*line == quote) quote = 0;
        } else if (*line == '"' || *line == '\'') {
            quote = *line;
        } else if (*line == ';') {
            *line = 0;
            return;
        }
    }
}

/* Splits "a, [b+1], 'c'" on top-level commas, in place. Returns the operand count. */
static int SplitOperands(char *text, char *operands[])
{
    int count = 0, depth = 0;
    char quote = 0, *start = text, *cursor;

    if (*SkipSpaces(text) == 0) return 0;

    for (cursor = text;; cursor++) {
        char character = *cursor;
        if (quote && character) {
            if (character == '\\' && cursor[1]) cursor++;
            else if (character == quote) quote = 0;
            continue;
        }
        if (character == '"' || character == '\'') {
            quote = character;
        } else if (character == '(' || character == '[') {
            depth++;
        } else if ((character == ')' || character == ']') && depth > 0) {
            depth--;
        } else if (character == 0 || (character == ',' && depth == 0)) {
            *cursor = 0;
            start = (char *)SkipSpaces(start);
            TrimRight(start);
            if (*start == 0) Error("empty operand");
            if (count == MaxOperands) {
                Error("too many operands");
                return count;
            }
            operands[count++] = start;
            if (character == 0) return count;
            start = cursor + 1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Symbols and registers                                               */

static int ParseRegister(const char *text)
{
    if ((text[0] == 'r' || text[0] == 'R') && text[1] >= '0' && text[1] <= '7' && text[2] == 0)
        return text[1] - '0';
    if (EqualsIgnoringCase(text, "sp")) return StackPointer;
    if (EqualsIgnoringCase(text, "lr")) return LinkRegister;
    return -1;
}

static int ExpectRegister(const char *text)
{
    int registerNumber = ParseRegister(text);
    if (registerNumber < 0) {
        Error("expected a register (r0-r7, sp, lr), got '%s'", text);
        return 0;
    }
    return registerNumber;
}

static Symbol *FindSymbol(const char *name)
{
    int index;
    for (index = 0; index < symbolCount; index++)
        if (!strcmp(symbols[index].name, name)) return &symbols[index];
    return NULL;
}

static void DefineSymbol(const char *name, int value, int isLabel)
{
    Symbol *symbol = FindSymbol(name);

    if (pass == 1) {
        if (ParseRegister(name) >= 0) {
            ErrorNow("'%s' is a register name and can't be used as a symbol", name);
            return;
        }
        if (symbol) {
            ErrorNow("'%s' is already defined on line %d", name, symbol->definedOnLine);
            return;
        }
        if (symbolCount == MaxSymbols) {
            ErrorNow("too many symbols");
            return;
        }
        symbol = &symbols[symbolCount++];
        strcpy(symbol->name, name);
        symbol->value = value;
        symbol->definedOnLine = lineNumber;
        return;
    }

    /* Pass 2: only the definition pass 1 accepted gets updated. */
    if (!symbol || symbol->definedOnLine != lineNumber) return;
    if (isLabel && symbol->value != value)
        Error("internal error: label '%s' moved between passes (0x%04X -> 0x%04X)",
              name, symbol->value, value);
    symbol->value = value;
}

/* Labels wait until the next statement starts, so they pick up any alignment padding. */
static void AddPendingLabel(const char *name)
{
    if (pendingLabelCount == MaxPendingLabels) {
        Error("too many labels in a row");
        return;
    }
    strcpy(pendingLabels[pendingLabelCount].name, name);
    pendingLabels[pendingLabelCount].line = lineNumber;
    pendingLabelCount++;
}

static void FlushPendingLabels(void)
{
    int index, savedLine = lineNumber;
    for (index = 0; index < pendingLabelCount; index++) {
        lineNumber = pendingLabels[index].line;
        DefineSymbol(pendingLabels[index].name, locationCounter, 1);
    }
    lineNumber = savedLine;
    pendingLabelCount = 0;
}

/* ------------------------------------------------------------------ */
/* Expressions                                                         */

static int ParseExpression(const char **text);

static int HexDigitValue(int character)
{
    if (isdigit(character)) return character - '0';
    character = tolower(character);
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    return -1;
}

/* Reads one possibly-escaped character inside a '...' or "..." literal. */
static int ParseLiteralCharacter(const char **text)
{
    const char *cursor = *text;
    int value;

    if (*cursor != '\\') {
        *text = cursor + 1;
        return (unsigned char)*cursor;
    }
    cursor++;
    switch (*cursor) {
    case 'n':  value = '\n'; break;
    case 't':  value = '\t'; break;
    case 'r':  value = '\r'; break;
    case '0':  value = 0;    break;
    case '\\': value = '\\'; break;
    case '\'': value = '\''; break;
    case '"':  value = '"';  break;
    case 'x': {
        int digits = 0;
        value = 0;
        cursor++;
        while (digits < 2 && HexDigitValue((unsigned char)*cursor) >= 0) {
            value = value * 16 + HexDigitValue((unsigned char)*cursor);
            cursor++;
            digits++;
        }
        if (!digits) Error("'\\x' needs one or two hex digits");
        *text = cursor;
        return value;
    }
    case 0:
        Error("escape '\\' at end of line");
        *text = cursor;
        return 0;
    default:
        Error("unknown escape '\\%c'", *cursor);
        value = (unsigned char)*cursor;
        break;
    }
    *text = cursor + 1;
    return value;
}

static int ParseNumber(const char **text)
{
    const char *cursor = *text;
    long value = 0;
    int base = 10, digits = 0;

    if (cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X')) { base = 16; cursor += 2; }
    else if (cursor[0] == '0' && (cursor[1] == 'b' || cursor[1] == 'B')) { base = 2; cursor += 2; }

    for (;; cursor++) {
        int digit;
        if (*cursor == '_') continue;               /* allow 0b1010_0101 */
        digit = HexDigitValue((unsigned char)*cursor);
        if (digit < 0 || digit >= base) break;
        if (value < 0x1000000) value = value * base + digit;
        digits++;
    }
    if (!digits) Error("malformed number");
    if (IsIdentifierCharacter((unsigned char)*cursor))
        Error("malformed number (unexpected '%c')", *cursor);
    *text = cursor;
    return (int)value;
}

static int ExpectClosingParenthesis(const char **text)
{
    const char *cursor = SkipSpaces(*text);
    if (*cursor != ')') {
        Error("missing ')'");
        return 0;
    }
    *text = cursor + 1;
    return 1;
}

static int ParsePrimary(const char **text)
{
    const char *cursor = SkipSpaces(*text);
    char name[MaxNameLength];
    int value = 0;

    if (*cursor == '(') {
        cursor++;
        value = ParseExpression(&cursor);
        ExpectClosingParenthesis(&cursor);
    } else if (isdigit((unsigned char)*cursor)) {
        value = ParseNumber(&cursor);
    } else if (*cursor == '\'') {
        cursor++;
        if (*cursor == 0 || *cursor == '\'') Error("empty character literal");
        else value = ParseLiteralCharacter(&cursor);
        if (*cursor == '\'') cursor++;
        else Error("missing closing ' in character literal");
    } else if (*cursor == '$') {
        cursor++;
        value = statementAddress;
    } else if (ReadIdentifier(&cursor, name)) {
        const char *after = SkipSpaces(cursor);
        int isLow = EqualsIgnoringCase(name, "lo"), isHigh = EqualsIgnoringCase(name, "hi");
        if (*after == '(' && (isLow || isHigh)) {
            cursor = after + 1;
            value = ParseExpression(&cursor);
            ExpectClosingParenthesis(&cursor);
            value = isHigh ? (value >> 8) & 0xFF : value & 0xFF;
        } else if (ParseRegister(name) >= 0) {
            Error("register '%s' can't be used in an expression", name);
        } else {
            Symbol *symbol = FindSymbol(name);
            if (symbol) {
                value = symbol->value;
            } else {
                expressionUnresolved = 1;   /* may be a forward reference in pass 1 */
                Error("undefined symbol '%s'", name);
            }
        }
    } else {
        if (*cursor) Error("unexpected '%c' in expression", *cursor);
        else Error("missing value in expression");
        cursor += strlen(cursor);
    }
    *text = cursor;
    return value;
}

static int ParseUnary(const char **text)
{
    const char *cursor = SkipSpaces(*text);
    if (*cursor == '-') { *text = cursor + 1; return -ParseUnary(text); }
    if (*cursor == '+') { *text = cursor + 1; return ParseUnary(text); }
    if (*cursor == '~') { *text = cursor + 1; return ~ParseUnary(text); }
    *text = cursor;
    return ParsePrimary(text);
}

static int ParseMultiplicative(const char **text)
{
    int value = ParseUnary(text);
    for (;;) {
        const char *cursor = SkipSpaces(*text);
        char operation = *cursor;
        int right;
        if (operation != '*' && operation != '/' && operation != '%') return value;
        *text = cursor + 1;
        right = ParseUnary(text);
        if (operation == '*') value *= right;
        else if (right == 0) { if (!expressionUnresolved) Error("division by zero"); }
        else if (operation == '/') value /= right;
        else value %= right;
    }
}

static int ParseAdditive(const char **text)
{
    int value = ParseMultiplicative(text);
    for (;;) {
        const char *cursor = SkipSpaces(*text);
        if (*cursor == '+') { *text = cursor + 1; value += ParseMultiplicative(text); }
        else if (*cursor == '-') { *text = cursor + 1; value -= ParseMultiplicative(text); }
        else return value;
    }
}

static int ParseShift(const char **text)
{
    int value = ParseAdditive(text);
    for (;;) {
        const char *cursor = SkipSpaces(*text);
        if (cursor[0] == '<' && cursor[1] == '<') { *text = cursor + 2; value <<= ParseAdditive(text) & 31; }
        else if (cursor[0] == '>' && cursor[1] == '>') { *text = cursor + 2; value >>= ParseAdditive(text) & 31; }
        else return value;
    }
}

static int ParseBitwiseAnd(const char **text)
{
    int value = ParseShift(text);
    for (;;) {
        const char *cursor = SkipSpaces(*text);
        if (*cursor != '&') return value;
        *text = cursor + 1;
        value &= ParseShift(text);
    }
}

static int ParseBitwiseXor(const char **text)
{
    int value = ParseBitwiseAnd(text);
    for (;;) {
        const char *cursor = SkipSpaces(*text);
        if (*cursor != '^') return value;
        *text = cursor + 1;
        value ^= ParseBitwiseAnd(text);
    }
}

static int ParseExpression(const char **text)
{
    int value = ParseBitwiseXor(text);
    for (;;) {
        const char *cursor = SkipSpaces(*text);
        if (*cursor != '|') return value;
        *text = cursor + 1;
        value |= ParseBitwiseXor(text);
    }
}

/* Evaluates a whole operand. *resolved (if given) is 0 when it used an unknown symbol. */
static int Evaluate(const char *text, int *resolved)
{
    const char *cursor = text;
    int value;
    expressionUnresolved = 0;
    value = ParseExpression(&cursor);
    cursor = SkipSpaces(cursor);
    if (*cursor) Error("unexpected '%s' after expression", cursor);
    if (resolved) *resolved = !expressionUnresolved;
    return value;
}

/* ------------------------------------------------------------------ */
/* Range checks                                                        */

static int FitSigned(int value, int bits, const char *what)
{
    int minimum = -(1 << (bits - 1)), maximum = (1 << (bits - 1)) - 1;
    if (value < minimum || value > maximum)
        Error("%s %d doesn't fit (allowed %d..%d)", what, value, minimum, maximum);
    return value & ((1 << bits) - 1);
}

/* Accepts either a signed or an unsigned value of the given width. */
static int FitSignedOrUnsigned(int value, int bits, const char *what)
{
    int minimum = -(1 << (bits - 1)), maximum = (1 << bits) - 1;
    if (value < minimum || value > maximum)
        Error("%s %d doesn't fit in %d bits (allowed %d..%d)", what, value, bits, minimum, maximum);
    return value & ((1 << bits) - 1);
}

/* Turns an absolute target into a word offset from the next instruction. */
static int BranchOffset(const char *targetText, int bits)
{
    int resolved, target = Evaluate(targetText, &resolved);
    int minimum = -(1 << (bits - 1)), maximum = (1 << (bits - 1)) - 1, offset;

    if (!resolved) return 0;
    if (target & 1) {
        Error("target 0x%04X is not an even address", target);
        return 0;
    }
    offset = (target - (statementAddress + InstructionSize)) / InstructionSize;
    if (offset < minimum || offset > maximum) {
        Error("target 0x%04X is out of reach (%+d words; this instruction reaches %d..%+d)%s",
              target, offset, minimum, maximum,
              bits == Offset8Bits ? " -- branch around a 'j' instead" : "");
        return 0;
    }
    return offset & ((1 << bits) - 1);
}

/* ------------------------------------------------------------------ */
/* Output                                                              */

static void EmitByte(int value)
{
    if (locationCounter >= ImageSize) {
        if (pass == 2 && !outOfSpaceReported) {
            Error("program runs past 0x%04X, where I/O space begins", ImageSize);
            outOfSpaceReported = 1;
        }
        locationCounter++;
        return;
    }
    if (pass == 2) {
        image[locationCounter] = (uint8_t)value;
        if (locationCounter + 1 > highestAddress) highestAddress = locationCounter + 1;
        if (lineByteCount == 0) lineFirstAddress = locationCounter;
        if (lineByteCount < ListingBytesShown) lineBytes[lineByteCount] = (uint8_t)value;
        lineByteCount++;
    }
    locationCounter++;
}

static void EmitWord(int value)
{
    EmitByte(value & 0xFF);
    EmitByte((value >> 8) & 0xFF);
}

static void AlignTo(int alignment)
{
    while (locationCounter % alignment) EmitByte(0);
}

/* Every statement that produces output starts here. */
static void BeginStatement(int alignment)
{
    AlignTo(alignment);
    lineByteCount = 0;          /* leave padding out of the listing */
    FlushPendingLabels();
    statementAddress = locationCounter;
}

static int ExpectOperandCount(const char *mnemonic, int count, int expected)
{
    if (count == expected) return 1;
    Error("'%s' takes %d operand%s, got %d", mnemonic, expected, expected == 1 ? "" : "s", count);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Instructions                                                        */

static const InstructionInfo *FindInstruction(const char *mnemonic)
{
    int index;
    for (index = 0; index < CountOf(Instructions); index++)
        if (!strcmp(Instructions[index].mnemonic, mnemonic)) return &Instructions[index];
    return NULL;
}

static int FindBranchCondition(const char *mnemonic)
{
    int index;
    if (mnemonic[0] != 'b') return -1;
    for (index = 0; index < CountOf(Branches); index++)
        if (!strcmp(Branches[index].suffix, mnemonic + 1)) return Branches[index].condition;
    return -1;
}

/* Parses "[ra]", "[ra+expr]" or "[ra-expr]". */
static void ParseMemoryOperand(const char *text, int *base, int *offset)
{
    char inner[MaxLineLength], name[MaxNameLength];
    const char *cursor;
    size_t length = strlen(text);

    *base = 0;
    *offset = 0;
    if (length < 2 || text[0] != '[' || text[length - 1] != ']') {
        Error("expected a memory operand like [r1+4], got '%s'", text);
        return;
    }
    memcpy(inner, text + 1, length - 2);
    inner[length - 2] = 0;

    cursor = SkipSpaces(inner);
    if (!ReadIdentifier(&cursor, name) || ParseRegister(name) < 0) {
        Error("memory operand '%s' must start with a register", text);
        return;
    }
    *base = ParseRegister(name);
    cursor = SkipSpaces(cursor);
    if (*cursor == 0) return;
    if (*cursor != '+' && *cursor != '-') {
        Error("expected '+' or '-' after the register in '%s'", text);
        return;
    }
    *offset = Evaluate(cursor, NULL);
}

static int AssembleTableInstruction(const InstructionInfo *info, char *operands[], int count)
{
    const char *name = info->mnemonic;
    int destination, sourceA, sourceB, value;

    switch (info->form) {
    case FormThreeRegisters:
        if (!ExpectOperandCount(name, count, 3)) return 0;
        destination = ExpectRegister(operands[0]);
        sourceA = ExpectRegister(operands[1]);
        sourceB = ExpectRegister(operands[2]);
        return Encode3Registers(info->opcode, destination, sourceA, sourceB, info->function);

    case FormTwoRegisters:
        if (!ExpectOperandCount(name, count, 2)) return 0;
        destination = ExpectRegister(operands[0]);
        sourceA = ExpectRegister(operands[1]);
        return Encode3Registers(info->opcode, destination, sourceA, 0, info->function);

    case FormCompareRegisters:
        if (!ExpectOperandCount(name, count, 2)) return 0;
        sourceA = ExpectRegister(operands[0]);
        sourceB = ExpectRegister(operands[1]);
        return Encode3Registers(info->opcode, 0, sourceA, sourceB, info->function);

    case FormRegisterImmediate:
        if (!ExpectOperandCount(name, count, 3)) return 0;
        destination = ExpectRegister(operands[0]);
        sourceA = ExpectRegister(operands[1]);
        value = FitSigned(Evaluate(operands[2], NULL), Immediate6Bits, "immediate");
        return EncodeImmediate(info->opcode, destination, sourceA, value);

    case FormCompareImmediate:
        if (!ExpectOperandCount(name, count, 2)) return 0;
        sourceA = ExpectRegister(operands[0]);
        value = FitSigned(Evaluate(operands[1], NULL), Immediate6Bits, "immediate");
        return EncodeImmediate(info->opcode, 0, sourceA, value);

    case FormLoadImmediate:
        if (!ExpectOperandCount(name, count, 2)) return 0;
        destination = ExpectRegister(operands[0]);
        value = FitSignedOrUnsigned(Evaluate(operands[1], NULL), Immediate8Bits, "immediate");
        return EncodeLong(info->opcode, destination, value);

    case FormMemory: {
        int offset;
        if (!ExpectOperandCount(name, count, 2)) return 0;
        destination = ExpectRegister(operands[0]);
        ParseMemoryOperand(operands[1], &sourceA, &offset);
        value = FitSigned(offset, Immediate6Bits, "offset");
        return EncodeImmediate(info->opcode, destination, sourceA, value);
    }

    case FormJump:
        if (!ExpectOperandCount(name, count, 1)) return 0;
        return EncodeJump(info->opcode, BranchOffset(operands[0], Offset12Bits));

    case FormSystemRegister:
        if (!ExpectOperandCount(name, count, 1)) return 0;
        return EncodeSystem(info->function, ExpectRegister(operands[0]));

    case FormSystemBare:
        if (!ExpectOperandCount(name, count, 0)) return 0;
        return EncodeSystem(info->function, 0);
    }
    return 0;
}

/* Returns the number of words written to words[], or 0 if not a pseudo-instruction. */
static int AssemblePseudoInstruction(const char *mnemonic, char *operands[], int count, int words[2])
{
    if (!strcmp(mnemonic, "nop")) {
        ExpectOperandCount(mnemonic, count, 0);
        words[0] = Encode3Registers(OpcodeAlu2, 0, 0, 0, Alu2Move);
        return 1;
    }
    if (!strcmp(mnemonic, "ret")) {
        ExpectOperandCount(mnemonic, count, 0);
        words[0] = EncodeSystem(SystemJumpRegister, LinkRegister);
        return 1;
    }
    if (!strcmp(mnemonic, "call")) {
        words[0] = ExpectOperandCount(mnemonic, count, 1)
                 ? EncodeJump(OpcodeJumpAndLink, BranchOffset(operands[0], Offset12Bits)) : 0;
        return 1;
    }
    if (!strcmp(mnemonic, "subi")) {
        words[0] = 0;
        if (ExpectOperandCount(mnemonic, count, 3)) {
            int destination = ExpectRegister(operands[0]);
            int source = ExpectRegister(operands[1]);
            int value = FitSigned(-Evaluate(operands[2], NULL), Immediate6Bits, "negated immediate");
            words[0] = EncodeImmediate(OpcodeAddImmediate, destination, source, value);
        }
        return 1;
    }
    if (!strcmp(mnemonic, "li") || !strcmp(mnemonic, "la")) {
        words[0] = words[1] = 0;
        if (ExpectOperandCount(mnemonic, count, 2)) {
            int destination = ExpectRegister(operands[0]);
            int value = FitSignedOrUnsigned(Evaluate(operands[1], NULL), 16, "value");
            words[0] = EncodeLong(OpcodeLoadImmediate, destination, value & 0xFF);
            words[1] = EncodeLong(OpcodeLoadHighImmediate, destination, (value >> 8) & 0xFF);
        }
        return 2;
    }
    if (!strcmp(mnemonic, "push")) {
        int source = ExpectOperandCount(mnemonic, count, 1) ? ExpectRegister(operands[0]) : 0;
        words[0] = EncodeImmediate(OpcodeAddImmediate, StackPointer, StackPointer, -InstructionSize);
        words[1] = EncodeImmediate(OpcodeStoreWord, source, StackPointer, 0);
        return 2;
    }
    if (!strcmp(mnemonic, "pop")) {
        int destination = ExpectOperandCount(mnemonic, count, 1) ? ExpectRegister(operands[0]) : 0;
        if (destination == StackPointer) Error("'pop sp' would lose the stack; load it with lw instead");
        words[0] = EncodeImmediate(OpcodeLoadWord, destination, StackPointer, 0);
        words[1] = EncodeImmediate(OpcodeAddImmediate, StackPointer, StackPointer, InstructionSize);
        return 2;
    }
    return 0;
}

/* Returns 0 if the mnemonic isn't known. */
static int AssembleInstruction(const char *mnemonic, char *operands[], int count)
{
    const InstructionInfo *info = FindInstruction(mnemonic);
    int condition = FindBranchCondition(mnemonic);
    int words[2], wordCount, index;

    BeginStatement(InstructionSize);

    if (info) {
        EmitWord(AssembleTableInstruction(info, operands, count));
        return 1;
    }
    if (condition >= 0) {
        int offset = ExpectOperandCount(mnemonic, count, 1) ? BranchOffset(operands[0], Offset8Bits) : 0;
        EmitWord(EncodeBranch(condition, offset));
        return 1;
    }
    wordCount = AssemblePseudoInstruction(mnemonic, operands, count, words);
    for (index = 0; index < wordCount; index++) EmitWord(words[index]);
    return wordCount > 0;
}

/* ------------------------------------------------------------------ */
/* Directives                                                          */

static void EmitString(const char *text, int addTerminator)
{
    const char *cursor = text;
    int length = 0;

    if (*cursor != '"') {
        Error("expected a string in double quotes, got '%s'", text);
        return;
    }
    cursor++;
    while (*cursor && *cursor != '"') {
        EmitByte(ParseLiteralCharacter(&cursor));
        if (++length == MaxStringLength) {
            Error("string is too long");
            return;
        }
    }
    if (*cursor != '"') Error("unterminated string");
    else if (*SkipSpaces(cursor + 1)) Error("unexpected '%s' after string", SkipSpaces(cursor + 1));
    if (addTerminator) EmitByte(0);
}

/* Evaluates a count or address that must be known in pass 1 to keep the layout fixed. */
static int EvaluateNow(const char *directive, const char *text, int *ok)
{
    int resolved, value = Evaluate(text, &resolved);
    *ok = resolved;
    if (!resolved && pass == 1)
        ErrorNow("'%s' needs a value that is defined above this line", directive);
    return value;
}

static void AssembleDirective(const char *directive, char *operands[], int count)
{
    int index, ok;

    if (!strcmp(directive, ".org")) {
        int address;
        if (!ExpectOperandCount(directive, count, 1)) return;
        address = EvaluateNow(directive, operands[0], &ok);
        if (!ok) return;
        if (address < locationCounter)
            Error("'.org 0x%04X' would move backwards from 0x%04X", address, locationCounter);
        else if (address > ImageSize)
            Error("'.org 0x%04X' is in I/O space", address);
        else
            locationCounter = address;
        BeginStatement(1);
    } else if (!strcmp(directive, ".word")) {
        BeginStatement(InstructionSize);
        for (index = 0; index < count; index++)
            EmitWord(FitSignedOrUnsigned(Evaluate(operands[index], NULL), 16, "word"));
    } else if (!strcmp(directive, ".byte")) {
        BeginStatement(1);
        for (index = 0; index < count; index++) {
            if (operands[index][0] == '"') EmitString(operands[index], 0);
            else EmitByte(FitSignedOrUnsigned(Evaluate(operands[index], NULL), 8, "byte"));
        }
    } else if (!strcmp(directive, ".ascii") || !strcmp(directive, ".asciz")) {
        BeginStatement(1);
        if (count == 0) Error("'%s' needs at least one string", directive);
        for (index = 0; index < count; index++)
            EmitString(operands[index], directive[5] == 'z');
    } else if (!strcmp(directive, ".space")) {
        int size;
        if (!ExpectOperandCount(directive, count, 1)) return;
        size = EvaluateNow(directive, operands[0], &ok);
        if (!ok) return;
        if (size < 0) { Error("'.space' size can't be negative"); return; }
        BeginStatement(1);
        for (index = 0; index < size; index++) EmitByte(0);
    } else if (!strcmp(directive, ".align")) {
        int alignment;
        if (!ExpectOperandCount(directive, count, 1)) return;
        alignment = EvaluateNow(directive, operands[0], &ok);
        if (!ok) return;
        if (alignment <= 0) { Error("'.align' needs a positive value"); return; }
        BeginStatement(alignment);
    } else if (!strcmp(directive, ".equ")) {
        const char *cursor;
        char name[MaxNameLength];
        if (!ExpectOperandCount(directive, count, 2)) return;
        cursor = operands[0];
        if (!ReadIdentifier(&cursor, name) || *cursor) {
            Error("'%s' is not a valid symbol name", operands[0]);
            return;
        }
        DefineSymbol(name, Evaluate(operands[1], NULL), 0);
    } else {
        Error("unknown directive '%s'", directive);
    }
}

/* ------------------------------------------------------------------ */
/* Lines                                                               */

static void AssembleLine(char *line)
{
    const char *cursor;
    char name[MaxNameLength], mnemonic[MaxNameLength];
    char *operands[MaxOperands];
    int count, index;

    StripComment(line);
    cursor = SkipSpaces(line);

    /* Leading "label:" definitions, or a "name = value" constant. */
    for (;;) {
        const char *start = cursor;
        if (!ReadIdentifier(&cursor, name)) break;
        cursor = SkipSpaces(cursor);
        if (*cursor == ':') {
            AddPendingLabel(name);
            cursor = SkipSpaces(cursor + 1);
            continue;
        }
        if (*cursor == '=') {
            DefineSymbol(name, Evaluate(cursor + 1, NULL), 0);
            return;
        }
        cursor = start;
        break;
    }

    if (*cursor == 0) return;
    if (!ReadIdentifier(&cursor, mnemonic)) {
        Error("expected an instruction or directive, got '%s'", cursor);
        return;
    }
    if (*cursor && *cursor != ' ' && *cursor != '\t') {
        Error("unexpected '%c' after '%s'", *cursor, mnemonic);
        return;
    }
    for (index = 0; mnemonic[index]; index++)
        mnemonic[index] = (char)tolower((unsigned char)mnemonic[index]);

    count = SplitOperands((char *)cursor, operands);

    if (mnemonic[0] == '.') AssembleDirective(mnemonic, operands, count);
    else if (!AssembleInstruction(mnemonic, operands, count)) Error("unknown instruction '%s'", mnemonic);
}

static void ListLine(const char *source)
{
    char hex[64] = "";
    int index, shown = lineByteCount < ListingBytesShown ? lineByteCount : ListingBytesShown;

    for (index = 0; index + 1 < shown; index += 2)
        sprintf(hex + strlen(hex), "%04X ", lineBytes[index] | (lineBytes[index + 1] << 8));
    if (index < shown) sprintf(hex + strlen(hex), "%02X ", lineBytes[index]);
    if (lineByteCount > ListingBytesShown) strcat(hex, "...");

    if (lineByteCount) fprintf(listingFile, "%04X  %-24s %5d  %s\n", lineFirstAddress, hex, lineNumber, source);
    else               fprintf(listingFile, "      %-24s %5d  %s\n", "", lineNumber, source);
}

static void ListSymbols(void)
{
    int index;
    fprintf(listingFile, "\nSymbols\n");
    for (index = 0; index < symbolCount; index++)
        fprintf(listingFile, "  %-24s 0x%04X  %6d   (line %d)\n", symbols[index].name,
                symbols[index].value & 0xFFFF, symbols[index].value, symbols[index].definedOnLine);
}

/* ------------------------------------------------------------------ */

static char *ReadWholeFile(const char *path)
{
    FILE *file = fopen(path, "rb");
    char *text;
    long size;

    if (!file) { perror(path); return NULL; }
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);
    text = malloc((size_t)size + 1);
    if (!text || fread(text, 1, (size_t)size, file) != (size_t)size) {
        fprintf(stderr, "%s: read failed\n", path);
        fclose(file);
        free(text);
        return NULL;
    }
    text[size] = 0;
    fclose(file);
    return text;
}

/* Splits text into lines in place. Returns the line count; *lines is malloc'd. */
static int SplitLines(char *text, char ***lines)
{
    int count = 1, index = 0;
    char *cursor;

    for (cursor = text; *cursor; cursor++) if (*cursor == '\n') count++;
    *lines = malloc(sizeof(char *) * (size_t)count);
    if (!*lines) return 0;

    (*lines)[index++] = text;
    for (cursor = text; *cursor; cursor++) {
        if (*cursor == '\n') {
            *cursor = 0;
            (*lines)[index++] = cursor + 1;
        }
    }
    for (index = 0; index < count; index++) {
        size_t length = strlen((*lines)[index]);
        if (length && (*lines)[index][length - 1] == '\r') (*lines)[index][length - 1] = 0;
    }
    return count;
}

static void Usage(const char *programName)
{
    fprintf(stderr,
            "usage: %s [-o output.bin] [-l] source.s\n"
            "  -o file   output file (default: source name with .bin)\n"
            "  -l        print a listing and symbol table to stdout\n",
            programName);
}

int main(int argc, char **argv)
{
    const char *outputPath = NULL;
    char defaultOutputPath[1024];
    char *source, **lines;
    int argumentIndex, lineCount, index;
    FILE *output;

    for (argumentIndex = 1; argumentIndex < argc; argumentIndex++) {
        const char *argument = argv[argumentIndex];
        if (!strcmp(argument, "-o") && argumentIndex + 1 < argc) outputPath = argv[++argumentIndex];
        else if (!strcmp(argument, "-l")) listingFile = stdout;
        else if (argument[0] == '-' || sourceName) { Usage(argv[0]); return 2; }
        else sourceName = argument;
    }
    if (!sourceName) { Usage(argv[0]); return 2; }

    if (!outputPath) {
        const char *dot = strrchr(sourceName, '.'), *slash = strrchr(sourceName, '/');
        size_t stemLength = (dot && (!slash || dot > slash)) ? (size_t)(dot - sourceName) : strlen(sourceName);
        if (stemLength > sizeof defaultOutputPath - 5) stemLength = sizeof defaultOutputPath - 5;
        memcpy(defaultOutputPath, sourceName, stemLength);
        strcpy(defaultOutputPath + stemLength, ".bin");
        outputPath = defaultOutputPath;
    }

    source = ReadWholeFile(sourceName);
    if (!source) return 1;
    lineCount = SplitLines(source, &lines);

    for (pass = 1; pass <= 2; pass++) {
        locationCounter = 0;
        statementAddress = 0;
        pendingLabelCount = 0;
        outOfSpaceReported = 0;

        for (index = 0; index < lineCount; index++) {
            char buffer[MaxLineLength];
            lineNumber = index + 1;
            lineByteCount = 0;
            if (strlen(lines[index]) >= sizeof buffer) {
                if (pass == 1) ErrorNow("line is longer than %d characters", MaxLineLength - 1);
                continue;
            }
            strcpy(buffer, lines[index]);
            AssembleLine(buffer);
            if (pass == 2 && listingFile) ListLine(lines[index]);
        }
        FlushPendingLabels();   /* labels at the very end of the file */
    }

    if (listingFile) ListSymbols();

    if (errorCount) {
        fprintf(stderr, "%d error%s, no output written\n", errorCount, errorCount == 1 ? "" : "s");
        return 1;
    }

    output = fopen(outputPath, "wb");
    if (!output) { perror(outputPath); return 1; }
    if (fwrite(image, 1, (size_t)highestAddress, output) != (size_t)highestAddress) {
        fprintf(stderr, "%s: write failed\n", outputPath);
        fclose(output);
        return 1;
    }
    fclose(output);
    fprintf(stderr, "%s: %d bytes -> %s\n", sourceName, highestAddress, outputPath);

    free(lines);
    free(source);
    return 0;
}