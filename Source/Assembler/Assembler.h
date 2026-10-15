// Assembler.h -- Kestrel16 assembly to a memory image.
//
// The image is raw little-endian bytes starting at address 0x0000, which is exactly what the
// emulator loads.
//
// ---------------------------------------------------------------------
// Source syntax
//
//   label:  mnemonic  operand, operand   ; comment
//
//   - Mnemonics, directives and register names are case-insensitive;
//     labels and symbols are case-sensitive.
//   - Registers: r0-r7, plus sp (= r6) and lr (= r7).
//   - Numbers: 42, 0x2A, 0b101010, 'A', '\n'  (escapes: \n \t \r \0 \\ \' \" \xHH)
//   - Expressions: + - * / % & | ^ ~ << >> and parentheses, symbols,
//     $ (address of the current statement), lo(x) and hi(x) (bytes of x).
//   - Memory operands: [r1], [r1+4], [r1-2], [sp+CONSTANT]
//   - Branch and jump operands are absolute targets (usually labels);
//     the assembler works out the relative offset and checks its range.
//
// Directives
//   name = expr          define a constant (same as .equ name, expr)
//   .equ name, expr
//   .org address         continue assembling at address (forward only)
//   .word expr, ...      16-bit values (aligned to an even address)
//   .byte expr|"str",... 8-bit values
//   .ascii "str", ...    string bytes
//   .asciz "str", ...    string bytes, each followed by a zero
//   .space count         count zero bytes
//   .align n             pad with zeros to a multiple of n
//
// Instructions are always placed at even addresses; the assembler pads
// automatically after odd-length data, and a label just before an
// instruction lands on the padded address.
//
// Real instructions (Source/Kestrel/Instructions.h has the full table)
//   add sub and or xor shl shr sar adc sbc   rd, ra, rb
//   mov not neg sxb                          rd, ra
//   cmp                                      ra, rb
//   addi                                     rd, ra, imm6     (-32..31)
//   cmpi                                     ra, imm6
//   ldi lhi                                  rd, imm8         (-128..255)
//   lw sw lb sb                              rd, [ra+imm6]
//   bra beq bne bltu bgeu blt bge bgt ble bmi bpl bvs bvc   target
//     (aliases: bz=beq bnz=bne bcc=blo=bltu bcs=bhs=bgeu)
//   j jal                                    target
//   jr jalr                                  ra
//   reti ei di halt trap
//
// Pseudo-instructions
//   li rd, expr     load any 16-bit value       (ldi + lhi, 2 words)
//   la rd, expr     same as li, reads better for addresses
//   push rx         addi sp, sp, -2 ; sw rx, [sp]  (2 words, sets flags)
//   pop rx          lw rx, [sp] ; addi sp, sp, 2   (2 words, sets flags)
//   call target     jal target
//   ret             jr lr
//   subi rd, ra, n  addi rd, ra, -n
//   nop             mov r0, r0                  (touches nothing)
// ---------------------------------------------------------------------

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Kestrel {

struct AssemblyResult {
    std::vector<uint8_t> image;       // address 0 up to the last byte written; empty with errors
    std::vector<std::string> errors;  // "source.s:12: error: ...", in the order found
    std::string listing;              // address, bytes and source per line, then the symbols
};

// sourceName is only for messages. With makeListing, result.listing is filled in.
AssemblyResult Assemble(const std::string& sourceName, const std::string& text, bool makeListing = false);

}  // namespace Kestrel
