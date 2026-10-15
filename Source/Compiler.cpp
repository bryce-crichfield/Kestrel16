// isa16cc -- a small C compiler for the ISA-16 architecture, single file, C++17.
//
// Build:  c++ -std=c++17 -O2 -Wall -o isa16cc isa16cc.cpp
// Run:    ./isa16cc prog.c              writes prog.s
//         ./isa16cc -o out.s prog.c     choose the output name
// Then:   ./isa16asm prog.s             writes prog.bin
//
// There is no preprocessor: one .c file in, one .s file out. A line starting
// with '#' is an error.
//
// Supported: char / short / int / long (all 16-bit except char), signed and
// unsigned, _Bool, void, pointers (incl. function pointers), arrays (multi-dim),
// structs and unions (nested, self-referencing), enums, typedef, static and
// extern, global and local initializers (nested braces, strings), string
// literals, all C operators (incl. compound assignment, ?:, comma, casts,
// sizeof), if/else, while, do/while, for (with declarations), switch/case/
// default, break, continue, goto/labels, return, recursion, variadic calls.
//
// Not supported: floating point, bit-fields, passing or returning structs by
// value (struct assignment works), designated initializers, 32-bit long,
// compound literals, VLAs.
//
// ---------------------------------------------------------------------------
// Code generation model
//
//   r0        accumulator / return value
//   r1-r4     scratch (caller-saved); r4 holds call targets
//   r5        frame pointer (callee-saved)
//   r6 = sp   stack pointer, r7 = lr
//
//   Arguments are pushed right-to-left, 2 bytes each; the caller pops them.
//   Frame:  [fp+4+2i] = argument i, [fp+2] = saved lr, [fp+0] = saved fp,
//           [fp-n]    = locals.
//   Calls use `la r4, target ; jalr r4`, so there is no range limit.
//   *, /, % are done by small runtime routines appended only when used.
//   Global C names get a leading '_' so they never clash with register names.
// ---------------------------------------------------------------------------

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using std::string;
using std::vector;

static string sourceName;

[[noreturn]] static void Fatal(int line, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    fprintf(stderr, "%s:%d: error: ", sourceName.c_str(), line);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
    exit(1);
}

// ===========================================================================
// Lexer

enum TokenKind { TokIdent, TokKeyword, TokNumber, TokString, TokPunct, TokEof };

struct Token {
    TokenKind kind;
    string text;        // identifier / keyword / punctuator, or string bytes
    long long value = 0;
    bool isUnsigned = false;
    int line = 0;
};

static const std::set<string> Keywords = {
    "auto", "break", "case", "char", "const", "continue", "default", "do", "double",
    "else", "enum", "extern", "float", "for", "goto", "if", "inline", "int", "long",
    "register", "restrict", "return", "short", "signed", "sizeof", "static", "struct",
    "switch", "typedef", "union", "unsigned", "void", "volatile", "while", "_Bool"};

static const char* Punctuators[] = {
    "<<=", ">>=", "...", "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
    "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
    "+", "-", "*", "/", "%", "&", "|", "^", "~", "!", "<", ">", "=", "?", ":", ";", ",",
    ".", "(", ")", "[", "]", "{", "}"};

static int HexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int ReadEscape(const string& src, size_t& i, int line)
{
    // src[i] is the character after the backslash
    char c = src[i++];
    switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'a': return 7;
    case 'b': return 8;
    case 'f': return 12;
    case 'v': return 11;
    case 'e': return 27;
    case '\\': return '\\';
    case '\'': return '\'';
    case '"': return '"';
    case '?': return '?';
    case 'x': {
        int value = 0, digits = 0;
        while (i < src.size() && HexValue(src[i]) >= 0) { value = value * 16 + HexValue(src[i++]); digits++; }
        if (!digits) Fatal(line, "\\x needs hex digits");
        return value & 0xFF;
    }
    default:
        if (c >= '0' && c <= '7') {
            int value = c - '0', digits = 1;
            while (digits < 3 && i < src.size() && src[i] >= '0' && src[i] <= '7') { value = value * 8 + (src[i++] - '0'); digits++; }
            return value & 0xFF;
        }
        Fatal(line, "unknown escape '\\%c'", c);
    }
}

static vector<Token> Lex(const string& src)
{
    vector<Token> tokens;
    size_t i = 0;
    int line = 1;
    bool lineStart = true;

    while (i < src.size()) {
        char c = src[i];
        if (c == '\n') { line++; i++; lineStart = true; continue; }
        if (isspace((unsigned char)c)) { i++; continue; }
        if (c == '/' && i + 1 < src.size() && src[i + 1] == '/') {
            while (i < src.size() && src[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < src.size() && src[i + 1] == '*') {
            int startLine = line;
            i += 2;
            while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) { if (src[i] == '\n') line++; i++; }
            if (i + 1 >= src.size()) Fatal(startLine, "unterminated comment");
            i += 2;
            continue;
        }
        if (c == '#' && lineStart)
            Fatal(line, "preprocessor directives are not supported (no #include/#define)");
        lineStart = false;

        Token tok;
        tok.line = line;

        if (isdigit((unsigned char)c)) {
            unsigned long long value = 0;
            if (c == '0' && i + 1 < src.size() && (src[i + 1] == 'x' || src[i + 1] == 'X')) {
                i += 2;
                if (HexValue(src[i]) < 0) Fatal(line, "malformed hex number");
                while (i < src.size() && HexValue(src[i]) >= 0) value = value * 16 + HexValue(src[i++]);
            } else if (c == '0' && i + 1 < src.size() && (src[i + 1] == 'b' || src[i + 1] == 'B')) {
                i += 2;
                while (i < src.size() && (src[i] == '0' || src[i] == '1')) value = value * 2 + (src[i++] - '0');
            } else if (c == '0') {
                while (i < src.size() && src[i] >= '0' && src[i] <= '7') value = value * 8 + (src[i++] - '0');
            } else {
                while (i < src.size() && isdigit((unsigned char)src[i])) value = value * 10 + (src[i++] - '0');
            }
            if (i < src.size() && (src[i] == '.' || src[i] == 'e' || src[i] == 'E'))
                Fatal(line, "floating point is not supported");
            bool isUnsigned = false;
            while (i < src.size() && strchr("uUlL", src[i])) { if (src[i] == 'u' || src[i] == 'U') isUnsigned = true; i++; }
            if (i < src.size() && (isalnum((unsigned char)src[i]) || src[i] == '_')) Fatal(line, "malformed number");
            if (value > 0xFFFF) Fatal(line, "constant %llu does not fit in 16 bits", value);
            tok.kind = TokNumber;
            tok.value = (long long)value;
            tok.isUnsigned = isUnsigned || value > 0x7FFF;
            tokens.push_back(tok);
            continue;
        }
        if (c == '\'') {
            i++;
            if (i >= src.size() || src[i] == '\'' || src[i] == '\n') Fatal(line, "empty character literal");
            int value;
            if (src[i] == '\\') { i++; value = ReadEscape(src, i, line); }
            else value = (unsigned char)src[i++];
            if (i >= src.size() || src[i] != '\'') Fatal(line, "unterminated character literal");
            i++;
            tok.kind = TokNumber;
            tok.value = (signed char)value;   // char is signed
            tokens.push_back(tok);
            continue;
        }
        if (c == '"') {
            i++;
            string bytes;
            while (i < src.size() && src[i] != '"') {
                if (src[i] == '\n') Fatal(line, "unterminated string");
                if (src[i] == '\\') { i++; bytes += (char)ReadEscape(src, i, line); }
                else bytes += src[i++];
            }
            if (i >= src.size()) Fatal(line, "unterminated string");
            i++;
            if (!tokens.empty() && tokens.back().kind == TokString) {
                tokens.back().text += bytes;    // "abc" "def" concatenation
                continue;
            }
            tok.kind = TokString;
            tok.text = bytes;
            tokens.push_back(tok);
            continue;
        }
        if (isalpha((unsigned char)c) || c == '_') {
            size_t start = i;
            while (i < src.size() && (isalnum((unsigned char)src[i]) || src[i] == '_')) i++;
            tok.text = src.substr(start, i - start);
            tok.kind = Keywords.count(tok.text) ? TokKeyword : TokIdent;
            tokens.push_back(tok);
            continue;
        }
        bool found = false;
        for (const char* p : Punctuators) {
            size_t length = strlen(p);
            if (src.compare(i, length, p) == 0) {
                tok.kind = TokPunct;
                tok.text = p;
                i += length;
                found = true;
                break;
            }
        }
        if (!found) Fatal(line, "unexpected character '%c'", c);
        tokens.push_back(tok);
    }
    Token eof;
    eof.kind = TokEof;
    eof.text = "end of file";
    eof.line = line;
    tokens.push_back(eof);
    return tokens;
}

// ===========================================================================
// Types

enum Kind { KindVoid, KindBool, KindChar, KindInt, KindPointer, KindArray, KindFunction, KindStruct };

struct Type;

struct Member {
    string name;
    Type* type;
    int offset;
};

struct StructInfo {
    vector<Member> members;
    bool complete = false;
    bool isUnion = false;
};

struct Type {
    Kind kind;
    int size = 0;
    int align = 1;
    bool isUnsigned = false;
    Type* base = nullptr;           // pointee, element, or return type
    int length = 0;                 // array length, -1 = incomplete
    vector<Type*> params;
    vector<string> paramNames;
    bool variadic = false;
    bool oldStyle = false;          // "int f()" -- arguments not checked
    StructInfo* info = nullptr;
};

static Type* MakeType(Kind kind, int size, int align, bool isUnsigned = false)
{
    Type* t = new Type;
    t->kind = kind;
    t->size = size;
    t->align = align;
    t->isUnsigned = isUnsigned;
    return t;
}

static Type* TypeVoid  = MakeType(KindVoid, 1, 1);
static Type* TypeBool  = MakeType(KindBool, 1, 1, true);
static Type* TypeChar  = MakeType(KindChar, 1, 1);
static Type* TypeUChar = MakeType(KindChar, 1, 1, true);
static Type* TypeInt   = MakeType(KindInt, 2, 2);
static Type* TypeUInt  = MakeType(KindInt, 2, 2, true);

static Type* PointerTo(Type* base)
{
    Type* t = MakeType(KindPointer, 2, 2, true);
    t->base = base;
    return t;
}

static Type* ArrayOf(Type* base, int length)
{
    Type* t = MakeType(KindArray, length < 0 ? 0 : base->size * length, base->align);
    t->base = base;
    t->length = length;
    return t;
}

static bool IsInteger(Type* t) { return t->kind == KindBool || t->kind == KindChar || t->kind == KindInt; }
static bool IsScalar(Type* t) { return IsInteger(t) || t->kind == KindPointer; }
static int AlignUp(int n, int a) { return (n + a - 1) / a * a; }

static Type* Decay(Type* t)
{
    if (t->kind == KindArray) return PointerTo(t->base);
    if (t->kind == KindFunction) return PointerTo(t);
    return t;
}

static Type* Promote(Type* t)
{
    if (t->kind == KindInt) return t;
    if (IsInteger(t)) return TypeInt;
    return t;
}

static Type* UsualArithmetic(Type* a, Type* b)
{
    a = Promote(a);
    b = Promote(b);
    return (a->isUnsigned || b->isUnsigned) ? TypeUInt : TypeInt;
}

// ===========================================================================
// Variables and AST

struct Var {
    string name;
    string label;                   // assembler label for globals
    Type* type = nullptr;
    bool isLocal = false;
    int offset = 0;                 // frame offset for locals
    bool isDefined = false;         // has a non-extern definition (or a body)
    bool hasInit = false;
    vector<uint8_t> data;
    std::map<int, std::pair<string, int>> relocations;  // offset -> label+addend word
};

enum NodeKind {
    NodeNumber, NodeVar, NodeAdd, NodeSub, NodeMul, NodeDiv, NodeMod, NodeAnd, NodeOr, NodeXor,
    NodeShl, NodeShr, NodeEq, NodeNe, NodeLt, NodeLe, NodeLogAnd, NodeLogOr, NodeNot, NodeBitNot,
    NodeNeg, NodeAddr, NodeDeref, NodeMember, NodeAssign, NodeCompound, NodeIncDec, NodeCond,
    NodeComma, NodeCall, NodeCast, NodePtrDiff, NodeZero,
    NodeBlock, NodeIf, NodeWhile, NodeDo, NodeFor, NodeReturn, NodeBreak, NodeContinue,
    NodeSwitch, NodeCase, NodeGoto, NodeLabel, NodeExprStmt
};

struct Node {
    NodeKind kind;
    Type* type = nullptr;
    Node *a = nullptr, *b = nullptr, *c = nullptr, *d = nullptr;
    vector<Node*> list;
    long long value = 0;            // number, var byte offset, inc delta, case value, ptrdiff size
    Var* var = nullptr;
    Member* member = nullptr;
    NodeKind op = NodeNumber;       // compound assignment operator
    bool opUnsigned = false;        // unsigned compare / divide / shift
    bool isPost = false;
    string name;
    int line = 0;
    int label = 0;
};

static Node* NewNode(NodeKind kind, int line)
{
    Node* n = new Node;
    n->kind = kind;
    n->line = line;
    return n;
}

static Node* NewNumber(long long value, int line, Type* type = TypeInt)
{
    Node* n = NewNode(NodeNumber, line);
    n->value = value;
    n->type = type;
    return n;
}

// ===========================================================================
// Parser state

static vector<Token> tokens;
static size_t pos;

struct Symbol {
    Var* var = nullptr;
    Type* typedefType = nullptr;
    bool isEnumConstant = false;
    long long enumValue = 0;
};

static vector<std::map<string, Symbol>> scopes;
static vector<std::map<string, Type*>> tagScopes;
static vector<Var*> globals;
static std::map<string, Var*> globalsByName;
static vector<Node*> switchStack;
static int frameSize;
static Type* currentReturnType;
static int labelCounter;
static int stringCounter;
static int staticCounter;

static Token& Tok() { return tokens[pos]; }
static Token& TokAt(size_t i) { return tokens[i < tokens.size() ? i : tokens.size() - 1]; }

static bool IsText(const Token& t, const char* s)
{
    return (t.kind == TokPunct || t.kind == TokKeyword) && t.text == s;
}
static bool Is(const char* s) { return IsText(Tok(), s); }
static bool Accept(const char* s)
{
    if (Is(s)) { pos++; return true; }
    return false;
}
static void Expect(const char* s)
{
    if (!Accept(s)) Fatal(Tok().line, "expected '%s' but found '%s'", s, Tok().text.c_str());
}
static string ExpectIdent()
{
    if (Tok().kind != TokIdent) Fatal(Tok().line, "expected an identifier but found '%s'", Tok().text.c_str());
    return tokens[pos++].text;
}

static void EnterScope() { scopes.emplace_back(); tagScopes.emplace_back(); }
static void LeaveScope() { scopes.pop_back(); tagScopes.pop_back(); }

static Symbol* FindSymbol(const string& name)
{
    for (int i = (int)scopes.size() - 1; i >= 0; i--) {
        auto it = scopes[i].find(name);
        if (it != scopes[i].end()) return &it->second;
    }
    return nullptr;
}

static Type* FindTag(const string& name)
{
    for (int i = (int)tagScopes.size() - 1; i >= 0; i--) {
        auto it = tagScopes[i].find(name);
        if (it != tagScopes[i].end()) return it->second;
    }
    return nullptr;
}

static bool IsTypeToken(const Token& t)
{
    static const std::set<string> typeWords = {
        "void", "_Bool", "char", "short", "int", "long", "signed", "unsigned", "struct", "union",
        "enum", "const", "volatile", "typedef", "static", "extern", "inline", "register", "auto",
        "restrict", "float", "double"};
    if (t.kind == TokKeyword) return typeWords.count(t.text) > 0;
    if (t.kind == TokIdent) {
        Symbol* s = FindSymbol(t.text);
        return s && s->typedefType;
    }
    return false;
}

static Node* Expression();
static Node* Assignment();
static Node* Conditional();
static Node* CastExpression();
static Node* Statement();
static Node* CompoundStatement();
static Type* ParseDeclarator(Type* type, string& name);
struct StorageClass;
Type* DeclarationSpecifiers(StorageClass* storage);
static Type* TypeSuffix(Type* type);
static long long ConstantExpression();
static long long Evaluate(Node* n, string* label = nullptr);

// ===========================================================================
// Declarations

struct StorageClass {
    bool isTypedef = false;
    bool isStatic = false;
    bool isExtern = false;
};

static Type* StructDeclaration(bool isUnion)
{
    int line = Tok().line;
    string tag;
    if (Tok().kind == TokIdent) tag = tokens[pos++].text;

    if (!tag.empty() && !Is("{")) {
        Type* existing = FindTag(tag);
        if (existing) return existing;
        Type* t = MakeType(KindStruct, 0, 1);
        t->info = new StructInfo;
        t->info->isUnion = isUnion;
        tagScopes.back()[tag] = t;
        return t;
    }

    Type* t = nullptr;
    if (!tag.empty()) {
        auto it = tagScopes.back().find(tag);
        if (it != tagScopes.back().end()) {
            if (it->second->info->complete) Fatal(line, "redefinition of '%s %s'", isUnion ? "union" : "struct", tag.c_str());
            t = it->second;
        }
    }
    if (!t) {
        t = MakeType(KindStruct, 0, 1);
        t->info = new StructInfo;
        t->info->isUnion = isUnion;
        if (!tag.empty()) tagScopes.back()[tag] = t;
    }

    Expect("{");
    int offset = 0, size = 0, align = 1;
    while (!Accept("}")) {
        Type* base = DeclarationSpecifiers(nullptr);
        if (Accept(";")) continue;
        do {
            string name;
            int memberLine = Tok().line;
            Type* memberType = ParseDeclarator(base, name);
            if (Is(":")) Fatal(memberLine, "bit-fields are not supported");
            if (memberType->kind == KindStruct && !memberType->info->complete)
                Fatal(memberLine, "member '%s' has incomplete type", name.c_str());
            if (memberType->kind == KindFunction || memberType->kind == KindVoid)
                Fatal(memberLine, "member '%s' has invalid type", name.c_str());
            if (memberType->kind == KindArray && memberType->length < 0)
                Fatal(memberLine, "flexible array members are not supported");
            int memberOffset;
            if (isUnion) {
                memberOffset = 0;
                if (memberType->size > size) size = memberType->size;
            } else {
                offset = AlignUp(offset, memberType->align);
                memberOffset = offset;
                offset += memberType->size;
                size = offset;
            }
            if (memberType->align > align) align = memberType->align;
            t->info->members.push_back({name, memberType, memberOffset});
        } while (Accept(","));
        Expect(";");
    }
    t->size = AlignUp(size, align);
    t->align = align;
    t->info->complete = true;
    return t;
}

static Type* EnumDeclaration()
{
    if (Tok().kind == TokIdent) pos++;      // tag is accepted and ignored; enums are int
    if (!Accept("{")) return TypeInt;
    long long value = 0;
    while (!Accept("}")) {
        string name = ExpectIdent();
        if (Accept("=")) value = ConstantExpression();
        Symbol s;
        s.isEnumConstant = true;
        s.enumValue = value;
        scopes.back()[name] = s;
        value++;
        if (!Accept(",")) { Expect("}"); break; }
    }
    return TypeInt;
}

Type* DeclarationSpecifiers(StorageClass* storage)
{
    int line = Tok().line;
    int nUnsigned = 0, nChar = 0, nVoid = 0, nBool = 0, nOther = 0;
    Type* named = nullptr;

    for (;;) {
        Token& t = Tok();
        if (t.kind == TokKeyword) {
            const string& s = t.text;
            if (s == "typedef" || s == "static" || s == "extern") {
                if (!storage) Fatal(t.line, "'%s' is not allowed here", s.c_str());
                if (s == "typedef") storage->isTypedef = true;
                if (s == "static") storage->isStatic = true;
                if (s == "extern") storage->isExtern = true;
                pos++;
                continue;
            }
            if (s == "const" || s == "volatile" || s == "inline" || s == "register" || s == "auto" || s == "restrict") { pos++; continue; }
            if (s == "float" || s == "double") Fatal(t.line, "floating point is not supported");
            if (s == "struct" || s == "union") {
                if (named || nOther || nChar || nVoid || nBool) Fatal(t.line, "invalid type");
                pos++;
                named = StructDeclaration(s == "union");
                continue;
            }
            if (s == "enum") {
                if (named || nOther || nChar || nVoid || nBool) Fatal(t.line, "invalid type");
                pos++;
                named = EnumDeclaration();
                continue;
            }
            if (s == "unsigned") nUnsigned++;
            else if (s == "signed" || s == "short" || s == "long" || s == "int") nOther++;
            else if (s == "char") nChar++;
            else if (s == "void") nVoid++;
            else if (s == "_Bool") nBool++;
            else break;
            pos++;
            continue;
        }
        if (t.kind == TokIdent && !named && !nUnsigned && !nOther && !nChar && !nVoid && !nBool) {
            Symbol* sym = FindSymbol(t.text);
            if (sym && sym->typedefType) { named = sym->typedefType; pos++; continue; }
        }
        break;
    }
    if (named) return named;
    if (nVoid) return TypeVoid;
    if (nBool) return TypeBool;
    if (nChar) return nUnsigned ? TypeUChar : TypeChar;
    if (nUnsigned || nOther) return nUnsigned ? TypeUInt : TypeInt;
    Fatal(line, "expected a type but found '%s'", Tok().text.c_str());
}

static Type* FunctionParameters(Type* returnType)
{
    Type* f = MakeType(KindFunction, 1, 1);
    f->base = returnType;
    if (Is("void") && IsText(TokAt(pos + 1), ")")) { pos += 2; return f; }
    if (Accept(")")) { f->oldStyle = true; return f; }
    for (;;) {
        if (Accept("...")) { f->variadic = true; Expect(")"); break; }
        Type* base = DeclarationSpecifiers(nullptr);
        string name;
        Type* t = ParseDeclarator(base, name);
        if (t->kind == KindArray) t = PointerTo(t->base);
        else if (t->kind == KindFunction) t = PointerTo(t);
        f->params.push_back(t);
        f->paramNames.push_back(name);
        if (Accept(")")) break;
        Expect(",");
    }
    return f;
}

static Type* TypeSuffix(Type* type)
{
    if (Accept("(")) return FunctionParameters(type);
    if (Accept("[")) {
        int line = Tok().line;
        int length = -1;
        if (!Is("]")) {
            long long n = ConstantExpression();
            if (n < 0) Fatal(line, "array size is negative");
            length = (int)n;
        }
        Expect("]");
        Type* element = TypeSuffix(type);
        if (element->kind == KindArray && element->length < 0) Fatal(line, "array has incomplete element type");
        return ArrayOf(element, length);
    }
    return type;
}

static Type* ParseDeclarator(Type* type, string& name)
{
    while (Accept("*")) {
        type = PointerTo(type);
        while (Accept("const") || Accept("volatile") || Accept("restrict")) {}
    }
    if (Is("(")) {
        const Token& next = TokAt(pos + 1);
        bool nested = IsText(next, "*") || IsText(next, "(") || IsText(next, "[") ||
                      (next.kind == TokIdent && !IsTypeToken(next));
        if (nested) {
            pos++;
            size_t start = pos;
            int depth = 1;
            while (depth) {
                if (Tok().kind == TokEof) Fatal(Tok().line, "unbalanced parentheses in declarator");
                if (Is("(")) depth++;
                else if (Is(")")) depth--;
                pos++;
            }
            Type* outer = TypeSuffix(type);
            size_t end = pos;
            pos = start;
            Type* result = ParseDeclarator(outer, name);
            Expect(")");
            pos = end;
            return result;
        }
    }
    if (Tok().kind == TokIdent) name = tokens[pos++].text;
    return TypeSuffix(type);
}

static Type* TypeName()
{
    Type* base = DeclarationSpecifiers(nullptr);
    string name;
    Type* t = ParseDeclarator(base, name);
    if (!name.empty()) Fatal(Tok().line, "unexpected name '%s' in type", name.c_str());
    return t;
}

// ===========================================================================
// Expression construction (with typing)

static void NeedScalar(Node* n, const char* what)
{
    Type* t = Decay(n->type);
    if (!IsScalar(t)) Fatal(n->line, "invalid operand to %s", what);
}

static void NeedInteger(Node* n, const char* what)
{
    if (!IsInteger(n->type)) Fatal(n->line, "operand of %s must be an integer", what);
}

static Node* MakeCast(Node* e, Type* t)
{
    if (t->kind == KindStruct || (e->type->kind == KindStruct && t->kind != KindVoid))
        Fatal(e->line, "invalid cast involving a struct");
    Node* n = NewNode(NodeCast, e->line);
    n->a = e;
    n->type = t;
    return n;
}

static Node* MakeBinary(NodeKind kind, Node* a, Node* b, int line, const char* what)
{
    NeedInteger(a, what);
    NeedInteger(b, what);
    Node* n = NewNode(kind, line);
    n->a = a;
    n->b = b;
    n->type = (kind == NodeShl || kind == NodeShr) ? Promote(a->type) : UsualArithmetic(a->type, b->type);
    n->opUnsigned = n->type->isUnsigned;
    return n;
}

static int ElementSize(Type* pointer, int line)
{
    Type* base = pointer->base;
    if (base->kind == KindVoid || base->kind == KindFunction) return 1;
    if (base->kind == KindStruct && !base->info->complete) Fatal(line, "arithmetic on pointer to incomplete type");
    return base->size;
}

static Node* Scaled(Node* index, int size)
{
    if (size == 1) return index;
    Node* m = NewNode(NodeMul, index->line);
    m->a = index;
    m->b = NewNumber(size, index->line);
    m->type = TypeInt;
    return m;
}

static Node* MakeAdd(Node* a, Node* b, int line)
{
    Type* ta = Decay(a->type);
    Type* tb = Decay(b->type);
    if (IsInteger(ta) && IsInteger(tb)) return MakeBinary(NodeAdd, a, b, line, "+");
    if (IsInteger(ta) && tb->kind == KindPointer) { std::swap(a, b); std::swap(ta, tb); }
    if (ta->kind == KindPointer && IsInteger(tb)) {
        Node* n = NewNode(NodeAdd, line);
        n->a = a;
        n->b = Scaled(b, ElementSize(ta, line));
        n->type = ta;
        return n;
    }
    Fatal(line, "invalid operands to +");
}

static Node* MakeSub(Node* a, Node* b, int line)
{
    Type* ta = Decay(a->type);
    Type* tb = Decay(b->type);
    if (IsInteger(ta) && IsInteger(tb)) return MakeBinary(NodeSub, a, b, line, "-");
    if (ta->kind == KindPointer && IsInteger(tb)) {
        Node* n = NewNode(NodeSub, line);
        n->a = a;
        n->b = Scaled(b, ElementSize(ta, line));
        n->type = ta;
        return n;
    }
    if (ta->kind == KindPointer && tb->kind == KindPointer) {
        Node* n = NewNode(NodePtrDiff, line);
        n->a = a;
        n->b = b;
        n->value = ElementSize(ta, line);
        n->type = TypeInt;
        return n;
    }
    Fatal(line, "invalid operands to -");
}

static Node* MakeCompare(NodeKind kind, Node* a, Node* b, int line, bool swap)
{
    NeedScalar(a, "comparison");
    NeedScalar(b, "comparison");
    if (swap) std::swap(a, b);
    Node* n = NewNode(kind, line);
    n->a = a;
    n->b = b;
    n->type = TypeInt;
    Type* ta = Decay(a->type);
    Type* tb = Decay(b->type);
    n->opUnsigned = ta->kind == KindPointer || tb->kind == KindPointer || UsualArithmetic(ta, tb)->isUnsigned;
    return n;
}

static bool IsLvalue(Node* n)
{
    return n->kind == NodeVar || n->kind == NodeDeref || n->kind == NodeMember;
}

static Node* MakeAssign(Node* lhs, Node* rhs, int line)
{
    if (!IsLvalue(lhs)) Fatal(line, "left side of assignment is not assignable");
    if (lhs->type->kind == KindArray || lhs->type->kind == KindFunction) Fatal(line, "cannot assign to an array or function");
    if (lhs->type->kind == KindStruct) {
        if (rhs->type != lhs->type) Fatal(line, "incompatible struct types in assignment");
    } else {
        NeedScalar(rhs, "assignment");
    }
    Node* n = NewNode(NodeAssign, line);
    n->a = lhs;
    n->b = rhs;
    n->type = lhs->type;
    return n;
}

static Node* MakeDeref(Node* e, int line)
{
    Type* t = Decay(e->type);
    if (t->kind != KindPointer) Fatal(line, "cannot dereference a non-pointer");
    if (t->base->kind == KindVoid) Fatal(line, "cannot dereference a void pointer");
    Node* n = NewNode(NodeDeref, line);
    n->a = e;
    n->type = t->base;
    return n;
}

static Node* MakeMember(Node* s, const string& name, int line)
{
    if (s->type->kind != KindStruct) Fatal(line, "'.%s' used on something that is not a struct", name.c_str());
    if (!s->type->info->complete) Fatal(line, "use of incomplete struct");
    for (Member& m : s->type->info->members) {
        if (m.name == name) {
            Node* n = NewNode(NodeMember, line);
            n->a = s;
            n->member = &m;
            n->type = m.type;
            return n;
        }
    }
    Fatal(line, "no member named '%s'", name.c_str());
}

static Node* MakeIncDec(Node* e, int delta, bool isPost, int line)
{
    if (!IsLvalue(e)) Fatal(line, "operand of ++/-- is not assignable");
    NeedScalar(e, "++/--");
    Node* n = NewNode(NodeIncDec, line);
    n->a = e;
    n->type = e->type;
    n->isPost = isPost;
    n->value = e->type->kind == KindPointer ? delta * ElementSize(e->type, line) : delta;
    return n;
}

static Var* NewStringVar(const string& bytes)
{
    Var* g = new Var;
    g->label = ".Lstr" + std::to_string(++stringCounter);
    g->type = ArrayOf(TypeChar, (int)bytes.size() + 1);
    g->isDefined = true;
    g->hasInit = true;
    g->data.assign(bytes.begin(), bytes.end());
    g->data.push_back(0);
    globals.push_back(g);
    return g;
}

static Node* VarNode(Var* v, int line)
{
    Node* n = NewNode(NodeVar, line);
    n->var = v;
    n->type = v->type;
    return n;
}

// ===========================================================================
// Expression parsing

static Node* CallExpression(Node* fn, int line)
{
    Type* ft = fn->type;
    if (ft->kind == KindPointer && ft->base->kind == KindFunction) ft = ft->base;
    if (ft->kind != KindFunction) Fatal(line, "called object is not a function");
    Node* n = NewNode(NodeCall, line);
    n->a = fn;
    n->type = ft->base;
    if (n->type->kind == KindStruct) Fatal(line, "returning structs by value is not supported (pass a pointer)");
    if (!Accept(")")) {
        do {
            Node* arg = Assignment();
            size_t index = n->list.size();
            if (arg->type->kind == KindStruct) Fatal(arg->line, "passing structs by value is not supported (pass a pointer)");
            if (arg->type->kind == KindVoid) Fatal(arg->line, "void value used as an argument");
            if (!ft->oldStyle && index < ft->params.size()) arg = MakeCast(arg, ft->params[index]);
            else if (!ft->oldStyle && !ft->variadic) Fatal(arg->line, "too many arguments in call");
            n->list.push_back(arg);
        } while (Accept(","));
        Expect(")");
    }
    if (!ft->oldStyle && n->list.size() < ft->params.size()) Fatal(line, "too few arguments in call");
    return n;
}

static Node* Primary()
{
    Token& t = Tok();
    int line = t.line;
    if (Accept("(")) {
        Node* e = Expression();
        Expect(")");
        return e;
    }
    if (t.kind == TokNumber) {
        pos++;
        return NewNumber(t.value, line, t.isUnsigned ? TypeUInt : TypeInt);
    }
    if (t.kind == TokString) {
        pos++;
        return VarNode(NewStringVar(t.text), line);
    }
    if (t.kind == TokIdent) {
        pos++;
        Symbol* s = FindSymbol(t.text);
        if (!s || s->typedefType) {
            if (!s && Is("(")) Fatal(line, "call to undeclared function '%s' (declare a prototype first)", t.text.c_str());
            Fatal(line, "undeclared identifier '%s'", t.text.c_str());
        }
        if (s->isEnumConstant) return NewNumber(s->enumValue, line);
        return VarNode(s->var, line);
    }
    Fatal(line, "expected an expression but found '%s'", t.text.c_str());
}

static Node* Postfix()
{
    Node* e = Primary();
    for (;;) {
        int line = Tok().line;
        if (Accept("[")) {
            Node* index = Expression();
            Expect("]");
            e = MakeDeref(MakeAdd(e, index, line), line);
        } else if (Accept("(")) {
            e = CallExpression(e, line);
        } else if (Accept(".")) {
            e = MakeMember(e, ExpectIdent(), line);
        } else if (Accept("->")) {
            e = MakeMember(MakeDeref(e, line), ExpectIdent(), line);
        } else if (Accept("++")) {
            e = MakeIncDec(e, 1, true, line);
        } else if (Accept("--")) {
            e = MakeIncDec(e, -1, true, line);
        } else {
            return e;
        }
    }
}

static Node* Unary()
{
    int line = Tok().line;
    if (Accept("+")) { Node* e = CastExpression(); NeedInteger(e, "unary +"); return MakeCast(e, Promote(e->type)); }
    if (Accept("-")) {
        Node* e = CastExpression();
        NeedInteger(e, "unary -");
        Node* n = NewNode(NodeNeg, line);
        n->a = e;
        n->type = Promote(e->type);
        return n;
    }
    if (Accept("!")) {
        Node* e = CastExpression();
        NeedScalar(e, "!");
        Node* n = NewNode(NodeNot, line);
        n->a = e;
        n->type = TypeInt;
        return n;
    }
    if (Accept("~")) {
        Node* e = CastExpression();
        NeedInteger(e, "~");
        Node* n = NewNode(NodeBitNot, line);
        n->a = e;
        n->type = Promote(e->type);
        return n;
    }
    if (Accept("&")) {
        Node* e = CastExpression();
        if (!IsLvalue(e)) Fatal(line, "cannot take the address of this expression");
        Node* n = NewNode(NodeAddr, line);
        n->a = e;
        n->type = PointerTo(e->type);
        return n;
    }
    if (Accept("*")) return MakeDeref(CastExpression(), line);
    if (Accept("++")) return MakeIncDec(Unary(), 1, false, line);
    if (Accept("--")) return MakeIncDec(Unary(), -1, false, line);
    if (Accept("sizeof")) {
        Type* t;
        if (Is("(") && IsTypeToken(TokAt(pos + 1))) {
            pos++;
            t = TypeName();
            Expect(")");
        } else {
            t = Unary()->type;
        }
        if (t->kind == KindStruct && !t->info->complete) Fatal(line, "sizeof of incomplete type");
        if (t->kind == KindArray && t->length < 0) Fatal(line, "sizeof of incomplete array");
        return NewNumber(t->size, line, TypeUInt);
    }
    return Postfix();
}

static Node* CastExpression()
{
    if (Is("(") && IsTypeToken(TokAt(pos + 1))) {
        int line = Tok().line;
        pos++;
        Type* t = TypeName();
        Expect(")");
        if (Is("{")) Fatal(line, "compound literals are not supported");
        Node* e = CastExpression();
        if (t->kind != KindVoid) NeedScalar(e, "cast");
        return MakeCast(e, t);
    }
    return Unary();
}

static Node* Multiplicative()
{
    Node* e = CastExpression();
    for (;;) {
        int line = Tok().line;
        if (Accept("*")) e = MakeBinary(NodeMul, e, CastExpression(), line, "*");
        else if (Accept("/")) e = MakeBinary(NodeDiv, e, CastExpression(), line, "/");
        else if (Accept("%")) e = MakeBinary(NodeMod, e, CastExpression(), line, "%");
        else return e;
    }
}

static Node* Additive()
{
    Node* e = Multiplicative();
    for (;;) {
        int line = Tok().line;
        if (Accept("+")) e = MakeAdd(e, Multiplicative(), line);
        else if (Accept("-")) e = MakeSub(e, Multiplicative(), line);
        else return e;
    }
}

static Node* Shift()
{
    Node* e = Additive();
    for (;;) {
        int line = Tok().line;
        if (Accept("<<")) e = MakeBinary(NodeShl, e, Additive(), line, "<<");
        else if (Accept(">>")) e = MakeBinary(NodeShr, e, Additive(), line, ">>");
        else return e;
    }
}

static Node* Relational()
{
    Node* e = Shift();
    for (;;) {
        int line = Tok().line;
        if (Accept("<")) e = MakeCompare(NodeLt, e, Shift(), line, false);
        else if (Accept("<=")) e = MakeCompare(NodeLe, e, Shift(), line, false);
        else if (Accept(">")) e = MakeCompare(NodeLt, e, Shift(), line, true);
        else if (Accept(">=")) e = MakeCompare(NodeLe, e, Shift(), line, true);
        else return e;
    }
}

static Node* Equality()
{
    Node* e = Relational();
    for (;;) {
        int line = Tok().line;
        if (Accept("==")) e = MakeCompare(NodeEq, e, Relational(), line, false);
        else if (Accept("!=")) e = MakeCompare(NodeNe, e, Relational(), line, false);
        else return e;
    }
}

static Node* BitAnd()
{
    Node* e = Equality();
    while (Is("&")) { int line = Tok().line; pos++; e = MakeBinary(NodeAnd, e, Equality(), line, "&"); }
    return e;
}

static Node* BitXor()
{
    Node* e = BitAnd();
    while (Is("^")) { int line = Tok().line; pos++; e = MakeBinary(NodeXor, e, BitAnd(), line, "^"); }
    return e;
}

static Node* BitOr()
{
    Node* e = BitXor();
    while (Is("|")) { int line = Tok().line; pos++; e = MakeBinary(NodeOr, e, BitXor(), line, "|"); }
    return e;
}

static Node* Logical(NodeKind kind, Node* a, Node* b, int line)
{
    NeedScalar(a, "logical operator");
    NeedScalar(b, "logical operator");
    Node* n = NewNode(kind, line);
    n->a = a;
    n->b = b;
    n->type = TypeInt;
    return n;
}

static Node* LogicalAnd()
{
    Node* e = BitOr();
    while (Is("&&")) { int line = Tok().line; pos++; e = Logical(NodeLogAnd, e, BitOr(), line); }
    return e;
}

static Node* LogicalOr()
{
    Node* e = LogicalAnd();
    while (Is("||")) { int line = Tok().line; pos++; e = Logical(NodeLogOr, e, LogicalAnd(), line); }
    return e;
}

static Node* Conditional()
{
    Node* cond = LogicalOr();
    if (!Is("?")) return cond;
    int line = Tok().line;
    pos++;
    NeedScalar(cond, "?:");
    Node* n = NewNode(NodeCond, line);
    n->a = cond;
    n->b = Expression();
    Expect(":");
    n->c = Conditional();
    Type* tb = Decay(n->b->type);
    Type* tc = Decay(n->c->type);
    if (IsInteger(tb) && IsInteger(tc)) n->type = UsualArithmetic(tb, tc);
    else if (tb->kind == KindPointer) n->type = tb;
    else if (tc->kind == KindPointer) n->type = tc;
    else if (tb->kind == KindStruct && tb == tc) n->type = tb;
    else if (tb->kind == KindVoid && tc->kind == KindVoid) n->type = TypeVoid;
    else Fatal(line, "incompatible operands to ?:");
    return n;
}

static Node* Assignment()
{
    Node* lhs = Conditional();
    int line = Tok().line;
    if (Accept("=")) return MakeAssign(lhs, Assignment(), line);

    static const struct { const char* text; NodeKind op; } compound[] = {
        {"+=", NodeAdd}, {"-=", NodeSub}, {"*=", NodeMul}, {"/=", NodeDiv}, {"%=", NodeMod},
        {"&=", NodeAnd}, {"|=", NodeOr}, {"^=", NodeXor}, {"<<=", NodeShl}, {">>=", NodeShr}};
    for (auto& c : compound) {
        if (!Accept(c.text)) continue;
        Node* rhs = Assignment();
        if (!IsLvalue(lhs)) Fatal(line, "left side of '%s' is not assignable", c.text);
        Node* n = NewNode(NodeCompound, line);
        n->op = c.op;
        n->a = lhs;
        n->type = lhs->type;
        if (lhs->type->kind == KindPointer) {
            if (c.op != NodeAdd && c.op != NodeSub) Fatal(line, "invalid '%s' on a pointer", c.text);
            NeedInteger(rhs, c.text);
            n->b = Scaled(rhs, ElementSize(lhs->type, line));
            n->opUnsigned = true;
        } else {
            NeedInteger(lhs, c.text);
            NeedInteger(rhs, c.text);
            n->b = rhs;
            Type* opType = (c.op == NodeShl || c.op == NodeShr) ? Promote(lhs->type) : UsualArithmetic(lhs->type, rhs->type);
            n->opUnsigned = opType->isUnsigned;
        }
        return n;
    }
    return lhs;
}

static Node* Expression()
{
    Node* e = Assignment();
    while (Is(",")) {
        int line = Tok().line;
        pos++;
        Node* n = NewNode(NodeComma, line);
        n->a = e;
        n->b = Assignment();
        n->type = n->b->type;
        e = n;
    }
    return e;
}

// ===========================================================================
// Constant evaluation (array sizes, case labels, enums, global initializers)

static long long Wrap16(long long v, bool isUnsigned)
{
    v &= 0xFFFF;
    if (!isUnsigned && v >= 0x8000) v -= 0x10000;
    return v;
}

static long long EvaluateAddress(Node* n, string* label)
{
    switch (n->kind) {
    case NodeVar:
        if (n->var->isLocal || !label) Fatal(n->line, "not a constant expression");
        if (!label->empty()) Fatal(n->line, "constant expression refers to two addresses");
        *label = n->var->label;
        return n->value;
    case NodeMember: return EvaluateAddress(n->a, label) + n->member->offset;
    case NodeDeref: return Evaluate(n->a, label);
    default: Fatal(n->line, "not a constant expression");
    }
}

static long long Evaluate(Node* n, string* label)
{
    bool u = n->opUnsigned;
    switch (n->kind) {
    case NodeNumber: return n->value;
    case NodeAdd: { long long a = Evaluate(n->a, label); return Wrap16(a + Evaluate(n->b, label), n->type->isUnsigned); }
    case NodeSub: { long long a = Evaluate(n->a, label); return Wrap16(a - Evaluate(n->b), n->type->isUnsigned); }
    case NodeMul: return Wrap16(Evaluate(n->a) * Evaluate(n->b), u);
    case NodeDiv:
    case NodeMod: {
        long long a = Evaluate(n->a), b = Evaluate(n->b);
        if (u) { a &= 0xFFFF; b &= 0xFFFF; }
        if (b == 0) Fatal(n->line, "division by zero in constant expression");
        return Wrap16(n->kind == NodeDiv ? a / b : a % b, u);
    }
    case NodeAnd: return Wrap16(Evaluate(n->a) & Evaluate(n->b), u);
    case NodeOr: return Wrap16(Evaluate(n->a) | Evaluate(n->b), u);
    case NodeXor: return Wrap16(Evaluate(n->a) ^ Evaluate(n->b), u);
    case NodeShl: return Wrap16(Evaluate(n->a) << (Evaluate(n->b) & 15), u);
    case NodeShr: {
        long long a = Evaluate(n->a);
        if (u) a &= 0xFFFF;
        return Wrap16(a >> (Evaluate(n->b) & 15), u);
    }
    case NodeEq: return Evaluate(n->a) == Evaluate(n->b);
    case NodeNe: return Evaluate(n->a) != Evaluate(n->b);
    case NodeLt:
    case NodeLe: {
        long long a = Evaluate(n->a), b = Evaluate(n->b);
        if (u) { a &= 0xFFFF; b &= 0xFFFF; }
        return n->kind == NodeLt ? a < b : a <= b;
    }
    case NodeLogAnd: return Evaluate(n->a) && Evaluate(n->b);
    case NodeLogOr: return Evaluate(n->a) || Evaluate(n->b);
    case NodeNot: return !Evaluate(n->a);
    case NodeBitNot: return Wrap16(~Evaluate(n->a), n->type->isUnsigned);
    case NodeNeg: return Wrap16(-Evaluate(n->a), n->type->isUnsigned);
    case NodeCond: return Evaluate(n->a) ? Evaluate(n->b, label) : Evaluate(n->c, label);
    case NodeComma: return Evaluate(n->b, label);
    case NodeCast: {
        long long v = Evaluate(n->a, label);
        Type* t = n->type;
        if (t->kind == KindBool) return v != 0;
        if (t->kind == KindChar) return t->isUnsigned ? (v & 0xFF) : (signed char)(v & 0xFF);
        if (label && !label->empty()) return v;
        return Wrap16(v, t->isUnsigned);
    }
    case NodeAddr: return EvaluateAddress(n->a, label);
    case NodeVar:
    case NodeDeref:
    case NodeMember:
        // arrays and functions decay to their (constant) address
        if (n->type->kind == KindArray || n->type->kind == KindFunction) return EvaluateAddress(n, label);
        Fatal(n->line, "not a constant expression");
    default: Fatal(n->line, "not a constant expression");
    }
}

static long long ConstantExpression()
{
    Node* e = Conditional();
    return Evaluate(e);
}

// ===========================================================================
// Initializers

using LeafFunction = std::function<void(Type*, int, Node*)>;

// Parses an initializer for type t at byte offset off. Returns the element count
// for arrays (used to complete "int a[] = {...}").
static int Initializer(Type* t, int off, const LeafFunction& leaf)
{
    int line = Tok().line;
    if (t->kind == KindArray) {
        Type* element = t->base;
        bool stringInit = element->kind == KindChar &&
                          (Tok().kind == TokString || (Is("{") && TokAt(pos + 1).kind == TokString));
        if (stringInit) {
            bool braced = Accept("{");
            string s = Tok().text;
            pos++;
            int count = t->length < 0 ? (int)s.size() + 1 : t->length;
            if ((int)s.size() > count) Fatal(line, "string is too long for the array");
            for (int i = 0; i < count && i <= (int)s.size(); i++)
                leaf(element, off + i, NewNumber(i < (int)s.size() ? (signed char)s[i] : 0, line));
            if (braced) { Accept(","); Expect("}"); }
            return count;
        }
        if (!Accept("{")) Fatal(line, "array initializer needs braces");
        int i = 0;
        while (!Accept("}")) {
            if (Is(".") || Is("[")) Fatal(Tok().line, "designated initializers are not supported");
            if (t->length >= 0 && i >= t->length) Fatal(Tok().line, "too many initializers for the array");
            Initializer(element, off + i * element->size, leaf);
            i++;
            if (!Accept(",")) { Expect("}"); break; }
        }
        return i;
    }
    if (t->kind == KindStruct) {
        if (Accept("{")) {
            size_t i = 0;
            auto& members = t->info->members;
            while (!Accept("}")) {
                if (Is(".") || Is("[")) Fatal(Tok().line, "designated initializers are not supported");
                if (i >= members.size() || (t->info->isUnion && i >= 1)) Fatal(Tok().line, "too many initializers for the struct");
                Initializer(members[i].type, off + members[i].offset, leaf);
                i++;
                if (!Accept(",")) { Expect("}"); break; }
            }
            return 1;
        }
        leaf(t, off, Assignment());
        return 1;
    }
    bool braced = Accept("{");
    Node* e = Assignment();
    if (braced) { Accept(","); Expect("}"); }
    leaf(t, off, e);
    return 1;
}

static void GlobalInitializer(Var* g)
{
    if (g->hasInit) Fatal(Tok().line, "redefinition of '%s'", g->name.c_str());
    g->hasInit = true;
    g->data.clear();
    g->relocations.clear();
    auto leaf = [&](Type* t, int off, Node* e) {
        if (t->kind == KindStruct) Fatal(e->line, "struct copy is not a constant initializer");
        if (!IsScalar(Decay(e->type))) Fatal(e->line, "invalid initializer");
        string label;
        long long v = Evaluate(MakeCast(e, t), &label);
        if ((int)g->data.size() < off + t->size) g->data.resize(off + t->size, 0);
        if (t->size == 1) {
            if (!label.empty()) Fatal(e->line, "an address does not fit in a char");
            g->data[off] = (uint8_t)v;
        } else {
            if (!label.empty()) {
                g->relocations[off] = {label, (int)v};
            } else {
                g->data[off] = (uint8_t)(v & 0xFF);
                g->data[off + 1] = (uint8_t)((v >> 8) & 0xFF);
            }
        }
    };
    int count = Initializer(g->type, 0, leaf);
    if (g->type->kind == KindArray && g->type->length < 0) g->type = ArrayOf(g->type->base, count);
    g->data.resize(g->type->size, 0);
}

// ===========================================================================
// Globals, locals, statements

static Var* DeclareGlobal(const string& name, Type* t, int line)
{
    auto it = globalsByName.find(name);
    if (it != globalsByName.end()) {
        Var* g = it->second;
        if ((g->type->kind == KindFunction) != (t->kind == KindFunction))
            Fatal(line, "'%s' redeclared as a different kind of symbol", name.c_str());
        if (g->type->kind == KindArray && g->type->length < 0 && t->kind == KindArray) g->type = t;
        if (g->type->kind == KindFunction && g->type->oldStyle && !t->oldStyle) g->type = t;
        scopes[0][name].var = g;
        return g;
    }
    Var* g = new Var;
    g->name = name;
    g->label = "_" + name;
    g->type = t;
    globals.push_back(g);
    globalsByName[name] = g;
    Symbol s;
    s.var = g;
    scopes[0][name] = s;
    return g;
}

static Node* LocalDeclaration()
{
    Node* block = NewNode(NodeBlock, Tok().line);
    StorageClass storage;
    Type* base = DeclarationSpecifiers(&storage);
    if (Accept(";")) return block;
    do {
        int line = Tok().line;
        string name;
        Type* t = ParseDeclarator(base, name);
        if (name.empty()) Fatal(line, "declaration needs a name");
        if (storage.isTypedef) {
            Symbol s;
            s.typedefType = t;
            scopes.back()[name] = s;
            continue;
        }
        if (t->kind == KindFunction || storage.isExtern) {
            Var* g = DeclareGlobal(name, t, line);
            Symbol s;
            s.var = g;
            scopes.back()[name] = s;
            continue;
        }
        if (t->kind == KindVoid) Fatal(line, "variable '%s' declared void", name.c_str());
        if (t->kind == KindStruct && !t->info->complete) Fatal(line, "variable '%s' has incomplete type", name.c_str());

        if (storage.isStatic) {
            Var* g = new Var;
            g->name = name;
            g->label = ".Lstatic" + std::to_string(++staticCounter) + "_" + name;
            g->type = t;
            g->isDefined = true;
            globals.push_back(g);
            Symbol s;
            s.var = g;
            scopes.back()[name] = s;
            if (Accept("=")) GlobalInitializer(g);
            if (g->type->kind == KindArray && g->type->length < 0) Fatal(line, "array '%s' needs a size", name.c_str());
            continue;
        }

        Var* v = new Var;
        v->name = name;
        v->type = t;
        v->isLocal = true;
        Symbol s;
        s.var = v;
        scopes.back()[name] = s;

        vector<Node*> stores;
        bool hasInit = Accept("=");
        if (hasInit) {
            auto leaf = [&](Type* lt, int off, Node* e) {
                Node* lhs = NewNode(NodeVar, e->line);
                lhs->var = v;
                lhs->value = off;
                lhs->type = lt;
                Node* stmt = NewNode(NodeExprStmt, e->line);
                stmt->a = MakeAssign(lhs, e, e->line);
                stores.push_back(stmt);
            };
            int count = Initializer(t, 0, leaf);
            if (t->kind == KindArray && t->length < 0) v->type = t = ArrayOf(t->base, count);
        }
        if (t->kind == KindArray && t->length < 0) Fatal(line, "array '%s' needs a size", name.c_str());
        frameSize += t->size;
        if (t->align >= 2) frameSize = AlignUp(frameSize, 2);
        v->offset = -frameSize;

        if (hasInit && (t->kind == KindArray || t->kind == KindStruct) && !(stores.size() == 1 && stores[0]->a->a->type == t)) {
            Node* zero = NewNode(NodeZero, line);
            zero->var = v;
            block->list.push_back(zero);
        }
        for (Node* st : stores) block->list.push_back(st);
    } while (Accept(","));
    Expect(";");
    return block;
}

static Node* Statement()
{
    Token& t = Tok();
    int line = t.line;

    if (Is("{")) return CompoundStatement();

    if (Accept("if")) {
        Node* n = NewNode(NodeIf, line);
        Expect("(");
        n->a = Expression();
        NeedScalar(n->a, "if");
        Expect(")");
        n->b = Statement();
        if (Accept("else")) n->c = Statement();
        return n;
    }
    if (Accept("while")) {
        Node* n = NewNode(NodeWhile, line);
        Expect("(");
        n->a = Expression();
        NeedScalar(n->a, "while");
        Expect(")");
        n->b = Statement();
        return n;
    }
    if (Accept("do")) {
        Node* n = NewNode(NodeDo, line);
        n->b = Statement();
        Expect("while");
        Expect("(");
        n->a = Expression();
        NeedScalar(n->a, "do-while");
        Expect(")");
        Expect(";");
        return n;
    }
    if (Accept("for")) {
        Node* n = NewNode(NodeFor, line);
        Expect("(");
        EnterScope();
        if (IsTypeToken(Tok())) n->a = LocalDeclaration();
        else if (!Accept(";")) {
            n->a = NewNode(NodeExprStmt, line);
            n->a->a = Expression();
            Expect(";");
        }
        if (!Is(";")) { n->b = Expression(); NeedScalar(n->b, "for"); }
        Expect(";");
        if (!Is(")")) n->c = Expression();
        Expect(")");
        n->d = Statement();
        LeaveScope();
        return n;
    }
    if (Accept("return")) {
        Node* n = NewNode(NodeReturn, line);
        if (!Accept(";")) {
            Node* e = Expression();
            Expect(";");
            if (currentReturnType->kind == KindVoid) {
                n->a = e;   // allowed if e is void; value ignored
            } else {
                if (e->type->kind == KindStruct || currentReturnType->kind == KindStruct)
                    Fatal(line, "returning structs by value is not supported");
                NeedScalar(e, "return");
                n->a = MakeCast(e, currentReturnType);
            }
        }
        return n;
    }
    if (Accept("break")) { Expect(";"); return NewNode(NodeBreak, line); }
    if (Accept("continue")) { Expect(";"); return NewNode(NodeContinue, line); }
    if (Accept("goto")) {
        Node* n = NewNode(NodeGoto, line);
        n->name = ExpectIdent();
        Expect(";");
        return n;
    }
    if (Accept("switch")) {
        Node* n = NewNode(NodeSwitch, line);
        Expect("(");
        n->a = Expression();
        NeedInteger(n->a, "switch");
        Expect(")");
        switchStack.push_back(n);
        n->b = Statement();
        switchStack.pop_back();
        return n;
    }
    if (Accept("case")) {
        if (switchStack.empty()) Fatal(line, "'case' outside of a switch");
        Node* n = NewNode(NodeCase, line);
        n->value = ConstantExpression();
        Expect(":");
        n->label = ++labelCounter;
        for (Node* other : switchStack.back()->list)
            if (Wrap16(other->value, false) == Wrap16(n->value, false)) Fatal(line, "duplicate case value %lld", n->value);
        switchStack.back()->list.push_back(n);
        n->a = Is("}") ? NewNode(NodeBlock, line) : Statement();
        return n;
    }
    if (Accept("default")) {
        if (switchStack.empty()) Fatal(line, "'default' outside of a switch");
        if (switchStack.back()->d) Fatal(line, "multiple default labels");
        Expect(":");
        Node* n = NewNode(NodeCase, line);
        n->label = ++labelCounter;
        switchStack.back()->d = n;
        n->a = Is("}") ? NewNode(NodeBlock, line) : Statement();
        return n;
    }
    if (t.kind == TokIdent && IsText(TokAt(pos + 1), ":")) {
        Node* n = NewNode(NodeLabel, line);
        n->name = t.text;
        pos += 2;
        n->a = Is("}") ? NewNode(NodeBlock, line) : Statement();
        return n;
    }
    if (Accept(";")) return NewNode(NodeBlock, line);

    Node* n = NewNode(NodeExprStmt, line);
    n->a = Expression();
    Expect(";");
    return n;
}

static Node* CompoundStatement()
{
    Node* n = NewNode(NodeBlock, Tok().line);
    Expect("{");
    EnterScope();
    while (!Accept("}")) {
        if (Tok().kind == TokEof) Fatal(Tok().line, "missing '}'");
        bool isLabel = Tok().kind == TokIdent && IsText(TokAt(pos + 1), ":");
        if (IsTypeToken(Tok()) && !isLabel) n->list.push_back(LocalDeclaration());
        else n->list.push_back(Statement());
    }
    LeaveScope();
    return n;
}

// ===========================================================================
// Code generation

static string code;
static bool useMultiply, useDivide, useMemcpy, useMemzero;
static vector<int> breakLabels, continueLabels;
static int functionId;

static void Emit(const char* format, ...)
{
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof buffer, format, args);
    va_end(args);
    code += "    ";
    code += buffer;
    code += "\n";
}

static void EmitLabel(const string& name) { code += name + ":\n"; }
static string LabelName(int n) { return ".L" + std::to_string(n); }
static int NewLabel() { return ++labelCounter; }
static bool Fits6(long long v) { return v >= -32 && v <= 31; }

static string MemoryOperand(const char* reg, int off)
{
    char buffer[32];
    if (off == 0) snprintf(buffer, sizeof buffer, "[%s]", reg);
    else snprintf(buffer, sizeof buffer, "[%s%+d]", reg, off);
    return buffer;
}

// rd = ra + v  (uses r3 as scratch for big constants; rd/ra must not be r3)
static void AddConstant(const char* rd, const char* ra, long long v)
{
    if (v == 0) {
        if (strcmp(rd, ra)) Emit("mov %s, %s", rd, ra);
        return;
    }
    if (Fits6(v)) {
        Emit("addi %s, %s, %lld", rd, ra, v);
        return;
    }
    Emit("li r3, %lld", v & 0xFFFF);
    Emit("add %s, %s, r3", rd, ra);
}

static void CallRuntime(const char* name)
{
    Emit("la r4, %s", name);
    Emit("jalr r4");
}

// r0 = value at [reg+off] (off must fit in 6 bits). Aggregates yield their address.
static void EmitLoad(Type* t, const char* reg, int off)
{
    switch (t->kind) {
    case KindArray:
    case KindFunction:
    case KindStruct:
        AddConstant("r0", reg, off);
        return;
    case KindChar:
    case KindBool:
        Emit("lb r0, %s", MemoryOperand(reg, off).c_str());
        if (t->isUnsigned) { Emit("li r3, 255"); Emit("and r0, r0, r3"); }
        else Emit("sxb r0, r0");
        return;
    default:
        Emit("lw r0, %s", MemoryOperand(reg, off).c_str());
    }
}

// [reg+off] = r0
static void EmitStore(Type* t, const char* reg, int off)
{
    if (t->kind == KindChar || t->kind == KindBool) Emit("sb r0, %s", MemoryOperand(reg, off).c_str());
    else Emit("sw r0, %s", MemoryOperand(reg, off).c_str());
}

static void EmitCast(Type* from, Type* to)
{
    if (to->kind == KindVoid) return;
    if (to->kind == KindBool) {
        if (from->kind == KindBool) return;
        int done = NewLabel();
        Emit("cmpi r0, 0");
        Emit("beq %s", LabelName(done).c_str());
        Emit("li r0, 1");
        EmitLabel(LabelName(done));
        return;
    }
    if (to->kind == KindChar) {
        if (from->kind == KindChar && from->isUnsigned == to->isUnsigned) return;
        if (from->kind == KindBool) return;
        if (to->isUnsigned) { Emit("li r3, 255"); Emit("and r0, r0, r3"); }
        else Emit("sxb r0, r0");
    }
}

static void BranchIfFalse(int label)
{
    int skip = NewLabel();
    Emit("cmpi r0, 0");
    Emit("bne %s", LabelName(skip).c_str());
    Emit("j %s", LabelName(label).c_str());
    EmitLabel(LabelName(skip));
}

static void BranchIfTrue(int label)
{
    int skip = NewLabel();
    Emit("cmpi r0, 0");
    Emit("beq %s", LabelName(skip).c_str());
    Emit("j %s", LabelName(label).c_str());
    EmitLabel(LabelName(skip));
}

static void GenerateExpression(Node* n);

static bool IsLocalDirect(Node* n, int* off)
{
    if (n->kind != NodeVar || !n->var->isLocal) return false;
    *off = n->var->offset + (int)n->value;
    return Fits6(*off);
}

// r0 = address of the lvalue n
static void GenerateAddress(Node* n)
{
    switch (n->kind) {
    case NodeVar:
        if (n->var->isLocal) AddConstant("r0", "r5", n->var->offset + n->value);
        else if (n->value) Emit("la r0, %s+%lld", n->var->label.c_str(), n->value);
        else Emit("la r0, %s", n->var->label.c_str());
        return;
    case NodeDeref:
        GenerateExpression(n->a);
        return;
    case NodeMember:
        GenerateAddress(n->a);
        AddConstant("r0", "r0", n->member->offset);
        return;
    default:
        if (n->type->kind == KindStruct) { GenerateExpression(n); return; }
        Fatal(n->line, "expression is not an lvalue");
    }
}

static bool IsPowerOfTwo(long long v, int* shift)
{
    if (v <= 0 || (v & (v - 1))) return false;
    *shift = 0;
    while ((1LL << *shift) != v) (*shift)++;
    return true;
}

// r0 = r0 op r1
static void ApplyOperator(NodeKind op, bool isUnsigned, Node* constantRight)
{
    int shift;
    switch (op) {
    case NodeAdd: Emit("add r0, r0, r1"); return;
    case NodeSub: Emit("sub r0, r0, r1"); return;
    case NodeAnd: Emit("and r0, r0, r1"); return;
    case NodeOr:  Emit("or r0, r0, r1"); return;
    case NodeXor: Emit("xor r0, r0, r1"); return;
    case NodeShl: Emit("shl r0, r0, r1"); return;
    case NodeShr: Emit(isUnsigned ? "shr r0, r0, r1" : "sar r0, r0, r1"); return;
    case NodeMul:
        if (constantRight && IsPowerOfTwo(constantRight->value, &shift)) {
            if (shift) { Emit("li r1, %d", shift); Emit("shl r0, r0, r1"); }
            return;
        }
        useMultiply = true;
        CallRuntime(".rt_mul");
        return;
    case NodeDiv:
    case NodeMod:
        if (isUnsigned && constantRight && IsPowerOfTwo(constantRight->value, &shift)) {
            if (op == NodeDiv) { if (shift) { Emit("li r1, %d", shift); Emit("shr r0, r0, r1"); } }
            else { Emit("li r1, %lld", constantRight->value - 1); Emit("and r0, r0, r1"); }
            return;
        }
        useDivide = true;
        CallRuntime(isUnsigned ? ".rt_divu" : ".rt_divs");
        if (op == NodeMod) Emit("mov r0, r1");
        return;
    case NodeEq:
    case NodeNe:
    case NodeLt:
    case NodeLe: {
        int yes = NewLabel(), done = NewLabel();
        Emit("cmp r0, r1");
        string y = LabelName(yes);
        if (op == NodeEq) Emit("beq %s", y.c_str());
        else if (op == NodeNe) Emit("bne %s", y.c_str());
        else if (op == NodeLt) Emit("%s %s", isUnsigned ? "bltu" : "blt", y.c_str());
        else if (isUnsigned) { Emit("bltu %s", y.c_str()); Emit("beq %s", y.c_str()); }
        else Emit("ble %s", y.c_str());
        Emit("li r0, 0");
        Emit("bra %s", LabelName(done).c_str());
        EmitLabel(y);
        Emit("li r0, 1");
        EmitLabel(LabelName(done));
        return;
    }
    default:
        Fatal(0, "internal error: bad operator");
    }
}

static void MemoryCopy(int size)
{
    // r1 = destination, r0 = source; result r0 = destination
    useMemcpy = true;
    Emit("li r2, %d", size);
    CallRuntime(".rt_memcpy");
}

static void GenerateExpression(Node* n)
{
    int off;
    switch (n->kind) {
    case NodeNumber:
        Emit("li r0, %lld", n->value & 0xFFFF);
        return;

    case NodeVar:
        if (IsLocalDirect(n, &off) && IsScalar(n->type)) { EmitLoad(n->type, "r5", off); return; }
        GenerateAddress(n);
        EmitLoad(n->type, "r0", 0);
        return;

    case NodeMember:
    case NodeDeref:
        GenerateAddress(n);
        EmitLoad(n->type, "r0", 0);
        return;

    case NodeAddr:
        GenerateAddress(n->a);
        return;

    case NodeCast:
        GenerateExpression(n->a);
        EmitCast(n->a->type, n->type);
        return;

    case NodeNeg:
        GenerateExpression(n->a);
        Emit("neg r0, r0");
        return;

    case NodeBitNot:
        GenerateExpression(n->a);
        Emit("not r0, r0");
        return;

    case NodeNot: {
        int yes = NewLabel(), done = NewLabel();
        GenerateExpression(n->a);
        Emit("cmpi r0, 0");
        Emit("beq %s", LabelName(yes).c_str());
        Emit("li r0, 0");
        Emit("bra %s", LabelName(done).c_str());
        EmitLabel(LabelName(yes));
        Emit("li r0, 1");
        EmitLabel(LabelName(done));
        return;
    }

    case NodeLogAnd: {
        int no = NewLabel(), done = NewLabel();
        GenerateExpression(n->a);
        BranchIfFalse(no);
        GenerateExpression(n->b);
        BranchIfFalse(no);
        Emit("li r0, 1");
        Emit("j %s", LabelName(done).c_str());
        EmitLabel(LabelName(no));
        Emit("li r0, 0");
        EmitLabel(LabelName(done));
        return;
    }

    case NodeLogOr: {
        int yes = NewLabel(), done = NewLabel();
        GenerateExpression(n->a);
        BranchIfTrue(yes);
        GenerateExpression(n->b);
        BranchIfTrue(yes);
        Emit("li r0, 0");
        Emit("j %s", LabelName(done).c_str());
        EmitLabel(LabelName(yes));
        Emit("li r0, 1");
        EmitLabel(LabelName(done));
        return;
    }

    case NodeCond: {
        int otherwise = NewLabel(), done = NewLabel();
        GenerateExpression(n->a);
        BranchIfFalse(otherwise);
        GenerateExpression(n->b);
        Emit("j %s", LabelName(done).c_str());
        EmitLabel(LabelName(otherwise));
        GenerateExpression(n->c);
        EmitLabel(LabelName(done));
        return;
    }

    case NodeComma:
        GenerateExpression(n->a);
        GenerateExpression(n->b);
        return;

    case NodeAdd: case NodeSub: case NodeMul: case NodeDiv: case NodeMod:
    case NodeAnd: case NodeOr: case NodeXor: case NodeShl: case NodeShr:
    case NodeEq: case NodeNe: case NodeLt: case NodeLe: {
        GenerateExpression(n->a);
        Node* constant = n->b->kind == NodeNumber ? n->b : nullptr;
        if (constant) {
            Emit("li r1, %lld", constant->value & 0xFFFF);
        } else {
            Emit("push r0");
            GenerateExpression(n->b);
            Emit("mov r1, r0");
            Emit("pop r0");
        }
        ApplyOperator(n->kind, n->opUnsigned, constant);
        return;
    }

    case NodePtrDiff: {
        GenerateExpression(n->a);
        Emit("push r0");
        GenerateExpression(n->b);
        Emit("mov r1, r0");
        Emit("pop r0");
        Emit("sub r0, r0, r1");
        int shift;
        if (IsPowerOfTwo(n->value, &shift)) {
            if (shift) { Emit("li r1, %d", shift); Emit("sar r0, r0, r1"); }
        } else {
            useDivide = true;
            Emit("li r1, %lld", n->value);
            CallRuntime(".rt_divs");
        }
        return;
    }

    case NodeAssign:
        if (n->type->kind == KindStruct) {
            GenerateAddress(n->a);
            Emit("push r0");
            GenerateAddress(n->b);
            Emit("pop r1");
            MemoryCopy(n->type->size);
            return;
        }
        if (IsLocalDirect(n->a, &off)) {
            GenerateExpression(n->b);
            EmitCast(n->b->type, n->a->type);
            EmitStore(n->a->type, "r5", off);
            return;
        }
        GenerateAddress(n->a);
        Emit("push r0");
        GenerateExpression(n->b);
        EmitCast(n->b->type, n->a->type);
        Emit("pop r1");
        EmitStore(n->a->type, "r1", 0);
        return;

    case NodeCompound:
        GenerateAddress(n->a);
        Emit("push r0");
        EmitLoad(n->a->type, "r0", 0);
        Emit("push r0");
        GenerateExpression(n->b);
        Emit("mov r1, r0");
        Emit("pop r0");
        ApplyOperator(n->op, n->opUnsigned, n->b->kind == NodeNumber ? n->b : nullptr);
        EmitCast(TypeInt, n->a->type);
        Emit("pop r1");
        EmitStore(n->a->type, "r1", 0);
        return;

    case NodeIncDec:
        GenerateAddress(n->a);
        Emit("mov r1, r0");
        EmitLoad(n->a->type, "r1", 0);
        if (n->isPost) Emit("mov r2, r0");
        AddConstant("r0", "r0", n->value);
        EmitCast(TypeInt, n->a->type);
        EmitStore(n->a->type, "r1", 0);
        if (n->isPost) Emit("mov r0, r2");
        return;

    case NodeCall: {
        int count = (int)n->list.size();
        for (int i = count - 1; i >= 0; i--) {
            GenerateExpression(n->list[i]);
            Emit("push r0");
        }
        Node* fn = n->a;
        if (fn->kind == NodeVar && fn->var->type->kind == KindFunction) {
            Emit("la r4, %s", fn->var->label.c_str());
        } else {
            GenerateExpression(fn);
            Emit("mov r4, r0");
        }
        Emit("jalr r4");
        if (count) AddConstant("sp", "sp", 2 * count);
        return;
    }

    case NodeZero:
        useMemzero = true;
        AddConstant("r0", "r5", n->var->offset);
        Emit("li r1, %d", n->var->type->size);
        CallRuntime(".rt_memzero");
        return;

    default:
        Fatal(n->line, "internal error: unexpected node in expression");
    }
}

static void GenerateStatement(Node* n)
{
    switch (n->kind) {
    case NodeBlock:
        for (Node* s : n->list) GenerateStatement(s);
        return;

    case NodeExprStmt:
    case NodeZero:
        GenerateExpression(n->kind == NodeZero ? n : n->a);
        return;

    case NodeIf: {
        int otherwise = NewLabel();
        GenerateExpression(n->a);
        BranchIfFalse(otherwise);
        GenerateStatement(n->b);
        if (n->c) {
            int done = NewLabel();
            Emit("j %s", LabelName(done).c_str());
            EmitLabel(LabelName(otherwise));
            GenerateStatement(n->c);
            EmitLabel(LabelName(done));
        } else {
            EmitLabel(LabelName(otherwise));
        }
        return;
    }

    case NodeWhile: {
        int top = NewLabel(), done = NewLabel();
        breakLabels.push_back(done);
        continueLabels.push_back(top);
        EmitLabel(LabelName(top));
        GenerateExpression(n->a);
        BranchIfFalse(done);
        GenerateStatement(n->b);
        Emit("j %s", LabelName(top).c_str());
        EmitLabel(LabelName(done));
        breakLabels.pop_back();
        continueLabels.pop_back();
        return;
    }

    case NodeDo: {
        int top = NewLabel(), next = NewLabel(), done = NewLabel();
        breakLabels.push_back(done);
        continueLabels.push_back(next);
        EmitLabel(LabelName(top));
        GenerateStatement(n->b);
        EmitLabel(LabelName(next));
        GenerateExpression(n->a);
        BranchIfTrue(top);
        EmitLabel(LabelName(done));
        breakLabels.pop_back();
        continueLabels.pop_back();
        return;
    }

    case NodeFor: {
        int top = NewLabel(), next = NewLabel(), done = NewLabel();
        if (n->a) GenerateStatement(n->a);
        breakLabels.push_back(done);
        continueLabels.push_back(next);
        EmitLabel(LabelName(top));
        if (n->b) {
            GenerateExpression(n->b);
            BranchIfFalse(done);
        }
        GenerateStatement(n->d);
        EmitLabel(LabelName(next));
        if (n->c) GenerateExpression(n->c);
        Emit("j %s", LabelName(top).c_str());
        EmitLabel(LabelName(done));
        breakLabels.pop_back();
        continueLabels.pop_back();
        return;
    }

    case NodeReturn:
        if (n->a) GenerateExpression(n->a);
        Emit("j .Lret%d", functionId);
        return;

    case NodeBreak:
        if (breakLabels.empty()) Fatal(n->line, "'break' outside of a loop or switch");
        Emit("j %s", LabelName(breakLabels.back()).c_str());
        return;

    case NodeContinue:
        if (continueLabels.empty()) Fatal(n->line, "'continue' outside of a loop");
        Emit("j %s", LabelName(continueLabels.back()).c_str());
        return;

    case NodeSwitch: {
        int done = NewLabel();
        GenerateExpression(n->a);
        for (Node* c : n->list) {
            int next = NewLabel();
            Emit("li r1, %lld", c->value & 0xFFFF);
            Emit("cmp r0, r1");
            Emit("bne %s", LabelName(next).c_str());
            Emit("j %s", LabelName(c->label).c_str());
            EmitLabel(LabelName(next));
        }
        Emit("j %s", LabelName(n->d ? n->d->label : done).c_str());
        breakLabels.push_back(done);
        GenerateStatement(n->b);
        breakLabels.pop_back();
        EmitLabel(LabelName(done));
        return;
    }

    case NodeCase:
        EmitLabel(LabelName(n->label));
        GenerateStatement(n->a);
        return;

    case NodeGoto:
        Emit("j .Lgoto%d_%s", functionId, n->name.c_str());
        return;

    case NodeLabel:
        EmitLabel(".Lgoto" + std::to_string(functionId) + "_" + n->name);
        GenerateStatement(n->a);
        return;

    default:
        Fatal(n->line, "internal error: unexpected statement");
    }
}

static void CheckGotos(Node* n, std::set<string>& defined, vector<Node*>& used)
{
    if (!n) return;
    if (n->kind == NodeLabel) {
        if (!defined.insert(n->name).second) Fatal(n->line, "duplicate label '%s'", n->name.c_str());
    }
    if (n->kind == NodeGoto) used.push_back(n);
    if (n->kind >= NodeBlock) {
        CheckGotos(n->a, defined, used);
        CheckGotos(n->b, defined, used);
        CheckGotos(n->c, defined, used);
        CheckGotos(n->d, defined, used);
        if (n->kind == NodeBlock) for (Node* s : n->list) CheckGotos(s, defined, used);
    }
}

static void FunctionDefinition(Var* f, Type* t, int line)
{
    if (f->isDefined) Fatal(line, "redefinition of function '%s'", f->name.c_str());
    f->isDefined = true;
    f->type = t;
    if (t->base->kind == KindStruct) Fatal(line, "returning structs by value is not supported");
    if (t->variadic && t->params.empty()) Fatal(line, "a variadic function needs at least one named parameter");

    frameSize = 0;
    currentReturnType = t->base;
    EnterScope();
    for (size_t i = 0; i < t->params.size(); i++) {
        if (t->paramNames[i].empty()) Fatal(line, "parameter %d of '%s' has no name", (int)i + 1, f->name.c_str());
        if (t->params[i]->kind == KindStruct) Fatal(line, "passing structs by value is not supported (pass a pointer)");
        Var* p = new Var;
        p->name = t->paramNames[i];
        p->type = t->params[i];
        p->isLocal = true;
        p->offset = 4 + 2 * (int)i;
        Symbol s;
        s.var = p;
        scopes.back()[p->name] = s;
    }
    Node* body = CompoundStatement();
    LeaveScope();

    std::set<string> definedLabels;
    vector<Node*> gotos;
    CheckGotos(body, definedLabels, gotos);
    for (Node* g : gotos)
        if (!definedLabels.count(g->name)) Fatal(g->line, "goto to undefined label '%s'", g->name.c_str());

    functionId++;
    code += "\n; ---- " + f->name + "\n";
    EmitLabel(f->label);
    Emit("push lr");
    Emit("push r5");
    Emit("mov r5, sp");
    Emit("li r2, .Lframe%d", functionId);
    Emit("sub sp, sp, r2");
    GenerateStatement(body);
    if (f->name == "main") Emit("li r0, 0");
    EmitLabel(".Lret" + std::to_string(functionId));
    Emit("mov sp, r5");
    Emit("pop r5");
    Emit("pop lr");
    Emit("ret");
    code += ".Lframe" + std::to_string(functionId) + " = " + std::to_string(AlignUp(frameSize, 2)) + "\n";
}

static void TopLevel()
{
    StorageClass storage;
    Type* base = DeclarationSpecifiers(&storage);
    if (Accept(";")) return;
    bool first = true;
    for (;;) {
        int line = Tok().line;
        string name;
        Type* t = ParseDeclarator(base, name);
        if (name.empty()) Fatal(line, "declaration needs a name");
        if (storage.isTypedef) {
            Symbol s;
            s.typedefType = t;
            scopes[0][name] = s;
        } else if (t->kind == KindFunction) {
            Var* f = DeclareGlobal(name, t, line);
            if (first && Is("{")) { FunctionDefinition(f, t, line); return; }
        } else {
            if (t->kind == KindVoid) Fatal(line, "variable '%s' declared void", name.c_str());
            Var* g = DeclareGlobal(name, t, line);
            if (!storage.isExtern) {
                if (t->kind == KindStruct && !t->info->complete) Fatal(line, "variable '%s' has incomplete type", name.c_str());
                g->isDefined = true;
            }
            if (Accept("=")) { g->isDefined = true; GlobalInitializer(g); }
        }
        first = false;
        if (Accept(",")) continue;
        Expect(";");
        return;
    }
}

// ===========================================================================
// Runtime support (appended only when used)

static const char* RuntimeMultiply = R"(
; r0 = r0 * r1   (clobbers r1-r3)
.rt_mul:
    mov r2, r0
    li r0, 0
.rt_mul_loop:
    cmpi r1, 0
    beq .rt_mul_done
    li r3, 1
    and r3, r1, r3
    cmpi r3, 0
    beq .rt_mul_skip
    add r0, r0, r2
.rt_mul_skip:
    add r2, r2, r2
    li r3, 1
    shr r1, r1, r3
    bra .rt_mul_loop
.rt_mul_done:
    ret
)";

static const char* RuntimeDivide = R"(
; unsigned: r0 = r0 / r1, r1 = r0 % r1   (clobbers r2, r3)
.rt_divu:
    li r2, 0
    li r3, 16
.rt_divu_loop:
    add r2, r2, r2
    cmpi r0, 0
    bpl .rt_divu_nobit
    addi r2, r2, 1
.rt_divu_nobit:
    add r0, r0, r0
    cmp r2, r1
    bltu .rt_divu_next
    sub r2, r2, r1
    addi r0, r0, 1
.rt_divu_next:
    addi r3, r3, -1
    cmpi r3, 0
    bne .rt_divu_loop
    mov r1, r2
    ret

; signed: r0 = r0 / r1, r1 = r0 % r1 (truncating, C semantics)
.rt_divs:
    push lr
    push r0
    xor r2, r0, r1
    push r2
    cmpi r0, 0
    bge .rt_divs_a
    neg r0, r0
.rt_divs_a:
    cmpi r1, 0
    bge .rt_divs_b
    neg r1, r1
.rt_divs_b:
    call .rt_divu
    pop r2
    cmpi r2, 0
    bge .rt_divs_c
    neg r0, r0
.rt_divs_c:
    pop r2
    cmpi r2, 0
    bge .rt_divs_d
    neg r1, r1
.rt_divs_d:
    pop lr
    ret
)";

static const char* RuntimeMemcpy = R"(
; copy r2 bytes from [r0] to [r1]; returns r0 = destination
.rt_memcpy:
    push r1
.rt_memcpy_loop:
    cmpi r2, 0
    beq .rt_memcpy_done
    lb r3, [r0]
    sb r3, [r1]
    addi r0, r0, 1
    addi r1, r1, 1
    addi r2, r2, -1
    bra .rt_memcpy_loop
.rt_memcpy_done:
    pop r0
    ret
)";

static const char* RuntimeMemzero = R"(
; zero r1 bytes at [r0]
.rt_memzero:
    li r3, 0
.rt_memzero_loop:
    cmpi r1, 0
    beq .rt_memzero_done
    sb r3, [r0]
    addi r0, r0, 1
    addi r1, r1, -1
    bra .rt_memzero_loop
.rt_memzero_done:
    ret
)";

// ===========================================================================
// Data section

static string DataSection()
{
    string out = "\n; ---- data\n";
    for (Var* g : globals) {
        if (g->type->kind == KindFunction || !g->isDefined) continue;
        int size = g->type->size;
        if (g->type->kind == KindArray && g->type->length < 0) {
            fprintf(stderr, "%s: warning: array '%s' has no size; assuming one element\n", sourceName.c_str(), g->name.c_str());
            size = g->type->base->size;
        }
        if (size == 0) size = 1;
        if (g->type->align >= 2) out += "    .align 2\n";
        out += g->label + ":\n";
        if (!g->hasInit) {
            out += "    .space " + std::to_string(size) + "\n";
            continue;
        }
        g->data.resize(size, 0);
        string bytes;
        int count = 0;
        auto flush = [&]() {
            if (count) out += "    .byte " + bytes + "\n";
            bytes.clear();
            count = 0;
        };
        for (int i = 0; i < size;) {
            auto it = g->relocations.find(i);
            if (it != g->relocations.end()) {
                flush();
                out += "    .word " + it->second.first;
                if (it->second.second) out += (it->second.second > 0 ? "+" : "") + std::to_string(it->second.second);
                out += "\n";
                i += 2;
                continue;
            }
            if (count) bytes += ", ";
            bytes += std::to_string(g->data[i]);
            if (++count == 16) flush();
            i++;
        }
        flush();
    }
    return out;
}

// ===========================================================================

static void Usage(const char* program)
{
    fprintf(stderr, "usage: %s [-o output.s] source.c\n", program);
}

int main(int argc, char** argv)
{
    string outputPath;
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if (arg == "-o" && i + 1 < argc) outputPath = argv[++i];
        else if (arg[0] == '-' || !sourceName.empty()) { Usage(argv[0]); return 2; }
        else sourceName = arg;
    }
    if (sourceName.empty()) { Usage(argv[0]); return 2; }
    if (outputPath.empty()) {
        size_t dot = sourceName.rfind('.'), slash = sourceName.rfind('/');
        string stem = (dot != string::npos && (slash == string::npos || dot > slash)) ? sourceName.substr(0, dot) : sourceName;
        outputPath = stem + ".s";
    }

    std::ifstream in(sourceName, std::ios::binary);
    if (!in) { perror(sourceName.c_str()); return 1; }
    std::stringstream buffer;
    buffer << in.rdbuf();

    tokens = Lex(buffer.str());
    pos = 0;
    EnterScope();
    while (Tok().kind != TokEof) TopLevel();

    auto mainIt = globalsByName.find("main");
    if (mainIt == globalsByName.end() || mainIt->second->type->kind != KindFunction || !mainIt->second->isDefined)
        Fatal(Tok().line, "no definition of 'main'");
    for (Var* g : globals)
        if (g->type->kind == KindFunction && !g->isDefined)
            fprintf(stderr, "%s: warning: function '%s' is declared but never defined\n", sourceName.c_str(), g->name.c_str());

    string out;
    out += "; generated by isa16cc from " + sourceName + "\n";
    out += "\n_start:\n    li sp, 0xFF00\n    la r4, _main\n    jalr r4\n    halt\n";
    out += code;
    if (useMultiply) out += RuntimeMultiply;
    if (useDivide) out += RuntimeDivide;
    if (useMemcpy) out += RuntimeMemcpy;
    if (useMemzero) out += RuntimeMemzero;
    out += DataSection();

    std::ofstream file(outputPath, std::ios::binary);
    if (!file) { perror(outputPath.c_str()); return 1; }
    file << out;
    return 0;
}