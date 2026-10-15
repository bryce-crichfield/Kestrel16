// KestrelSimulator -- the Circuit Workbench, the hardware side of the Kestrel16 project.
//
// Build circuits out of logic gates, wrap them up as components with their own input and output
// pins, and use those components as black boxes inside bigger ones. Then simulate the result
// gate by gate with a controllable clock. The model is in Circuit.h, how it is simulated in
// Simulation.h, and the file format in ComponentFile.h.
//
// Each component is saved as Components/<name>.yaml. It is meant to be read and written by hand,
// or by an AI working from a datasheet. Components refer to the components they use by name.
//
// Command line
//   KestrelSimulator                     run the editor
//   KestrelSimulator --components DIR    load and save components in DIR
//   KestrelSimulator --self-test         check the simulator without opening a window
//   KestrelSimulator --check-hardware    check the machine's components against the emulator
//   KestrelSimulator --screenshot FILE   render a few frames, save a PNG, and exit
//   KestrelSimulator --run COMPONENT --program FILE
//                                        simulate without a window; the Terminal prints to
//                                        stdout. FILE is a .c, .s or .bin (see --help)

#include "Checks.h"
#include "Circuit.h"
#include "Editor.h"
#include "HeadlessRun.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace Kestrel;

static int Usage(const char* program, int status)
{
    fprintf(stderr,
            "usage: %s [--components DIR] [--self-test] [--check-hardware] [--screenshot FILE] [--simulate]\n"
            "       %s [--components DIR] --run COMPONENT [--program FILE] [--until PIN]... [--fail-on PIN]...\n"
            "                             [--max-cycles N] [--stats]\n"
            "--run simulates COMPONENT without a window: its Terminal prints to stdout, and piped stdin is typed into it.\n"
            "  --program FILE   a .c, .s or .bin, built and loaded into every RAM that has an image\n"
            "  --until PIN      stop (status 0) when this 1-bit Output goes high (default HALTED)\n"
            "  --fail-on PIN    stop (status 1) when this one does (default ILLEGAL)\n"
            "  --max-cycles N   give up after N clock cycles (status 3; default 10000000)\n"
            "  --hz N           hold the clock to N cycles per second (if the simulation can keep up)\n"
            "  --stats          report cycles, time, clock rate and gate counts on stderr\n",
            program, program);
    return status;
}

int main(int argc, char** argv)
{
    const char* screenshotPath = nullptr;
    bool simulateOnStart = false;
    RunOptions run;

    for (int index = 1; index < argc; index++) {
        std::string argument = argv[index];
        if (argument == "--self-test") return RunSelfTest();
        if (argument == "--check-hardware") return RunHardwareCheck();
        if (argument == "--components" && index + 1 < argc) {
            componentsDirectory = argv[++index];
            if (componentsDirectory.back() != '/' && componentsDirectory.back() != '\\') componentsDirectory += '/';
        } else if (argument == "--screenshot" && index + 1 < argc) {
            screenshotPath = argv[++index];
        } else if (argument == "--simulate") {
            simulateOnStart = true;
        } else if (argument == "--run" && index + 1 < argc) {
            run.component = argv[++index];
        } else if (argument == "--program" && index + 1 < argc) {
            run.program = argv[++index];
        } else if (argument == "--until" && index + 1 < argc) {
            run.stopPins.push_back(argv[++index]);
        } else if (argument == "--fail-on" && index + 1 < argc) {
            run.failPins.push_back(argv[++index]);
        } else if (argument == "--max-cycles" && index + 1 < argc) {
            run.maxCycles = std::strtoull(argv[++index], nullptr, 0);
        } else if (argument == "--hz" && index + 1 < argc) {
            run.hz = std::strtod(argv[++index], nullptr);
        } else if (argument == "--stats") {
            run.stats = true;
        } else if (argument == "--help" || argument == "-h") {
            return Usage(argv[0], 0);
        } else {
            return Usage(argv[0], 2);
        }
    }

    if (!run.component.empty()) return RunHeadless(run);
    if (!run.program.empty() || !run.stopPins.empty() || !run.failPins.empty() || run.stats) return Usage(argv[0], 2);
    return RunEditor(screenshotPath, simulateOnStart);
}
