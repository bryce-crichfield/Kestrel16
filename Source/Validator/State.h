// State.h -- what KestrelValidator compares: the state a machine ends in.
//
// That is all of RAM plus what the program printed. Registers and flags aren't read from the
// machine; a program that ends in __dump (Dump.h) writes them into RAM first, so they're in it.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Kestrel {

struct MachineState {
    std::vector<uint8_t> ram;  // 0 up to Address::IoBase
    std::string output;
};

// The differences between expected (the emulator's) and actual, one per line, by name: the
// registers and flags in the dump area (if dumped), then memory and output. Empty if they match.
std::vector<std::string> Compare(const MachineState& expected, const MachineState& actual, bool dumped);

// The dump area, decoded: "R0=0x0000 ... Z=0 N=1 C=0 V=0".
std::string DescribeDump(const MachineState& state);

}  // namespace Kestrel
