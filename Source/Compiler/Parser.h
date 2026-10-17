// Parser.h -- the parser (Parser.cpp): tokens to declarations and statements, function by function.
// Internal to Source/Compiler; Compiler.h is the compiler's interface.

#pragma once

#include "Lexer.h"
#include "Types.h"

#include <map>

namespace Kestrel::Compiler {

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
extern int labelCounter;  // .L labels: case labels here, the rest in the code generator
extern int stringCounter;

Token& Tok();
Token& TokAt(size_t index);
bool IsText(const Token& token, const char* text);
bool Is(const char* text);
bool Accept(const char* text);
void Expect(const char* text);
string ExpectIdent();
Symbol* FindSymbol(const string& name);
bool IsTypeToken(const Token& token);
Type* TypeName();
void EnterScope();
void TopLevel();
void ResetParser();

}  // namespace Kestrel::Compiler
