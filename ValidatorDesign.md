# KestrelValidator: Design Notes

Status: **steps 1–4 done** (`Source/Validator/`, the `validate_<program>` tests). This replaces the earlier differential verification notes, which leaned on reading nets inside the flattened circuit and planned for pipelines before the basics existed.

## Goal

Differential testing. Take a program, run it through the full chain (compile, assemble, run) on the **emulator** and on the **gate-level simulator**, and check that both end in the same state. The emulator is the reference: if they disagree, the hardware is wrong (see ARCHITECTURE.md, "The one rule").

One day the second machine will be the real, breadboarded Kestrel. So there's one constraint on the design: **the validator may only observe the simulator in ways the real hardware could support cheaply.** It doesn't matter whether we ever build that hardware link. Designing as if we will keeps the simulator honest, and it means the hardware later needs a new transport, not a new design.

## The shape

```
            program.c / .s
                 │  BuildProgram (Toolchain)
                 ▼
               image ──────────────┬──────────────────┐
                                   ▼                  ▼
                           EmulatorTarget      SimulatorTarget      (later: HardwareTarget)
                                   │                  │
                                   ▼                  ▼
                              final state        final state
                                   └──── compare ─────┘
                                         │
                               pass, or the first named difference
```

`KestrelValidator <program> [--input TEXT] [--max-cycles N]` exits 0 if the states match, and 1 with a report if they don't.

## What gets compared: the final state

**State = all of RAM + the bytes printed to the terminal.** Nothing else.

Registers and flags are architectural too, but they're not on any wire you can easily reach on a breadboard. So we don't read them directly. Instead, the program's last act is to **dump them into RAM** (see "The dump routine" below). Then RAM holds everything, and "compare RAM" covers the whole architectural state. This is the key simplification: the validator only ever needs to read memory.

The comparison reports by name:

```
Fibonacci_c: states differ
  R3     expected 0x1F00, got 0x1E00
  flags  expected Z=0 N=0 C=1 V=0, got Z=0 N=0 C=0 V=0
  memory 0x2040: expected 0x12, got 0x00  (3 more bytes differ)
  output identical (11 bytes)
```

The dump area is decoded into register names; any other byte is reported as memory.

## The debug port (what the validator assumes the hardware has)

The cheapest possible one:

1. **Clock control.** The validator owns the clock: run, stop, single cycle. On a breadboard that's a microcontroller driving the clock line instead of an oscillator.
2. **Reset.**
3. **Memory access while stopped.** With the CPU held, the port can read and write RAM over the bus. This loads the program and reads the final state. On a breadboard: the CPU's bus drivers go tri-state on a "bus request" line, and the microcontroller drives address/data itself.
4. **Halted signal.** One output that says the CPU sits in HALT.
5. **The UART.** The validator types the input and records the output.

No register probes, no scan chains, no retirement signal. Items 1–4 are a handful of chips plus a microcontroller, which can double as the program loader.

## Targets

```cpp
class Target {
   public:
    virtual ~Target() = default;
    virtual void Reset() = 0;
    virtual void WriteMemory(uint16_t address, const std::vector<uint8_t>& bytes) = 0;
    virtual std::vector<uint8_t> ReadMemory(uint16_t address, size_t count) = 0;
    virtual RunResult Run(uint64_t limit) = 0;  // until halted, illegal, or the limit
    virtual void Type(const std::string& input) = 0;
    virtual std::string Output() = 0;
};
```

- **EmulatorTarget** wraps `Kestrel::Machine`. The limit counts instructions.
- **SimulatorTarget** wraps the simulation of `Kestrel16 Computer`. The limit counts clock cycles. It touches the circuit only through what the debug port offers: the RAM components' contents (standing in for bus access), the reset and clock inputs, a HALTED output, and the UART. **It never reaches into CPU internals**, even though in the simulator it could. That's the rule that keeps the hardware path open.
- **HardwareTarget** (later) is the same interface over a serial link to the microcontroller.

`ArchState` and the comparison live in the validator; the targets live with the validator. The validator links the emulator core and the simulator core, so the simulation code (`Simulation`, `Circuit`, `ComponentFile`) needs to be a library without the editor and raylib.

## The dump routine

A small assembly routine, `__dump`, that validated programs end with instead of a bare HALT:

- It stores R0–R7 and the flags to a fixed **dump area** in RAM, then halts with interrupts off.
- Flags can't be stored directly, so it builds a flags word with conditional branches (one `Bcc` per flag) before running anything that changes flags. Stores and `LDI`/`LHI` leave the flags alone, so it can save the registers first.
- To free a register for the base address without losing it, it first pushes R0 below the stack pointer. That memory write happens identically on both machines, so it doesn't affect the comparison.
- For C, the validator rewrites the compiler's `_start` so that after `main` returns it runs `j __dump`, not `halt` (`Source/Validator/Dump.cpp`); the compiler itself is unchanged. An assembly program opts in by ending with `j __dump`, and then the validator appends the routine; that program then only builds through the validator. The validator could also accept programs that just halt, and compare only RAM and output.

The dump area is `DumpArea` in `MemoryMap.h`: the 24 bytes at 0xFBE0, the top of the stack. The stack doesn't move: by the time `__dump` runs the program is finished, so whatever stack it overwrites is dead, and it's overwritten identically on both machines. The emulator runs the same routine, so both sides dump the same way.

The interrupt state saved by an exception (EPC, saved flags, saved IE) isn't dumped. Interrupt bugs still show up through their effects on registers and memory.

## Determinism

The comparison only makes sense if both machines see the same world:

- **Same input**, typed at the start. The UART holds the bytes until the program reads them. Programs that depend on *when* a key arrives aren't validated.
- **The timer counts instructions**, not cycles, so timer interrupts land on the same instruction on both machines. This must stay true: nothing a program can observe may depend on cycle timing the emulator doesn't model (ISA vs microarchitecture: cycles per instruction, fetch overlap and microcode states are free to differ).
- **Same starting RAM.** Both start zeroed and get the same image.

## Finding where it went wrong

A final-state mismatch says *that* something broke, not *where*. In rough order of cost:

1. **Shorter programs.** Small targeted tests (one per instruction group) localise most bugs.
2. **Checkpoints.** A `__checkpoint` call dumps the registers to the dump area and halts. The validator compares, then restarts the CPU. Restarting from HALT needs an interrupt (or a "continue" line on the port), so this waits until the basics work.
3. **Bisection.** Rebuild the program with `__dump` at instruction N and binary-search for the first N where the states differ. Slow, but fully automatic and uses nothing new from the hardware.
4. **A simulator-only trace.** For debugging only, the simulator may print what it sees inside the CPU each instruction. That is a debugging aid, not part of pass/fail, and the hardware won't have it.

## Tests

- `validate_<program>` in CTest for every program in `Programs/`, replacing the `GATE_LEVEL_TESTS` output comparison and the `simulate_<name>` tests.
- The `program_<name>` emulator tests keep checking printed output against expected strings: that tests the program and toolchain, while the validator tests the hardware.
- Gate-level speed is tens of thousands of cycles per second, so long programs need `--max-cycles` and a `TIMEOUT`.

## Order

1. `MachineState`, the dump area in `MemoryMap.h`, and the `__dump` routine. Run it on the emulator alone and check the dump decodes correctly.
2. `Target`, `EmulatorTarget`, and the comparison and report.
3. Split the simulation code into a library; `SimulatorTarget` on top of it.
4. `KestrelValidator` and the `validate_<program>` tests. Retire `GATE_LEVEL_TESTS`.
5. Small per-instruction-group test programs.
6. Later: checkpoints, bisection, a random program generator (valid instructions from `Instructions.h`, with shrinking on failure), and HardwareTarget.

## Open questions

- Should the assembler gain an `.include`, so assembly programs can end in `__dump` and still build with KestrelAssembler?
- Is restart-from-checkpoint worth a "continue" line on the port, or is bisection enough?
