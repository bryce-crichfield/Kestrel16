// KestrelEmulator -- runs Kestrel16 programs on the reference machine (Source/Kestrel/Machine.h).
//
// Run:    KestrelEmulator prog.bin     load raw little-endian binary at 0x0000
//         KestrelEmulator              run the built-in demo (prints a greeting)
//         KestrelEmulator -t prog.bin  trace every instruction to stderr
//         KestrelEmulator -x prog.bin  dump registers when the machine stops
//         KestrelEmulator -n 1000 ...  stop after 1000 steps (0 = no limit)
//         KestrelEmulator --run prog.c build a .c or .s first, and stop once piped input runs out
//
// The UART is the console: what the program prints goes to stdout, and what is typed (or
// piped in) is what it reads.

#include "Kestrel/Alu.h"
#include "Kestrel/Disassembler.h"
#include "Kestrel/Files.h"
#include "Kestrel/Machine.h"
#include "Toolchain/Toolchain.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/select.h>
#include <unistd.h>
#define HavePosixIo 1
#elif defined(_WIN32)
#include <conio.h>
#include <io.h>
#define HaveWindowsIo 1
#endif

using namespace Kestrel;

namespace {

// The console as the UART's terminal: non-blocking input on POSIX and a Windows console; piped
// input on Windows is read as the program asks for it.
class Console : public Terminal {
   public:
    bool stopAtEndOfInput = false;  // --run: note when input runs out while the program waits
    bool inputExhausted = false;

    bool KeyWaiting() override { return Poll(); }

    uint8_t ReadKey() override {
        haveByte = false;
        return byte;
    }

    void Print(uint8_t value) override {
        fputc(value, stdout);
        fflush(stdout);
    }

   private:
    bool haveByte = false, endOfFile = false;
    uint8_t byte = 0;

    bool Poll() {
        if (haveByte)
            return true;
        if (endOfFile)
            return false;
#ifdef HavePosixIo
        {
            const int standardInput = 0;
            fd_set readSet;
            struct timeval noWait = {0, 0};
            FD_ZERO(&readSet);
            FD_SET(standardInput, &readSet);
            if (select(standardInput + 1, &readSet, nullptr, nullptr, &noWait) > 0) {
                uint8_t character;
                ssize_t bytesRead = read(standardInput, &character, 1);
                if (bytesRead == 1) {
                    byte = character;
                    haveByte = true;
                    return true;
                }
                if (bytesRead == 0)
                    endOfFile = true;
            }
        }
#elif defined(HaveWindowsIo)
        if (_isatty(_fileno(stdin))) {
            if (_kbhit()) {
                int character = _getch();
                if (character == '\r')
                    character = '\n';
                // a console doesn't echo for _getch, so echo like a terminal would
                if (character == '\n' || (character >= ' ' && character < 127)) {
                    putchar(character);
                    fflush(stdout);
                }
                if (character == 26) {
                    endOfFile = true;
                    return false;
                }  // Ctrl+Z
                byte = static_cast<uint8_t>(character);
                haveByte = true;
                return true;
            }
        } else {
            int character = getchar();  // a pipe or file: blocks only until the next byte or EOF
            if (character != EOF) {
                byte = static_cast<uint8_t>(character);
                haveByte = true;
                return true;
            }
            endOfFile = true;
        }
#endif
        if (endOfFile && stopAtEndOfInput)
            inputExhausted = true;
        return false;
    }
};

// Built-in demo: prints a string through the UART.
constexpr uint16_t DemoMessageAddress = 0x0100;

void LoadDemo(Machine& machine) {
    const uint16_t code[] = {
        EncodeJump(Opcode::Jump, 1),                                       // 0000: j start
        EncodeSystem(SystemFunction::ReturnFromInterrupt, 0),              // 0002: reti (interrupt stub)
        EncodeLong(Opcode::LoadImmediate, 0, 0x00),                        // 0004: ldi r0, 0x00
        EncodeLong(Opcode::LoadHighImmediate, 0, 0x01),                    // 0006: lhi r0, 0x01  (r0 = message)
        EncodeLong(Opcode::LoadImmediate, 1, Address::UartData & 0xFF),    // 0008: ldi r1, low(UART)
        EncodeLong(Opcode::LoadHighImmediate, 1, Address::UartData >> 8),  // 000A: lhi r1, high(UART)
        EncodeImmediate(Opcode::LoadByte, 2, 0, 0),                        // 000C: loop: lb r2, [r0+0]
        EncodeImmediate(Opcode::CompareImmediate, 0, 2, 0),                // 000E: cmpi r2, 0
        EncodeBranch(Condition::Equal, 3),                                 // 0010: beq done
        EncodeImmediate(Opcode::StoreByte, 2, 1, 0),                       // 0012: sb r2, [r1+0]
        EncodeImmediate(Opcode::AddImmediate, 0, 0, 1),                    // 0014: addi r0, r0, 1
        EncodeJump(Opcode::Jump, -6),                                      // 0016: j loop
        EncodeSystem(SystemFunction::Halt, 0),                             // 0018: done: halt
    };
    size_t index = 0;
    for (uint16_t word : code) {
        machine.memory[index++] = static_cast<uint8_t>(word & 0xFF);
        machine.memory[index++] = static_cast<uint8_t>(word >> 8);
    }
    const char* message = "Hello from Kestrel16!\nIf you want to run a different program, pass its filename on the command line.\n";
    memcpy(&machine.memory[DemoMessageAddress], message, strlen(message) + 1);
}

constexpr unsigned long long DefaultStepLimit = 50000000;

void Usage(const char* programName) {
    fprintf(stderr,
            "usage: %s [-t] [-x] [-n steps] [program.bin]\n"
            "       %s [-t] [-x] [-n steps] --run program.c|.s|.bin\n"
            "  -t        trace each instruction to stderr\n"
            "  -x        dump registers at exit\n"
            "  -n steps  stop after this many steps (default %llu, 0 = unlimited)\n"
            "  no file   run the built-in demo\n"
            "  --run     build a .c or .s first, run it with no step limit (unless -n),\n"
            "            and stop once piped input has run out and the program waits for more\n",
            programName, programName, DefaultStepLimit);
}

const char* StepResultName(StepResult result) {
    switch (result) {
        case StepResult::Halted:
            return "halted";
        case StepResult::Illegal:
            return "illegal instruction";
        default:
            return "step limit";
    }
}

// Loads a program image at address 0, up to where I/O space starts.
void LoadImage(Machine& machine, const std::vector<uint8_t>& image, const std::string& name) {
    size_t count = std::min(image.size(), static_cast<size_t>(Address::IoBase));
    if (image.size() > count)
        fprintf(stderr, "warning: %s is larger than %d bytes, truncated\n", name.c_str(), Address::IoBase);
    std::copy(image.begin(), image.begin() + static_cast<long>(count), machine.memory.begin());
}

void TraceLine(const Machine& machine, const StepInfo& info) {
    const Cpu& cpu = machine.cpu;
    auto flag = [&](uint8_t bit, char letter) { return (cpu.flags & bit) ? letter : '-'; };
    fprintf(stderr, "%04X: %04X  %-20s | r0=%04X r1=%04X r2=%04X r3=%04X r4=%04X r5=%04X r6=%04X r7=%04X %c%c%c%c\n",
            info.address, info.word, Disassemble(info.address, info.word).c_str(), cpu.registers[0], cpu.registers[1],
            cpu.registers[2], cpu.registers[3], cpu.registers[4], cpu.registers[5], cpu.registers[6], cpu.registers[7],
            flag(Flag::Zero, 'Z'), flag(Flag::Negative, 'N'), flag(Flag::Carry, 'C'), flag(Flag::Overflow, 'V'));
}

void DumpRegisters(const Cpu& cpu) {
    for (int index = 0; index < RegisterCount; index++)
        fprintf(stderr, "r%d=%04X%s", index, cpu.registers[index], index == 3 ? "\n" : "  ");
    auto flag = [&](uint8_t bit) { return (cpu.flags & bit) ? 1 : 0; };
    fprintf(stderr, "\nflags: Z=%d N=%d C=%d V=%d\n", flag(Flag::Zero), flag(Flag::Negative), flag(Flag::Carry),
            flag(Flag::Overflow));
}

}  // namespace

int main(int argc, char** argv) {
    std::string fileName;
    bool trace = false, dump = false, run = false, stepLimitGiven = false;
    unsigned long long stepLimit = DefaultStepLimit;

    for (int index = 1; index < argc; index++) {
        const char* argument = argv[index];
        if (!strcmp(argument, "-t"))
            trace = true;
        else if (!strcmp(argument, "-x"))
            dump = true;
        else if (!strcmp(argument, "-n") && index + 1 < argc)
            stepLimit = strtoull(argv[++index], nullptr, 0), stepLimitGiven = true;
        else if (!strcmp(argument, "--run") && index + 1 < argc)
            run = true, fileName = argv[++index];
        else if (argument[0] == '-') {
            Usage(argv[0]);
            return 2;
        } else
            fileName = argument;
    }

    Console console;
    Machine machine(console);
    if (run) {
        console.stopAtEndOfInput = true;
        if (!stepLimitGiven)
            stepLimit = 0;
        std::vector<uint8_t> image;
        if (!BuildProgram(fileName, image, stderr))
            return 2;
        LoadImage(machine, image, fileName);
    } else if (!fileName.empty()) {
        std::vector<uint8_t> image;
        if (!ReadFile(fileName, image)) {
            perror(fileName.c_str());
            return 1;
        }
        LoadImage(machine, image, fileName);
    } else {
        LoadDemo(machine);
    }

    StepResult result;
    StepInfo info;
    while ((result = machine.Step(&info)) == StepResult::Running) {
        if (trace && info.executed)
            TraceLine(machine, info);
        if (stepLimit && machine.cpu.steps >= stepLimit) {
            result = StepResult::LimitReached;
            break;
        }
        if (console.inputExhausted)
            break;
    }
    if (result == StepResult::Illegal)
        fprintf(stderr, "\nillegal instruction 0x%04X at 0x%04X\n", info.word, info.address);
    fflush(stdout);

    fprintf(stderr, "[%s after %llu steps, pc=0x%04X]\n",
            console.inputExhausted && result == StepResult::Running ? "end of input" : StepResultName(result),
            machine.cpu.steps, machine.cpu.programCounter);

    if (dump)
        DumpRegisters(machine.cpu);

    return result == StepResult::Illegal ? 1 : 0;
}
