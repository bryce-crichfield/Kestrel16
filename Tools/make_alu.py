"""Generates the ALU (run from anywhere):

    python Tools/make_alu.py

Writes Components/ALU Slice.bin, the image burned into each of the four slice ROMs (a 64K x 8
EEPROM such as a 27C512), and Components/ALU 16.yaml, which wires four slices together.

Every slice holds the same image. A slice works on one nibble; its address is

    A0-3  B0-3  OP0-3  Cin  SIN  P0-1     (bit 0 first)

SIN is bit 0 of the slice above (right shifts move bits down), and P0-1 is the slice's position,
0 for bits 0-3 up to 3 for bits 12-15, set with Low/High nodes. Its data is

    Y0-3  Cout  ZERO  V  (spare)

Carries ripple up from slice to slice through Cin/Cout. Operations (the OP input):
"""
import os

COMPONENTS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'Components')

OPERATIONS = ['ADD', 'SUB', 'AND', 'OR', 'XOR', 'SHL', 'SHR', 'SAR', 'NOT', 'NEG', 'PASSA', 'PASSB', 'SXB']
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

    lines = ['name: ALU 16', 'description: "%s"' % description, 'nodes:']
    wires = []
    node_id = [0]

    def node(text, x, y):
        node_id[0] += 1
        lines.append('  - {id: %d, %s, at: [%d, %d]}' % (node_id[0], text, x, y))
        return node_id[0]

    a = [node('kind: Input, label: A%d' % i, 40, 40 + 100 * i) for i in range(16)]
    b = [node('kind: Input, label: B%d' % i, 40, 1700 + 100 * i) for i in range(16)]
    op = [node('kind: Input, label: OP%d' % i, 40, 3360 + 100 * i) for i in range(4)]
    cin = node('kind: Input, label: Cin', 40, 3780)
    low, high = node('kind: Low', 380, 40), node('kind: High', 380, 140)

    address_names = ['A0', 'A1', 'A2', 'A3', 'B0', 'B1', 'B2', 'B3', 'OP0', 'OP1', 'OP2', 'OP3', 'Cin', 'SIN', 'P0', 'P1']
    data_names = ['Y0', 'Y1', 'Y2', 'Y3', 'Cout', 'ZERO', 'V', 'NC']
    slices = []
    for k in range(4):
        node_id[0] += 1
        slices.append(node_id[0])
        lines += ['  - id: %d' % node_id[0], '    kind: ROM', '    at: [%d, %d]' % (560, 300 + 900 * k),
                  '    address: 16', '    data: 8', '    inputs: [%s]' % ', '.join(address_names),
                  '    outputs: [%s]' % ', '.join(data_names), '    image: ALU Slice.bin']
    zero_low, zero_high, zero = (node('kind: And', 900, y) for y in (900, 2700, 1800))
    outputs = [node('kind: Output, label: Y%d' % i, 1200, 40 + 100 * i) for i in range(16)]
    flag = {name: node('kind: Output, label: %s' % name, 1200, 1700 + 100 * i)
            for i, name in enumerate(['Z', 'N', 'C', 'V'])}

    for k, rom in enumerate(slices):
        for i in range(4):
            wires.append('%d -> %d.A%d' % (a[4 * k + i], rom, i))
            wires.append('%d -> %d.B%d' % (b[4 * k + i], rom, i))
            wires.append('%d.Y%d -> %d' % (rom, i, outputs[4 * k + i]))
            wires.append('%d -> %d.OP%d' % (op[i], rom, i))
        wires.append('%d -> %d.Cin' % (cin, rom) if k == 0 else '%d.Cout -> %d.Cin' % (slices[k - 1], rom))
        wires.append('%d -> %d.SIN' % (a[4 * k + 4] if k < 3 else low, rom))
        wires.append('%d -> %d.P0' % (high if k & 1 else low, rom))
        wires.append('%d -> %d.P1' % (high if k & 2 else low, rom))
        wires.append('%d -> %d./CE' % (low, rom))
        wires.append('%d -> %d./OE' % (low, rom))
    wires += ['%d.ZERO -> %d.A' % (slices[0], zero_low), '%d.ZERO -> %d.B' % (slices[1], zero_low),
              '%d.ZERO -> %d.A' % (slices[2], zero_high), '%d.ZERO -> %d.B' % (slices[3], zero_high),
              '%d.Y -> %d.A' % (zero_low, zero), '%d.Y -> %d.B' % (zero_high, zero),
              '%d.Y -> %d' % (zero, flag['Z']), '%d.Y3 -> %d' % (slices[3], flag['N']),
              '%d.Cout -> %d' % (slices[3], flag['C']), '%d.V -> %d' % (slices[3], flag['V'])]

    lines.append('wires:')
    lines += ['  - ' + wire for wire in wires]
    with open(os.path.join(COMPONENTS, 'ALU 16.yaml'), 'w', newline='\n') as file:
        file.write('\n'.join(lines) + '\n')


if __name__ == '__main__':
    make_image()
    make_alu()
    print('wrote ALU Slice.bin and ALU 16.yaml to', os.path.normpath(COMPONENTS))
