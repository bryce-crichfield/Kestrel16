// Toolchain.h -- a program file to a memory image, compiling and assembling as needed, in-process.
// The emulator's and the simulator's --run use it.

#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace Kestrel {

// path is a .c (compiled, then assembled), a .s (assembled) or a .bin (read as it is).
// Warnings and errors go to diagnostics. Returns false if the program couldn't be built.
// editAssembly, if given, may change the assembly text (compiled or read) before it's assembled;
// fromC says it came out of the compiler.
using AssemblyEdit = std::function<void(std::string& assembly, bool fromC)>;
bool BuildProgram(const std::string& path, std::vector<uint8_t>& image, FILE* diagnostics, const AssemblyEdit& editAssembly = nullptr);

}  // namespace Kestrel
