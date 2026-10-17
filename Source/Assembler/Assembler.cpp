#include "Assembler.h"

#include "Kestrel/Isa.h"

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace Kestrel {

namespace {

constexpr int ImageSize = Address::IoBase;  // everything above is I/O space
constexpr int MaxSymbols = 4096;
constexpr int MaxNameLength = 64;
constexpr int MaxLineLength = 1024;
constexpr int MaxOperands = 32;
constexpr int MaxPendingLabels = 64;
constexpr int MaxStringLength = 1024;
constexpr int ListingBytesShown = 8;

struct Symbol {
    std::string name;
    int value;
    int definedOnLine;
};

struct PendingLabel {
    std::string name;
    int line;
};

// ---------------------------------------------------------------------------
// Text helpers

std::string Format(const char* format, va_list arguments) {
    char text[1024];
    vsnprintf(text, sizeof text, format, arguments);
    return text;
}

const char* SkipSpaces(const char* text) {
    while (*text == ' ' || *text == '\t') {
        text++;
    }
    return text;
}

void TrimRight(char* text) {
    size_t length = strlen(text);
    while (length && isspace(static_cast<unsigned char>(text[length - 1]))) {
        text[--length] = 0;
    }
}

bool IsIdentifierStart(int character) {
    return isalpha(character) || character == '_' || character == '.';
}

bool IsIdentifierCharacter(int character) {
    return isalnum(character) || character == '_' || character == '.';
}

bool EqualsIgnoringCase(const char* a, const char* b) {
    while (*a && *b) {
        if (tolower(static_cast<unsigned char>(*a)) != tolower(static_cast<unsigned char>(*b))) {
            return false;
        }
        a++;
        b++;
    }
    return *a == *b;
}

// Reads an identifier at *text into name and advances past it. Returns false if there is none.
bool ReadIdentifier(const char** text, char* name) {
    const char* cursor = *text;
    int length = 0;
    if (!IsIdentifierStart(static_cast<unsigned char>(*cursor))) {
        return false;
    }
    while (IsIdentifierCharacter(static_cast<unsigned char>(*cursor))) {
        if (length < MaxNameLength - 1) {
            name[length++] = *cursor;
        }
        cursor++;
    }
    name[length] = 0;
    *text = cursor;
    return true;
}

// Cuts the line at the first ';' that is not inside quotes.
void StripComment(char* line) {
    char quote = 0;
    for (; *line; line++) {
        if (quote) {
            if (*line == '\\' && line[1]) {
                line++;
            }
            else if (*line == quote) {
                quote = 0;
            }
        }
        else if (*line == '"' || *line == '\'') {
            quote = *line;
        }
        else if (*line == ';') {
            *line = 0;
            return;
        }
    }
}

int HexDigitValue(int character) {
    if (isdigit(character)) {
        return character - '0';
    }
    character = tolower(character);
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    return -1;
}

int ParseRegister(const char* text) {
    if ((text[0] == 'r' || text[0] == 'R') && text[1] >= '0' && text[1] <= '7' && text[2] == 0) {
        return text[1] - '0';
    }
    if (EqualsIgnoringCase(text, "sp")) {
        return StackPointer;
    }
    if (EqualsIgnoringCase(text, "lr")) {
        return LinkRegister;
    }
    return -1;
}

const InstructionInfo* FindInstruction(const char* mnemonic) {
    for (int index = 0; index < InstructionCount; index++) {
        if (!strcmp(Instructions[index].mnemonic, mnemonic)) {
            return &Instructions[index];
        }
    }
    return nullptr;
}

int FindBranchCondition(const char* mnemonic) {
    if (mnemonic[0] != 'b') {
        return -1;
    }
    for (int index = 0; index < BranchCount; index++) {
        if (!strcmp(Branches[index].suffix, mnemonic + 1)) {
            return static_cast<int>(Branches[index].condition);
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// One assembly: two passes over the source, the first to measure and collect labels, the
// second to emit.

class Assembly {
   public:
    Assembly(const std::string& sourceName, bool makeListing) : sourceName(sourceName), makeListing(makeListing) {}

    AssemblyResult Run(const std::string& text);

   private:
    std::string sourceName;
    bool makeListing;
    AssemblyResult result;

    std::vector<Symbol> symbols;
    std::vector<uint8_t> image = std::vector<uint8_t>(ImageSize, 0);
    int highestAddress = 0;    // one past the last byte written
    int locationCounter = 0;   // where the next byte goes
    int statementAddress = 0;  // start of the current statement, for $ and branches
    int pass = 0;              // 1 = measure and collect labels, 2 = emit
    int lineNumber = 0;
    bool expressionUnresolved = false;
    bool outOfSpaceReported = false;
    std::vector<PendingLabel> pendingLabels;

    uint8_t lineBytes[ListingBytesShown] = {};
    int lineByteCount = 0;
    int lineFirstAddress = 0;

    void ReportError(const char* format, va_list arguments);
    void Error(const char* format, ...);
    void ErrorNow(const char* format, ...);

    int SplitOperands(char* text, char* operands[]);
    int ExpectRegister(const char* text);
    Symbol* FindSymbol(const char* name);
    void DefineSymbol(const char* name, int value, bool isLabel);
    void AddPendingLabel(const char* name);
    void FlushPendingLabels();

    int ParseLiteralCharacter(const char** text);
    int ParseNumber(const char** text);
    bool ExpectClosingParenthesis(const char** text);
    int ParsePrimary(const char** text);
    int ParseUnary(const char** text);
    int ParseMultiplicative(const char** text);
    int ParseAdditive(const char** text);
    int ParseShift(const char** text);
    int ParseBitwiseAnd(const char** text);
    int ParseBitwiseXor(const char** text);
    int ParseExpression(const char** text);
    int Evaluate(const char* text, bool* resolved);

    int FitSigned(int value, int bits, const char* what);
    int FitSignedOrUnsigned(int value, int bits, const char* what);
    int BranchOffset(const char* targetText, int bits);

    void EmitByte(int value);
    void EmitWord(int value);
    void AlignTo(int alignment);
    void BeginStatement(int alignment);
    bool ExpectOperandCount(const char* mnemonic, int count, int expected);

    void ParseMemoryOperand(const char* text, int* base, int* offset);
    int AssembleTableInstruction(const InstructionInfo* info, char* operands[], int count);
    int AssemblePseudoInstruction(const char* mnemonic, char* operands[], int count, int words[2]);
    bool AssembleInstruction(const char* mnemonic, char* operands[], int count);

    void EmitString(const char* text, bool addTerminator);
    int EvaluateNow(const char* directive, const char* text, bool* ok);
    void AssembleDirective(const char* directive, char* operands[], int count);
    void AssembleLine(char* line);

    void ListLine(const std::string& source);
    void ListSymbols();
};

// ---------------------------------------------------------------------------
// Error reporting

void Assembly::ReportError(const char* format, va_list arguments) {
    result.errors.push_back(sourceName + ":" + std::to_string(lineNumber) + ": error: " + Format(format, arguments));
}

// Most problems are reported only in pass 2, so each appears exactly once.
void Assembly::Error(const char* format, ...) {
    if (pass != 2) {
        return;
    }
    va_list arguments;
    va_start(arguments, format);
    ReportError(format, arguments);
    va_end(arguments);
}

// Problems that would make the two passes disagree are reported immediately.
void Assembly::ErrorNow(const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    ReportError(format, arguments);
    va_end(arguments);
}

// Splits "a, [b+1], 'c'" on top-level commas, in place. Returns the operand count.
int Assembly::SplitOperands(char* text, char* operands[]) {
    int count = 0, depth = 0;
    char quote = 0, *start = text;

    if (*SkipSpaces(text) == 0) {
        return 0;
    }

    for (char* cursor = text;; cursor++) {
        char character = *cursor;
        if (quote && character) {
            if (character == '\\' && cursor[1]) {
                cursor++;
            }
            else if (character == quote) {
                quote = 0;
            }
            continue;
        }
        if (character == '"' || character == '\'') {
            quote = character;
        }
        else if (character == '(' || character == '[') {
            depth++;
        }
        else if ((character == ')' || character == ']') && depth > 0) {
            depth--;
        }
        else if (character == 0 || (character == ',' && depth == 0)) {
            *cursor = 0;
            start = const_cast<char*>(SkipSpaces(start));
            TrimRight(start);
            if (*start == 0) {
                Error("empty operand");
            }
            if (count == MaxOperands) {
                Error("too many operands");
                return count;
            }
            operands[count++] = start;
            if (character == 0) {
                return count;
            }
            start = cursor + 1;
        }
    }
}

// ---------------------------------------------------------------------------
// Symbols and registers

int Assembly::ExpectRegister(const char* text) {
    int registerNumber = ParseRegister(text);
    if (registerNumber < 0) {
        Error("expected a register (r0-r7, sp, lr), got '%s'", text);
        return 0;
    }
    return registerNumber;
}

Symbol* Assembly::FindSymbol(const char* name) {
    for (Symbol& symbol : symbols) {
        if (symbol.name == name) {
            return &symbol;
        }
    }
    return nullptr;
}

void Assembly::DefineSymbol(const char* name, int value, bool isLabel) {
    Symbol* symbol = FindSymbol(name);

    if (pass == 1) {
        if (ParseRegister(name) >= 0) {
            ErrorNow("'%s' is a register name and can't be used as a symbol", name);
            return;
        }
        if (symbol) {
            ErrorNow("'%s' is already defined on line %d", name, symbol->definedOnLine);
            return;
        }
        if (static_cast<int>(symbols.size()) == MaxSymbols) {
            ErrorNow("too many symbols");
            return;
        }
        symbols.push_back({name, value, lineNumber});
        return;
    }

    // Pass 2: only the definition pass 1 accepted gets updated.
    if (!symbol || symbol->definedOnLine != lineNumber) {
        return;
    }
    if (isLabel && symbol->value != value) {
        Error("internal error: label '%s' moved between passes (0x%04X -> 0x%04X)", name, symbol->value, value);
    }
    symbol->value = value;
}

// Labels wait until the next statement starts, so they pick up any alignment padding.
void Assembly::AddPendingLabel(const char* name) {
    if (static_cast<int>(pendingLabels.size()) == MaxPendingLabels) {
        Error("too many labels in a row");
        return;
    }
    pendingLabels.push_back({name, lineNumber});
}

void Assembly::FlushPendingLabels() {
    int savedLine = lineNumber;
    for (const PendingLabel& label : pendingLabels) {
        lineNumber = label.line;
        DefineSymbol(label.name.c_str(), locationCounter, true);
    }
    lineNumber = savedLine;
    pendingLabels.clear();
}

// ---------------------------------------------------------------------------
// Expressions

// Reads one possibly-escaped character inside a '...' or "..." literal.
int Assembly::ParseLiteralCharacter(const char** text) {
    const char* cursor = *text;
    int value;

    if (*cursor != '\\') {
        *text = cursor + 1;
        return static_cast<unsigned char>(*cursor);
    }
    cursor++;
    switch (*cursor) {
        case 'n':
            value = '\n';
            break;
        case 't':
            value = '\t';
            break;
        case 'r':
            value = '\r';
            break;
        case '0':
            value = 0;
            break;
        case '\\':
            value = '\\';
            break;
        case '\'':
            value = '\'';
            break;
        case '"':
            value = '"';
            break;
        case 'x': {
            int digits = 0;
            value = 0;
            cursor++;
            while (digits < 2 && HexDigitValue(static_cast<unsigned char>(*cursor)) >= 0) {
                value = value * 16 + HexDigitValue(static_cast<unsigned char>(*cursor));
                cursor++;
                digits++;
            }
            if (!digits) {
                Error("'\\x' needs one or two hex digits");
            }
            *text = cursor;
            return value;
        }
        case 0:
            Error("escape '\\' at end of line");
            *text = cursor;
            return 0;
        default:
            Error("unknown escape '\\%c'", *cursor);
            value = static_cast<unsigned char>(*cursor);
            break;
    }
    *text = cursor + 1;
    return value;
}

int Assembly::ParseNumber(const char** text) {
    const char* cursor = *text;
    long value = 0;
    int base = 10, digits = 0;

    if (cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X')) {
        base = 16;
        cursor += 2;
    }
    else if (cursor[0] == '0' && (cursor[1] == 'b' || cursor[1] == 'B')) {
        base = 2;
        cursor += 2;
    }

    for (;; cursor++) {
        if (*cursor == '_') {
            continue;  // allow 0b1010_0101
        }
        int digit = HexDigitValue(static_cast<unsigned char>(*cursor));
        if (digit < 0 || digit >= base) {
            break;
        }
        if (value < 0x1000000) {
            value = value * base + digit;
        }
        digits++;
    }
    if (!digits) {
        Error("malformed number");
    }
    if (IsIdentifierCharacter(static_cast<unsigned char>(*cursor))) {
        Error("malformed number (unexpected '%c')", *cursor);
    }
    *text = cursor;
    return static_cast<int>(value);
}

bool Assembly::ExpectClosingParenthesis(const char** text) {
    const char* cursor = SkipSpaces(*text);
    if (*cursor != ')') {
        Error("missing ')'");
        return false;
    }
    *text = cursor + 1;
    return true;
}

int Assembly::ParsePrimary(const char** text) {
    const char* cursor = SkipSpaces(*text);
    char name[MaxNameLength];
    int value = 0;

    if (*cursor == '(') {
        cursor++;
        value = ParseExpression(&cursor);
        ExpectClosingParenthesis(&cursor);
    }
    else if (isdigit(static_cast<unsigned char>(*cursor))) {
        value = ParseNumber(&cursor);
    }
    else if (*cursor == '\'') {
        cursor++;
        if (*cursor == 0 || *cursor == '\'') {
            Error("empty character literal");
        }
        else {
            value = ParseLiteralCharacter(&cursor);
        }
        if (*cursor == '\'') {
            cursor++;
        }
        else {
            Error("missing closing ' in character literal");
        }
    }
    else if (*cursor == '$') {
        cursor++;
        value = statementAddress;
    }
    else if (ReadIdentifier(&cursor, name)) {
        const char* after = SkipSpaces(cursor);
        bool isLow = EqualsIgnoringCase(name, "lo"), isHigh = EqualsIgnoringCase(name, "hi");
        if (*after == '(' && (isLow || isHigh)) {
            cursor = after + 1;
            value = ParseExpression(&cursor);
            ExpectClosingParenthesis(&cursor);
            value = isHigh ? (value >> 8) & 0xFF : value & 0xFF;
        }
        else if (ParseRegister(name) >= 0) {
            Error("register '%s' can't be used in an expression", name);
        }
        else if (Symbol* symbol = FindSymbol(name)) {
            value = symbol->value;
        }
        else {
            expressionUnresolved = true;  // may be a forward reference in pass 1
            Error("undefined symbol '%s'", name);
        }
    }
    else {
        if (*cursor) {
            Error("unexpected '%c' in expression", *cursor);
        }
        else {
            Error("missing value in expression");
        }
        cursor += strlen(cursor);
    }
    *text = cursor;
    return value;
}

int Assembly::ParseUnary(const char** text) {
    const char* cursor = SkipSpaces(*text);
    if (*cursor == '-') {
        *text = cursor + 1;
        return -ParseUnary(text);
    }
    if (*cursor == '+') {
        *text = cursor + 1;
        return ParseUnary(text);
    }
    if (*cursor == '~') {
        *text = cursor + 1;
        return ~ParseUnary(text);
    }
    *text = cursor;
    return ParsePrimary(text);
}

int Assembly::ParseMultiplicative(const char** text) {
    int value = ParseUnary(text);
    for (;;) {
        const char* cursor = SkipSpaces(*text);
        char operation = *cursor;
        if (operation != '*' && operation != '/' && operation != '%') {
            return value;
        }
        *text = cursor + 1;
        int right = ParseUnary(text);
        if (operation == '*') {
            value *= right;
        }
        else if (right == 0) {
            if (!expressionUnresolved) {
                Error("division by zero");
            }
        }
        else if (operation == '/') {
            value /= right;
        }
        else {
            value %= right;
        }
    }
}

int Assembly::ParseAdditive(const char** text) {
    int value = ParseMultiplicative(text);
    for (;;) {
        const char* cursor = SkipSpaces(*text);
        if (*cursor == '+') {
            *text = cursor + 1;
            value += ParseMultiplicative(text);
        }
        else if (*cursor == '-') {
            *text = cursor + 1;
            value -= ParseMultiplicative(text);
        }
        else {
            return value;
        }
    }
}

int Assembly::ParseShift(const char** text) {
    int value = ParseAdditive(text);
    for (;;) {
        const char* cursor = SkipSpaces(*text);
        if (cursor[0] == '<' && cursor[1] == '<') {
            *text = cursor + 2;
            value <<= ParseAdditive(text) & 31;
        }
        else if (cursor[0] == '>' && cursor[1] == '>') {
            *text = cursor + 2;
            value >>= ParseAdditive(text) & 31;
        }
        else {
            return value;
        }
    }
}

int Assembly::ParseBitwiseAnd(const char** text) {
    int value = ParseShift(text);
    for (;;) {
        const char* cursor = SkipSpaces(*text);
        if (*cursor != '&') {
            return value;
        }
        *text = cursor + 1;
        value &= ParseShift(text);
    }
}

int Assembly::ParseBitwiseXor(const char** text) {
    int value = ParseBitwiseAnd(text);
    for (;;) {
        const char* cursor = SkipSpaces(*text);
        if (*cursor != '^') {
            return value;
        }
        *text = cursor + 1;
        value ^= ParseBitwiseAnd(text);
    }
}

int Assembly::ParseExpression(const char** text) {
    int value = ParseBitwiseXor(text);
    for (;;) {
        const char* cursor = SkipSpaces(*text);
        if (*cursor != '|') {
            return value;
        }
        *text = cursor + 1;
        value |= ParseBitwiseXor(text);
    }
}

// Evaluates a whole operand. *resolved (if given) is false when it used an unknown symbol.
int Assembly::Evaluate(const char* text, bool* resolved) {
    const char* cursor = text;
    expressionUnresolved = false;
    int value = ParseExpression(&cursor);
    cursor = SkipSpaces(cursor);
    if (*cursor) {
        Error("unexpected '%s' after expression", cursor);
    }
    if (resolved) {
        *resolved = !expressionUnresolved;
    }
    return value;
}

// ---------------------------------------------------------------------------
// Range checks

int Assembly::FitSigned(int value, int bits, const char* what) {
    int minimum = -(1 << (bits - 1)), maximum = (1 << (bits - 1)) - 1;
    if (value < minimum || value > maximum) {
        Error("%s %d doesn't fit (allowed %d..%d)", what, value, minimum, maximum);
    }
    return value & ((1 << bits) - 1);
}

// Accepts either a signed or an unsigned value of the given width.
int Assembly::FitSignedOrUnsigned(int value, int bits, const char* what) {
    int minimum = -(1 << (bits - 1)), maximum = (1 << bits) - 1;
    if (value < minimum || value > maximum) {
        Error("%s %d doesn't fit in %d bits (allowed %d..%d)", what, value, bits, minimum, maximum);
    }
    return value & ((1 << bits) - 1);
}

// Turns an absolute target into a word offset from the next instruction.
int Assembly::BranchOffset(const char* targetText, int bits) {
    bool resolved;
    int target = Evaluate(targetText, &resolved);
    int minimum = -(1 << (bits - 1)), maximum = (1 << (bits - 1)) - 1;

    if (!resolved) {
        return 0;
    }
    if (target & 1) {
        Error("target 0x%04X is not an even address", target);
        return 0;
    }
    int offset = (target - (statementAddress + InstructionSize)) / InstructionSize;
    if (offset < minimum || offset > maximum) {
        Error("target 0x%04X is out of reach (%+d words; this instruction reaches %d..%+d)%s", target, offset, minimum,
              maximum, bits == Offset8Field.bits ? " -- branch around a 'j' instead" : "");
        return 0;
    }
    return offset & ((1 << bits) - 1);
}

// ---------------------------------------------------------------------------
// Output

void Assembly::EmitByte(int value) {
    if (locationCounter >= ImageSize) {
        if (pass == 2 && !outOfSpaceReported) {
            Error("program runs past 0x%04X, where I/O space begins", ImageSize);
            outOfSpaceReported = true;
        }
        locationCounter++;
        return;
    }
    if (pass == 2) {
        image[static_cast<size_t>(locationCounter)] = static_cast<uint8_t>(value);
        if (locationCounter + 1 > highestAddress) {
            highestAddress = locationCounter + 1;
        }
        if (lineByteCount == 0) {
            lineFirstAddress = locationCounter;
        }
        if (lineByteCount < ListingBytesShown) {
            lineBytes[lineByteCount] = static_cast<uint8_t>(value);
        }
        lineByteCount++;
    }
    locationCounter++;
}

void Assembly::EmitWord(int value) {
    EmitByte(value & 0xFF);
    EmitByte((value >> 8) & 0xFF);
}

void Assembly::AlignTo(int alignment) {
    while (locationCounter % alignment) {
        EmitByte(0);
    }
}

// Every statement that produces output starts here.
void Assembly::BeginStatement(int alignment) {
    AlignTo(alignment);
    lineByteCount = 0;  // leave padding out of the listing
    FlushPendingLabels();
    statementAddress = locationCounter;
}

bool Assembly::ExpectOperandCount(const char* mnemonic, int count, int expected) {
    if (count == expected) {
        return true;
    }
    Error("'%s' takes %d operand%s, got %d", mnemonic, expected, expected == 1 ? "" : "s", count);
    return false;
}

// ---------------------------------------------------------------------------
// Instructions

// Parses "[ra]", "[ra+expr]" or "[ra-expr]".
void Assembly::ParseMemoryOperand(const char* text, int* base, int* offset) {
    char inner[MaxLineLength], name[MaxNameLength];
    size_t length = strlen(text);

    *base = 0;
    *offset = 0;
    if (length < 2 || text[0] != '[' || text[length - 1] != ']') {
        Error("expected a memory operand like [r1+4], got '%s'", text);
        return;
    }
    memcpy(inner, text + 1, length - 2);
    inner[length - 2] = 0;

    const char* cursor = SkipSpaces(inner);
    if (!ReadIdentifier(&cursor, name) || ParseRegister(name) < 0) {
        Error("memory operand '%s' must start with a register", text);
        return;
    }
    *base = ParseRegister(name);
    cursor = SkipSpaces(cursor);
    if (*cursor == 0) {
        return;
    }
    if (*cursor != '+' && *cursor != '-') {
        Error("expected '+' or '-' after the register in '%s'", text);
        return;
    }
    *offset = Evaluate(cursor, nullptr);
}

int Assembly::AssembleTableInstruction(const InstructionInfo* info, char* operands[], int count) {
    const char* name = info->mnemonic;
    int destination, sourceA, sourceB, value;

    switch (info->form) {
        case InstructionForm::ThreeRegisters:
            if (!ExpectOperandCount(name, count, 3)) {
                return 0;
            }
            destination = ExpectRegister(operands[0]);
            sourceA = ExpectRegister(operands[1]);
            sourceB = ExpectRegister(operands[2]);
            return EncodeRegisters(info->opcode, destination, sourceA, sourceB, info->function);

        case InstructionForm::TwoRegisters:
            if (!ExpectOperandCount(name, count, 2)) {
                return 0;
            }
            destination = ExpectRegister(operands[0]);
            sourceA = ExpectRegister(operands[1]);
            return EncodeRegisters(info->opcode, destination, sourceA, 0, info->function);

        case InstructionForm::CompareRegisters:
            if (!ExpectOperandCount(name, count, 2)) {
                return 0;
            }
            sourceA = ExpectRegister(operands[0]);
            sourceB = ExpectRegister(operands[1]);
            return EncodeRegisters(info->opcode, 0, sourceA, sourceB, info->function);

        case InstructionForm::RegisterImmediate:
            if (!ExpectOperandCount(name, count, 3)) {
                return 0;
            }
            destination = ExpectRegister(operands[0]);
            sourceA = ExpectRegister(operands[1]);
            value = FitSigned(Evaluate(operands[2], nullptr), Immediate6Field.bits, "immediate");
            return EncodeImmediate(info->opcode, destination, sourceA, value);

        case InstructionForm::CompareImmediate:
            if (!ExpectOperandCount(name, count, 2)) {
                return 0;
            }
            sourceA = ExpectRegister(operands[0]);
            value = FitSigned(Evaluate(operands[1], nullptr), Immediate6Field.bits, "immediate");
            return EncodeImmediate(info->opcode, 0, sourceA, value);

        case InstructionForm::LoadImmediate:
            if (!ExpectOperandCount(name, count, 2)) {
                return 0;
            }
            destination = ExpectRegister(operands[0]);
            value = FitSignedOrUnsigned(Evaluate(operands[1], nullptr), Immediate8Field.bits, "immediate");
            return EncodeLong(info->opcode, destination, value);

        case InstructionForm::Memory: {
            int offset;
            if (!ExpectOperandCount(name, count, 2)) {
                return 0;
            }
            destination = ExpectRegister(operands[0]);
            ParseMemoryOperand(operands[1], &sourceA, &offset);
            value = FitSigned(offset, Immediate6Field.bits, "offset");
            return EncodeImmediate(info->opcode, destination, sourceA, value);
        }

        case InstructionForm::Jump:
            if (!ExpectOperandCount(name, count, 1)) {
                return 0;
            }
            return EncodeJump(info->opcode, BranchOffset(operands[0], Offset12Field.bits));

        case InstructionForm::SystemRegister:
            if (!ExpectOperandCount(name, count, 1)) {
                return 0;
            }
            return EncodeSystem(static_cast<SystemFunction>(info->function), ExpectRegister(operands[0]));

        case InstructionForm::SystemBare:
            if (!ExpectOperandCount(name, count, 0)) {
                return 0;
            }
            return EncodeSystem(static_cast<SystemFunction>(info->function), 0);
    }
    return 0;
}

// Returns the number of words written to words[], or 0 if not a pseudo-instruction.
int Assembly::AssemblePseudoInstruction(const char* mnemonic, char* operands[], int count, int words[2]) {
    if (!strcmp(mnemonic, "nop")) {
        ExpectOperandCount(mnemonic, count, 0);
        words[0] = EncodeRegisters(Opcode::Alu2, 0, 0, 0, static_cast<int>(Alu2Function::Move));
        return 1;
    }
    if (!strcmp(mnemonic, "ret")) {
        ExpectOperandCount(mnemonic, count, 0);
        words[0] = EncodeSystem(SystemFunction::JumpRegister, LinkRegister);
        return 1;
    }
    if (!strcmp(mnemonic, "call")) {
        words[0] = ExpectOperandCount(mnemonic, count, 1)
                       ? EncodeJump(Opcode::JumpAndLink, BranchOffset(operands[0], Offset12Field.bits))
                       : 0;
        return 1;
    }
    if (!strcmp(mnemonic, "subi")) {
        words[0] = 0;
        if (ExpectOperandCount(mnemonic, count, 3)) {
            int destination = ExpectRegister(operands[0]);
            int source = ExpectRegister(operands[1]);
            int value = FitSigned(-Evaluate(operands[2], nullptr), Immediate6Field.bits, "negated immediate");
            words[0] = EncodeImmediate(Opcode::AddImmediate, destination, source, value);
        }
        return 1;
    }
    if (!strcmp(mnemonic, "li") || !strcmp(mnemonic, "la")) {
        words[0] = words[1] = 0;
        if (ExpectOperandCount(mnemonic, count, 2)) {
            int destination = ExpectRegister(operands[0]);
            int value = FitSignedOrUnsigned(Evaluate(operands[1], nullptr), WordBits, "value");
            words[0] = EncodeLong(Opcode::LoadImmediate, destination, value & 0xFF);
            words[1] = EncodeLong(Opcode::LoadHighImmediate, destination, (value >> 8) & 0xFF);
        }
        return 2;
    }
    if (!strcmp(mnemonic, "push")) {
        int source = ExpectOperandCount(mnemonic, count, 1) ? ExpectRegister(operands[0]) : 0;
        words[0] = EncodeImmediate(Opcode::AddImmediate, StackPointer, StackPointer, -InstructionSize);
        words[1] = EncodeImmediate(Opcode::StoreWord, source, StackPointer, 0);
        return 2;
    }
    if (!strcmp(mnemonic, "pop")) {
        int destination = ExpectOperandCount(mnemonic, count, 1) ? ExpectRegister(operands[0]) : 0;
        if (destination == StackPointer) {
            Error("'pop sp' would lose the stack; load it with lw instead");
        }
        words[0] = EncodeImmediate(Opcode::LoadWord, destination, StackPointer, 0);
        words[1] = EncodeImmediate(Opcode::AddImmediate, StackPointer, StackPointer, InstructionSize);
        return 2;
    }
    return 0;
}

// Returns false if the mnemonic isn't known.
bool Assembly::AssembleInstruction(const char* mnemonic, char* operands[], int count) {
    const InstructionInfo* info = FindInstruction(mnemonic);
    int condition = FindBranchCondition(mnemonic);

    BeginStatement(InstructionSize);

    if (info) {
        EmitWord(AssembleTableInstruction(info, operands, count));
        return true;
    }
    if (condition >= 0) {
        int offset = ExpectOperandCount(mnemonic, count, 1) ? BranchOffset(operands[0], Offset8Field.bits) : 0;
        EmitWord(EncodeBranch(static_cast<Condition>(condition), offset));
        return true;
    }
    int words[2];
    int wordCount = AssemblePseudoInstruction(mnemonic, operands, count, words);
    for (int index = 0; index < wordCount; index++) {
        EmitWord(words[index]);
    }
    return wordCount > 0;
}

// ---------------------------------------------------------------------------
// Directives

void Assembly::EmitString(const char* text, bool addTerminator) {
    const char* cursor = text;
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
    if (*cursor != '"') {
        Error("unterminated string");
    }
    else if (*SkipSpaces(cursor + 1)) {
        Error("unexpected '%s' after string", SkipSpaces(cursor + 1));
    }
    if (addTerminator) {
        EmitByte(0);
    }
}

// Evaluates a count or address that must be known in pass 1 to keep the layout fixed.
int Assembly::EvaluateNow(const char* directive, const char* text, bool* ok) {
    bool resolved;
    int value = Evaluate(text, &resolved);
    *ok = resolved;
    if (!resolved && pass == 1) {
        ErrorNow("'%s' needs a value that is defined above this line", directive);
    }
    return value;
}

void Assembly::AssembleDirective(const char* directive, char* operands[], int count) {
    bool ok;

    if (!strcmp(directive, ".org")) {
        if (!ExpectOperandCount(directive, count, 1)) {
            return;
        }
        int address = EvaluateNow(directive, operands[0], &ok);
        if (!ok) {
            return;
        }
        if (address < locationCounter) {
            Error("'.org 0x%04X' would move backwards from 0x%04X", address, locationCounter);
        }
        else if (address > ImageSize) {
            Error("'.org 0x%04X' is in I/O space", address);
        }
        else {
            locationCounter = address;
        }
        BeginStatement(1);
    }
    else if (!strcmp(directive, ".word")) {
        BeginStatement(InstructionSize);
        for (int index = 0; index < count; index++) {
            EmitWord(FitSignedOrUnsigned(Evaluate(operands[index], nullptr), WordBits, "word"));
        }
    }
    else if (!strcmp(directive, ".byte")) {
        BeginStatement(1);
        for (int index = 0; index < count; index++) {
            if (operands[index][0] == '"') {
                EmitString(operands[index], false);
            }
            else {
                EmitByte(FitSignedOrUnsigned(Evaluate(operands[index], nullptr), 8, "byte"));
            }
        }
    }
    else if (!strcmp(directive, ".ascii") || !strcmp(directive, ".asciz")) {
        BeginStatement(1);
        if (count == 0) {
            Error("'%s' needs at least one string", directive);
        }
        for (int index = 0; index < count; index++) {
            EmitString(operands[index], directive[5] == 'z');
        }
    }
    else if (!strcmp(directive, ".space")) {
        if (!ExpectOperandCount(directive, count, 1)) {
            return;
        }
        int size = EvaluateNow(directive, operands[0], &ok);
        if (!ok) {
            return;
        }
        if (size < 0) {
            Error("'.space' size can't be negative");
            return;
        }
        BeginStatement(1);
        for (int index = 0; index < size; index++) {
            EmitByte(0);
        }
    }
    else if (!strcmp(directive, ".align")) {
        if (!ExpectOperandCount(directive, count, 1)) {
            return;
        }
        int alignment = EvaluateNow(directive, operands[0], &ok);
        if (!ok) {
            return;
        }
        if (alignment <= 0) {
            Error("'.align' needs a positive value");
            return;
        }
        BeginStatement(alignment);
    }
    else if (!strcmp(directive, ".equ")) {
        char name[MaxNameLength];
        if (!ExpectOperandCount(directive, count, 2)) {
            return;
        }
        const char* cursor = operands[0];
        if (!ReadIdentifier(&cursor, name) || *cursor) {
            Error("'%s' is not a valid symbol name", operands[0]);
            return;
        }
        DefineSymbol(name, Evaluate(operands[1], nullptr), false);
    }
    else {
        Error("unknown directive '%s'", directive);
    }
}

// ---------------------------------------------------------------------------
// Lines

void Assembly::AssembleLine(char* line) {
    char name[MaxNameLength], mnemonic[MaxNameLength];
    char* operands[MaxOperands];

    StripComment(line);
    const char* cursor = SkipSpaces(line);

    // Leading "label:" definitions, or a "name = value" constant.
    for (;;) {
        const char* start = cursor;
        if (!ReadIdentifier(&cursor, name)) {
            break;
        }
        cursor = SkipSpaces(cursor);
        if (*cursor == ':') {
            AddPendingLabel(name);
            cursor = SkipSpaces(cursor + 1);
            continue;
        }
        if (*cursor == '=') {
            DefineSymbol(name, Evaluate(cursor + 1, nullptr), false);
            return;
        }
        cursor = start;
        break;
    }

    if (*cursor == 0) {
        return;
    }
    if (!ReadIdentifier(&cursor, mnemonic)) {
        Error("expected an instruction or directive, got '%s'", cursor);
        return;
    }
    if (*cursor && *cursor != ' ' && *cursor != '\t') {
        Error("unexpected '%c' after '%s'", *cursor, mnemonic);
        return;
    }
    for (int index = 0; mnemonic[index]; index++) {
        mnemonic[index] = static_cast<char>(tolower(static_cast<unsigned char>(mnemonic[index])));
    }

    int count = SplitOperands(const_cast<char*>(cursor), operands);

    if (mnemonic[0] == '.') {
        AssembleDirective(mnemonic, operands, count);
    }
    else if (!AssembleInstruction(mnemonic, operands, count)) {
        Error("unknown instruction '%s'", mnemonic);
    }
}

void Assembly::ListLine(const std::string& source) {
    char hex[64] = "", text[MaxLineLength + 64];
    int index, shown = lineByteCount < ListingBytesShown ? lineByteCount : ListingBytesShown;

    for (index = 0; index + 1 < shown; index += 2) {
        snprintf(hex + strlen(hex), sizeof hex - strlen(hex), "%04X ", lineBytes[index] | (lineBytes[index + 1] << 8));
    }
    if (index < shown) {
        snprintf(hex + strlen(hex), sizeof hex - strlen(hex), "%02X ", lineBytes[index]);
    }
    if (lineByteCount > ListingBytesShown) {
        strcat(hex, "...");
    }

    if (lineByteCount) {
        snprintf(text, sizeof text, "%04X  %-24s %5d  ", lineFirstAddress, hex, lineNumber);
    }
    else {
        snprintf(text, sizeof text, "      %-24s %5d  ", "", lineNumber);
    }
    result.listing += text + source + "\n";
}

void Assembly::ListSymbols() {
    char text[MaxNameLength + 64];
    result.listing += "\nSymbols\n";
    for (const Symbol& symbol : symbols) {
        snprintf(text, sizeof text, "  %-24s 0x%04X  %6d   (line %d)\n", symbol.name.c_str(), symbol.value & 0xFFFF,
                 symbol.value, symbol.definedOnLine);
        result.listing += text;
    }
}

AssemblyResult Assembly::Run(const std::string& text) {
    // Lines, without their '\n' or a '\r' before it.
    std::vector<std::string> lines;
    for (size_t start = 0;;) {
        size_t end = text.find('\n', start);
        std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    for (pass = 1; pass <= 2; pass++) {
        locationCounter = 0;
        statementAddress = 0;
        pendingLabels.clear();
        outOfSpaceReported = false;

        for (size_t index = 0; index < lines.size(); index++) {
            char buffer[MaxLineLength];
            lineNumber = static_cast<int>(index) + 1;
            lineByteCount = 0;
            if (lines[index].size() >= sizeof buffer) {
                if (pass == 1) {
                    ErrorNow("line is longer than %d characters", MaxLineLength - 1);
                }
                continue;
            }
            strcpy(buffer, lines[index].c_str());
            AssembleLine(buffer);
            if (pass == 2 && makeListing) {
                ListLine(lines[index]);
            }
        }
        FlushPendingLabels();  // labels at the very end of the file
    }

    if (makeListing) {
        ListSymbols();
    }
    if (result.errors.empty()) {
        result.image.assign(image.begin(), image.begin() + highestAddress);
    }
    return result;
}

}  // namespace

AssemblyResult Assemble(const std::string& sourceName, const std::string& text, bool makeListing) {
    return Assembly(sourceName, makeListing).Run(text);
}

}  // namespace Kestrel
