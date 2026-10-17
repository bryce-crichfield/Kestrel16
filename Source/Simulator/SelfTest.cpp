// SelfTest.cpp -- --self-test: builds small circuits and checks the simulator and the file format.

#include "Checks.h"
#include "Circuit.h"
#include "ComponentFile.h"
#include "Simulation.h"

#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

namespace Kestrel {

int testFailures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        printf("  FAIL: %s\n", what);
        testFailures++;
    }
}

static void TestFullAdder(const char* heading) {
    Component* fullAdder = FindComponent("Full Adder");
    Simulation simulation;
    BuildSimulation(simulation, *fullAdder);
    printf("%s: %zu gates, %d nets\n", heading, simulation.gates.size(), simulation.netCount);
    for (int value = 0; value < 8; value++) {
        int a = value & 1, b = (value >> 1) & 1, carryIn = (value >> 2) & 1;
        SetInput(simulation, *fullAdder, "A", a);
        SetInput(simulation, *fullAdder, "B", b);
        SetInput(simulation, *fullAdder, "Cin", carryIn);
        int total = a + b + carryIn;
        Check(ReadOutput(simulation, *fullAdder, "S") == (total & 1), "full adder sum");
        Check(ReadOutput(simulation, *fullAdder, "Cout") == (total >> 1), "full adder carry");
    }
}

int RunSelfTest() {
    // Half adder: S = A xor B, C = A and B
    Component* halfAdder = CreateComponent("Half Adder");
    {
        uint32_t a = AddNode(*halfAdder, NodeKind::Input, "", "A");
        uint32_t b = AddNode(*halfAdder, NodeKind::Input, "", "B");
        uint32_t xorGate = AddNode(*halfAdder, NodeKind::Xor);
        uint32_t andGate = AddNode(*halfAdder, NodeKind::And);
        uint32_t sum = AddNode(*halfAdder, NodeKind::Output, "", "S");
        uint32_t carry = AddNode(*halfAdder, NodeKind::Output, "", "C");
        Connect(*halfAdder, a, 0, xorGate, 0);
        Connect(*halfAdder, b, 0, xorGate, 1);
        Connect(*halfAdder, a, 0, andGate, 0);
        Connect(*halfAdder, b, 0, andGate, 1);
        Connect(*halfAdder, xorGate, 0, sum, 0);
        Connect(*halfAdder, andGate, 0, carry, 0);
    }

    // Full adder from two half adders: tests nesting.
    Component* fullAdder = CreateComponent("Full Adder");
    {
        uint32_t a = AddNode(*fullAdder, NodeKind::Input, "", "A");
        uint32_t b = AddNode(*fullAdder, NodeKind::Input, "", "B");
        uint32_t carryIn = AddNode(*fullAdder, NodeKind::Input, "", "Cin");
        uint32_t first = AddNode(*fullAdder, NodeKind::Component, "Half Adder");
        uint32_t second = AddNode(*fullAdder, NodeKind::Component, "Half Adder");
        uint32_t orGate = AddNode(*fullAdder, NodeKind::Or);
        uint32_t sum = AddNode(*fullAdder, NodeKind::Output, "", "S");
        uint32_t carryOut = AddNode(*fullAdder, NodeKind::Output, "", "Cout");
        Connect(*fullAdder, a, 0, first, 0);
        Connect(*fullAdder, b, 0, first, 1);
        Connect(*fullAdder, first, 0, second, 0);  // S of the first
        Connect(*fullAdder, carryIn, 0, second, 1);
        Connect(*fullAdder, first, 1, orGate, 0);  // C of the first
        Connect(*fullAdder, second, 1, orGate, 1);
        Connect(*fullAdder, second, 0, sum, 0);
        Connect(*fullAdder, orGate, 0, carryOut, 0);
    }

    {
        Simulation simulation;
        BuildSimulation(simulation, *halfAdder);
        printf("Half adder: %zu gates, %d nets\n", simulation.gates.size(), simulation.netCount);
        for (int value = 0; value < 4; value++) {
            int a = value & 1, b = value >> 1;
            SetInput(simulation, *halfAdder, "A", a);
            SetInput(simulation, *halfAdder, "B", b);
            Check(ReadOutput(simulation, *halfAdder, "S") == (a ^ b), "half adder sum");
            Check(ReadOutput(simulation, *halfAdder, "C") == (a & b), "half adder carry");
        }
    }

    TestFullAdder("Full adder");

    // SR latch from two cross-coupled NANDs: tests feedback.
    Component* latch = CreateComponent("SR Latch");
    {
        uint32_t set = AddNode(*latch, NodeKind::Input, "", "/S");
        uint32_t reset = AddNode(*latch, NodeKind::Input, "", "/R");
        uint32_t top = AddNode(*latch, NodeKind::Nand);
        uint32_t bottom = AddNode(*latch, NodeKind::Nand);
        uint32_t q = AddNode(*latch, NodeKind::Output, "", "Q");
        Connect(*latch, set, 0, top, 0);
        Connect(*latch, bottom, 0, top, 1);
        Connect(*latch, reset, 0, bottom, 0);
        Connect(*latch, top, 0, bottom, 1);
        Connect(*latch, top, 0, q, 0);

        Simulation simulation;
        BuildSimulation(simulation, *latch);
        printf("SR latch: %zu gates\n", simulation.gates.size());
        SetInput(simulation, *latch, "/S", 1);
        SetInput(simulation, *latch, "/R", 0);  // reset
        SetInput(simulation, *latch, "/R", 1);  // hold
        Check(ReadOutput(simulation, *latch, "Q") == 0, "latch holds 0 after reset");
        SetInput(simulation, *latch, "/S", 0);  // set
        SetInput(simulation, *latch, "/S", 1);  // hold
        Check(ReadOutput(simulation, *latch, "Q") == 1, "latch holds 1 after set");
        Check(!simulation.unstable, "latch settles");
    }

    // 2-bit ripple counter: each flip-flop's /Q feeds its own D, so it toggles.
    // Bit 0 is clocked by the clock, bit 1 by bit 0's /Q (which rises when Q0 falls).
    Component* counter = CreateComponent("Counter");
    {
        uint32_t clock = AddNode(*counter, NodeKind::Clock);
        uint32_t bit0 = AddNode(*counter, NodeKind::FlipFlop);
        uint32_t bit1 = AddNode(*counter, NodeKind::FlipFlop);
        uint32_t q0 = AddNode(*counter, NodeKind::Output, "", "Q0");
        uint32_t q1 = AddNode(*counter, NodeKind::Output, "", "Q1");
        Connect(*counter, bit0, 1, bit0, 0);
        Connect(*counter, clock, 0, bit0, 1);
        Connect(*counter, bit1, 1, bit1, 0);
        Connect(*counter, bit0, 1, bit1, 1);
        Connect(*counter, bit0, 0, q0, 0);
        Connect(*counter, bit1, 0, q1, 0);

        Simulation simulation;
        BuildSimulation(simulation, *counter);
        bool counted = true;
        for (int cycle = 1; cycle <= 8; cycle++) {
            HalfStep(simulation);  // rising edge
            HalfStep(simulation);  // falling edge
            int value = ReadOutput(simulation, *counter, "Q0") | (ReadOutput(simulation, *counter, "Q1") << 1);
            if (value != (cycle & 3))
                counted = false;
        }
        Check(counted, "ripple counter counts 1, 2, 3, 0, ...");
        printf("Counter: slowest path %d gate delays\n", simulation.slowestSettleSteps);
    }

    // A NOT gate feeding itself never settles.
    Component* ring = CreateComponent("Ring");
    {
        uint32_t inverter = AddNode(*ring, NodeKind::Not);
        Connect(*ring, inverter, 0, inverter, 0);
        Simulation simulation;
        BuildSimulation(simulation, *ring);
        Check(simulation.unstable, "oscillator is reported unstable");
    }

    // A shift register written by hand in YAML, the way a chip would be described from its
    // datasheet: no positions, wires by pin name.
    {
        static const char* const ShiftRegisterYaml = R"(
name: Shift Register 4
description: 4-bit serial in, parallel out. Q0 takes D on each rising CLK; the rest shift along.
nodes:
  - {id: 1, kind: Input, label: D}
  - {id: 2, kind: Input, label: CLK}
  - {id: 3, kind: FlipFlop}
  - {id: 4, kind: FlipFlop}
  - {id: 5, kind: FlipFlop}
  - {id: 6, kind: FlipFlop}
  - {id: 7, kind: Output, label: Q0}
  - {id: 8, kind: Output, label: Q1}
  - {id: 9, kind: Output, label: Q2}
  - {id: 10, kind: Output, label: Q3}
wires:
  - 1 -> 3.D
  - 3.Q -> 4.D
  - 4.Q -> 5.D
  - 5.Q -> 6.D
  - 2 -> 3.CLK
  - 2 -> 4.CLK
  - 2 -> 5.CLK
  - 2 -> 6.CLK
  - 3.Q -> 7
  - 4.Q -> 8
  - 5.Q -> 9
  - 6.0 -> 10
)";
        std::unique_ptr<Component> parsed;
        try {
            parsed = ComponentFromYaml(ShiftRegisterYaml);
        } catch (const std::exception& exception) {
            printf("  shift register YAML: %s\n", exception.what());
        }
        Check(parsed != nullptr, "shift register YAML parses");
        if (parsed) {
            library.push_back(std::move(parsed));
            Component& shifter = *library.back();
            Check(ResolveWires(shifter).empty() && shifter.wires.size() == 12, "shift register wires resolve");

            Simulation simulation;
            BuildSimulation(simulation, shifter);
            auto clockIn = [&](int bit) {
                SetInput(simulation, shifter, "D", bit);
                SetInput(simulation, shifter, "CLK", 1);
                SetInput(simulation, shifter, "CLK", 0);
            };
            auto read = [&] {
                int value = 0;
                for (int bit = 0; bit < 4; bit++)
                    value |= ReadOutput(simulation, shifter, ("Q" + std::to_string(bit)).c_str()) << bit;
                return value;
            };
            // Shift in 1, 0, 1, 1: the first bit in ends up in Q3.
            for (int bit : {1, 0, 1, 1})
                clockIn(bit);
            Check(read() == 0b1011, "shift register holds 1,0,1,1 (Q3..Q0)");
            clockIn(0);
            Check(read() == 0b0110, "shift register shifts once more");
            printf("Shift register (from YAML): %zu gates\n", simulation.gates.size());

            // Saving and parsing again must give the same text.
            std::string text = ComponentToYaml(shifter);
            std::unique_ptr<Component> again = ComponentFromYaml(text);
            ResolveWires(*again);
            Check(ComponentToYaml(*again) == text, "shift register YAML round-trips exactly");
            shifter.dirty = true;  // include it in the save and reload below
        }
    }

    // Buses: bus Input/Output nodes, range wires, and side-by-side wires merging into one.
    {
        static const char* const PassYaml = R"(
name: Bus Pass
nodes:
  - {id: 1, kind: Input, label: A, width: 4}
  - {id: 2, kind: Output, label: Y, width: 4}
wires:
  - 1 -> 2
)";
        static const char* const UseYaml = R"(
name: Bus Use
nodes:
  - {id: 1, kind: Input, label: X, width: 4}
  - {id: 3, component: Bus Pass}
  - {id: 5, kind: Output, label: Q, width: 2}
  - {id: 6, kind: Output, label: Z, width: 2}
  - {id: 7, kind: Output, label: TOP}
wires:
  - 1 -> 3.A0..A3
  - 3.Y2..Y3 -> 5
  - 3.Y0 -> 6.Z0
  - 3.Y1 -> 6.Z1
  - 3.Y3 -> 7
)";
        std::unique_ptr<Component> pass, use;
        try {
            pass = ComponentFromYaml(PassYaml);
            use = ComponentFromYaml(UseYaml);
        } catch (const std::exception& exception) {
            printf("  bus YAML: %s\n", exception.what());
        }
        Check(pass && use, "bus YAML parses");
        if (pass && use) {
            library.push_back(std::move(pass));
            Component& passComponent = *library.back();
            library.push_back(std::move(use));
            Component& useComponent = *library.back();
            Check(ResolveWires(passComponent).empty() && passComponent.wires.size() == 1 &&
                      passComponent.wires[0].width == 4,
                  "a bare bus node id wires all its bits");
            Check(ResolveWires(useComponent).empty() && useComponent.wires.size() == 4,
                  "range wires resolve, and Y0 -> Z0, Y1 -> Z1 merge into one bus wire");
            Check(InputCount(*useComponent.FindNode(3)) == 4 && PinName(*useComponent.FindNode(3), true, 2) == "Y2",
                  "a bus Input gives its user pins A0..A3");

            Simulation simulation;
            BuildSimulation(simulation, useComponent);
            SetSwitch(simulation, PinNodeByLabel(useComponent, NodeKind::Input, "X"), 0b1011);
            Check(ReadBus(simulation, useComponent, "Q", 2) == 0b10 && ReadBus(simulation, useComponent, "Z", 2) == 0b11 &&
                      ReadOutput(simulation, useComponent, "TOP") == 1,
                  "bus wires carry every bit");

            std::string text = ComponentToYaml(useComponent);
            Check(text.find("3.Y0..Y1 -> 6") != std::string::npos && text.find("1 -> 3.A0..A3") != std::string::npos,
                  "bus wires save as ranges");
            std::unique_ptr<Component> again = ComponentFromYaml(text);
            ResolveWires(*again);
            Check(ComponentToYaml(*again) == text, "bus YAML round-trips exactly");
            passComponent.dirty = useComponent.dirty = true;
        }

        std::unique_ptr<Component> bad = ComponentFromYaml(
            "name: Bad Bus\nnodes:\n  - {id: 1, kind: Input, label: A, width: 4}\n"
            "  - {id: 2, kind: Output, label: Y, width: 3}\nwires:\n  - 1 -> 2\n");
        Check(ResolveWires(*bad).size() == 1, "a wire whose ends differ in width is reported");
    }

    // A ROM as a 2-to-4 decoder, the way an opcode decoder EEPROM would be set up.
    {
        static const char* const DecoderYaml = R"(
name: Decoder ROM
nodes:
  - {id: 1, kind: Input, label: S0}
  - {id: 2, kind: Input, label: S1}
  - id: 3
    kind: ROM
    address: 2
    data: 4
    outputs: [Y0, Y1, "", Y3]
    contents:
      - "0000: 1 2 4 8"
  - {id: 4, kind: Output, label: Y0}
  - {id: 5, kind: Output, label: Y1}
  - {id: 6, kind: Output, label: Y2}
  - {id: 7, kind: Output, label: Y3}
  - {id: 8, kind: Input, label: OE}
wires:
  - 8 -> 3./OE
  - 1 -> 3.A0
  - 2 -> 3.A1
  - 3.Y0 -> 4
  - 3.Y1 -> 5
  - 3.D2 -> 6
  - 3.Y3 -> 7
)";
        std::unique_ptr<Component> parsed;
        try {
            parsed = ComponentFromYaml(DecoderYaml);
        } catch (const std::exception& exception) {
            printf("  decoder YAML: %s\n", exception.what());
        }
        Check(parsed != nullptr, "ROM decoder YAML parses");
        if (parsed) {
            library.push_back(std::move(parsed));
            Component& decoder = *library.back();
            Check(ResolveWires(decoder).empty() && decoder.wires.size() == 7, "ROM decoder wires resolve by pin name");

            Simulation simulation;
            BuildSimulation(simulation, decoder);
            bool decodes = true;
            for (int select = 0; select < 4; select++) {
                SetInput(simulation, decoder, "S0", select & 1);
                SetInput(simulation, decoder, "S1", select >> 1);
                for (int line = 0; line < 4; line++)
                    if (ReadOutput(simulation, decoder, ("Y" + std::to_string(line)).c_str()) != (line == select))
                        decodes = false;
            }
            Check(decodes, "ROM decodes 2 bits to one of 4 lines");
            SetInput(simulation, decoder, "OE", 1);
            Check(ReadLevel(simulation, decoder, "Y3") == Floating && ReadLevel(simulation, decoder, "Y0") == Floating,
                  "ROM outputs let go (Z) when /OE is 1");
            SetInput(simulation, decoder, "OE", 0);

            // Edits to the contents show up in a running simulation.
            const Node* rom = decoder.FindNode(3);
            rom->memory->words[3] = 0x1;
            RecordSettle(simulation);
            Check(ReadOutput(simulation, decoder, "Y0") == 1 && ReadOutput(simulation, decoder, "Y3") == 0,
                  "ROM edits reach a running simulation");
            rom->memory->words[3] = 0x8;

            std::string text = ComponentToYaml(decoder);
            std::unique_ptr<Component> again = ComponentFromYaml(text);
            ResolveWires(*again);
            Check(ComponentToYaml(*again) == text, "ROM YAML round-trips exactly");
            decoder.dirty = true;
        }
    }

    // Two tri-state buffers sharing one wire: a two-device bus.
    Component* bus = CreateComponent("Bus");
    {
        uint32_t a = AddNode(*bus, NodeKind::Input, "", "A");
        uint32_t b = AddNode(*bus, NodeKind::Input, "", "B");
        uint32_t enableA = AddNode(*bus, NodeKind::Input, "", "/OEA");
        uint32_t enableB = AddNode(*bus, NodeKind::Input, "", "/OEB");
        uint32_t driverA = AddNode(*bus, NodeKind::TriState);
        uint32_t driverB = AddNode(*bus, NodeKind::TriState);
        uint32_t line = AddNode(*bus, NodeKind::Output, "", "BUS");
        Connect(*bus, a, 0, driverA, 0);
        Connect(*bus, enableA, 0, driverA, 1);
        Connect(*bus, b, 0, driverB, 0);
        Connect(*bus, enableB, 0, driverB, 1);
        Connect(*bus, driverA, 0, line, 0);
        Connect(*bus, driverB, 0, line, 0);  // a second wire into the same input
        Check(bus->wires.size() == 6, "an input keeps several wires");

        Simulation simulation;
        BuildSimulation(simulation, *bus);
        SetInput(simulation, *bus, "A", 1);
        SetInput(simulation, *bus, "B", 0);
        SetInput(simulation, *bus, "/OEA", 0);
        SetInput(simulation, *bus, "/OEB", 1);
        Check(ReadLevel(simulation, *bus, "BUS") == High && simulation.conflicts == 0, "bus carries A when A drives");
        SetInput(simulation, *bus, "/OEA", 1);
        SetInput(simulation, *bus, "/OEB", 0);
        Check(ReadLevel(simulation, *bus, "BUS") == Low, "bus carries B when B drives");
        SetInput(simulation, *bus, "/OEB", 1);
        Check(ReadLevel(simulation, *bus, "BUS") == Floating, "bus floats (Z) with nothing driving");
        SetInput(simulation, *bus, "/OEA", 0);
        SetInput(simulation, *bus, "/OEB", 0);
        Check(ReadLevel(simulation, *bus, "BUS") == Conflict && simulation.conflicts == 1,
              "two drivers disagreeing are a conflict");
        SetInput(simulation, *bus, "B", 1);
        Check(ReadLevel(simulation, *bus, "BUS") == High && simulation.conflicts == 0, "two drivers agreeing are fine");
    }

    // A RAM on a data bus: tri-state buffers drive DIN onto D when DRIVE is 0, and the RAM's
    // D pins (the same wires) come out to D0 and D1.
    {
        static const char* const RamYaml = R"(
name: RAM Test
nodes:
  - {id: 1, kind: Input, label: A0}
  - {id: 2, kind: Input, label: A1}
  - {id: 3, kind: Input, label: /CE}
  - {id: 4, kind: Input, label: /OE}
  - {id: 5, kind: Input, label: /WE}
  - {id: 6, kind: Input, label: DIN0}
  - {id: 7, kind: Input, label: DIN1}
  - {id: 8, kind: Input, label: DRIVE}
  - {id: 9, kind: TriState}
  - {id: 10, kind: TriState}
  - {id: 11, kind: RAM, address: 2, data: 2}
  - {id: 12, kind: Output, label: D0}
  - {id: 13, kind: Output, label: D1}
wires:
  - 1 -> 11.A0
  - 2 -> 11.A1
  - 3 -> 11./CE
  - 4 -> 11./OE
  - 5 -> 11./WE
  - 6 -> 9.A
  - 7 -> 10.A
  - 8 -> 9./OE
  - 8 -> 10./OE
  - 9.Y -> 11.D0
  - 10.Y -> 11.D1
  - 11.D0 -> 12
  - 11.D1 -> 13
)";
        std::unique_ptr<Component> parsed;
        try {
            parsed = ComponentFromYaml(RamYaml);
        } catch (const std::exception& exception) {
            printf("  RAM YAML: %s\n", exception.what());
        }
        Check(parsed != nullptr, "RAM YAML parses");
        if (parsed) {
            library.push_back(std::move(parsed));
            Component& ramTest = *library.back();
            std::vector<std::string> wireProblems = ResolveWires(ramTest);
            for (const std::string& problem : wireProblems)
                printf("  %s\n", problem.c_str());
            Check(wireProblems.empty() && ramTest.wires.size() == 13, "RAM wires resolve, D0 on both sides");

            Simulation simulation;  // randomRam is off: RAM starts as zeros
            BuildSimulation(simulation, ramTest);
            auto set = [&](const char* label, int value) { SetInput(simulation, ramTest, label, value); };
            auto level = [&](const char* label) { return ReadLevel(simulation, ramTest, label); };
            // RAM never writes while powering up, though /WE starts low here; it starts as zeros.
            set("/WE", 1);
            set("DRIVE", 1);
            Check(level("D0") == Low && level("D1") == Low, "RAM reads zeros at power-up");
            set("/OE", 1);

            // Write 2 (D1=1, D0=0) at address 1.
            set("A0", 1);
            set("DIN1", 1);
            set("DRIVE", 0);
            set("/WE", 0);
            set("/WE", 1);
            set("DRIVE", 1);
            Check(level("D0") == Floating && level("D1") == Floating, "D floats when nothing drives it");
            set("/OE", 0);
            Check(level("D0") == Low && level("D1") == High, "RAM reads back what was written");
            set("A0", 0);
            Check(level("D1") == Low, "other addresses are untouched");
            set("/CE", 1);
            Check(level("D0") == Floating && level("D1") == Floating, "deselected RAM lets go of D");
            set("/CE", 0);

            // /WE low beats /OE low: the RAM stops driving and stores what's on D (nothing: 0).
            set("A0", 1);
            set("/WE", 0);
            Check(level("D1") == Floating, "RAM doesn't drive D while writing, even with /OE low");
            set("/WE", 1);
            Check(level("D1") == Low, "writing a floating bus stores 0");

            // Reading while the buffers drive something else is a bus conflict.
            set("A0", 0);
            set("A1", 1);
            set("DIN1", 0);
            set("DIN0", 1);
            set("DRIVE", 0);
            set("/WE", 0);  // store 1 at address 2
            set("/WE", 1);  // now the RAM drives 01 alongside the buffers' 01: no conflict
            Check(simulation.conflicts == 0, "RAM and a buffer agreeing is fine");
            set("DIN0", 0);  // the buffers drive 00 while the RAM drives 01
            Check(simulation.conflicts == 1 && level("D0") == Conflict && level("D1") == Low,
                  "RAM and a buffer driving the same wire differently conflict");

            std::string text = ComponentToYaml(ramTest);
            std::unique_ptr<Component> again = ComponentFromYaml(text);
            ResolveWires(*again);
            Check(ComponentToYaml(*again) == text, "RAM YAML round-trips exactly");
            ramTest.dirty = true;
        }
    }

    // The Terminal on a data bus, driven through tri-state buffers like the RAM test.
    {
        Component terminalTest;
        uint32_t select = AddNode(terminalTest, NodeKind::Input, "", "RS");
        uint32_t chipEnable = AddNode(terminalTest, NodeKind::Input, "", "/CE");
        uint32_t outputEnable = AddNode(terminalTest, NodeKind::Input, "", "/OE");
        uint32_t writeEnable = AddNode(terminalTest, NodeKind::Input, "", "/WE");
        uint32_t drive = AddNode(terminalTest, NodeKind::Input, "", "/DRIVE");
        uint32_t terminal = AddNode(terminalTest, NodeKind::Terminal);
        Connect(terminalTest, select, 0, terminal, 0);
        Connect(terminalTest, chipEnable, 0, terminal, 1);
        Connect(terminalTest, outputEnable, 0, terminal, 2);
        Connect(terminalTest, writeEnable, 0, terminal, 3);
        for (int bit = 0; bit < 8; bit++) {
            uint32_t in = AddNode(terminalTest, NodeKind::Input, "", "DIN" + std::to_string(bit));
            uint32_t buffer = AddNode(terminalTest, NodeKind::TriState);
            uint32_t out = AddNode(terminalTest, NodeKind::Output, "", "D" + std::to_string(bit));
            Connect(terminalTest, in, 0, buffer, 0);
            Connect(terminalTest, drive, 0, buffer, 1);
            Connect(terminalTest, buffer, 0, terminal, TerminalControlPins + bit);
            Connect(terminalTest, terminal, bit, out, 0);
        }
        Connect(terminalTest, terminal, TerminalDataBits, AddNode(terminalTest, NodeKind::Output, "", "IRQ"), 0);

        Simulation simulation;
        BuildSimulation(simulation, terminalTest);
        auto set = [&](const char* label, int value) { SetInput(simulation, terminalTest, label, value); };
        auto setByte = [&](int byte) {
            for (int bit = 0; bit < 8; bit++)
                set(("DIN" + std::to_string(bit)).c_str(), (byte >> bit) & 1);
        };
        auto readByte = [&] {
            int byte = 0;
            for (int bit = 0; bit < 8; bit++)
                byte |= ReadOutput(simulation, terminalTest, ("D" + std::to_string(bit)).c_str()) << bit;
            return byte;
        };
        // Every switch starts at 0, which is a write cycle in progress: end it, then start clean.
        set("/WE", 1);
        set("/OE", 1);
        simulation.console.clear();

        for (char character : {'H', 'i'}) {
            setByte(character);
            set("/WE", 0);
            set("/WE", 1);
        }
        Check(simulation.console == "Hi", "terminal prints each byte written to RS=0 once");
        set("RS", 1);
        set("/WE", 0);
        set("/WE", 1);
        Check(simulation.console == "Hi", "writing RS=1 (status) prints nothing");

        set("/DRIVE", 1);  // the buffers let go so the terminal can drive D
        simulation.keys = {'a', 'b'};
        RecordSettle(simulation);
        Check(ReadOutput(simulation, terminalTest, "IRQ") == 1, "IRQ is 1 while a key is waiting");
        set("/OE", 0);
        Check(readByte() == 0x03, "status: key waiting, ready to print");
        set("/OE", 1);
        set("RS", 0);
        set("/OE", 0);
        bool first = readByte() == 'a';
        set("/OE", 1);
        set("/OE", 0);
        Check(first && readByte() == 'b', "reads take typed keys in order, one per read");
        set("/OE", 1);
        Check(simulation.keys.empty() && ReadOutput(simulation, terminalTest, "IRQ") == 0, "IRQ drops once keys are read");
        set("RS", 1);
        set("/OE", 0);
        Check(readByte() == 0x02, "status: nothing waiting");
        set("/CE", 1);
        Check(ReadLevel(simulation, terminalTest, "D0") == Floating, "deselected terminal lets go of D");
    }

    // Recursion guard.
    Check(Uses(*fullAdder, "Half Adder"), "full adder uses half adder");
    Check(!Uses(*halfAdder, "Full Adder"), "half adder doesn't use full adder");

    // Save, reload and test again. Also renames a used component on the way.
    componentsDirectory = (fs::temp_directory_path() / "kestrel16-selftest/").string();
    std::error_code error;
    fs::remove_all(componentsDirectory, error);
    RenameComponent(*halfAdder, "HA");
    Check(SaveAll() == 0, "save all");
    library.clear();
    std::vector<std::string> problems = LoadLibrary();
    for (const std::string& problem : problems)
        printf("  %s\n", problem.c_str());
    Check(problems.empty() && library.size() == 11, "reload all eleven components");
    Check(FindComponent("HA") && FindComponent("Full Adder"), "renamed reference survives the round trip");
    if (FindComponent("Full Adder"))
        TestFullAdder("Full adder (reloaded)");
    if (Component* decoder = FindComponent("Decoder ROM")) {
        const Node* rom = decoder->FindNode(3);
        Check(rom && rom->memory->words == std::vector<uint32_t>({1, 2, 4, 8}) && PinName(*rom, true, 3) == "Y3",
              "ROM contents and pin names survive the round trip");
    }

    // A ROM can take its contents from a binary file, such as an assembled program.
    {
        MemoryData image;
        image.Resize(2, 16);
        image.words = {0x1234, 0xBEEF, 0, 7};
        Check(SaveRomImage(image, componentsDirectory + "program.bin"), "write a ROM image");
        MemoryData linked;
        linked.Resize(1, 16);
        linked.image = "program.bin";
        std::string problem = LoadRomImage(linked);
        Check(linked.words == std::vector<uint32_t>({0x1234, 0xBEEF}) && !problem.empty(),
              "ROM image loads little-endian words, and says when it doesn't all fit");

        // The same file preloads a RAM when its simulation starts.
        Component machine;
        uint32_t ramNode = AddNode(machine, NodeKind::Ram);
        MemoryData& ram = *machine.FindNode(ramNode)->memory;
        ram.Resize(2, 16);
        Connect(machine, AddNode(machine, NodeKind::High), 0, ramNode, 2 + 2);  // /WE high: never write
        ram.image = "program.bin";
        Check(LoadRomImage(ram).empty(), "RAM preload image loads");
        Simulation simulation;
        simulation.randomRam = true;  // preloaded words win over random ones
        BuildSimulation(simulation, machine);
        Check(simulation.rams.size() == 1 && simulation.rams[0].words == std::vector<uint32_t>({0x1234, 0xBEEF, 0, 7}),
              "RAM starts out holding its preload image");
    }
    fs::remove_all(componentsDirectory, error);

    printf(testFailures ? "%d check(s) FAILED\n" : "All checks passed\n", testFailures);
    return testFailures ? 1 : 0;
}

}  // namespace Kestrel
