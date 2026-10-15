// AluOperations.h -- the operations of the hardware ALU (ALU 16, built from four ALU Slice ROMs),
// as data. These are the ALU's own 4-bit OP codes, which the microcode picks; they are not the
// instruction set's function codes (see Instructions.h).
//
// Definitions only, like Instructions.h: define the macro, include, undefine. Tools/make_alu.py
// and Tools/make_cpu.py read it through Tools/definitions.py (which upper-cases the names: ADD,
// PASSA, ...), and the Simulator's hardware check checks the ALU against the emulator's with it.
// No include guard on purpose.
//
// A shift moves one bit; the bit shifted out comes out on C. Logic operations pass the carry in
// through to C. Codes 13-15 act as PassA.

#ifdef KestrelAluOperation
// KestrelAluOperation(Name, code)
KestrelAluOperation(Add, 0)       // A + B + Cin
KestrelAluOperation(Sub, 1)       // A + ~B + Cin
KestrelAluOperation(And, 2)
KestrelAluOperation(Or, 3)
KestrelAluOperation(Xor, 4)
KestrelAluOperation(Shl, 5)       // A << 1
KestrelAluOperation(Shr, 6)       // A >> 1, logical
KestrelAluOperation(Sar, 7)       // A >> 1, arithmetic
KestrelAluOperation(Not, 8)       // ~A
KestrelAluOperation(Neg, 9)       // 0 + ~A + Cin
KestrelAluOperation(PassA, 10)
KestrelAluOperation(PassB, 11)
KestrelAluOperation(Sxb, 12)      // A's low byte, sign extended; C is don't-care
#endif
