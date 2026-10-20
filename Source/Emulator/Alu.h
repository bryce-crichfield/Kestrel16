// Alu.h -- the Kestrel16's arithmetic and its flags, as the emulator (the reference) computes them.
// The validator holds the gate-level computer to the results they produce.

#pragma once

#include "Kestrel/Isa.h"

#include <cstdint>

namespace Kestrel {

struct Flag {
    static constexpr uint8_t Zero = 1 << 0;
    static constexpr uint8_t Negative = 1 << 1;
    static constexpr uint8_t Carry = 1 << 2;
    static constexpr uint8_t Overflow = 1 << 3;
    static constexpr uint8_t InterruptEnable = 1 << 4;  // only in the flags saved on an exception
    static constexpr uint8_t ArithmeticMask = Zero | Negative | Carry | Overflow;
};

enum class ShiftKind { Left,
                       RightLogical,
                       RightArithmetic };

// Each takes the flags and updates the ones it sets.
uint16_t AluAdd(uint16_t a, uint16_t b, unsigned carryIn, uint8_t& flags);           // a + b + carryIn: Z N C V
uint16_t AluSubtract(uint16_t a, uint16_t b, unsigned carryIn, uint8_t& flags);      // a + ~b + carryIn: Z N C V
uint16_t AluLogic(uint16_t result, uint8_t& flags);                                  // Z N
uint16_t AluShift(ShiftKind kind, uint16_t value, unsigned amount, uint8_t& flags);  // Z N, and C = the
                                                                                     // last bit out (kept if amount is 0)
uint16_t SignExtendByte(uint16_t value);

bool ConditionHolds(int condition, uint8_t flags);  // false for a condition that doesn't exist

}  // namespace Kestrel
