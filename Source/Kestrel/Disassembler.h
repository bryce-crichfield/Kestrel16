// Disassembler.h -- one instruction word back to assembly text.

#pragma once

#include <cstdint>
#include <string>

namespace Kestrel {

// The instruction at address, as the assembler would take it ("???" for an illegal word).
// Branch and jump targets are shown as absolute addresses.
std::string Disassemble(uint16_t address, uint16_t word);

}  // namespace Kestrel
