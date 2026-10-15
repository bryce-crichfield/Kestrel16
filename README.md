# Kestrel16

A 16-bit computer designed from the gates up, with its own toolchain.

- **The machine:** a 16-bit RISC-style CPU with 8 registers, 64 KB of memory, memory-mapped I/O (a UART and a timer) and interrupts.
- **A reference emulator:** the definition of correct behaviour.
- **A C compiler:** structs, pointers, arrays, a preprocessor, and most of C (no floating point).
- **An assembler:** labels, expressions, directives and pseudo-instructions.
- **A gate-level simulator and circuit editor:** the real CPU, built from gates, ROMs and RAM chips modeled on 74HC parts. It runs the same programs as the emulator and gives the same answers.

See [ARCHITECTURE.md](ARCHITECTURE.md) for how the code is organized.

## Build

You need CMake, a C++17 compiler and Python 3. On Windows, use MSYS2 MinGW-w64.

```sh
cmake -S . -B Build
cmake --build Build
ctest --test-dir Build
```

Executables go to `Binary/`, and the example programs to `Binary/Programs/`.

## Usage

Run a C or assembly program on the emulator (it is built first):

```sh
Binary/KestrelEmulator --run Programs/Fibonacci.c
Binary/KestrelEmulator --run Programs/Shell.c        # interactive; type 'help'
Binary/KestrelEmulator -t Binary/Programs/Interrupts.bin  # trace each instruction
```

Run the same program on the gate-level computer:

```sh
Binary/KestrelSimulator --run "Kestrel16 Computer" --program Programs/Fibonacci.c
```

Open the circuit editor:

```sh
Binary/KestrelSimulator
```

Use the tools on their own:

```sh
Binary/KestrelCompiler prog.c          # → prog.s
Binary/KestrelAssembler -l prog.s      # → prog.bin, with a listing
Binary/KestrelEmulator prog.bin
```

Regenerate the CPU's components after changing the hardware design:

```sh
python Tools/make_alu.py && python Tools/make_registers.py && python Tools/make_cpu.py
```

## Layout

| Path | What it holds |
|---|---|
| `Source/Kestrel/` | The machine definition and the reference emulator core |
| `Source/Compiler/`, `Source/Assembler/` | The toolchain |
| `Source/Emulator/`, `Source/Simulator/` | The two ways to run programs |
| `Tools/` | Python generators for the CPU's hardware |
| `Components/` | The hardware, as circuit files |
| `Programs/` | Example and test programs |
