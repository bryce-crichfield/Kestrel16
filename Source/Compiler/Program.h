// Program.h -- the whole program (Program.cpp): start-up code, functions, runtime routines and data.
// Internal to Source/Compiler; Compiler.h is the compiler's interface.

#pragma once

#include "Errors.h"

namespace Kestrel::Compiler {

string ProgramText();  // everything, once every function has been generated

}  // namespace Kestrel::Compiler
