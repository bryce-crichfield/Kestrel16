// Toolchain.h -- a program file to a memory image, compiling and assembling as needed, in-process.
// The emulator's and the simulator's --run use it.

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace Kestrel {

// path is a .c (compiled, then assembled), a .s (assembled) or a .bin (read as it is).
// Warnings and errors go to diagnostics. Returns false if the program couldn't be built.
bool BuildProgram(const std::string& path, std::vector<uint8_t>& image, FILE* diagnostics);

}  // namespace Kestrel
