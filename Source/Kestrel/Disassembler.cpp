#include "Disassembler.h"

#include "Isa.h"

#include <cstdio>

namespace Kestrel {

std::string Disassemble(uint16_t address, uint16_t word) {
    Instruction in = Decode(word);
    uint16_t nextAddress = static_cast<uint16_t>(address + InstructionSize);
    char text[48];

    if (in.opcode == Opcode::Branch) {
        snprintf(text, sizeof text, "b%s 0x%04X", ConditionSuffix(in.condition), BranchTarget(nextAddress, in.offset8));
        return text;
    }
    const InstructionInfo* info = FindInstruction(in);
    if (!info)
        return "???";
    const char* name = info->mnemonic;
    switch (info->form) {
        case InstructionForm::ThreeRegisters:
            snprintf(text, sizeof text, "%s r%d, r%d, r%d", name, in.destination, in.sourceA, in.sourceB);
            break;
        case InstructionForm::TwoRegisters:
            snprintf(text, sizeof text, "%s r%d, r%d", name, in.destination, in.sourceA);
            break;
        case InstructionForm::CompareRegisters:
            snprintf(text, sizeof text, "%s r%d, r%d", name, in.sourceA, in.sourceB);
            break;
        case InstructionForm::RegisterImmediate:
            snprintf(text, sizeof text, "%s r%d, r%d, %d", name, in.destination, in.sourceA, in.immediate6);
            break;
        case InstructionForm::CompareImmediate:
            snprintf(text, sizeof text, "%s r%d, %d", name, in.sourceA, in.immediate6);
            break;
        case InstructionForm::LoadImmediate:
            snprintf(text, sizeof text, "%s r%d, 0x%02X", name, in.destination, in.immediate8);
            break;
        case InstructionForm::Memory:
            snprintf(text, sizeof text, "%s r%d, [r%d%+d]", name, in.destination, in.sourceA, in.immediate6);
            break;
        case InstructionForm::Jump:
            snprintf(text, sizeof text, "%s 0x%04X", name, BranchTarget(nextAddress, in.offset12));
            break;
        case InstructionForm::SystemRegister:
            snprintf(text, sizeof text, "%s r%d", name, in.systemSource);
            break;
        case InstructionForm::SystemBare:
            snprintf(text, sizeof text, "%s", name);
            break;
    }
    return text;
}

}  // namespace Kestrel
