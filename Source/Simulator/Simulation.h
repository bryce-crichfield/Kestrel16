// Simulation.h -- running a component gate by gate.
//
// The component being simulated is flattened: every nested component is expanded until only
// primitives are left, and connected pins are merged into nets.
//
// Timing is "unit delay": in each step every gate looks at its inputs as they were at the end
// of the previous step, and all gates update together. So one step is one gate delay, and the
// number of steps a circuit needs to settle after the clock ticks is the length of its slowest
// path. Feedback is allowed (latches work); a circuit that never settles is reported as
// unstable.
//
// A net is 0, 1, Z (nothing drives it: every driver is off, as a tri-state output is when
// disabled) or X (drivers disagree: a bus conflict, which would short two chips in real life).
// A gate reading Z or X sees 0, as does an unconnected input.
//
// D flip-flops copy D to Q when their CLK input goes from 0 to 1. Because all gates update
// together, every flip-flop clocked by the same edge samples its D input at the same moment, as
// in real synchronous logic.

#pragma once

#include "Circuit.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Kestrel {

enum class GateKind : uint8_t { Switch, Low, High, Clock, Buffer, Not, And, Or, Nand, Nor, Xor, Xnor, FlipFlop, TriState };

struct Gate {
    GateKind kind;
    int inputs[2] = {-1, -1};   // net ids (endpoint ids while flattening)
    int outputs[2] = {-1, -1};
    uint8_t state = 0;          // switch position, or flip-flop contents
    uint8_t lastClock = 0;      // flip-flop: CLK as of the previous step
};

// A ROM in the flattened circuit: reads its address nets, drives its data nets.
struct RomUnit {
    std::shared_ptr<const MemoryData> data;
    std::vector<int> address, outputs;  // net ids (endpoint ids while flattening)
    int chipEnable = -1, outputEnable = -1;
};

// A RAM in the flattened circuit. Its contents belong to the simulation.
struct RamUnit {
    std::shared_ptr<const MemoryData> memory;
    std::vector<int> address, data;  // net ids; data nets are both read and driven
    int chipEnable = -1, outputEnable = -1, writeEnable = -1;
    std::vector<uint32_t> words;
};

// A Terminal in the flattened circuit. Its screen and keyboard are the simulation's console and keys.
struct TerminalUnit {
    int registerSelect = -1, chipEnable = -1, outputEnable = -1, writeEnable = -1, irq = -1;
    std::vector<int> data;  // net ids; read when written, driven when read
    bool writing = false, reading = false;  // in the middle of a write or read cycle
    int writeRegister = 0, readRegister = 0;
    uint8_t writeByte = 0;
};

constexpr int MaxSettleSteps = 1000;

// Net levels. Only High counts as 1 to whatever reads the net.
constexpr uint8_t Low = 0, High = 1, Floating = 2, Conflict = 3;

struct Simulation {
    std::vector<Gate> gates;
    std::vector<RomUnit> roms;
    std::vector<RamUnit> rams;
    std::vector<TerminalUnit> terminals;
    std::string console;       // what the terminals have printed
    std::deque<uint8_t> keys;  // typed, not yet read
    bool keysWanted = false;   // a status read found no key waiting (headless runs read stdin then)
    std::vector<uint8_t> signals, nextSignals;
    int netCount = 0;

    // The edited component's own nodes, for drawing and inspecting: pin -> net.
    std::unordered_map<uint32_t, std::vector<int>> nodeInputNets, nodeOutputNets;
    std::unordered_map<uint32_t, std::vector<int>> switchGates;  // Input node -> its switch gates, bit 0 first
    std::unordered_map<uint32_t, int> topRams;      // RAM node -> its unit in rams

    uint8_t clock = 0;
    uint64_t halfCycles = 0;
    int lastSettleSteps = 0;
    int slowestSettleSteps = 0;
    int conflicts = 0;  // nets driven to 0 and 1 at once, after the last settle
    bool unstable = false;
    int missingComponents = 0;
    bool nestingTooDeep = false;

    bool running = false;
    float frequency = 2.0f;  // Hz
    bool randomRam = false;  // RAM powers up holding random bytes, as real RAM does, rather than zeros
    double pendingHalfCycles = 0;

    int NetOfOutput(uint32_t node, int pin) const
    {
        auto found = nodeOutputNets.find(node);
        return found != nodeOutputNets.end() && pin < static_cast<int>(found->second.size()) ? found->second[pin] : -1;
    }
    int NetOfInput(uint32_t node, int pin) const
    {
        auto found = nodeInputNets.find(node);
        return found != nodeInputNets.end() && pin < static_cast<int>(found->second.size()) ? found->second[pin] : -1;
    }
    int Value(int net) const { return net >= 0 && signals[net] == High; }
    uint8_t Level(int net) const { return net >= 0 ? signals[net] : Floating; }
};

// Flattens top into simulation and powers it up (keeping the run settings).
void BuildSimulation(Simulation& simulation, const Component& top);
void RecordSettle(Simulation& simulation);  // lets everything settle after a change
void HalfStep(Simulation& simulation);      // the clock flips, and everything settles

// The top component's Input switches.
void SetSwitchBits(Simulation& simulation, uint32_t inputNode, uint32_t value);  // without settling
void SetSwitch(Simulation& simulation, uint32_t inputNode, uint32_t value);
uint32_t SwitchValue(const Simulation& simulation, uint32_t inputNode);

// Driving and reading a component's pins by label, for tests and headless runs.
uint32_t PinNodeByLabel(const Component& component, NodeKind side, const char* label);
void SetInput(Simulation& simulation, const Component& component, const char* label, int value);
int ReadOutput(const Simulation& simulation, const Component& component, const char* label);
uint8_t ReadLevel(const Simulation& simulation, const Component& component, const char* label);
// Sets several inputs at once without settling: "A" with 16 bits sets the bus Input A, or else
// the Inputs A0..A15.
void SetBus(Simulation& simulation, const Component& component, const char* name, int bits, uint32_t value);
uint32_t ReadBus(const Simulation& simulation, const Component& component, const char* name, int bits);

}  // namespace Kestrel
