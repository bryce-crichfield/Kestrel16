// KestrelValidator -- differential testing: a program on the emulator and on the gate-level
// computer must end in the same state (ValidatorDesign.md).
//
// Run:    KestrelValidator prog.c|prog.s|prog.bin
//           --components DIR        where the components are (default: Build/Components)
//           --component NAME        the machine to check (default: "Kestrel16 Computer")
//           --input TEXT            typed into the terminal before the program starts
//           --max-instructions N    the emulator's limit (default 10,000,000)
//           --max-cycles N          the simulator's limit (default 50,000,000)
//           --verbose               print both final states
//
// A compiled program ends in __dump (Dump.h), so its registers and flags are compared too; an
// assembly program does if it ends with "j __dump". Exit status: 0 the states match, 1 they
// differ (or a machine stopped differently), 2 a usage, build or load problem.

#include "Dump.h"
#include "State.h"
#include "Targets.h"

#include "Circuit.h"
#include "Toolchain/Toolchain.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace Kestrel;

namespace {

int Usage(const char* program) {
    fprintf(stderr,
            "usage: %s [--components DIR] [--component NAME] [--input TEXT] [--max-instructions N]\n"
            "       [--max-cycles N] [--verbose] program.c|.s|.bin\n",
            program);
    return 2;
}

// Runs one target; false (having said why) if it couldn't be run at all.
bool RunTarget(Target& target, const std::vector<uint8_t>& image, const std::string& input, uint64_t limit, RunResult& result) {
    if (!target.Load(image)) {
        fprintf(stderr, "%s: %s\n", target.Name(), target.Problem().c_str());
        return false;
    }
    target.Type(input);
    result = target.Run(limit);
    fprintf(stderr, "%s: stopped on %s after %llu %s\n", target.Name(), RunResultName(result),
            static_cast<unsigned long long>(target.Ran()), target.Unit());
    if (result == RunResult::Failed) {
        fprintf(stderr, "%s: %s\n", target.Name(), target.Problem().c_str());
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string path, componentName = "Kestrel16 Computer", input;
    uint64_t maxInstructions = 10000000, maxCycles = 50000000;
    bool verbose = false;
    for (int index = 1; index < argc; index++) {
        std::string argument = argv[index];
        bool hasValue = index + 1 < argc;
        if (argument == "--components" && hasValue) {
            componentsDirectory = argv[++index];
            if (!componentsDirectory.empty() && componentsDirectory.back() != '/' && componentsDirectory.back() != '\\') {
                componentsDirectory += '/';
            }
        }
        else if (argument == "--component" && hasValue) {
            componentName = argv[++index];
        }
        else if (argument == "--input" && hasValue) {
            input = argv[++index];
        }
        else if (argument == "--max-instructions" && hasValue) {
            maxInstructions = strtoull(argv[++index], nullptr, 0);
        }
        else if (argument == "--max-cycles" && hasValue) {
            maxCycles = strtoull(argv[++index], nullptr, 0);
        }
        else if (argument == "--verbose") {
            verbose = true;
        }
        else if (argument[0] != '-' && path.empty()) {
            path = argument;
        }
        else {
            return Usage(argv[0]);
        }
    }
    if (path.empty()) {
        return Usage(argv[0]);
    }

    bool dumped = false;
    std::vector<uint8_t> image;
    if (!BuildProgram(path, image, stderr, [&](std::string& assembly, bool fromC) { dumped = AddDump(assembly, fromC); })) {
        return 2;
    }

    std::string problem;
    std::unique_ptr<Target> reference = MakeEmulatorTarget();
    std::unique_ptr<Target> machine = MakeSimulatorTarget(componentName, problem);
    if (!machine) {
        fprintf(stderr, "%s\n", problem.c_str());
        return 2;
    }

    RunResult expectedResult, actualResult;
    if (!RunTarget(*reference, image, input, maxInstructions, expectedResult)) {
        return 2;
    }
    if (expectedResult != RunResult::Halted) {
        fprintf(stderr, "%s: the reference has to halt for there to be a state to compare\n", path.c_str());
        return 2;
    }
    if (!RunTarget(*machine, image, input, maxCycles, actualResult)) {
        return 1;
    }

    MachineState expected = reference->ReadState(), actual = machine->ReadState();
    if (verbose && dumped) {
        printf("emulator:  %s\nsimulator: %s\n", DescribeDump(expected).c_str(), DescribeDump(actual).c_str());
    }
    std::vector<std::string> differences = Compare(expected, actual, dumped);
    if (actualResult != expectedResult) {
        differences.insert(differences.begin(), std::string("stopped on ") + RunResultName(actualResult) + ", expected " +
                                                    RunResultName(expectedResult));
    }
    if (differences.empty()) {
        printf("%s: states match (%s)\n", path.c_str(),
               dumped ? "registers, flags, memory and output" : "memory and output; no __dump");
        return 0;
    }
    printf("%s: states differ\n", path.c_str());
    for (const std::string& difference : differences) {
        printf("  %s\n", difference.c_str());
    }
    return 1;
}
