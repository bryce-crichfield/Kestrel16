// HeadlessRun.cpp -- --run: a component simulated without a window.

#include "HeadlessRun.h"
#include "Circuit.h"
#include "ComponentFile.h"
#include "Simulation.h"
#include "Toolchain/Toolchain.h"

#include <chrono>
#include <cstdio>
#include <thread>

namespace Kestrel {

int RunHeadless(const RunOptions& options) {
    std::vector<std::string> problems = LoadLibrary();
    for (const std::string& problem : problems)
        fprintf(stderr, "%s\n", problem.c_str());
    Component* component = FindComponent(options.component);
    if (!component) {
        fprintf(stderr, "no component named '%s' in %s\n", options.component.c_str(), componentsDirectory.c_str());
        return 2;
    }

    if (!options.program.empty()) {
        std::vector<uint8_t> image;
        if (!BuildProgram(options.program, image, stderr))
            return 2;
        int loaded = 0;
        for (Node& node : component->nodes) {
            if (node.kind != NodeKind::Ram || !node.memory || node.memory->image.empty())
                continue;
            node.memory->image = options.program;
            std::string problem = LoadRomBytes(*node.memory, image);
            if (!problem.empty()) {
                fprintf(stderr, "%s\n", problem.c_str());
                return 2;
            }
            loaded++;
        }
        if (!loaded) {
            fprintf(stderr, "'%s' has no RAM with an image to load the program into\n", component->name.c_str());
            return 2;
        }
    }

    // With no --until or --fail-on, stop on HALTED and fail on ILLEGAL, if the component has them.
    std::vector<std::string> stopPins = options.stopPins, failPins = options.failPins;
    auto hasOutput = [&](const std::string& label) {
        for (const Node* node : PinNodes(*component, NodeKind::Output))
            if (node->label == label && node->width == 1)
                return true;
        return false;
    };
    if (stopPins.empty() && failPins.empty()) {
        if (hasOutput("HALTED"))
            stopPins.push_back("HALTED");
        if (hasOutput("ILLEGAL"))
            failPins.push_back("ILLEGAL");
    }
    for (const std::vector<std::string>* pins : {&stopPins, &failPins})
        for (const std::string& label : *pins)
            if (!hasOutput(label)) {
                fprintf(stderr, "'%s' has no 1-bit Output named '%s'\n", component->name.c_str(), label.c_str());
                return 2;
            }

    Simulation simulation;
    BuildSimulation(simulation, *component);
    if (simulation.missingComponents)
        fprintf(stderr, "%d missing component(s) left out\n", simulation.missingComponents);
    // Stdin (a pipe or the console) is read a line at a time, only once the program asks for a
    // key that isn't there, so a program that never reads doesn't wait on input.
    bool endOfInput = false;
    size_t starvedAt = 0;
    int starvedPolls = 0;

    auto firstHigh = [&](const std::vector<std::string>& labels) -> const std::string* {
        for (const std::string& label : labels)
            if (ReadOutput(simulation, *component, label.c_str()))
                return &label;
        return nullptr;
    };
    size_t printed = 0;
    auto flushConsole = [&] {
        if (simulation.console.size() == printed)
            return;
        fwrite(simulation.console.data() + printed, 1, simulation.console.size() - printed, stdout);
        fflush(stdout);
        printed = simulation.console.size();
    };

    auto start = std::chrono::steady_clock::now();
    uint64_t cycles = 0;
    int status = 3;
    std::string reason = "the cycle limit";
    // --hz paces the clock against wall time from paceStart; waiting on input restarts it.
    auto paceStart = start;
    uint64_t paceCycles = 0;
    double waitedSeconds = 0;
    for (;;) {
        if (const std::string* pin = firstHigh(failPins)) {
            status = 1;
            reason = *pin;
            break;
        }
        if (const std::string* pin = firstHigh(stopPins)) {
            status = 0;
            reason = *pin;
            break;
        }
        if (simulation.unstable) {
            status = 4;
            reason = "an oscillation";
            break;
        }
        if (simulation.keysWanted) {
            simulation.keysWanted = false;
            // Printing polls the status register too, so the program only counts as waiting for
            // a key once it has gone on polling without printing anything.
            if (simulation.console.size() != starvedAt)
                starvedAt = simulation.console.size(), starvedPolls = 0;
            if (simulation.keys.empty() && ++starvedPolls > 100) {
                starvedPolls = 0;
                if (endOfInput) {
                    status = 0;
                    reason = "the end of input";
                    break;
                }
                flushConsole();  // show the prompt before waiting on a console line
                auto waitStart = std::chrono::steady_clock::now();
                int character;
                while ((character = getchar()) != EOF) {
                    simulation.keys.push_back(static_cast<uint8_t>(character));
                    if (character == '\n')
                        break;
                }
                if (character == EOF)
                    endOfInput = true;
                paceStart = std::chrono::steady_clock::now(), paceCycles = cycles;
                waitedSeconds += std::chrono::duration<double>(paceStart - waitStart).count();
            }
        }
        if (cycles >= options.maxCycles)
            break;
        HalfStep(simulation);
        HalfStep(simulation);
        cycles++;
        flushConsole();
        if (options.hz > 0) {
            auto due = paceStart + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                       std::chrono::duration<double>((cycles - paceCycles) / options.hz));
            if (due > std::chrono::steady_clock::now())
                std::this_thread::sleep_until(due);
        }
    }
    flushConsole();

    // Time spent waiting for a typed line isn't the machine's.
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() - waitedSeconds;
    if (status != 0 || options.stats)
        fprintf(stderr, "%s: stopped on %s after %llu cycles (%.1f s, %.0f Hz; %zu gates, slowest settle %d steps, %d conflicting nets)\n",
                component->name.c_str(), reason.c_str(), static_cast<unsigned long long>(cycles), seconds,
                seconds > 0 ? cycles / seconds : 0.0, simulation.gates.size(), simulation.slowestSettleSteps,
                simulation.conflicts);
    return status;
}

}  // namespace Kestrel
