# Differential Verification: Design Notes

Status: **shelved, not started.** These notes collect the design discussion so far, so the work can be picked up later.

## Goal

Run the same program on the reference emulator (`Kestrel::Machine`) and on the gate-level computer (`Kestrel16 Computer` in the simulator), and prove that they behave the same, instruction by instruction. The emulator is the reference: if the two disagree, the hardware is wrong (see ARCHITECTURE.md, "The one rule").

Today we only compare **printed output** (`GATE_LEVEL_TESTS`, and `--check-hardware` running Fibonacci). That catches a bug only if it happens to change what the program prints, and it says nothing about *where* things went wrong.

## Architecture vs microarchitecture

This distinction drives the whole design.

- **Architecture (the ISA)** is everything a program can observe: R0–R7, PC, flags, the interrupt enable, EPC and the saved flags, memory, I/O, and *which instruction* an interrupt arrives after. The assembler and compiler target this. **This must match exactly.**
- **Microarchitecture** is how one hardware design implements the ISA: microcode states, fetch overlap, cycles per instruction, and, later, pipelining, caches or banked memory. **This is free to differ**, and it is expected to change.

The emulator executes the ISA only. It does **not** model fetch overlap or microcode states, and it shouldn't, because then it would be a second copy of one particular hardware design rather than a spec. A pipelined redesign would then "fail" against its own reference.

The rule that keeps this sound: **nothing a program can observe may depend on timing that the emulator doesn't model.** It holds today, because the Timer counts *instructions* (the CPU's `TICK`), not clock cycles. If a cycle-based timer or a cycle-counter register is ever added, cycles become architectural and the emulator must model them.

Banked memory: if bank selection is done through a register or I/O port that programs write, then it is architectural, and the emulator must implement it (in `Machine` and `MemoryMap.h`). How the hardware wires the banks is microarchitecture.

## Compare per instruction, not per clock cycle

The two machines cannot be compared "after N clock cycles". The emulator runs one instruction per `Step()`, while the hardware takes 1–4 cycles per instruction. So the comparison runs in **lockstep at instruction retirement**:

```
loop:
  emulator.Step()                              → the reference effects of instruction k
  clock the hardware until it retires one      → the hardware's effects of instruction k
  compare; on mismatch, stop and report
```

A failure then reads like: *instruction #48,213 at 0x01A4, `ADD R3, R1, R2`: R3 expected 0x1F00, got 0x1E00; C flag differs.* That points at one microcode step instead of "the output was wrong somewhere".

## What "retire" means, and who defines it

An instruction **retires** when it is finished: all its architectural effects (register write, flags, memory write, PC change) are committed, and it can no longer be cancelled. Every instruction that the emulator executes retires exactly once, in program order. Instructions that the hardware starts but throws away (for example, after a mispredicted branch in a future pipeline) never retire.

**The hardware defines retirement explicitly, as an output signal.** The HDL doesn't infer it. The CPU design must expose a `RETIRE` strobe, and the verifier probes it.

- **Today:** the control unit already has `TICK` (microcode bit in `CONTROL_B`, an output of `Kestrel16 CPU`). It pulses once per instruction, in the FETCH that starts the *next* instruction (see `fetch(tick, …)` in `Tools/make_cpu.py`), and the Timer counts it. So "TICK high on a rising edge" ≈ "the previous instruction is done". It works as a first retire signal, but it is defined as "fetch", not as "retire", and a few cases need checking: the first fetch after reset, HALT, interrupt entry (INT1/INT2), and ILLEGAL.
- **Better:** add a dedicated `RETIRE` microcode bit, asserted in each instruction's *last* cycle. That makes the meaning explicit and independent of how fetch works.

### Does this survive pipelining?

**Yes, provided we compare what is committed, not snapshots of the whole machine.** In a pipeline, several instructions are in flight at once, so "the machine's state right now" is a blend of half-finished instructions. Snapshotting every register at an arbitrary moment would break. Two fixes make the method pipeline-proof:

1. **Retirement comes from the last pipeline stage.** The writeback/commit stage asserts `RETIRE` for each instruction it completes, along with that instruction's PC. Flushed instructions never assert it.
2. **Compare a retirement trace, not full snapshots.** At each retirement, the hardware reports what that instruction *did*:
   - its PC (and the instruction word),
   - the register write, if any (which register, what value),
   - the new flags, if they changed,
   - the memory write, if any (address, bytes, lane),
   - whether an interrupt or trap was entered.

   The emulator reports the same record from `Step()`. The verifier compares records one by one. Full-state snapshots (all registers, memory) are still taken at the end, and optionally every N instructions, as a backstop.

This is the method used in real CPU verification. RISC-V's **RVFI** (RISC-V Formal Interface) is exactly a set of commit-port signals that a core exposes per retired instruction, and Spike (the reference ISA simulator) is compared against it. Our `RETIRE` and the trace fields are a small RVFI.

With today's multi-cycle CPU, full snapshots at `TICK` would also work, because only one instruction is in flight. But building on the trace from the start means pipelining won't force a rewrite.

### Interrupts

The emulator takes an interrupt at a `Step()` boundary, before the next instruction. The hardware must take it after the same retired instruction. The trace records "interrupt entered" as its own event, so a disagreement shows up as a mismatch. Both sides must be fed the same terminal input bytes at the same instruction (or the same input stream, consumed as the program asks for it).

## Probes: reading state out of the hardware

The trace fields have to come from nets inside the flattened circuit.

### What's in the way today (from reading the code)

- `BuildSimulation` (`Source/Simulator/Simulation.cpp`, `Flattener::Expand`) flattens the whole hierarchy into gates and nets, and keeps a pin → net map **only for the top component's own nodes** (`nodeInputNets`/`nodeOutputNets`). There's no way to ask for "the net of R3 bit 5".
- **Component instances have no names.** In the YAML, an instance is `{id: 23, component: Register File}`. `Node::label` is only used for Input/Output pins. The Register File holds eight anonymous `Register 16` instances. So a path has to use node ids (`17/23/41.QA`) unless instances get labels.
- Some architectural state isn't on any component pin. The **saved flags and saved IE** (`exceptionFlags` in the emulator) are internal flip-flops in `Status` with no outputs. A probe would have to reach a flip-flop's `Q` directly, or `Status` would need extra outputs.
- Register outputs are **tri-state read ports** (`QA`/`QB` of `Register 16` drive only while `/OEA`/`/OEB` are low). To read a register's value at any time, the probe should read the flip-flops' `Q` inside the register, not the ports.

### Proposed pieces

1. **Hierarchical net names during flattening.** While `Expand` recurses, record path → net ids for the paths someone asks for (or for every component-instance output and every flip-flop `Q`, which is simpler and cheap next to settling). A path is the chain of node ids from the top: `"<cpu id>/<register file id>/<register id>/<flip-flop id>.Q"`.
2. **Optionally, instance labels.** Let `component:` nodes carry a `label:` (saved by `ComponentFile.cpp`, set by the generators: `R3`, `PC`, `Status`). Paths become readable (`CPU/RegisterFile/R3`) and don't break when node ids shift. This needs the editor to show and edit them too.
3. **A probe file**, generated by `Tools/make_cpu.py` (it already builds every part, so it knows every id; hand-written files would drift from the circuit). For example, `Components/Kestrel16 Computer.probes.yaml`:
   ```yaml
   retire: CPU/RETIRE          # or CPU/TICK today
   pc:     CPU/RetiredPC       # the PC of the instruction retiring
   registers:
     R0..R7: CPU/RegisterFile/R{n}   # flip-flop Q bits
   flags:  {Z: CPU/Status/Z, N: ..., C: ..., V: ..., IE: ...}
   saved:  {Z: CPU/Status/SZ, ...}   # the exception copies
   epc:    CPU/EPC
   timer:  {reload: Timer/Reload, count: Timer/Counter}
   memory: [EvenRAM, OddRAM]          # byte lanes, interleaved
   ```
   Only the machine needs this; ordinary components don't.
4. **Shared state types in `KestrelCore`:** an `ArchState` struct (registers, PC, flags, IE, EPC, saved flags, interrupt mask, timer) with a `Diff()` that lists the differences by name, and a `RetireRecord` for the trace. The emulator fills them from `Cpu` (which already holds all of this: `registers`, `programCounter`, `exceptionProgramCounter`, `flags`, `exceptionFlags`, `interruptsEnabled`, `interruptMask`, `timerPending`, `timerReload`, `timerCount`). The simulator fills them from the probes.

### Memory

Comparing 64 KB after every instruction is too slow. Instead:

- Log every write on both sides: `WriteWord`/`WriteByte` in `Machine`, and the RAM write strobe in the simulator's `Settle` (`RamUnit`). Compare the logs per instruction, which catches a wrong address or byte lane right away.
- Compare all of memory at halt (and optionally every N instructions).
- Note that a RAM write in the simulator happens "every step while /WE is low", so the log should record the value when /WE rises (the end of the write), not every settle step.

## Cycle counts (optional, agreed as a good idea)

Cycle counts are microarchitecture, so they aren't part of the ISA comparison. They're still worth checking, to catch performance regressions in the microcode (an extra wasted state). The idea:

- A **cycle-cost table** per instruction class, stated as the intended performance of the current design: ALU op 1, load 3, store 3, taken branch 2, untaken branch 1, and so on (to be read off `execute()` in `make_cpu.py`).
- The diff runner also counts hardware cycles between retirements and checks them against the table, separately from the correctness check (a mismatch is a warning, or a separate failing test).
- When the microarchitecture changes on purpose (pipelining), update the table. It is *not* part of `Machine`.

## The tool

- `KestrelSimulator --diff <program> [--component "Kestrel16 Computer"] [--max-instructions N]`
  - Builds the program in-process (`BuildProgram`), loads it into the emulator and into the RAMs (as `--run` does in `HeadlessRun.cpp`).
  - Runs lockstep until HALT with nothing left to wake it, ILLEGAL, or the limit.
  - On the first mismatch: prints the instruction number, PC, disassembly, the expected and actual record, the last ~16 retired instructions, and the cycle count. Exit status 1.
- CMake: a `diff_<name>` test for every program in `Programs/`, replacing the hand-picked `GATE_LEVEL_TESTS` output comparison (keep it for the terminal-output check, or fold that into the trace as I/O events).
- Speed: the gate-level computer runs at roughly tens of thousands of cycles per second (Fibonacci: see the timing that `--check-hardware` prints). Long programs need `--max-instructions`, or a run that only compares every N instructions.

## Later: random instruction streams

The biggest payoff once the lockstep runner works.

- Generate random *valid* instruction sequences from a seed (from `Instructions.h` via `Isa.h`, never a hard-coded opcode list), with random register and memory starting state. Keep jumps and memory accesses inside a sandbox region; let some sequences enable interrupts and the timer.
- Run both machines and diff. Edge cases (shift by 0 or by 15, SUB with carry, flags on byte loads, interrupts landing on a jump, stores at odd addresses) show up here long before hand-written programs hit them.
- On failure, **shrink**: drop instructions and simplify operands while the failure persists, and report the smallest failing program and its seed. Save failing seeds as regression tests.

## Suggested order

1. `ArchState`, `RetireRecord` and the emulator side (small).
2. Hierarchical net recording in the flattener, and the probe loader. Decide: node-id paths vs instance labels.
3. Add the `RETIRE` microcode bit (and a retired-PC source, or reconstruct it from the emulator's trace at first). Have `make_cpu.py` emit the probe file.
4. The `--diff` lockstep runner, and `diff_<name>` tests in CMake.
5. The cycle-cost table check.
6. The random instruction fuzzer and shrinker.

## Open questions

- Node-id paths or instance labels? Labels are nicer and sturdier, but touch the file format and the editor.
- Is `TICK` exactly one pulse per retired instruction in every case (reset, HALT wake-up, interrupt entry, RETI, ILLEGAL)? Check this before relying on it, or go straight to a `RETIRE` bit.
- Where does the hardware get the *retired* PC? With fetch overlap, the PC register has already moved on by the time TICK fires. Options: a small register that latches the PC at fetch and is reported at retire, or reconstruct it from the emulator side for now.
- Should terminal input be modelled as a trace event (byte consumed at instruction k) so both sides can't drift on when input arrives?
- If banked memory is added: is the bank register architectural (programs write it)? If so, it goes into `Machine`, `MemoryMap.h` and `ArchState`.
