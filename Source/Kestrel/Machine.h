// Machine.h -- the Kestrel16 itself: the reference implementation everything else is held to.
// The emulator runs it; the instruction set is described in Instructions.h and the memory map
// in MemoryMap.h.

#pragma once

#include "Isa.h"

#include <array>
#include <cstdint>

namespace Kestrel {

constexpr int MemorySize = 65536;

// What is on the other end of the UART: a keyboard and a screen.
class Terminal {
public:
    virtual ~Terminal() = default;
    virtual bool KeyWaiting() = 0;      // a typed byte is ready to read
    virtual uint8_t ReadKey() = 0;      // takes it (only called when KeyWaiting())
    virtual void Print(uint8_t byte) = 0;
};

struct Cpu {
    uint16_t registers[RegisterCount] = {};
    uint16_t programCounter = Address::ResetVector;
    uint16_t exceptionProgramCounter = 0;
    uint8_t flags = 0;
    uint8_t exceptionFlags = 0;
    bool interruptsEnabled = false;
    bool halted = false;
    uint8_t interruptMask = 0;      // bit n: slot n may interrupt
    bool timerPending = false;      // the timer's interrupt request, until acknowledged
    uint16_t timerReload = 0;
    uint16_t timerCount = 0;
    unsigned long long steps = 0;
};

enum class StepResult { Running, Halted, Illegal, LimitReached };

// What one Step did: the instruction it ran (or tried to), if it ran one.
struct StepInfo {
    bool executed = false;  // false while sleeping in HALT, waiting for an interrupt
    uint16_t address = 0;
    uint16_t word = 0;
};

class Machine {
public:
    explicit Machine(Terminal& terminal) : terminal(terminal) {}

    std::array<uint8_t, MemorySize> memory{};
    Cpu cpu;

    // Runs one instruction (taking an interrupt first if one is due). Returns Running, Halted
    // (HALT with nothing left to wake it) or Illegal (cpu.programCounter is left on the word).
    StepResult Step(StepInfo* info = nullptr);

    uint16_t ReadMemoryWord(uint16_t address) const;

private:
    Terminal& terminal;

    uint16_t IoRead(uint16_t address);
    void IoWrite(uint16_t address, uint16_t value);
    uint16_t ReadWord(uint16_t address);
    void WriteWord(uint16_t address, uint16_t value);
    uint16_t ReadByte(uint16_t address);
    void WriteByte(uint16_t address, uint8_t value);

    void TickTimer();
    uint8_t PendingInterrupts(uint8_t slots);  // the requests among slots, bit n = slot n
    bool CanWake();                            // could an interrupt still end a HALT?
    uint8_t PackFlags() const;
    void EnterException(uint16_t returnAddress);
    bool ExecuteAlu(const Instruction& in);
    bool ExecuteAlu2(const Instruction& in);
    bool ExecuteSystem(const Instruction& in, uint16_t& nextAddress);
    uint16_t EffectiveAddress(const Instruction& in) const;
};

}  // namespace Kestrel
