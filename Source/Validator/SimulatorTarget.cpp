// SimulatorTarget.cpp -- the gate-level computer, simulated.
//
// It keeps to what the debug port would offer (Target.h): the RAM chips' contents, the clock, the
// HALTED and ILLEGAL outputs and the terminal. It never reads the CPU's insides, although the
// simulation could.

#include "Targets.h"

#include "Circuit.h"
#include "ComponentFile.h"
#include "Kestrel/Isa.h"
#include "Simulation.h"

namespace Kestrel {

namespace {

class SimulatorTarget : public Target {
   public:
    explicit SimulatorTarget(Component& component) : component(component) {}

    const char* Name() const override { return "simulator"; }
    const char* Unit() const override { return "cycles"; }

    // Like the debug port writing RAM while the CPU is held: the program goes into every top-level
    // RAM chip (each keeps its byte lane), and the machine powers up running it.
    bool Load(const std::vector<uint8_t>& image) override {
        int loaded = 0;
        for (Node& node : component.nodes) {
            if (node.kind != NodeKind::Ram || !node.memory) {
                continue;
            }
            std::string loadProblem = LoadRomBytes(*node.memory, image);
            if (!loadProblem.empty()) {
                problem = loadProblem;
                return false;
            }
            node.memory->image = "(program)";  // powers up holding the image, not zeros
            loaded++;
        }
        if (!loaded) {
            problem = "'" + component.name + "' has no RAM to load the program into";
            return false;
        }
        BuildSimulation(simulation, component);
        if (simulation.missingComponents) {
            problem = std::to_string(simulation.missingComponents) + " missing component(s) in '" + component.name + "'";
            return false;
        }
        return true;
    }

    void Type(const std::string& input) override { simulation.keys.insert(simulation.keys.end(), input.begin(), input.end()); }

    RunResult Run(uint64_t limit) override {
        for (ran = 0; ran < limit; ran++) {
            if (ReadOutput(simulation, component, "ILLEGAL")) {
                return RunResult::Illegal;
            }
            if (ReadOutput(simulation, component, "HALTED")) {
                return RunResult::Halted;
            }
            if (simulation.unstable) {
                problem = "the circuit oscillates";
                return RunResult::Failed;
            }
            HalfStep(simulation);
            HalfStep(simulation);
        }
        return RunResult::LimitReached;
    }

    uint64_t Ran() const override { return ran; }

    // Like the debug port reading RAM back over the bus: each chip's bytes, in its lane.
    MachineState ReadState() override {
        MachineState state;
        state.ram.assign(Address::IoBase, 0);
        for (const auto& [node, unit] : simulation.topRams) {
            const RamUnit& ram = simulation.rams[static_cast<size_t>(unit)];
            int lane = ram.memory->lane;
            for (size_t word = 0; word < ram.words.size(); word++) {
                size_t address = lane ? 2 * word + static_cast<size_t>(lane - 1) : word;
                if (address < state.ram.size()) {
                    state.ram[address] = static_cast<uint8_t>(ram.words[word]);
                }
            }
        }
        state.output = simulation.console;
        return state;
    }

    std::string Problem() const override { return problem; }

   private:
    Component& component;
    Simulation simulation;
    uint64_t ran = 0;
    std::string problem;
};

bool HasOutput(const Component& component, const char* label) {
    for (const Node* node : PinNodes(component, NodeKind::Output)) {
        if (node->label == label && node->width == 1) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::unique_ptr<Target> MakeSimulatorTarget(const std::string& name, std::string& problem) {
    std::vector<std::string> problems = LoadLibrary();  // only matter if the component is missing
    Component* component = FindComponent(name);
    if (!component) {
        problem = "no component named '" + name + "' in " + componentsDirectory;
        for (const std::string& libraryProblem : problems) {
            problem += "\n" + libraryProblem;
        }
        return nullptr;
    }
    for (const char* label : {"HALTED", "ILLEGAL"}) {
        if (!HasOutput(*component, label)) {
            problem = "'" + name + "' has no 1-bit Output named " + label;
            return nullptr;
        }
    }
    return std::make_unique<SimulatorTarget>(*component);
}

}  // namespace Kestrel
