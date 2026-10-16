"""Generates the ALU (run from anywhere):

    python Tools/make_alu.py

Writes Build/Components/ALU Slice.bin, the image burned into each of the four slice ROMs (a 64K x 8
EEPROM such as a 27C512), and Build/Components/ALU 16.yaml, which wires four slices together.

Every slice holds the same image. A slice works on one nibble; its address is

    A0-3  B0-3  OP0-3  Cin  SIN  P0-1     (bit 0 first)

SIN is bit 0 of the slice above (right shifts move bits down), and P0-1 is the slice's position,
0 for bits 0-3 up to 3 for bits 12-15, set with Low/High nodes. Its data is

    Y0-3  Cout  ZERO  V  (spare)

Carries ripple up from slice to slice through Cin/Cout. Operations (the OP input):
"""
import os
from definitions import AluOperations
from hdl import *

COMPONENTS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'Build', 'Components')

# Source/Kestrel/AluOperations.h, by code: ADD, SUB, ..., PASSA, PASSB, SXB.
OPERATIONS = [name.upper() for name, code in sorted(AluOperations.items(), key=lambda item: item[1])]
assert [AluOperations[name] for name in sorted(AluOperations, key=AluOperations.get)] == list(range(len(OPERATIONS)))
__doc__ += '\n'.join('    %2d %s' % (code, name) for code, name in enumerate(OPERATIONS)) + '\n    13-15 act as PASSA\n'


def slice_word(a, b, op, cin, sin, position):
    """What one slice outputs. Returns (y, cout, v)."""
    bottom, top, high = position == 0, position == 3, position >= 2
    name = OPERATIONS[op] if op < len(OPERATIONS) else 'PASSA'
    v = 0
    if name in ('ADD', 'SUB', 'NEG'):
        x, y = {'ADD': (a, b), 'SUB': (a, ~b & 15), 'NEG': (0, ~a & 15)}[name]
        total = x + y + cin
        result, cout = total & 15, total >> 4
        if top:  # signed overflow: operands share a sign the result doesn't
            v = int((x >> 3) == (y >> 3) and (result >> 3) != (x >> 3))
        return result, cout, v
    if name == 'SHL':  # one bit; the bit shifted out of bit 15 ends up on C
        fill = 0 if bottom else cin
        return ((a << 1) | fill) & 15, a >> 3, 0
    if name in ('SHR', 'SAR'):  # one bit; bit 0 rides the carry chain up to C
        fill = (a >> 3 if name == 'SAR' else 0) if top else sin
        return (a >> 1) | (fill << 3), (a & 1) if bottom else cin, 0
    if name == 'SXB':  # bit 7 rides the carry chain up from slice 1 to the high byte
        if high:
            return (15 if cin else 0), cin, 0
        return a, a >> 3, 0
    result = {'AND': a & b, 'OR': a | b, 'XOR': a ^ b, 'NOT': ~a & 15, 'PASSB': b}.get(name, a)
    return result, cin, 0  # logic operations pass the carry through, so C comes out unchanged


def make_image():
    image = bytearray(1 << 16)
    for address in range(1 << 16):
        a, b, op = address & 15, (address >> 4) & 15, (address >> 8) & 15
        cin, sin, position = (address >> 12) & 1, (address >> 13) & 1, address >> 14
        y, cout, v = slice_word(a, b, op, cin, sin, position)
        image[address] = y | cout << 4 | int(y == 0) << 5 | v << 6
    with open(os.path.join(COMPONENTS, 'ALU Slice.bin'), 'wb') as file:
        file.write(image)


def make_alu():
    ops = ', '.join('%d %s' % (code, name) for code, name in enumerate(OPERATIONS))
    description = (
        '16-bit ALU from four ROM slices, each a 64K x 8 EEPROM holding ALU Slice.bin (made by '
        'Tools/make_alu.py) and working on 4 bits. OP: %s; 13-15 act as PASSA. Shifts move one bit; '
        'the control unit repeats them (B & 15) times. Cin: 0 for ADD, 1 for SUB/CMP/NEG, the C flag '
        'for ADC/SBC, shifts and everything else. C: adder carry, the bit shifted out, or Cin passed '
        'through. V: overflow for ADD/SUB/NEG. Z and N describe Y. Which flags get stored is up to the '
        'control unit.' % ops)
    with component('ALU 16', description):
        A, B, OP = bus_input('A', 16), bus_input('B', 16), bus_input('OP', 4)
        carry = inputs('Cin')
        address = ['A0', 'A1', 'A2', 'A3', 'B0', 'B1', 'B2', 'B3', 'OP0', 'OP1', 'OP2', 'OP3', 'Cin', 'SIN', 'P0', 'P1']
        data = ['Y0', 'Y1', 'Y2', 'Y3', 'Cout', 'ZERO', 'V', 'NC']
        slices, Y = [], Bus()
        for k in range(4):
            chip = rom(address, data, image='ALU Slice.bin')
            A[4 * k:4 * k + 4] >> chip.A
            B[4 * k:4 * k + 4] >> chip.B
            OP >> chip.OP
            carry >> chip.Cin  # carries ripple up from slice to slice
            carry = chip.Cout
            (A[4 * k + 4] if k < 3 else low()) >> chip.SIN  # bit 0 of the slice above
            bus(high() if k & 1 else low(), high() if k & 2 else low()) >> chip.P  # the slice's position
            slices.append(chip)
            Y += chip.Y
        output('Y', Y)
        output('Z', (slices[0].ZERO & slices[1].ZERO) & (slices[2].ZERO & slices[3].ZERO))
        output('N', Y[15])
        output('C', carry)
        output('V', slices[3].V)


if __name__ == '__main__':
    make_image()
    make_alu()
    print('wrote ALU Slice.bin and ALU 16.yaml to', os.path.normpath(COMPONENTS))
