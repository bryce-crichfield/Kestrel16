// Dump.cpp -- __dump and how it's added to a program.

#include "Dump.h"

#include "Kestrel/Isa.h"

#include <cstdio>

namespace Kestrel {

const char* const DumpFlagNames[DumpFlagWords] = {"Z", "N", "C", "V"};

namespace {

// Stores and ldi/lhi/lw leave the flags alone, so the registers are saved first, and then each
// flag is read with the branch that tests it. The offsets fit lw/sw's -32..31.
const char* const DumpRoutineFormat = R"(
; ------------------------------------------------------------------ __dump (added by KestrelValidator)
__dump:
    sw   r0, [sp-2]          ; free r0 for the dump area's address
    ldi  r0, lo(0x%04X)
    lhi  r0, hi(0x%04X)
    sw   r1, [r0+2]
    sw   r2, [r0+4]
    sw   r3, [r0+6]
    sw   r4, [r0+8]
    sw   r5, [r0+10]
    sw   r6, [r0+12]
    sw   r7, [r0+14]
    lw   r1, [sp-2]
    sw   r1, [r0+0]
    ldi  r1, 1
    beq  __dump_z
    ldi  r1, 0
__dump_z:
    sw   r1, [r0+16]
    ldi  r1, 1
    bmi  __dump_n
    ldi  r1, 0
__dump_n:
    sw   r1, [r0+18]
    ldi  r1, 1
    bcs  __dump_c
    ldi  r1, 0
__dump_c:
    sw   r1, [r0+20]
    ldi  r1, 1
    bvs  __dump_v
    ldi  r1, 0
__dump_v:
    sw   r1, [r0+22]
    di
    halt
)";

// The compiler's _start (Compiler/Program.cpp) calls main, then halts.
const char* const CompiledEnding = "jalr r4\n    halt\n";
const char* const DumpEnding = "jalr r4\n    j __dump\n";

}  // namespace

bool AddDump(std::string& assembly, bool fromC) {
    if (fromC) {
        size_t ending = assembly.find(CompiledEnding);
        if (ending == std::string::npos) {
            return false;
        }
        assembly.replace(ending, std::string(CompiledEnding).size(), DumpEnding);
    }
    else if (assembly.find("__dump") == std::string::npos) {
        return false;
    }
    char routine[1024];
    snprintf(routine, sizeof routine, DumpRoutineFormat, Address::DumpArea, Address::DumpArea);
    assembly += routine;
    return true;
}

}  // namespace Kestrel
