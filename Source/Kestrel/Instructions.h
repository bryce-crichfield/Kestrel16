// Instructions.h -- the Kestrel16 instruction set, as data.
//
// This is the one place the instruction set is defined. It holds definitions only: every
// non-comment line is a macro call, and whoever includes the file defines the macro of the
// section it wants (each section is skipped unless its macro is defined), then undefines it.
// Source/Kestrel/Isa.h builds the C++ enums and tables from it; Tools/definitions.py reads it for
// the hardware generators. So keep to one call per line, with plain names, numbers and "text"
// (assembly spellings are quoted: and, or, xor and not are operator keywords in C++).
// There is no include guard on purpose.
//
// Machine model
//   - 64 KB byte-addressed, little-endian. Word accesses ignore address bit 0.
//   - R0-R7 general purpose (R0 is NOT hardwired to zero).
//     Convention: R0/R1 args+return, R2/R3 temps, R4/R5 callee-saved,
//     R6 = stack pointer, R7 = link register.
//   - Flags: Zero, Negative, Carry, Overflow.
//     Carry means "no borrow" after subtraction (ARM style).
//   - Reset: PC = 0x0000. Interrupt / trap vector: 0x0002 (see MemoryMap.h).
//   - I/O is memory mapped at 0xFC00-0xFFFF, in 8 slots (see MemoryMap.h).
//
// Encodings (bit 15 on the left, all instructions 16 bits)
//   Register:  opcode4 | dest3[11:9] | sourceA3[8:6] | sourceB3[5:3] | function3[2:0]
//   Immediate: opcode4 | dest3[11:9] | sourceA3[8:6] | immediate6[5:0]  (signed)
//   Long:      opcode4 | dest3[11:9] | 0[8]          | immediate8[7:0]
//   Branch:    opcode4 | condition4[11:8] | offset8[7:0]   (signed, in words)
//   Jump:      opcode4 | offset12[11:0]                    (signed, in words)
//   System:    opcode4 | function4[11:8] | source3[7:5] | unused[4:0]
//   Branch/jump offsets are relative to the NEXT instruction (PC + 2).
//
// What the instructions do
//   ALU      dest = a op b; shifts shift by (b & 15), and C is the last bit shifted out
//   ALU2     CMP sets flags from a - b and writes nothing; MOV leaves the flags alone
//   ADDI     dest = a + immediate6        (sets flags)
//   LDI      dest = immediate8            (zero extended, flags untouched)
//   LHI      dest.high = immediate8       (low byte kept)
//   LW       dest = memory16[a + immediate6]
//   SW       memory16[a + immediate6] = dest
//   LB       dest = zeroExtend(memory8[a + immediate6])
//   SB       memory8[a + immediate6] = dest.low
//   Bcc      if condition: PC += offset8 * 2
//   J        PC += offset12 * 2
//   JAL      R7 = PC + 2; PC += offset12 * 2
//   SYS      see the system functions below
//   CMPI     flags = a - immediate6
//   Opcodes E and F, ALU2 function 7 and system functions 7-15 are illegal.
//
// Interrupts: an interrupt (when enabled) or TRAP saves PC and flags plus the interrupt-enable
// bit into hidden exception registers, disables interrupts, and jumps to the interrupt vector.
// RETI restores them. HALT sleeps until an interrupt; with interrupts off (or no timer) the
// emulator treats it as "program finished" and stops.

#ifdef KestrelConstant
// KestrelConstant(Name, value)
KestrelConstant(RegisterCount, 8)
KestrelConstant(WordBits, 16)
KestrelConstant(InstructionSize, 2)    // bytes; instructions sit at even addresses
#endif

#ifdef KestrelRegister
// KestrelRegister(Name, number, "assemblerName"): the registers with a role
KestrelRegister(StackPointer, 6, "sp")
KestrelRegister(LinkRegister, 7, "lr")
#endif

#ifdef KestrelField
// KestrelField(Name, shift, bits): where each field sits in an instruction word
KestrelField(Opcode, 12, 4)
KestrelField(Destination, 9, 3)
KestrelField(SourceA, 6, 3)
KestrelField(SourceB, 3, 3)
KestrelField(Function, 0, 3)
KestrelField(Condition, 8, 4)
KestrelField(SystemFunction, 8, 4)
KestrelField(SystemSource, 5, 3)
KestrelField(Immediate6, 0, 6)         // signed
KestrelField(Immediate8, 0, 8)         // unsigned
KestrelField(Offset8, 0, 8)            // signed, in words
KestrelField(Offset12, 0, 12)          // signed, in words
#endif

#ifdef KestrelOpcode
// KestrelOpcode(Name, value)
KestrelOpcode(Alu, 0x0)
KestrelOpcode(Alu2, 0x1)
KestrelOpcode(AddImmediate, 0x2)
KestrelOpcode(LoadImmediate, 0x3)
KestrelOpcode(LoadHighImmediate, 0x4)
KestrelOpcode(LoadWord, 0x5)
KestrelOpcode(StoreWord, 0x6)
KestrelOpcode(LoadByte, 0x7)
KestrelOpcode(StoreByte, 0x8)
KestrelOpcode(Branch, 0x9)
KestrelOpcode(Jump, 0xA)
KestrelOpcode(JumpAndLink, 0xB)
KestrelOpcode(System, 0xC)
KestrelOpcode(CompareImmediate, 0xD)
#endif

#ifdef KestrelAluFunction
// KestrelAluFunction(Name, value): the function field of opcode Alu
KestrelAluFunction(Add, 0)
KestrelAluFunction(Subtract, 1)
KestrelAluFunction(And, 2)
KestrelAluFunction(Or, 3)
KestrelAluFunction(Xor, 4)
KestrelAluFunction(ShiftLeft, 5)
KestrelAluFunction(ShiftRightLogical, 6)
KestrelAluFunction(ShiftRightArithmetic, 7)
#endif

#ifdef KestrelAlu2Function
// KestrelAlu2Function(Name, value): the function field of opcode Alu2
KestrelAlu2Function(AddWithCarry, 0)
KestrelAlu2Function(SubtractWithCarry, 1)
KestrelAlu2Function(Compare, 2)
KestrelAlu2Function(Move, 3)
KestrelAlu2Function(Not, 4)
KestrelAlu2Function(Negate, 5)
KestrelAlu2Function(SignExtendByte, 6)
#endif

#ifdef KestrelSystemFunction
// KestrelSystemFunction(Name, value): the system function field of opcode System
KestrelSystemFunction(JumpRegister, 0)
KestrelSystemFunction(JumpAndLinkRegister, 1)
KestrelSystemFunction(ReturnFromInterrupt, 2)
KestrelSystemFunction(EnableInterrupts, 3)
KestrelSystemFunction(DisableInterrupts, 4)
KestrelSystemFunction(Halt, 5)
KestrelSystemFunction(Trap, 6)
#endif

#ifdef KestrelCondition
// KestrelCondition(Name, value, "suffix"): branch conditions; the branch mnemonic is b + suffix
KestrelCondition(Always, 0, "ra")
KestrelCondition(Equal, 1, "eq")
KestrelCondition(NotEqual, 2, "ne")
KestrelCondition(LessUnsigned, 3, "ltu")
KestrelCondition(GreaterOrEqualUnsigned, 4, "geu")
KestrelCondition(Less, 5, "lt")
KestrelCondition(GreaterOrEqual, 6, "ge")
KestrelCondition(Greater, 7, "gt")
KestrelCondition(LessOrEqual, 8, "le")
KestrelCondition(Minus, 9, "mi")
KestrelCondition(Plus, 10, "pl")
KestrelCondition(OverflowSet, 11, "vs")
KestrelCondition(OverflowClear, 12, "vc")
#endif

#ifdef KestrelBranchAlias
// KestrelBranchAlias("suffix", Condition): other branch mnemonics the assembler accepts
KestrelBranchAlias("z", Equal)
KestrelBranchAlias("nz", NotEqual)
KestrelBranchAlias("lo", LessUnsigned)
KestrelBranchAlias("cc", LessUnsigned)
KestrelBranchAlias("hs", GreaterOrEqualUnsigned)
KestrelBranchAlias("cs", GreaterOrEqualUnsigned)
#endif

#ifdef KestrelForm
// KestrelForm(Name): how an instruction's operands are written in assembly
KestrelForm(ThreeRegisters)            // op rd, ra, rb
KestrelForm(TwoRegisters)              // op rd, ra
KestrelForm(CompareRegisters)          // op ra, rb
KestrelForm(RegisterImmediate)         // op rd, ra, imm6
KestrelForm(CompareImmediate)          // op ra, imm6
KestrelForm(LoadImmediate)             // op rd, imm8
KestrelForm(Memory)                    // op rd, [ra+imm6]
KestrelForm(Jump)                      // op target
KestrelForm(SystemRegister)            // op ra
KestrelForm(SystemBare)                // op
#endif

#ifdef KestrelInstruction
// KestrelInstruction("mnemonic", Form, Opcode, Function): every instruction but the branches
// (b + a condition suffix). Function names one of the functions above, or None.
KestrelInstruction("add", ThreeRegisters, Alu, Add)
KestrelInstruction("sub", ThreeRegisters, Alu, Subtract)
KestrelInstruction("and", ThreeRegisters, Alu, And)
KestrelInstruction("or", ThreeRegisters, Alu, Or)
KestrelInstruction("xor", ThreeRegisters, Alu, Xor)
KestrelInstruction("shl", ThreeRegisters, Alu, ShiftLeft)
KestrelInstruction("shr", ThreeRegisters, Alu, ShiftRightLogical)
KestrelInstruction("sar", ThreeRegisters, Alu, ShiftRightArithmetic)
KestrelInstruction("adc", ThreeRegisters, Alu2, AddWithCarry)
KestrelInstruction("sbc", ThreeRegisters, Alu2, SubtractWithCarry)
KestrelInstruction("cmp", CompareRegisters, Alu2, Compare)
KestrelInstruction("mov", TwoRegisters, Alu2, Move)
KestrelInstruction("not", TwoRegisters, Alu2, Not)
KestrelInstruction("neg", TwoRegisters, Alu2, Negate)
KestrelInstruction("sxb", TwoRegisters, Alu2, SignExtendByte)
KestrelInstruction("addi", RegisterImmediate, AddImmediate, None)
KestrelInstruction("cmpi", CompareImmediate, CompareImmediate, None)
KestrelInstruction("ldi", LoadImmediate, LoadImmediate, None)
KestrelInstruction("lhi", LoadImmediate, LoadHighImmediate, None)
KestrelInstruction("lw", Memory, LoadWord, None)
KestrelInstruction("sw", Memory, StoreWord, None)
KestrelInstruction("lb", Memory, LoadByte, None)
KestrelInstruction("sb", Memory, StoreByte, None)
KestrelInstruction("j", Jump, Jump, None)
KestrelInstruction("jal", Jump, JumpAndLink, None)
KestrelInstruction("jr", SystemRegister, System, JumpRegister)
KestrelInstruction("jalr", SystemRegister, System, JumpAndLinkRegister)
KestrelInstruction("reti", SystemBare, System, ReturnFromInterrupt)
KestrelInstruction("ei", SystemBare, System, EnableInterrupts)
KestrelInstruction("di", SystemBare, System, DisableInterrupts)
KestrelInstruction("halt", SystemBare, System, Halt)
KestrelInstruction("trap", SystemBare, System, Trap)
#endif
