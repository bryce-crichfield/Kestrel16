// Preprocessor.h -- the preprocessor (Preprocessor.cpp): C source text to C source text.
// Internal to Source/Compiler; Compiler.h is the compiler's interface.

#pragma once

#include "Errors.h"

#include <map>

namespace Kestrel::Compiler {

struct Macro {
    bool function = false;
    vector<string> params;
    vector<string> body;  // pp-tokens, whitespace included
};

extern vector<string> includePaths;
extern std::map<string, Macro> macros;

vector<string> PPTokens(const string& text);
void Preprocess(const string& file, const string& text, string& out, int depth);
void ResetPreprocessor();

}  // namespace Kestrel::Compiler
