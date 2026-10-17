// Errors.h -- compile errors and warnings (Compiler.cpp).
// Internal to Source/Compiler; Compiler.h is the compiler's interface.

#pragma once

#include <stdexcept>
#include <string>
#include <vector>

namespace Kestrel::Compiler {

using std::string;
using std::vector;

struct CompileError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

extern string sourceName;
extern vector<string> warnings;

// A location is one int: the file's index in fileNames times LocationLines, plus the line.
// Both are kept by the preprocessor (Preprocessor.cpp).
constexpr int LocationLines = 1 << 20;
extern vector<string> fileNames;  // every file read, for error locations
int Location(const string& file, int line);

// Throws a CompileError for a location (see Location()).
[[noreturn]] void Fatal(int location, const char* format, ...);

}  // namespace Kestrel::Compiler
