// State.cpp -- comparing the state two machines end in.

#include "State.h"
#include "Dump.h"

#include "Kestrel/Isa.h"

#include <cstdio>

namespace Kestrel {

namespace {

uint16_t WordAt(const std::vector<uint8_t>& ram, size_t address) {
    return static_cast<uint16_t>(ram[address] | ram[address + 1] << 8);
}

bool InDumpArea(size_t address) {
    return address >= Address::DumpArea && address < Address::DumpArea + DumpBytes;
}

std::string Printable(const std::string& text) {
    std::string out;
    for (unsigned char character : text) {
        if (character == '\n') {
            out += "\\n";
        }
        else if (character < ' ' || character >= 127) {
            char escaped[8];
            snprintf(escaped, sizeof escaped, "\\x%02X", character);
            out += escaped;
        }
        else {
            out += static_cast<char>(character);
        }
    }
    return out;
}

}  // namespace

std::vector<std::string> Compare(const MachineState& expected, const MachineState& actual, bool dumped) {
    std::vector<std::string> differences;
    char line[256];
    if (dumped) {
        for (int index = 0; index < DumpRegisterWords + DumpFlagWords; index++) {
            size_t address = Address::DumpArea + 2 * static_cast<size_t>(index);
            uint16_t want = WordAt(expected.ram, address), got = WordAt(actual.ram, address);
            if (want == got) {
                continue;
            }
            if (index < DumpRegisterWords) {
                snprintf(line, sizeof line, "R%d     expected 0x%04X, got 0x%04X", index, want, got);
            }
            else {
                snprintf(line, sizeof line, "flag %s expected %d, got %d", DumpFlagNames[index - DumpRegisterWords], want, got);
            }
            differences.push_back(line);
        }
    }

    // Memory: the first few bytes that differ, then how many more.
    const int shown = 8;
    int differing = 0;
    for (size_t address = 0; address < expected.ram.size(); address++) {
        if ((dumped && InDumpArea(address)) || expected.ram[address] == actual.ram[address]) {
            continue;
        }
        if (++differing <= shown) {
            snprintf(line, sizeof line, "memory 0x%04zX: expected 0x%02X, got 0x%02X", address, expected.ram[address],
                     actual.ram[address]);
            differences.push_back(line);
        }
    }
    if (differing > shown) {
        snprintf(line, sizeof line, "memory: %d more bytes differ", differing - shown);
        differences.push_back(line);
    }

    if (expected.output != actual.output) {
        size_t at = 0;
        while (at < expected.output.size() && at < actual.output.size() && expected.output[at] == actual.output[at]) {
            at++;
        }
        snprintf(line, sizeof line, "output differs from byte %zu (expected %zu bytes, got %zu)", at, expected.output.size(),
                 actual.output.size());
        differences.push_back(line);
        differences.push_back("  expected \"" + Printable(expected.output) + "\"");
        differences.push_back("  got      \"" + Printable(actual.output) + "\"");
    }
    return differences;
}

std::string DescribeDump(const MachineState& state) {
    std::string out;
    char item[32];
    for (int index = 0; index < DumpRegisterWords + DumpFlagWords; index++) {
        uint16_t word = WordAt(state.ram, Address::DumpArea + 2 * static_cast<size_t>(index));
        if (index < DumpRegisterWords) {
            snprintf(item, sizeof item, "%sR%d=0x%04X", index ? " " : "", index, word);
        }
        else {
            snprintf(item, sizeof item, " %s=%d", DumpFlagNames[index - DumpRegisterWords], word);
        }
        out += item;
    }
    return out;
}

}  // namespace Kestrel
