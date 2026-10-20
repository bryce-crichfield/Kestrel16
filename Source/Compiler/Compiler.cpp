// Compiler.cpp -- Compile() and Preprocess(): resets the compiler and runs its stages.
//
// The compiler runs in stages, one file (and header) each:
//   Preprocessor.cpp   text -> text, with # <line> "<file>" markers
//   Lexer.cpp          text -> tokens
//   Parser.cpp         tokens -> declarations and statements (AST), function by function
//   Expressions.cpp    the expressions, typed as they are built, and constant evaluation
//   Types.cpp          C types and syntax tree nodes
//   Generator.cpp      each function's AST -> assembly, as soon as it is parsed
//   Program.cpp        the whole program: start-up code, functions, runtime routines, data
// Their state is file-level, so a compile is not thread-safe; Compile() resets it first.

#include "Compiler.h"

#include "Errors.h"
#include "Generator.h"
#include "Lexer.h"
#include "Parser.h"
#include "Preprocessor.h"
#include "Program.h"
#include "Types.h"

#include <cstdarg>
#include <cstdio>

namespace Kestrel::Compiler {

string sourceName;
vector<string> warnings;

void Fatal(int location, const char* format, ...) {
    int file = location / LocationLines;
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof message, format, args);
    va_end(args);
    const string& name = file < static_cast<int>(fileNames.size()) ? fileNames[static_cast<size_t>(file)] : sourceName;
    throw CompileError(name + ":" + std::to_string(location % LocationLines) + ": error: " + message);
}

namespace {

// Clears what the last compile left and preprocesses text. Returns the preprocessed text.
string Start(const string& name, const string& text, const CompileOptions& options) {
    sourceName = name;
    warnings.clear();
    ResetPreprocessor();
    ResetParser();
    ResetGenerator();
    includePaths = options.includePaths;
    for (const auto& [macro, value] : options.defines) {
        macros[macro].body = PPTokens(value);
    }

    string preprocessed;
    Preprocess(sourceName, text, preprocessed, 0);
    return preprocessed;
}

string CompileText(const string& name, const string& text, const CompileOptions& options) {
    tokens = Lex(Start(name, text, options));
    pos = 0;
    EnterScope();
    while (Tok().kind != TokEof) {
        TopLevel();
    }

    auto mainIt = globalsByName.find("main");
    if (mainIt == globalsByName.end() || mainIt->second->type->kind != KindFunction || !mainIt->second->isDefined) {
        Fatal(Tok().line, "no definition of 'main'");
    }
    for (Var* global : globals) {
        if (global->type->kind == KindFunction && !global->isDefined) {
            warnings.push_back(sourceName + ": warning: function '" + global->name + "' is declared but never defined");
        }
    }
    return ProgramText();
}

}  // namespace

}  // namespace Kestrel::Compiler

namespace Kestrel {

CompileResult Compile(const std::string& sourceName, const std::string& text, const CompileOptions& options) {
    CompileResult result;
    try {
        result.output = Compiler::CompileText(sourceName, text, options);
        result.ok = true;
    } catch (const Compiler::CompileError& error) {
        result.error = error.what();
    }
    result.warnings = Compiler::warnings;
    return result;
}

CompileResult Preprocess(const std::string& sourceName, const std::string& text, const CompileOptions& options) {
    CompileResult result;
    try {
        result.output = Compiler::Start(sourceName, text, options);
        result.ok = true;
    } catch (const Compiler::CompileError& error) {
        result.error = error.what();
    }
    return result;
}

}  // namespace Kestrel
