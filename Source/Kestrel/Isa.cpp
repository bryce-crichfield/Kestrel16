#include "Isa.h"

namespace Kestrel {

namespace {

// Every function name of Instructions.h, so a table row can name its function without saying
// which kind it is (the names don't overlap).
enum FunctionName : int {
    None = 0,
#define KestrelAluFunction(name, value) name = value,
#define KestrelAlu2Function(name, value) name = value,
#define KestrelSystemFunction(name, value) name = value,
#include "Instructions.h"
#undef KestrelAluFunction
#undef KestrelAlu2Function
#undef KestrelSystemFunction
};

bool HasFunction(Opcode opcode) {
    return opcode == Opcode::Alu || opcode == Opcode::Alu2 || opcode == Opcode::System;
}

}  // namespace

const InstructionInfo Instructions[] = {
#define KestrelInstruction(mnemonic, form, opcode, function) \
    {mnemonic, InstructionForm::form, Opcode::opcode, FunctionName::function},
#include "Instructions.h"
#undef KestrelInstruction
};
const int InstructionCount = sizeof Instructions / sizeof Instructions[0];

const BranchInfo Branches[] = {
#define KestrelCondition(name, value, suffix) {suffix, Condition::name},
#include "Instructions.h"
#undef KestrelCondition
#define KestrelBranchAlias(suffix, condition) {suffix, Condition::condition},
#include "Instructions.h"
#undef KestrelBranchAlias
};
const int BranchCount = sizeof Branches / sizeof Branches[0];

const char* ConditionSuffix(int condition) {
    switch (condition) {
#define KestrelCondition(name, value, suffix) \
    case value:                               \
        return suffix;
#include "Instructions.h"
#undef KestrelCondition
        default:
            return "??";
    }
}

int SignExtend(int value, int bits) {
    int signMask = 1 << (bits - 1);
    value &= (1 << bits) - 1;
    return (value ^ signMask) - signMask;
}

Instruction Decode(uint16_t word) {
    Instruction instruction;
    instruction.opcode = static_cast<Opcode>(OpcodeField.Get(word));
    instruction.destination = DestinationField.Get(word);
    instruction.sourceA = SourceAField.Get(word);
    instruction.sourceB = SourceBField.Get(word);
    instruction.function = FunctionField.Get(word);
    instruction.condition = ConditionField.Get(word);
    instruction.systemFunction = SystemFunctionField.Get(word);
    instruction.systemSource = SystemSourceField.Get(word);
    instruction.immediate6 = SignExtend(word, Immediate6Field.bits);
    instruction.immediate8 = Immediate8Field.Get(word);
    instruction.offset8 = SignExtend(word, Offset8Field.bits);
    instruction.offset12 = SignExtend(word, Offset12Field.bits);
    return instruction;
}

const InstructionInfo* FindInstruction(const Instruction& instruction) {
    for (int index = 0; index < InstructionCount; index++) {
        const InstructionInfo& info = Instructions[index];
        if (info.opcode != instruction.opcode) {
            continue;
        }
        if (!HasFunction(info.opcode)) {
            return &info;
        }
        int function = info.opcode == Opcode::System ? instruction.systemFunction : instruction.function;
        if (info.function == function) {
            return &info;
        }
    }
    return nullptr;
}

uint16_t EncodeRegisters(Opcode opcode, int destination, int sourceA, int sourceB, int function) {
    return OpcodeField.Put(static_cast<int>(opcode)) | DestinationField.Put(destination) | SourceAField.Put(sourceA) |
           SourceBField.Put(sourceB) | FunctionField.Put(function);
}

uint16_t EncodeImmediate(Opcode opcode, int destination, int sourceA, int immediate6) {
    return OpcodeField.Put(static_cast<int>(opcode)) | DestinationField.Put(destination) | SourceAField.Put(sourceA) |
           Immediate6Field.Put(immediate6);
}

uint16_t EncodeLong(Opcode opcode, int destination, int immediate8) {
    return OpcodeField.Put(static_cast<int>(opcode)) | DestinationField.Put(destination) | Immediate8Field.Put(immediate8);
}

uint16_t EncodeBranch(Condition condition, int offset8) {
    return OpcodeField.Put(static_cast<int>(Opcode::Branch)) | ConditionField.Put(static_cast<int>(condition)) |
           Offset8Field.Put(offset8);
}

uint16_t EncodeJump(Opcode opcode, int offset12) {
    return OpcodeField.Put(static_cast<int>(opcode)) | Offset12Field.Put(offset12);
}

uint16_t EncodeSystem(SystemFunction function, int source) {
    return OpcodeField.Put(static_cast<int>(Opcode::System)) | SystemFunctionField.Put(static_cast<int>(function)) |
           SystemSourceField.Put(source);
}

}  // namespace Kestrel
