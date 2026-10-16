// HardwareCheck.cpp -- --check-hardware: the machine's components against the emulator's behaviour.

#include "Checks.h"
#include "Circuit.h"
#include "ComponentFile.h"
#include "Simulation.h"
#include "Kestrel/Alu.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <utility>

namespace Kestrel {

struct AluResult {
    uint16_t y;
    int carry, overflow;  // overflow is only checked for the adder operations
};

// The ALU 16 operations, as the emulator computes them (Source/Kestrel/Alu.h). Carry in starts out
// in C, which the logic operations leave alone, as the hardware passes it through.
static AluResult ReferenceAlu(int op, uint16_t a, uint16_t b, int carryIn)
{
    uint8_t flags = carryIn ? Flag::Carry : 0;
    auto result = [&](uint16_t y) { return AluResult{y, (flags & Flag::Carry) ? 1 : 0, (flags & Flag::Overflow) ? 1 : 0}; };
    switch (static_cast<AluOperation>(op)) {
    case AluOperation::Add:   return result(AluAdd(a, b, carryIn, flags));
    case AluOperation::Sub:   return result(AluSubtract(a, b, carryIn, flags));
    case AluOperation::And:   return result(AluLogic(a & b, flags));
    case AluOperation::Or:    return result(AluLogic(a | b, flags));
    case AluOperation::Xor:   return result(AluLogic(a ^ b, flags));
    // Shifts move one bit; the control unit repeats them (b & 15) times.
    case AluOperation::Shl:   return result(AluShift(ShiftKind::Left, a, 1, flags));
    case AluOperation::Shr:   return result(AluShift(ShiftKind::RightLogical, a, 1, flags));
    case AluOperation::Sar:   return result(AluShift(ShiftKind::RightArithmetic, a, 1, flags));
    case AluOperation::Not:   return result(AluLogic(static_cast<uint16_t>(~a), flags));
    case AluOperation::Neg:   return result(AluSubtract(0, a, carryIn, flags));
    case AluOperation::PassB: return {b, carryIn, 0};
    case AluOperation::Sxb:   return {SignExtendByte(a), -1, 0};  // C: don't care
    default:                  return {a, carryIn, 0};             // PassA, and 13-15
    }
}

int RunHardwareCheck()
{
    std::vector<std::string> problems = LoadLibrary();
    for (const std::string& problem : problems) printf("  %s\n", problem.c_str());
    Check(problems.empty(), "the components load");

    if (Component* alu = FindComponent("ALU 16")) {
        Simulation simulation;
        BuildSimulation(simulation, *alu);
        printf("ALU 16: %zu gates, %d nets\n", simulation.gates.size(), simulation.netCount);
        Check(simulation.missingComponents == 0, "ALU 16 has all its parts");

        std::vector<uint16_t> values = {0, 1, 2, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0x8001, 0xFFFE, 0xFFFF};
        uint32_t seed = 12345;
        auto random = [&] { return seed = seed * 1103515245u + 12345u, static_cast<uint16_t>(seed >> 8); };
        for (int i = 0; i < 40; i++) values.push_back(random());
        for (int shift = 0; shift < 16; shift++) values.push_back(static_cast<uint16_t>(0x9000 + shift));

        int failures = 0, vectors = 0, slowest = 0;
        for (int op = 0; op < 16; op++) {
            for (uint16_t a : values) {
                for (uint16_t b : values) {
                    for (int carryIn = 0; carryIn < 2; carryIn++) {
                        SetBus(simulation, *alu, "A", 16, a);
                        SetBus(simulation, *alu, "B", 16, b);
                        SetBus(simulation, *alu, "OP", 4, op);
                        SetBus(simulation, *alu, "Cin", 1, carryIn);
                        RecordSettle(simulation);
                        slowest = std::max(slowest, simulation.lastSettleSteps);
                        vectors++;

                        AluResult expected = ReferenceAlu(op, a, b, carryIn);
                        uint32_t y = ReadBus(simulation, *alu, "Y", 16);
                        int carry = ReadOutput(simulation, *alu, "C"), overflow = ReadOutput(simulation, *alu, "V");
                        int zero = ReadOutput(simulation, *alu, "Z"), negative = ReadOutput(simulation, *alu, "N");
                        AluOperation operation = static_cast<AluOperation>(op);
                        bool adder = operation == AluOperation::Add || operation == AluOperation::Sub || operation == AluOperation::Neg;
                        if (y != expected.y || (expected.carry >= 0 && carry != expected.carry) || (adder && overflow != expected.overflow) ||
                            zero != (expected.y == 0) || negative != (expected.y >> 15)) {
                            if (failures++ < 10)
                                printf("  ALU op %d a=%04X b=%04X cin=%d: got Y=%04X C%d V%d Z%d N%d, want Y=%04X C%d V%d\n",
                                       op, a, b, carryIn, y, carry, overflow, zero, negative, expected.y,
                                       expected.carry, expected.overflow);
                        }
                    }
                }
            }
        }
        printf("ALU 16: %d vectors, slowest settle %d steps\n", vectors, slowest);
        Check(failures == 0, "ALU 16 matches the emulator");

        // A shift by n is n one-bit shifts, Y fed back to A, the last carry ending up in C.
        int shiftFailures = 0;
        const std::pair<AluOperation, ShiftKind> shifts[] = {
            {AluOperation::Shl, ShiftKind::Left}, {AluOperation::Shr, ShiftKind::RightLogical},
            {AluOperation::Sar, ShiftKind::RightArithmetic}};
        for (auto [operation, kind] : shifts) {
            int op = static_cast<int>(operation);
            for (uint16_t value : values) {
                for (int amount = 1; amount < 16; amount++) {
                    uint16_t a = value;
                    int carry = 0;
                    for (int step = 0; step < amount; step++) {
                        SetBus(simulation, *alu, "A", 16, a);
                        SetBus(simulation, *alu, "OP", 4, op);
                        RecordSettle(simulation);
                        a = static_cast<uint16_t>(ReadBus(simulation, *alu, "Y", 16));
                        carry = ReadOutput(simulation, *alu, "C");
                    }
                    uint8_t flags = 0;
                    uint16_t want = AluShift(kind, value, static_cast<unsigned>(amount), flags);
                    int wantCarry = (flags & Flag::Carry) ? 1 : 0;
                    if ((a != want || carry != wantCarry) && shiftFailures++ < 10)
                        printf("  shift op %d %04X by %d: got %04X C%d, want %04X C%d\n", op, value, amount, a,
                               carry, want, wantCarry);
                }
            }
        }
        Check(shiftFailures == 0, "repeated one-bit shifts match the emulator's shifts");
    } else {
        Check(false, "there is an ALU 16 component");
    }

    if (Component* file = FindComponent("Register File")) {
        Simulation simulation;
        BuildSimulation(simulation, *file);
        printf("Register File: %zu gates, %d nets\n", simulation.gates.size(), simulation.netCount);
        Check(simulation.missingComponents == 0, "Register File has all its parts");

        uint16_t registers[8];
        uint32_t seed = 777;
        auto random = [&] { return seed = seed * 1103515245u + 12345u, static_cast<uint16_t>(seed >> 8); };
        auto clock = [&] {
            SetBus(simulation, *file, "CLK", 1, 1);
            RecordSettle(simulation);
            SetBus(simulation, *file, "CLK", 1, 0);
            RecordSettle(simulation);
        };
        auto write = [&](int r, uint16_t value, int enable) {
            SetBus(simulation, *file, "W", 3, r);
            SetBus(simulation, *file, "D", 16, value);
            SetBus(simulation, *file, "WE", 1, enable);
            RecordSettle(simulation);
            clock();
            if (enable) registers[r] = value;
        };
        for (int r = 0; r < 8; r++) write(r, random(), 1);

        int failures = 0;
        for (int i = 0; i < 2000; i++) {
            write(random() & 7, random(), (random() & 3) != 0);  // a quarter of the cycles don't write
            int ra = random() & 7, rb = random() & 7;
            SetBus(simulation, *file, "RA", 3, ra);
            SetBus(simulation, *file, "RB", 3, rb);
            RecordSettle(simulation);
            uint32_t qa = ReadBus(simulation, *file, "QA", 16), qb = ReadBus(simulation, *file, "QB", 16);
            if ((qa != registers[ra] || qb != registers[rb]) && failures++ < 10)
                printf("  R%d=%04X R%d=%04X: read %04X %04X\n", ra, registers[ra], rb, registers[rb], qa, qb);
        }
        Check(failures == 0, "Register File keeps R0-R7 like the emulator");
        Check(simulation.conflicts == 0, "Register File read ports never conflict");
    } else {
        Check(false, "there is a Register File component");
    }

    // The whole machine runs Fibonacci.bin and prints what the emulator prints.
    if (Component* computer = FindComponent("Kestrel16 Computer")) {
        Simulation simulation;
        BuildSimulation(simulation, *computer);
        printf("Kestrel16 Computer: %zu gates, %d nets\n", simulation.gates.size(), simulation.netCount);
        Check(simulation.missingComponents == 0, "Kestrel16 Computer has all its parts");
        auto start = std::chrono::steady_clock::now();
        int cycles = 0, slowest = 0;
        while (cycles < 200000 && !ReadOutput(simulation, *computer, "HALTED") && !ReadOutput(simulation, *computer, "ILLEGAL")) {
            for (int edge = 0; edge < 2; edge++) {
                simulation.clock ^= 1;
                RecordSettle(simulation);
                slowest = std::max(slowest, simulation.lastSettleSteps);
            }
            cycles++;
        }
        double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        printf("Kestrel16 Computer: %d cycles in %.1f s, slowest settle %d steps, %d conflicting nets%s\n", cycles, seconds,
               slowest, simulation.conflicts, simulation.unstable ? ", UNSTABLE" : "");
        printf("--- terminal ---\n%s\n----------------\n", simulation.console.c_str());
        static const char* const Expected =
            "Fibonacci numbers that fit in 16 bits:\n0 1 1 2 3 5 8 13 21 34 55 89 144 233 377 610 987 1597 2584 "
            "4181 6765 10946 17711 28657 46368\ntimer interrupts: 109\n";
        Check(!ReadOutput(simulation, *computer, "ILLEGAL"), "Kestrel16 Computer doesn't hit an illegal instruction");
        Check(simulation.console == Expected, "Kestrel16 Computer runs Fibonacci.bin like the emulator");
    } else {
        Check(false, "there is an Kestrel16 Computer component");
    }

    if (testFailures) {
        printf("%d check(s) failed\n", testFailures);
        return 1;
    }
    printf("All hardware checks passed\n");
    return 0;
}

}  // namespace Kestrel
