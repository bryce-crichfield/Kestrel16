// CompilerInternal.h -- what the compiler's parts share. Not for use outside Source/Compiler;
// Compiler.h is the compiler's interface.
//
// The compiler runs in stages, one file each:
//   Preprocessor.cpp   text -> text, with # <line> "<file>" markers
//   Lexer.cpp          text -> tokens
//   Parser.cpp         tokens -> declarations and statements (AST), function by function
//   Expressions.cpp    the expressions, typed as they are built, and constant evaluation
//   CodeGenerator.cpp  each function's AST -> assembly, as soon as it is parsed
//   Program.cpp        the whole program: start-up code, functions, runtime routines, data
//   Compiler.cpp       Compile() and Preprocess(): resets everything and runs the stages
// Their state is file-level, so a compile is not thread-safe; Compile() resets it first.

#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Kestrel::Compiler {

using std::string;
using std::vector;

// ---------------------------------------------------------------------------
// Errors (Compiler.cpp)

struct CompileError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

extern string sourceName;
extern vector<string> warnings;

// Throws a CompileError for a location (see Location()).
[[noreturn]] void Fatal(int location, const char* format, ...);

// ---------------------------------------------------------------------------
// Preprocessor (Preprocessor.cpp)

// A location is one int: the file's index in fileNames times LocationLines, plus the line.
constexpr int LocationLines = 1 << 20;
extern vector<string> fileNames;  // every file read, for error locations
int Location(const string& file, int line);

struct Macro {
    bool function = false;
    vector<string> params;
    vector<string> body;  // pp-tokens, whitespace included
};

extern vector<string> includePaths;
extern std::map<string, Macro> macros;

vector<string> PPTokens(const string& s);
void Preprocess(const string& file, const string& text, string& out, int depth);
void ResetPreprocessor();

// ---------------------------------------------------------------------------
// Lexer (Lexer.cpp)

enum TokenKind { TokIdent, TokKeyword, TokNumber, TokString, TokPunct, TokEof };

struct Token {
    TokenKind kind;
    string text;  // identifier / keyword / punctuator, or string bytes
    long long value = 0;
    bool isUnsigned = false;
    int line = 0;
};

vector<Token> Lex(const string& src);

// ---------------------------------------------------------------------------
// Types and the AST (Types.cpp)

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
    Type* base = nullptr;  // pointee, element, or return type
    int length = 0;        // array length, -1 = incomplete
    vector<Type*> params;
    vector<string> paramNames;
    bool variadic = false;
    bool oldStyle = false;  // "int f()" -- arguments not checked
    StructInfo* info = nullptr;
};

extern Type* TypeVoid;
extern Type* TypeBool;
extern Type* TypeChar;
extern Type* TypeUChar;
extern Type* TypeInt;
extern Type* TypeUInt;

Type* MakeType(Kind kind, int size, int align, bool isUnsigned = false);
Type* PointerTo(Type* base);
Type* ArrayOf(Type* base, int length);
bool IsInteger(Type* t);
bool IsScalar(Type* t);
int AlignUp(int n, int a);
Type* Decay(Type* t);
Type* Promote(Type* t);
Type* UsualArithmetic(Type* a, Type* b);

struct Var {
    string name;
    string label;  // assembler label for globals
    Type* type = nullptr;
    bool isLocal = false;
    int offset = 0;          // frame offset for locals
    bool isDefined = false;  // has a non-extern definition (or a body)
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
    long long value = 0;       // number, var byte offset, inc delta, case value, ptrdiff size
    Var* var = nullptr;
    Member* member = nullptr;
    NodeKind op = NodeNumber;  // compound assignment operator
    bool opUnsigned = false;   // unsigned compare / divide / shift
    bool isPost = false;
    string name;
    int line = 0;
    int label = 0;
};

Node* NewNode(NodeKind kind, int line);
Node* NewNumber(long long value, int line, Type* type = TypeInt);

// ---------------------------------------------------------------------------
// Parser (Parser.cpp)

struct Symbol {
    Var* var = nullptr;
    Type* typedefType = nullptr;
    bool isEnumConstant = false;
    long long enumValue = 0;
};

extern vector<Token> tokens;
extern size_t pos;
extern vector<Var*> globals;
extern std::map<string, Var*> globalsByName;
extern int labelCounter;   // .L labels: case labels here, the rest in the code generator
extern int stringCounter;

Token& Tok();
Token& TokAt(size_t i);
bool IsText(const Token& t, const char* s);
bool Is(const char* s);
bool Accept(const char* s);
void Expect(const char* s);
string ExpectIdent();
Symbol* FindSymbol(const string& name);
bool IsTypeToken(const Token& t);
Type* TypeName();
void EnterScope();
void TopLevel();
void ResetParser();

// ---------------------------------------------------------------------------
// Expressions (Expressions.cpp)

Node* Expression();
Node* Assignment();
Node* Conditional();
long long ConstantExpression();
long long Evaluate(Node* n, string* label = nullptr);
long long Wrap16(long long v, bool isUnsigned);
Node* MakeCast(Node* e, Type* t);
Node* MakeAssign(Node* lhs, Node* rhs, int line);
void NeedScalar(Node* n, const char* what);
void NeedInteger(Node* n, const char* what);

// ---------------------------------------------------------------------------
// Code generation (CodeGenerator.cpp)

extern string code;
extern bool useMultiply, useDivide, useMemcpy, useMemzero;

void GenerateFunction(Var* f, Node* body, int frameSize);
void ResetCodeGenerator();

// ---------------------------------------------------------------------------
// The program (Program.cpp)

string ProgramText();  // everything, once every function has been generated

}  // namespace Kestrel::Compiler
