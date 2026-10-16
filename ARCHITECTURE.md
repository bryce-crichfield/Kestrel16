# Architecture

This document describes how the Kestrel16 software is organized: what each piece owns, which boundaries must hold, and where change is expected. For what the machine *is*, read `Source/Kestrel/Instructions.h` and `Source/Kestrel/MemoryMap.h`.

## The one rule

**The emulator is the reference.** `Kestrel::Machine` defines correct behaviour. The assembler and compiler target it, and the gate-level hardware is checked against it. If the hardware and the emulator disagree, the hardware is wrong. To change behaviour, change the definitions and `Machine` first, then bring everything else in line.

## Layers

```
        Source/Kestrel/*.h definitions  ◄──── Tools/definitions.py ──► Tools/make_*.py ──► Build/Components/
                    │
              KestrelCore  (Isa, Alu, Machine, Disassembler, Files)
               ┌────┴─────────────┐
   KestrelAssemblerCore    KestrelCompilerCore
               └────┬─────────────┘
             KestrelToolchain  (.c / .s / .bin → memory image)
               ┌────┴─────────────┐
      KestrelEmulator      KestrelSimulator  (also links raylib, ImGui, imnodes, yaml-cpp)

   KestrelAssembler, KestrelCompiler: thin mains over their Core libraries
```

Dependencies point downward only. Nothing in `KestrelCore` knows about the assembler, compiler or simulator. The assembler and compiler don't know about each other: the compiler emits assembly *text*, and the toolchain joins them. Only `KestrelSimulator` depends on graphics or YAML.

| Directory | Target | Owns |
|---|---|---|
| `Source/Kestrel/` | `KestrelCore` | The machine: the definition headers, ISA enums and encode/decode, ALU and flags, the reference CPU, the disassembler, file helpers |
| `Source/Assembler/` | `KestrelAssemblerCore` + `KestrelAssembler` | Assembly text → image (`Assemble()`) |
| `Source/Compiler/` | `KestrelCompilerCore` + `KestrelCompiler` | C → assembly text (`Compile()`, `Preprocess()`) |
| `Source/Toolchain/` | `KestrelToolchain` | A program file → a memory image, in-process (`BuildProgram()`) |
| `Source/Emulator/` | `KestrelEmulator` | The console as a `Terminal`, the CLI, tracing |
| `Source/Simulator/` | `KestrelSimulator` | The circuit model, YAML files, the gate simulation, the editor, the self-test and hardware check |
| `Tools/` | (Python) | Generates the hardware components and microcode ROMs into `Build/Components/` |
| `Programs/` | (CMake) | Example and test programs, built into `Binary/Programs/` |
| `Tests/` | (CMake) | Test helpers |

## Boundaries to defend

1. **The machine is defined once.** Opcodes, functions, conditions, field layout, the memory map and the hardware ALU codes live only in `Instructions.h`, `MemoryMap.h` and `AluOperations.h`. These files hold **definitions only**: one macro call per line, with plain names, numbers and `"quoted"` text, because Python parses them with a regex. Never hard-code an opcode, an address or a condition number anywhere else, in C++ or in Python. Read it from `Isa.h` / `Alu.h` or from `definitions.py`.

2. **Libraries don't do process things.** Core libraries never call `exit`, never print, and never parse `argv`. Errors come back as values: `AssemblyResult::errors`, `CompileResult::error` and `warnings`, and `StepResult`. Only `Main.cpp` files (and `BuildProgram`'s diagnostics stream) touch stdout and stderr. This is what lets the emulator and simulator build programs in-process. Keep it that way.

3. **`Machine` talks to the world through `Terminal`.** The CPU core doesn't know about consoles, pipes or the simulator. New I/O devices belong in `Machine`'s I/O decode (and in `MemoryMap.h`), with any host side behind an interface like `Terminal`.

4. **Public headers vs internal headers.**
   - `Compiler.h`, `Assembler.h`, `Toolchain.h`, `Editor.h`, `HeadlessRun.h` and `Checks.h` are interfaces.
   - `CompilerInternal.h` and `EditorInternal.h` are shared *only* inside their own directory. Don't include them from elsewhere.

5. **Namespaces.** Everything is in `Kestrel`. The compiler's internals are in `Kestrel::Compiler`, because its `Node`, `Type` and `Symbol` would otherwise collide with the simulator's types in the same executable. File-local helpers are `static` or in an anonymous namespace. Don't export a name that only one file uses.

6. **Generated files are generated.** The yaml files and ROM images in `Build/Components/` that `Tools/make_*.py` produce are build output: the `hardware` target writes them, so they live under `Build/`, out of the source tree. Change the generator and rebuild; don't hand-edit them.

7. **Behaviour is pinned by tests.**
   - Each program in `Programs/` has expected output in `CMakeLists.txt`. Programs listed in `GATE_LEVEL_TESTS` must print the same on the gate-level computer as on the emulator.
   - `--check-hardware` holds the ALU, register file and whole computer to `KestrelCore`.
   - A refactor that changes any of these outputs is not a refactor.

## Where flexibility is built in

- **Adding or changing an instruction.**
  1. Edit `Instructions.h`.
  2. Implement it in `Machine::Step`.
  3. Write its microcode in `make_cpu.py`'s `execute()`.

  The assembler's table, the disassembler and the generator's opcode numbers follow automatically. Pseudo-instructions are assembler-only (`AssemblePseudoInstruction`).
- **Moving the memory map.** Change `MemoryMap.h`. The machine, assembler (image size), compiler (stack top) and address-decode ROM follow.
- **New I/O devices.** A device is a card in one of the 8 I/O slots (128 bytes each from `IoBase`; the 32-pin slot signals are listed in `Tools/make_cpu.py`). Give it a `KestrelSlot` and its registers in `MemoryMap.h`, its registers and its interrupt request in `Machine`'s I/O decode, and a card component plugged into `make_computer()`. Interrupts are levels: slot n is bit n of the pending and mask registers, and the driver clears its request at the card.
- **New front ends.** Anything can call `Compile()`, `Assemble()` or `BuildProgram()` and run a `Machine` with its own `Terminal`: a debugger, a test runner, a GUI emulator.
- **Compiler stages are separable.** The preprocessor is text → text, the lexer text → tokens, and the code generator works per function. Optimizations or a new backend slot in between stages without touching the parser.
- **The hardware is data.** Components are YAML and ROM images. The simulator knows only primitives (gates, flip-flops, ROM, RAM, Terminal). The CPU's design lives entirely in `Tools/`, so it can be redesigned without touching C++.
- **Tests are cheap to add.** Drop a `.c` or `.s` into `Programs/`. It is built automatically; give it an `EXPECTED_<name>` and, optionally, add it to `GATE_LEVEL_TESTS`.

## Known compromises

- **Compiler state is global.** Its stages share file-level state, and `Compile()` resets it before each run. It is not thread-safe or re-entrant.
- **Python names don't match the rest.** The Python tools still use snake_case, unlike the C++.
- **One shared global library.** The simulator's component `library` and `componentsDirectory` are globals that the editor, the tests and headless runs all use.
