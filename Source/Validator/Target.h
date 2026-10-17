// Target.h -- a machine KestrelValidator can run a program on.
//
// A target offers only what a cheap debug port on the real machine could: load memory, run (the
// validator owns the clock), notice that the CPU halted, type and read the terminal, and read
// memory back once it has stopped. Nothing reaches into the CPU, so the same interface can one
// day sit on a link to the breadboard.

#pragma once

#include "State.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Kestrel {

enum class RunResult { Halted,
                       Illegal,
                       LimitReached,
                       Failed };  // the target itself went wrong (e.g. the circuit oscillates)

const char* RunResultName(RunResult result);

class Target {
   public:
    virtual ~Target() = default;
    virtual const char* Name() const = 0;
    // The program's image, loaded at 0x0000 into RAM that is otherwise zero.
    virtual bool Load(const std::vector<uint8_t>& image) = 0;
    // Bytes the terminal holds for the program to read, typed before it starts.
    virtual void Type(const std::string& input) = 0;
    // Runs until the CPU halts for good, hits an illegal instruction, or the limit (in the
    // target's own unit: instructions or clock cycles).
    virtual RunResult Run(uint64_t limit) = 0;
    virtual uint64_t Ran() const = 0;  // how much of the limit it used
    virtual const char* Unit() const = 0;
    virtual MachineState ReadState() = 0;
    virtual std::string Problem() const { return ""; }  // why Load or Run failed
};

}  // namespace Kestrel
