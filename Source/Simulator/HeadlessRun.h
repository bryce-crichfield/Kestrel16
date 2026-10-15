// HeadlessRun.h -- --run: simulates a component with no window, gate by gate, and prints what
// its Terminal prints to stdout. With a program (a .c, .s or .bin, built in-process), the program
// replaces the image of every top-level RAM that has one, keeping each RAM's byte lane. Piped
// stdin is typed into the Terminal as the program asks for it; the run stops (status 0) once it
// has run out and the program waits for more.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Kestrel {

struct RunOptions {
    std::string component;
    std::string program;
    std::vector<std::string> stopPins, failPins;  // 1-bit Output labels
    uint64_t maxCycles = 10000000;
    double hz = 0;  // clock rate to hold to; 0 = as fast as the simulation goes
    bool stats = false;
};

// Exit status: 0 stopped on a stop pin, 1 a fail pin went high, 2 a usage or load problem,
// 3 the cycle limit, 4 the circuit oscillates.
int RunHeadless(const RunOptions& options);

}  // namespace Kestrel
