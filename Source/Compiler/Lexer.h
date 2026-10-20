// Lexer.h -- the lexer (Lexer.cpp): preprocessed text to tokens.
// Internal to Source/Compiler; Compiler.h is the compiler's interface.

#pragma once

#include "Errors.h"

namespace Kestrel::Compiler {

enum TokenKind { TokIdent,
                 TokKeyword,
                 TokNumber,
                 TokString,
                 TokPunct,
                 TokEof };

struct Token {
    TokenKind kind;
    string text;  // identifier / keyword / punctuator, or string bytes
    long long value = 0;
    bool isUnsigned = false;
    int line = 0;
};

vector<Token> Lex(const string& src);

}  // namespace Kestrel::Compiler
