// Dump.h -- __dump: the routine a validated program ends with, instead of a bare halt.
//
// It stores R0-R7 and the flags into the dump area (Address::DumpArea) and halts with interrupts
// off, so the registers become part of RAM, which is all the validator reads. The only other
// thing it changes is the word just below the stack pointer (R0 waits there while the routine
// frees a register for the address), and it does that the same way on every machine.
//
// The dump area, in words: R0-R7, then Z, N, C and V as 0 or 1.

#pragma once

#include <string>

namespace Kestrel {

constexpr int DumpRegisterWords = 8, DumpFlagWords = 4;
constexpr int DumpBytes = 2 * (DumpRegisterWords + DumpFlagWords);
extern const char* const DumpFlagNames[DumpFlagWords];

// Adds __dump to a program's assembly. A compiled program's runtime ends in it instead of halt;
// an assembly program gets it if it uses it (ends with "j __dump"). Returns whether the program
// now ends in __dump.
bool AddDump(std::string& assembly, bool fromC);

}  // namespace Kestrel
