// Isa.h -- the Kestrel16 instruction set and memory map for C++: enums and tables built from
// Instructions.h and MemoryMap.h, and encoding and decoding of instruction words.

#pragma once

#include <cstdint>

namespace Kestrel {

#define KestrelConstant(name, value) constexpr int name = value;
#include "Instructions.h"
#undef KestrelConstant

#define KestrelRegister(name, number, assemblerName) constexpr int name = number;
#include "Instructions.h"
#undef KestrelRegister

// Where a field sits in an instruction word: OpcodeField, DestinationField, ...
struct Field {
    int shift, bits;
    constexpr uint16_t Mask() const { return static_cast<uint16_t>((1u << bits) - 1); }
    constexpr int Get(uint16_t word) const { return (word >> shift) & Mask(); }
    constexpr uint16_t Put(int value) const { return static_cast<uint16_t>((value & Mask()) << shift); }
};
#define KestrelField(name, shift, bits) constexpr Field name##Field{shift, bits};
#include "Instructions.h"
#undef KestrelField

enum class Opcode : uint8_t {
#define KestrelOpcode(name, value) name = value,
#include "Instructions.h"
#undef KestrelOpcode
};

enum class AluFunction : uint8_t {
#define KestrelAluFunction(name, value) name = value,
#include "Instructions.h"
#undef KestrelAluFunction
};

enum class Alu2Function : uint8_t {
#define KestrelAlu2Function(name, value) name = value,
#include "Instructions.h"
#undef KestrelAlu2Function
};

enum class SystemFunction : uint8_t {
#define KestrelSystemFunction(name, value) name = value,
#include "Instructions.h"
#undef KestrelSystemFunction
};

enum class Condition : uint8_t {
#define KestrelCondition(name, value, suffix) name = value,
#include "Instructions.h"
#undef KestrelCondition
};

enum class InstructionForm : uint8_t {
#define KestrelForm(name) name,
#include "Instructions.h"
#undef KestrelForm
};

struct Address {
#define KestrelAddress(name, value) static constexpr uint16_t name = value;
#include "MemoryMap.h"
#undef KestrelAddress
#define KestrelRegisterAddress(name, address) static constexpr uint16_t name = address;
#include "MemoryMap.h"
#undef KestrelRegisterAddress
};

struct IoBus {
#define KestrelIoBus(name, value) static constexpr int name = value;
#include "MemoryMap.h"
#undef KestrelIoBus
};
static_assert(Address::IoBase + IoBus::SlotSize * IoBus::SlotCount == 0x10000, "the slots fill the I/O space");

// Slot numbers, which are also the devices' bits in the interrupt pending and mask registers.
struct Slot {
#define KestrelSlot(name, number) static constexpr int name = number;
#include "MemoryMap.h"
#undef KestrelSlot
};

constexpr int SlotOf(uint16_t ioAddress) {
    return (ioAddress - Address::IoBase) / IoBus::SlotSize;
}
static_assert(SlotOf(Address::TimerAcknowledge) == Slot::System && SlotOf(Address::UartStatus) == Slot::Uart,
              "each register sits in its device's slot");

struct UartStatus {
#define KestrelUartStatusBit(name, bit) static constexpr uint16_t name = 1 << bit;
#include "MemoryMap.h"
#undef KestrelUartStatusBit
};

// A table row of Instructions.h. function is the AluFunction, Alu2Function or SystemFunction
// (as a number), or 0 for an instruction without one.
struct InstructionInfo {
    const char* mnemonic;
    InstructionForm form;
    Opcode opcode;
    int function;
};

extern const InstructionInfo Instructions[];
extern const int InstructionCount;

// A branch mnemonic is "b" + a suffix: every condition's own and the aliases.
struct BranchInfo {
    const char* suffix;
    Condition condition;
};

extern const BranchInfo Branches[];
extern const int BranchCount;

const char* ConditionSuffix(int condition);  // "??" for a condition with none

// Every field of an instruction word, decoded as if it had that field.
struct Instruction {
    Opcode opcode;
    int destination;
    int sourceA;
    int sourceB;
    int function;
    int condition;
    int systemFunction;
    int systemSource;
    int immediate6;  // sign extended
    int immediate8;  // zero extended
    int offset8;     // sign extended, in words
    int offset12;    // sign extended, in words
};

int SignExtend(int value, int bits);
Instruction Decode(uint16_t word);

// The row of Instructions that word is, or nullptr (a branch, or an illegal word).
const InstructionInfo* FindInstruction(const Instruction& instruction);

// Where a branch or jump goes: offsets count words from the next instruction.
inline uint16_t BranchTarget(uint16_t nextAddress, int offsetInWords) {
    return static_cast<uint16_t>(nextAddress + offsetInWords * InstructionSize);
}

uint16_t EncodeRegisters(Opcode opcode, int destination, int sourceA, int sourceB, int function);
uint16_t EncodeImmediate(Opcode opcode, int destination, int sourceA, int immediate6);
uint16_t EncodeLong(Opcode opcode, int destination, int immediate8);
uint16_t EncodeBranch(Condition condition, int offset8);
uint16_t EncodeJump(Opcode opcode, int offset12);
uint16_t EncodeSystem(SystemFunction function, int source);

}  // namespace Kestrel
