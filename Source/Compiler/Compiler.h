// Compiler.h -- a small C compiler for the Kestrel16: C source to Kestrel16 assembly.
//
// One .c file (plus what it #includes) in, one assembly program out. The preprocessor covers
// #include, #define (object- and function-like), #undef, #if / #ifdef / #ifndef / #elif / #else /
// #endif and line continuation.
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
// How the generated code uses the machine is described in CodeGenerator.cpp.

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace Kestrel {

struct CompileOptions {
    std::vector<std::string> includePaths;                     // searched for #include, in order
    std::vector<std::pair<std::string, std::string>> defines;  // name, value: as -D name=value
};

struct CompileResult {
    bool ok = false;
    std::string output;                 // the assembly (or with Preprocess, the preprocessed text)
    std::vector<std::string> warnings;  // "source.c: warning: ..."
    std::string error;                  // "file.c:12: error: ..." when !ok
};

// sourceName is the file text came from: messages name it, and #include "..." searches its folder.
CompileResult Compile(const std::string& sourceName, const std::string& text, const CompileOptions& options = {});
CompileResult Preprocess(const std::string& sourceName, const std::string& text, const CompileOptions& options = {});

}  // namespace Kestrel
