// Generator.h -- code generation (Generator.cpp): each function's syntax tree to assembly, as soon as it is parsed.
// Internal to Source/Compiler; Compiler.h is the compiler's interface.

#pragma once

#include "Types.h"

namespace Kestrel::Compiler {

extern string code;
extern bool useMultiply, useDivide, useMemcpy, useMemzero;

void GenerateFunction(Var* function, Node* body, int frameSize);
void ResetGenerator();

}  // namespace Kestrel::Compiler
