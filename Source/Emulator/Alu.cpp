#include "Alu.h"

namespace Kestrel {

namespace {

constexpr uint16_t SignBit16 = 0x8000;
constexpr uint32_t CarryOut16 = 0x10000;

bool HasFlag(uint8_t flags, uint8_t flag) {
    return (flags & flag) != 0;
}

void SetZeroNegative(uint16_t result, uint8_t& flags) {
    flags &= static_cast<uint8_t>(~(Flag::Zero | Flag::Negative));
    if (result == 0) {
        flags |= Flag::Zero;
    }
    if (result & SignBit16) {
        flags |= Flag::Negative;
    }
}

}  // namespace

uint16_t AluAdd(uint16_t a, uint16_t b, unsigned carryIn, uint8_t& flags) {
    uint32_t sum = static_cast<uint32_t>(a) + b + carryIn;
    uint16_t result = static_cast<uint16_t>(sum);
    SetZeroNegative(result, flags);
    flags &= static_cast<uint8_t>(~(Flag::Carry | Flag::Overflow));
    if (sum & CarryOut16) {
        flags |= Flag::Carry;
    }
    if (~(a ^ b) & (a ^ result) & SignBit16) {
        flags |= Flag::Overflow;
    }
    return result;
}

uint16_t AluSubtract(uint16_t a, uint16_t b, unsigned carryIn, uint8_t& flags) {
    return AluAdd(a, static_cast<uint16_t>(~b), carryIn, flags);
}

uint16_t AluLogic(uint16_t result, uint8_t& flags) {
    SetZeroNegative(result, flags);
    return result;
}

uint16_t AluShift(ShiftKind kind, uint16_t value, unsigned amount, uint8_t& flags) {
    uint16_t result = value;
    if (amount) {
        int carry;
        if (kind == ShiftKind::Left) {
            carry = (value >> (WordBits - amount)) & 1;
            result = static_cast<uint16_t>(value << amount);
        }
        else {
            carry = (value >> (amount - 1)) & 1;
            result = static_cast<uint16_t>(value >> amount);
            if (kind == ShiftKind::RightArithmetic && (value & SignBit16)) {
                result |= static_cast<uint16_t>(0xFFFFu << (WordBits - amount));
            }
        }
        flags = static_cast<uint8_t>((flags & ~Flag::Carry) | (carry ? Flag::Carry : 0));
    }
    SetZeroNegative(result, flags);
    return result;
}

uint16_t SignExtendByte(uint16_t value) {
    return (value & 0x80) ? static_cast<uint16_t>(value | 0xFF00) : static_cast<uint16_t>(value & 0xFF);
}

bool ConditionHolds(int condition, uint8_t flags) {
    bool zero = HasFlag(flags, Flag::Zero), negative = HasFlag(flags, Flag::Negative);
    bool carry = HasFlag(flags, Flag::Carry), overflow = HasFlag(flags, Flag::Overflow);
    switch (static_cast<Condition>(condition)) {
        case Condition::Always:
            return true;
        case Condition::Equal:
            return zero;
        case Condition::NotEqual:
            return !zero;
        case Condition::LessUnsigned:
            return !carry;
        case Condition::GreaterOrEqualUnsigned:
            return carry;
        case Condition::Less:
            return negative != overflow;
        case Condition::GreaterOrEqual:
            return negative == overflow;
        case Condition::Greater:
            return !zero && negative == overflow;
        case Condition::LessOrEqual:
            return zero || negative != overflow;
        case Condition::Minus:
            return negative;
        case Condition::Plus:
            return !negative;
        case Condition::OverflowSet:
            return overflow;
        case Condition::OverflowClear:
            return !overflow;
        default:
            return false;
    }
}

}  // namespace Kestrel
