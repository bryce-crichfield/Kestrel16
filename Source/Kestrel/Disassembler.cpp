#include "Disassembler.h"

#include "Isa.h"

#include <cstdio>

namespace Kestrel {

std::string Disassemble(uint16_t address, uint16_t word) {
    Instruction instruction = Decode(word);
    uint16_t nextAddress = static_cast<uint16_t>(address + InstructionSize);
    char text[48];

    if (instruction.opcode == Opcode::Branch) {
        snprintf(text, sizeof text, "b%s 0x%04X", ConditionSuffix(instruction.condition), BranchTarget(nextAddress, instruction.offset8));
        return text;
    }
    const InstructionInfo* info = FindInstruction(instruction);
    if (!info) {
        return "???";
    }
    const char* name = info->mnemonic;
    switch (info->form) {
        case InstructionForm::ThreeRegisters:
            snprintf(text, sizeof text, "%s r%d, r%d, r%d", name, instruction.destination, instruction.sourceA, instruction.sourceB);
            break;
        case InstructionForm::TwoRegisters:
            snprintf(text, sizeof text, "%s r%d, r%d", name, instruction.destination, instruction.sourceA);
            break;
        case InstructionForm::CompareRegisters:
            snprintf(text, sizeof text, "%s r%d, r%d", name, instruction.sourceA, instruction.sourceB);
            break;
        case InstructionForm::RegisterImmediate:
            snprintf(text, sizeof text, "%s r%d, r%d, %d", name, instruction.destination, instruction.sourceA, instruction.immediate6);
            break;
        case InstructionForm::CompareImmediate:
            snprintf(text, sizeof text, "%s r%d, %d", name, instruction.sourceA, instruction.immediate6);
            break;
        case InstructionForm::LoadImmediate:
            snprintf(text, sizeof text, "%s r%d, 0x%02X", name, instruction.destination, instruction.immediate8);
            break;
        case InstructionForm::Memory:
            snprintf(text, sizeof text, "%s r%d, [r%d%+d]", name, instruction.destination, instruction.sourceA, instruction.immediate6);
            break;
        case InstructionForm::Jump:
            snprintf(text, sizeof text, "%s 0x%04X", name, BranchTarget(nextAddress, instruction.offset12));
            break;
        case InstructionForm::SystemRegister:
            snprintf(text, sizeof text, "%s r%d", name, instruction.systemSource);
            break;
        case InstructionForm::SystemBare:
            snprintf(text, sizeof text, "%s", name);
            break;
    }
    return text;
}

}  // namespace Kestrel
