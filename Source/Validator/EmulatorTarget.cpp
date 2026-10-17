// EmulatorTarget.cpp -- the reference: Kestrel::Machine.

#include "Targets.h"

#include "Kestrel/Machine.h"

#include <algorithm>
#include <deque>

namespace Kestrel {

const char* RunResultName(RunResult result) {
    switch (result) {
        case RunResult::Halted:
            return "halted";
        case RunResult::Illegal:
            return "illegal instruction";
        case RunResult::LimitReached:
            return "the limit";
        case RunResult::Failed:
            return "a failure";
    }
    return "?";
}

namespace {

class RecordingTerminal : public Terminal {
   public:
    std::deque<uint8_t> keys;
    std::string output;

    bool KeyWaiting() override { return !keys.empty(); }
    uint8_t ReadKey() override {
        uint8_t key = keys.front();
        keys.pop_front();
        return key;
    }
    void Print(uint8_t byte) override { output += static_cast<char>(byte); }
};

class EmulatorTarget : public Target {
   public:
    const char* Name() const override { return "emulator"; }
    const char* Unit() const override { return "instructions"; }

    bool Load(const std::vector<uint8_t>& image) override {
        if (image.size() > Address::IoBase) {
            problem = "the program doesn't fit below the I/O space";
            return false;
        }
        machine.memory.fill(0);
        std::copy(image.begin(), image.end(), machine.memory.begin());
        return true;
    }

    void Type(const std::string& input) override { terminal.keys.insert(terminal.keys.end(), input.begin(), input.end()); }

    RunResult Run(uint64_t limit) override {
        for (ran = 0; ran < limit; ran++) {
            StepInfo info;
            StepResult result = machine.Step(&info);
            if (result == StepResult::Halted) {
                return RunResult::Halted;
            }
            if (result == StepResult::Illegal) {
                return RunResult::Illegal;
            }
        }
        return RunResult::LimitReached;
    }

    uint64_t Ran() const override { return ran; }

    MachineState ReadState() override {
        MachineState state;
        state.ram.assign(machine.memory.begin(), machine.memory.begin() + Address::IoBase);
        state.output = terminal.output;
        return state;
    }

    std::string Problem() const override { return problem; }

   private:
    RecordingTerminal terminal;
    Machine machine{terminal};
    uint64_t ran = 0;
    std::string problem;
};

}  // namespace

std::unique_ptr<Target> MakeEmulatorTarget() {
    return std::make_unique<EmulatorTarget>();
}

}  // namespace Kestrel
