// Simulation.cpp -- flattening a component into gates and nets, and running them.

#include "Simulation.h"
#include <algorithm>
#include <random>

namespace Kestrel {

namespace {

// Expands a component into primitive gates. Pins are "endpoints"; connecting two
// endpoints merges them (union-find), and each merged group becomes one net.
class Flattener {
   public:
    std::vector<Gate> gates;
    std::vector<RomUnit> roms;
    std::vector<RamUnit> rams;
    std::vector<TerminalUnit> terminals;
    std::unordered_map<uint32_t, int> topRams;
    std::unordered_map<uint32_t, std::vector<int>> topInputs, topOutputs;
    std::unordered_map<uint32_t, std::vector<int>> topSwitches;
    int missingComponents = 0;
    bool nestingTooDeep = false;

    int NewEndpoint() {
        parent.push_back(static_cast<int>(parent.size()));
        return static_cast<int>(parent.size()) - 1;
    }

    int Find(int endpoint) {
        while (parent[endpoint] != endpoint) {
            parent[endpoint] = parent[parent[endpoint]];
            endpoint = parent[endpoint];
        }
        return endpoint;
    }

    void Merge(int a, int b) { parent[Find(a)] = Find(b); }

    // boundaryInputs/Outputs are the endpoints of the pins on the instance being expanded.
    void Expand(const Component& component, const std::vector<int>& boundaryInputs, const std::vector<int>& boundaryOutputs, int depth) {
        struct Endpoints {
            std::vector<int> inputs, outputs;
        };
        std::unordered_map<uint32_t, Endpoints> endpoints;
        size_t inputPin = 0, outputPin = 0;

        for (const Node& node : component.nodes) {
            Endpoints& pins = endpoints[node.id];
            for (int index = 0; index < InputCount(node); index++) {
                pins.inputs.push_back(NewEndpoint());
            }
            for (int index = 0; index < OutputCount(node); index++) {
                pins.outputs.push_back(NewEndpoint());
            }

            switch (node.kind) {
                case NodeKind::Input:
                    for (size_t bit = 0; bit < pins.outputs.size(); bit++, inputPin++) {
                        if (depth == 0) {
                            Gate gate{GateKind::Switch};
                            gate.outputs[0] = pins.outputs[bit];
                            topSwitches[node.id].push_back(static_cast<int>(gates.size()));
                            gates.push_back(gate);
                        }
                        else if (inputPin < boundaryInputs.size()) {
                            Merge(pins.outputs[bit], boundaryInputs[inputPin]);
                        }
                    }
                    break;
                case NodeKind::Output:
                    for (size_t bit = 0; bit < pins.inputs.size(); bit++, outputPin++) {
                        if (depth > 0 && outputPin < boundaryOutputs.size()) {
                            Merge(pins.inputs[bit], boundaryOutputs[outputPin]);
                        }
                    }
                    break;
                case NodeKind::Rom: {
                    RomUnit rom{node.memory, pins.inputs, pins.outputs};
                    rom.address.resize(static_cast<size_t>(node.memory->addressBits));
                    rom.chipEnable = pins.inputs[static_cast<size_t>(node.memory->addressBits)];
                    rom.outputEnable = pins.inputs[static_cast<size_t>(node.memory->addressBits) + 1];
                    roms.push_back(rom);
                    break;
                }
                case NodeKind::Ram: {
                    size_t addressBits = static_cast<size_t>(node.memory->addressBits);
                    RamUnit ram;
                    ram.memory = node.memory;
                    ram.address.assign(pins.inputs.begin(), pins.inputs.begin() + static_cast<long>(addressBits));
                    ram.chipEnable = pins.inputs[addressBits];
                    ram.outputEnable = pins.inputs[addressBits + 1];
                    ram.writeEnable = pins.inputs[addressBits + 2];
                    for (size_t bit = 0; bit < pins.outputs.size(); bit++) {
                        Merge(pins.inputs[addressBits + RamControlPins + bit], pins.outputs[bit]);  // one wire, two sides
                        ram.data.push_back(pins.outputs[bit]);
                    }
                    if (depth == 0) {
                        topRams[node.id] = static_cast<int>(rams.size());
                    }
                    rams.push_back(std::move(ram));
                    break;
                }
                case NodeKind::Terminal: {
                    TerminalUnit terminal;
                    terminal.registerSelect = pins.inputs[0];
                    terminal.chipEnable = pins.inputs[1];
                    terminal.outputEnable = pins.inputs[2];
                    terminal.writeEnable = pins.inputs[3];
                    for (int bit = 0; bit < TerminalDataBits; bit++) {
                        Merge(pins.inputs[static_cast<size_t>(TerminalControlPins + bit)], pins.outputs[static_cast<size_t>(bit)]);
                        terminal.data.push_back(pins.outputs[static_cast<size_t>(bit)]);
                    }
                    terminal.irq = pins.outputs[TerminalDataBits];
                    terminals.push_back(std::move(terminal));
                    break;
                }
                case NodeKind::Component: {
                    const Component* child = FindComponent(node.typeName);
                    if (!child) {
                        missingComponents++;
                    }
                    else if (depth >= MaxNestingDepth) {
                        nestingTooDeep = true;
                    }
                    else {
                        Expand(*child, pins.inputs, pins.outputs, depth + 1);
                    }
                    break;
                }
                default: {
                    static const GateKind GateKinds[PrimitiveCount] = {
                        GateKind::Switch, GateKind::Switch, GateKind::Low, GateKind::High, GateKind::Clock,
                        GateKind::Buffer, GateKind::Not, GateKind::And, GateKind::Or, GateKind::Nand,
                        GateKind::Nor, GateKind::Xor, GateKind::Xnor, GateKind::FlipFlop, GateKind::TriState,
                        GateKind::Low, GateKind::Low, GateKind::Low};  // ROM, RAM and Terminal aren't gates
                    Gate gate{GateKinds[static_cast<int>(node.kind)]};
                    for (size_t index = 0; index < pins.inputs.size(); index++) {
                        gate.inputs[index] = pins.inputs[index];
                    }
                    for (size_t index = 0; index < pins.outputs.size(); index++) {
                        gate.outputs[index] = pins.outputs[index];
                    }
                    gates.push_back(gate);
                    break;
                }
            }

            if (depth == 0) {
                topInputs[node.id] = pins.inputs;
                topOutputs[node.id] = pins.outputs;
            }
        }

        for (const Wire& wire : component.wires) {
            auto from = endpoints.find(wire.fromNode), to = endpoints.find(wire.toNode);
            if (from == endpoints.end() || to == endpoints.end()) {
                continue;
            }
            if (wire.fromPin + wire.width > from->second.outputs.size() || wire.toPin + wire.width > to->second.inputs.size()) {
                continue;
            }
            for (uint32_t bit = 0; bit < wire.width; bit++) {
                Merge(from->second.outputs[wire.fromPin + bit], to->second.inputs[wire.toPin + bit]);
            }
        }
    }

    // Turns endpoint ids into compact net ids everywhere. Returns the net count.
    int AssignNets() {
        std::vector<int> netOfRoot(parent.size(), -1);
        int netCount = 0;
        auto toNet = [&](int endpoint) {
            if (endpoint < 0) {
                return -1;
            }
            int root = Find(endpoint);
            if (netOfRoot[root] < 0) {
                netOfRoot[root] = netCount++;
            }
            return netOfRoot[root];
        };
        for (Gate& gate : gates) {
            for (int& input : gate.inputs) {
                input = toNet(input);
            }
            for (int& output : gate.outputs) {
                output = toNet(output);
            }
        }
        for (RomUnit& rom : roms) {
            for (int& input : rom.address) {
                input = toNet(input);
            }
            rom.chipEnable = toNet(rom.chipEnable);
            rom.outputEnable = toNet(rom.outputEnable);
            for (int& output : rom.outputs) {
                output = toNet(output);
            }
        }
        for (TerminalUnit& terminal : terminals) {
            for (int* net : {&terminal.registerSelect, &terminal.chipEnable, &terminal.outputEnable,
                             &terminal.writeEnable, &terminal.irq}) {
                *net = toNet(*net);
            }
            for (int& data : terminal.data) {
                data = toNet(data);
            }
        }
        for (RamUnit& ram : rams) {
            for (int& input : ram.address) {
                input = toNet(input);
            }
            for (int& data : ram.data) {
                data = toNet(data);
            }
            ram.chipEnable = toNet(ram.chipEnable);
            ram.outputEnable = toNet(ram.outputEnable);
            ram.writeEnable = toNet(ram.writeEnable);
        }
        for (auto& entry : topInputs) {
            for (int& endpoint : entry.second) {
                endpoint = toNet(endpoint);
            }
        }
        for (auto& entry : topOutputs) {
            for (int& endpoint : entry.second) {
                endpoint = toNet(endpoint);
            }
        }
        return netCount;
    }

   private:
    std::vector<int> parent;
};

}  // namespace

// Lets every gate react until nothing changes. Returns the number of gate delays it took.
// With edgesEnabled false (at power-up), flip-flops only track their clock level and RAM doesn't
// write: every net starts out floating, which would otherwise look like /WE low for a moment.
static int Settle(Simulation& simulation, bool edgesEnabled = true) {
    std::vector<uint8_t>& current = simulation.signals;
    std::vector<uint8_t>& next = simulation.nextSignals;

    // Every net starts each step undriven; each driver then adds its level.
    auto drive = [&](int net, int level) {
        if (net < 0 || level == Floating) {
            return;
        }
        uint8_t& now = next[net];
        now = now == Floating || now == level ? static_cast<uint8_t>(level) : Conflict;
    };
    auto read = [&](int net) -> int { return net >= 0 && current[net] == High; };

    for (int step = 1; step <= MaxSettleSteps; step++) {
        bool changed = false;
        next.assign(current.size(), Floating);

        for (Gate& gate : simulation.gates) {
            auto in = [&](int index) -> int { return read(gate.inputs[index]); };
            int value;
            switch (gate.kind) {
                case GateKind::Switch:
                    value = gate.state;
                    break;
                case GateKind::Low:
                    value = 0;
                    break;
                case GateKind::High:
                    value = 1;
                    break;
                case GateKind::Clock:
                    value = simulation.clock;
                    break;
                case GateKind::Buffer:
                    value = in(0);
                    break;
                case GateKind::Not:
                    value = !in(0);
                    break;
                case GateKind::And:
                    value = in(0) & in(1);
                    break;
                case GateKind::Or:
                    value = in(0) | in(1);
                    break;
                case GateKind::Nand:
                    value = !(in(0) & in(1));
                    break;
                case GateKind::Nor:
                    value = !(in(0) | in(1));
                    break;
                case GateKind::Xor:
                    value = in(0) ^ in(1);
                    break;
                case GateKind::Xnor:
                    value = !(in(0) ^ in(1));
                    break;
                case GateKind::FlipFlop: {
                    int clockNow = in(1);
                    if (edgesEnabled && clockNow && !gate.lastClock && gate.state != in(0)) {
                        gate.state = static_cast<uint8_t>(in(0));
                        changed = true;
                    }
                    if (gate.lastClock != clockNow) {
                        changed = true;
                    }
                    gate.lastClock = static_cast<uint8_t>(clockNow);
                    value = gate.state;
                    drive(gate.outputs[1], !value);
                    break;
                }
                case GateKind::TriState:
                    value = in(1) ? Floating : in(0);
                    break;
                default:
                    value = 0;
                    break;
            }
            drive(gate.outputs[0], value);
        }
        for (const RomUnit& rom : simulation.roms) {
            if (read(rom.chipEnable) || read(rom.outputEnable)) {
                continue;  // not selected: outputs let go
            }
            size_t address = 0;
            for (size_t bit = 0; bit < rom.address.size(); bit++) {
                if (read(rom.address[bit])) {
                    address |= size_t{1} << bit;
                }
            }
            uint32_t word = address < rom.data->words.size() ? rom.data->words[address] : 0;
            for (size_t bit = 0; bit < rom.outputs.size(); bit++) {
                drive(rom.outputs[bit], (word >> bit) & 1);
            }
        }
        for (RamUnit& ram : simulation.rams) {
            if (read(ram.chipEnable)) {
                continue;  // deselected: ignore everything, drive nothing
            }
            size_t address = 0;
            for (size_t bit = 0; bit < ram.address.size(); bit++) {
                if (read(ram.address[bit])) {
                    address |= size_t{1} << bit;
                }
            }
            if (!read(ram.writeEnable)) {
                if (!edgesEnabled) {
                    continue;
                }
                // Writing: store whatever is on D, every step, as the real chip does while /WE is low.
                uint32_t word = 0;
                for (size_t bit = 0; bit < ram.data.size(); bit++) {
                    if (read(ram.data[bit])) {
                        word |= 1u << bit;
                    }
                }
                if (ram.words[address] != word) {
                    ram.words[address] = word;
                    changed = true;
                }
            }
            else if (!read(ram.outputEnable)) {
                uint32_t word = ram.words[address];
                for (size_t bit = 0; bit < ram.data.size(); bit++) {
                    drive(ram.data[bit], (word >> bit) & 1);
                }
            }
        }
        for (TerminalUnit& terminal : simulation.terminals) {
            // A write or read takes effect once, when its cycle ends, as a real UART's does: the
            // byte is latched while /WE is low and printed when /WE (or /CE) goes back to 1.
            bool selected = !read(terminal.chipEnable), writeEnable = !read(terminal.writeEnable);
            bool writeNow = edgesEnabled && selected && writeEnable;
            bool readNow = edgesEnabled && selected && !writeEnable && !read(terminal.outputEnable);
            int registerNow = read(terminal.registerSelect);
            if (writeNow) {
                terminal.writing = true;
                terminal.writeRegister = registerNow;
                terminal.writeByte = 0;
                for (int bit = 0; bit < TerminalDataBits; bit++) {
                    if (read(terminal.data[static_cast<size_t>(bit)])) {
                        terminal.writeByte |= static_cast<uint8_t>(1 << bit);
                    }
                }
            }
            else if (terminal.writing) {
                terminal.writing = false;
                if (terminal.writeRegister == 0) {
                    simulation.console += static_cast<char>(terminal.writeByte);
                    changed = true;
                }
            }
            if (readNow) {
                // Latch the register as the read starts: when it ends, the address can move on
                // before the decoder lets go of /CE, which would turn a data read into a status one.
                if (!terminal.reading) {
                    terminal.readRegister = registerNow;
                }
                terminal.reading = true;
                if (terminal.readRegister && simulation.keys.empty()) {
                    simulation.keysWanted = true;
                }
                bool keyWaiting = !simulation.keys.empty();
                int byte;
                if (registerNow) {
                    byte = 2;  // status: bit 1 = ready to print, bit 0 = key waiting
                    if (keyWaiting) {
                        byte |= 1;
                    }
                }
                else if (keyWaiting) {
                    byte = simulation.keys.front();
                }
                else {
                    byte = 0;
                }
                for (int bit = 0; bit < TerminalDataBits; bit++) {
                    drive(terminal.data[static_cast<size_t>(bit)], (byte >> bit) & 1);
                }
            }
            else if (terminal.reading) {
                terminal.reading = false;
                if (terminal.readRegister == 0 && !simulation.keys.empty()) {
                    simulation.keys.pop_front();
                    changed = true;
                }
            }
            drive(terminal.irq, !simulation.keys.empty());
        }

        if (next != current) {
            changed = true;
        }
        current.swap(next);
        if (!changed) {
            simulation.unstable = false;
            simulation.conflicts = static_cast<int>(std::count(current.begin(), current.end(), Conflict));
            return step - 1;
        }
    }
    simulation.unstable = true;
    return MaxSettleSteps;
}

void RecordSettle(Simulation& simulation) {
    simulation.lastSettleSteps = Settle(simulation);
    simulation.slowestSettleSteps = std::max(simulation.slowestSettleSteps, simulation.lastSettleSteps);
}

void BuildSimulation(Simulation& simulation, const Component& top) {
    Flattener flattener;
    flattener.Expand(top, {}, {}, 0);
    int netCount = flattener.AssignNets();

    bool keepRunning = simulation.running, keepRandomRam = simulation.randomRam;
    float keepFrequency = simulation.frequency;
    simulation = Simulation{};
    simulation.running = keepRunning;
    simulation.frequency = keepFrequency;
    simulation.randomRam = keepRandomRam;
    simulation.gates = std::move(flattener.gates);
    simulation.roms = std::move(flattener.roms);
    simulation.rams = std::move(flattener.rams);
    simulation.topRams = std::move(flattener.topRams);
    simulation.terminals = std::move(flattener.terminals);
    // RAM powers up preloaded from its image, or else holding zeros or random bytes.
    std::mt19937 random(std::random_device{}());
    for (RamUnit& ram : simulation.rams) {
        if (!ram.memory->image.empty()) {
            ram.words = ram.memory->words;
        }
        else {
            ram.words.assign(ram.memory->words.size(), 0);
            if (simulation.randomRam) {
                for (uint32_t& word : ram.words) {
                    word = static_cast<uint32_t>(random()) & ram.memory->DataMask();
                }
            }
        }
    }
    simulation.netCount = netCount;
    simulation.signals.assign(static_cast<size_t>(netCount), Floating);
    simulation.nodeInputNets = std::move(flattener.topInputs);
    simulation.nodeOutputNets = std::move(flattener.topOutputs);
    simulation.switchGates = std::move(flattener.topSwitches);
    simulation.missingComponents = flattener.missingComponents;
    simulation.nestingTooDeep = flattener.nestingTooDeep;
    // Power-up: let everything settle without treating initial clock levels as edges.
    simulation.lastSettleSteps = Settle(simulation, false);
}

void HalfStep(Simulation& simulation) {
    simulation.clock ^= 1;
    simulation.halfCycles++;
    RecordSettle(simulation);
}

// Sets an Input node's switches (bit n of value for its pin n) without settling.
void SetSwitchBits(Simulation& simulation, uint32_t inputNode, uint32_t value) {
    auto found = simulation.switchGates.find(inputNode);
    if (found == simulation.switchGates.end()) {
        return;
    }
    for (size_t bit = 0; bit < found->second.size(); bit++) {
        simulation.gates[static_cast<size_t>(found->second[bit])].state = static_cast<uint8_t>((value >> bit) & 1);
    }
}

void SetSwitch(Simulation& simulation, uint32_t inputNode, uint32_t value) {
    SetSwitchBits(simulation, inputNode, value);
    RecordSettle(simulation);
}

uint32_t SwitchValue(const Simulation& simulation, uint32_t inputNode) {
    auto found = simulation.switchGates.find(inputNode);
    if (found == simulation.switchGates.end()) {
        return 0;
    }
    uint32_t value = 0;
    for (size_t bit = 0; bit < found->second.size(); bit++) {
        value |= static_cast<uint32_t>(simulation.gates[static_cast<size_t>(found->second[bit])].state) << bit;
    }
    return value;
}

uint32_t PinNodeByLabel(const Component& component, NodeKind side, const char* label) {
    for (const Node& node : component.nodes) {
        if (node.kind == side && node.label == label) {
            return node.id;
        }
    }
    return 0;
}

void SetInput(Simulation& simulation, const Component& component, const char* label, int value) {
    SetSwitch(simulation, PinNodeByLabel(component, NodeKind::Input, label), value);
}

int ReadOutput(const Simulation& simulation, const Component& component, const char* label) {
    return simulation.Value(simulation.NetOfInput(PinNodeByLabel(component, NodeKind::Output, label), 0));
}

uint8_t ReadLevel(const Simulation& simulation, const Component& component, const char* label) {
    return simulation.Level(simulation.NetOfInput(PinNodeByLabel(component, NodeKind::Output, label), 0));
}

void SetBus(Simulation& simulation, const Component& component, const char* name, int bits, uint32_t value) {
    if (uint32_t bus = PinNodeByLabel(component, NodeKind::Input, name); bus && component.FindNode(bus)->width == bits) {
        SetSwitchBits(simulation, bus, value);
        return;
    }
    for (int bit = 0; bit < bits; bit++) {
        std::string label = bits == 1 ? name : name + std::to_string(bit);
        SetSwitchBits(simulation, PinNodeByLabel(component, NodeKind::Input, label.c_str()), (value >> bit) & 1);
    }
}

uint32_t ReadBus(const Simulation& simulation, const Component& component, const char* name, int bits) {
    uint32_t value = 0;
    uint32_t bus = PinNodeByLabel(component, NodeKind::Output, name);
    for (int bit = 0; bit < bits; bit++) {
        int net = bus ? simulation.NetOfInput(bus, bit)
                      : simulation.NetOfInput(PinNodeByLabel(component, NodeKind::Output, (name + std::to_string(bit)).c_str()), 0);
        value |= static_cast<uint32_t>(simulation.Value(net)) << bit;
    }
    return value;
}

}  // namespace Kestrel
